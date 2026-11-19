#!/usr/bin/env python3
r"""
shader_effects.py - which game effect does this captured shader belong to? (#80)

A capture names a shader only by a hash of its microcode:

    captures/frame_2310/shaders/ps_4834dce2860f4cb2.bin   (45 dwords)

The game's shader pack says which effect those bytes came from:

    $ python3 tools/shader_effects.py captures/frame_2310/shaders/ps_4834dce2860f4cb2.bin
    ps_4834dce2860f4cb2  shaders\bbshaders\player_highlight.fx360  [exact]  45 dwords at 0x15d8

The pack is `PackedOutputX360.pkh` (index) + `PackedOutputX360.pkd` (data), under
`game/disc/packedfilesx360/`; tools/pkd.py reads its index (`parse_index`). Index layout, as
verified against the shipped pack:

    u32 entry_count            2035 on the shipped pack
    u32 total_name_bytes       93754 = sum of the name_len fields below
    per entry:
        u32 name_len           the name is NUL-padded up to this length
        name[name_len]         e.g. "shaders\\shadow.fx360" (backslashes, as the game names them)
        u32 offset, u32 size   where the file's bytes sit in the .pkd

The byte order is *checked*, not assumed: each order is parsed, and an order is accepted only
if its entries consume the index exactly and every offset + size stays inside the data file.
On the shipped pack that is little-endian; the big-endian reading that the rest of the disc
uses - and that this issue expected - cannot be parsed at all, its first entry claiming a name
0x24000000 bytes long.

57 of those 2035 entries are `.fx360` effect files, and they are what the question is about:
they are the units the game's renderer names (docs/ROADMAP.md refers to `player_highlight.fx360`
pass 0, `bb_ball.fx360`, and so on). Each effect holds the microcode of several shaders, with
other data between them, so a shader's bytes sit partway into an entry.

Measured over the captures in this checkout - 173 distinct shaders (84 pixel, 89 vertex) from
4111 .bin files, across 37 frame folders and the flat `captures/shaders/` that `--dump-shaders`
writes:

    pixel shaders     82 sit verbatim inside one effect, 2 are nowhere in the pack
    vertex shaders     0 verbatim, 62 share one long run, 27 share too little to name an effect
    found in more than one effect: none
    agreement for a partial vertex shader: 50-88% of its dwords, median 71%
    agreement for a vertex shader that misses: 21 of 27 share 9 dwords, 3 share 5, 1 shares
      nothing, and 2 share 12 of their 30 dwords and are dropped by the half-the-shader rule
      rather than by --near 12 - so the threshold is not what leaves 25 of them unnamed
    and that run always ends where the captured microcode does, starting at dword 12-93 of it:
      the capture holds extra dwords in front of the pack's copy of the shader
    and 12 of the 62 partial lines name effects that cannot be separated (bb_cloth, bb_foam,
      bb_leather and bb_armour_leather all agree with vs_22b228a32b5e9c43 over 94 dwords)

So a pixel shader answers the question directly and a vertex shader does not: not one captured
vertex shader matches its pack copy byte for byte. Each shares one unbroken run of dwords with
that copy and differs over the rest - vs_22b228a32b5e9c43 shares dwords 30-125 with
`bbshaders\bb_satin.fx360` (96 of its 126, at byte 0x8bc of that entry), and 94 with `bb_cloth`,
`bb_foam`, `bb_leather` and `bb_armour_leather` - one vertex shader shipped by a family of
materials. The tool reports the run as a labelled `partial`: a shortlist of effects, with how
much agrees, rather than a match it cannot claim or nothing at all. Two pixel shaders are in
the pack nowhere, one of them a 9-dword shader that runs in every capture - whatever built
those two did not read this pack.

Never commit anything from `game/`, nor the `.fx360` files that `--extract` writes out.
"""

import argparse
import mmap
import pathlib
import sys

