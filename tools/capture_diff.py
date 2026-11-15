#!/usr/bin/env python3
"""capture_diff.py - what changed between two GPU frame captures (issue #79).

    python3 -I tools/capture_diff.py captures/frame_1006 captures/frame_1026
    python3 -I tools/capture_diff.py --brief build/cap_before/frame_40 cap_after/frame_40

A frame capture (runtime/gpu/capture.cpp) is a folder: summary.txt with the counts and the
sets the frame used, draws.txt with one block per draw (1.4 MB for a full frame),
registers_start.txt with the register dump the frame started from, and shaders/*.bin holding
the microcode. Comparing two captures with plain diff answers the wrong question twice over:
draws.txt is mostly raw register deltas that move every time a vertex buffer is allocated
somewhere else, and diff reads one changed draw as changing every block after it, because each
block is numbered. A 20-draw change comes back as a 5000-line hunk.

So compare at the level the question is asked in:

  * counts: draws, EDRAM copies/resolves, the primitive mix, PM4 packet types;
  * sets: render target setups, textures, pipeline states, and the microcode each shader stage
    used - including the .bin files themselves, because the file name is a hash of the
    microcode and the same name with different bytes means something is wrong;
  * the resolve/copy list: the EDRAM copies a frame performed, in the order it performed them;
  * the frame-start register dump: which registers the frame began with, and which of them a
    different frame left in a different state;
  * the draw sequence: the draw number is dropped from each block, so an inserted draw reads as
    an insertion instead of shifting the rest of the frame, and blocks line up on what they are
    (same opcode, primitive, index source) rather than on exact text, so a draw whose state
    changed is reported as that change one field at a time, not as a delete plus an insert;
  * self-consistency: the counts summary.txt claims have to match what the other two files hold -
    the draw blocks in draws.txt, the EDRAM copies in it, the .bin files next to the shader list -
    since a capture that lost part of its contents is not evidence of anything, and can otherwise
    match a capture that kept them.

By default a section that reports nothing is left out of the output, so a 900-draw frame prints
only what moved; `--all` prints every section, changed or not, which is the way to check that a
section you expected to move is the one that did not.

By default the per-draw comparison leaves out the raw register deltas and the base addresses of
vertex buffers and textures - those record where something happened to be allocated, not what
was drawn, and without dropping them two runs of the same scene never match. `--raw` puts them
back in when the allocation is the thing being chased. What is printed among those register
deltas is not all registers, though, and two of the things in there are kept either way: the
shader constants (`vs c4 = (...)`, a uniform - where an object stands and what colour it is,
which is most of what moves from one frame to the next) and the textures (compared as what they
are, format and size, not the address they were uploaded to). Two more things in there are left
out. The "(N ALU constant dwords changed)" marker counts dwords that moved since the draw *before*
this one, so it moves when the previous draw moves; and the vertex fetch constants are not kept
yet, because by design the capture hashes 64 KB from wherever they point, which is more than the
draw reads and moves between two frames of a still picture. `--raw` shows both as written.

Exit status: 0 if the two captures agree, 1 if they differ, 2 if either folder is not a
capture. A section in summary.txt that this script does not know about is reported as an
unknown section rather than skipped, so a capture from a newer runtime shows up as a
difference instead of hiding one.

`--max N` caps the items per section and `--fields N` the changed fields shown for one draw, so
the report stays a report; both say how much they left out and what to raise them to.
"""

import argparse
import difflib
import re
import sys
from pathlib import Path

WIDTH = 200        # compact draw lines are long; the preview is truncated, the verdict is not

# "    vs c12 = (1, 2, 3, 4)", which the capture prints among the register changes but which is
# not a register: it is one of the shader's inputs. The "(N ALU constant dwords changed)" line
# above them counts dwords that moved since the previous draw, and is treated with the deltas.
ALU_CONST = re.compile(r"(vs|ps) c\d+ = \(")
# "  vs=1111111111111111 ps=2222222222222222" - which program pair a draw ran.
VS_PS = re.compile(r"^vs=([0-9a-f]{4,16})\s+ps=([0-9a-f]{4,16})$")


