"""The Flasher's side of language packs: the panel's language at install, installed languages
brought to the bundle's version at update (docs/features/37 language-packs.md, I18N-045)."""
import dataclasses
import hashlib
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from roboflash import packstore, testkit  # noqa: E402
from roboflash.boards import load_catalogue  # noqa: E402
from roboflash.fakechip import p4_v13  # noqa: E402
from roboflash.jobs import JobFailed, Runner  # noqa: E402
from roboflash.probe import ProbeResult  # noqa: E402
from test_flasher_bundle import pack_file, languages  # noqa: E402
from test_flasher_jobs import BootSerial, GOOD_BOOT  # noqa: E402

ASSETS_AT = 0x800000
ASSETS_SIZE = 0x100000          # 256 sectors
SECTOR = packstore.SECTOR


def partition_table():
    def rec(label, offset, size, subtype, ptype=1):
        return b"\xaa\x50" + bytes([ptype, subtype]) + struct.pack("<II", offset, size) + \
            label.encode().ljust(16, b"\0") + struct.pack("<I", 0)
    data = rec("otadata", 0x2D000, 0x2000, 0) + rec("app", 0x30000, 0x300000, 0x10, ptype=0) + \
        rec("assets", ASSETS_AT, ASSETS_SIZE, 0x40)
    return data + b"\xff" * (3 * 1024 - len(data))


def image_with_assets(profile="dev"):
    img = testkit.synthetic_image(profile=profile)
    parts = [dataclasses.replace(p, data=partition_table(),
                                 sha256=hashlib.sha256(partition_table()).hexdigest())
             if p.role == "partition-table" else p for p in img.parts]
    return dataclasses.replace(img, parts=parts)


def assets(board):
    return bytes(board.flash[ASSETS_AT:ASSETS_AT + ASSETS_SIZE])


