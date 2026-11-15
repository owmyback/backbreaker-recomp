"""Tests for tools/shader_effects.py (#80).

The pack these tests run against is written out by hand in setUpClass, and it has to be: the
real PackedOutputX360.pkh/.pkd pair is extracted game data under game/, which is gitignored and
so is not in CI. The hand-written pack keeps the parts of the real format that matter - 12-byte
index header, NUL-padded name lengths, offset/size pairs, blobs that hold several shaders
between other data, and effects that share one shader's bytes.

TestAgainstTheShippedPack at the bottom runs against the real pack when this machine has the
game files and skips otherwise, so the fixture cannot quietly drift away from what ships.
"""
import contextlib
import io
import shutil
import pathlib
import struct
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import shader_effects as se      # noqa: E402  (the tools folder is not a package)

W = 4


def shader(sid, dwords):
    """Deterministic stand-in for microcode: `dwords` dwords, unlike any other shader's."""
    return b"".join(struct.pack(">I", (k * 2654435761 + sid * 40503 + 0x9e3779b9) & 0xffffffff)
                    for k in range(dwords))


def patch(blob, first, last, sid):
    """Rewrite dwords first..last inclusive - what the engine's linker does to a vertex shader."""
    return blob[:first * W] + shader(sid, last - first + 1) + blob[(last + 1) * W:]


def write_pack(dirpath, endian="<"):
    """Write <dirpath>/fixture.{pkh,pkd}; return the capture folder that goes with them.

    Six entries, four of them effects:
        shaders\\shadow.fx360                        ps_b at dword 3
        shaders\\bbshaders\\player_highlight.fx360   ps_a at dword 0, ps_b at dword 26
        shaders\\bbshaders\\bb_cloth.fx360           a 40-dword vertex shader, dwords 10-19
                                                   rewritten by the linker
        shaders\\draw.fx360                         nothing that was ever captured
        textures\\brick.tga                         ps_c, which no effect contains
        movies\\intro.bik                           padding-sized non-effect entry
    The capture holds six shaders: ps_a (one effect), ps_b (two effects), ps_c (no effect, but
    in the pack), ps_d (nowhere in the pack), vs_e whose pack copy was relinked in the middle (so
    only its tail agrees), and vs_f whose pack copy was relinked at the end (so only its head
    agrees, and the run stops short of the end of what was captured), and vs_g, which agrees
    with its pack copy over 12 of its 30 dwords - enough for --near, not enough to name an
    effect. draw.fx360 also holds a second
    copy of vs_f relinked two dwords differently, which is the same shader for reporting
    purposes: that line has to name both effects and their agreement counts.
    """
    ps_a, ps_b, ps_c, ps_gone = shader(1, 20), shader(2, 12), shader(3, 15), shader(5, 18)
    vs_real, vs_tail, vs_stub = shader(4, 40), shader(6, 40), shader(7, 30)
    entries = [
        ("shaders\\shadow.fx360", b"\0" * 12 + ps_b + b"tail" * 8),
        ("shaders\\bbshaders\\player_highlight.fx360",
         ps_a + b"mid" * 8 + ps_b + b"\0" * 4),
        ("shaders\\bbshaders\\bb_cloth.fx360", b"head" * 3 + patch(vs_real, 10, 19, 99)
         + b"foot" * 5 + patch(vs_tail, 30, 39, 77) + b"mid" * 6),
        ("shaders\\draw.fx360", b"filler" * 24 + patch(vs_tail, 28, 39, 88) + b"end" * 4
         + patch(vs_stub, 0, 17, 77)),
        ("textures\\brick.tga", b"\xff\xd8" + ps_c + b"\0" * 16),
        ("movies\\intro.bik", b"x" * 64),
    ]

    index = struct.pack(endian + "II", len(entries),
                        sum(len(n) + (-len(n)) % 4 for n, _ in entries))
    data = bytearray()
    for name, blob in entries:
        pad = (-len(name)) % 4
        index += struct.pack(endian + "I", len(name) + pad)
        index += name.encode("latin1") + b"\0" * pad
        index += struct.pack(endian + "II", len(data), len(blob))
        data += blob
    (dirpath / "fixture.pkh").write_bytes(index)
    (dirpath / "fixture.pkd").write_bytes(bytes(data))

    shaders = dirpath / "cap" / "shaders"
    shaders.mkdir(parents=True, exist_ok=True)
    for stage, digest, blob in (("ps", "a" * 16, ps_a), ("ps", "b" * 16, ps_b),
                                ("ps", "c" * 16, ps_c), ("ps", "d" * 16, ps_gone),
                                ("vs", "e" * 16, vs_real), ("vs", "f" * 16, vs_tail), ("vs", "g" * 16, vs_stub)):
        (shaders / f"{stage}_{digest}.bin").write_bytes(blob)
    return dirpath / "cap"


class PackFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = pathlib.Path(cls.tmp.name)
        cls.cap = write_pack(cls.dir)
        cls.prefix = cls.dir / "fixture"

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def main(self, *argv, stream=None):
        """Run the tool against the fixture pack; return (exit code, what it printed)."""
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = se.main(["--pack", str(self.prefix)] + [str(a) for a in argv])
        text = out.getvalue()
        if stream is not None:
            stream.write(text)
        return code, text

    def one(self, text, prefix):
        lines = [l for l in text.splitlines() if l.startswith(prefix)]
        self.assertEqual(len(lines), 1, f"expected one {prefix} row in:\n{text}")
        return lines[0]


class TestParseIndex(PackFixture):
    def test_reads_the_layout_the_shipped_pack_uses(self):
        with se.Pack(self.prefix) as pack:
            self.assertEqual(pack.endian, "<")
            self.assertEqual([e[0] for e in pack.entries],
                             ["shaders\\shadow.fx360",
                              "shaders\\bbshaders\\player_highlight.fx360",
                              "shaders\\bbshaders\\bb_cloth.fx360",
                              "shaders\\draw.fx360",
                              "textures\\brick.tga",
                              "movies\\intro.bik"])
            for name, off, size in pack.entries:
                self.assertLess(off + size, pack.pkd_size + 1)
                self.assertEqual(len(pack.data((name, off, size))), size)

    def test_the_byte_order_is_parsed_rather_than_assumed(self):
        """The same index written big-endian has to parse too, or "<" is just a coincidence."""
        be = self.dir / "be"
        be.mkdir()
        write_pack(be, endian=">")
        with se.Pack(be / "fixture") as pack:      # a prefix path, or one with an extension
            self.assertEqual(pack.endian, ">")
            with se.Pack(self.prefix) as little:
                self.assertEqual([e[0] for e in pack.entries],
                                 [e[0] for e in little.entries])

    def test_rejects_an_entry_running_past_the_data_file(self):
        bad = self.dir / "overrun"
        bad.mkdir()
        index = struct.pack("<II", 1, 4) + struct.pack("<I", 4) + b"gone" \
            + struct.pack("<II", 0, 999999)
        (bad / "bad.pkh").write_bytes(index)
        (bad / "bad.pkd").write_bytes(b"small")
        with self.assertRaises(se.PackError) as cm:
            se.Pack(bad / "bad")
        msg = str(cm.exception)
        self.assertIn("past the 5-byte data file", msg)
        self.assertIn("little-endian", msg)
        self.assertIn("big-endian", msg)

    def test_rejects_an_index_whose_entries_end_early(self):
        bad = self.dir / "short"
        bad.mkdir()
        index = struct.pack("<II", 1, 4) + struct.pack("<I", 4) + b"gone" \
            + struct.pack("<II", 0, 5) + b"trailing junk"
        (bad / "bad.pkh").write_bytes(index)
        (bad / "bad.pkd").write_bytes(b"small")
        with self.assertRaises(se.PackError) as cm:
            se.Pack(bad / "bad")
        self.assertIn("cover 24 of 37 index bytes", str(cm.exception))

    def test_a_short_header_is_not_reported_as_zero_entries(self):
        tiny = self.dir / "tiny"
        tiny.mkdir()
        (tiny / "bad.pkh").write_bytes(b"pk")
        (tiny / "bad.pkd").write_bytes(b"")
        with self.assertRaises(se.PackError) as cm:
            se.Pack(tiny / "bad")
        self.assertIn("too short", str(cm.exception))

    def test_an_index_whose_name_total_disagrees_is_rejected(self):
        # The header's second u32 is the byte count of all the names. It is a free cross-check
        # on the layout, so a pack that disagrees is reported, not half-believed.
        pkh = (self.dir / "fixture.pkh").read_bytes()
        bad = struct.pack("<II", 6, 99999) + pkh[8:]
        size = (self.dir / "fixture.pkd").stat().st_size
        with self.assertRaises(se.PackError) as cm:
            se.parse_index(bad, size)
        self.assertIn("bytes of names", str(cm.exception))

    def test_a_pack_with_an_empty_data_file_is_a_clean_error_not_a_traceback(self):
        # one entry holding nothing, in a 0-byte data file: parseable, and --all-entries must
        # not crash trying to mmap it
        name = b"a.fx360"
        nlen = len(name) + (-len(name) % 4)
        head = struct.pack("<II", 1, nlen)
        head += struct.pack("<I", nlen) + name + b"\0" * (nlen - len(name))
        head += struct.pack("<II", 0, 0)
        (self.dir / "empty.pkh").write_bytes(head)
        (self.dir / "empty.pkd").write_bytes(b"")
        code, text = self.main(self.cap, "--pack", str(self.dir / "empty"), "--all-entries")
        self.assertEqual(code, 1)
        self.assertIn("7 shaders -> 0 exact, 0 partial, 7 not in pack", text)

    def test_a_missing_pack_says_where_the_game_files_live(self):
        with self.assertRaises(se.PackError) as cm:
            se.Pack(self.dir / "nowhere")
        self.assertIn("game/", str(cm.exception))
        self.assertIn("--pack", str(cm.exception))


