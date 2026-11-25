// A league save made by the game's other version loaded with the wrong audibles: on defense the
// audible plays were offensive plays, and the defenders lined up in an offensive formation.
//
// A Season or Road to Backbreaker save keeps each team's eight audibles (four on offense, four on
// defense) as play numbers, and a play's number is its place in the game's play lists
// (play_numbers.h). Title Update 2 has longer lists in another order, so a number saved by the
// disc's version names another play there: every team's defensive audibles, 79-135 on the disc,
// are offensive plays in the update's list, and the offensive ones are other offensive plays. The
// other way round, a save from the update gives the disc's version special plays, or numbers it
// has no play for. The game doesn't check: its two loaders (sub_82292AF8 and sub_823A9FC0, the
// same code in both versions) look each number up (sub_82329B40) and give the team what comes back
// (sub_8223E118). On a console that only showed when an update was installed over a season in
// progress; here both versions share one saves folder and the player can switch at any time.
//
// The fix notes the numbers a loader reads, and when it is done looks at each team's: audibles this
// version's lists can't have numbered (an offensive play in a defensive slot, a number past the
// list) are renumbered from the other version's lists, play file by play file. A play this version
// doesn't have, or any such save when the other version's lists can't be read, leaves the team's
// default there, the audible the game gives a new team. The next save then holds this version's
// numbers. Saves made by this version are left alone.
#include <ppc_config.h>
#include <ppc_context.h>

#include "kernel/kernel.h"
#include "log.h"
#include "paths.h"
#include "play_numbers.h"

#include <mutex>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82292AF8); // league save with its own count of teams: reads them (r3 the stream)
extern "C" PPC_FUNC(__imp__sub_823A9FC0); // league save with all 56 teams (Road to Backbreaker): reads them
extern "C" PPC_FUNC(__imp__sub_82329B40); // the play with a number (r3 play database, r4 number), or null
extern "C" PPC_FUNC(__imp__sub_8223E118); // sets a team's audible (r3 team, r4 slot 1-4, r5 offense, r6 play, r7 flipped)
extern "C" PPC_FUNC(__imp__sub_82150000); // the team table
extern "C" PPC_FUNC(__imp__sub_82150210); // a team's default offensive audibles, by play name (r3 team table, r4 team)
extern "C" PPC_FUNC(__imp__sub_821503A8); // and its default defensive ones

namespace
{
    constexpr uint32_t kOffense = 8480, kDefense = 8500; // team: play pointers, slot 1-4 at +4 * slot (sub_8223E150 reads them)
    constexpr uint32_t kPlays = 60;       // play database: first node of its list { play, next }
    constexpr uint32_t kPlayNumber = 104; // play: its number

