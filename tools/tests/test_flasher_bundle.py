"""FLS-015: firmware bundles are checked before anything can be flashed from them; images carry
their own chip, silicon range and signing key; make_bundle refuses what a panel should not get."""
import hashlib
import importlib.util
import io
import json
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))

from roboflash import bundle as bundle_mod  # noqa: E402
from roboflash import make_bundle, testkit  # noqa: E402
from roboflash.imagefile import parse_app, public_key_digest, key_digest_from_pem, rev_text  # noqa: E402


_SPEC = importlib.util.spec_from_file_location("i18n", Path(__file__).resolve().parents[1] / "i18n.py")
i18n = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(i18n)


def pack_file(locale="fr-CA", version="0.6.0", draft=False, key=testkit.TEST_KEY):
    """A signed-looking .robolang (made-up signature, the CRC right) of the tool's fixture input."""
    cat = i18n.fixture_catalog()
    keys = [i18n.key_hash(c, m) for c, m, _, _ in i18n.FIXTURE_ITEMS]
    payload = i18n.build_pack_payload(locale, i18n.FIXTURE_NAME, cat.rule, draft, i18n.catalog_layout(cat),
                                      keys, version)
    return i18n.unsigned_pack_file(payload)[:-4096] + testkit.signature_sector(key)


def languages(*packs):
    from roboflash.packstore import read_pack
    out = []
    for data in packs:
        i = read_pack(data)
        out.append(bundle_mod.Language(i.locale, i.name, i.version, i.draft, "", "", data))
    return out


def with_languages(images, langs):
    with tempfile.TemporaryDirectory() as d:
        path = bundle_mod.write(Path(d) / "t.roboflash", images, "2026-09-30T00:00:00+00:00",
                                {"commit": "test"}, images[0].signing_key_sha256, langs)
        return path.read_bytes()


def rewrite(data, change):
    """Bundle bytes with change(files: dict name -> bytes) applied."""
    src = zipfile.ZipFile(io.BytesIO(data))
    files = {n: src.read(n) for n in src.namelist()}
    change(files)
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        for n, b in files.items():
            z.writestr(n, b)
    return out.getvalue()


