#!/usr/bin/env bash
# Runs a portable build (#134) in containers of other distributions, each with its own Vulkan
# loader and Mesa, to check it starts and renders there. Needs podman; game content stays local.
#
#   packaging/portable/test_distros.sh [dist/Backbreaker-x86_64.AppImage] [distro ...]
#   packaging/portable/test_distros.sh --installer <Backbreaker-Recompiled-x86_64.AppImage> --image <your disc image> [distro ...]
#
# With --installer (#138), each distribution installs the game from the image with the installer
# (packaging/installer/build_installer.sh) - its bundled clang, no compiler in the container - and then
# runs the same checks through `backbreaker-installer play`. The image is mounted read-only under a
# neutral name; its path is never printed. Results go to dist/installer-test/<distro>/ (install.log
# too). BB_TEST_JOBS=<n> sets the installer's parallel compiles (default: by CPU threads and memory).
#
# Distros: ubuntu22 ubuntu24 debian12 debian13 fedora arch (default: all). Each runs the AppImage
# headless (extract-and-run: no FUSE in a container) with its player files in a temporary home,
# then prints the log's verdict and keeps the screenshots in dist/distro-test/<distro>/.
#   GPU tier (Mesa new enough for the host GPU): the Exhibition route to the kickoff, --fixed-seed
#   CPU tier (older Mesa): boot to the title screen on lavapipe, Mesa's software Vulkan driver
# Set BB_TEST_TIER_<distro>=cpu|gpu to override a distro's tier. BB_TEST_SAVES=<dir> starts each
# run with a copy of that saves folder (a fresh profile takes the route elsewhere: the game opens
# Training Camp for a new player).
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
INSTALLER="" IMAGE=""
if [ "${1:-}" = --installer ]; then
    [ $# -ge 4 ] && [ "$3" = --image ] || { echo "usage: $0 --installer <AppImage> --image <disc image> [distro ...]" >&2; exit 2; }
    INSTALLER=$(readlink -f "$2"); IMAGE=$(readlink -f "$4"); shift 4
    [ -f "$INSTALLER" ] || { echo "no installer at $INSTALLER" >&2; exit 1; }
    [ -r "$IMAGE" ] || { echo "the disc image isn't readable (its path is never printed)" >&2; exit 1; }
    APP=$INSTALLER
    OUT=$REPO/dist/installer-test
else
    APP=$(readlink -f "${1:-$REPO/dist/Backbreaker-x86_64.AppImage}")
    shift || true
    OUT=$REPO/dist/distro-test
fi
DISTROS=("$@")
[ ${#DISTROS[@]} -gt 0 ] || DISTROS=(ubuntu22 ubuntu24 debian12 debian13 fedora arch)
[ -f "$APP" ] || { echo "no AppImage at $APP" >&2; exit 1; }

image_of() {
    case $1 in
        ubuntu22) echo "docker.io/library/ubuntu:22.04|apt-get update -qq && apt-get install -y -qq --no-install-recommends mesa-vulkan-drivers libvulkan1 >/dev/null|cpu" ;;
        ubuntu24) echo "docker.io/library/ubuntu:24.04|apt-get update -qq && apt-get install -y -qq --no-install-recommends mesa-vulkan-drivers libvulkan1 >/dev/null|gpu" ;;
        debian12) echo "docker.io/library/debian:12|apt-get update -qq && apt-get install -y -qq --no-install-recommends mesa-vulkan-drivers libvulkan1 >/dev/null|cpu" ;;
        debian13) echo "docker.io/library/debian:13|apt-get update -qq && apt-get install -y -qq --no-install-recommends mesa-vulkan-drivers libvulkan1 >/dev/null|gpu" ;;
        fedora)   echo "registry.fedoraproject.org/fedora:latest|dnf install -y -q mesa-vulkan-drivers vulkan-loader >/dev/null|gpu" ;;
        arch)     echo "docker.io/library/archlinux:latest|pacman -Sy --noconfirm --needed vulkan-radeon vulkan-swrast vulkan-icd-loader >/dev/null|gpu" ;;
        *) return 1 ;;
    esac
}

