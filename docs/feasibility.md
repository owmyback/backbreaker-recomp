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

# Recompiler run - 2026-11-12

## Tooling
- XenonRecomp @ ddd128b built with clang 22.1.8 (third_party/, gitignored)
- XenonAnalyse found 0 jump tables: its patterns expect `lis/addi/rlwinm/lwzx` and no nops;
  Backbreaker emits `lis/rlwinm/addi/lwzx` and pads with nops.
- Replaced with `tools/find_switch_tables.py`: 431 switch tables (265 word, 122 byte, 44 halfword),
  all labels verified inside their function. Bounds checks can be up to ~80 instructions back.
- 50 switches live in leaf functions with no .pdata entry; XenonRecomp cut those functions short
  (998 "jump outside function" errors). `tools/leaf_functions.py` emits 48 explicit `functions`
  entries bounded by surrounding `bl` targets -> 0 errors.
- Save/restore helpers located by byte pattern (unique hits), see config/backbreaker.toml.

## Result
- 30,191 functions recompiled into 116 C++ files (153 MB); all 116 compile with clang -O2 (~23 s on 16 threads)
- 12,507 instructions XenonRecomp doesn't implement, 62 distinct mnemonics, in 1,120 functions (3.7%):
  - update-form loads/stores: lhzu stfsu lfsu sthu lbzux stbux lwzux stdux ...  (simple)
  - integer carry ops: addc addme subfze eqv; CR ops: cror crorc; branches: bdzf bdnzt
  - VMX halfword/saturating integer ops: vslh vsrah vsubshs vaddsws vspltish vmaxsh vminsh vpk*...
  - plus 39 `vcmpgtuh.` with record bit
  These are all well-defined PowerPC/VMX ops; each needs a few lines in XenonRecomp/recompiler.cpp.

## Not done yet
- longjmp/setjmp addresses
- No runtime: nothing links or runs yet (kernel, GPU, audio, input all missing)

# Missing instructions added - 2026-11-12
- patches/xenonrecomp-backbreaker-instructions.patch (against XenonRecomp ddd128b):
  all 62 missing mnemonics implemented; `vcmpgtuh.` record form added; `bdnzf` now uses the
  real CR bit instead of assuming `eq`; CTR branches (bdz/bdnz/bdnzf/bdzf/bdnzt) tail-call
  when the target is outside the function, like other conditional branches.
- Recompiler run: 0 warnings. All 116 generated files compile.
- tests/instr_semantics.cpp checks the layout-sensitive ops (packs, vsro, vsel128, vctuxs,
  unsigned compares/saturation, carry ops) against hand-computed PowerPC results; a mutation
  (swapped pack operands) is caught.
- Not tested yet: the simple update-form loads/stores and lane-wise ops are only reviewed, not executed.
