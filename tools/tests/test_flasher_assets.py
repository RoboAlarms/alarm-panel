"""The Flasher's side of the asset packs (letters, saver picture, certificates): a bundle carries
them, install writes them after the firmware, update brings them to the bundle's version
(docs/features/39-Optimizations/asset-packs.md "The Flasher", OPT-007)."""
import hashlib
import importlib.util
import json
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from roboflash import bundle as bundle_mod, make_bundle, packstore, testkit  # noqa: E402
from roboflash.boards import load_catalogue  # noqa: E402
from roboflash.fakechip import p4_v13  # noqa: E402
from roboflash.jobs import JobFailed, Runner  # noqa: E402
from roboflash.probe import ProbeResult  # noqa: E402
from test_flasher_bundle import rewrite, pack_file, languages  # noqa: E402
from test_flasher_jobs import BootSerial, GOOD_BOOT  # noqa: E402
from test_flasher_languages import (ASSETS_AT, ASSETS_SIZE, SECTOR, assets, image_with_assets,  # noqa: E402
                                    mount, preinstall)

_SPEC = importlib.util.spec_from_file_location("asset_pack", Path(__file__).resolve().parents[1] / "asset_pack.py")
ap = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(ap)

PART = {packstore.KIND_LETTERS: ("latin", packstore.PART_FONT),
        packstore.KIND_PICTURE: ("photo", packstore.PART_JPEG),
        packstore.KIND_CERTS: ("bundle", packstore.PART_CERTS)}


def asset_file(kind, version="0.6.0", draft=False, size=5000, key=testkit.TEST_KEY):
    """A signed-looking .roboasset (made-up signature, the envelope right) of fictional bytes."""
    name = packstore.ASSET_STORE_NAMES[kind]
    pname, ptype = PART[kind]
    payload = ap.build_payload(kind, name, version, [(pname, ptype, bytes([kind]) * size)], draft=draft)
    return payload + b"\xff" * (-len(payload) % SECTOR) + testkit.signature_sector(key)


def asset_list(*files):
    out = []
    for data in files:
        i = packstore.read_asset(data)
        out.append(bundle_mod.Asset(i.kind, i.name, i.version, i.draft, "", "", data))
    return out


def three(version="0.6.0", **kw):
    return [asset_file(k, version, **kw) for k in (packstore.KIND_LETTERS, packstore.KIND_PICTURE,
                                                   packstore.KIND_CERTS)]