    uint32_t load32(uint8_t* base, uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); }
    void store32(uint8_t* base, uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v); }

    // What a loader read, on the thread it runs on
    struct Load
    {
        struct Team
        {
            uint32_t team = 0;
            play_numbers::Audibles saved;
        };
        std::vector<Team> teams;
        uint32_t database = 0;
        int number = -1;      // the number looked up last
        bool looked = false;  // and not given to a team yet
    };
    thread_local Load* t_load = nullptr;

    const char* const kLists[3] = { "media\\PlayDatabase\\Offensive_Plays.xml", "media\\PlayDatabase\\Defensive_Plays.xml",
                                    "media\\PlayDatabase\\Special_Plays.xml" };

    struct Lists
    {
        play_numbers::PlayList own, other;
    };

    // This version's play lists as the game read them at boot, and the other version's. The
    // update's own archive holds its offensive and defensive lists; the special plays' is the disc's.
    const Lists& lists()
    {
        static Lists l;
        static std::once_flag once;
        std::call_once(once, [] {
            std::string disc[3], update[3];
            bool haveUpdate = false;
            for (int i = 0; i < 3; i++)
            {
                kernel::readArchiveEntry("game:\\packedfilesx360\\PackedOutputX360", kLists[i], disc[i]);
                const bool found = BB_EXE(kernel::readHostArchiveEntry(paths::data() + "/extracted/tu2/PatchPackedOutputX360", kLists[i], update[i]),
                                          kernel::readArchiveEntry("update:\\PatchPackedOutputX360", kLists[i], update[i]));
                haveUpdate = haveUpdate || found;
                if (!found)
                    update[i] = disc[i];
            }
            const play_numbers::PlayList discList = play_numbers::makeList(disc[0], disc[1], disc[2]);
            // without the update's files there is no list of its own to renumber from
            const play_numbers::PlayList updateList = haveUpdate ? play_numbers::makeList(update[0], update[1], update[2]) : play_numbers::PlayList{};
            l.own = BB_EXE(discList, updateList);
            l.other = BB_EXE(updateList, discList);
            if (l.own.empty())
                warn("[saves] the play lists couldn't be read, so a save's audibles aren't checked");
        });
        return l;
    }

    uint32_t playNumbered(uint8_t* base, uint32_t database, int number)
    {
        if (number < 0)
            return 0;
        for (uint32_t node = load32(base, database + kPlays); node; node = load32(base, node + 4))
        {
            const uint32_t play = load32(base, node);
            if (play && int(load32(base, play + kPlayNumber)) == number)
                return play;
        }
        return 0;
    }

    void renumberAudibles(const Load& load, PPCContext& ctx, uint8_t* base)
    {
        const Lists& l = lists();
        if (l.own.empty() || !load.database)
            return;
        int teams = 0, renumbered = 0, defaults = 0;
        for (const Load::Team& t : load.teams)
        {
            if (!play_numbers::fromAnotherList(l.own, t.saved))
                continue;
            teams++;
            // The team's defaults first: they stay where the other version's play isn't in this one
            PPCContext call = ctx;
            __imp__sub_82150000(call, base);
            const uint32_t table = call.r3.u32;
            call = ctx;
            call.r3.u64 = table;
            call.r4.u64 = t.team;
            __imp__sub_82150210(call, base);
            call = ctx;
            call.r3.u64 = table;
            call.r4.u64 = t.team;
            __imp__sub_821503A8(call, base);
            const play_numbers::Audibles own = play_numbers::renumber(l.other, l.own, t.saved);
            for (int i = 0; i < play_numbers::kSlots; i++)
                for (int side = 0; side < 2; side++)
                {
                    if ((side ? t.saved.defense[i] : t.saved.offense[i]) == -1)
                        continue; // the save had none for this slot
                    const uint32_t play = playNumbered(base, load.database, side ? own.defense[i] : own.offense[i]);
                    if (play)
                        store32(base, t.team + (side ? kDefense : kOffense) + 4 * uint32_t(i + 1), play);
                    (play ? renumbered : defaults)++;
                }
        }
        if (teams)
            info("[saves] %d team(s) in this save had audibles numbered by %s: %d renumbered for this version's, %d set to the team's default%s",
                 teams, BB_EXE("the title update's play list", "the disc's play list"), renumbered, defaults,
                 l.other.empty() ? " (the title update's files aren't here to renumber from)" : "");
    }

    template <typename Loader> void loadAndRenumber(Loader loader, PPCContext& ctx, uint8_t* base)
    {
        Load load;
        Load* const outer = t_load;
        t_load = &load;
        loader(ctx, base);
        t_load = outer;
        renumberAudibles(load, ctx, base);
    }
}

PPC_FUNC(sub_82292AF8)
{
    loadAndRenumber(__imp__sub_82292AF8, ctx, base);
}

PPC_FUNC(sub_823A9FC0)
{
    loadAndRenumber(__imp__sub_823A9FC0, ctx, base);
}

// While a loader runs: the number it looks up...
PPC_FUNC(sub_82329B40)
{
    if (Load* load = t_load)
    {
        load->database = ctx.r3.u32;
        load->number = ctx.r4.s32;
        load->looked = true;
    }
    __imp__sub_82329B40(ctx, base);
}

// ...and the team, side and slot it then gives that play to
PPC_FUNC(sub_8223E118)
{
    Load* const load = t_load;
    const uint32_t slot = ctx.r4.u32;
    if (load && load->looked && slot >= 1 && slot <= uint32_t(play_numbers::kSlots))
    {
        load->looked = false;
        const uint32_t team = ctx.r3.u32;
        if (load->teams.empty() || load->teams.back().team != team)
            load->teams.push_back({ team, {} });
        play_numbers::Audibles& saved = load->teams.back().saved;
        ((ctx.r5.u32 & 0xFF) ? saved.offense : saved.defense)[slot - 1] = load->number;
    }
    __imp__sub_8223E118(ctx, base);
}
