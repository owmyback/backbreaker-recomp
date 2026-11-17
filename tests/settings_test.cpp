// Tests runtime/settings.cpp (issues #9, #115, #89): reading and writing game/settings.ini.
//
// What is checked, through real files in a temporary directory:
//   - no file: the defaults (1280x720, no position, windowed, scale 1, 60 fps)
//   - a file written by a build before #115 (no resolution_scale key): scale 1, the rest kept; and
//     one from before #89 (no fps key): 60 fps
//   - save then load gives back every key, resolution_scale included - the window writes the file
//     when it closes, and must not drop the scale
//   - hand-edited lines: spaces around the key and value, CRLF line ends, only the one key
//   - resolution_scale out of range or not a number: the nearest valid value (or 1)
//   - fps: 30 or 60, anything else the default (60)
//   - anisotropy: 1-16, default 16; out of range the nearest, not a number the default
//   - native_720p: 0 or 1, default 1; anything else the default
//   - skip_intro: 0 or 1, default 0; anything else the default
//   - a file that can't be written: save returns false
//
// No game files. Build and run: README "Tests" / .github/workflows/ci.yml
//   clang++ -std=c++20 -Wall -Wextra -O2 -I runtime tests/settings_test.cpp runtime/settings.cpp -o build/settings_test

#include "../runtime/settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

static int failures = 0, checks = 0;

static void ok(bool good, const char* what)
{
    checks++;
    if (!good) failures++, printf("FAIL %s\n", what);
}

static std::string g_dir;

static std::string write(const char* name, const char* text)
{
    std::string path = g_dir + "/" + name;
    FILE* f = fopen(path.c_str(), "wb");
    fputs(text, f);
    fclose(f);
    return path;
}

static uint32_t scaleOf(const char* name, const char* text) { return settings::load(write(name, text)).resolutionScale; }
static uint32_t fpsOf(const char* name, const char* text) { return settings::load(write(name, text)).fps; }
static uint32_t anisoOf(const char* name, const char* text) { return settings::load(write(name, text)).anisotropy; }
static bool nativeOf(const char* name, const char* text) { return settings::load(write(name, text)).native720p; }
static bool skipOf(const char* name, const char* text) { return settings::load(write(name, text)).skipIntro; }

