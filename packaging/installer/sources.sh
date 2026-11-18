#!/usr/bin/env bash
# The release's third-party-sources.tar.xz (#160): the source of the installer AppImage's GPL and
# LGPL parts that no distribution keeps, attached to each release beside the AppImage:
#   XenonRecomp/   at the commit scripts/setup.sh pins, with its submodules: binutils' PowerPC
#                  disassembler (GPL-2.0-or-later) and libmspack (LGPL-2.1) are built into it
#   xenonrecomp-backbreaker-instructions.patch   this repository's patch, which the build applies
#   FFmpeg/        the xmaframes fork at the commit scripts/build_xma_decoder.sh pins (LGPL-2.1+)
#   libfuse/       fuse-3.15.0.tar.xz and mount.c.diff, which AppImage's type2 runtime (the start of
#                  the AppImage file) is built with (LGPL-2.1)
#   README.txt     the same, for whoever opens it
# glibc, libstdc++ and libgcc come from Ubuntu's source packages, which Launchpad keeps (NOTICES.txt
# links each at its version), and the Linux headers are source already. The rest of the AppImage is
# under licences that ask for no source (NOTICES.txt), or is this project's own: the repository.
#
#   packaging/installer/sources.sh [<out.tar.xz>]   (default build-installer/third-party-sources.tar.xz)
#
# Needs git, curl, tar and xz, and third_party/XenonRecomp (scripts/setup.sh); fetches FFmpeg's
# source into third_party/FFmpeg when it isn't there.
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO"
OUT=${1:-build-installer/third-party-sources.tar.xz}
XENON=third_party/XenonRecomp
PATCH=patches/xenonrecomp-backbreaker-instructions.patch
# The AppImage runtime this was checked against (packaging/portable/Containerfile), and the libfuse
# its scripts/common/install-dependencies.sh builds, with the checksums that script and its tag give
RUNTIME_TAG=20251108
FUSE=fuse-3.15.0
FUSE_SHA256=70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0
MOUNT_DIFF_SHA256=1c7fd9e26717545a476b226b083a9f9d05676c180edbd71a04bbd8a73599dc44

say() { printf '[sources] %s\n' "$*"; }
die() { printf '[sources] ERROR: %s\n' "$*" >&2; exit 1; }

for tool in git curl tar xz sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool is not installed"
done
xenon_rev=$(sed -n 's/^XENON_REV=\([0-9a-f]*\).*/\1/p' scripts/setup.sh)
ffmpeg_rev=$(sed -n 's/^FFMPEG_REV=\([0-9a-f]*\).*/\1/p' scripts/build_xma_decoder.sh)
runtime_tag=$(sed -n 's/^ARG APPIMAGE_RUNTIME_TAG=//p' packaging/portable/Containerfile)
[ "$runtime_tag" = "$RUNTIME_TAG" ] ||
    die "the AppImage runtime is $runtime_tag now: set FUSE and the checksums here from its scripts/common/install-dependencies.sh"
[ -d $XENON/.git ] || die "$XENON is missing: scripts/setup.sh clones and patches it"
[ "$(git -C $XENON rev-parse HEAD)" = "$(git -C $XENON rev-parse "$xenon_rev^{commit}")" ] ||
    die "$XENON isn't at $xenon_rev, the commit scripts/setup.sh pins"
scripts/build_xma_decoder.sh --fetch
[ "$(git -C third_party/FFmpeg rev-parse HEAD)" = "$ffmpeg_rev" ] || die "third_party/FFmpeg isn't at $ffmpeg_rev"

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/sources/libfuse"

say "XenonRecomp $xenon_rev and its submodules, as committed (the patch is beside them)"
git -C $XENON archive --prefix=XenonRecomp/ HEAD | tar -x -C "$stage/sources"
# shellcheck disable=SC2016 # $displaypath and $STAGE are for the command foreach runs
STAGE=$stage/sources git -C $XENON submodule foreach --quiet --recursive \
    'git archive --prefix="XenonRecomp/$displaypath/" HEAD | tar -x -C "$STAGE"'
cp $PATCH "$stage/sources/"

say "FFmpeg (xmaframes fork) $ffmpeg_rev"
git -C third_party/FFmpeg archive --prefix=FFmpeg/ HEAD | tar -x -C "$stage/sources"

say "libfuse ($FUSE) and the AppImage runtime's patch to it"
curl -sSfL -o "$stage/sources/libfuse/$FUSE.tar.xz" \
    "https://github.com/libfuse/libfuse/releases/download/$FUSE/$FUSE.tar.xz"
curl -sSfL -o "$stage/sources/libfuse/mount.c.diff" \
    "https://raw.githubusercontent.com/AppImage/type2-runtime/$RUNTIME_TAG/patches/libfuse/mount.c.diff"
(cd "$stage/sources/libfuse" && sha256sum -c --quiet - <<EOF
$FUSE_SHA256  $FUSE.tar.xz
$MOUNT_DIFF_SHA256  mount.c.diff
EOF
) || die "libfuse's download doesn't match its checksum"

cat >"$stage/sources/README.txt" <<EOF
Backbreaker installer: the source of its GPL and LGPL parts that no distribution keeps

- XenonRecomp/: XenonRecomp (MIT) at $xenon_rev, https://github.com/hedge-dev/XenonRecomp,
  with its submodules. Built into it: thirdparty/disasm, binutils' PowerPC disassembler
  (GPL-2.0-or-later), and thirdparty/libmspack's LZX decoder (LGPL-2.1), so the program as a whole
  is under GPL-2.0-or-later. The installer's XenonRecomp is this tree with
  xenonrecomp-backbreaker-instructions.patch applied (git apply), built with CMake and Ninja in
  Release mode (packaging/installer/build_installer.sh in the Backbreaker repository).
- FFmpeg/: FFmpeg (LGPL-2.1-or-later), the xmaframes fork at $ffmpeg_rev,
  https://github.com/has207/FFmpeg. The installer's libavcodec.a and libavutil.a are built from it
  by scripts/build_xma_decoder.sh in the Backbreaker repository (only the xmaframes decoder, static).
- libfuse/: libfuse $FUSE (its library is LGPL-2.1), https://github.com/libfuse/libfuse, and
  mount.c.diff from AppImage's type2-runtime at $RUNTIME_TAG
  (https://github.com/AppImage/type2-runtime), which builds the runtime at the start of the AppImage
  file with this libfuse, the patch applied (scripts/common/install-dependencies.sh there).

glibc, libstdc++ and libgcc are Ubuntu 22.04's packages: NOTICES.txt links each one's source
package at its version on Launchpad. The Linux headers in the sysroot are source themselves.
Everything else is listed in NOTICES.txt with its licence and source.
EOF

say "packing $OUT"
mkdir -p "$(dirname "$OUT")"
# The same inputs make the same archive: names sorted, no owners, one date (the commit's)
tar -C "$stage" --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$(git log -1 --format=%ct)" -cf - sources | xz -T0 -6 >"$OUT.tmp"
mv "$OUT.tmp" "$OUT"
say "done: $OUT ($(du -h "$OUT" | cut -f1))"
