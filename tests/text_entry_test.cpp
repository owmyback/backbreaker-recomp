// Checks runtime/video/text_entry.h: the text-entry panel's keys, selection and text (the on-screen
// keyboard the game asks for to name a custom team). No game files, no SDL. Build:
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/text_entry_test.cpp -o build/text_entry_test
#include "../runtime/video/text_entry.h"

#include <cstdio>
#include <string>

using namespace overlay::text_entry;

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

static State fresh(size_t maxLength = 14)
{
    State s;
    s.maxLength = maxLength;
    return s;
}

// Moves to a key of the first four rows and presses it
static void pick(State& s, char key)
{
    for (int r = 0; r < kCharRows; r++)
        for (int c = 0; c < kColumns; c++)
            if (kKeys[r][c] == key)
            {
                while (s.row != r) s.move(0, 1);
                while (s.column != c) s.move(1, 0);
                expect(s.press() == Action::None, std::string("a character key is no action: ") + key);
                return;
            }
    expect(false, std::string("no such key: ") + key);
}

static void toBottom(State& s, BottomKey key)
{
    while (s.row != kCharRows) s.move(0, 1);
    while (s.bottomKey() != key) s.move(1, 0);
}

int main()
{
    // The grid: ten keys a row, every key once
    {
        std::string all;
        for (const char* row : kKeys)
        {
            expect(std::string(row).size() == size_t(kColumns), std::string("ten keys in row ") + row);
            all += row;
        }
        for (size_t i = 0; i < all.size(); i++)
            expect(all.find(all[i]) == i, std::string("key once: ") + all[i]);
        expect(kBottomSpan * kBottomKeys == kColumns, "the actions span the grid");
    }

    // Starts on A; picking keys spells a name
    {
        State s = fresh();
        expect(s.keyAt(s.row, s.column) == 'A', "starts on A");
        for (char c : std::string("OWLS")) pick(s, c);
        toBottom(s, kSpaceKey);
        expect(s.press() == Action::None, "space is no action");
        pick(s, '7');
        pick(s, '&');
        expect(s.text == u"OWLS 7&", "picked keys spell the text");
        expect(s.shown() == "OWLS 7&", "shown as typed");
    }

    // The text stops at the game's length
    {
        State s = fresh(3);
        expect(s.type(u'A') && s.type(u'B') && s.type(u'C'), "three fit in three");
        expect(!s.type(u'D'), "a fourth doesn't");
        s.press(); // the selected key, A
        expect(s.text == u"ABC", "a key pressed when full adds nothing");
        expect(s.backspace() && s.text == u"AB", "delete takes the last one");
        expect(s.type(u'Z') && s.text == u"ABZ", "and there is room again");
        State none = fresh(0);
        expect(!none.type(u'A') && none.text.empty(), "a field of no characters takes none");
    }

    // Delete on nothing
    {
        State s = fresh();
        expect(!s.backspace(), "nothing to delete");
        toBottom(s, kDeleteKey);
        expect(s.press() == Action::None && s.text.empty(), "the Delete key on nothing");
    }

    // The selection goes around the edges
    {
        State s = fresh();
        s.row = 0;
        s.column = 0;
        s.move(-1, 0);
        expect(s.row == 0 && s.column == kColumns - 1, "left of the first key is the last");
        s.move(1, 0);
        expect(s.column == 0, "and right of it the first");
        s.move(0, -1);
        expect(s.row == kCharRows, "up from the top row is the actions");
        s.move(0, 1);
        expect(s.row == 0, "and down from them the top row");
    }

    // The actions: each two columns wide, a step sideways is one action, the column comes back
    {
        State s = fresh();
        s.row = kCharRows - 1;
        s.column = 7;
        s.move(0, 1);
        expect(s.row == kCharRows && s.bottomKey() == kCancelKey, "under column 7 is Cancel");
        s.move(0, -1);
        expect(s.row == kCharRows - 1 && s.column == 7, "back up to the same key");
        s.move(0, 1);
        s.move(1, 0);
        expect(s.bottomKey() == kDoneKey, "right of Cancel is Done");
        s.move(1, 0);
        expect(s.bottomKey() == kShiftKey, "right of Done goes around to abc");
        s.move(-1, 0);
        expect(s.bottomKey() == kDoneKey, "and back");
        expect(s.press() == Action::Done, "Done");
        toBottom(s, kCancelKey);
        expect(s.press() == Action::Cancel, "Cancel");
        expect(s.text.empty(), "neither changes the text");
    }

    // abc / ABC: the letters only
    {
        State s = fresh();
        toBottom(s, kShiftKey);
        expect(s.press() == Action::None && s.lower, "abc switches to small letters");
        pick(s, 'Q');
        pick(s, '4');
        pick(s, '-');
        expect(s.text == u"q4-", "small letters, the rest unchanged");
        toBottom(s, kShiftKey);
        s.press();
        pick(s, 'Q');
        expect(s.text == u"q4-Q", "and capitals again");
    }

    // Typed text: UTF-8 in, UTF-16 kept, controls and what doesn't fit in 16 bits left out
    {
        State s = fresh(32);
        expect(s.typeUtf8("Iron Owls") == 9 && s.text == u"Iron Owls", "plain text");
        expect(s.typeUtf8("\xC3\x89") == 1 && s.text.back() == char16_t(0xC9), "two bytes: E acute");
        expect(s.typeUtf8("\xE2\x82\xAC") == 1 && s.text.back() == char16_t(0x20AC), "three bytes: the euro sign");
        const size_t before = s.text.size();
        expect(s.typeUtf8("\xF0\x9F\x8F\x88") == 0 && s.text.size() == before, "four bytes: left out");
        expect(s.typeUtf8("\t\n\x7F") == 0 && s.text.size() == before, "controls: left out");
        expect(s.typeUtf8("\xC3") == 0 && s.text.size() == before, "half a character: left out");
        expect(s.typeUtf8("a\xC3(b") == 3 && s.text.substr(before) == u"a(b", "and what follows it is kept");
        expect(s.shown() == "Iron Owls??a(b", "the panel's font shows ASCII, the rest as ?");
        State small = fresh(4);
        expect(small.typeUtf8("ABCDEFG") == 4 && small.text == u"ABCD", "typed text stops at the length too");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
