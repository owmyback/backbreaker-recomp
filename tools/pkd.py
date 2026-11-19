#!/usr/bin/env python3
r"""
pkd.py - the game's archive: list, extract and repack it, and share mods as changes (#169)

Most of the game is in one archive, `packedfilesx360/PackedOutputX360`: an index (`.pkh`) and
the data (`.pkd`). A file in the mods folder named like one of its entries replaces that entry
when the game starts, with no repacking (docs/modding.md):

    mods/media/PlayDatabase/Offense/Dagger.pd   replaces   media\PlayDatabase\Offense\Dagger.pd

    tools/pkd.py list ['media/PlayDatabase/*' ...]     names and sizes, or just the matching ones
    tools/pkd.py extract OUT ['media/ai/*' ...]        entries as files under OUT
    tools/pkd.py extract --mods 'media/ai/Audibles*'   ... into the mods folder, to edit there
    tools/pkd.py diff -o my-mod.patch                  what the mods folder changes, as a patch
    tools/pkd.py apply my-mod.patch                    a patch into the mods folder, from your copy
    tools/pkd.py repack SRC OUT_PREFIX                 a whole new archive with SRC's files in it

A mod is shared as changes, a unified diff of the text entries it edits, never as the game's
files: `apply` makes the edited files from the player's own archive. Patterns are shell-style
(`*`, `?`, `[...]`), matched without regard to case, with `/` or `\` between folders.

The archive is found in the checkout (game/disc/packedfilesx360/), then in the installed game
(~/.local/share/backbreaker-recomp/install/game/disc/...); `--pack` names another, as either file
or the path without its extension. The mods folder is the checkout's game/mods when the archive is
the checkout's, otherwise ~/.local/share/backbreaker-recomp/mods (`$XDG_DATA_HOME`); `--mods`
names another. Nothing is written inside the checkout unless git ignores the place, as it does
game/: what this writes is the game's content.

Index layout, as verified against the shipped archive (2035 entries):

    u32 entry_count
    u32 total_name_bytes       the sum of the name_len fields below
    per entry:
        u32 name_len           the name is NUL-padded up to this length (one NUL on the shipped one)
        name[name_len]         e.g. "shaders\\shadow.fx360" (backslashes, as the game names them)
        u32 offset, u32 size   where the entry's bytes sit in the .pkd

The byte order is *checked*, not assumed: each order is parsed, and an order is accepted only if
its entries consume the index exactly and every offset + size stays inside the data file. The
shipped archive is little-endian, unlike the rest of the disc. Its entries are in offset order,
each at a 2048-byte boundary, taking at least one 2048-byte block (an empty entry too), so the
data is a multiple of 2048. `repack` keeps that: the shipped archive repacked with nothing
replaced comes back byte for byte.
"""

import argparse
import difflib
import fnmatch
import hashlib
import os
import pathlib
import re
import struct
import subprocess
import sys

CHECKOUT = pathlib.Path(__file__).resolve().parent.parent
ARCHIVE = pathlib.Path("packedfilesx360") / "PackedOutputX360"
ALIGN = 2048
MOD_HEADER = "# Backbreaker mod: changes to entries of the game's archive. Apply with tools/pkd.py apply."


class PackError(Exception):
    """The pack is missing, or its index does not parse in either byte order."""


class ModError(Exception):
    """A mod can't be made or applied; nothing was written."""


