#pragma once
// Play numbers, and what they mean in each version of the game (saved_audibles.cpp).
//
// The game numbers its plays as it loads them at boot: the files named in
// media\PlayDatabase\Offensive_Plays.xml from 0, in the order they are listed, then
// Defensive_Plays.xml's, then Special_Plays.xml's. A league save keeps each team's audibles as
// those numbers. Title Update 2 has its own Offensive_Plays.xml and Defensive_Plays.xml, with more
// plays in a different order, so the same number is another play there: 81 is Inferno on the
// disc and FB Power with the update.
//
// Host-only (no guest memory), so tests/play_numbers_test.cpp runs it on made-up lists.
#include <string>
#include <vector>

namespace play_numbers
{
    struct PlayList
    {
        std::vector<std::string> files; // each play's file as its list names it, in lowercase: offense\dagger.pd
        int offense = 0, defense = 0;   // how many of each; the special plays follow
        bool empty() const { return files.empty(); }
        bool isOffense(int number) const { return number >= 0 && number < offense; }
        bool isDefense(int number) const { return number >= offense && number < offense + defense; }
    };

    // The <File> elements of one list, in order, in lowercase
    std::vector<std::string> filesOf(const std::string& xml);
    PlayList makeList(const std::string& offensiveXml, const std::string& defensiveXml, const std::string& specialXml);

    // The number `to` gives the play that `from` numbers `number`: the same file. -1 when `to`
    // doesn't have it, or `from` has no such number.
    int translate(const PlayList& from, const PlayList& to, int number);

    constexpr int kSlots = 4;
    // One team's audibles as play numbers; -1 where there is none
    struct Audibles
    {
        int offense[kSlots] = { -1, -1, -1, -1 };
        int defense[kSlots] = { -1, -1, -1, -1 };
    };

    // True when `own` can't have numbered them: an offensive audible that isn't one of its
    // offensive plays, or a defensive one that isn't one of its defensive plays. The game only lets
    // the player pick an audible from its side's plays, so this means another list numbered them.
    // (The two versions' defensive numbers don't overlap, so a save from the other one always shows.)
    bool fromAnotherList(const PlayList& own, const Audibles& saved);

    // The same audibles in `own`'s numbers, taking the saved ones as `other`'s. -1 where the play
    // isn't in `own`, or isn't on that side there.
    Audibles renumber(const PlayList& other, const PlayList& own, const Audibles& saved);
}