class BundleAssetTests(unittest.TestCase):
    def setUp(self):
        self.images = testkit.standard_images()

    def data(self, *files):
        with tempfile.TemporaryDirectory() as d:
            path = bundle_mod.write(Path(d) / "t.roboflash", self.images, "2026-09-30T00:00:00+00:00",
                                    {"commit": "test"}, self.images[0].signing_key_sha256, (), (),
                                    asset_list(*files))
            return path.read_bytes()

    def test_round_trip_and_manifest(self):  # OPT-007
        files = three()
        data = self.data(*files)
        b = bundle_mod.load(data)
        self.assertEqual(sorted(a.name for a in b.assets), ["certs", "letters", "saver"])
        self.assertEqual({a.data for a in b.assets}, set(files))
        import io, zipfile
        m = json.loads(zipfile.ZipFile(io.BytesIO(data)).read("manifest.json"))
        self.assertEqual(set(m["assets"][0]), {"kind", "name", "version", "draft", "path", "sha256", "size"})
        self.assertEqual(b.to_dict()["assets"][0]["name"], m["assets"][0]["name"])

    def test_a_bundle_without_assets_loads(self):  # OPT-007
        b = bundle_mod.load(self.data())
        self.assertEqual(b.assets, [])

    def test_another_key_is_refused(self):  # OPT-007
        other = bytes(reversed(testkit.TEST_KEY))
        with self.assertRaisesRegex(bundle_mod.BundleError, "another key"):
            bundle_mod.load(self.data(asset_file(packstore.KIND_LETTERS, key=other)))

    def test_another_version_is_refused(self):  # OPT-007
        with self.assertRaisesRegex(bundle_mod.BundleError, "made for version 0.5.0"):
            bundle_mod.load(self.data(asset_file(packstore.KIND_LETTERS, "0.5.0")))

    def test_damage_and_lies_are_refused(self):  # OPT-007
        data = self.data(asset_file(packstore.KIND_CERTS))

        def flip(f):
            f["assets/certs.roboasset"] = f["assets/certs.roboasset"][:-1] + b"\x00"
        with self.assertRaisesRegex(bundle_mod.BundleError, "checksum"):
            bundle_mod.load(rewrite(data, flip))

        def lie(f):
            m = json.loads(f["manifest.json"])
            m["assets"][0]["version"] = "9.9.9"
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "version"):
            bundle_mod.load(rewrite(data, lie))

        def junk(f):
            m = json.loads(f["manifest.json"])
            f["assets/certs.roboasset"] = b"x" * 8192
            m["assets"][0].update(sha256=hashlib.sha256(b"x" * 8192).hexdigest(), size=8192)
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "certs.roboasset"):
            bundle_mod.load(rewrite(data, junk))

    def test_an_older_reader_would_ignore_the_list(self):  # OPT-007
        import io, zipfile
        m = json.loads(zipfile.ZipFile(io.BytesIO(self.data(asset_file(packstore.KIND_CERTS)))).read("manifest.json"))
        self.assertEqual(m["format"], 1)

    def test_make_bundle_takes_the_first_builds_assets_folder(self):  # OPT-007
        from test_flasher_bundle import MakeBundleTests
        with tempfile.TemporaryDirectory() as root:
            mk = MakeBundleTests()
            dirs = [mk.build_dir(root, i) for i in testkit.standard_images()[:2]]
            (dirs[0] / "assets").mkdir()
            for f in three():
                (dirs[0] / "assets" / (packstore.ASSET_STORE_NAMES[packstore.read_asset(f).kind] + ".roboasset")).write_bytes(f)
            out = Path(root) / "out.roboflash"
            notes = []
            make_bundle.make(dirs, out=out, key=None, log=notes.append)
            self.assertEqual(sorted(a.name for a in bundle_mod.load(out).assets), ["certs", "letters", "saver"])
            self.assertTrue(any("asset pack letters" in n for n in notes))
            # Another board's build with the same packs (the 10.1" beside the 7") bundles; other
            # contents under the same name are refused: one set serves every image.
            ten = mk.build_dir(root, testkit.standard_images()[3])
            (ten / "assets").mkdir()
            for f in three():
                (ten / "assets" / (packstore.ASSET_STORE_NAMES[packstore.read_asset(f).kind] + ".roboasset")).write_bytes(f)
            make_bundle.make(dirs + [ten], out=out, key=None, log=notes.append)
            self.assertEqual(len(bundle_mod.load(out).images), 3)
            (ten / "assets" / "saver.roboasset").write_bytes(asset_file(packstore.KIND_PICTURE, size=6000))
            with self.assertRaisesRegex(bundle_mod.BundleError, "one set of asset packs"):
                make_bundle.make(dirs + [ten], out=out, key=None, log=notes.append)
            (dirs[0] / "assets" / "certs.roboasset").write_bytes(asset_file(packstore.KIND_CERTS, "0.1.0"))
            with self.assertRaisesRegex(bundle_mod.BundleError, "made for version"):
                make_bundle.make(dirs, out=out, key=None, log=notes.append)


class AssetJobTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.img = image_with_assets()
        self.files = three()
        self.packs = asset_list(*self.files)
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

    def install(self, image=None, **kw):
        return self.run_job(self.runner.install("COM4", image or self.img, backup=False, **kw))

    def update(self, packs=None):
        self.probe = ProbeResult(port="COM4", answered=True, idle=True, version="0.5.9", board="crowpanel_p4")
        for p in self.img.parts:
            self.board.flash[p.offset:p.offset + len(p.data)] = p.data
        return self.run_job(self.runner.update("COM4", self.img, assets=self.packs if packs is None else packs))

    def test_install_writes_every_pack_after_the_firmware_none_default(self):  # OPT-007
        job = self.install(assets=self.packs)
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual([s.key for s in job.steps][-3:], ["assets", "restart", "boot"])
        self.assertEqual(job.step("assets").label, "Install the letters, picture and certificates")
        store = mount(self.board)
        self.assertEqual(len(store.entries), 3)
        for kind, data in zip((packstore.KIND_LETTERS, packstore.KIND_PICTURE, packstore.KIND_CERTS), self.files):
            e = store.find(kind, packstore.ASSET_STORE_NAMES[kind])
            self.assertEqual((e.flags, e.length, e.sha256), (0, len(data), hashlib.sha256(data).digest()))
            self.assertEqual(bytes(self.board.flash[ASSETS_AT + e.first_sector * SECTOR:][:len(data)]), data)
        firmware_end = max(i for i, (o, _) in enumerate(self.board.writes) if o < ASSETS_AT)
        first_asset = min(i for i, (o, _) in enumerate(self.board.writes) if o >= ASSETS_AT)
        self.assertLess(firmware_end, first_asset)

    def test_install_after_a_language(self):  # OPT-007
        lang = languages(pack_file("fr-CA"))[0]
        job = self.install(language=lang, assets=self.packs)
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual([s.key for s in job.steps][-4:], ["language", "assets", "restart", "boot"])
        store = mount(self.board)
        self.assertEqual(store.find(packstore.KIND_LANGUAGE, "fr-CA").flags, packstore.FLAG_DEFAULT)
        self.assertEqual(len(store.entries), 4)

    def test_install_without_assets_or_without_a_partition_adds_no_step(self):  # OPT-007
        job = self.install()
        self.assertNotIn("assets", [s.key for s in job.steps])
        self.assertEqual(assets(self.board), b"\xff" * ASSETS_SIZE)
        job = self.install(testkit.synthetic_image(), assets=self.packs)
        self.assertEqual(job.state, "done", job.error)
        self.assertNotIn("assets", [s.key for s in job.steps])

    def test_a_draft_is_refused_for_a_release_image(self):  # OPT-007
        draft = asset_list(asset_file(packstore.KIND_LETTERS, draft=True))
        with self.assertRaises(JobFailed) as cm:
            self.runner.install("COM4", image_with_assets("release"), backup=False, assets=draft)
        self.assertIn("draft", cm.exception.title)
        self.assertEqual(self.board.connects, 0)
        self.assertEqual(self.install(assets=draft).state, "done")     # a dev image takes it

    def test_a_pack_that_does_not_fit_leaves_the_firmware_installed(self):  # OPT-007
        big = asset_list(asset_file(packstore.KIND_LETTERS, size=ASSETS_SIZE),
                         asset_file(packstore.KIND_CERTS))
        job = self.install(assets=big)
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(job.result["assets_not_stored"], ["letters"])
        self.assertTrue(any("letters: not stored" in line for line in job.log))
        store = mount(self.board)
        self.assertIsNone(store.find(packstore.KIND_LETTERS, "letters"))
        self.assertIsNotNone(store.find(packstore.KIND_CERTS, "certs"))

    def put(self, *files):
        store = packstore.Store(ASSETS_SIZE // SECTOR)
        for data in files:
            i = packstore.read_asset(data)
            plan = store.plan_put(packstore.Entry(i.kind, 0, 0, 0, i.payload_length, b"", i.name), data)
            for off, blob in plan.writes:
                self.board.flash[ASSETS_AT + off:ASSETS_AT + off + len(blob)] = blob
            store = plan.store

    def test_a_corrupt_installed_length_is_replaced_without_a_huge_read(self):  # FLS2-004 (OPT-007)
        """The table's CRC is right but an entry's payload length is not what its blob can hold:
        the installed pack counts as unreadable and is replaced; nothing past it is ever read."""
        old = asset_file(packstore.KIND_LETTERS, "0.5.0")
        fits = packstore.read_asset(old).payload_length
        for bad in (0, 1, packstore.ASSET_HEADER - 1, len(old) - SECTOR + 1, len(old), 0x7FFFFFFF,
                    0xFFFFFFFC, 0xFFFFFFFF):
            with self.subTest(payload_length=bad):
                self.board = p4_v13()
                store = packstore.Store(ASSETS_SIZE // SECTOR)
                plan = store.plan_put(packstore.Entry(packstore.KIND_LETTERS, 0, 0, 0, bad, b"", "letters"), old)
                for off, blob in plan.writes:
                    self.board.flash[ASSETS_AT + off:ASSETS_AT + off + len(blob)] = blob
                entry = mount(self.board).find(packstore.KIND_LETTERS, "letters")
                self.assertEqual(entry.payload_length, bad)        # the table took it: its CRC is right
                job = self.update(asset_list(self.files[0]))
                self.assertEqual(job.state, "done", job.error)
                for offset, size in self.board.reads:
                    if offset >= ASSETS_AT:
                        self.assertLessEqual(offset + size, ASSETS_AT + ASSETS_SIZE)
                        self.assertLessEqual(size, len(old))
                self.assertTrue(any("letters: unreadable to 0.6.0" in line for line in job.log), job.log)
                after = mount(self.board).find(packstore.KIND_LETTERS, "letters")
                self.assertEqual(after.payload_length, packstore.read_asset(self.files[0]).payload_length)
        self.assertTrue(packstore.payload_fits(packstore.Entry(packstore.KIND_LETTERS, 0, 2, len(old), fits,
                                                               b"", "letters")))

    def test_update_replaces_a_different_version_and_adds_a_missing_pack(self):  # OPT-007
        self.put(asset_file(packstore.KIND_LETTERS, "0.5.0"), asset_file(packstore.KIND_PICTURE))
        before = mount(self.board)
        job = self.update()
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(job.step("assets").label, "Bring the letters, picture and certificates up to date")
        after = mount(self.board)
        e = after.find(packstore.KIND_LETTERS, "letters")
        self.assertEqual(bytes(self.board.flash[ASSETS_AT + e.first_sector * SECTOR:][:len(self.files[0])]),
                         self.files[0])
        self.assertEqual(after.find(packstore.KIND_PICTURE, "saver"), before.find(packstore.KIND_PICTURE, "saver"))
        self.assertIsNotNone(after.find(packstore.KIND_CERTS, "certs"))
        self.assertTrue(any("saver: already at" in line for line in job.log))

    def test_update_leaves_an_unknown_kind_and_a_language_alone(self):  # OPT-007
        self.put(*[asset_file(packstore.KIND_LETTERS)])
        store = mount(self.board)
        web = packstore.Entry(packstore.KIND_WEB, 0, 0, 0, 100, b"", "web")
        plan = store.plan_put(web, b"w" * SECTOR)
        for off, blob in plan.writes:
            self.board.flash[ASSETS_AT + off:ASSETS_AT + off + len(blob)] = blob
        before = mount(self.board).find(packstore.KIND_WEB, "web")
        job = self.update(asset_list(*three()))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(mount(self.board).find(packstore.KIND_WEB, "web"), before)

    def test_update_with_everything_current_writes_nothing(self):  # OPT-007
        self.put(*self.files)
        before = assets(self.board)
        job = self.update()
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(assets(self.board), before)
        self.assertFalse([o for o, _ in self.board.writes if o >= ASSETS_AT])


if __name__ == "__main__":
    unittest.main()
