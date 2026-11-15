#!/usr/bin/env python3
"""Self-tests for tools/capture_diff.py (issue #79).

    python3 -m unittest discover -s tests -p 'test_*.py'

The point of these is not the formatting: it is that the rules the tool claims are the rules it
applies. Each pair under tests/data is built so that one rule is the only thing that can fire -
capture_a against capture_c differs in nothing but a raw register delta and a texture's base
address, so those two runs pin down both directions of the "ignored by default" claim: exit 0
without --raw, exit 1 with it. A difference in the *fixture* would be invisible here, which is
why the fixtures are checked against the tool rather than eyeballed.
"""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "capture_diff.py"
DATA = ROOT / "tests" / "data"


def run(*args):
    """Run the tool as a user does, so the exit status is the thing under test."""
    return subprocess.run([sys.executable, str(TOOL)] + [str(a) for a in args],
                          capture_output=True, text=True)


def verdict(out):
    return out.strip().splitlines()[-1]


def categories(out):
    return [c.strip() for c in verdict(out).split("differ: ", 1)[1].split(",")]


def sections(out):
    """The section headings in the report - which is how "only the parts that moved get printed"
    gets checked."""
    return [l for l in out.splitlines()
            if l.startswith("== ") and " match" not in l and " differ" not in l]


def fixture(name):
    return DATA / name


class CapturePairTest(unittest.TestCase):
    """The three fixture folders: a and b differ everywhere; a and c differ only in things the
    tool is supposed to ignore."""

    def test_a_folder_compared_with_itself_matches(self):
        for name in ("capture_a", "capture_c"):
            r = run(fixture(name), fixture(name))
            self.assertEqual(0, r.returncode, r.stdout + r.stderr)
            self.assertIn("match", verdict(r.stdout))
            # Nothing else printed but the header, because a section that reports nothing is
            # left out: this is what makes a 900-draw frame's report readable.
            self.assertEqual(["== captures"], sections(r.stdout))

    def test_a_capture_contradicting_itself_never_reports_a_match(self):
        """capture_b's shader list is wrong on purpose: it claims four pixel shaders and ships
        three. A frame that lost its microcode is not evidence of anything, so even a capture
        against itself has to fail."""
        r = run(fixture("capture_b"), fixture("capture_b"))
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertEqual(["a capture contradicts its own files"], categories(r.stdout))
        self.assertIn("capture_b: summary says 4 pixel shaders, shaders/ holds 3", r.stdout)

    def test_a_against_b_flags_every_kind_of_difference(self):
        r = run(fixture("capture_a"), fixture("capture_b"))
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertEqual(
            ["draw count", "primitive mix", "render target setups", "textures seen at draws",
             "shaders", "shader microcode", "pipeline states", "PM4 packets",
             "resolve/copy list", "frame-start registers", "draw blocks",
             "a capture contradicts its own files"],
            categories(r.stdout))

    def test_raw_deltas_and_base_addresses_are_ignored_by_default_and_compared_with_raw(self):
        # capture_c is capture_a with two raw register deltas and one texture base address
        # changed, and nothing else.
        default = run(fixture("capture_a"), fixture("capture_c"))
        self.assertEqual(0, default.returncode, default.stdout + default.stderr)
        self.assertIn("raw register deltas and base addresses ignored", verdict(default.stdout))

        raw = run("--raw", fixture("capture_a"), fixture("capture_c"))
        self.assertEqual(1, raw.returncode, raw.stdout + raw.stderr)
        self.assertEqual(["draw blocks"], categories(raw.stdout))
        self.assertIn("PA_SC_WINDOW_SCISSOR_TL", raw.stdout)
        self.assertIn("COHER_BASE_HOST", raw.stdout)
        self.assertIn("base=", raw.stdout)
        # ... and only that: everything the fixtures share stays out of the report.
        self.assertEqual(["== captures", "== draw sequence"], sections(raw.stdout))

    def test_a_changed_draw_shows_the_field_that_changed_not_the_whole_block(self):
        r = run(fixture("capture_a"), fixture("capture_b"))
        self.assertIn("- tex[0] 8_8_8_8 2D 256x256 tiled", r.stdout)
        self.assertIn("+ tex[0] 8_8_8_8 2D 512x512 tiled", r.stdout)
        self.assertIn("- depth: D24S8 @tile 160 test=le write=1", r.stdout)
        self.assertIn("+ depth: D24S8 @tile 160 test=off write=0", r.stdout)
        # A uniform, printed by the capture among the raw register deltas but not one: the thing
        # that moved here is where the quad's geometry was mapped.
        self.assertIn("- vs c4 = (640, -360, 1, 0)", r.stdout)
        self.assertIn("+ vs c4 = (640, -355, 1, 0)", r.stdout)
        # Not a whole-block delete/insert pair for the same draw.
        self.assertIn("the same draw with different state", r.stdout)


