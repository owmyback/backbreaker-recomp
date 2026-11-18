#!/usr/bin/env bash
# shellcheck disable=SC2016 # dpkg-query's '${Field}' formats aren't shell expansions
# Builds the installer (#138): build-installer/Backbreaker-Installer-x86_64.AppImage, and the same
# bundle unpacked in build-installer/AppDir. It holds no game content - our runtime kit, the open-source
# toolchain and XenonRecomp - so it can be published once the licences are settled (#138, part 5).
#
#   packaging/installer/build_installer.sh
#
# Runs in the portable build container (packaging/portable/Containerfile), starting it with podman or
# docker when run on the host, like scripts/setup.sh --portable (BB_CONTAINER_ENGINE=docker picks
# one, as the release workflow does). Needs third_party/XenonRecomp (the patched clone, as
# scripts/setup.sh makes it); builds third_party/ffmpeg-xma-portable (scripts/build_xma_decoder.sh)
# when it's missing; no game files.
#
# The bundle (installer/build.h has the layout) comes from the container, which is Ubuntu 22.04:
#   toolchain/   clang and lld from apt.llvm.org with the libraries they load (not glibc)
#   sysroot/     the files of Ubuntu's libc6, libc6-dev, linux-libc-dev, libstdc++-12-dev,
#                libgcc-12-dev, libvulkan-dev and libvulkan1 - so the game a player builds needs
#                glibc 2.35, like the portable build (#134)
#   kit/         the runtime kit, built with BB_PORTABLE (SDL3, shaderc, FFmpeg static)
#   xenonrecomp/ XenonRecomp with patches/, its ppc_context.h, and SOURCE: the commit and patch
#   config/      config/*.toml
#   MANIFEST.txt every package and version in it, for the licences' source offers, and what an
#                update needs (the ppc inputs hash and prepare.version, .github/workflows/release.yml)
#   NOTICES.txt  every third-party part's licence and source (packaging/installer/notices.sh)
# MANIFEST.txt and NOTICES.txt are copied beside the AppImage too, for the release.
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO"
OUT=build-installer
APPDIR=$OUT/AppDir
B=$APPDIR/usr/lib/backbreaker-installer
XENON=third_party/XenonRecomp
LLVM=/usr/lib/llvm-23

say() { printf '[installer-build] %s\n' "$*"; }
die() { printf '[installer-build] ERROR: %s\n' "$*" >&2; exit 1; }

# --- on the host: run this script in the build container -----------------------------------------
if [ ! -d "$LLVM" ] || [ ! -f /usr/local/share/appimage-runtime-x86_64 ]; then
    if [ -n "${BB_CONTAINER_ENGINE:-}" ]; then engine=$BB_CONTAINER_ENGINE
    elif command -v podman >/dev/null 2>&1; then engine=podman
    elif command -v docker >/dev/null 2>&1; then engine=docker
    else die "needs podman or docker (it runs in the build container, packaging/portable/Containerfile)"
    fi
    tag="backbreaker-portable:$(sha256sum packaging/portable/Containerfile | cut -c1-12)"
    if ! "$engine" image inspect "$tag" >/dev/null 2>&1; then
        say "building the build container $tag (once, about 15 minutes)"
        "$engine" build -t "$tag" -f packaging/portable/Containerfile packaging/portable
    fi
    # GITHUB_*: the branch a CI build names in the kit's version (cmake/git_version.cmake)
    run=("$engine" run --rm -v "$REPO:/src" -w /src --security-opt label=disable -e GITHUB_REF_NAME -e GITHUB_HEAD_REF)
    if [ "$engine" = podman ]; then run+=(--userns=keep-id); else run+=(--user "$(id -u):$(id -g)"); fi
    # third_party may be a symlink elsewhere; a worktree's repository is in the main checkout
    if [ -L third_party ]; then t=$(readlink -f third_party); run+=(-v "$t:$t"); fi
    if [ -f .git ]; then t=$(cd "$(git rev-parse --git-common-dir)" && pwd); run+=(-v "$t:$t:ro"); fi
    exec "${run[@]}" "$tag" packaging/installer/build_installer.sh "$@"
fi

# --- in the container ----------------------------------------------------------------------------
[ -d $XENON/.git ] || die "$XENON is missing: scripts/setup.sh clones and patches it (or README, \"XenonRecomp setup\")"
git -C $XENON apply --reverse --check "$REPO/patches/xenonrecomp-backbreaker-instructions.patch" 2>/dev/null ||
    die "$XENON doesn't have patches/xenonrecomp-backbreaker-instructions.patch applied (scripts/setup.sh applies it)"

