#!/usr/bin/env python3
"""Tests for tools/pkd.py (#169) against an archive made up here: no game files.

Covers the index (written and read back in both byte orders), list patterns, extract (and what
it keeps), the refusal to write the game's files where git would pick them up, repack (an
unchanged archive comes back byte for byte), and mods shared as changes: diff and apply with CRLF
files, a last line without a line end, names with spaces, a patch that lost its CRs, a patch from
GNU diff, stacking on a file already in the mods folder, and the cases where nothing is written.
"""

import contextlib
import hashlib
import io
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import pkd  # noqa: E402  (the tools folder is not a package)

PLAY = b'<?xml version="1.0"?>\r\n<Play name="Dagger">\r\n\t<Team>\r\n' + b"".join(
    b'\t\t<Player id="0" role="wr%d">\r\n\t\t\t<Position>%d,1</Position>\r\n\t\t</Player>\r\n' % (i, i)
    for i in range(12)) + b"\t</Team>\r\n</Play>\r\n"
BOOK = b'<PDPlayDatabase name="Offense">\n  <PDPlay>\n    <File>Offense\\Dagger.pd</File>\n  </PDPlay>\n</PDPlayDatabase>'
TEXTURE = bytes(range(256)) * 9
ENTRIES = [
    ("media\\PlayDatabase\\Offense\\Dagger.pd", PLAY),
    ("media\\PlayDatabase\\Offensive_Plays.xml", BOOK),             # no line end at the end
    ("media\\ui\\Final_XUI\\Single Player\\Title.xui", b"<scene>\n<Id>Title</Id>\n</scene>\n"),
    ("media\\images\\field.dds", TEXTURE),
    ("media\\empty.log", b""),
]


def make_pack(folder, entries=ENTRIES, endian="<"):
    """An archive laid out as the shipped one: 2048-byte boundaries, the data padded to 2048."""
    folder = pathlib.Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    data, index = bytearray(), []
    for name, blob in entries:
        index.append((name, len(data), len(blob)))
        data += blob
        data += b"\0" * (-len(data) % pkd.ALIGN if blob else pkd.ALIGN)  # as the shipped one: an empty entry takes a block
    prefix = folder / "PackedOutputX360"
    (folder / "PackedOutputX360.pkh").write_bytes(pkd.build_index(index, endian))
    (folder / "PackedOutputX360.pkd").write_bytes(bytes(data))
    return prefix


def run(*argv):
    """pkd.main's exit code, with what it printed."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = pkd.main([str(a) for a in argv])
    return code, out.getvalue(), err.getvalue()


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self.prefix = make_pack(self.root / "disc" / "packedfilesx360")
        self.mods = self.root / "mods"

    def tearDown(self):
        self.tmp.cleanup()

    def pkd(self, *argv):
        return run("--pack", self.prefix, *argv)

    def edit(self, name, blob):
        path = self.mods / pkd.rel_path(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
        return path


class IndexTests(Base):
    def test_round_trip_both_byte_orders(self):
        entries = [("a\\b.xml", 0, 5), ("c.pd", 2048, 0)]
        for endian in "<>":
            parsed, found = pkd.parse_index(pkd.build_index(entries, endian), 4096)
            self.assertEqual(parsed, entries)
            self.assertEqual(found, endian)

    def test_names_that_leave_the_folder_are_refused(self):
        for bad in ("..\\x", "a\\..\\..\\x", "\\abs", "a\\\\b", ""):
            with self.assertRaises(pkd.PackError):
                pkd.rel_path(bad)
        self.assertEqual(str(pkd.rel_path("media\\ui\\Single Player\\x.xui")), "media/ui/Single Player/x.xui")


class ListExtractTests(Base):
    def test_list_patterns_any_case_and_separator(self):
        code, out, _ = self.pkd("list", "MEDIA/playdatabase/*")
        self.assertEqual(code, 0)
        self.assertEqual([l.split(None, 1)[1] for l in out.splitlines()],
                         ["media\\PlayDatabase\\Offense\\Dagger.pd", "media\\PlayDatabase\\Offensive_Plays.xml"])
        code, out, _ = self.pkd("list", "-l")
        self.assertEqual(len(out.splitlines()), len(ENTRIES))
        self.assertTrue(out.splitlines()[1].split()[0] == "2048")

    def test_extract_writes_the_entries(self):
        out = self.root / "out"
        code, _, _ = self.pkd("extract", out)
        self.assertEqual(code, 0)
        for name, blob in ENTRIES:
            self.assertEqual((out / pkd.rel_path(name)).read_bytes(), blob)

    def test_extract_into_mods_keeps_an_edited_file(self):
        mine = self.edit("media/playdatabase/offense/dagger.pd", b"edited")  # another case
        code, out, _ = self.pkd("extract", "--mods", "--mods-dir", self.mods, "media/PlayDatabase/*")
        self.assertEqual(code, 0)
        self.assertIn("kept", out)
        self.assertEqual(mine.read_bytes(), b"edited")
        self.assertEqual((self.mods / "media/playdatabase/Offensive_Plays.xml").read_bytes(), BOOK)
        self.assertEqual(sorted(p.name for p in (self.mods / "media").iterdir()), ["playdatabase"],
                         "the folder that was there, in its own case, is the one used")
        code, _, _ = self.pkd("extract", "--mods", "--mods-dir", self.mods, "--force", "*Dagger.pd")
        self.assertEqual(mine.read_bytes(), PLAY)

    def test_extract_into_mods_needs_a_pattern(self):
        code, _, err = self.pkd("extract", "--mods", "--mods-dir", self.mods)
        self.assertEqual(code, 1)
        self.assertIn("name the entries", err)
        self.assertFalse(self.mods.exists())

    def test_nothing_matches(self):
        code, _, err = self.pkd("extract", self.root / "out", "nothing/*")
        self.assertEqual(code, 1)
        self.assertIn("no entry matches", err)


class WherePkdWritesTests(unittest.TestCase):
    """The game's files never go where git would pick them up (needs git and this checkout)."""

    def test_checkout_folders(self):
        if not (pkd.CHECKOUT / ".git").exists():
            self.skipTest("not a git checkout")
        with self.assertRaises(pkd.PackError):
            pkd.check_writable_place(pkd.CHECKOUT / "tools" / "media")
        with self.assertRaises(pkd.PackError):
            pkd.check_writable_place(pkd.CHECKOUT / "mods")
        pkd.check_writable_place(pkd.CHECKOUT / "game" / "mods" / "media")   # game/ is ignored
        pkd.check_writable_place(pathlib.Path(tempfile.gettempdir()) / "elsewhere")

    def test_extract_refuses(self):
        with tempfile.TemporaryDirectory() as t:
            prefix = make_pack(pathlib.Path(t) / "p")
            code, _, err = run("--pack", prefix, "extract", pkd.CHECKOUT / "tools" / "unpacked")
            self.assertEqual(code, 1)
            self.assertIn("inside the checkout", err)
            self.assertFalse((pkd.CHECKOUT / "tools" / "unpacked").exists())