class ImageFileTests(unittest.TestCase):
    def test_header_fields(self):
        app = testkit.synthetic_app("esp32p4", 1, 199, "alarm_crowpanel_p4-dev", "0.6.0")
        a = parse_app(app)
        self.assertEqual((a.chip, a.min_rev, a.max_rev, a.project, a.version),
                         ("esp32p4", 1, 199, "alarm_crowpanel_p4-dev", "0.6.0"))
        self.assertTrue(a.accepts_rev(103))
        self.assertFalse(a.accepts_rev(301))
        self.assertEqual(rev_text(103), "v1.3")
        self.assertEqual(rev_text(301), "v3.1")

    def test_signature_key_matches_the_pem(self):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        n = key.public_key().public_numbers()
        raw = n.n.to_bytes(384, "little") + n.e.to_bytes(4, "little")
        app = testkit.synthetic_app(key=raw)
        with tempfile.TemporaryDirectory() as d:
            pem = Path(d) / "k.pem"
            pem.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                              serialization.NoEncryption()))
            self.assertEqual(parse_app(app).signing_key_sha256, key_digest_from_pem(pem))
            self.assertEqual(key_digest_from_pem(pem), public_key_digest(n.n, n.e))

    def test_only_the_signature_sector_counts(self):
        """FLS-015: the signature is read where espsecure reads it - the image's last 4 KiB, each
        block checked by its CRC. Two 2026-09-24 builds had "E7 02" at a 4 KiB boundary inside the
        app, and the first such bytes were once taken for the signature ("signed with different
        keys")."""
        app = bytearray(testkit.synthetic_app())
        want = public_key_digest(int.from_bytes(testkit.TEST_KEY[:384], "little"),
                                 int.from_bytes(testkit.TEST_KEY[384:], "little"))
        self.assertEqual(parse_app(bytes(app)).signing_key_sha256, want)
        decoy = testkit.signature_sector(bytes(reversed(testkit.TEST_KEY)))
        app[8192:8192 + 4096] = decoy     # a whole valid-looking block, but inside the app
        self.assertEqual(parse_app(bytes(app)).signing_key_sha256, want)
        app[-4096 + 1196] ^= 0xFF         # the real block's CRC broken: no signature at all
        self.assertEqual(parse_app(bytes(app)).signing_key_sha256, "")
        self.assertEqual(parse_app(bytes(app[:-100])).signing_key_sha256, "")   # not whole sectors

    def test_unsigned_and_foreign_images(self):
        self.assertEqual(parse_app(testkit.synthetic_app(signed=False)).signing_key_sha256, "")
        with self.assertRaises(ValueError):
            parse_app(b"\x00" * 512)


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.images = testkit.standard_images()
        self.data = testkit.bundle_bytes(self.images)

    def test_round_trip(self):
        b = bundle_mod.load(self.data, name="x.roboflash")
        self.assertEqual([i.key for i in b.images],
                         ["crowpanel_p4-v1-dev", "crowpanel_p4-v3-dev", "e32r40t-v3-legacy",
                          "crowpanel_10-v1-dev"])
        v1 = b.image("crowpanel_p4-v1-dev")
        self.assertEqual([p.offset for p in v1.parts], [0x2000, 0xC000, 0x2D000, 0x30000])
        self.assertEqual(v1.silicon_text(), "silicon up to v1.99")
        self.assertEqual(b.image("crowpanel_p4-v3-dev").silicon_text(), "silicon v3.1 to v3.99")

    def test_damaged_part_is_refused(self):
        bad = rewrite(self.data, lambda f: f.__setitem__("crowpanel_p4-v1-dev/app.bin",
                                                         f["crowpanel_p4-v1-dev/app.bin"][:-1] + b"\x00"))
        with self.assertRaisesRegex(bundle_mod.BundleError, "damaged"):
            bundle_mod.load(bad)

    def test_a_file_damaged_on_the_way_is_refused_in_words(self):
        """A copy or download that changed the file inside: zipfile finds out only when it reads
        the part, and says so with its own errors. They are refused as damage, never let out:
        one lying next to the Flasher once kept it from starting."""
        raw = bytearray(self.data)
        info = zipfile.ZipFile(io.BytesIO(self.data)).getinfo("crowpanel_p4-v1-dev/app.bin")
        name_len, extra_len = struct.unpack_from("<HH", raw, info.header_offset + 26)
        start = info.header_offset + 30 + name_len + extra_len
        for at in range(start + 100, start + info.compress_size - 100, 997):   # the compressed bytes
            raw[at] ^= 0x5A
        with self.assertRaisesRegex(bundle_mod.BundleError, "damaged"):
            bundle_mod.load(bytes(raw))

    def test_a_manifest_that_is_not_one(self):
        for text in (b"[]", b'"x"', b'{"format": 1, "images": {"a": 1}}', b'{"format": 1, "images": ["x"]}',
                     b'{"format": 1, "images": [{"files": ["x"]}]}'):
            with self.subTest(text=text), self.assertRaises(bundle_mod.BundleError):
                bundle_mod.load(rewrite(self.data, lambda f, t=text: f.__setitem__("manifest.json", t)))

    def test_manifest_must_agree_with_the_app(self):
        def lie(f):
            m = json.loads(f["manifest.json"])
            m["images"][0]["chip_rev_max"] = 399
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "newest silicon"):
            bundle_mod.load(rewrite(self.data, lie))

    def test_unsafe_names_and_missing_parts(self):
        def escape(f):
            m = json.loads(f["manifest.json"])
            m["images"][0]["files"][0]["path"] = "../evil.bin"
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "unsafe"):
            bundle_mod.load(rewrite(self.data, escape))

        def drop(f):
            m = json.loads(f["manifest.json"])
            m["images"][0]["files"].pop()
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "exactly the parts"):
            bundle_mod.load(rewrite(self.data, drop))

    def test_newer_format_and_garbage(self):
        def newer(f):
            m = json.loads(f["manifest.json"])
            m["format"] = 2
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "newer Flasher"):
            bundle_mod.load(rewrite(self.data, newer))
        with self.assertRaises(bundle_mod.BundleError):
            bundle_mod.load(b"not a zip")

    def test_parts_must_fit_the_flash(self):
        def small(f):
            m = json.loads(f["manifest.json"])
            m["images"][0]["flash_size"] = "128KB"
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "past the end"):
            bundle_mod.load(rewrite(self.data, small))