def parse_index(pkh, pkd_size):
    """(entries, endian) with entries = [(name, offset, size), ...].

    Tries little- then big-endian and keeps whichever reading is self-consistent: the entries
    must end exactly at the end of the index, and every entry must lie inside the data file.
    That settles the byte order by evidence rather than assumption, and fails loudly if the
    layout ever changes instead of returning nonsense offsets.
    """
    if len(pkh) < 8:
        raise PackError("pack index is too short to hold a header")
    problems = []
    for endian in "<>":
        tag = "little" if endian == "<" else "big"
        count = struct.unpack(endian + "I", pkh[:4])[0]
        if count == 0 or count * 12 > len(pkh):
            problems.append(f"{tag}-endian: {count} entries cannot fit in "
                            f"{len(pkh)} index bytes")
            continue
        p, ents, why, name_bytes = 8, [], None, 0
        total_name_bytes = struct.unpack(endian + "I", pkh[4:8])[0]
        for _ in range(count):
            if p + 4 > len(pkh):
                why = "the entry list runs past the end of the index"
                break
            (nlen,) = struct.unpack(endian + "I", pkh[p:p + 4])
            p += 4
            if nlen == 0 or nlen > len(pkh) - p:
                why = f"a name length of {nlen} does not fit in the index"
                break
            name = pkh[p:p + nlen].split(b"\0")[0].decode("latin1")
            name_bytes += nlen
            p += nlen
            if p + 8 > len(pkh):
                why = "an entry's offset/size pair runs past the end of the index"
                break
            off, size = struct.unpack(endian + "II", pkh[p:p + 8])
            p += 8
            if off + size > pkd_size:
                why = (f"{name or '(unnamed)'} claims {size} bytes at {off}, past the "
                       f"{pkd_size}-byte data file")
                break
            ents.append((name, off, size))
        if why:
            problems.append(f"{tag}-endian: {why}")
            continue
        if p != len(pkh):
            problems.append(f"{tag}-endian: {count} entries cover {p} of {len(pkh)} "
                            f"index bytes")
            continue
        if name_bytes != total_name_bytes:
            problems.append(f"{tag}-endian: the header says {total_name_bytes} bytes of names, "
                            f"the entries hold {name_bytes}")
            continue
        return ents, endian
    raise PackError("the pack index parses in neither byte order:\n  " +
                    "\n  ".join(problems) +
                    "\n  if the pack layout has changed, parse_index() needs updating before "
                    "anything this tool prints can be trusted")


def build_index(entries, endian):
    """The .pkh bytes for [(name, offset, size), ...], names NUL-terminated as the game's are."""
    names = [n.encode("latin1") + b"\0" for n, _, _ in entries]
    out = [struct.pack(endian + "II", len(entries), sum(len(n) for n in names))]
    for raw, (_, off, size) in zip(names, entries):
        out.append(struct.pack(endian + "I", len(raw)) + raw + struct.pack(endian + "II", off, size))
    return b"".join(out)


def key(name):
    """An entry's name or a relative path, as the lookup compares them: lowercase, backslashes."""
    return name.replace("/", "\\").lower()


def rel_path(name):
    """The relative path an entry gets on disk. Refuses names that would leave the folder."""
    parts = name.replace("\\", "/").split("/")
    if not name or name.startswith(("/", "\\")) or any(p in ("", ".", "..") for p in parts):
        raise PackError(f"the entry name {name!r} isn't a plain relative path")
    return pathlib.PurePosixPath(*parts)


def archive_prefix(path):
    """An archive's path without its extension, from either file or the prefix itself."""
    path = pathlib.Path(path)
    return path.with_suffix("") if path.suffix.lower() in (".pkh", ".pkd") else path


def sibling(prefix, ext):
    return prefix.parent / (prefix.name + ext)


class Pack:
    """The entries of one .pkh/.pkd pair."""

    def __init__(self, prefix):
        prefix = archive_prefix(prefix)
        self.prefix = prefix
        self.pkh_path = sibling(prefix, ".pkh")
        self.pkd_path = sibling(prefix, ".pkd")
        for p in (self.pkh_path, self.pkd_path):
            if not p.is_file():
                raise PackError(f"no archive at {self.pkh_path} / {self.pkd_path}")
        self.pkd_size = self.pkd_path.stat().st_size
        self.entries, self.endian = parse_index(self.pkh_path.read_bytes(), self.pkd_size)
        self.by_key = {}
        for e in self.entries:
            self.by_key.setdefault(key(e[0]), e)
        self._data = None

    def close(self):
        if self._data:
            self._data.close()
            self._data = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def data(self, entry):
        _, off, size = entry
        if self._data is None:
            self._data = self.pkd_path.open("rb")
        self._data.seek(off)
        return self._data.read(size)

    def find(self, name):
        return self.by_key.get(key(name))

    def matching(self, patterns):
        if not patterns:
            return list(self.entries)
        pats = [key(p) for p in patterns]
        return [e for e in self.entries if any(fnmatch.fnmatchcase(key(e[0]), p) for p in pats)]


