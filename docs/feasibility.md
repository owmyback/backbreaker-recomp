# Feasibility analysis - 2026-11-12

## Executable
- Single `default.xex`, XEX2, encryption=normal, compression=basic -> decrypted to valid PE (PowerPC BE)
- Build date 2010-04-25, image base 0x82000000, entry 0x82402578, image size 0xE20000
- ~2.2M instructions in .text + 8 .embsec_ code sections
- .pdata: 19,184 function entries (function boundaries for XenonAnalyse)
- VMX/VMX128: ~4% of instructions
- Imports: xam.xex, xboxkrnl.exe only

## Middleware
- NaturalMotion in-house engine (Nmg*), Euphoria + Morpheme (CPU code)
- FMOD with XMA audio banks (.fsb/.fev)
- WMV/XMV cutscenes (Mocomp video shaders)
- Statically linked D3D; XHV voice chat; Xbox LIVE
- No Havok, Bink, Scaleform

## Verdict
Code translation looks clean. Runtime (GPU -> Vulkan, XMA, video, kernel) is the bulk of the work.
Risk: Euphoria physics floating-point sensitivity.

## Next steps
1. Clone XenonRecomp into third_party/, install clang, build
2. Run XenonAnalyse on game/extracted/default.pe, write TOML config, run XenonRecomp
3. Measure how many functions translate without manual fixes