# The archive's index is read by tools/pkd.py, which documents its layout (#169). Named by path,
# since `python3 -I` doesn't put the script's folder on sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from pkd import PackError, parse_index  # noqa: E402

# The pack is extracted game data, and `game/` is a gitignored working copy of the disc. The
# default is anchored to this file so the tool works whatever the current folder is.
DEFAULT_PACK = str(pathlib.Path(__file__).resolve().parent.parent
                   / "game" / "disc" / "packedfilesx360" / "PackedOutputX360")
DEFAULT_NEAR = 12      # dwords of contiguous agreement needed before a "partial" is printed
NEAR_FRACTION = 0.5    # ... and that run must be at least this much of the shader
NEAR_TIE_FRACTION = 0.1  # agreement this near the best still counts as the same shader
WORD = 4               # microcode is a sequence of big-endian dwords
WINDOW = 4 * WORD      # anchor width used to look for shared runs
CACHE_MAX = 8 << 20    # entries up to this size are kept in memory (every .fx360 is)


class InputError(Exception):
    """Something named on the command line is not a capture folder or a shader dump."""


class Pack:
    """Random access to the entries of one .pkh/.pkd pair."""

    def __init__(self, prefix):
        prefix = pathlib.Path(prefix)
        if prefix.suffix.lower() in (".pkh", ".pkd"):
            prefix = prefix.with_suffix("")
        self.pkh_path = prefix.with_suffix(".pkh")
        self.pkd_path = prefix.with_suffix(".pkd")
        for p in (self.pkh_path, self.pkd_path):
            if not p.is_file():
                raise PackError(
                    f"no pack at {self.pkh_path} / {self.pkd_path}\n"
                    f"  this tool needs the game files, which live under game/ in the disc copy "
                    f"(gitignored);\n  pass --pack PATH to point at another .pkh/.pkd pair.")
        self.pkh = self.pkh_path.read_bytes()
        self.pkd_size = self.pkd_path.stat().st_size
        self.entries, self.endian = parse_index(self.pkh, self.pkd_size)
        self._data = None
        self._map = None
        self._cache = {}
        self._windows = {}

    def close(self):
        for closer in (getattr(self._map, "close", None), getattr(self._data, "close", None)):
            if closer:
                closer()
        self._map = self._data = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def data(self, entry):
        """The bytes of one entry, cached while they are small enough to be worth keeping."""
        if entry in self._cache:
            return self._cache[entry]
        _, off, size = entry
        if self._data is None:
            self._data = self.pkd_path.open("rb")
        self._data.seek(off)
        blob = self._data.read(size)
        if size <= CACHE_MAX:
            self._cache[entry] = blob
        return blob

    def effects(self, suffix=".fx360"):
        return [e for e in self.entries if e[0].lower().endswith(suffix)]

    def entry_at(self, offset):
        best = None
        for e in self.entries:
            if e[1] <= offset < e[1] + e[2]:
                best = e
                break
        return best

    def scan(self, needle):
        """Byte offset of `needle` anywhere in the .pkd, or -1. Used by --all-entries."""
        if self._data is None:
            self._data = self.pkd_path.open("rb")
        if self._map is None:
            if not self.pkd_size:
                return -1          # mmap refuses to map an empty file
            self._map = mmap.mmap(self._data.fileno(), 0, access=mmap.ACCESS_READ)
        return self._map.find(needle)

    def anchors(self, entry):
        """{anchor bytes: [byte offsets]} over one entry, dword-strided."""
        if entry not in self._windows:
            blob = self.data(entry)
            table = {}
            for i in range(0, len(blob) - WINDOW + 1, WORD):
                table.setdefault(blob[i:i + WINDOW], []).append(i)
            self._windows[entry] = table
        return self._windows[entry]


def short(name):
    """shaders\\bbshaders\\bb_cloth.fx360 -> bb_cloth.fx360. A function rather than inline in the
    f-strings below: a backslash inside an f-string expression is a SyntaxError before Python 3.12."""
    return name.rsplit("\\", 1)[-1]