class BundleLanguageTests(unittest.TestCase):
    """I18N-045: a bundle carries language packs, made for its version and signed with its key."""

    def setUp(self):
        self.images = testkit.standard_images()

    def data(self, *packs):
        return with_languages(self.images, languages(*packs))

    def test_round_trip(self):
        es = pack_file("es-US")
        data = self.data(pack_file("fr-CA"), es)
        b = bundle_mod.load(data)
        self.assertEqual(sorted(l.locale for l in b.languages), ["es-US", "fr-CA"])
        lang = b.language("es-US")
        self.assertEqual((lang.name, lang.version, lang.draft, lang.data, lang.path),
                         (i18n.FIXTURE_NAME, "0.6.0", False, es, "lang/es-US.robolang"))
        self.assertEqual(lang.size, len(es))
        listed = b.to_dict()["languages"]
        self.assertEqual({l["locale"] for l in listed}, {"es-US", "fr-CA"})
        self.assertTrue(all("data" not in l for l in listed))
        manifest = json.loads(zipfile.ZipFile(io.BytesIO(data)).read("manifest.json"))
        self.assertEqual(manifest["format"], 1)
        self.assertEqual(set(manifest["languages"][0]), {"locale", "name", "version", "draft", "path",
                                                         "sha256", "size"})

    def test_draft_flag_survives(self):
        b = bundle_mod.load(self.data(pack_file("es-US", draft=True)))
        self.assertTrue(b.language("es-US").draft)

    def test_an_old_bundle_without_languages_loads(self):
        b = bundle_mod.load(testkit.bundle_bytes(self.images))
        self.assertEqual(b.languages, [])
        self.assertEqual(b.to_dict()["languages"], [])

    def test_unknown_manifest_keys_are_ignored(self):
        def add(f):
            m = json.loads(f["manifest.json"])
            m["future"] = 1
            m["languages"][0]["extra"] = "x"
            f["manifest.json"] = json.dumps(m).encode()
        self.assertEqual(len(bundle_mod.load(rewrite(self.data(pack_file()), add)).languages), 1)

    def test_a_pack_signed_with_another_key_is_refused(self):
        other = pack_file(key=bytes(reversed(testkit.TEST_KEY)))
        with self.assertRaisesRegex(bundle_mod.BundleError, "signed with another key than the firmware"):
            bundle_mod.load(self.data(other))
        unsigned = i18n.unsigned_pack_file(i18n.fixture_payload()[0])
        with self.assertRaisesRegex(bundle_mod.BundleError, "another key"):
            bundle_mod.load(self.data(unsigned))

    def test_a_manifest_that_lies_about_version_locale_or_draft(self):
        data = self.data(pack_file("fr-CA"))
        for key, value, words in (("version", "0.5.0", "version"), ("locale", "es-US", "locale"),
                                  ("draft", True, "draft")):
            def lie(f, key=key, value=value):
                m = json.loads(f["manifest.json"])
                m["languages"][0][key] = value
                f["manifest.json"] = json.dumps(m).encode()
            with self.assertRaisesRegex(bundle_mod.BundleError, words):
                bundle_mod.load(rewrite(data, lie))

    def test_a_changed_byte_a_missing_member_and_an_unsafe_path(self):
        data = self.data(pack_file())

        def flip(f):
            b = bytearray(f["lang/fr-CA.robolang"])
            b[200] ^= 1
            f["lang/fr-CA.robolang"] = bytes(b)
        with self.assertRaisesRegex(bundle_mod.BundleError, "damaged"):
            bundle_mod.load(rewrite(data, flip))
        with self.assertRaisesRegex(bundle_mod.BundleError, "missing"):
            bundle_mod.load(rewrite(data, lambda f: f.pop("lang/fr-CA.robolang")))

        def unsafe(f):
            m = json.loads(f["manifest.json"])
            m["languages"][0]["path"] = "../x.robolang"
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "unsafe"):
            bundle_mod.load(rewrite(data, unsafe))

    def test_a_file_that_is_not_a_pack_is_refused(self):
        def swap(f):
            junk = b"x" * 8192
            f["lang/fr-CA.robolang"] = junk
            m = json.loads(f["manifest.json"])
            m["languages"][0].update(sha256=hashlib.sha256(junk).hexdigest(), size=len(junk))
            f["manifest.json"] = json.dumps(m).encode()
        with self.assertRaisesRegex(bundle_mod.BundleError, "language pack"):
            bundle_mod.load(rewrite(self.data(pack_file()), swap))