class RepackTests(Base):
    def test_unchanged_archive_comes_back_byte_for_byte(self):
        code, _, _ = self.pkd("repack", self.root / "nothing", self.root / "new" / "PackedOutputX360")
        self.assertEqual(code, 0)
        for ext in (".pkh", ".pkd"):
            self.assertEqual((self.root / "new" / ("PackedOutputX360" + ext)).read_bytes(),
                             pkd.sibling(self.prefix, ext).read_bytes())

    def test_replacement_and_files_left_out(self):
        self.edit("MEDIA/images/FIELD.dds", b"new texture" * 500)
        self.edit("media/not/in/the/archive.xml", b"x")
        code, out, _ = self.pkd("repack", self.mods, self.root / "new" / "Packed.v2")
        self.assertEqual(code, 0)
        self.assertIn("1 from", out)
        self.assertIn("left out media/not/in/the/archive.xml", out)
        with pkd.Pack(self.root / "new" / "Packed.v2.pkh") as p:
            self.assertEqual([e[0] for e in p.entries], [n for n, _ in ENTRIES])
            self.assertTrue(all(off % pkd.ALIGN == 0 for _, off, _ in p.entries))
            self.assertEqual(p.data(p.find("media/images/field.dds")), b"new texture" * 500)
            self.assertEqual(p.data(p.find("media/PlayDatabase/Offense/Dagger.pd")), PLAY)
            self.assertEqual(p.pkd_size % pkd.ALIGN, 0)

    def test_wont_overwrite_its_input(self):
        code, _, err = self.pkd("repack", self.mods, self.prefix)
        self.assertEqual(code, 1)
        self.assertIn("won't overwrite", err)


def edited_play():
    lines = PLAY.split(b"\r\n")
    lines[4] = b"\t\t\t<Position>-20,1</Position>"            # changed
    lines.insert(30, b"\t\t<!-- a note of mine -->")           # added, in a second hunk
    del lines[40]                                              # removed
    return b"\r\n".join(lines)