def read_shader(path):
    data = path.read_bytes()
    if not data:
        raise InputError(f"{path}: empty shader dump")
    if len(data) % WORD:
        raise InputError(f"{path}: {len(data)} bytes is not a whole number of dwords - "
                         f"a truncated shader dump?")
    return data


def collect_shaders(inputs):
    """{(stage, hash): (path, copies seen, bytes)} from capture folders and/or .bin files.

    Every file is read here, since the caller needs its bytes anyway, and the same hash seen
    twice is checked to really be the same microcode: the whole tool rests on the filename hash
    identifying the bytes behind it.
    """
    found = {}

    def add(p):
        stage, sep, sha = p.stem.rpartition("_")
        if not sep or stage not in ("vs", "ps"):
            return False
        key = (stage, sha)
        data = read_shader(p)
        if key not in found:
            found[key] = (p, 1, data)
            return True
        path0, copies, data0 = found[key]
        if len(data) != len(data0) or data != data0:
            raise InputError(
                f"{p}: {len(data) // WORD} dwords, but {path0} has the same shader hash with "
                f"{len(data0) // WORD} different dwords - captures from different builds?")
        found[key] = (path0, copies + 1, data0)
        return True

    for raw in inputs:
        p = pathlib.Path(raw)
        if p.is_file():
            if not add(p):
                raise InputError(f"{p}: not named like vs_<hash>.bin or ps_<hash>.bin")
            continue
        if not p.is_dir():
            raise InputError(f"{p}: no such file or folder")
        hits = sorted(p.glob("**/shaders/*.bin")) or sorted(p.glob("**/*.bin"))
        if not hits:
            raise InputError(f"{p}: no shader .bin files under it "
                             f"(a capture folder holds a shaders/ folder)")
        added = sum(1 for f in hits if add(f))
        if not added:
            raise InputError(f"{p}: {len(hits)} .bin files under it, none named like "
                             f"vs_<hash>.bin or ps_<hash>.bin")
    return found


def exact_hits(needle, entries, pack):
    """[(entry, byte offset)] for every entry holding the whole shader verbatim."""
    hits = []
    for e in entries:
        blob = pack.data(e)
        pos = blob.find(needle)
        while pos >= 0:
            hits.append((e, pos))
            pos = blob.find(needle, pos + 1)
    return hits