class TestMatchInternals(PackFixture):
    def test_exact_hits_lists_every_entry_that_holds_the_shader(self):
        with se.Pack(self.prefix) as pack:
            self.assertEqual([h[0][0] for h in se.exact_hits(shader(1, 20), pack.effects(), pack)],
                             ["shaders\\bbshaders\\player_highlight.fx360"])
            self.assertEqual([h[1] for h in se.exact_hits(shader(2, 12), pack.effects(), pack)],
                             [12, 104])

    def test_exact_hits_sees_a_shader_outside_the_effects(self):
        with se.Pack(self.prefix) as pack:
            self.assertEqual(se.exact_hits(shader(3, 15), pack.effects(), pack), [])
            tga = [e for e in pack.entries if e[0].endswith(".tga")]
            self.assertEqual([h[0][0] for h in se.exact_hits(shader(3, 15), tga, pack)],
                             ["textures\\brick.tga"])

    def test_best_run_walks_past_the_relinked_block(self):
        with se.Pack(self.prefix) as pack:
            entry = next(e for e in pack.effects() if e[0].endswith("bb_cloth.fx360"))
            run, at_shader, at_pack = se.best_run(shader(4, 40), entry, pack)
            self.assertEqual((run, at_shader, at_pack), (20, 20, 23))

    def test_best_run_also_finds_agreement_at_the_start(self):
        with se.Pack(self.prefix) as pack:
            entry = next(e for e in pack.effects() if e[0].endswith("bb_cloth.fx360"))
            needle = patch(shader(4, 40), 20, 29, 11)   # a different block relinked this time
            run, at_shader, at_pack = se.best_run(needle, entry, pack)
            self.assertEqual((run, at_shader, at_pack), (10, 0, 3))

    def test_best_run_ignores_agreement_shorter_than_a_window(self):
        with se.Pack(self.prefix) as pack:
            entry = next(e for e in pack.effects() if e[0].endswith("shadow.fx360"))
            self.assertEqual(se.best_run(shader(77, 30), entry, pack), (0, 0, 0))


