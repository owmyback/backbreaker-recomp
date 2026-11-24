#pragma once
// What the overlay's text-entry panel holds and does (the console's on-screen keyboard, which the
// game asks for to name a custom team): the text, the grid of keys and the selected one. The
// drawing and the events are overlay.cpp's. Header-only, with no SDL in it, so
// tests/text_entry_test.cpp can check it on its own.
//
// The grid is four rows of ten characters and a row of five actions, each two columns wide:
//   1 2 3 4 5 6 7 8 9 0
//   A B C D E F G H I J
//   K L M N O P Q R S T
//   U V W X Y Z - ' . &
//   [abc] [Space] [Delete] [Cancel] [Done]
// A typed character can be any the game's text holds (UTF-16); the grid offers these.
#include <cstddef>
#include <string>

namespace overlay::text_entry
{
    constexpr int kColumns = 10, kCharRows = 4, kRows = kCharRows + 1;
    constexpr const char* kKeys[kCharRows] = { "1234567890", "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-'.&" };

    enum class Action { None, Done, Cancel };
    enum BottomKey { kShiftKey, kSpaceKey, kDeleteKey, kCancelKey, kDoneKey, kBottomKeys };
    constexpr int kBottomSpan = kColumns / kBottomKeys; // columns under each action

    struct State
    {
        std::u16string text;
        size_t maxLength = 0; // in characters
        bool lower = false;   // the letters of the grid
        int row = 1, column = 0;

        // The character on a key of the first four rows
        char keyAt(int r, int c) const
        {
            const char k = kKeys[r][c];
            return lower && k >= 'A' && k <= 'Z' ? char(k - 'A' + 'a') : k;
        }

        // On the last row: the action under the selected column
        BottomKey bottomKey() const { return BottomKey(column / kBottomSpan); }

        // Adds a character at the end. False when the text is full or it isn't one to print.
        bool type(char16_t c)
        {
            if (c < 0x20 || c == 0x7F || (c >= 0xD800 && c <= 0xDFFF)) return false; // controls, half a surrogate pair
            if (text.size() >= maxLength) return false;
            text.push_back(c);
            return true;
        }

        // What a keyboard typed, as UTF-8. Characters outside the 16-bit plane are left out.
        // Returns how many were added.
        size_t typeUtf8(const char* s)
        {
            size_t added = 0;
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p;)
            {
                char32_t c = 0xFFFD;
                int more = 0;
                if (*p < 0x80) c = *p;
                else if ((*p & 0xE0) == 0xC0) { c = *p & 0x1F; more = 1; }
                else if ((*p & 0xF0) == 0xE0) { c = *p & 0x0F; more = 2; }
                else if ((*p & 0xF8) == 0xF0) { c = *p & 0x07; more = 3; }
                p++;
                for (; more > 0 && (*p & 0xC0) == 0x80; more--, p++) c = (c << 6) | (*p & 0x3F);
                if (more == 0 && c <= 0xFFFF && type(char16_t(c))) added++;
            }
            return added;
        }

        bool backspace()
        {
            if (text.empty()) return false;
            text.pop_back();
            return true;
        }

        // Moves the selection by one key, around the edges. On the last row a step left or right is
        // a whole action.
        void move(int dx, int dy)
        {
            row = (row + dy % kRows + kRows) % kRows;
            if (row == kCharRows)
            {
                const int key = ((column / kBottomSpan + dx) % kBottomKeys + kBottomKeys) % kBottomKeys;
                // Coming from above the column stays, so going back up returns to the same key
                if (dx) column = key * kBottomSpan;
            }
            else
                column = ((column + dx) % kColumns + kColumns) % kColumns;
        }

        // The selected key, pressed
        Action press()
        {
            if (row < kCharRows)
            {
                type(char16_t(keyAt(row, column)));
                return Action::None;
            }
            switch (bottomKey())
            {
            case kShiftKey: lower = !lower; break;
            case kSpaceKey: type(u' '); break;
            case kDeleteKey: backspace(); break;
            case kCancelKey: return Action::Cancel;
            case kDoneKey: return Action::Done;
            default: break;
            }
            return Action::None;
        }

        // The text for a font that has printable ASCII only: anything else shows as '?'
        std::string shown() const
        {
            std::string out;
            for (char16_t c : text) out += c >= 0x20 && c < 0x7F ? char(c) : '?';
            return out;
        }
    };
}