def mount(board):
    a = assets(board)
    return packstore.Store.mount(a[:SECTOR], a[SECTOR:2 * SECTOR], ASSETS_SIZE // SECTOR)


def preinstall(board, *packs):
    """Packs (data, flags) put into the fake chip's store the way the Flasher would."""
    store = packstore.Store(ASSETS_SIZE // SECTOR)
    for data, flags in packs:
        info = packstore.read_pack(data)
        plan = store.plan_put(packstore.Entry(packstore.KIND_LANGUAGE, flags, 0, 0, info.payload_length,
                                              b"", info.locale), data)
        for off, blob in plan.writes:
            board.flash[ASSETS_AT + off:ASSETS_AT + off + len(blob)] = blob
        store = plan.store


class LanguageJobTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.img = image_with_assets()
        self.fr = pack_file("fr-CA")
        self.es = pack_file("es-US", draft=True)
        self.langs = languages(self.fr, self.es)
        self.board = p4_v13()
        self.probe = ProbeResult(port="COM4")
        self.runner = Runner(emit=lambda d: None, catalogue=load_catalogue(),
                             connect=lambda port, log, cancel, baud: self.board.connect(port, log, cancel, baud),
                             probe=lambda port: self.probe, backups_dir=self.tmp.name,
                             opener=lambda: BootSerial(GOOD_BOOT), boot_timeout=2)

    def tearDown(self):
        self.tmp.cleanup()

    def run_job(self, job):
        deadline = time.time() + 20
        while job.state == "running" and time.time() < deadline:
            time.sleep(0.01)
        return job

    def install(self, language, image=None):
        return self.run_job(self.runner.install("COM4", image or self.img, backup=False, language=language))

    def update(self, langs=None):
        self.probe = ProbeResult(port="COM4", answered=True, idle=True, version="0.5.9", board="crowpanel_p4")
        for p in self.img.parts:
            self.board.flash[p.offset:p.offset + len(p.data)] = p.data
        return self.run_job(self.runner.update("COM4", self.img, languages=self.langs if langs is None else langs))

    def test_install_writes_the_pack_then_a_table_with_it_flagged_default(self):  # I18N-045
        job = self.install(self.langs[0])
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual([s.key for s in job.steps][-3:], ["language", "restart", "boot"])
        self.assertEqual(job.step("language").state, "done")
        self.assertEqual(assets(self.board)[2 * SECTOR:2 * SECTOR + len(self.fr)], self.fr)
        store = mount(self.board)
        e = store.find(packstore.KIND_LANGUAGE, "fr-CA")
        self.assertEqual((e.first_sector, e.length, e.flags, e.sha256),
                         (2, len(self.fr), packstore.FLAG_DEFAULT, hashlib.sha256(self.fr).digest()))
        self.assertEqual(len(store.entries), 1)
        a = assets(self.board)
        self.assertIsNotNone(packstore.decode_table(a[SECTOR:2 * SECTOR], ASSETS_SIZE // SECTOR))
        # the blob is written before either table sector
        offs = [o for o, _ in self.board.writes if o >= ASSETS_AT]
        self.assertEqual(offs[0], ASSETS_AT + 2 * SECTOR)
        self.assertEqual(offs[1:], [ASSETS_AT, ASSETS_AT + SECTOR])   # A, then B (flagged default)

    def test_install_without_a_language_leaves_assets_alone(self):  # I18N-045
        job = self.install(None)
        self.assertEqual(job.state, "done", job.error)
        self.assertNotIn("language", [s.key for s in job.steps])
        self.assertEqual(assets(self.board), b"\xff" * ASSETS_SIZE)
        self.assertFalse([o for o, _ in self.board.writes if o >= ASSETS_AT])

    def test_a_draft_is_refused_for_a_release_image(self):  # I18N-045
        release = image_with_assets("release")
        with self.assertRaises(JobFailed) as cm:
            self.runner.install("COM4", release, backup=False, language=self.langs[1])
        self.assertIn("draft", cm.exception.title)
        self.assertEqual(self.board.connects, 0)
        self.assertEqual(self.install(self.langs[0], release).state, "done")   # a final pack is fine

    def test_a_damaged_blob_stops_before_the_table_is_written(self):  # I18N-045
        self.board = p4_v13(corrupt_writes={ASSETS_AT + 2 * SECTOR})
        job = self.install(self.langs[0])
        self.assertEqual(job.state, "failed")
        self.assertEqual(job.step("language").state, "failed")
        self.assertIn("does not read back", job.error["title"])
        self.assertEqual(assets(self.board)[:2 * SECTOR], b"\xff" * (2 * SECTOR))
        self.assertEqual(self.board.resets, 0)

    def test_update_replaces_an_old_pack_keeping_its_flags(self):  # I18N-045
        old = pack_file("fr-CA", version="0.5.0")
        keep = pack_file("it-IT", version="0.5.0")
        current = pack_file("es-US", draft=True)
        preinstall(self.board, (old, packstore.FLAG_DEFAULT), (keep, 0), (current, 0))
        before = mount(self.board)
        job = self.update()
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(job.step("language").label, "Bring the languages up to date")
        after = mount(self.board)
        fr = after.find(packstore.KIND_LANGUAGE, "fr-CA")
        self.assertEqual(fr.flags, packstore.FLAG_DEFAULT)
        self.assertEqual(bytes(self.board.flash[ASSETS_AT + fr.first_sector * SECTOR:][:len(self.fr)]), self.fr)
        self.assertNotEqual(fr.first_sector, before.find(packstore.KIND_LANGUAGE, "fr-CA").first_sector)
        for name in ("it-IT", "es-US"):      # not in the bundle / already current: untouched
            self.assertEqual(after.find(packstore.KIND_LANGUAGE, name), before.find(packstore.KIND_LANGUAGE, name))
        self.assertTrue(any("it-IT: not in this bundle" in line for line in job.log))

    def test_update_with_everything_current_writes_nothing_to_assets(self):  # I18N-045
        preinstall(self.board, (self.fr, packstore.FLAG_DEFAULT))
        before = assets(self.board)
        job = self.update()
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(assets(self.board), before)
        self.assertFalse([o for o, _ in self.board.writes if o >= ASSETS_AT])


if __name__ == "__main__":
    unittest.main()