class TestMain(PackFixture):
    def test_exact_partial_and_absent_are_three_different_answers(self):
        code, text = self.main(self.cap)
        self.assertEqual(code, 1)
        self.assertIn("7 distinct shaders (4 pixel, 3 vertex) from 7 .bin files", text)
        labels = [l.split()[0] for l in text.splitlines()
                  if l.startswith(("ps_", "vs_"))]
        self.assertEqual(labels, sorted(labels))       # the default sort is by shader
        self.assertRegex(text, r"ps_aaa+\s+shaders\\bbshaders\\player_highlight\.fx360\s+"
                               r"\[exact\]\s+20 dwords at 0x0")
        self.assertIn("the same microcode in 2 entries", self.one(text, "ps_bbb"))
        self.assertIn("[not in pack]", self.one(text, "ps_ccc"))
        row = self.one(text, "vs_eee")
        self.assertIn("[partial]", row)
        self.assertIn("bb_cloth.fx360", row)
        self.assertIn("20/40 dwords agree: shader 20-39 is pack 23-42", row)
        self.assertIn("2 exact, 2 partial, 3 not in pack; 3 of 4 effects used", text)
        self.assertIn("--all-entries", text)

    def test_partial_is_explained_as_a_shortlist_not_an_answer(self):
        _, text = self.main(self.cap)
        self.assertIn("the pack's copy is not byte for byte what ran", text)
        self.assertIn("a shortlist of effects, not an answer", text)
        # reaching the end of the captured microcode is said, and only when the run does
        self.assertIn("(runs to the end of the captured shader)", self.one(text, "vs_eee"))
        self.assertNotIn("runs to the end", self.one(text, "vs_fff"))
        _, text = self.main(self.cap, "--quiet")
        self.assertNotIn("shortlist", text)      # --quiet drops the explanation, not the counts

    def test_effects_it_cannot_separate_are_listed_with_their_counts(self):
        """draw.fx360 holds the same vertex shader relinked two dwords differently.

        The 2-dword difference in agreement is about how each effect's copy was relinked, not
        about which effect drew this triangle, so naming only bb_cloth would be a guess dressed
        up as an answer. Both effects are on the line and the counts rank them.
        """
        code, text = self.main(self.cap)
        self.assertIn("1 more within 4 dwords: draw.fx360 28", self.one(text, "vs_fff"))
        self.assertNotIn("within 4 dwords", self.one(text, "vs_eee"))

    def test_agreement_long_enough_for_near_but_not_half_the_shader_is_not_an_answer(self):
        """vs_g agrees with its copy in draw.fx360 over the last 12 of its 30 dwords.

        12 clears --near 12 but is not half the shader, and naming an effect on 12 of 30 dwords
        would be a guess. The line says which of the two cuts it failed, because "below --near"
        would have been false.
        """
        code, text = self.main(self.cap)
        row = self.one(text, "vs_ggg")
        self.assertIn("[not in pack]", row)
        self.assertIn("best agreement anywhere: 12/30 dwords with shaders\\draw.fx360, "
                      "shorter than half of the 30 dwords it is", row)

    def test_all_entries_finds_the_shader_that_no_effect_contains(self):
        code, text = self.main(self.cap, "--all-entries")
        self.assertEqual(code, 1)                 # ps_d is still nowhere in the pack
        row = self.one(text, "ps_ccc")
        self.assertIn("[exact]", row)
        self.assertIn("textures\\brick.tga", row)
        self.assertIn("3 exact, 2 partial, 2 not in pack; 3 of 4 effects used, 1 outside "
                      "the effects", text)
        self.assertNotIn("rerun with --all-entries", text)

    def test_the_near_threshold_decides_whether_a_partial_is_claimed(self):
        vs = self.cap / "shaders" / f"vs_{'e' * 16}.bin"
        _, text = self.main(vs, "--near", "21")
        self.assertIn("[not in pack]", text)
        self.assertIn("best agreement anywhere: 20/40 dwords", text)
        _, text = self.main(vs, "--near", "0")
        self.assertIn("[not in pack]", text)
        self.assertNotIn("best agreement", text)
        _, text = self.main(vs, "--near", "20")
        self.assertIn("[partial]", text)

    def test_a_shader_in_several_captures_is_reported_once_with_a_count(self):
        again = self.dir / "cap2" / "shaders"
        again.mkdir(parents=True)
        src = self.cap / "shaders" / f"ps_{'a' * 16}.bin"
        (again / src.name).write_bytes(src.read_bytes())
        _, text = self.main(self.cap, again)
        self.assertIn("(in 2 captures)", self.one(text, "ps_aaa"))
        self.assertIn("7 distinct shaders", text)

    def test_one_bin_file_works_without_a_capture_folder(self):
        code, text = self.main(self.cap / "shaders" / f"ps_{'a' * 16}.bin")
        self.assertEqual(code, 0)
        self.assertIn("[exact]", text)
        self.assertNotIn("(in ", text)

    def test_by_effect_sorts_by_effect_and_leaves_the_failures_last(self):
        _, text = self.main(self.cap, "--by-effect")
        names = [l.split("  ")[1] for l in text.splitlines()
                 if "[exact]" in l or "[partial]" in l or "[not in pack]" in l]
        self.assertEqual([n for n in names if n != "-"], sorted(n for n in names if n != "-"))
        self.assertEqual(names[-2:], ["-", "-"])

    def test_quiet_prints_only_the_summary(self):
        _, text = self.main(self.cap, "--quiet")
        self.assertTrue(text.startswith("\nsummary:"), text)
        self.assertNotIn("ps_aaa", text)

    def test_extract_writes_the_effects_under_the_names_the_game_uses(self):
        outdir = self.dir / "unpacked"
        code, _ = self.main("--extract", str(outdir))
        self.assertEqual(code, 0)
        got = sorted(p.relative_to(outdir).as_posix() for p in outdir.rglob("*") if p.is_file())
        self.assertEqual(got, ["shaders/bbshaders/bb_cloth.fx360",
                               "shaders/bbshaders/player_highlight.fx360",
                               "shaders/draw.fx360", "shaders/shadow.fx360"])
        with se.Pack(self.prefix) as pack:
            entry = next(e for e in pack.entries if e[0].endswith("shadow.fx360"))
            self.assertEqual((outdir / "shaders/shadow.fx360").read_bytes(), pack.data(entry))

    def test_extract_with_captures_written_out_only_writes_the_effects_they_used(self):
        outdir = self.dir / "used"
        self.main(self.cap, "--extract", str(outdir))
        got = sorted(p.relative_to(outdir).as_posix() for p in outdir.rglob("*") if p.is_file())
        self.assertEqual(got, ["shaders/bbshaders/bb_cloth.fx360",
                               "shaders/bbshaders/player_highlight.fx360",
                               "shaders/shadow.fx360"])

    def quiet_main(self, *argv, err):
        """main() with both streams captured, for the paths that report failure."""
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            return se.main(["--pack", str(self.prefix)] + [str(a) for a in argv])

    def test_a_folder_with_no_shaders_exits_two(self):
        empty = self.dir / "empty"
        empty.mkdir()
        err = io.StringIO()
        code = self.quiet_main(empty, err=err)
        self.assertEqual(code, 2)
        self.assertIn("no shader .bin files", err.getvalue())

    def test_a_file_that_is_not_a_shader_dump_exits_two(self):
        err = io.StringIO()
        code = self.quiet_main(self.prefix.with_suffix(".pkd"), err=err)
        self.assertEqual(code, 2)
        self.assertIn("not named like", err.getvalue())

    def test_a_shader_whose_length_is_not_whole_dwords_exits_two(self):
        torn = self.dir / "cap" / "shaders" / f"ps_{'f' * 16}.bin"
        torn.write_bytes(b"\0" * 11)
        try:
            err = io.StringIO()
            code = self.quiet_main(torn, err=err)
            self.assertEqual(code, 2)
            self.assertIn("not a whole number of dwords", err.getvalue())
        finally:
            torn.unlink()

    def test_a_missing_pack_exits_two_rather_than_traceback(self):
        err = io.StringIO()
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = se.main(["--pack", str(self.dir / "nothing"), str(self.cap)])
        self.assertEqual(code, 2)
        self.assertIn("no pack at", err.getvalue())


