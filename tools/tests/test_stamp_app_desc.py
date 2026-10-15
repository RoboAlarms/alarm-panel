"""docs/features/07 UI-035 (System details' build date), BL-176: every build that links the app
stamps its own date and time into esp_app_desc (tools/stamp_app_desc.py)."""
import importlib.util
import io
import struct
import tempfile
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("stamp_app_desc", TOOLS / "stamp_app_desc.py")
sad = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sad)

ADDR = 0x40000020  # where ESP-IDF puts the descriptor: the start of the first flash segment


def descriptor(magic=0xABCD5432, time_=b"23:16:08", date=b"Oct  5 2026"):
    """An esp_app_desc_t (256 bytes) with an old build's date."""
    d = bytearray(256)
    struct.pack_into("<I", d, 0, magic)
    d[16:21] = b"0.7.0"
    d[48:60] = b"alarm_test-x"
    d[80:80 + len(time_)] = time_
    d[96:96 + len(date)] = date
    d[112:116] = b"v6.1"
    return bytes(d)


def make_elf(cls=32, desc=None, name=b"esp_app_desc", size=256):
    """A small little-endian ELF: a data section holding 16 bytes and then the descriptor, a
    symbol table with a decoy whose name ends in esp_app_desc, and the descriptor's symbol, its
    name the decoy's tail (a linker may share string-table suffixes so)."""
    desc = descriptor() if desc is None else desc
    data = b"\x11" * 16 + desc
    strtab = b"\0my_esp_app_desc\0" + name + b"\0"
    shstrtab = b"\0.flash.appdesc\0.symtab\0.strtab\0.shstrtab\0"
    if cls == 32:
        ehdr, shdr, symf = struct.Struct("<16sHHIIIIIHHHHHH"), struct.Struct("<10I"), "<IIIBBH"
    else:
        ehdr, shdr = struct.Struct("<16sHHIQQQIHHHHHH"), struct.Struct("<IIQQQQIIQQ")
        symf = "<IBBHQQ"

    def sym(name_off, value, sz, shndx):
        if cls == 32:
            return struct.pack(symf, name_off, value, sz, 0x11, 0, shndx)
        return struct.pack(symf, name_off, 0x11, 0, shndx, value, sz)

    symtab = (sym(0, 0, 0, 0) + sym(1, ADDR, 16, 1)
              + sym(strtab.index(name + b"\0", 2), ADDR + 16, size, 1))
    off_data = ehdr.size
    off_sym = off_data + len(data)
    off_str = off_sym + len(symtab)
    off_shstr = off_str + len(strtab)
    shoff = off_shstr + len(shstrtab)
    entsize = len(sym(0, 0, 0, 0))
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 2, ADDR, off_data, len(data), 0, 0, 4, 0),  # PROGBITS, ALLOC
        (16, 2, 0, 0, off_sym, len(symtab), 3, 2, 4, entsize),  # SYMTAB, links .strtab
        (24, 3, 0, 0, off_str, len(strtab), 0, 0, 1, 0),
        (32, 3, 0, 0, off_shstr, len(shstrtab), 0, 0, 1, 0),
    ]
    ident = b"\x7fELF" + bytes([1 if cls == 32 else 2, 1, 1]) + b"\0" * 9
    head = ehdr.pack(ident, 2, 0xF3, 1, 0, 0, shoff, 0, ehdr.size, 0, 0, shdr.size,
                     len(sections), 4)
    return head + data + symtab + strtab + shstrtab + b"".join(shdr.pack(*s) for s in sections)


def fields(blob):
    off = sad.find_app_desc(blob)
    return blob[off + 80:off + 96], blob[off + 96:off + 112]


class StampTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def elf(self, **kw):
        path = Path(self.tmp.name) / "app.elf"
        path.write_bytes(make_elf(**kw))
        return path

    def test_stamps_the_link_time_in_the_compilers_format(self):
        # UI-035, BL-176: the date and time are this build's, as __DATE__/__TIME__ write them.
        for cls in (32, 64):
            path = self.elf(cls=cls)
            before = path.read_bytes()
            when = time.struct_time((2026, 10, 6, 9, 5, 3, 1, 279, 0))
            self.assertEqual(sad.stamp(path, when), ("09:05:03", "Oct  6 2026"))
            after = path.read_bytes()
            self.assertEqual(fields(after), (b"09:05:03".ljust(16, b"\0"),
                                             b"Oct  6 2026".ljust(16, b"\0")))
            # Only the two fields change: the version, the name and the rest stay.
            changed = [i for i in range(len(before)) if before[i] != after[i]]
            off = sad.find_app_desc(after)
            self.assertTrue(changed)
            self.assertTrue(all(off + 80 <= i < off + 112 for i in changed), changed)

    def test_two_digit_day_and_month_names(self):
        path = self.elf()
        sad.stamp(path, time.struct_time((2027, 1, 16, 23, 59, 59, 5, 16, 0)))
        self.assertEqual(fields(path.read_bytes())[1], b"Jan 16 2027".ljust(16, b"\0"))

    def test_source_date_epoch_is_utc_like_gcc(self):
        when = sad.build_time({"SOURCE_DATE_EPOCH": "1791331200"})
        self.assertEqual(sad.fields(when), ("00:00:00", "Oct  7 2026"))

    def test_a_descriptor_without_a_date_is_left_alone(self):
        # CONFIG_APP_COMPILE_TIME_DATE off (a reproducible build): no date appears.
        path = self.elf(desc=descriptor(time_=b"", date=b""))
        before = path.read_bytes()
        self.assertIsNone(sad.stamp(path, time.localtime()))
        self.assertEqual(path.read_bytes(), before)

    def test_refuses_an_elf_without_a_descriptor(self):
        for kw in ({"name": b"other_symbol"}, {"desc": descriptor(magic=0x12345678)},
                   {"size": 128}):
            path = self.elf(**kw)
            before = path.read_bytes()
            with self.assertRaises(sad.StampError):
                sad.stamp(path, time.localtime())
            self.assertEqual(path.read_bytes(), before)
        with self.assertRaises(sad.StampError):
            sad.find_app_desc(b"not an elf")

    def test_main_reports_and_fails_the_build(self):
        good, out = self.elf(), io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(sad.main([str(good)]), 0)
        self.assertIn("app.elf built", out.getvalue())
        bad = Path(self.tmp.name) / "bad.elf"
        bad.write_bytes(b"\x7fELF")
        with redirect_stderr(io.StringIO()):
            self.assertEqual(sad.main([str(bad)]), 1)
            self.assertEqual(sad.main([str(Path(self.tmp.name) / "missing.elf")]), 1)
            self.assertEqual(sad.main([]), 2)


if __name__ == "__main__":
    unittest.main()
