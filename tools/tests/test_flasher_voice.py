"""BL-204: the Flasher's side of the voice pack. A bundle carries signed voices, a new panel gets the
one for its language, and an update replaces the panel's voice only with a newer revision of the
same voice (docs/features/08 D08, docs/features/34 FLS-031)."""
import hashlib
import io
import json
import re
import struct
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from roboflash import bundle as bundle_mod, make_bundle, packstore, testkit  # noqa: E402
from roboflash.boards import load_catalogue  # noqa: E402
from roboflash.fakechip import p4_v13  # noqa: E402
from roboflash.jobs import JobFailed, Runner  # noqa: E402
from roboflash.probe import ProbeResult  # noqa: E402
from test_flasher_assets import asset_list, three  # noqa: E402
from test_flasher_bundle import rewrite  # noqa: E402
from test_flasher_jobs import BootSerial, GOOD_BOOT  # noqa: E402
from test_flasher_languages import ASSETS_AT, SECTOR, image_with_assets, mount  # noqa: E402

FIXTURE_H = (Path(__file__).resolve().parents[2] / "components" / "core" / "voice_pack" / "test"
             / "voice_pack_fixture_v2.h")


def fixture_payload():
    """The frozen RVOC format 1 payload (schema 2, revision 2, en-US) the C reader is held to."""
    body = FIXTURE_H.read_text(encoding="utf-8").split("voice_pack_fixture_v2[", 1)[1].split("{", 1)[1]
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body.split("}", 1)[0]))


def voice_file(revision=2, locale="en-US", voice_id=None, draft=False, key=testkit.TEST_KEY, payload=None):
    """A signed-looking .robovoice (made-up signature, the envelope right) of the fixture with its
    header's identity changed (the Flasher reads the header; the panel checks the clips)."""
    p = bytearray(payload or fixture_payload())
    struct.pack_into("<I", p, 12, revision)
    struct.pack_into("<I", p, 20, packstore.VOICE_FLAG_DRAFT if draft else 0)
    p[24:40] = locale.encode().ljust(16, b"\0")
    if voice_id is not None:
        p[40:72] = voice_id.encode().ljust(32, b"\0")
    return bytes(p) + b"\xff" * (-len(p) % SECTOR) + testkit.signature_sector(key)


def voices(*files):
    out = []
    for data in files:
        i = packstore.read_voice(data)
        out.append(bundle_mod.Voice(i.locale, i.voice_id, i.revision, i.draft, "", "", data))
    return out


class ReadVoiceTests(unittest.TestCase):
    def test_the_frozen_fixture_reads(self):  # BL-204, D08
        data = voice_file()
        i = packstore.read_voice(data)
        self.assertEqual((i.locale, i.revision, i.schema, i.draft), ("en-US", 2, 2, False))
        self.assertTrue(i.voice_id)
        self.assertEqual(i.payload_length, len(fixture_payload()))
        self.assertEqual(i.payload_sha256, hashlib.sha256(fixture_payload()).hexdigest())
        self.assertEqual(i.signing_key_sha256, testkit.standard_images()[0].signing_key_sha256)
        self.assertEqual(len(data), 2 * SECTOR)
        head = packstore.read_voice_header(data[:packstore.VOICE_HEADER])
        self.assertEqual((head.locale, head.voice_id, head.revision), (i.locale, i.voice_id, i.revision))

    def test_a_bad_envelope_or_header_is_refused(self):  # BL-204, D08
        good = voice_file()
        payload = fixture_payload()

        def header(off, fmt, value):
            p = bytearray(payload)
            struct.pack_into(fmt, p, off, value)
            return voice_file(payload=bytes(p))
        cases = [(good[:-1], "sectors"), (good[:SECTOR], "sectors"),
                 (b"XVOC" + good[4:], "magic"),
                 (header(4, "<H", 2), "format 2"),
                 (header(16, "<I", 3), "newer than this Flasher"),
                 (header(8, "<I", len(payload) + SECTOR), "length"),
                 (good[:len(payload)] + b"\x00" + good[len(payload) + 1:], "padding"),
                 (voice_file(locale="en US"), "locale"),
                 (voice_file(voice_id=""), "voice")]
        cases.append((voice_file(revision=0), "revision is 0"))
        p = bytearray(payload)
        struct.pack_into("<I", p, 20, 2)
        cases.append((bytes(p) + b"\xff" * (-len(p) % SECTOR) + testkit.signature_sector(testkit.TEST_KEY),
                      "unknown flags"))
        for data, words in cases:
            with self.subTest(words=words), self.assertRaisesRegex(packstore.StoreError, words):
                packstore.read_voice(data)