class ModTests(Base):
    def make_mod(self):
        self.edit("media/PlayDatabase/Offense/Dagger.pd", edited_play())
        self.edit("media/PlayDatabase/Offensive_Plays.xml", BOOK.replace(b"Dagger", b"Dagger Deep"))
        self.edit("media/ui/Final_XUI/single player/title.xui", b"<scene>\n<Id>Mine</Id>\n</scene>\n")
        self.edit("media/Movies/505.wmv", b"a whole-file replacement, not an archive entry")
        patch = self.root / "my.patch"
        code, _, err = self.pkd("diff", "--mods-dir", self.mods, "-o", patch)
        self.assertEqual(code, 0, err)
        return patch

    def apply(self, patch, mods):
        return self.pkd("apply", patch, "--mods-dir", mods)

    def test_diff_holds_changes_only(self):
        text = self.make_mod().read_bytes()
        self.assertTrue(text.startswith(pkd.MOD_HEADER.encode()))
        self.assertIn(b"# entry media\\PlayDatabase\\Offense\\Dagger.pd sha256 " +
                      hashlib.sha256(PLAY).hexdigest().encode(), text)
        self.assertIn(b"+++ b/media/ui/Final_XUI/Single Player/Title.xui", text)  # the entry's own case
        self.assertIn(b"\\ No newline at end of file", text)
        self.assertNotIn(b"505.wmv", text)
        self.assertNotIn(b'role="wr5"', text, "lines away from a change aren't in the patch")

    def test_apply_reproduces_the_edits(self):
        patch = self.make_mod()
        fresh = self.root / "fresh"
        code, out, err = self.apply(patch, fresh)
        self.assertEqual(code, 0, err)
        self.assertEqual((fresh / "media/PlayDatabase/Offense/Dagger.pd").read_bytes(), edited_play())
        self.assertEqual((fresh / "media/PlayDatabase/Offensive_Plays.xml").read_bytes(),
                         BOOK.replace(b"Dagger", b"Dagger Deep"))
        self.assertEqual((fresh / "media/ui/Final_XUI/Single Player/Title.xui").read_bytes(),
                         b"<scene>\n<Id>Mine</Id>\n</scene>\n")

    def test_apply_a_patch_that_lost_its_crs(self):
        patch = self.make_mod()
        patch.write_bytes(patch.read_bytes().replace(b"\r\n", b"\n"))
        fresh = self.root / "fresh"
        code, _, err = self.apply(patch, fresh)
        self.assertEqual(code, 0, err)
        self.assertEqual((fresh / "media/PlayDatabase/Offense/Dagger.pd").read_bytes(), edited_play())

    def test_apply_on_top_of_a_file_already_there(self):
        patch = self.make_mod()
        other = self.root / "other"
        first = other / "media/PlayDatabase/Offense/Dagger.pd"
        first.parent.mkdir(parents=True)
        first.write_bytes(b"<!-- another mod's line -->\r\n<!-- and another -->\r\n" + PLAY)
        code, out, err = self.apply(patch, other)
        self.assertEqual(code, 0, err)
        self.assertIn("on top of the file that was there", out)
        self.assertEqual(first.read_bytes(), b"<!-- another mod's line -->\r\n<!-- and another -->\r\n" + edited_play())

    def test_a_different_copy_writes_nothing(self):
        patch = self.make_mod()
        other = make_pack(self.root / "other-disc", [(n, b + b"\r\n" if n.endswith(".pd") else b) for n, b in ENTRIES])
        fresh = self.root / "fresh"
        code, _, err = run("--pack", other, "apply", patch, "--mods-dir", fresh)
        self.assertEqual(code, 1)
        self.assertIn("different copy", err)
        self.assertFalse(fresh.exists())

    def test_a_hunk_that_doesnt_match_writes_nothing(self):
        patch = self.make_mod()
        other = self.root / "other"
        (other / "media/ui/Final_XUI/Single Player").mkdir(parents=True)
        (other / "media/ui/Final_XUI/Single Player/Title.xui").write_bytes(b"something else entirely\n")
        code, _, err = self.apply(patch, other)
        self.assertEqual(code, 1)
        self.assertIn("doesn't match", err)
        self.assertFalse((other / "media/PlayDatabase").exists(), "the files before it aren't written either")

    def test_gnu_diff_output_applies(self):
        patch = self.root / "gnu.patch"
        patch.write_bytes(
            b"diff -ru a/media/PlayDatabase/Offensive_Plays.xml b/media/PlayDatabase/Offensive_Plays.xml\n"
            b"--- a/media/PlayDatabase/Offensive_Plays.xml\t2026-11-18 10:00:00.000000000 +0000\n"
            b"+++ b/media/PlayDatabase/Offensive_Plays.xml\t2026-11-18 10:05:00.000000000 +0000\n"
            b"@@ -2,3 +2,4 @@\n"
            b"   <PDPlay>\n"
            b"     <File>Offense\\Dagger.pd</File>\n"
            b"+    <File>Offense\\Mine.pd</File>\n"
            b"   </PDPlay>\n")
        code, _, err = self.apply(patch, self.root / "fresh")
        self.assertEqual(code, 0, err)
        self.assertEqual((self.root / "fresh/media/PlayDatabase/Offensive_Plays.xml").read_bytes(),
                         BOOK.replace(b"Dagger.pd</File>\n", b"Dagger.pd</File>\n    <File>Offense\\Mine.pd</File>\n"))

    def test_binary_entries_are_left_out(self):
        self.edit("media/images/field.dds", b"\0mine")
        code, _, err = self.pkd("diff", "--mods-dir", self.mods)
        self.assertEqual(code, 1)
        self.assertIn("left out media\\images\\field.dds", err)
        self.assertIn("nothing in", err)

    def test_an_entry_the_archive_doesnt_have(self):
        patch = self.root / "x.patch"
        patch.write_bytes(b"--- a/../outside.xml\n+++ b/../outside.xml\n@@ -0,0 +1 @@\n+x\n")
        code, _, err = self.apply(patch, self.root / "fresh")
        self.assertEqual(code, 1)
        self.assertIn("no entry by that name", err)
        self.assertFalse((self.root / "outside.xml").exists())


if __name__ == "__main__":
    unittest.main()
