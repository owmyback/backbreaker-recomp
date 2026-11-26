#pragma once
// Images the renderer has taken out of use, kept for the next request of the same kind (#189).
//
// A resolve destination is replaced when a resolve to its address has another size or format. The
// image it had can't be destroyed there and then: the presenter may still be showing it, and its
// memory is part of a block that only fills (renderer.cpp, ImageBlock). It used to be put aside for
// good. My Teams' uniform editor redraws the kit into one address at 512x512 and then at 1024x1024,
// four times for each change of a colour, so every change put 21 MB of graphics memory aside
// (measured: 60 changes, 1284 MB), and the renderer ends the run when the card has none left.
//
// Here the image waits, and the next request for an image of its kind takes it instead of making
// one. Two rules keep that as safe as putting it aside was:
// - Not in the frame it was put here. Draws already recorded in that frame may sample it, so within
//   a frame nothing else is drawn into it.
// - Not while it is the image on screen (`shown`): the presenter reads the last front buffer the
//   renderer published, on its own thread, until the next one is published.
// What is in a taken image is whatever its last use left, as a new image's contents are undefined.
//
// Header-only, with no Vulkan in it, so tests/spare_images_test.cpp can check it on its own.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gpu::spare
{
    // What makes two images interchangeable: all are 2D, one level, one layer, one sample
    struct Kind
    {
        uint32_t format = 0, width = 0, height = 0, usage = 0;
        bool operator==(const Kind&) const = default;
    };

    template <class Image> class Pool
    {
    public:
        // `handle` identifies the image for take()'s `shown`; `frame` is the frame it left use in
        void put(const Kind& kind, uint64_t handle, const Image& image, uint64_t frame)
        {
            entries_.push_back({ kind, handle, frame, image });
        }

        // An image of `kind` put here before `frame` that isn't `shown`: the one waiting longest.
        // False when there is none, and a new image is to be made.
        bool take(const Kind& kind, uint64_t frame, uint64_t shown, Image& out)
        {
            for (size_t i = 0; i < entries_.size(); i++)
            {
                const Entry& e = entries_[i];
                if (!(e.kind == kind) || e.frame >= frame || e.handle == shown) continue;
                out = e.image;
                entries_.erase(entries_.begin() + ptrdiff_t(i));
                return true;
            }
            return false;
        }

        size_t size() const { return entries_.size(); }

    private:
        struct Entry
        {
            Kind kind;
            uint64_t handle, frame;
            Image image;
        };
        std::vector<Entry> entries_;
    };
}
