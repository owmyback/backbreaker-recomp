#pragma once
// The achievements list the player sees (the overlay and --achievements), as the maintainer asked on
// 2026-11-19: without the online ones, and the others rescaled to 1000 G. The game itself is
// unchanged: XAM's enumerator and the unlocks it reports still use the title's own list (29, 1000 G),
// as on the console. Header-only so tests/achievement_list_test.cpp can check it without game files.
//
// Online play isn't there (roadmap phase 12), so Online, On form (31, 80 G), Head to Head (32, 20 G)
// and Create and Dominate (33, 20 G) can't be earned, and the other 26 come to 880 G. Each of them is
// scaled by 1000/880 and rounded to the nearest 5 G, so achievements worth the same stay worth the
// same. That comes to 1010 G; the two 60 G ones get 65 instead of 70, the step that moves them least
// from their exact share (68.2), to make it 1000:
//   10 -> 10, 20 -> 25, 30 -> 35, 40 -> 45, 60 -> 65, 80 -> 90, 90 -> 100, 100 -> 115
// The table is checked against the title's list when it loads: for another title, or a list that
// isn't the one it was made for, the title's own list is shown unchanged.
#include "spa.h"

#include <cstdint>
#include <iterator>
#include <vector>

namespace kernel::achievement_list
{
    constexpr uint32_t kTitleId = 0x464F07DA;

    struct Entry
    {
        uint16_t id;
        uint16_t title; // its gamerscore in the title's list
        uint16_t shown; // in the player's list; 0: left out
    };

    constexpr Entry kEntries[] = {
        { 1, 40, 45 },  { 2, 10, 10 },   { 3, 20, 25 },   { 4, 30, 35 },  { 5, 10, 10 },  { 6, 10, 10 },
        { 7, 20, 25 },  { 8, 10, 10 },   { 9, 20, 25 },   { 10, 10, 10 }, { 11, 20, 25 }, { 12, 10, 10 },
        { 15, 30, 35 }, { 16, 20, 25 },  { 17, 30, 35 },  { 18, 10, 10 }, { 19, 80, 90 }, { 20, 60, 65 },
        { 21, 40, 45 }, { 22, 80, 90 },  { 23, 100, 115 }, { 24, 60, 65 }, { 25, 90, 100 }, { 26, 20, 25 },
        { 27, 20, 25 }, { 29, 30, 35 },
        { 31, 80, 0 },  { 32, 20, 0 },   { 33, 20, 0 }, // online
    };

    inline const Entry* find(uint16_t id)
    {
        for (const Entry& e : kEntries)
            if (e.id == id) return &e;
        return nullptr;
    }

    // Whether the table is for this list: the title, and the same achievements with the gamerscore
    // the table expects, no more and no fewer.
    inline bool matches(uint32_t titleId, const std::vector<spa::Achievement>& list)
    {
        if (titleId != kTitleId || list.size() != std::size(kEntries)) return false;
        for (size_t i = 0; i < list.size(); i++)
        {
            const Entry* e = find(list[i].id);
            if (!e || e->title != list[i].gamerscore) return false;
            for (size_t j = 0; j < i; j++)
                if (list[j].id == list[i].id) return false;
        }
        return true;
    }

    // The list as the player sees it: the achievements shown, in the title's order, with their
    // gamerscore. Anything else gets the title's own list.
    inline std::vector<spa::Achievement> playerList(uint32_t titleId, const std::vector<spa::Achievement>& list)
    {
        if (!matches(titleId, list)) return list;
        std::vector<spa::Achievement> out;
        for (const spa::Achievement& a : list)
            if (const Entry* e = find(a.id); e->shown)
            {
                out.push_back(a);
                out.back().gamerscore = e->shown;
            }
        return out;
    }
}