class Capture:
    """One capture folder, as far as this script understands it."""

    def __init__(self, path, raw=False):
        self.dir = Path(path)
        self.name = self.dir.name
        self.raw = raw
        self.missing = []
        self.frame = "?"
        self.front = ""
        self.build = ""           # "built from <rev> (<branch>, built <date>)", captures since #75
        self.draws = None
        self.copies = None
        self.prim = {}
        self.prim_claimed = {}    # what summary.txt says the primitive mix was, counted per type
        self.shader_counts = {}
        self.rt = []
        self.tex = []
        self.states = {}          # state string -> draws that used it
        self.pairs = {}           # "vs ps" -> draws that ran that program pair
        self.pm4 = {}             # opcode -> packet count, from the summary's histogram
        self.unknown = {}         # section title -> lines, anything not recognised below
        self.events = []          # draws and shader loads, in order, one string each
        self.draw_blocks = 0      # how many draw blocks draws.txt actually holds
        self.copy_list = []       # the EDRAM copies, in order
        self.reg_start = {}       # register address -> (name, value)
        self.shader_files = {}    # "vs_<hash>" / "ps_<hash>" -> microcode bytes

        self._summary(self._lines("summary.txt"))
        self._draws(self._lines("draws.txt"))
        self._regs(self._lines("registers_start.txt"))
        self._shader_files()

    def _lines(self, fname):
        p = self.dir / fname
        if not p.is_file():
            self.missing.append(fname)
            return None
        return p.read_text(errors="replace").splitlines()

    # --- summary.txt ---------------------------------------------------------------------
    # Format: a column-0 "what it is" line per section, indented detail under each. Parsing it
    # by section title rather than by position means a capture that gains a section, or gets
    # them in a different order, still reads correctly.
    def _summary(self, lines):
        if lines is None:
            return
        section = None
        for line in lines:
            if not line.strip():
                continue
            if line[0].isspace():
                if section is not None:
                    self._item(section, line.strip())
                continue
            if line.startswith("Backbreaker GPU frame capture"):
                self.frame = line.rsplit("frame", 1)[-1].strip()
                continue
            if line.startswith("front buffer"):
                self.front = line[len("front buffer"):].strip()
                continue
            if line.startswith("built from "):
                # Which binary made the capture (#75). Not a difference: comparing the same frame
                # captured by two builds is the bisect this tool exists for.
                self.build = line[len("built from "):].strip()
                continue
            if line.startswith("draws:"):
                m = re.search(r"draws:\s*(\d+)", line)
                c = re.search(r"resolves:\s*(\d+)", line)
                self.draws = int(m.group(1)) if m else None
                self.copies = int(c.group(1)) if c else 0
                continue
            if line.startswith("shaders used:"):
                for stage in ("vertex", "pixel"):
                    m = re.search(r"(\d+)\s+%s" % stage, line)
                    if m:
                        self.shader_counts[stage] = int(m.group(1))
                continue
            # Everything else is "title: maybe some trailing stats"; the detail is indented.
            section = line.rsplit(":", 1)[0].strip()
            self.unknown.setdefault(section, [])

    def _item(self, section, item):
        if section.startswith("primitive types"):
            m = re.match(r"(\S+)\s+(\d+)$", item)
            if m:
                self.prim_claimed[m.group(1)] = int(m.group(2))
                self.unknown.pop(section, None)
        elif section.startswith("render target setups"):
            self.rt.append(item)
            self.unknown.pop(section, None)
        elif section.startswith("textures seen"):
            self.tex.append(item)
            self.unknown.pop(section, None)
        elif section.startswith("pipeline states"):
            m = re.match(r"(\d+)\s+(.*)$", item)
            if m:
                self.states[m.group(2)] = self.states.get(m.group(2), 0) + int(m.group(1))
                self.unknown.pop(section, None)
        elif section.startswith("PM4"):
            m = re.match(r"(0x[0-9A-Fa-f]+)\s+(\d+)$", item)
            if m:
                self.pm4[m.group(1)] = int(m.group(2))
                self.unknown.pop(section, None)
        else:
            self.unknown.setdefault(section, []).append(item)

    # --- draws.txt -----------------------------------------------------------------------
    # Blank-line separated blocks, one per draw, plus "load ... shader" lines between them.
    # Each block collapses to a single string: a draw then becomes one unit of the comparison,
    # instead of a dozen lines that can each drift on their own.
    def _draws(self, lines):
        if lines is None:
            return
        cur = None
        in_raw = False

        def flush():
            nonlocal cur
            if cur is not None:
                joined = " ; ".join(cur)
                self.events.append(joined)
                pair = next((VS_PS.match(f) for f in cur[1:] if f.startswith("vs=")), None)
                if pair:
                    key = "%s %s" % (pair.group(1), pair.group(2))
                    self.pairs[key] = self.pairs.get(key, 0) + 1
                cur = None

        for line in lines:
            if line.startswith("draw "):
                flush()
                # "draw 133  DRAW_INDX prim=..." - the ordinal is dropped. Comparing the
                # sequence rather than the number is what keeps one inserted draw from
                # showing up as the whole rest of the frame having changed.
                m = re.match(r"draw\s+\d+\s+(.*)$", line)
                self.draw_blocks += 1
                head = m.group(1) if m else line
                pm = re.search(r"\bprim=([a-z_0-9]+)", head)
                if pm:
                    k = pm.group(1)
                    self.prim[k] = self.prim.get(k, 0) + 1
                if not self.raw:
                    # The dma source's base address is where the index buffer happened to be
                    # allocated this run; the draw itself is the same draw.
                    head = re.sub(r"base=[0-9A-Fa-f]+", "base=*", head)
                cur = [head]
                in_raw = False
                continue
            if line.startswith(("load vertex shader ", "load pixel shader ")):
                flush()
                self.events.append(line.strip())
                continue
            if cur is None or not line.startswith("  "):
                continue
            body = line.strip()
            if body.startswith("raw register changes"):
                in_raw = True
                if self.raw:
                    cur.append("raw register changes:")
                continue
            if in_raw and not self.raw:
                # Two of the things listed under the register changes are not registers at all:
                # the shader constants, which are the uniforms - where an object stands, what
                # colour it is - and the textures, which are resources. Both stay in, the first
                # as written and the second as what it is rather than where it lives. What goes
                # is the register writes themselves, which restate the decoded state above.
                if body.startswith("tex["):
                    cur.append(tex_identity(body))
                elif ALU_CONST.match(body):
                    cur.append(body)
                # The "(N ALU constant dwords changed)" marker that heads those constants is left
                # out with the register writes. It counts dwords that moved since the draw *before
                # this one* in the frame, and the shadow it compares against is updated as the
                # frame goes, so it changes when the previous draw changes even if this draw is
                # untouched - and in captures from before the constants themselves were printed it
                # can name 271 changed dwords with not one constant listed. The values are what
                # say what this draw drew with, so they stay; the count comes back with --raw.
                # The vertex fetch constants stay with the register writes rather than moving up
                # with the resources. They carry a hash of what is at the address, which would be
                # worth comparing if that were the draw's vertices - but the capture hashes the
                # first 64 KB there by design, its own comment asking for "enough to tell two
                # frames' data apart", which spans memory the draw never reads. The hash therefore
                # moves between two frames of a still picture. --raw prints these as written; a
                # hash of just the extent the draw reads would be worth comparing, and would move
                # up with the textures.
                continue
            cur.append(body if self.raw else
                       (tex_identity(body) if body.startswith("tex[") else
                        re.sub(r"base=[0-9A-Fa-f]+", "base=*", body)))
        flush()

        # The resolve/copy list: an EDRAM copy is its target setup - pitch, msaa, the formats
        # and tiles it reads and writes - so that is the shape that gets compared, in order.
        for ev in self.events:
            if "edram: Copy" in ev:
                self.copy_list.append(" | ".join(
                    f for f in ev.split(" ; ")
                    if f.startswith(("edram:", "rt0:", "rt1:", "depth:", "draw:"))))

    # --- registers_start.txt -------------------------------------------------------------
    def _regs(self, lines):
        if lines is None:
            return
        for line in lines:
            m = re.match(r"([0-9A-Fa-f]{4})\s+(\S+)\s+([0-9A-Fa-f]{8})", line)
            if m:
                self.reg_start[m.group(1).upper()] = (m.group(2), m.group(3))

    def _shader_files(self):
        d = self.dir / "shaders"
        if not d.is_dir():
            self.missing.append("shaders/")
            return
        for p in sorted(d.iterdir()):
            if p.suffix == ".bin":
                self.shader_files[p.stem] = p.read_bytes()


