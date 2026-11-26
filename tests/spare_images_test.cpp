// Checks runtime/gpu/spare_images.h (#189): the images the renderer has taken out of use and gives
// to the next request of the same kind. No game files, no Vulkan:
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/spare_images_test.cpp -o build/spare_images_test
//
// The last check is the case from the issue: My Teams' uniform editor resolves 512x512 and then
// 1024x1024 to one address, four times for each change of a colour. However many colours are tried,
// the images made stay a handful.
#include "../runtime/gpu/spare_images.h"

#include <cstdio>
#include <set>

using namespace gpu::spare;

static int failures = 0, checks = 0;

static void check(bool ok, const char* what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what);
    }
}

int main()
{
    const Kind small{ 37, 512, 512, 7 }, large{ 37, 1024, 1024, 7 };
    {
        Pool<int> p;
        int out = 0;
        check(!p.take(small, 5, 0, out), "nothing in an empty pool");
        p.put(small, 100, 1, 5);
        check(p.size() == 1, "one waiting");
        check(!p.take(small, 5, 0, out), "not in the frame it was put in");
        check(!p.take(large, 6, 0, out), "not for another size");
        check(!p.take(Kind{ 38, 512, 512, 7 }, 6, 0, out), "not for another format");
        check(!p.take(Kind{ 37, 512, 512, 6 }, 6, 0, out), "not for another usage");
        check(!p.take(small, 6, 100, out), "not while it is on screen");
        check(p.size() == 1, "still waiting after the refusals");
        check(p.take(small, 6, 0, out) && out == 1, "taken in a later frame");
        check(p.size() == 0 && !p.take(small, 7, 0, out), "and only once");
    }
    {
        // The one waiting longest goes first; the one on screen is passed over for the next
        Pool<int> p;
        int out = 0;
        p.put(small, 100, 1, 1);
        p.put(large, 200, 2, 1);
        p.put(small, 300, 3, 2);
        check(p.take(small, 9, 0, out) && out == 1, "the oldest of its kind");
        check(p.take(small, 9, 0, out) && out == 3, "then the next");
        check(p.take(large, 9, 0, out) && out == 2, "the other kind is still there");
        p.put(small, 100, 1, 9);
        p.put(small, 300, 3, 9);
        check(p.take(small, 10, 100, out) && out == 3, "the one on screen is passed over");
        check(p.take(small, 10, 300, out) && out == 1, "and can go once another is on screen");
    }
    {
        // The uniform editor: one address, 512x512 then 1024x1024, four times for each change of a
        // colour, all in one frame; a change every other frame, 2000 of them. The front buffer is
        // another image throughout.
        Pool<int> p;
        std::set<int> made;
        int next = 1, current = 0;
        Kind currentKind{};
        auto resolve = [&](const Kind& kind, uint64_t frame) {
            if (current && currentKind == kind) return;
            if (current) p.put(currentKind, uint64_t(current), current, frame);
            int img = 0;
            if (!p.take(kind, frame, 999999, img))
            {
                img = next++;
                made.insert(img);
            }
            current = img;
            currentKind = kind;
        };
        size_t afterFirst = 0;
        for (uint64_t change = 0; change < 2000; change++)
        {
            const uint64_t frame = 10 + change * 2;
            for (int redraw = 0; redraw < 4; redraw++)
            {
                resolve(small, frame);
                resolve(large, frame);
            }
            if (change == 1) afterFirst = made.size();
        }
        check(afterFirst <= 9, "two changes of a colour make at most nine images");
        check(made.size() == afterFirst, "and 1998 more changes make none");
        check(p.size() + 1 == made.size(), "every image made is the current one or waiting");
    }
    printf("%s (%d checks, %d failures)\n", failures ? "spare image checks FAILED" : "all spare image checks passed", checks, failures);
    return failures ? 1 : 0;
}
