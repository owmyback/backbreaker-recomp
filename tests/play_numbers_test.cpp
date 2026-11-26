// Tests runtime/play_numbers.cpp: a league save's audibles are play numbers, and the two versions
// of the game number their plays differently (runtime/saved_audibles.cpp).
//
// What is checked, on made-up play lists shaped like the game's (a disc's, and an update's with
// more plays in another order):
//   - filesOf: the <File> elements in order, lowercased, white space around them dropped, forward
//     slashes as backslashes; nothing for text without any, and an unclosed last one is left out
//   - makeList: offense from 0, then defense, then special plays, with the two counts
//   - translate: the same file's number in the other list both ways, -1 for a play the other list
//     doesn't have, for a number past the list and for -1
//   - fromAnotherList: a version's own audibles pass; the other version's don't, each way round (a
//     defensive audible that is an offensive play, a number past the list); empty slots don't count
//   - renumber: every audible that exists in both comes back as the same file; a play only one
//     version has, and a number on the wrong side of the list it came from, come back -1; with no
//     other list at all, everything does
//
// No game files. Build and run: docs/testing.md / .github/workflows/ci.yml
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/play_numbers_test.cpp runtime/play_numbers.cpp -o build/play_numbers_test

#include "../runtime/play_numbers.h"

#include <cstdio>
#include <string>

using namespace play_numbers;

static int failures = 0, checks = 0;

static void ok(bool good, const char* what)
{
    checks++;
    if (!good) failures++, printf("FAIL %s\n", what);
}

static std::string listOf(const char* name, std::initializer_list<const char*> files)
{
    std::string xml = std::string("<PDPlayDatabase name=\"") + name + "\">\r\n\r\n";
    for (const char* f : files)
        xml += std::string("  <PDPlay>\r\n    <File>") + f + "</File>\r\n    <Image>" + f + ".dds</Image>\r\n  </PDPlay>\r\n";
    return xml + "</PDPlayDatabase>\r\n";
}