def tex_identity(body):
    """tex[ 1] 8_8_8_8 2D 16x256 tiled pitch=32 base=0A7FC000 ... -> what it is, not where."""
    m = re.match(r"tex\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)", body)
    return "tex[%s] %s %s %s %s" % m.groups() if m else body.split("  raw")[0].rstrip()


def clip(s, n=WIDTH):
    return s if len(s) <= n else s[:n] + " ..."


def rle(items):
    """[x, x, x, y] -> [(x, 3), (y, 1)]. A frame often repeats the same draw twenty times in a
    row (one HUD element each), so a collapsed line says more in the same space."""
    out = []
    for s in items:
        if out and out[-1][0] == s:
            out[-1][1] += 1
        else:
            out.append([s, 1])
    return [(s, n) for s, n in out]

def field_diff(x, y, width=WIDTH):
    """Two draw blocks that differ, two lines per field that is not the same. The caller
    indents each line; these carry only their own - / + / -> marker."""
    fa, fb = x.split(" ; "), y.split(" ; ")
    out = []
    for i in range(max(len(fa), len(fb))):
        l = fa[i] if i < len(fa) else "<no such field in A>"
        r = fb[i] if i < len(fb) else "<no such field in B>"
        if l != r:
            if l in r or r in l:
                out.append("~ " + clip(l, width))
                out.append("  -> " + clip(r, width))
            else:
                out.append("- " + clip(l, width))
                out.append("+ " + clip(r, width))
    return out or ["same fields, different order"]