class OpenFileTests(unittest.TestCase):
    """A file opened by hand (Advanced tools > Software) is checked whole before it is used: a
    damaged one is refused and what was in use stays (FLS-015). The Flasher carries no bundle of
    its own: its firmware comes from the firmware server (test_flasher_server.py)."""

    def test_a_damaged_file_is_refused(self):
        from roboflash.controller import Controller
        from roboflash.jobs import JobFailed
        data = testkit.bundle_bytes(testkit.standard_images())
        damaged = bytearray(data)
        for at in range(2000, len(damaged) // 2, 1009):
            damaged[at] ^= 0x5A
        with tempfile.TemporaryDirectory() as root:
            c = Controller(lister=lambda: [], backups_dir=root, captures_dir=root, logs_dir=root)
            c.load_bundle_bytes(data, "opened.roboflash")
            with self.assertRaises(JobFailed):
                c.load_bundle_bytes(bytes(damaged), "damaged.roboflash")
            self.assertEqual(c.snapshot()["bundle"]["name"], "opened.roboflash")
            self.assertIn("damaged.roboflash", c.snapshot()["bundle_error"])


class MakeBundleTests(unittest.TestCase):
    """make_bundle reads a build folder the way ESP-IDF leaves it."""

    def build_dir(self, root, image, sdk=None):
        d = Path(root) / image.key
        (d / "config").mkdir(parents=True)
        names = {"bootloader": "bootloader/bootloader.bin", "partition-table": "partition_table/partition-table.bin",
                 "otadata": "ota_data_initial.bin", "app": f"{image.project}.bin"}
        fa = {"flash_settings": {"flash_mode": "dio", "flash_freq": "80m", "flash_size": "keep"},
              "extra_esptool_args": {"chip": image.chip}}
        for p in image.parts:
            path = d / names[p.role]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(p.data)
            fa[p.role] = {"offset": hex(p.offset), "file": names[p.role]}
        (d / "flasher_args.json").write_text(json.dumps(fa))
        (d / "config" / "sdkconfig.json").write_text(json.dumps(sdk or {"ESPTOOLPY_FLASHSIZE": image.flash_size,
                                                                          "ALARM_BUILD_DEV": True}))
        return d

    def test_bundle_from_build_folders(self):
        with tempfile.TemporaryDirectory() as root:
            dirs = [self.build_dir(root, i) for i in testkit.standard_images()[:2]]
            out = Path(root) / "out.roboflash"
            make_bundle.make(dirs, out=out, key=None, log=lambda t: None)
            b = bundle_mod.load(out)
            self.assertEqual({i.key for i in b.images}, {"crowpanel_p4-v1-dev", "crowpanel_p4-v3-dev"})
            self.assertEqual(b.image("crowpanel_p4-v1-dev").flash_mode, "dio")

    def test_packs_from_the_first_build_folder_go_into_the_bundle(self):
        with tempfile.TemporaryDirectory() as root:
            dirs = [self.build_dir(root, i) for i in testkit.standard_images()[:2]]
            (dirs[0] / "lang").mkdir()
            (dirs[0] / "lang" / "fr-CA.robolang").write_bytes(pack_file("fr-CA"))
            (dirs[0] / "lang" / "es-US.robolang").write_bytes(pack_file("es-US", draft=True))
            out = Path(root) / "out.roboflash"
            notes = []
            make_bundle.make(dirs, out=out, key=None, log=notes.append)
            b = bundle_mod.load(out)
            self.assertEqual([l.locale for l in b.languages], ["es-US", "fr-CA"])
            self.assertTrue(b.language("es-US").draft)
            self.assertTrue(any("language es-US" in n for n in notes))

    def test_no_packs_is_a_note_not_an_error(self):
        with tempfile.TemporaryDirectory() as root:
            dirs = [self.build_dir(root, testkit.standard_images()[0])]
            notes = []
            make_bundle.make(dirs, out=Path(root) / "o.roboflash", key=None, log=notes.append)
            self.assertEqual(bundle_mod.load(Path(root) / "o.roboflash").languages, [])
            self.assertTrue(any("no language packs" in n for n in notes))

    def test_packs_for_another_version_or_key_are_refused(self):
        for pack, words in ((pack_file(version="0.5.0"), "made for version 0.5.0"),
                            (pack_file(key=bytes(reversed(testkit.TEST_KEY))), "another key")):
            with tempfile.TemporaryDirectory() as root:
                d = self.build_dir(root, testkit.standard_images()[0])
                (d / "lang").mkdir()
                (d / "lang" / "fr-CA.robolang").write_bytes(pack)
                with self.assertRaisesRegex(bundle_mod.BundleError, words):
                    make_bundle.make([d], out=Path(root) / "o.roboflash", key=None, log=lambda t: None)

    def test_a_board_the_flasher_does_not_offer_is_not_bundled(self):
        """The unfinished 4" (docs/features/19): a build of it never becomes a bundle image."""
        with tempfile.TemporaryDirectory() as root:
            e32 = testkit.standard_images()[2]
            self.assertEqual(e32.board, "e32r40t")
            with self.assertRaisesRegex(bundle_mod.BundleError, "does not offer 'e32r40t'.*not finished"):
                make_bundle.image_from_build(self.build_dir(root, e32, {"ESPTOOLPY_FLASHSIZE": "4MB"}))

    def test_dark_unsigned_and_foreign_builds_are_refused(self):
        with tempfile.TemporaryDirectory() as root:
            image = testkit.standard_images()[0]
            dark = self.build_dir(root, image, {"ESPTOOLPY_FLASHSIZE": "16MB", "BOARD_BACKLIGHT_FORCE_OFF": True})
            with self.assertRaisesRegex(bundle_mod.BundleError, "dark build"):
                make_bundle.image_from_build(dark)
        with tempfile.TemporaryDirectory() as root:
            image = testkit.synthetic_image()
            app = next(p for p in image.parts if p.role == "app")
            app.data = testkit.synthetic_app(signed=False)
            with self.assertRaisesRegex(bundle_mod.BundleError, "not signed"):
                make_bundle.image_from_build(self.build_dir(root, image))
        with tempfile.TemporaryDirectory() as root:
            from cryptography.hazmat.primitives import serialization
            from cryptography.hazmat.primitives.asymmetric import rsa
            owner = Path(root) / "owner.pem"
            owner.write_bytes(rsa.generate_private_key(public_exponent=65537, key_size=3072).private_bytes(
                serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
            d = self.build_dir(root, testkit.standard_images()[0])
            with self.assertRaisesRegex(bundle_mod.BundleError, "owner's key"):
                make_bundle.make([d], out=Path(root) / "x.roboflash", key=owner, log=lambda t: None)


def synthetic_radio(kind="zigbee", version="1.0.0", project="roboalarms_h2_rcp", chip="esp32h2"):
    """The Zigbee module's software as make_bundle reads it: three parts, the app unsigned."""
    app = testkit.synthetic_app(chip, 0, 199, project, version, 64 * 1024, signed=False, seed=11)
    pieces = {"bootloader": b"\xe9" + bytes(16 * 1024 - 1), "partition-table": b"\xaa\x50" + bytes(3070),
              "app": app}
    offsets = bundle_mod.RADIO_KINDS["zigbee"]["parts"]
    parts = [bundle_mod.Part(role, offsets[role], "", "", data) for role, data in pieces.items()]
    a = parse_app(app)
    return bundle_mod.Radio(kind=kind, chip=chip, project=project, version=version, elf_sha256=a.elf_sha256,
                            chip_rev_min=0, chip_rev_max=199, flash_size="2MB", flash_mode="dio",
                            flash_freq="48m", parts=parts,
                            rcp_version="openthread-esp32/0000000-0000000; esp32h2;  2026-10-01 00:00:00 UTC",
                            source={"commit": "abc1234", "dirty": False})


def with_radios(images, radios):
    with tempfile.TemporaryDirectory() as d:
        path = bundle_mod.write(Path(d) / "t.roboflash", images, "2026-10-01T00:00:00+00:00",
                                {"commit": "test"}, images[0].signing_key_sha256, (), radios)
        return path.read_bytes()


def edit_manifest(data, change):
    def apply(f):
        m = json.loads(f["manifest.json"])
        change(m)
        f["manifest.json"] = json.dumps(m).encode()
    return rewrite(data, apply)


class BundleRadioTests(unittest.TestCase):
    """FLS-015: a bundle may carry the Zigbee module's software (firmware/radios/h2_rcp) in its own
    list, unsigned, each part checked by SHA-256, with its version and source - and never as a
    panel image."""

    def setUp(self):
        self.images = testkit.standard_images()
        self.data = with_radios(self.images, [synthetic_radio()])

    def test_round_trip(self):
        b = bundle_mod.load(self.data)
        r = b.radio("zigbee")
        self.assertIsNotNone(r)
        self.assertIsNone(b.radio("thread"))
        self.assertEqual((r.chip, r.project, r.version, r.chip_rev_min, r.chip_rev_max, r.flash_size,
                          r.flash_mode, r.flash_freq, r.source),
                         ("esp32h2", "roboalarms_h2_rcp", "1.0.0", 0, 199, "2MB", "dio", "48m",
                          {"commit": "abc1234", "dirty": False}))
        self.assertTrue(r.rcp_version.startswith("openthread-esp32/"))
        self.assertEqual([(p.role, p.offset) for p in r.parts],
                         [("bootloader", 0x0), ("partition-table", 0x8000), ("app", 0x10000)])
        want = synthetic_radio()
        self.assertEqual([p.data for p in r.parts], [p.data for p in want.parts])
        manifest = json.loads(zipfile.ZipFile(io.BytesIO(self.data)).read("manifest.json"))
        self.assertEqual(manifest["format"], 1)    # the format number stays: older Flashers ignore the key
        entry = manifest["radios"][0]
        self.assertEqual(set(entry), {"kind", "chip", "project", "version", "elf_sha256", "chip_rev_min",
                                      "chip_rev_max", "flash_size", "flash_mode", "flash_freq", "source",
                                      "rcp_version", "parts"})
        self.assertEqual([(f["role"], f["offset"], f["path"]) for f in entry["parts"]],
                         [("bootloader", "0x0", "radios/zigbee/bootloader.bin"),
                          ("partition-table", "0x8000", "radios/zigbee/partition-table.bin"),
                          ("app", "0x10000", "radios/zigbee/app.bin")])
        listed = b.to_dict()["radios"]
        self.assertEqual([(x["kind"], x["version"], x["chip"]) for x in listed], [("zigbee", "1.0.0", "esp32h2")])
        self.assertTrue(all("data" not in x and "parts" not in x for x in listed))

    def test_without_rcp_version_the_key_is_left_out(self):
        radio = synthetic_radio()
        radio.rcp_version = ""
        data = with_radios(self.images, [radio])
        self.assertNotIn("rcp_version", json.loads(zipfile.ZipFile(io.BytesIO(data)).read("manifest.json"))["radios"][0])
        self.assertEqual(bundle_mod.load(data).radio("zigbee").rcp_version, "")

    def test_an_old_bundle_without_radios_loads(self):
        b = bundle_mod.load(testkit.bundle_bytes(self.images))
        self.assertEqual(b.radios, [])
        self.assertIsNone(b.radio("zigbee"))
        self.assertEqual(b.to_dict()["radios"], [])

    def test_an_older_reader_sees_only_its_images(self):
        """What a Flasher without radios reads: `images` and `languages` alone, unchanged."""
        manifest = json.loads(zipfile.ZipFile(io.BytesIO(self.data)).read("manifest.json"))
        old = json.loads(zipfile.ZipFile(io.BytesIO(testkit.bundle_bytes(self.images))).read("manifest.json"))
        self.assertEqual(manifest["images"], old["images"])
        no_radios = edit_manifest(self.data, lambda m: m.pop("radios"))
        self.assertEqual([i.key for i in bundle_mod.load(no_radios).images],
                         [i.key for i in bundle_mod.load(self.data).images])

    def test_an_unknown_kind_is_skipped(self):
        def add(m):
            other = json.loads(json.dumps(m["radios"][0]))
            other["kind"] = "matter-someday"
            other["parts"] = [{"role": "app", "offset": "0x0", "path": "radios/x/app.bin", "sha256": "0" * 64}]
            m["radios"].insert(0, other)
        with self.assertLogs("roboflash.bundle", "WARNING") as logs:
            b = bundle_mod.load(edit_manifest(self.data, add))
        self.assertEqual([r.kind for r in b.radios], ["zigbee"])
        self.assertIn("matter-someday", logs.output[0])

    def test_another_layout_is_skipped_and_the_images_kept(self):
        """A newer Flasher's radio layout (other parts, or a part moved): the radio is skipped,
        the panel images load (FLS-015)."""
        def extra(m):
            m["radios"][0]["parts"].append(dict(m["radios"][0]["parts"][2], role="ota-data"))
        for change in (lambda m: m["radios"][0]["parts"].pop(),
                       lambda m: m["radios"][0]["parts"][0].update(offset="0x2000"), extra):
            with self.subTest(change=change), self.assertLogs("roboflash.bundle", "WARNING") as logs:
                b = bundle_mod.load(edit_manifest(self.data, change))
            self.assertEqual(b.radios, [])
            self.assertEqual([i.key for i in b.images], [i.key for i in self.images])
            self.assertIn("zigbee radio's software", logs.output[0])

    def test_a_wrong_checksum_or_missing_part_is_refused(self):
        def flip(f):
            raw = bytearray(f["radios/zigbee/app.bin"])
            raw[300] ^= 1
            f["radios/zigbee/app.bin"] = bytes(raw)
        with self.assertRaisesRegex(bundle_mod.BundleError, "damaged"):
            bundle_mod.load(rewrite(self.data, flip))

        def wrong_sha(m):
            m["radios"][0]["parts"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(bundle_mod.BundleError, "radios/zigbee/bootloader.bin is damaged"):
            bundle_mod.load(edit_manifest(self.data, wrong_sha))
        with self.assertRaisesRegex(bundle_mod.BundleError, "missing"):
            bundle_mod.load(rewrite(self.data, lambda f: f.pop("radios/zigbee/partition-table.bin")))

    def test_a_manifest_that_lies_or_is_malformed_is_refused(self):
        cases = (
            (lambda m: m["radios"][0].update(version="9.9.9"), "version"),
            (lambda m: m["radios"][0].update(chip_rev_max=299), "newest silicon"),
            (lambda m: m["radios"][0].update(chip="esp32c6"), "chip"),
            (lambda m: m["radios"][0].update(project="alarm_crowpanel_p4-dev"), "project"),
            (lambda m: m["radios"][0].update(parts={"app": 1}), "without its parts"),
            (lambda m: m["radios"][0]["parts"][0].update(offset="soon"), "bootloader offset"),
            (lambda m: m["radios"][0]["parts"][2].update(path="../app.bin"), "unsafe"),
            (lambda m: m["radios"][0].update(flash_size="64KB"), "past the end"),
            (lambda m: m["radios"][0].pop("elf_sha256"), "without elf_sha256"),
            (lambda m: m.update(radios={"zigbee": 1}), "radios properly"),
            (lambda m: m["radios"].append(json.loads(json.dumps(m["radios"][0]))), "twice"),
        )
        for change, words in cases:
            with self.subTest(words=words), self.assertRaisesRegex(bundle_mod.BundleError, words):
                bundle_mod.load(edit_manifest(self.data, change))

    def test_radios_never_reach_the_panel_images(self):
        b = bundle_mod.load(self.data)
        plain = bundle_mod.load(testkit.bundle_bytes(self.images))
        self.assertEqual(b.version_text(), plain.version_text())
        self.assertNotIn("1.0.0", b.version_text())
        self.assertEqual([i.key for i in b.images], [i.key for i in plain.images])
        self.assertTrue(all(i.chip != "esp32h2" for i in b.images))
        self.assertIsNone(b.image("zigbee"))
        from roboflash import boards
        for board in ("crowpanel_p4", "zigbee", "esp32h2", "roboalarms_h2_rcp"):
            for rev in (None, 0, 101, 199, 301):
                with self.subTest(board=board, rev=rev):
                    self.assertEqual(boards.pick_image(b, board, rev), boards.pick_image(plain, board, rev))
        self.assertEqual(boards.pick_image(b, "zigbee", 0), "")
        d = b.to_dict()
        self.assertEqual([i["key"] for i in d["images"]], [i.key for i in plain.images])

    def test_boards_json_agrees_with_the_bundle(self):
        board = json.loads((Path(__file__).resolve().parents[1] / "flasher" / "roboflash" / "boards.json")
                           .read_text(encoding="utf-8"))["boards"]["crowpanel_p4"]
        radio = board["zigbee"]["radio"]
        want = bundle_mod.RADIO_KINDS["zigbee"]
        self.assertEqual((radio["chip"], radio["project"]), (want["chip"], want["project"]))
        self.assertEqual({k: int(v, 0) for k, v in radio["parts"].items()}, want["parts"])


class MakeBundleRadioTests(unittest.TestCase):
    """make_bundle --radio: only the h2_rcp project for the H2, only from clean source unless told."""

    def radio_dir(self, root, radio=None, project=None, target=None, flasher_args=True):
        radio = radio or synthetic_radio()
        d = Path(root) / "h2_rcp"
        (d / "config").mkdir(parents=True)
        names = {"bootloader": "bootloader/bootloader.bin", "partition-table": "partition_table/partition-table.bin",
                 "app": "roboalarms_h2_rcp.bin"}
        for p in radio.parts:
            path = d / names[p.role]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(p.data)
        if flasher_args:
            fa = {"flash_settings": {"flash_mode": "dio", "flash_size": "2MB", "flash_freq": "48m"},
                  "extra_esptool_args": {"chip": "esp32h2"}}
            for p in radio.parts:
                fa[p.role] = {"offset": hex(p.offset), "file": names[p.role]}
            (d / "flasher_args.json").write_text(json.dumps(fa))
        else:
            (d / "flash_args").write_text("--flash-mode dio --flash-freq 48m --flash-size 2MB\n" + "".join(
                f"{p.offset:#x} {names[p.role]}\n" for p in radio.parts))
        (d / "project_description.json").write_text(json.dumps({
            "project_name": project or "roboalarms_h2_rcp", "target": target or "esp32h2",
            "app_bin": "roboalarms_h2_rcp.bin", "project_path": str(Path(root) / "src")}))
        (d / "config" / "sdkconfig.json").write_text(json.dumps({
            "ESP_REV_MIN_FULL": 0, "ESP_REV_MAX_FULL": 199, "ESPTOOLPY_FLASHSIZE": "2MB",
            "ESPTOOLPY_FLASHMODE": "dio", "ESPTOOLPY_FLASHFREQ": "48m"}))
        (d / "rcp_version").write_text(radio.rcp_version)
        return d

    def source(self, dirty):
        from unittest import mock
        return mock.patch.object(make_bundle, "_radio_source", return_value={"commit": "abc1234", "dirty": dirty})

    def test_bundle_with_the_radio(self):
        with tempfile.TemporaryDirectory() as root:
            image_dir = MakeBundleTests.build_dir(None, root, testkit.standard_images()[0])
            out = Path(root) / "out.roboflash"
            notes = []
            with self.source(False):
                make_bundle.make([image_dir], out=out, key=None, log=notes.append,
                                 radio_dirs=[self.radio_dir(root)])
            r = bundle_mod.load(out).radio("zigbee")
            self.assertEqual((r.version, r.source, r.flash_freq), ("1.0.0", {"commit": "abc1234", "dirty": False}, "48m"))
            self.assertEqual(r.rcp_version, synthetic_radio().rcp_version)
            self.assertTrue(any("zigbee radio" in n and "unsigned" in n for n in notes))

    def test_flash_args_alone_will_do(self):
        with tempfile.TemporaryDirectory() as root, self.source(False):
            r = make_bundle.radio_from_build(self.radio_dir(root, flasher_args=False))
            self.assertEqual([(p.role, p.offset) for p in r.parts],
                             [("bootloader", 0), ("partition-table", 0x8000), ("app", 0x10000)])

    def test_a_dirty_radio_build_is_refused_without_the_flag(self):
        with tempfile.TemporaryDirectory() as root, self.source(True):
            d = self.radio_dir(root)
            with self.assertRaisesRegex(bundle_mod.BundleError, "not committed.*--allow-dirty-radio"):
                make_bundle.radio_from_build(d)
            notes = []
            r = make_bundle.radio_from_build(d, allow_dirty=True, log=notes.append)
            self.assertEqual(r.source, {"commit": "abc1234", "dirty": True})
            self.assertTrue(notes and notes[0].startswith("warning:"))
        from unittest import mock
        with tempfile.TemporaryDirectory() as root, \
                mock.patch.object(make_bundle, "_radio_source", return_value={}):
            with self.assertRaisesRegex(bundle_mod.BundleError, "can't tell"):
                make_bundle.radio_from_build(self.radio_dir(root))

    def test_the_dirty_check_looks_at_the_radios_folder_but_not_its_build(self):
        import subprocess
        with tempfile.TemporaryDirectory() as root:
            repo, src = Path(root), Path(root) / "radio"
            src.mkdir()
            git = ["git", "-c", "user.name=Alex", "-c", "user.email=alex@example.com", "-c", "commit.gpgsign=false"]
            subprocess.run(git + ["init", "-q"], cwd=repo, check=True)
            (src / ".gitignore").write_text("build/\n")
            (src / "main.c").write_text("int x;\n")
            subprocess.run(git + ["add", "."], cwd=repo, check=True)
            subprocess.run(git + ["commit", "-q", "-m", "x"], cwd=repo, check=True)
            radio_commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=repo, check=True,
                                          capture_output=True, text=True).stdout.strip()
            (repo / "panel.c").write_text("int p;\n")   # a later commit that leaves the radio alone
            subprocess.run(git + ["add", "."], cwd=repo, check=True)
            subprocess.run(git + ["commit", "-q", "-m", "y"], cwd=repo, check=True)
            (src / "build").mkdir()
            (src / "build" / "out.bin").write_bytes(b"x")
            self.assertEqual(make_bundle._radio_source(src), {"commit": radio_commit, "dirty": False})
            (src / "main.c").write_text("int y;\n")
            self.assertEqual(make_bundle._radio_source(src), {"commit": radio_commit, "dirty": True})

    def test_a_wrong_project_or_chip_is_refused(self):
        for kwargs, words in (({"project": "alarm_crowpanel_p4"}, "not the zigbee radio's software"),
                              ({"target": "esp32c6"}, "built for 'esp32c6'"),
                              ({"radio": synthetic_radio(project="ot_rcp")}, "app says ot_rcp")):
            with self.subTest(words=words), tempfile.TemporaryDirectory() as root, self.source(False):
                with self.assertRaisesRegex(bundle_mod.BundleError, words):
                    make_bundle.radio_from_build(self.radio_dir(root, **kwargs))

    def test_a_part_at_another_offset_is_refused(self):
        with tempfile.TemporaryDirectory() as root, self.source(False):
            d = self.radio_dir(root)
            fa = json.loads((d / "flasher_args.json").read_text())
            fa["app"]["offset"] = "0x20000"
            (d / "flasher_args.json").write_text(json.dumps(fa))
            with self.assertRaisesRegex(bundle_mod.BundleError, "app at 0x20000"):
                make_bundle.radio_from_build(d)


def move_part(data, role, offset, key="crowpanel_p4-v1-dev"):
    """Bundle bytes with one image's part moved in the manifest (its bytes and checksum unchanged)."""
    def change(f):
        m = json.loads(f["manifest.json"])
        image = next(i for i in m["images"] if f"{i['board']}-{i['variant']}-{i['profile']}" == key)
        next(p for p in image["files"] if p["role"] == role)["offset"] = offset
        f["manifest.json"] = json.dumps(m).encode()
    return rewrite(data, change)


class LayoutTests(unittest.TestCase):
    """FLS2-002 (FLS-015, FLS-008): every part goes where the chip and the image's own partition
    table put it, checked when the bundle opens, before anything can be written."""

    def setUp(self):
        self.data = testkit.bundle_bytes(testkit.standard_images())

    def test_normal_bundles_open(self):  # FLS2-002: the 7", 10.1" and the 4" layouts still load
        b = bundle_mod.load(self.data)
        for image in b.images:
            bundle_mod.check_layout(image)
        self.assertEqual([p.offset for p in b.image("e32r40t-v3-legacy").parts], [0x1000, 0xC000, 0x1D000, 0x20000])

    def test_otadata_moved_onto_the_settings_is_refused(self):  # FLS2-002: the saved NVS-offset case
        bad = move_part(self.data, "otadata", "0xd000")
        with self.assertRaisesRegex(bundle_mod.BundleError, r"otadata.*0xd000.*0x2d000"):
            bundle_mod.load(bad)

    def test_parts_away_from_their_place_are_refused(self):  # FLS2-002
        for role, offset, words in (
                ("app", "-0x30000", "app.*negative|app.*before"),
                ("app", "-0x1000", "negative|before"),
                ("app", "0x31000", "app.*0x31000.*0x30000"),            # inside ota_0, not at its start
                ("app", "0x4b0000", "app.*0x4b0000"),                   # ota_1, not the app the panel boots
                ("app", "0xd000", "app.*0xd000"),                       # a data partition
                ("app", "0x10000000", "past the end|0x10000000"),       # beyond 16 MB
                ("otadata", "0x2e000", "otadata.*0x2e000"),             # overlaps phy_init
                ("otadata", "0x2d800", "otadata.*0x2d800"),             # not on a sector
                ("bootloader", "0x0", "bootloader.*0x0.*0x2000"),       # the P4's ROM reads it at 0x2000
                ("partition-table", "0x4000", "overlap|partition-table"),   # over the bootloader
                ("partition-table", "0xc800", "partition-table.*0xc800")):
            with self.subTest(role=role, offset=offset):
                with self.assertRaisesRegex(bundle_mod.BundleError, words):
                    bundle_mod.load(move_part(self.data, role, offset))

    def table_image(self, table, **kw):
        return testkit.synthetic_image(table=table, **kw)

    def test_a_partition_table_that_does_not_hold_together_is_refused(self):  # FLS2-002
        p4 = list(testkit.P4_TABLE)
        beyond = p4[:-1] + [("coredump", 1, 3, 0xFF8000 + 0x8000, 0x10000)]      # past 16 MB
        overlapping = p4[:2] + [("phy_init", 1, 1, 0x2E000, 0x2000)] + p4[3:]   # over otadata
        no_otadata = [p for p in p4 if p[0] != "otadata"]
        small_app = [("ota_0", 0, 0x10, 0x30000, 0x10000) if p[0] == "ota_0" else p for p in p4]
        unaligned_app = [("ota_0", 0, 0x10, 0x31000, 0x47F000) if p[0] == "ota_0" else p for p in p4]
        below_table = [("nvs", 1, 2, 0xC000, 0x21000)] + p4[1:]                 # over the table itself
        for table, words in ((beyond, "past the end"), (overlapping, "overlap"), (no_otadata, "no otadata"),
                             (small_app, "does not fit"), (unaligned_app, "64 KB|0x31000"),
                             (below_table, "partition table")):
            with self.subTest(words=words), self.assertRaisesRegex(bundle_mod.BundleError, words):
                bundle_mod.load(testkit.bundle_bytes([self.table_image(table)]))

    def test_a_damaged_or_foreign_table_is_refused(self):  # FLS2-002
        image = testkit.synthetic_image()
        pt = image.part("partition-table")
        for data, words in ((bytes(3072), "not an ESP-IDF partition table"),
                            (b"\xff" * 3072, "no partitions"),
                            (pt.data[:40] + bytes([pt.data[40] ^ 1]) + pt.data[41:], "MD5"),
                            (pt.data + b"\xff" * 4096, "larger")):
            with self.subTest(words=words):
                pt.data = data
                pt.sha256 = hashlib.sha256(data).hexdigest()
                image.partition_table_sha256 = pt.sha256
                with self.assertRaisesRegex(bundle_mod.BundleError, words):
                    bundle_mod.check_layout(image)

    def test_image_from_build_checks_the_layout_too(self):  # FLS2-002: --build installs skip the bundle
        with tempfile.TemporaryDirectory() as root:
            d = MakeBundleTests.build_dir(None, root, testkit.standard_images()[0])
            make_bundle.image_from_build(d)            # as built: accepted
            fa = json.loads((d / "flasher_args.json").read_text())
            fa["otadata"]["offset"] = "0xd000"
            (d / "flasher_args.json").write_text(json.dumps(fa))
            with self.assertRaisesRegex(bundle_mod.BundleError, "otadata"):
                make_bundle.image_from_build(d)


if __name__ == "__main__":
    unittest.main()