rm -rf "$APPDIR" "$OUT/Backbreaker-Installer-x86_64.AppImage" "$OUT/MANIFEST.txt" "$OUT/NOTICES.txt"
mkdir -p "$APPDIR/usr/bin" "$B"

say "the XMA decoder (FFmpeg), built once into third_party/ffmpeg-xma-portable"
scripts/build_xma_decoder.sh --prefix third_party/ffmpeg-xma-portable --build-dir third_party/ffmpeg-portable-src

say "XenonRecomp, built here ($OUT/xenonrecomp) from the patched clone"
cmake -S $XENON -B $OUT/xenonrecomp -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
ninja -C $OUT/xenonrecomp >/dev/null
mkdir -p "$B/xenonrecomp" "$B/config"
install -m 755 $OUT/xenonrecomp/XenonRecomp/XenonRecomp "$B/xenonrecomp/"
install -m 644 $XENON/XenonUtils/ppc_context.h "$B/xenonrecomp/"
# What the installer keys ppc/ on instead of the program's bytes (installer/build.h, bundlePpcInputs):
# another build container's clang makes a different XenonRecomp from the same source
{
    echo "XenonRecomp $(git -C $XENON rev-parse HEAD)"
    echo "patches/xenonrecomp-backbreaker-instructions.patch $(sha256sum patches/xenonrecomp-backbreaker-instructions.patch | cut -d' ' -f1)"
} >"$B/xenonrecomp/SOURCE"
install -m 644 config/backbreaker.toml config/backbreaker_switch_tables.toml "$B/config/"

say "the runtime kit and the installer (BB_PORTABLE, no game files)"
cmake -B $OUT/build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBB_PORTABLE=ON \
    -DBB_RUNTIME_ONLY=ON -DFFMPEG_XMA="$REPO/third_party/ffmpeg-xma-portable" >/dev/null
ninja -C $OUT/build backbreaker_runtime backbreaker-installer >/dev/null
cmake --install $OUT/build --component runtime --prefix "$B/kit" >/dev/null
install -m 755 $OUT/build/backbreaker-installer "$APPDIR/usr/bin/"

say "toolchain: clang and lld $(clang --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')"
T=$B/toolchain
mkdir -p "$T/bin" "$T/lib/clang"
install -m 755 "$(readlink -f $LLVM/bin/clang)" "$T/bin/clang"
ln -s clang "$T/bin/clang++"
install -m 755 "$(readlink -f $LLVM/bin/lld)" "$T/bin/lld"
ln -s lld "$T/bin/ld.lld"
# Only the resource directory's headers: no sanitizer or profile runtimes are used
major=$(ls $LLVM/lib/clang)
mkdir -p "$T/lib/clang/$major"
cp -a "$LLVM/lib/clang/$major/include" "$T/lib/clang/$major/"
# The shared libraries clang and lld load, except glibc's own, which the host provides
owners=()
for lib in $(ldd "$T/bin/clang" "$T/bin/lld" | awk '/=> \// { print $3 }' | sort -u); do
    case $(basename "$lib") in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|ld-linux*|libresolv.so.*) continue ;;
    esac
    install -m 644 "$(readlink -f "$lib")" "$T/lib/$(basename "$lib")"
    # dpkg knows a file by the path its package installed it at: ldd's, or the real one
    owner=$( { dpkg -S "$lib" || dpkg -S "$(readlink -f "$lib")"; } 2>/dev/null | head -1 | cut -d: -f1 || true)
    owners+=("${owner:-$(basename "$lib") (no package found)}")
done

say "sysroot: Ubuntu 22.04's glibc, libstdc++ and Vulkan loader"
S=$B/sysroot
packages=(libc6 libc6-dev linux-libc-dev libstdc++-12-dev libgcc-12-dev libvulkan-dev libvulkan1)
mkdir -p "$S"
# The packages' headers and libraries (not their docs, binaries, locales or gconv modules), with
# symlinks kept as symlinks
dpkg -L "${packages[@]}" | grep -E '^/(usr/)?(include|lib|lib64)/' |
    grep -vE '/(gconv|audit|locale)/|\.(gz)$' | while IFS= read -r f; do
        { [ -f "$f" ] || [ -L "$f" ]; } && [ ! -d "$f" ] && printf '%s\n' "${f#/}"
    done | tar -C / -cf - --no-recursion -T - | tar -C "$S" -xf -