class Report:
    """Builds the output; every check says whether it moved, which makes the verdict line."""

    def __init__(self, args):
        self.args = args
        self.width = args.width
        self.out = []
        self.moved = []
        self.buf = None          # the section being built; dropped if it turns out to match
        self.mark = 0
        self.buf_keep = False

    def head(self, title):
        """Start a section, closing the previous one. A section that reports nothing is left out
        unless --all asks to see the parts of the frame that did not change."""
        self.flush()
        self.buf = ["== %s" % title]
        self.mark = len(self.moved)
        self.buf_keep = False

    def keep(self):
        """Keep the current section whatever it reports."""
        self.buf_keep = True

    def flush(self):
        if self.buf is None:
            return
        if self.buf_keep or self.args.all or len(self.moved) > self.mark:
            self.out.extend(self.buf)
        self.buf = None

    def line(self, s):
        """A count, a heading or a place: kept by --brief, which asks for the shape of the
        difference without its contents."""
        (self.buf if self.buf is not None else self.out).append("   " + s)

    def row(self, s):
        """The items themselves: --brief leaves these out."""
        if self.args.brief:
            return
        self.line(s)

    def moved_to(self, what):
        if what not in self.moved:
            self.moved.append(what)

    def same(self):
        """Reached when a section found nothing to report. Without --all the section is dropped
        from the output anyway; under --all this is what marks it as read and equal rather than
        silently absent."""
        self.row("same")

    def d(self, x, y):
        return str(x) if x == y else "%s -> %s (%+d)" % (x, y, (y or 0) - (x or 0))

    def capped(self, items, show, keep=False):
        """A capped list of rows. keep=True for rows that name a thing and count it - a pipeline
        state, a shader, a PM4 opcode - which --brief still prints; the default is content, which
        --brief leaves out."""
        for s in items[:show]:
            (self.line if keep else self.row)(s)
        if len(items) > show:
            self.line("... %d more (use --max %d)" % (len(items) - show, len(items)))

    def some(self, items, show, what):
        """Preview the first `show` items of a run, without implying --max covers them."""
        for s in items[:show]:
            self.row(s)
        if len(items) > show:
            self.row("    ... %d more %s in this run" % (len(items) - show, what))

    # --- sections ------------------------------------------------------------------------
    def header(self, a, b):
        self.head("captures")
        self.keep()
        for c in (a, b):
            self.line("%s: frame %s, front buffer %s%s" % (c.name, c.frame, c.front,
                                                          ", built from " + c.build if c.build else ""))
        if a.front != b.front:
            self.moved_to("front buffer")

    def counts(self, a, b):
        self.head("draws")
        # Counted out of draws.txt, not out of summary.txt: the summary is one file's word for
        # what happened, the draw blocks are what happened. Where the two disagree is reported
        # under "the captures against themselves".
        self.line("draws %s" % self.d(a.draw_blocks, b.draw_blocks))
        self.line("EDRAM copies/resolves %s" % self.d(len(a.copy_list), len(b.copy_list)))
        if (a.draw_blocks, len(a.copy_list)) != (b.draw_blocks, len(b.copy_list)):
            self.moved_to("draw count")

    def prim(self, a, b):
        self.head("primitive types")
        for k in sorted(set(a.prim) | set(b.prim)):
            self.line("  %-22s %s draws" % (k, self.d(a.prim.get(k, 0), b.prim.get(k, 0))))
        if a.prim != b.prim:
            self.moved_to("primitive mix")

    def setdiff(self, la, lb, title):
        self.head(title)
        sa, sb = set(la), set(lb)
        self.line("  %d in A, %d in B, %d shared" % (len(sa), len(sb), len(sa & sb)))
        gone, new = sorted(sa - sb), sorted(sb - sa)
        self.capped(["  - only A: %s" % s for s in gone], self.args.max, keep=True)
        self.capped(["  + only B: %s" % s for s in new], self.args.max, keep=True)
        if gone or new:
            self.moved_to(title)
        else:
            self.same()

    def states(self, a, b):
        self.head("pipeline states")
        shared = set(a.states) & set(b.states)
        self.line("  %d in A, %d in B, %d shared" % (len(a.states), len(b.states), len(shared)))
        gone = sorted((a.states[k], k) for k in set(a.states) - set(b.states))
        new = sorted((b.states[k], k) for k in set(b.states) - set(a.states))
        shift = sorted((a.states[k], b.states[k], k) for k in shared if a.states[k] != b.states[k])
        self.capped(["  - was %2d draws: %s" % (n, k) for n, k in gone], self.args.max, keep=True)
        self.capped(["  + now %2d draws: %s" % (n, k) for n, k in new], self.args.max, keep=True)
        self.capped(["  ~ %s draws: %s" % (self.d(x, y), k) for x, y, k in shift], self.args.max,
                    keep=True)
        if gone or new or shift:
            self.moved_to("pipeline states")
        else:
            self.same()

    def pm4(self, a, b):
        self.head("PM4 packets")
        if not a.pm4 or not b.pm4:
            self.line("  (neither capture recorded a packet histogram)")
            return
        keys = sorted(set(a.pm4) | set(b.pm4))
        changed = ["  %s %s" % (k, self.d(a.pm4.get(k, 0), b.pm4.get(k, 0)))
                   for k in keys if a.pm4.get(k, 0) != b.pm4.get(k, 0)]
        self.capped(changed, self.args.max, keep=True)
        if changed:
            self.moved_to("PM4 packets")
        else:
            self.same()

    def shaders(self, a, b):
        self.head("shaders")
        for stage, pre in (("vertex", "vs"), ("pixel", "ps")):
            ha = {k for k in a.shader_files if k.startswith(pre + "_")}
            hb = {k for k in b.shader_files if k.startswith(pre + "_")}
            self.line("  %s: %d in A, %d in B" % (stage, len(ha), len(hb)))
            gone, new = sorted(ha - hb), sorted(hb - ha)
            self.capped(["    - %s not used by B" % k for k in gone], self.args.max, keep=True)
            self.capped(["    + %s new in B" % k for k in new], self.args.max, keep=True)
            if gone or new:
                self.moved_to("shaders")
            # The file name is a hash of the microcode, so one name with two sets of bytes
            # means the hash is not covering what it should, or one capture overwrote another.
            bad = [k for k in sorted(ha & hb) if a.shader_files[k] != b.shader_files[k]]
            self.capped(["    ! %s same hash, different microcode: %d vs %d bytes"
                         % (k, len(a.shader_files[k]), len(b.shader_files[k])) for k in bad],
                        self.args.max, keep=True)
            if bad:
                self.moved_to("shader microcode")
        for stage in sorted(set(a.shader_counts) | set(b.shader_counts)):
            x, y = a.shader_counts.get(stage), b.shader_counts.get(stage)
            if x is not None and y is not None and x != y:
                self.line("  summary says %s %s shaders" % (self.d(x, y), stage))
                self.moved_to("shaders")
        # Which program pair a draw ran, counted. A pair used by 40 draws in one frame and 12 in
        # the next is 28 draws' worth of something not being drawn with it, which the state and
        # primitive counts do not say on their own.
        keys = sorted(set(a.pairs) | set(b.pairs))
        moved = [k for k in keys if a.pairs.get(k, 0) != b.pairs.get(k, 0)]
        self.line("  program pairs: %d in A, %d in B, %d used by a different number of draws"
                  % (len(a.pairs), len(b.pairs), len(moved)))
        self.capped(["    vs %s ps %s %s draws"
                     % (k.split()[0], k.split()[1],
                        self.d(a.pairs.get(k, 0), b.pairs.get(k, 0))) for k in moved],
                    self.args.max)
        if moved:
            self.moved_to("shaders")
        elif a.pairs != b.pairs:
            self.moved_to("shaders")
        # A capture whose own files disagree with its summary is a capture bug, and it is reported
        # once, under "the captures against themselves".

    def consistency(self, a, b):
        """Each capture against itself. summary.txt counts the frame; the other two files are the
        frame. When they disagree - a truncated draws.txt, a shader whose microcode never got
        written - comparing one capture against another can come out as a clean match, so the
        contradiction is the finding."""
        self.head("the captures against themselves")
        for c in (a, b):
            if c.draws is not None and c.draws != c.draw_blocks:
                self.line("  %s: summary says %d draws, draws.txt holds %d"
                         % (c.name, c.draws, c.draw_blocks))
                self.moved_to("a capture contradicts its own files")
            if c.copies is not None and c.copies != len(c.copy_list):
                self.line("  %s: summary says %d EDRAM copies/resolves, draws.txt holds %d"
                         % (c.name, c.copies, len(c.copy_list)))
                self.moved_to("a capture contradicts its own files")
            # The primitive histogram is a count of the same draw blocks, so it has to agree with
            # them type by type; a capture that lost draws from the middle would otherwise read as
            # a frame that simply drew fewer of something.
            if c.prim_claimed and c.prim_claimed != c.prim:
                for k in sorted(set(c.prim_claimed) | set(c.prim)):
                    if c.prim_claimed.get(k) != c.prim.get(k):
                        self.line("  %s: summary counts %s %s draws, draws.txt holds %s"
                                  % (c.name, c.prim_claimed.get(k, "no"), k,
                                     c.prim.get(k, "none")))
                self.moved_to("a capture contradicts its own files")
            for stage, pre in (("vertex", "vs"), ("pixel", "ps")):
                n = c.shader_counts.get(stage)
                files = len([k for k in c.shader_files if k.startswith(pre + "_")])
                if n is not None and n != files:
                    self.line("  %s: summary says %d %s shaders, shaders/ holds %d"
                             % (c.name, n, stage, files))
                    self.moved_to("a capture contradicts its own files")

    def copies(self, a, b):
        self.head("resolve / copy list")
        ca, cb = a.copy_list, b.copy_list
        self.line("  %d in A, %d in B" % (len(ca), len(cb)))
        matcher = difflib.SequenceMatcher(None, ca, cb, autojunk=False)
        for tag, i1, i2, j1, j2 in matcher.get_opcodes():
            if tag == "equal" or self.args.brief:
                continue
            self.line("  %s at A@%d (%d) / B@%d (%d)" % (tag, i1, i2 - i1, j1, j2 - j1))
            self.some(["    - %s%s" % ("x%d " % n if n > 1 else "", clip(s, self.width))
                       for s, n in rle(ca[i1:i2])], 4, "copies")
            self.some(["    + %s%s" % ("x%d " % n if n > 1 else "", clip(s, self.width))
                       for s, n in rle(cb[j1:j2])], 4, "copies")
        if ca != cb:
            self.moved_to("resolve/copy list")
        else:
            self.line("  same, in the same order")

    def registers(self, a, b):
        self.head("registers at frame start")
        if not a.reg_start or not b.reg_start:
            self.line("  (no register dump to compare)")
            return
        keys = sorted(set(a.reg_start) | set(b.reg_start))
        changed = []
        for k in keys:
            x, y = a.reg_start.get(k), b.reg_start.get(k)
            if x != y:
                changed.append("  %s %-28s %s -> %s"
                               % (k, (y or x)[0], (x or ("?", "?"))[1], (y or ("?", "?"))[1]))
        self.line("  %d read in A, %d in B, %d differ"
                 % (len(a.reg_start), len(b.reg_start), len(changed)))
        self.capped(changed, self.args.max)
        if changed:
            self.moved_to("frame-start registers")
        else:
            self.same()

    def sequence(self, a, b):
        self.head("draw sequence")
        ea, eb = a.events, b.events
        # Align on the first field of each event - "DRAW_INDX prim=quad_list count=4 ..." or
        # "load pixel shader ..." - which is what says *which* draw it is. Two runs of the same
        # scene then line up even when their state does not, and a state change shows as a state
        # change instead of a delete plus an insert.
        ka = [e.split(" ; ")[0] for e in ea]
        kb = [e.split(" ; ")[0] for e in eb]
        ops = difflib.SequenceMatcher(None, ka, kb, autojunk=False).get_opcodes()
        findings = []            # (kind, index in A, index in B, events in A, events in B)
        pairs = 0
        for tag, i1, i2, j1, j2 in ops:
            if tag == "equal":
                for k, (x, y) in enumerate(zip(ea[i1:i2], eb[j1:j2])):
                    if x != y:
                        pairs += 1
                        findings.append(("same draw, different state", i1 + k, j1 + k, [x], [y]))
                continue
            findings.append((tag, i1, j1, ea[i1:i2], eb[j1:j2]))
        identical = sum(j2 - j1 for tag, i1, i2, j1, j2 in ops if tag == "equal") - pairs
        # A run removed here and the same run inserted there is the same work happening at a
        # different point in the frame. difflib reports that as two runs; say "moved" once.
        for d in [f for f in findings if f[0] == "delete"]:
            for i in [f for f in findings if f[0] == "insert"]:
                if sorted(i[4]) == sorted(d[3]):
                    at = findings.index(d)
                    findings.remove(d)
                    findings.remove(i)
                    findings.insert(at, ("move", d[1], i[2], d[3], i[4]))
                    break
        self.line("  %d events in A, %d in B: %d identical, %d the same draw with different"
                 " state, %d runs inserted, removed, moved or changed"
                 % (len(ea), len(eb), identical, pairs,
                    len([f for f in findings if f[0] != "same draw, different state"])))
        if not findings:
            self.same()
            return
        self.moved_to("draw blocks")
        if self.args.brief:
            return
        for kind, at, bat, x, y in findings[:self.args.max]:
            if kind == "same draw, different state":
                self.line("  changed at A@%d / B@%d:" % (at, bat))
                fields = field_diff(x[0], y[0], self.width)
                # One changed draw is one finding, however many fields moved: a skinning matrix
                # set is thirty of them, and the tenth says no more than the ninth.
                for line in fields[:self.args.fields]:
                    self.row("    " + line)
                if len(fields) > self.args.fields:
                    self.row("    (... %d more fields changed on this draw; --fields N shows "
                             "more)" % (len(fields) - self.args.fields))
                continue
            if kind == "move":
                self.line("  moved: the same %d events sit at A@%d in A and B@%d in B%s"
                         % (len(x), at, bat, "" if x == y else ", in a different order"))
                self.row("    %s" % clip(x[0], self.width))
                if len(x) > 1:
                    self.row("    (... %d more, all present in the other capture at that place)"
                             % (len(x) - 1))
                continue
            what = {"replace": "changed", "insert": "only in B", "delete": "only in A"}[kind]
            self.line("  %s at A@%d (%d) / B@%d (%d)" % (what, at, len(x), bat, len(y)))
            self.some(["    - %s%s" % ("x%d " % n if n > 1 else "", clip(s, self.width))
                       for s, n in rle(x)], 4, "draws")
            self.some(["    + %s%s" % ("x%d " % n if n > 1 else "", clip(s, self.width))
                       for s, n in rle(y)], 4, "draws")
        if len(findings) > self.args.max:
            self.line("  ... %d more changed draws or runs; --max %d shows them all"
                     % (len(findings) - self.args.max, len(findings)))

    def unknown_sections(self, a, b):
        # Not "only if they differ": if either capture has a section this script never read, the
        # verdict below cannot claim to have compared them in full.
        keys = sorted(set(a.unknown) | set(b.unknown))
        if not keys:
            return
        self.head("sections this script does not read")
        for k in keys:
            self.line("  %s: %d lines in A, %d in B"
                     % (k, len(a.unknown.get(k, [])), len(b.unknown.get(k, []))))
            self.capped(["      A: %s" % clip(s, 100) for s in a.unknown.get(k, [])],
                        self.args.max)
            self.capped(["      B: %s" % clip(s, 100) for s in b.unknown.get(k, [])],
                        self.args.max)
        self.moved_to("summary sections this script does not read")


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Compare two GPU frame capture folders (captures/frame_N/).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="exit 0: the captures agree; 1: they differ; 2: one is not a readable capture.")
    ap.add_argument("a", help="the first capture folder")
    ap.add_argument("b", help="the second capture folder")
    ap.add_argument("--raw", action="store_true",
                    help="compare raw register deltas and resource base addresses too")
    ap.add_argument("--brief", action="store_true",
                    help="what moved, named and counted: no draw blocks, register values or "
                         "shader excerpts")
    ap.add_argument("--max", type=int, default=12, metavar="N",
                    help="most items to show per section (default 12)")
    ap.add_argument("--fields", type=int, default=8, metavar="N",
                    help="most changed fields to show for one draw (default 8): a skinned mesh "
                         "moves thirty matrices at once")
    ap.add_argument("--width", type=int, default=WIDTH, metavar="N",
                    help="truncate each drawn block to N characters (default %d)" % WIDTH)
    ap.add_argument("--all", action="store_true", help="print the sections that match as well")
    args = ap.parse_args(argv)

    for p in (args.a, args.b):
        if not Path(p).is_dir():
            print("not a capture folder: %s" % p, file=sys.stderr)
            return 2
    a, b = Capture(args.a, args.raw), Capture(args.b, args.raw)
    for c in (a, b):
        if c.missing:
            print("%s: not a capture folder (no %s)" % (c.dir, ", ".join(c.missing)),
                  file=sys.stderr)
            return 2

    r = Report(args)
    r.header(a, b)
    r.counts(a, b)
    r.prim(a, b)
    r.setdiff(a.rt, b.rt, "render target setups")
    r.setdiff(a.tex, b.tex, "textures seen at draws")
    r.shaders(a, b)
    r.states(a, b)
    r.pm4(a, b)
    r.copies(a, b)
    r.registers(a, b)
    r.sequence(a, b)
    r.consistency(a, b)
    r.unknown_sections(a, b)
    r.flush()

    print("\n".join(x for x in r.out if x is not None))
    print()
    if r.moved:
        print("== %s and %s differ: %s" % (a.name, b.name, ", ".join(r.moved)))
        return 1
    note = "" if args.raw else " (raw register deltas and base addresses ignored)"
    print("== %s and %s match%s" % (a.name, b.name, note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