REAL = pathlib.Path(se.DEFAULT_PACK)
CAPTURES = ROOT / "captures"


class TestCollect(PackFixture):
    """How capture folders and .bin files turn into the set of shaders to look up.

    Everything here is built by write_pack() above: capture input handling has nothing to do
    with which pack the shader bytes are looked up in, so these run in CI, where there are no
    game files.
    """

    def test_a_capture_folder_groups_the_shaders_by_hash(self):
        got = se.collect_shaders([self.cap])
        self.assertEqual(sorted(got), [("ps", "a" * 16), ("ps", "b" * 16), ("ps", "c" * 16),
                                       ("ps", "d" * 16), ("vs", "e" * 16), ("vs", "f" * 16), ("vs", "g" * 16)])
        # the bytes come back with the name, because the caller needs them anyway
        self.assertEqual(got[("ps", "a" * 16)][2],
                         (pathlib.Path(self.cap) / "shaders" / "ps_aaaaaaaaaaaaaaaa.bin"
                          ).read_bytes())

    def test_the_same_shader_in_two_captures_is_counted_once_with_a_total(self):
        again = self.dir / "again" / "shaders"
        again.mkdir(parents=True)
        for name in ("ps_aaaaaaaaaaaaaaaa.bin", "ps_bbbbbbbbbbbbbbbb.bin",
                     "vs_eeeeeeeeeeeeeeee.bin", "vs_ffffffffffffffff.bin"):
            shutil.copy2(str(pathlib.Path(self.cap) / "shaders" / name), str(again / name))
        got = se.collect_shaders([self.cap, str(self.dir / "again")])
        self.assertEqual(len(got), 7)
        self.assertEqual({k: v[1] for k, v in got.items()},
                         {("ps", "a" * 16): 2, ("ps", "b" * 16): 2, ("ps", "c" * 16): 1,
                          ("ps", "d" * 16): 1, ("vs", "e" * 16): 2, ("vs", "f" * 16): 2,
                          ("vs", "g" * 16): 1})

    def test_a_capture_folder_and_one_of_its_own_files_are_one_shader(self):
        extra = str(pathlib.Path(self.cap) / "shaders" / "ps_cccccccccccccccc.bin")
        self.assertEqual(len(se.collect_shaders([self.cap, extra])), 7)

    def test_a_shaders_folder_with_no_frame_folder_around_it_works(self):
        # that is where --dump-shaders writes, and it is also what a shader dump handed over
        # from another tool looks like
        got = se.collect_shaders([str(pathlib.Path(self.cap) / "shaders")])
        self.assertEqual(len(got), 7)

    def test_a_folder_nested_under_the_named_folder_is_still_found(self):
        deep = self.dir / "nested" / "inner" / "shaders"
        deep.mkdir(parents=True)
        shutil.copy2(str(pathlib.Path(self.cap) / "shaders" / "ps_aaaaaaaaaaaaaaaa.bin"),
                     str(deep / "ps_aaaaaaaaaaaaaaaa.bin"))
        got = se.collect_shaders([str(self.dir / "nested")])
        self.assertEqual(list(got), [("ps", "a" * 16)])

    def test_bin_files_that_are_not_named_like_shaders_say_so(self):
        bad = self.dir / "junk"
        bad.mkdir(exist_ok=True)
        (bad / "mystery.bin").write_bytes(b"\1" * 8)
        with self.assertRaises(se.InputError) as cm:
            se.collect_shaders([str(bad)])
        self.assertIn("none named like", str(cm.exception))

    def test_the_same_hash_with_different_bytes_is_an_error(self):
        # The filename hash is the whole premise of the tool: two files claiming the same
        # shader have to be the same microcode, or the captures came from different builds.
        conflict = self.dir / "conflict" / "shaders"
        conflict.mkdir(parents=True)
        src = pathlib.Path(self.cap) / "shaders" / "ps_aaaaaaaaaaaaaaaa.bin"
        (conflict / "ps_aaaaaaaaaaaaaaaa.bin").write_bytes(src.read_bytes() + b"\0" * 4)
        try:
            se.collect_shaders([self.cap, str(conflict)])
            self.fail("expected an InputError")
        except se.InputError as e:
            self.assertIn("same shader hash", str(e))
            self.assertIn("different builds", str(e))


