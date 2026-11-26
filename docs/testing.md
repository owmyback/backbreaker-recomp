# Tests

No game files needed; CI runs these on every push (`.github/workflows/ci.yml`). Run them from the
repo root. The first one reads headers from the XenonRecomp clone (README, "XenonRecomp setup"):

    X=third_party/XenonRecomp
    mkdir -p build && clang++ -std=c++20 -O2 -march=x86-64-v3 -I tests/ppc_config_stub -I $X/XenonUtils -I $X/thirdparty/simde tests/instr_semantics.cpp -o build/instr_semantics && build/instr_semantics

Tiled 2D addressing (`runtime/gpu/tiling.h`, issue #12) is header-only, so nothing else is needed.
Build both translation units - `tests/tiling_test_odr.cpp` is what catches a definition in the
header that is not inline:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/tiling_test.cpp tests/tiling_test_odr.cpp -o build/tiling_test && build/tiling_test

The mip level layout (`runtime/gpu/mip_layout.h`, issue #46) is header-only too:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/mip_layout_test.cpp -o build/mip_layout_test && build/mip_layout_test

So are the resolve clear values per render target format (`runtime/gpu/clear_value.h`, issue #43):

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/clear_value_test.cpp -o build/clear_value_test && build/clear_value_test

And the audio frame conversion (`runtime/apu/frame_convert.h`, issue #90: the game's planar
big-endian 5.1 frames to interleaved 5.1 and to the stereo downmix):

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/audio_convert_test.cpp -o build/audio_convert_test && build/audio_convert_test

The XMA hardware context's packet and frame walk (`runtime/apu/xma_context.h`, issue #93) runs on
synthetic XMA2 packets with a fake frame decoder (no FFmpeg, no game data): frames split across
packets and input buffers, a split frame header, loops with subframe precision, a ring that is
not a multiple of a frame, and a frame the decoder rejects:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/xma_context_test.cpp -o build/xma_context_test && build/xma_context_test

The achievement database parser (`runtime/kernel/spa.h`, issue #97) reads hand-built XDBF blobs:
achievements, their strings per language with the fallback to the title's default, the image
table, UTF-8 to UTF-16, and blobs that are cut short or claim more than they hold:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/spa_test.cpp -o build/spa_test && build/spa_test

The unlock sound (`runtime/apu/system_sound.h`, issue #105) is synthesized, so its length, peak,
silent ends and note order are checked directly:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/system_sound_test.cpp -o build/system_sound_test && build/system_sound_test

The PNG writer (`runtime/debug/png.cpp`, issue #76) is checked by writing images and reading them
back: chunk framing and CRC-32, the IHDR fields, the IDAT zlib stream and its Adler-32, and every
pixel. It links zlib to do the inflating (`zlib1g-dev` on Ubuntu, `zlib-devel` on Fedora), so it
covers IDAT however it is encoded - stored blocks today, real deflate if the writer changes:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/png_test.cpp runtime/debug/png.cpp -lz -o build/png_test && build/png_test

It writes its test images to a temporary directory and removes them again; `PNG_TEST_VERBOSE=1`
prints one line per image (file size, IDAT bytes, which row filters it met).

The texture write watch (`runtime/memory.cpp`, #73) is tested on the real memory layout: writes
through each physical view, two textures sharing a page, file reads into protected pages, and many
threads writing while the pages are re-protected:

    mkdir -p build && clang++ -std=c++20 -O2 -pthread -I runtime tests/memory_watch_test.cpp runtime/memory.cpp -o build/memory_watch_test && build/memory_watch_test

The settings file (`runtime/settings.cpp`, #115) is read and written in a temporary directory:
defaults, a file from before `resolution_scale` existed, save-then-load of every key, hand-edited
lines, and out-of-range or non-numeric scales:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 -I runtime tests/settings_test.cpp runtime/settings.cpp -o build/settings_test && build/settings_test

NTSTATUS to Win32 error codes (`runtime/kernel/ntstatus.h`, #132), what the game's own file calls
see when one fails:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/ntstatus_test.cpp -o build/ntstatus_test && build/ntstatus_test

Archive entries replaced from the mods folder (`runtime/kernel/pack_overlay.cpp`, #169), on archives
the test makes up: the rewritten index, the appended data and reads across it, case-insensitive
names, both byte orders, an index that doesn't parse, and one entry read back with and without a
replacement:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/pack_overlay_test.cpp runtime/kernel/pack_overlay.cpp -o build/pack_overlay_test && build/pack_overlay_test

Play numbers (`runtime/play_numbers.cpp`): a league save keeps each team's audibles as play numbers,
which mean other plays in Title Update 2 than on the disc. On play lists the test makes up: reading
a list, the same play's number in the other version, telling a save from the other version, and
renumbering its audibles, with plays only one version has:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/play_numbers_test.cpp runtime/play_numbers.cpp -o build/play_numbers_test && build/play_numbers_test

The text-entry panel that stands in for the console's on-screen keyboard
(`runtime/video/text_entry.h`): its grid of keys, the selection going around the edges, the game's
length limit, and typed UTF-8:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/text_entry_test.cpp -o build/text_entry_test && build/text_entry_test

The images the renderer has taken out of use and gives to the next request of the same kind
(`runtime/gpu/spare_images.h`, #189): not in the frame they were replaced in, not the one on screen,
and My Teams' uniform editor, which replaces one image eight times for each change of a colour,
making no new image after the first changes:

    mkdir -p build && clang++ -std=c++20 -Wall -Wextra -O2 tests/spare_images_test.cpp -o build/spare_images_test && build/spare_images_test

The tools' own tests (`tests/test_*.py`; `tests/test_pkd.py` for `tools/pkd.py`) need no game files either:

    python3 -m unittest discover -s tests -p 'test_*.py'

To test against a recompile of the real game, replace `-I tests/ppc_config_stub` with `-I ppc`.

`scripts/setup.sh` is linted as well, with shellcheck (same CI file; `apt-get install shellcheck`
if your distro does not ship it):

    shellcheck scripts/setup.sh
