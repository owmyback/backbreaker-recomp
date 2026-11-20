// Checks runtime/kernel/achievement_list.h: the player's achievements list without the online ones,
// rescaled to 1000 G. No game files: the lists here are built from the header's own table. Build:
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/achievement_list_test.cpp -o build/achievement_list_test
#include "../runtime/kernel/achievement_list.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace kernel;
using namespace kernel::achievement_list;

static int failures = 0, checks = 0;

static void expect(bool ok, const std::string& what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what.c_str());
    }
}

// The title's list as the table describes it, in the table's order
static std::vector<spa::Achievement> titleList()
{
    std::vector<spa::Achievement> list;
    for (const Entry& e : kEntries)
    {
        spa::Achievement a;
        a.id = e.id;
        a.gamerscore = e.title;
        a.label = "achievement " + std::to_string(e.id);
        a.imageId = 100 + e.id;
        list.push_back(a);
    }
    return list;
}

static uint32_t total(const std::vector<spa::Achievement>& list)
{
    uint32_t g = 0;
    for (auto& a : list) g += a.gamerscore;
    return g;
}

int main()
{
    // 1. The table itself: the title's 1000 G, and 1000 G shown in steps of 5
    {
        uint32_t title = 0, shown = 0;
        std::set<uint16_t> ids, left;
        bool steps = true, same = true, order = true;
        for (const Entry& e : kEntries)
        {
            title += e.title;
            shown += e.shown;
            ids.insert(e.id);
            if (!e.shown) left.insert(e.id);
            if (e.shown % 5) steps = false;
            for (const Entry& o : kEntries)
            {
                if (!e.shown || !o.shown) continue;
                if (e.title == o.title && e.shown != o.shown) same = false;
                if (e.title < o.title && e.shown > o.shown) order = false;
            }
        }
        expect(ids.size() == std::size(kEntries), "every id once");
        expect(title == 1000, "the title's list is 1000 G (" + std::to_string(title) + ")");
        expect(shown == 1000, "the player's list is 1000 G (" + std::to_string(shown) + ")");
        expect(left == std::set<uint16_t>{ 31, 32, 33 }, "the three online achievements are left out");
        expect(std::size(kEntries) - left.size() == 26, "26 are shown");
        expect(steps, "every shown gamerscore is a multiple of 5");
        expect(same, "achievements worth the same in the title stay worth the same");
        expect(order, "a bigger achievement is never shown as worth less than a smaller one");
        for (const Entry& e : kEntries)
            if (e.shown)
            {
                double exact = e.title * 1000.0 / 880.0;
                expect(e.shown >= exact - 5 && e.shown <= exact + 5,
                       "achievement " + std::to_string(e.id) + " is within 5 G of its exact share");
            }
    }

    // 2. The title's list becomes the player's
    {
        auto list = titleList();
        expect(matches(kTitleId, list), "the title's list matches");
        auto shown = playerList(kTitleId, list);
        expect(shown.size() == 26, "26 in the player's list");
        expect(total(shown) == 1000, "1000 G in the player's list");
        bool online = false, kept = true;
        size_t at = 0;
        for (auto& a : shown)
        {
            if (a.id >= 31) online = true;
            while (at < list.size() && list[at].id != a.id) at++; // the title's order
            if (at == list.size() || a.label != list[at].label || a.imageId != list[at].imageId) kept = false;
        }
        expect(!online, "no online achievement in the player's list");
        expect(kept, "texts, images and order come from the title's list");
        const spa::Achievement* road = nullptr;
        for (auto& a : shown)
            if (a.id == 23) road = &a;
        expect(road && road->gamerscore == 115, "100 G becomes 115 G");
    }

    // 3. Anything else is shown as the title has it
    {
        auto list = titleList();
        expect(playerList(0x4D530001, list).size() == list.size(), "another title: its own list");
        expect(total(playerList(0x4D530001, list)) == 1000, "another title: its own gamerscore");

        auto changed = list;
        changed[0].gamerscore = 50;
        expect(!matches(kTitleId, changed), "a different gamerscore doesn't match");
        expect(playerList(kTitleId, changed).size() == list.size(), "a different gamerscore: the title's own list");

        auto shorter = list;
        shorter.pop_back();
        expect(!matches(kTitleId, shorter), "a missing achievement doesn't match");

        auto twice = list;
        twice[1] = twice[0];
        expect(!matches(kTitleId, twice), "an id twice doesn't match");

        auto extra = list;
        extra.back().id = 40;
        expect(!matches(kTitleId, extra), "an id the table doesn't have doesn't match");

        expect(playerList(kTitleId, {}).empty(), "no achievements: none");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