# --- where things are --------------------------------------------------------------------------

def data_home():
    return pathlib.Path(os.environ.get("XDG_DATA_HOME") or pathlib.Path.home() / ".local" / "share")


def checkout_pack():
    return CHECKOUT / "game" / "disc" / ARCHIVE


def find_pack(arg):
    if arg:
        return Pack(arg)
    tried = [checkout_pack(), data_home() / "backbreaker-recomp" / "install" / "game" / "disc" / ARCHIVE]
    for prefix in tried:
        if sibling(prefix, ".pkh").is_file():
            return Pack(prefix)
    raise PackError("the game's archive isn't in either usual place:\n  " +
                    "\n  ".join(str(t) + ".pkh" for t in tried) +
                    "\n  pass --pack with its path: packedfilesx360/PackedOutputX360.pkh in the game's "
                    "disc folder\n  (an install's <prefix>/game/disc; RecompLauncher's is in the "
                    "game's folder, under build/game/disc)")


def mods_folder(arg, pack):
    if arg:
        return pathlib.Path(arg)
    if pack.prefix.resolve() == checkout_pack().resolve():
        return CHECKOUT / "game" / "mods"
    return data_home() / "backbreaker-recomp" / "mods"


def check_writable_place(path):
    """Refuse to write the game's content where git would pick it up (#169's rule)."""
    path = pathlib.Path(path).resolve()
    try:
        path.relative_to(CHECKOUT)
    except ValueError:
        return
    if not (CHECKOUT / ".git").exists():
        return
    try:
        ignored = subprocess.run(["git", "-C", str(CHECKOUT), "check-ignore", "-q", "--no-index", str(path)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    except OSError:
        ignored = False
    if not ignored:
        raise PackError(f"{path} is inside the checkout, where git doesn't ignore it: the game's files "
                        f"must never be committed.\n  write under game/ (ignored), or outside the checkout")


def files_under(root):
    root = pathlib.Path(root)
    return sorted(p for p in root.rglob("*") if p.is_file()) if root.is_dir() else []


class Folder:
    """The files under a folder by their path in it, without regard to case, as the runtime matches
    the mods folder against the archive's entries (runtime/kernel/pack_overlay.cpp): of two files
    whose paths differ only in case, the first in sorted order."""

    def __init__(self, root):
        self.root = pathlib.Path(root)
        self.files = {}
        for p in files_under(self.root):
            self.files.setdefault(key(p.relative_to(self.root).as_posix()), p)

    def find(self, name):
        return self.files.get(key(name))

    def place(self, name):
        """Where a new file for the entry goes: in the folders already there whatever their case, so
        an entry never ends up in a second folder that differs only in case."""
        p = self.root
        for part in rel_path(name).parts[:-1]:
            exact = p / part
            if not exact.is_dir() and p.is_dir():
                exact = next((c for c in sorted(p.iterdir()) if c.is_dir() and c.name.lower() == part.lower()), exact)
            p = exact
        return p / rel_path(name).name


def is_text(blob):
    return b"\0" not in blob


# --- unified diffs -----------------------------------------------------------------------------

def lines_of(blob):
    return blob.decode("latin1").splitlines(keepends=True)


def make_patch(name, original, edited):
    """A unified diff from the entry's original bytes to the edited ones, with its SHA-256 first."""
    a, b = lines_of(original), lines_of(edited)
    path = rel_path(name).as_posix()
    out = [f"# entry {name} sha256 {hashlib.sha256(original).hexdigest()}\n"]
    for line in difflib.unified_diff(a, b, "a/" + path, "b/" + path, n=3):
        if line.startswith(("---", "+++")):
            out.append(line.rstrip("\n") + "\n")
        elif line.endswith("\n"):
            out.append(line)
        else:
            out.append(line + "\n\\ No newline at end of file\n")
    return "".join(out)


HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def parse_patch(text):
    """[{entry, path, sha256, hunks: [(old_start, [(op, line, eol)])]}] from a mod's text."""
    files, cur, sha, entry_name = [], None, None, None
    lines = text.splitlines(keepends=True)
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^# entry (.+) sha256 ([0-9a-f]{64})\s*$", line)
        if m:
            entry_name, sha = m.group(1), m.group(2)
            i += 1
            continue
        if line.startswith("--- ") and i + 1 < len(lines) and lines[i + 1].startswith("+++ "):
            target = lines[i + 1][4:].rstrip("\r\n").split("\t")[0]
            if target.startswith("b/"):
                target = target[2:]
            cur = {"path": target, "entry": entry_name, "sha256": sha, "hunks": []}
            files.append(cur)
            sha = entry_name = None
            i += 2
            continue
        m = HUNK.match(line)
        if m and cur is not None:
            old_len = int(m.group(2)) if m.group(2) is not None else 1
            new_len = int(m.group(4)) if m.group(4) is not None else 1
            body, i = [], i + 1
            seen_old = seen_new = 0
            while i < len(lines) and (seen_old < old_len or seen_new < new_len):
                raw = lines[i]
                op, content = raw[:1], raw[1:]
                if op == "\\":
                    if body:
                        body[-1] = (body[-1][0], body[-1][1], False)
                    i += 1
                    continue
                if raw.strip("\r\n") == "":
                    op, content = " ", ""  # an empty context line whose space was stripped on the way
                if op not in " +-":
                    raise ModError(f"{cur['path']}: line {i + 1} of the patch isn't part of a hunk: {raw.rstrip()!r}")
                body.append((op, content.rstrip("\r\n"), True))
                seen_old += op in " -"
                seen_new += op in " +"
                i += 1
            if i < len(lines) and lines[i].startswith("\\"):
                body[-1] = (body[-1][0], body[-1][1], False)
                i += 1
            if seen_old != old_len or seen_new != new_len:
                raise ModError(f"{cur['path']}: a hunk is cut short (the patch was changed or truncated)")
            cur["hunks"].append((int(m.group(1)), body))
            continue
        i += 1
    return files


def apply_hunks(path, base, hunks):
    """The base bytes with the hunks applied. Lines are compared without their line ends, so a patch
    that lost its CRs on the way still applies; added lines take the base's line end. A hunk that
    doesn't match where it says is looked for nearby, as patch(1) does."""
    lines = lines_of(base)
    crlf = sum(l.endswith("\r\n") for l in lines) > len(lines) // 2
    eol = "\r\n" if crlf else "\n"
    bare = [l.rstrip("\r\n") for l in lines]
    out, pos, shift = [], 0, 0
    for n, (old_start, body) in enumerate(hunks, 1):
        old = [t for op, t, _ in body if op in " -"]
        # Where the hunk says it goes, 0-based: a hunk with no old lines inserts after line old_start
        stated = old_start - 1 if old else old_start
        want = stated + shift
        at = None
        for d in range(len(lines) + 1):
            for cand in (want,) if d == 0 else (want - d, want + d):
                if pos <= cand <= len(lines) - len(old) and bare[cand:cand + len(old)] == old:
                    at = cand
                    break
            if at is not None:
                break
        if at is None:
            raise ModError(f"{path}: hunk {n} doesn't match (the file isn't the one the mod was made from)")
        shift = at - stated
        out.extend(lines[pos:at])
        i = at
        for op, text, has_eol in body:
            if op == " ":
                out.append(lines[i])
                i += 1
            elif op == "-":
                i += 1
            else:
                out.append(text + (eol if has_eol else ""))
        pos = i
    out.extend(lines[pos:])
    # The last line may have lost its line end when the hunk ended there
    for k in range(len(out) - 1):
        if not out[k].endswith("\n"):
            out[k] += eol
    return "".join(out).encode("latin1")


# --- commands ----------------------------------------------------------------------------------

def cmd_list(args):
    with find_pack(args.pack) as pack:
        for name, off, size in pack.matching(args.patterns):
            print(f"{off:>10}  {size:>10}  {name}" if args.long else f"{size:>10}  {name}")
    return 0


def write_file(path, blob):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".pkd-tmp")
    tmp.write_bytes(blob)
    tmp.replace(path)


def cmd_extract(args):
    with find_pack(args.pack) as pack:
        if args.mods:
            if args.out:
                args.patterns.insert(0, args.out)
            if not args.patterns:
                raise PackError("name the entries to copy into the mods folder: every file there replaces "
                                "the game's, and the whole archive is 633 MB")
            out = mods_folder(args.mods_dir, pack)
        elif not args.out:
            raise PackError("extract needs a folder to write to, or --mods")
        else:
            out = pathlib.Path(args.out)
        check_writable_place(out)
        chosen = pack.matching(args.patterns)
        if not chosen:
            raise PackError("no entry matches " + " ".join(repr(p) for p in args.patterns))
        wrote = kept = 0
        folder = Folder(out)
        for entry in chosen:
            blob = pack.data(entry)
            existing = folder.find(entry[0])
            target = existing or folder.place(entry[0])
            if existing and existing.read_bytes() != blob and not args.force:
                print(f"kept {target}: it's already there and differs (--force overwrites it)")
                kept += 1
                continue
            write_file(target, blob)
            wrote += 1
        print(f"{wrote} file(s) written to {out}" + (f", {kept} kept" if kept else ""))
    return 0


def cmd_repack(args):
    with find_pack(args.pack) as pack:
        src = pathlib.Path(args.src)
        out = archive_prefix(args.out)
        for ext in (".pkh", ".pkd"):
            check_writable_place(sibling(out, ext))
        if out.resolve() == pack.prefix.resolve():
            raise PackError("repack writes a new archive; it won't overwrite the one it reads")
        replaced = set()
        entries = []
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp_pkd = sibling(out, ".pkd.tmp")
        folder = Folder(src)
        with tmp_pkd.open("wb") as f:
            for entry in pack.entries:
                mine = folder.find(entry[0])
                blob = mine.read_bytes() if mine else pack.data(entry)
                if mine:
                    replaced.add(key(entry[0]))
                off = f.tell()
                if off + len(blob) > 0xFFFFFFFF:
                    raise PackError("the new archive would be larger than 4 GB, which its index can't address")
                f.write(blob)
                f.write(b"\0" * (-f.tell() % ALIGN if blob else ALIGN))  # an empty entry takes a block too
                entries.append((entry[0], off, len(blob)))
        tmp_pkd.replace(sibling(out, ".pkd"))
        write_file(sibling(out, ".pkh"), build_index(entries, pack.endian))
        unused = [p for k, p in sorted(folder.files.items()) if k not in replaced]
        print(f"{out}.pkh/.pkd: {len(entries)} entries, {len(replaced)} from {src}")
        for p in unused:
            print(f"  left out {p.relative_to(src)}: the archive has no entry by that name")
    return 0


def cmd_diff(args):
    with find_pack(args.pack) as pack:
        mods = mods_folder(args.mods_dir, pack)
        if not mods.is_dir():
            raise ModError(f"there's no mods folder at {mods}")
        parts, binary = [], []
        for p in files_under(mods):
            entry = pack.find(p.relative_to(mods).as_posix())
            if not entry:
                continue
            original, edited = pack.data(entry), p.read_bytes()
            if original == edited:
                continue
            if not (is_text(original) and is_text(edited)):
                binary.append(entry[0])
                continue
            parts.append(make_patch(entry[0], original, edited))
        for name in binary:
            print(f"left out {name}: it isn't text, so it can't be shared as changes", file=sys.stderr)
        if not parts:
            raise ModError(f"nothing in {mods} changes a text entry of the archive")
        text = MOD_HEADER + "\n" + "".join(parts)
        if args.output:
            check_writable_place(pathlib.Path(args.output))
            write_file(pathlib.Path(args.output), text.encode("latin1"))
            print(f"{args.output}: changes to {len(parts)} entr{'y' if len(parts) == 1 else 'ies'}", file=sys.stderr)
        else:
            sys.stdout.buffer.write(text.encode("latin1"))
    return 0


def cmd_apply(args):
    with find_pack(args.pack) as pack:
        mods = mods_folder(args.mods_dir, pack)
        check_writable_place(mods)
        text = pathlib.Path(args.mod).read_bytes().decode("latin1")
        files = parse_patch(text)
        if not files:
            raise ModError(f"{args.mod} holds no changes to apply")
        results = []
        folder = Folder(mods)
        for fp in files:
            entry = pack.find(fp["path"])
            if not entry:
                raise ModError(f"{fp['path']}: the archive has no entry by that name")
            existing = folder.find(entry[0])
            if existing:
                base, source = existing.read_bytes(), f"{existing} (already in the mods folder)"
            else:
                base, source = pack.data(entry), "the archive"
                if fp["sha256"] and hashlib.sha256(base).hexdigest() != fp["sha256"]:
                    raise ModError(f"{entry[0]}: the mod was made from a different copy of this entry")
            try:
                result = apply_hunks(entry[0], base, fp["hunks"])
            except ModError as e:
                raise ModError(f"{e}\n  applied to {source}")
            results.append((existing or folder.place(entry[0]), result, entry[0], existing is not None))
        for target, blob, name, stacked in results:
            write_file(target, blob)
            print(f"{target}: {name}" + (", on top of the file that was there" if stacked else ""))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip(),
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="See the top of tools/pkd.py and docs/modding.md.")
    ap.add_argument("--pack", help="the archive (.pkh, .pkd, or the path without its extension)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="names and sizes of the entries")
    p.add_argument("patterns", nargs="*")
    p.add_argument("-l", "--long", action="store_true", help="the offsets too")
    p.set_defaults(fn=cmd_list)

    p = sub.add_parser("extract", help="entries as files")
    p.add_argument("out", nargs="?", help="the folder to write to (not with --mods)")
    p.add_argument("patterns", nargs="*")
    p.add_argument("--mods", action="store_true", help="into the mods folder, to edit them there")
    p.add_argument("--mods-dir", help="the mods folder")
    p.add_argument("--force", action="store_true", help="overwrite files that are there and differ")
    p.set_defaults(fn=cmd_extract)

    p = sub.add_parser("repack", help="a new archive with the files in SRC instead of the entries they're named like")
    p.add_argument("src")
    p.add_argument("out", help="the new archive's path without its extension")
    p.set_defaults(fn=cmd_repack)

    p = sub.add_parser("diff", help="what the mods folder changes in the archive, as a unified diff")
    p.add_argument("--mods-dir", help="the mods folder")
    p.add_argument("-o", "--output", help="write the patch here instead of to stdout")
    p.set_defaults(fn=cmd_diff)

    p = sub.add_parser("apply", help="a mod's changes into the mods folder, made from your own archive")
    p.add_argument("mod")
    p.add_argument("--mods-dir", help="the mods folder")
    p.set_defaults(fn=cmd_apply)

    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except (PackError, ModError) as e:
        print(f"pkd.py: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