def best_run(needle, entry, pack):
    """Longest run of dwords the shader and one entry agree on.

    (dwords, first shared dword in the shader, its dword offset in the entry), or (0, 0, 0).
    Anchored on 16-byte windows: exact enough for microcode, and it finds agreement that starts
    anywhere in the shader rather than only at its beginning.
    """
    table = pack.anchors(entry)
    blob = pack.data(entry)
    best = (0, 0, 0)
    for n in range(0, len(needle) - WINDOW + 1, WORD):
        for pos in table.get(needle[n:n + WINDOW], ()):
            delta = pos - n    # the anchor pairs shader byte i with blob byte delta + i
            end = n + WINDOW
            while end + WORD <= len(needle) and delta + end + WORD <= len(blob) \
                    and blob[delta + end:delta + end + WORD] == needle[end:end + WORD]:
                end += WORD
            start = n
            while start - WORD >= 0 and delta + start - WORD >= 0 \
                    and blob[delta + start - WORD:delta + start] == needle[start - WORD:start]:
                start -= WORD
            run = (end - start) // WORD
            if run > best[0]:
                best = (run, start // WORD, (delta + start) // WORD)
    return best


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="map captured shader microcode to the game effect file it came from")
    ap.add_argument("inputs", nargs="*",
                    help="capture folders (captures/frame_1006) and/or shader .bin files")
    ap.add_argument("--pack", default=DEFAULT_PACK, metavar="PREFIX",
                    help=".pkh/.pkd pair without the extension (default "
                         "game/disc/packedfilesx360/PackedOutputX360 in this checkout)")
    ap.add_argument("--effects", action="store_true",
                    help="list the effect entries in the pack and stop")
    ap.add_argument("--extract", metavar="DIR",
                    help="write the .fx360 entries out to DIR (game data: never commit it)")
    ap.add_argument("--all-entries", action="store_true",
                    help="for shaders not found in the effects, scan the rest of the pack too")
    ap.add_argument("--near", type=int, default=DEFAULT_NEAR, metavar="DWORDS",
                    help="smallest shared run worth printing as a partial match, in dwords "
                         f"(default {DEFAULT_NEAR}; 0 turns partial matching off)")
    ap.add_argument("--by-effect", action="store_true",
                    help="group the output by effect instead of sorting by shader")
    ap.add_argument("--quiet", action="store_true", help="just the summary")
    args = ap.parse_args(argv)

    if not args.inputs and not args.effects and not args.extract:
        ap.error("nothing to do: name capture folders or .bin files, or use --effects "
                 "or --extract DIR")

    try:
        pack = Pack(args.pack)
    except PackError as e:
        print(str(e), file=sys.stderr)
        return 2
    try:
        fx = pack.effects()
        endian = "little" if pack.endian == "<" else "big"
        if args.effects:
            print(f"{pack.pkh_path}: {len(pack.entries)} entries, {len(fx)} of them .fx360, "
                  f"index fields are {endian}-endian")
            for name, off, size in fx:
                print(f"  {size:>8}  0x{off:08x}  {name}")
            return 0

        shaders = collect_shaders(args.inputs) if args.inputs else {}
        if shaders and not args.quiet:
            ps = sum(1 for k in shaders if k[0] == "ps")
            print(f"pack: {pack.pkh_path} - {len(pack.entries)} entries, {len(fx)} effects, "
                  f"{endian}-endian index")
            print(f"{len(shaders)} distinct shaders ({ps} pixel, {len(shaders) - ps} vertex) "
                  f"from {sum(v[1] for v in shaders.values())} .bin files\n")

        rows, hit_entries = [], {}
        exact_n = partial_n = missing_n = 0
        for (stage, sha), (path, copies, needle) in shaders.items():
            best_any = (0, 0, 0, None)  # best agreement for the shader being handled
            dwords = len(needle) // WORD
            label = f"{stage}_{sha}"
            seen = f"  (in {copies} captures)" if copies > 1 else ""
            hits = exact_hits(needle, fx, pack)
            if not hits and args.all_entries:
                pos = pack.scan(needle)
                e = pack.entry_at(pos) if pos >= 0 else None
                if e:
                    hits = [(e, pos - e[1])]
            if hits:
                exact_n += 1
                for e, _ in hits:
                    hit_entries[e] = True
                names = ", ".join(e[0] for e, _ in hits)
                where = ", ".join(f"0x{p:x}" for _, p in hits)
                rows.append((label, names, "exact", f"{dwords} dwords at {where}{seen}",
                             "" if len(hits) == 1 else
                             f"  (the same microcode in {len(hits)} entries)"))
                continue

            if args.near:
                runs = []
                for e in fx:
                    run, at_shader, at_pack = best_run(needle, e, pack)
                    if run > best_any[0]:
                        best_any = (run, at_shader, at_pack, e)
                    if run >= args.near and run >= NEAR_FRACTION * dwords:
                        runs.append((run, at_shader, at_pack, e))
                if runs:
                    top = max(r[0] for r in runs)
                    leaders = sorted((r for r in runs if r[0] == top), key=lambda r: r[3][0])
                    run, at_shader, at_pack, entry = leaders[0]
                    partial_n += 1
                    hit_entries[entry] = True
                    ends_at = " (runs to the end of the captured shader)" \
                        if at_shader + run == dwords else ""
                    detail = (f"{run}/{dwords} dwords agree: shader "
                              f"{at_shader}-{at_shader + run - 1} is pack "
                              f"{at_pack}-{at_pack + run - 1}{ends_at}{seen}")
                    # Effects that ship the same vertex shader differ in agreement only by how
                    # much each copy was relinked, which says nothing about which of them drew
                    # this triangle. So the line names the runners-up with their counts instead
                    # of pretending to pick one, and the reader ranks them.
                    margin = max(4, int(round(NEAR_TIE_FRACTION * top)))
                    also = sorted((r for r in runs if top - margin <= r[0] < top),
                                  key=lambda r: (-r[0], r[3][0]))
                    more = ""
                    if len(leaders) > 1:
                        rest = ", ".join(short(r[3][0]) for r in leaders[1:5])
                        more = f"  (the same agreement in {len(leaders)} effects: {rest}"
                        more += ")" if len(leaders) <= 5 else ", ...)"
                    elif also:
                        rest = ", ".join(f"{short(r[3][0])} {r[0]}" for r in also[:4])
                        more = f"  ({len(also)} more within {margin} dwords: {rest}"
                        more += ")" if len(also) <= 4 else f", +{len(also) - 4} more)"
                    rows.append((label, entry[0], "partial", detail, more))
                    continue

            missing_n += 1
            hint = ""
            if args.near and best_any[3] and best_any[0]:
                # Which of the two cuts it failed matters: 12 shared dwords out of a 30-dword
                # shader was not rejected for being short of --near 12.
                which = (f"under the --near {args.near} dwords" if best_any[0] < args.near
                         else f"shorter than half of the {dwords} dwords it is")
                hint = (f"  (best agreement anywhere: {best_any[0]}/{dwords} dwords with "
                        f"{best_any[3][0]}, {which})")
            elif not args.all_entries:
                hint = "  (only the .fx360 entries searched: --all-entries scans the pack)"
            rows.append((label, "-", "not in pack", f"{dwords} dwords{seen}", hint))

        rows.sort(key=lambda r: (r[1] == "-", r[1], r[0]) if args.by_effect else (r[0],))
        width = max([len(r[0]) for r in rows] + [6]) if rows else 6
        for label, name, kind, detail, more in rows:
            if not args.quiet:
                print(f"{label:<{width}}  {name}  [{kind}]  {detail}{more}")

        if args.extract:
            out = pathlib.Path(args.extract)
            picked = fx if not args.inputs else sorted(hit_entries)
            for e in picked:
                dst = out / e[0].replace("\\", "/")
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(pack.data(e))
            print(f"\nwrote {len(picked)} effect files under {out}/ - game data, "
                  f"never commit it (game/ is gitignored)")

        outside = len([e for e in hit_entries if e not in fx])
        used = len(hit_entries) - outside
        print(f"\nsummary: {len(rows)} shaders -> {exact_n} exact, {partial_n} partial, "
              f"{missing_n} not in pack; {used} of {len(fx)} effects used"
              + (f", {outside} outside the effects" if outside else "")
              + ("" if args.all_entries or not missing_n else
                 ", rerun with --all-entries to search the whole pack"))
        if partial_n and not args.quiet:
            print("  'partial' means the pack's copy is not byte for byte what ran. In these")
            print("  captures the shared run always ends where the captured microcode does and")
            print("  starts partway in: what differs is the front of the shader (not yet known")
            print("  why - plausibly the control-flow/fetch part being rewritten when the shader")
            print("  is bound). The agreement count makes the line")
            print("  a shortlist of effects, not an answer: effects that ship the same vertex")
            print("  shader are named together, with the counts that rank them.")
        return 0 if missing_n == 0 else 1
    except InputError as e:
        print(str(e), file=sys.stderr)
        return 2
    finally:
        pack.close()


if __name__ == "__main__":
    sys.exit(main())