# Absolute symlinks would point at the host's files: they are made to point inside the sysroot.
# Then links to what isn't copied (gcc's sanitizer runtimes, the shared libstdc++: the game links it
# statically) are dropped.
find "$S" -type l | while IFS= read -r link; do
    target=$(readlink "$link")
    case $target in
        /*) ln -sfn "$(realpath -m --relative-to="$(dirname "$link")" "$S$target")" "$link" ;;
    esac
done
find "$S" -xtype l -delete

{
    echo "Backbreaker installer bundle: package versions (Ubuntu 22.04 and apt.llvm.org) for source offers"
    for p in "${packages[@]}" "clang-$major" "lld-$major" "libllvm$major" "libclang-cpp$major"; do
        dpkg-query -W -f='${Package} ${Version}\n' "$p" 2>/dev/null || echo "$p (not a package here)"
    done
    for owner in "${owners[@]}"; do
        dpkg-query -W -f='${Package} ${Version}\n' "$owner" 2>/dev/null || echo "$owner"
    done | sort -u
    echo "XenonRecomp $(git -C $XENON rev-parse --short HEAD) + patches/xenonrecomp-backbreaker-instructions.patch"
    echo "FFmpeg (XMA decoder) $(cat third_party/ffmpeg-xma-portable/.rev 2>/dev/null || echo "see scripts/build_xma_decoder.sh")"
    echo "runtime kit $(cat "$B/kit/share/backbreaker-runtime/version.txt")"
    # The release workflow compares these with the last release's: RecompLauncher reruns install
    # only when prepare.version changes, so new ppc inputs need a new prepare.version
    echo "ppc inputs $("$APPDIR/usr/bin/backbreaker-installer" ppc-inputs)"
    echo "prepare.version $(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["prepare"]["version"])' \
        packaging/installer/recomplauncher.json)"
} >"$B/MANIFEST.txt"

say "notices: every third-party part's licence and source"
# shellcheck disable=SC2046 # the owners' package names, a word each
packaging/installer/notices.sh "$B/NOTICES.txt" "${packages[@]}" "clang-$major" "lld-$major" "libllvm$major" \
    "libclang-cpp$major" $(for o in "${owners[@]}"; do dpkg-query -W -f='${Package}\n' "$o" 2>/dev/null || true; done)

say "AppImage"
cat >"$APPDIR/AppRun" <<'RUN'
#!/bin/sh
# Also works unpacked (--appimage-extract), where the AppImage runtime sets no APPDIR
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/backbreaker-installer" "$@"
RUN
chmod 755 "$APPDIR/AppRun"
cat >"$APPDIR/backbreaker-installer.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Backbreaker Installer
Comment=Builds Backbreaker from your own disc image (static recompilation)
Exec=backbreaker-installer
Icon=backbreaker-installer
Categories=Game;
Terminal=true
DESK
# A plain 1x1 PNG: the game's own icon is the player's (tools/title_icon.py), never shipped
printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\rIDATx\x9cc\xf8\x0f\x04\x00\x09\xfb\x03\xfd\xe3U\xf2\x9c\x00\x00\x00\x00IEND\xaeB`\x82' \
    >"$APPDIR/backbreaker-installer.png"
ln -s backbreaker-installer.png "$APPDIR/.DirIcon"
# RecompLauncher reads /recomplauncher.json from the AppImage itself (#138, part 4)
install -m 644 packaging/installer/recomplauncher.json "$APPDIR/recomplauncher.json"
mksquashfs "$APPDIR" $OUT/.fs.squashfs -root-owned -noappend -comp zstd -Xcompression-level 15 -quiet >/dev/null
cat /usr/local/share/appimage-runtime-x86_64 $OUT/.fs.squashfs >$OUT/Backbreaker-Installer-x86_64.AppImage
chmod 755 $OUT/Backbreaker-Installer-x86_64.AppImage
rm -f $OUT/.fs.squashfs
cp "$B/MANIFEST.txt" "$B/NOTICES.txt" $OUT/

say "done: $OUT/Backbreaker-Installer-x86_64.AppImage ($(du -h $OUT/Backbreaker-Installer-x86_64.AppImage | cut -f1)), unpacked $(du -sh "$APPDIR" | cut -f1)"
say "  toolchain $(du -sh "$T" | cut -f1), sysroot $(du -sh "$S" | cut -f1), kit $(du -sh "$B/kit" | cut -f1)"