class TestAgainstTheShippedPack(unittest.TestCase):
    """The pairs issue #80 states, against the pack it stated them from.

    Both inputs are gitignored game data: the pack under game/, and the captures themselves
    (they hold shader microcode). So this class skips in CI - which is the whole reason the
    fixture above exists - and checks for drift on a machine that has them.
    """

    pairs = [("ps_4834dce2860f4cb2", "player_highlight"),
             ("ps_c537b824205252a3", "player_highlight"),
             ("ps_cd070e1223efbdeb", "player_highlight"),
             ("ps_a67db8f4e10d0451", "3d_primitives"),
             ("ps_cdcb2d05fe492215", "3d_primitives"),
             ("ps_52ca8e59fd924d7d", "shadow")]

    @classmethod
    def setUpClass(cls):
        if not REAL.with_suffix(".pkh").is_file():
            raise unittest.SkipTest("needs the extracted game files under game/ (gitignored)")
        cls.bins = sorted(CAPTURES.glob("*/shaders/*.bin"))
        if len(cls.bins) < 1000:
            raise unittest.SkipTest(f"needs the captures: {len(cls.bins)} .bin files under "
                                    "captures/*/shaders/ here, fewer than the 1000 these "
                                    "thresholds assume (captures/ is gitignored)")
        # A capture set from another session may not contain every documented shader; that is
        # not a failure, but checking nothing would be.
        cls.found = {d: (sorted(CAPTURES.glob(f"*/shaders/{d}.bin"))[0], e)
                     for d, e in cls.pairs if list(CAPTURES.glob(f"*/shaders/{d}.bin"))}
        if not cls.found:
            raise unittest.SkipTest("none of the documented shaders are in these captures")

    def test_the_documented_shader_to_effect_pairs(self):
        self.assertGreater(len(self.found), 0)
        for digest, (binpath, effect) in self.found.items():
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = se.main([str(binpath), "--pack", str(REAL)])
            with self.subTest(shader=digest):
                self.assertEqual(code, 0)
                row = out.getvalue()
                self.assertIn("[exact]", row)
                self.assertIn(effect, row)

    def test_pixel_shaders_match_verbatim_where_vertex_shaders_only_overlap(self):
        with se.Pack(REAL) as pack:
            self.assertEqual(pack.endian, "<")
            fx = pack.effects()
            exact = partial = 0
            for path in self.bins:
                blob = path.read_bytes()
                if se.exact_hits(blob, fx, pack):
                    exact += 1
                elif path.stem.startswith("vs") and any(
                        se.best_run(blob, e, pack)[0] >= se.DEFAULT_NEAR for e in fx):
                    partial += 1
            self.assertGreater(exact, 1000)
            self.assertGreater(partial, 100)
            self.assertLess(partial, exact)


if __name__ == "__main__":
    unittest.main()