ROUTE="a@20,a@26,a@42,right@47,a@51,a@80,down@83,down@84.5,down@86,down@87.5,down@89,down@90.5,down@92,a@94,a@108,a@114,a@120,a@126,a@132,a@138"
mkdir -p "$OUT"
for d in "${DISTROS[@]}"; do
    spec=$(image_of "$d") || { echo "unknown distro $d" >&2; exit 2; }
    IFS='|' read -r base install tier <<<"$spec"
    var=BB_TEST_TIER_$d; tier=${!var:-$tier}
    tag=bbtest-$d
    if ! podman image inspect "$tag" >/dev/null 2>&1; then
        echo "[$d] preparing $base"
        printf 'FROM %s\nRUN %s\n' "$base" "$install" | podman build -q -t "$tag" -f - >/dev/null
    fi
    home=$OUT/$d/home; rm -rf "${OUT:?}/$d"; mkdir -p "$home"
    if [ -n "${BB_TEST_SAVES:-}" ]; then
        mkdir -p "$home/.local/share/backbreaker-recomp" && cp -a "$BB_TEST_SAVES" "$home/.local/share/backbreaker-recomp/saves"
    fi
    if [ "$tier" = gpu ]; then
        args=(--headless --mute --fixed-seed --press "$ROUTE" --timeout 160 --screenshot-at "60,110,125,140,150")
        dev=(--device /dev/dri --group-add keep-groups)
        env=()
    else
        args=(--headless --mute --timeout 120 --screenshot-at "30,60,100")
        dev=()
        env=(-e BB_TEST_LAVAPIPE=1)
    fi
    echo "[$d] running ($tier tier${INSTALLER:+, installing from the image first})"
    # The game: the AppImage itself, or the installer's `play` after an install from the image
    start='exec /app/Backbreaker.AppImage "$@"'
    mounts=(-v "$APP:/app/Backbreaker.AppImage:ro")
    if [ -n "$INSTALLER" ]; then
        mounts+=(-v "$IMAGE:/image:ro")
        env+=(-e "BB_TEST_JOBS=${BB_TEST_JOBS:-}")
        start='cd "$TMPDIR" && /app/Backbreaker.AppImage --appimage-extract >/dev/null || exit 90
               begin=$(date +%s)
               squashfs-root/AppRun install /image ${BB_TEST_JOBS:+--jobs "$BB_TEST_JOBS"} > /home/test/install.log 2>&1 ||
                   { rc=$?; echo "[test] install failed with exit $rc"; exit $rc; }
               echo "[test] installed in $(( $(date +%s) - begin )) s"
               exec squashfs-root/AppRun play -- "$@"'
    fi
    set +e
    podman run --rm --userns=keep-id --security-opt label=disable "${dev[@]}" "${env[@]}" \
        -e HOME=/home/test -e APPIMAGE_EXTRACT_AND_RUN=1 -e TMPDIR=/home/test/tmp \
        -v "$home:/home/test" "${mounts[@]}" -w /home/test "$tag" \
        sh -c 'mkdir -p "$TMPDIR"
               if [ -n "${BB_TEST_LAVAPIPE:-}" ]; then # lvp_icd.x86_64.json or lvp_icd.json, by distribution
                   VK_DRIVER_FILES=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1); export VK_DRIVER_FILES VK_ICD_FILENAMES=$VK_DRIVER_FILES
               fi
               '"$start" sh "${args[@]}" > "$OUT/$d/run.log" 2>&1
    rc=$?
    [ -f "$home/install.log" ] && mv "$home/install.log" "$OUT/$d/install.log"
    mv "$home/.local/share/backbreaker-recomp/screenshots" "$OUT/$d/screenshots" 2>/dev/null
    glibc=$(podman run --rm "$tag" sh -c 'ldd --version 2>&1 | head -1' | grep -oE '[0-9]+\.[0-9]+$')
    device=$(grep -m1 -o '\[vulkan\] device: .*' "$OUT/$d/run.log" | cut -c18-)
    grep -q 'rendering is off' "$OUT/$d/run.log" && device="NO VULKAN DEVICE (rendering off)"
    shots=$(find "$OUT/$d/screenshots" -name '*.png' 2>/dev/null | wc -l)
    crash=$(grep -c 'Segmentation fault\|SIGILL\|fatal\|error while loading shared' "$OUT/$d/run.log")
    set -e
    # exit 124 is --timeout's: the run lasted its full time
    installed=$(grep -m1 -oE 'installed in [0-9]+ s|install failed with exit [0-9]+' "$OUT/$d/run.log")
    echo "[$d] exit $rc, glibc $glibc, ${installed:+$installed, }$device, $shots screenshots, $crash crash lines"
done