class ModifiedCopyTest(unittest.TestCase):
    """Changes made to a copy of capture_a, which is how a real frame gets compared against a
    near-identical one."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = fixture("capture_a")

    def copy(self, name="frame"):
        dst = Path(self.tmp.name) / name
        shutil.copytree(self.base, dst)
        return dst

    def draws(self, folder, text):
        (folder / "draws.txt").write_text(text)

    def blocks(self, text):
        """What comes before the first draw (the shader loads), then the draw blocks in order,
        each starting at its "draw N" line."""
        import re
        parts = re.split(r"(?m)^(?=draw \d+ )", text)
        return parts[0], [p for p in parts[1:] if p]

    def renumber(self, blocks):
        import re
        return "".join(re.sub(r"(?m)^draw \d+ ", "draw %d " % i, b, count=1)
                       for i, b in enumerate(blocks, 1))

    def test_renumbering_the_draws_is_not_a_difference(self):
        """Adding a draw earlier in the frame renumbers every draw after it. Dropping the ordinal
        is what stops that reading as the rest of the frame having changed."""
        shifted = self.copy("shifted")
        text = (self.base / "draws.txt").read_text()
        import re
        self.draws(shifted, re.sub(r"^draw (\d+) ", lambda m: "draw %d " % (int(m.group(1)) + 100),
                                   text, flags=re.M))
        r = run(self.base, shifted)
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)

    def test_one_inserted_draw_is_one_insertion(self):
        """A frame that gained one draw must read as one insertion, not as everything after it
        having changed - which is the failure mode a plain text diff has on 900 draws."""
        grown = self.copy("grown")
        head, ds = self.blocks((self.base / "draws.txt").read_text())
        self.assertEqual(5, len(ds), "the fixture has five draws")
        ds.insert(3, ds[2])                   # a second copy of the third draw
        self.draws(grown, head + self.renumber(ds))
        # ... and summary.txt's counts move with it, the way a real capture's would: it counts the
        # same draw blocks twice, once as a total and once per primitive type.
        summary = (grown / "summary.txt").read_text()
        (grown / "summary.txt").write_text(
            summary.replace("draws: 5", "draws: 6", 1)
            .replace("\n  quad_list                2\n", "\n  quad_list                3\n"))
        r = run(self.base, grown)
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        # The duplicated draw's program pair really does run one more draw and the primitive mix
        # really did change: both are true about the frame, not noise from the insertion.
        self.assertEqual(["draw count", "primitive mix", "shaders", "draw blocks"],
                         categories(r.stdout))
        self.assertIn("only in B at", r.stdout)
        self.assertIn("draws 5 -> 6", r.stdout)
        # Everything that did not move stayed put: one run in the report, and no draw read as
        # changed when it was the same draw in the same state.
        self.assertIn("1 runs inserted, removed, moved or changed", r.stdout)
        self.assertNotIn("changed at", r.stdout)

    def test_a_capture_that_lost_the_tail_of_its_draws_is_not_a_match(self):
        """A draws.txt that stops short of summary.txt's count - a run killed mid-capture - has to
        show up even against an identical copy of itself."""
        short = self.copy("short")
        head, ds = self.blocks((self.base / "draws.txt").read_text())
        self.draws(short, head + "".join(ds[:-1]))
        r = run(self.base, short)
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertIn("a capture contradicts its own files", categories(r.stdout))
        self.assertIn("short: summary says 5 draws, draws.txt holds 4", r.stdout)
        self.assertEqual(1, run(short, short).returncode)

    def test_a_changed_shader_constant_is_a_difference_even_without_raw(self):
        """The uniforms are not registers, so --raw has nothing to do with whether they count. A
        quad drawn at a different place is a different frame, and a tool that only looked at the
        decoded state would call these two the same draw in the same state."""
        moved = self.copy("moved")
        text = (self.base / "draws.txt").read_text()
        self.assertIn("vs c4 = (640, -360, 1, 0)", text, "the fixture draws with a uniform")
        self.draws(moved, text.replace("vs c4 = (640, -360, 1, 0)", "vs c4 = (640, -350, 1, 0)"))
        r = run(self.base, moved)
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertEqual(["draw blocks"], categories(r.stdout))
        self.assertIn("vs c4 = (640, -360, 1, 0)", r.stdout)
        self.assertIn("vs c4 = (640, -350, 1, 0)", r.stdout)

    def test_the_count_of_changed_constants_is_not_a_difference_in_the_draw(self):
        """The capture writes how many constant dwords changed since the draw before this one, and
        updates what it compares against as the frame goes. So that count moves when the previous
        draw moves, while this draw's constants sit still - and in captures from before the
        constants were printed it can name 271 changed dwords with not one constant listed. It goes
        with the register deltas, and --raw brings it back."""
        bumped = self.copy("bumped")
        text = (self.base / "draws.txt").read_text()
        self.assertIn("(2 ALU constant dwords changed)", text, "the fixture records the count")
        self.draws(bumped, text.replace("(2 ALU constant dwords changed)",
                                        "(9 ALU constant dwords changed)"))
        quiet = run(self.base, bumped)
        self.assertEqual(0, quiet.returncode, quiet.stdout + quiet.stderr)
        loud = run("--raw", self.base, bumped)
        self.assertEqual(1, loud.returncode, loud.stdout + loud.stderr)
        self.assertIn("9 ALU constant dwords changed", loud.stdout)

    def test_a_draw_whose_many_fields_moved_reports_a_field_at_a_time(self):
        """One changed draw is one finding, and its fields are its detail: a skinned mesh moves
        thirty matrices at once, so the report shows a few and says how many more there were."""
        moved = self.copy("moved")
        head, ds = self.blocks((self.base / "draws.txt").read_text())
        target = ds[1]
        self.assertIn("vs c4 = ", target, "the fixture's second draw carries a uniform")
        target = (target.replace("vs c4 = (640, -360, 1, 0)", "vs c4 = (640, -350, 1, 0)")
                        .replace("depth: D24S8 @tile 160 test=le write=1",
                                 "depth: D24S8 @tile 160 test=never write=0")
                        .replace("scissor: window (0,0)-(640,360)",
                                 "scissor: window (0,0)-(640,300)"))
        ds[1] = target
        self.draws(moved, head + "".join(ds))
        many = run(self.base, moved)
        few = run("--fields", "2", self.base, moved)
        self.assertEqual(1, many.returncode, many.stdout + many.stderr)
        self.assertEqual(verdict(many.stdout), verdict(few.stdout))
        self.assertIn("changed at", many.stdout)
        self.assertIn("more fields changed on this draw; --fields N shows more", few.stdout)
        self.assertLess(len(few.stdout.splitlines()), len(many.stdout.splitlines()))

    def test_the_packet_histogram_is_compared_opcode_by_opcode(self):
        """summary.txt lists the type-3 opcodes the frame used with a count each. One opcode
        changing count is a different amount of command-buffer work, which is worth naming; the
        capture writes no separate packet log, so this histogram is the only packet record."""
        more = self.copy("more")
        text = (self.base / "summary.txt").read_text()
        self.assertIn("\n  0x22    3\n", text, "the fixture records a packet histogram")
        (more / "summary.txt").write_text(text.replace("\n  0x22    3\n", "\n  0x22    9\n"))
        r = run(self.base, more)
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertEqual(["PM4 packets"], categories(r.stdout))
        self.assertIn("0x22 3 -> 9 (+6)", r.stdout)

    def test_vertex_fetch_constants_wait_for_a_hash_of_the_right_extent(self):
        """The capture hashes the vertex data behind each fetch constant, which would be worth
        comparing - but it hashes the first 64 KB at that address, its own comment asking for
        enough to tell two frames apart, which is more than the draw reads and moves between two
        frames of a still picture. So it stays with the raw register deltas: ignored by default,
        shown as written by --raw, rather than passed off as the geometry."""
        moved = self.copy("moved")
        text = (self.base / "draws.txt").read_text()
        self.assertIn("vtx[ 0] base=00105B00 dwords=6 endian=2 data=aaf1eddd5017e1bc", text,
                      "the fixture records a vertex fetch constant")
        self.draws(moved, text.replace("data=aaf1eddd5017e1bc", "data=deadbeefdeadbeef"))
        quiet = run(self.base, moved)
        self.assertEqual(0, quiet.returncode, quiet.stdout + quiet.stderr)
        loud = run("--raw", self.base, moved)
        self.assertEqual(1, loud.returncode, loud.stdout + loud.stderr)
        self.assertIn("data=deadbeefdeadbeef", loud.stdout)

    def test_a_program_pair_used_by_a_different_number_of_draws_is_reported(self):
        """The pair of programs a draw runs is counted across the frame. When a draw switches to a
        program the frame already loads, the sets of shaders do not change at all, so only the
        count says which program lost draws to which."""
        other = self.copy("other")
        text = (self.base / "draws.txt").read_text()
        head = text.split("\n\n")[2]              # draw 3, the second of the two that share a pair
        self.assertIn("vs=1111111111111111 ps=2222222222222222", head,
                      "draw 3 runs the pair this test moves")
        self.draws(other, text.replace(head, head.replace("ps=2222222222222222",
                                                          "ps=4444444444444444")))
        r = run(self.base, other)
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertIn("shaders", categories(r.stdout))
        self.assertIn("vs 1111111111111111 ps 4444444444444444 1 -> 2 (+1) draws", r.stdout)
        self.assertIn("vs 1111111111111111 ps 2222222222222222 3 -> 2 (-1) draws", r.stdout)

    def test_a_summary_that_counts_the_same_draws_twice_and_disagrees_is_a_contradiction(self):
        """summary.txt counts the draw blocks once as a total and once per primitive type. When
        the two disagree, part of the frame was lost or rewritten, and nothing further in the
        comparison can be trusted - so the disagreement is the finding, not a smaller frame."""
        doctored = self.copy("doctored")
        text = (self.base / "summary.txt").read_text()
        self.assertIn("\n  quad_list                2\n", text,
                      "the fixture counts its draws twice, once per type")
        (doctored / "summary.txt").write_text(
            text.replace("\n  quad_list                2\n", "\n  quad_list                5\n"))
        r = run(self.base, doctored)
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertIn("summary counts 5 quad_list draws, draws.txt holds 2", r.stdout)
        self.assertEqual(["a capture contradicts its own files"], categories(r.stdout))

    def test_a_section_this_script_does_not_read_is_reported_not_skipped(self):
        """capture.cpp adding a section to summary.txt must not quietly become an unchecked
        difference."""
        grown = self.copy("grown")
        (grown / "summary.txt").write_text(
            (grown / "summary.txt").read_text() +
            "\ndraws skipped by the front end:\n  12  back-face culled\n")
        r = run(self.base, grown)
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertIn("summary sections this script does not read", categories(r.stdout))
        self.assertIn("draws skipped by the front end", r.stdout)
        # even against itself: an unchecked section means a match cannot be claimed
        self.assertEqual(1, run(grown, grown).returncode)

    def test_the_build_a_capture_came_from_is_shown_and_is_not_a_difference(self):
        """Since #75 summary.txt says which binary made the capture. The same frame from two
        builds is the bisect case, so the build line is shown, never counted."""
        def stamped(name, rev):
            c = self.copy(name)
            lines = (c / "summary.txt").read_text().splitlines(keepends=True)
            lines.insert(1, "built from %s (main, built 2026-11-15)\n" % rev)
            (c / "summary.txt").write_text("".join(lines))
            return c
        old, new = stamped("old", "0b48d7c"), stamped("new", "a9d670a-dirty")
        self.assertEqual(0, run(old, old).returncode)
        r = run(old, new)
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)
        self.assertIn("built from 0b48d7c (main, built 2026-11-15)", r.stdout)
        self.assertIn("built from a9d670a-dirty (main, built 2026-11-15)", r.stdout)
        self.assertNotIn("does not read", r.stdout)
        self.assertEqual(0, run(self.base, new).returncode)   # one capture from before #75

    def test_a_capture_whose_draws_file_holds_no_draws_is_not_a_match(self):
        """The most truncated draws.txt there is - killed before the first block - is the case the
        count check exists for."""
        empty = self.copy("empty")
        head, _ = self.blocks((self.base / "draws.txt").read_text())
        self.draws(empty, head)
        r = run(empty, empty)
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertIn("empty: summary says 5 draws, draws.txt holds 0", r.stdout)

    def test_microcode_behind_a_shader_name_which_does_not_match_its_hash(self):
        """A shader file whose contents are not what its name hashes to means the capture is
        lying, so a match on the name alone means nothing."""
        grown = self.copy("grown")
        # ps_4444 keeps its name but gets somebody else's microcode.
        shutil.copy(grown / "shaders" / "ps_2222222222222222.bin",
                    grown / "shaders" / "ps_4444444444444444.bin")
        r = run(self.base, grown)
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertIn("shader microcode", categories(r.stdout))
        self.assertIn("ps_4444444444444444", r.stdout)
        self.assertIn("same hash, different microcode", r.stdout)


class UsageTest(unittest.TestCase):
    def test_a_folder_which_is_not_a_capture(self):
        r = run(ROOT / "tools", ROOT / "docs")
        self.assertEqual(2, r.returncode)
        self.assertIn("not a capture folder", r.stderr)

    def test_a_folder_which_does_not_exist(self):
        r = run("/nonexistent/frame_1", fixture("capture_a"))
        self.assertEqual(2, r.returncode)
        self.assertIn("/nonexistent/frame_1", r.stderr)

    def test_a_capture_missing_its_files_names_what_is_missing(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        empty = Path(tmp.name) / "frame_9"
        empty.mkdir()
        r = run(empty, fixture("capture_a"))
        self.assertEqual(2, r.returncode)
        self.assertIn("summary.txt", r.stderr)

    def test_brief_names_what_moved_without_the_draw_blocks(self):
        full = run(fixture("capture_a"), fixture("capture_b"))
        brief = run("--brief", fixture("capture_a"), fixture("capture_b"))
        self.assertEqual(full.returncode, brief.returncode)
        self.assertEqual(verdict(full.stdout), verdict(brief.stdout))
        self.assertLess(len(brief.stdout.splitlines()), len(full.stdout.splitlines()))
        # The contents of the frame are out ...
        for content in ("DRAW_INDX", "load pixel shader", "depth: D24S8", "raw register"):
            self.assertNotIn(content, brief.stdout)
        # ... and what moved is still named, so the report is actionable on its own.
        self.assertIn("ps_3333333333333333 new in B", brief.stdout)
        self.assertIn("summary says 4 pixel shaders, shaders/ holds 3", brief.stdout)
        self.assertIn("point_list             1 -> 0", brief.stdout)

    def test_max_limits_the_report_without_changing_the_verdict(self):
        few = run("--max", "1", fixture("capture_a"), fixture("capture_b"))
        many = run("--max", "50", fixture("capture_a"), fixture("capture_b"))
        self.assertEqual(verdict(few.stdout), verdict(many.stdout))
        self.assertIn("more (use --max", few.stdout)
        self.assertLess(len(few.stdout.splitlines()), len(many.stdout.splitlines()))

    def test_all_prints_the_sections_that_match(self):
        default = run(fixture("capture_a"), fixture("capture_c"))
        everything = run("--all", fixture("capture_a"), fixture("capture_c"))
        self.assertIn("== render target setups", everything.stdout)
        self.assertNotIn("== render target setups", default.stdout)
        # Under --all a section that matched is printed and says it matched, which is the only
        # way to tell "checked and equal" from "never looked at".
        self.assertIn("same", everything.stdout)
        self.assertEqual(default.returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