int main()
{
    // filesOf
    {
        auto f = filesOf(listOf("Offense", { "Offense\\Dive.pd", " Offense/Sweep Left.pd\t", "OFFENSE\\Post.PD" }));
        ok(f.size() == 3 && f[0] == "offense\\dive.pd" && f[1] == "offense\\sweep left.pd" && f[2] == "offense\\post.pd",
           "filesOf: in order, lowercased, trimmed, backslashes");
        ok(filesOf("").empty() && filesOf("<PDPlayDatabase name=\"Offense\">\n</PDPlayDatabase>").empty(), "filesOf: none");
        ok(filesOf("<File>a.pd</File><File>b.pd").size() == 1, "filesOf: an unclosed element is left out");
    }

    // A disc's lists: 3 offensive plays (0-2), 3 defensive (3-5), 2 special (6-7)
    const PlayList disc = makeList(listOf("Offense", { "Offense\\Dive.pd", "Offense\\Sweep.pd", "Offense\\Post.pd" }),
                                   listOf("Defense", { "Defense\\Cover 2.pd", "Defense\\Blitz.pd", "Defense\\Prevent.pd" }),
                                   listOf("Special", { "Special\\Punt.pd", "Special\\Field Goal.pd" }));
    // An update's: 5 offensive (0-4), with new ones in front and Post gone; 4 defensive (5-8); the same special plays (9-10)
    const PlayList update = makeList(listOf("Offense", { "Offense\\Bunch Mesh.pd", "Offense\\Spread Slants.pd", "Offense\\Sweep.pd", "Offense\\Dive.pd",
                                                         "Offense\\Ace Screen.pd" }),
                                     listOf("Defense", { "Defense\\3 Deep.pd", "Defense\\Prevent.pd", "Defense\\Cover 2.pd", "Defense\\Blitz.pd" }),
                                     listOf("Special", { "Special\\Punt.pd", "Special\\Field Goal.pd" }));
    ok(disc.files.size() == 8 && disc.offense == 3 && disc.defense == 3, "makeList: the disc's counts");
    ok(update.files.size() == 11 && update.offense == 5 && update.defense == 4, "makeList: the update's counts");
    ok(disc.isOffense(0) && disc.isOffense(2) && !disc.isOffense(3) && !disc.isOffense(-1), "isOffense: 0 to the offensive count");
    ok(disc.isDefense(3) && disc.isDefense(5) && !disc.isDefense(2) && !disc.isDefense(6), "isDefense: the next ones, not the special plays");
    ok(PlayList{}.empty() && !disc.empty(), "empty");

    // translate
    ok(translate(disc, update, 0) == 3 && translate(disc, update, 1) == 2, "translate: offensive plays, disc to update");
    ok(translate(disc, update, 3) == 7 && translate(disc, update, 4) == 8 && translate(disc, update, 5) == 6, "translate: defensive plays, disc to update");
    ok(translate(update, disc, 3) == 0 && translate(update, disc, 7) == 3 && translate(update, disc, 9) == 6, "translate: update to disc");
    ok(translate(disc, update, 2) == -1, "translate: a play the update dropped");
    ok(translate(update, disc, 0) == -1 && translate(update, disc, 5) == -1, "translate: plays only the update has");
    ok(translate(disc, update, 8) == -1 && translate(disc, update, -1) == -1 && translate(PlayList{}, update, 0) == -1,
       "translate: a number past the list, -1, an empty list");

    // A team as each version saves it: Dive, Sweep, Dive, Sweep / Cover 2, Blitz, Prevent, Cover 2
    Audibles byDisc, byUpdate;
    const int discNumbers[2][4] = { { 0, 1, 0, 1 }, { 3, 4, 5, 3 } }, updateNumbers[2][4] = { { 3, 2, 3, 2 }, { 7, 8, 6, 7 } };
    for (int i = 0; i < kSlots; i++)
    {
        byDisc.offense[i] = discNumbers[0][i], byDisc.defense[i] = discNumbers[1][i];
        byUpdate.offense[i] = updateNumbers[0][i], byUpdate.defense[i] = updateNumbers[1][i];
    }

    // fromAnotherList
    ok(!fromAnotherList(disc, byDisc) && !fromAnotherList(update, byUpdate), "fromAnotherList: a version's own audibles pass");
    ok(fromAnotherList(update, byDisc), "fromAnotherList: the disc's in the update (defensive audibles that are offensive plays there)");
    ok(fromAnotherList(disc, byUpdate), "fromAnotherList: the update's in the disc's (numbers past its defensive plays)");
    ok(!fromAnotherList(disc, Audibles{}) && !fromAnotherList(update, Audibles{}), "fromAnotherList: a team with no audibles passes");
    {
        Audibles one;
        one.defense[2] = 1; // one defensive slot holding an offensive play
        ok(fromAnotherList(disc, one), "fromAnotherList: one slot on the wrong side is enough");
        Audibles past;
        past.offense[0] = 40;
        ok(fromAnotherList(disc, past), "fromAnotherList: a number past the list");
    }

    // renumber
    {
        const Audibles a = renumber(disc, update, byDisc);
        bool same = true;
        for (int i = 0; i < kSlots; i++)
            same = same && a.offense[i] == byUpdate.offense[i] && a.defense[i] == byUpdate.defense[i];
        ok(same, "renumber: the disc's numbers as the update's");
        const Audibles b = renumber(update, disc, byUpdate);
        same = true;
        for (int i = 0; i < kSlots; i++)
            same = same && b.offense[i] == byDisc.offense[i] && b.defense[i] == byDisc.defense[i];
        ok(same, "renumber: and back");
        ok(!fromAnotherList(update, a) && !fromAnotherList(disc, b), "renumber: what comes back passes as that version's own");
    }
    {
        Audibles fromUpdate = byUpdate;
        fromUpdate.offense[0] = 0; // Bunch Mesh: only the update has it
        fromUpdate.defense[3] = 5; // 3 Deep: the same
        const Audibles a = renumber(update, disc, fromUpdate);
        ok(a.offense[0] == -1 && a.defense[3] == -1, "renumber: plays this version doesn't have come back -1");
        ok(a.offense[1] == 1 && a.defense[0] == 3, "renumber: the others are still renumbered");
        Audibles fromDisc = byDisc;
        fromDisc.offense[2] = 2; // Post: the update dropped it
        ok(renumber(disc, update, fromDisc).offense[2] == -1, "renumber: a play the update dropped comes back -1");
    }
    {
        Audibles odd = byDisc;
        odd.offense[1] = 4; // a defensive play of the disc's in an offensive slot
        odd.defense[1] = 6; // a special play in a defensive slot
        odd.defense[2] = -1;
        const Audibles a = renumber(disc, update, odd);
        ok(a.offense[1] == -1 && a.defense[1] == -1 && a.defense[2] == -1, "renumber: a number on the wrong side of its own list, or none, comes back -1");
    }
    {
        const Audibles a = renumber(PlayList{}, disc, byUpdate);
        bool none = true;
        for (int i = 0; i < kSlots; i++)
            none = none && a.offense[i] == -1 && a.defense[i] == -1;
        ok(none, "renumber: without the other version's list, nothing is renumbered");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