class BundleVoiceTests(unittest.TestCase):
    def setUp(self):
        self.images = testkit.standard_images()

    def data(self, *files):
        with tempfile.TemporaryDirectory() as d:
            path = bundle_mod.write(Path(d) / "t.roboflash", self.images, "2026-10-10T00:00:00+00:00",
                                    {"commit": "test"}, self.images[0].signing_key_sha256, (), (), (),
                                    voices(*files))
            return path.read_bytes()

    def test_round_trip_and_manifest(self):  # BL-204
        files = [voice_file(8), voice_file(3, locale="es-MX")]
        data = self.data(*files)
        b = bundle_mod.load(data)
        self.assertEqual([(v.locale, v.revision) for v in b.voices], [("en-US", 8), ("es-MX", 3)])
        self.assertEqual([v.data for v in b.voices], files)
        m = json.loads(zipfile.ZipFile(io.BytesIO(data)).read("manifest.json"))
        self.assertEqual(m["format"], 1)       # an older Flasher ignores the list
        self.assertEqual(set(m["voices"][0]), {"locale", "voice_id", "revision", "draft", "path", "sha256", "size"})
        self.assertEqual(b.to_dict()["voices"][0]["revision"], 8)
        self.assertEqual(bundle_mod.load(self.data()).voices, [])

    def test_another_key_damage_and_lies_are_refused(self):  # BL-204
        with self.assertRaisesRegex(bundle_mod.BundleError, "another key"):
            bundle_mod.load(self.data(voice_file(key=bytes(reversed(testkit.TEST_KEY)))))
        data = self.data(voice_file())
        name = json.loads(zipfile.ZipFile(io.BytesIO(data)).read("manifest.json"))["voices"][0]["path"]

        def flip(f):
            f[name] = f[name][:-1] + b"\x00"
        with self.assertRaisesRegex(bundle_mod.BundleError, "checksum"):
            bundle_mod.load(rewrite(data, flip))

        def lie(f):
            m = json.loads(f["manifest.json"])
            m["voices"][0]["revision"] = 9
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "revision"):
            bundle_mod.load(rewrite(data, lie))

        def twice(f):
            m = json.loads(f["manifest.json"])
            m["voices"].append(dict(m["voices"][0]))
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "twice"):
            bundle_mod.load(rewrite(data, twice))

    def test_the_voice_for_a_new_panel(self):  # BL-204, D09, ADR-053
        en, mx, es, draft = voices(voice_file(8), voice_file(3, locale="es-MX"), voice_file(2, locale="es-US"),
                                   voice_file(9, voice_id="next", draft=True))
        pick = bundle_mod.pick_voice
        self.assertIs(pick([en, mx, es], "es-MX"), mx)
        self.assertIs(pick([en, mx, es], "ES-mx"), mx)              # letter case aside
        self.assertIsNone(pick([en, es], "es-MX"))                  # never another language's voice (ADR-053)
        self.assertIsNone(pick([en, mx], "fr-CA"))                  # nor English's under another language
        self.assertIs(pick([en, mx]), en)                           # English: built in, only its voice
        self.assertIsNone(pick([mx]))
        self.assertIs(pick([en, draft], "en-US", "dev"), draft)     # the higher revision
        self.assertIs(pick([en, draft], "en-US", "release"), en)    # never a draft for a release image

    def test_make_bundle_takes_voice_files(self):  # BL-204
        from test_flasher_bundle import MakeBundleTests
        with tempfile.TemporaryDirectory() as root:
            mk = MakeBundleTests()
            dirs = [mk.build_dir(root, i) for i in testkit.standard_images()[:2]]
            f = Path(root) / "en-US.robovoice"
            f.write_bytes(voice_file(8))
            out = Path(root) / "out.roboflash"
            notes = []
            make_bundle.make(dirs, out=out, key=None, log=notes.append, voice_files=[f])
            self.assertEqual([v.label() for v in bundle_mod.load(out).voices],
                             [f"en-US {packstore.read_voice(f.read_bytes()).voice_id} r8"])
            with self.assertRaisesRegex(bundle_mod.BundleError, "twice"):
                make_bundle.make(dirs, out=out, key=None, log=notes.append, voice_files=[f, f])
            f.write_bytes(voice_file(key=bytes(reversed(testkit.TEST_KEY))))
            with self.assertRaisesRegex(bundle_mod.BundleError, "another key"):
                make_bundle.make(dirs, out=out, key=None, log=notes.append, voice_files=[f])
            f.write_bytes(b"\xff" * 8192)
            with self.assertRaisesRegex(bundle_mod.BundleError, "magic"):
                make_bundle.make(dirs, out=out, key=None, log=notes.append, voice_files=[f])


class VoiceJobTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.img = image_with_assets()
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

    def stored(self):
        e = mount(self.board).find(packstore.KIND_VOICE, packstore.VOICE_STORE_NAME)
        if e is None:
            return None
        return bytes(self.board.flash[ASSETS_AT + e.first_sector * SECTOR:][:e.length])

    def update(self, bundle_voices, image=None):
        self.probe = ProbeResult(port="COM4", answered=True, idle=True, version="0.5.9", board="crowpanel_p4")
        img = image or self.img
        for p in img.parts:
            self.board.flash[p.offset:p.offset + len(p.data)] = p.data
        return self.run_job(self.runner.update("COM4", img, voices=bundle_voices))

    def put_language(self, locale, default=True):
        """A language pack in the panel's store, marked as the panel's language (the Flasher's install) or not."""
        from test_flasher_bundle import pack_file
        data = pack_file(locale)
        i = packstore.read_pack(data)
        plan = mount(self.board).plan_put(packstore.Entry(packstore.KIND_LANGUAGE, packstore.FLAG_DEFAULT if default else 0,
                                                          0, 0, i.payload_length, b"", i.locale), data)
        for off, blob in plan.writes:
            self.board.flash[ASSETS_AT + off:ASSETS_AT + off + len(blob)] = blob

    def put(self, data):
        i = packstore.read_voice(data)
        store = mount(self.board)
        plan = store.plan_put(packstore.Entry(packstore.KIND_VOICE, 0, 0, 0, i.payload_length, b"",
                                              packstore.VOICE_STORE_NAME), data)
        for off, blob in plan.writes:
            self.board.flash[ASSETS_AT + off:ASSETS_AT + off + len(blob)] = blob

    def test_install_writes_the_voice_last(self):  # BL-204
        v = voices(voice_file(8))[0]
        job = self.run_job(self.runner.install("COM4", self.img, backup=False, assets=asset_list(*three()), voice=v))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual([s.key for s in job.steps][-4:], ["assets", "voice", "restart", "boot"])
        self.assertEqual(self.stored(), v.data)
        e = mount(self.board).find(packstore.KIND_VOICE, "primary")
        self.assertEqual((e.flags, e.sha256), (0, hashlib.sha256(v.data).digest()))

    def test_install_without_a_voice_or_a_partition_adds_no_step(self):  # BL-204
        job = self.run_job(self.runner.install("COM4", self.img, backup=False))
        self.assertNotIn("voice", [s.key for s in job.steps])
        job = self.run_job(self.runner.install("COM4", testkit.synthetic_image(), backup=False,
                                               voice=voices(voice_file())[0]))
        self.assertEqual(job.state, "done", job.error)
        self.assertNotIn("voice", [s.key for s in job.steps])

    def test_a_draft_voice_is_refused_for_a_release_image(self):  # BL-204
        draft = voices(voice_file(draft=True))[0]
        with self.assertRaises(JobFailed) as cm:
            self.runner.install("COM4", image_with_assets("release"), backup=False, voice=draft)
        self.assertIn("draft", cm.exception.title)
        self.assertEqual(self.board.connects, 0)

    def test_update_adds_a_voice_to_a_panel_without_one(self):  # BL-204
        job = self.update(voices(voice_file(3, locale="es-MX"), voice_file(8)))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(job.step("voice").label, "Bring the voice up to date")
        self.assertEqual(packstore.read_voice(self.stored()).revision, 8)     # US English

    def test_update_replaces_the_same_voice_only_with_a_newer_revision(self):  # BL-204, ADR-053
        self.put(voice_file(5))
        job = self.update(voices(voice_file(8)))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(packstore.read_voice(self.stored()).revision, 8)
        self.assertIn("r5 to r8", job.step("voice").detail)
        for bundle_voice, words in ((voice_file(8), "already"), (voice_file(7), "newer than the firmware"),
                                    (voice_file(9, locale="es-MX"), "no en-US voice in this firmware")):
            with self.subTest(words=words):
                before = bytes(self.board.flash[ASSETS_AT:ASSETS_AT + 64 * SECTOR])
                self.board.writes.clear()
                job = self.update(voices(bundle_voice))
                self.assertEqual(job.state, "done", job.error)
                self.assertIn(words, job.step("voice").detail)
                self.assertEqual(bytes(self.board.flash[ASSETS_AT:ASSETS_AT + 64 * SECTOR]), before)
                self.assertFalse([o for o, _ in self.board.writes if o >= ASSETS_AT])

    def test_update_installs_the_panel_languages_own_voice(self):  # ADR-053
        # Another voice of the panel's language replaces the one it has (the language's voice now).
        self.put(voice_file(5))
        job = self.update(voices(voice_file(4, voice_id="another"), voice_file(9, locale="es-MX")))
        self.assertEqual(job.state, "done", job.error)
        got = packstore.read_voice(self.stored())
        self.assertEqual((got.locale, got.voice_id, got.revision), ("en-US", "another", 4))
        self.assertIn("replaced by en-US another r4", job.step("voice").detail)
        # A panel installed in Spanish (the store's mark) that has English's voice gets Spanish's.
        self.put_language("es-MX")
        job = self.update(voices(voice_file(8), voice_file(3, locale="es-MX")))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(packstore.read_voice(self.stored()).locale, "es-MX")
        self.assertIn("the es-MX voice", job.step("voice").detail)

    def test_update_keeps_the_voice_of_an_installed_language(self):  # ADR-053
        # Nothing marked as the panel's language, a voice of an installed one: the panel chose it.
        self.put_language("es-MX", default=False)
        self.put(voice_file(2, locale="es-MX"))
        job = self.update(voices(voice_file(8), voice_file(3, locale="es-MX")))
        self.assertEqual(job.state, "done", job.error)
        got = packstore.read_voice(self.stored())
        self.assertEqual((got.locale, got.revision), ("es-MX", 3))
        # A voice of a language not installed (a bench's): English's, the panel's language without a pack.
        self.put(voice_file(2, locale="fr-CA"))
        job = self.update(voices(voice_file(8), voice_file(3, locale="fr-CA")))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(packstore.read_voice(self.stored()).locale, "en-US")

    def test_update_never_writes_a_draft_under_a_release_image(self):  # BL-204
        self.put(voice_file(5))
        job = self.update(voices(voice_file(9, draft=True)), image_with_assets("release"))
        self.assertEqual(job.state, "done", job.error)
        self.assertEqual(packstore.read_voice(self.stored()).revision, 5)


if __name__ == "__main__":
    unittest.main()
