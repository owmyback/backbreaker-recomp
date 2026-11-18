#!/usr/bin/env bash
# Package a portable build (#134) into dist/: run by scripts/setup.sh --portable inside the build
# container, after build-portable/backbreaker is built.
#
#   dist/backbreaker-linux-x86_64/          the plain folder: ./backbreaker starts it
#   dist/backbreaker-linux-x86_64.tar.gz    the same, to copy to another machine
#   dist/Backbreaker-x86_64.AppImage        one file: chmod +x and run
#
# All three hold the player's own game files (game/disc, the extracted executable) and code
# recompiled from them: they are for the player's own machines and are never published.
# The game files are hard links to the checkout's game/ where the file system allows, so the
# folder costs no extra space; the AppImage and the archive are real copies (about 1.1 GB each).
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$REPO"
BIN=${1:-build-portable/backbreaker}
RUNTIME=${APPIMAGE_RUNTIME:-/usr/local/share/appimage-runtime-x86_64}
NAME=backbreaker-linux-x86_64
OUT=dist/$NAME
APPDIR=dist/.AppDir

say() { printf '[package] %s\n' "$*"; }
die() { printf '[package] ERROR: %s\n' "$*" >&2; exit 1; }

[ -x "$BIN" ] || die "no binary at $BIN"
if ! { [ -s game/extracted/default.pe ] && [ -s game/extracted/default.xex ] && [ -d game/disc ]; }; then
    die "game/ is incomplete (disc/, extracted/default.pe and default.xex are needed)"
fi
[ -f "$RUNTIME" ] || die "no AppImage runtime at $RUNTIME (the build container has it)"

# Copy game data as hard links when possible (same file system), else as files
link_tree() { cp -al "$1" "$2" 2>/dev/null || cp -a "$1" "$2"; }

game_data() { # $1 = the game/ directory to fill
    mkdir -p "$1/extracted"
    link_tree game/disc "$1/disc"
    for f in default.pe default.xex; do link_tree "game/extracted/$f" "$1/extracted/$f"; done
    # The title's icon for the game's window (runtime/video/presenter.cpp); none is fine
    python3 -I tools/title_icon.py game/extracted/default.pe "$1/icon.png" >/dev/null 2>&1 || true
}

rm -rf "$OUT" "$APPDIR" "dist/$NAME.tar.gz" dist/Backbreaker-x86_64.AppImage
mkdir -p "$OUT/bin"

# --- the folder -----------------------------------------------------------------------------
say "folder: $OUT"
install -m 755 "$BIN" "$OUT/bin/backbreaker"
game_data "$OUT/game"
cat > "$OUT/backbreaker" <<'LAUNCH'
#!/bin/sh
# Starts the game from wherever this folder is. Settings and saves go to
# ~/.config/backbreaker-recomp and ~/.local/share/backbreaker-recomp (docs/runtime.md, "Where files live").
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/bin/backbreaker" --data-dir "$HERE/game" "$@"
LAUNCH
chmod 755 "$OUT/backbreaker"
cat > "$OUT/README.txt" <<'TXT'
Backbreaker (static recompilation) - your own build, from your own disc. Not for sharing.

Run:        ./backbreaker            (any options: ./backbreaker --help is not a thing; see the repo's docs/runtime.md)
Settings:   ~/.config/backbreaker-recomp/settings.ini
Saves etc.: ~/.local/share/backbreaker-recomp/  (saves, achievements, screenshots, captures)
Mods:       ~/.local/share/backbreaker-recomp/mods/ - a file there replaces the disc's file at the same path
Needs:      x86-64 CPU with AVX2 (2013+), a Vulkan 1.3 driver, glibc 2.35+ (any current distribution)
Steam Deck: add ./backbreaker (or the AppImage) as a non-Steam game; Gaming Mode works with Steam Input.
TXT

# --- the AppImage ---------------------------------------------------------------------------
say "AppImage: dist/Backbreaker-x86_64.AppImage"
mkdir -p "$APPDIR/usr/bin"
install -m 755 "$BIN" "$APPDIR/usr/bin/backbreaker"
game_data "$APPDIR/game"
cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
# The runtime sees APPIMAGE/APPDIR and reads the game from $APPDIR/game (docs/runtime.md, "Where files live")
exec "$APPDIR/usr/bin/backbreaker" "$@"
RUN
chmod 755 "$APPDIR/AppRun"
cat > "$APPDIR/backbreaker-recomp.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Backbreaker
Comment=Backbreaker (static recompilation)
Exec=backbreaker
Icon=backbreaker-recomp
Categories=Game;SportsGame;
Terminal=false
StartupWMClass=backbreaker-recomp
DESK
if python3 -I tools/title_icon.py game/extracted/default.pe "$APPDIR/backbreaker-recomp.png" >/dev/null; then
    say "icon: the title's own image"
else
    # No title image: a plain 1x1 PNG keeps the AppImage valid
    printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\rIDATx\x9cc\xf8\x0f\x04\x00\x09\xfb\x03\xfd\xe3U\xf2\x9c\x00\x00\x00\x00IEND\xaeB`\x82' \
        > "$APPDIR/backbreaker-recomp.png"
fi
ln -s backbreaker-recomp.png "$APPDIR/.DirIcon"
mksquashfs "$APPDIR" dist/.fs.squashfs -root-owned -noappend -comp zstd -Xcompression-level 3 -quiet >/dev/null
cat "$RUNTIME" dist/.fs.squashfs > dist/Backbreaker-x86_64.AppImage
chmod 755 dist/Backbreaker-x86_64.AppImage
rm -rf "$APPDIR" dist/.fs.squashfs

# --- the archive ----------------------------------------------------------------------------
say "archive: dist/$NAME.tar.gz"
tar -C dist -czf "dist/$NAME.tar.gz" "$NAME"

say "done: $(du -h dist/Backbreaker-x86_64.AppImage | cut -f1) AppImage, $(du -h "dist/$NAME.tar.gz" | cut -f1) archive"
