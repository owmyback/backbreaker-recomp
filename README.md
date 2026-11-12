# Backbreaker (Xbox 360) static recompilation

Exploratory project to statically recompile Backbreaker (2010, NaturalMotion) for native Linux
using XenonRecomp. Requires a legally owned disc image.

## Layout
- `tools/`        - our own helper scripts (disc extraction, XEX decryption)
- `game/`         - extracted/decrypted game files (gitignored, copyrighted)
- `third_party/`  - upstream clones such as XenonRecomp (gitignored)
- `docs/`         - notes and analysis results

## Source ISO
An ISO image made from your own retail Backbreaker disc (its path is not recorded in this repository).

## Reproduce extraction
    python3 -I tools/xdvdfs.py "<iso>" game/extracted default.xex > docs/disc-listing.txt
    python3 -I tools/xex_decrypt.py game/extracted/default.xex game/extracted/default.pe