int main()
{
    std::string tmpl = (std::filesystem::temp_directory_path() / "settings_test_XXXXXX").string();
    if (!mkdtemp(tmpl.data())) { perror("mkdtemp"); return 1; }
    g_dir = tmpl;

    {
        settings::Settings s = settings::load(g_dir + "/missing.ini");
        ok(s.width == 1280 && s.height == 720, "no file: 1280x720");
        ok(!s.hasPosition && !s.fullscreen, "no file: no position, windowed");
        ok(s.resolutionScale == 1, "no file: scale 1");
        ok(s.fps == 60, "no file: 60 fps");
        ok(s.anisotropy == 16, "no file: anisotropy 16");
        ok(s.native720p, "no file: native 720p");
        ok(!s.skipIntro, "no file: boot logos shown");
    }
    {
        // exactly what the presenter wrote before #115
        settings::Settings s = settings::load(write("old.ini", "width=2560\nheight=1362\nx=0\ny=0\nfullscreen=0\n"));
        ok(s.width == 2560 && s.height == 1362, "old file: window size kept");
        ok(s.hasPosition && s.x == 0 && s.y == 0, "old file: position kept");
        ok(s.resolutionScale == 1, "old file: scale 1");
        ok(s.fps == 60, "old file: 60 fps");
    }
    {
        settings::Settings a;
        a.width = 1920; a.height = 1080; a.x = 40; a.y = 30; a.hasPosition = true;
        a.fullscreen = true; a.resolutionScale = 3; a.fps = 30; a.anisotropy = 4;
        a.fullscreen = true; a.resolutionScale = 3; a.fps = 30; a.native720p = false; a.skipIntro = true;
        std::string path = g_dir + "/roundtrip.ini";
        ok(settings::save(a, path), "save succeeds");
        settings::Settings b = settings::load(path);
        ok(b.width == 1920 && b.height == 1080 && b.x == 40 && b.y == 30 && b.hasPosition, "round trip: window");
        ok(b.fullscreen, "round trip: fullscreen");
        ok(b.resolutionScale == 3, "round trip: resolution_scale kept");
        ok(b.fps == 30, "round trip: fps kept");
        ok(b.anisotropy == 4, "round trip: anisotropy kept");
        ok(!b.native720p, "round trip: native_720p=0 kept");
        ok(b.skipIntro, "round trip: skip_intro=1 kept");
        // and again: a second close of the window writes the same file
        ok(settings::save(b, path) && settings::load(path).resolutionScale == 3, "second save keeps the scale");
        ok(settings::load(path).fps == 30, "second save keeps fps");
    }
    {
        settings::Settings s = settings::load(write("only.ini", "resolution_scale=2\n"));
        ok(s.resolutionScale == 2, "only the key: scale 2");
        ok(s.width == 1280 && s.height == 720 && !s.hasPosition, "only the key: window defaults");
    }
    ok(scaleOf("spaces.ini", "  resolution_scale = 2  \n") == 2, "spaces around key and value");
    ok(scaleOf("crlf.ini", "width=1280\r\nresolution_scale=4\r\n") == 4, "CRLF line ends");
    ok(scaleOf("noeol.ini", "resolution_scale=2") == 2, "no newline at the end");
    ok(scaleOf("high.ini", "resolution_scale=9\n") == 4, "9 -> 4");
    ok(scaleOf("zero.ini", "resolution_scale=0\n") == 1, "0 -> 1");
    ok(scaleOf("negative.ini", "resolution_scale=-2\n") == 1, "-2 -> 1");
    ok(scaleOf("word.ini", "resolution_scale=two\n") == 1, "not a number -> 1");
    ok(scaleOf("suffix.ini", "resolution_scale=2x\n") == 1, "trailing junk -> 1");
    ok(scaleOf("empty.ini", "resolution_scale=\n") == 1, "empty value -> 1");
    ok(scaleOf("last.ini", "resolution_scale=2\nresolution_scale=3\n") == 3, "the last line wins");
    ok(scaleOf("other.ini", "resolution_scale_x=3\nmy_resolution_scale=3\n") == 1, "similar keys are not the key");
    {
        // a file written before #89, the user's kind: every other key kept, 60 fps
        settings::Settings s = settings::load(write("pre89.ini", "width=2560\nheight=1362\nx=0\ny=0\nfullscreen=0\nresolution_scale=2\n"));
        ok(s.fps == 60 && s.resolutionScale == 2 && s.width == 2560, "file from before #89: 60 fps, the rest kept");
    }
    ok(fpsOf("fps30.ini", "fps=30\n") == 30, "fps=30");
    ok(fpsOf("fps60.ini", "fps=60\n") == 60, "fps=60");
    ok(fpsOf("fpsspace.ini", " fps = 30 \r\n") == 30, "fps with spaces and CRLF");
    ok(fpsOf("fps120.ini", "fps=120\n") == 60, "fps=120 -> 60");
    ok(fpsOf("fps45.ini", "fps=45\n") == 60, "fps=45 -> 60");
    ok(fpsOf("fps30x.ini", "fps=30x\n") == 60, "fps=30x -> 60");
    ok(fpsOf("fpsempty.ini", "fps=\n") == 60, "empty fps -> 60");
    ok(fpsOf("fpslast.ini", "fps=60\nfps=30\n") == 30, "fps: the last line wins");
    ok(fpsOf("fpsother.ini", "max_fps=30\nfps_cap=30\n") == 60, "similar keys are not fps");

    {
        // the user's file before the anisotropy key: 16, the rest kept
        settings::Settings s = settings::load(write("preaniso.ini", "width=2560\nheight=1362\nresolution_scale=2\nfps=60\n"));
        ok(s.anisotropy == 16 && s.resolutionScale == 2, "file without anisotropy: 16, the rest kept");
    }
    ok(anisoOf("aniso1.ini", "anisotropy=1\n") == 1, "anisotropy=1 (off)");
    ok(anisoOf("aniso8.ini", " anisotropy = 8 \r\n") == 8, "anisotropy with spaces and CRLF");
    ok(anisoOf("aniso32.ini", "anisotropy=32\n") == 16, "anisotropy=32 -> 16");
    ok(anisoOf("aniso0.ini", "anisotropy=0\n") == 1, "anisotropy=0 -> 1");
    ok(anisoOf("anisoword.ini", "anisotropy=max\n") == 16, "not a number -> 16");
    ok(nativeOf("pre720.ini", "width=2560\nheight=1362\nresolution_scale=2\nfps=60\n"), "file without native_720p: on");
    ok(!nativeOf("native0.ini", "native_720p=0\n"), "native_720p=0");
    ok(nativeOf("native1.ini", " native_720p = 1 \r\n"), "native_720p=1 with spaces and CRLF");
    ok(nativeOf("nativeword.ini", "native_720p=off\n"), "native_720p=off -> the default (1)");
    ok(!skipOf("noskip.ini", "width=2560\nheight=1362\nnative_720p=1\n"), "file without skip_intro: logos shown");
    ok(skipOf("skip1.ini", " skip_intro = 1 \r\n"), "skip_intro=1 with spaces and CRLF");
    ok(!skipOf("skip0.ini", "skip_intro=0\n"), "skip_intro=0");
    ok(!skipOf("skipword.ini", "skip_intro=yes\n"), "skip_intro=yes -> the default (0)");

    ok(!settings::save(settings::Settings{}, g_dir + "/no/such/dir/settings.ini"), "unwritable path: save fails");

    std::filesystem::remove_all(g_dir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
