"""DEV-003/004/006/007/014: identity, artifact and reconnect regression tests."""
import importlib.util
import contextlib
import io
import json
from pathlib import Path
import ssl
import sys
import struct
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("panelctl", Path(__file__).resolve().parents[1] / "panelctl.py")
p = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(p)


class ToolTests(unittest.TestCase):
    def test_wireless_enrollment_preserves_key_and_checks_panel_identity(self):
        from types import SimpleNamespace
        from cryptography import x509
        from cryptography.hazmat.primitives import serialization
        with tempfile.TemporaryDirectory() as d, patch.object(p, 'REGISTRY', Path(d)), contextlib.redirect_stdout(io.StringIO()):
            args = SimpleNamespace(name='remote-panel')
            p.prepare_network_enrollment(args)
            pending = Path(d) / 'remote-panel.pending.json'
            local = json.loads(pending.read_text())
            key_before = (Path(d) / local['key']).read_bytes()
            with self.assertRaises(RuntimeError):
                p.prepare_network_enrollment(args)
            cert = x509.load_pem_x509_certificate((Path(d) / local['cert']).read_bytes())
            receipt = Path(d) / 'receipt.json'
            receipt.write_text(json.dumps({'id':'0123456789ab','host':'192.0.2.10','port':6055,
                                          'certificate_der':cert.public_bytes(serialization.Encoding.DER).hex()}))
            args.file, args.host = str(receipt), None
            class Response:
                status = 200
                identity = 'badidentity'
                def read(self): return json.dumps({'id':self.identity}).encode()
            class Connection:
                def __init__(self, *a): pass
                def request(self, *a): pass
                def close(self): pass
                def getresponse(self): return Response()
            with patch.object(p, 'PinnedConnection', Connection):
                with self.assertRaises(RuntimeError):
                    p.import_network_enrollment(args)
                self.assertTrue(pending.exists())
                self.assertFalse((Path(d) / 'remote-panel.json').exists())
                Response.identity = '0123456789ab'
                p.import_network_enrollment(args)
            self.assertEqual((Path(d) / local['key']).read_bytes(), key_before)
            self.assertEqual(json.loads((Path(d) / 'remote-panel.json').read_text())['id'], '0123456789ab')
            self.assertFalse(pending.exists())

    def test_alias_cannot_escape_registry(self):
        for value in ("../panel", "a/b", "C:\\key", ".", "", "a" * 49):
            with self.assertRaises(ValueError): p.alias_path(value)

    def test_valid_alias(self):
        self.assertEqual(p.alias_path("bench-2").name, "bench-2.json")

    def test_image_offsets(self):
        b = bytearray(288)
        b[0] = 0xE9
        struct.pack_into("<I", b, 32, 0xABCD5432)
        struct.pack_into("<H", b, 15, 100)
        struct.pack_into("<H", b, 17, 199)
        b[80:80+21] = b"alarm_crowpanel_p4-dev"
        b[176:208] = bytes(range(32))
        info = p.image_info(b)
        self.assertEqual(info["project"], "alarm_crowpanel_p4-dev")
        self.assertEqual(info["elf"], bytes(range(32)).hex())
        self.assertEqual((info["min_revision"], info["max_revision"]), (100, 199))

    def test_reject_non_app(self):
        for blob in (b"", bytes(287), bytes(1000)):
            with self.assertRaises(ValueError): p.image_info(blob)

    def test_screenshot_requires_complete_frame(self):
        with self.assertRaises(ValueError): p.png565(b"\0", 1, 1)

    def test_png_encodes_primary_colours(self):
        import zlib
        png = p.png565(struct.pack("<HHH", 0xF800, 0x07E0, 0x001F), 3, 1)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        at = png.index(b"IDAT")
        size = struct.unpack_from(">I", png, at-4)[0]
        self.assertEqual(zlib.decompress(png[at+4:at+4+size]), b"\0\xff\0\0\0\xff\0\0\0\xff")

    def test_pin_is_checked_before_http(self):
        class Sock:
            def getpeercert(self, binary_form): return b"impostor"
            def close(self): pass
        c = object.__new__(p.PinnedConnection)
        c.fingerprint = "0" * 64
        c.sock = Sock()
        c._HTTPConnection__response = None
        with patch.object(p.http.client.HTTPSConnection, "connect"), patch.object(c, "close") as closed:
            with self.assertRaises(ssl.SSLError): c.connect()
            closed.assert_called_once()

    def test_device_id_is_checked_even_with_valid_pin(self):
        c = object.__new__(p.Client)
        c.device = {"id": "expected"}
        c.conn = None
        c.request = lambda *_: b'{"id":"another","api":1}'
        with self.assertRaisesRegex(RuntimeError, "Wrong device"): c.info()

    def test_ambiguous_selection_does_not_connect(self):
        with tempfile.TemporaryDirectory() as d, patch.object(p, "REGISTRY", Path(d)):
            for name in ("one", "two"):
                p.save(Path(d)/(name+".json"), {"fingerprint": "x", "host": "example"})
            class Args: device = None; host = None
            with self.assertRaisesRegex(RuntimeError, "Select one"): p.target(Args())

    def test_same_boot_is_not_success_and_confirmation_waits_for_valid(self):
        op = {"old_boot":"old", "elf":"newelf", "operation":"op"}
        states = iter([
            {"boot":"old"},
            {"boot":"new", "elf":"newelf", "operation":"op", "confirm_required":True, "probation":True},
            {"boot":"new", "elf":"newelf", "operation":"op", "confirm_required":False, "probation":False}])
        class Fake:
            name="bench"
            def info(self): return next(states)
            def request(self,*args): self.call=args
        c=Fake()
        with tempfile.TemporaryDirectory() as d, patch.object(p,"REGISTRY",Path(d)), patch.object(p.time,"sleep"):
            result=p.wait_update(c,op)
            self.assertFalse(result["probation"])
            self.assertEqual(c.call,("POST","confirm","op new"))
            self.assertEqual(json.loads((Path(d)/"bench.operation.json").read_text())["result"],"confirmed")

    def test_speaker_posts_then_reads_back(self):
        # AUD-025: the bench silence is set, then the panel's own answer is what is reported.
        class Fake:
            device = {"id": "0123456789ab"}
            def __init__(self): self.calls = []
            def request(self, method, endpoint, data=None, headers=None):
                self.calls.append((method, endpoint, data))
                return b"off\n" if method == "GET" else b"off"
        c = Fake()
        self.assertEqual(p.speaker(c, "off"), {"speaker": "off", "id": "0123456789ab"})
        self.assertEqual(c.calls, [("POST", "speaker", "off"), ("GET", "speaker", None)])
        c.calls.clear()
        p.speaker(c)
        self.assertEqual(c.calls, [("GET", "speaker", None)])

    def test_speaker_missing_route_is_explained(self):
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, *a, **k): raise p.DeviceError(404, "Not Found")
        with self.assertRaisesRegex(RuntimeError, "no speaker switch"):
            p.speaker(Fake(), "off")

    # ---- the voice pack (BL-202, AUD-027) ----
    @staticmethod
    def _pack(size=8192, magic=b"RVOC", length=4000):
        data = bytearray(b"\xff" * size)
        data[:4] = magic
        data[8:12] = struct.pack("<I", length)
        return bytes(data)

    def _voice_file(self, d, data):
        f = Path(d) / "en.robovoice"
        f.write_bytes(data)
        return f

    def test_voice_local_checks_refuse_before_contact(self):
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, *a, **k): raise AssertionError("must not contact the panel")
        with tempfile.TemporaryDirectory() as d:
            for data, text in ((self._pack(4096), "not a voice pack"), (self._pack(8192 + 100), "not a voice pack"),
                               (self._pack(970752 + 4096), "not a voice pack"),
                               (self._pack(magic=b"OGGS"), "RVOC"), (self._pack(length=9000), "payload length")):
                with self.assertRaisesRegex(RuntimeError, text):
                    p.voice_install(Fake(), self._voice_file(d, data))

    def test_voice_install_request_and_answer(self):
        class Fake:
            device = {"id": "0123456789ab"}
            def __init__(self): self.calls = []
            def request(self, method, endpoint, data=None, headers=None):
                self.calls.append((method, endpoint, len(data), headers))
                return b'{"result":"ok"}'
        c = Fake()
        with tempfile.TemporaryDirectory() as d:
            r = p.voice_install(c, self._voice_file(d, self._pack()))
        self.assertEqual(c.calls, [("PUT", "voice", 8192, {"Content-Type": "application/octet-stream"})])
        self.assertEqual(r["result"], "ok")
        self.assertIn("Installed", r["message"])

    def test_voice_install_refusals_are_in_plain_words(self):
        for word, status, text in (("locked", 409, "armed"), ("busy", 409, "update"), ("bad_signature", 400, "not signed"),
                                   ("bad_pack", 400, "not a valid voice pack"), ("no_space", 507, "no room"),
                                   ("failed", 500, "unchanged"), ("unavailable", 503, "no voice feature")):
            class Fake:
                device = {"id": "0123456789ab"}
                def request(self, *a, **k): raise p.DeviceError(status, '{"result":"%s"}' % word)
            with tempfile.TemporaryDirectory() as d, self.assertRaisesRegex(RuntimeError, word + ".*" + text):
                p.voice_install(Fake(), self._voice_file(d, self._pack()))

    def test_voice_missing_route_is_explained(self):
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, *a, **k): raise p.DeviceError(404, "Not Found")
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(RuntimeError, "no voice-pack route"):
                p.voice_install(Fake(), self._voice_file(d, self._pack()))
        with self.assertRaisesRegex(RuntimeError, "no voice-pack route"):
            p.voice_status(Fake())

    def test_voice_status_request_and_words(self):
        st = {"state": "ready", "draft": True, "locale": "en-US", "voice_id": "v1", "pack_revision": 3,
              "clips": 120, "file_bytes": 8192, "why": ""}
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, method, endpoint, data=None, headers=None):
                self.call = (method, endpoint, data)
                return json.dumps(st).encode()
        c = Fake()
        self.assertEqual(p.voice_status(c), st)
        self.assertEqual(c.call, ("GET", "voice", None))
        self.assertIn("revision 3", p.voice_status_text(st))
        self.assertIn("DRAFT", p.voice_status_text(st))
        self.assertEqual(p.voice_status_text({"state": "unavailable", "why": "damaged"}), "Voice: unavailable (damaged)")

    def test_voice_arguments_parse_after_the_verb(self):
        with tempfile.TemporaryDirectory() as d, patch.object(p, "REGISTRY", Path(d)):
            seen = {}
            class C:
                def close(self): pass
            def fake_target(args): seen["device"] = args.device; return C()
            def fake_cmd(args, client): seen["cmd"] = (args.voice_command, getattr(args, "file", None), getattr(args, "json", None))
            with patch.object(p, "target", fake_target), patch.object(p, "voice_command", fake_cmd):
                with patch.object(sys, "argv", ["panelctl", "voice", "install", "a.robovoice", "--device", "bench"]):
                    p.main()
                self.assertEqual((seen["device"], seen["cmd"]), ("bench", ("install", "a.robovoice", None)))
                with patch.object(sys, "argv", ["panelctl", "voice", "status", "--json", "--device", "bench"]):
                    p.main()
                self.assertEqual(seen["cmd"], ("status", None, True))
                with patch.object(sys, "argv", ["panelctl", "voice", "play", "a.wav", "--device", "bench"]):
                    p.main()
                self.assertEqual((seen["device"], seen["cmd"]), ("bench", ("play", "a.wav", None)))
                with patch.object(sys, "argv", ["panelctl", "voice", "say", "0x0002", "53", "--gap", "100", "--device", "bench"]):
                    p.main()
                self.assertEqual(seen["device"], "bench")

    # ---- the asset packs (BL-124, OPT-006) ----
    @staticmethod
    def _asset(size=8192, magic=b"RAST", length=1160, kind=3):
        data = bytearray(b"\xff" * size)
        data[:4] = magic
        data[8:12] = struct.pack("<I", length)
        data[12] = kind
        return bytes(data)

    def _asset_file(self, d, data):
        f = Path(d) / "letters.roboasset"
        f.write_bytes(data)
        return f

    def test_asset_local_checks_refuse_before_contact(self):
        # OPT-006: a file the panel would refuse for its size, envelope or kind never leaves the PC
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, *a, **k): raise AssertionError("must not contact the panel")
        big_letters = 256 * 1024 + 4096
        with tempfile.TemporaryDirectory() as d:
            for data, text in ((self._asset(4096), "not an asset pack"), (self._asset(8192 + 100), "not an asset pack"),
                               (self._asset(magic=b"RVOC"), "RAST"), (self._asset(length=9000), "payload length"),
                               (self._asset(length=100), "payload length"), (self._asset(kind=6), "kind 6"),
                               (self._asset(kind=2), "kind 2"),
                               (self._asset(big_letters, length=big_letters - 4096 - 100), "over the letters pack's bound")):
                with self.assertRaisesRegex(RuntimeError, text):
                    p.asset_install(Fake(), self._asset_file(d, data))

    def test_asset_bounds_agree_with_the_builder(self):
        # OPT-006: panelctl's per-kind bounds are asset_pack.py's (and pack_assets.h's PA_*_MAX_FILE);
        # the builder's kind 0x80, the Flasher's board descriptions, is never a panel's
        spec = importlib.util.spec_from_file_location("asset_pack", Path(__file__).resolve().parents[1] / "asset_pack.py")
        ap = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ap)
        self.assertEqual({k: b for k, (_, b) in p.ASSET_KINDS.items()},
                         {kind: limit for kind, limit in ap.KINDS.values() if kind != ap.ps.KIND_BOARDS})

    def test_asset_check_takes_what_the_builder_writes(self):
        # OPT-006: a pack tools/asset_pack.py writes passes panelctl's own checks unchanged
        spec = importlib.util.spec_from_file_location("asset_pack", Path(__file__).resolve().parents[1] / "asset_pack.py")
        ap = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ap)
        with tempfile.TemporaryDirectory() as d, contextlib.redirect_stdout(io.StringIO()):
            font = Path(d) / "font_body.bin"
            font.write_bytes(bytes(range(256)) * 4)
            out = ap.build("letters", "0.9.0", d, [("body", 1, font.read_bytes())], draft=True, match=0x7145E655)
            self.assertEqual(p.asset_check(out), Path(out).read_bytes())

    def test_asset_install_request_and_answer(self):
        class Fake:
            device = {"id": "0123456789ab"}
            def __init__(self, answer): self.calls, self.answer = [], answer
            def request(self, method, endpoint, data=None, headers=None):
                self.calls.append((method, endpoint, len(data), headers))
                return self.answer
        c = Fake(b'{"result":"ok","kind":"letters","state":"ready"}')
        with tempfile.TemporaryDirectory() as d:
            r = p.asset_install(c, self._asset_file(d, self._asset()))
            self.assertEqual(c.calls, [("PUT", "asset", 8192, {"Content-Type": "application/octet-stream"})])
            self.assertEqual((r["result"], r["kind"], r["state"]), ("ok", "letters", "ready"))
            self.assertEqual(r["message"], "Installed.")
            # OPT-013: a certificates pack takes effect at the next start, and says so
            c = Fake(b'{"result":"ok","kind":"certificates","state":"needs_restart"}')
            r = p.asset_install(c, self._asset_file(d, self._asset(kind=5)))
            self.assertIn("next start", r["message"])
            c = Fake(b'{"result":"ok","kind":"picture","state":"damaged"}')
            r = p.asset_install(c, self._asset_file(d, self._asset(kind=4)))
            self.assertIn("damaged", r["message"])

    def test_asset_install_refusals_are_in_plain_words(self):
        for word, status, text in (("locked", 409, "armed"), ("busy", 409, "update"), ("signature", 400, "not signed"),
                                   ("format", 400, "not a valid asset pack"), ("kind", 400, "kind or name"),
                                   ("draft", 400, "release firmware"), ("size", 413, "size"),
                                   ("no_space", 507, "no room"), ("flash", 500, "unchanged"),
                                   ("off", 503, "no pack store")):
            class Fake:
                device = {"id": "0123456789ab"}
                def request(self, *a, **k): raise p.DeviceError(status, '{"result":"%s"}' % word)
            with tempfile.TemporaryDirectory() as d, self.assertRaisesRegex(RuntimeError, word + ".*" + text):
                p.asset_install(Fake(), self._asset_file(d, self._asset()))

    def test_asset_missing_route_is_explained(self):
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, *a, **k): raise p.DeviceError(404, "Not Found")
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(RuntimeError, "no asset-pack route"):
                p.asset_install(Fake(), self._asset_file(d, self._asset()))
        with self.assertRaisesRegex(RuntimeError, "no asset-pack route"):
            p.asset_status(Fake())

    def test_asset_status_request_and_words(self):
        st = {"kinds": [{"kind": "letters", "name": "letters", "state": "ready", "version": "0.9.0", "match": "0x7145e655",
                         "bytes": 102400, "draft": True, "why": ""},
                        {"kind": "picture", "name": "saver", "state": "none", "version": "", "match": "0x00000000",
                         "bytes": 0, "draft": False, "why": ""},
                        {"kind": "certificates", "name": "certs", "state": "refused", "version": "0.8.0",
                         "match": "0x00000000", "bytes": 65536, "draft": False, "why": "made for another firmware version"}],
              "free_bytes": 4096}
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, method, endpoint, data=None, headers=None):
                self.call = (method, endpoint, data)
                return json.dumps(st).encode()
        c = Fake()
        self.assertEqual(p.asset_status(c), st)
        self.assertEqual(c.call, ("GET", "asset", None))
        text = p.asset_status_text(st)
        self.assertIn("letters      ready, version 0.9.0, match 0x7145e655, 102400 bytes, DRAFT", text)
        self.assertIn("picture      none\n", text)
        self.assertIn("refused, version 0.8.0", text)
        self.assertIn("(made for another firmware version)", text)
        self.assertIn("Free in the store: 4096 bytes", text)
        self.assertIn("no pack store", p.asset_status_text({"kinds": [], "free_bytes": 0}))

    def test_asset_arguments_parse_after_the_verb(self):
        with tempfile.TemporaryDirectory() as d, patch.object(p, "REGISTRY", Path(d)):
            seen = {}
            class C:
                def close(self): pass
            def fake_target(args): seen["device"] = args.device; return C()
            def fake_cmd(args, client): seen["cmd"] = (args.asset_command, getattr(args, "file", None), getattr(args, "json", None))
            with patch.object(p, "target", fake_target), patch.object(p, "asset_command", fake_cmd):
                with patch.object(sys, "argv", ["panelctl", "asset", "install", "letters.roboasset", "--device", "bench"]):
                    p.main()
                self.assertEqual((seen["device"], seen["cmd"]), ("bench", ("install", "letters.roboasset", None)))
                with patch.object(sys, "argv", ["panelctl", "asset", "status", "--json", "--device", "bench"]):
                    p.main()
                self.assertEqual(seen["cmd"], ("status", None, True))

    # ---- the audio bench (development builds, AUD-030) ----
    @staticmethod
    def _wav(d, rate=16000, channels=1, width=2, frames=1600):
        import wave
        f = Path(d) / "try.wav"
        with wave.open(str(f), "wb") as w:
            w.setnchannels(channels); w.setsampwidth(width); w.setframerate(rate)
            w.writeframes(b"\x01\x00" * (frames * channels * width // 2))
        return f

    def test_play_refuses_other_shapes_before_contact(self):
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, *a, **k): raise AssertionError("must not contact the panel")
        with tempfile.TemporaryDirectory() as d:
            for kw in ({"rate": 22050}, {"channels": 2}, {"width": 1}, {"frames": 0}, {"frames": 16000 * 31}):
                with self.assertRaises(RuntimeError):
                    p.voice_play(Fake(), self._wav(d, **kw))

    def test_play_sends_the_samples_alone(self):
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, method, endpoint, data=None, headers=None):
                self.call = (method, endpoint, data, headers)
                return b'{"result":"ok","ms":100}'
        c = Fake()
        with tempfile.TemporaryDirectory() as d:
            r = p.voice_play(c, self._wav(d))
        self.assertEqual(c.call, ("PUT", "play", b"\x01\x00" * 1600, {"Content-Type": "application/octet-stream"}))
        self.assertEqual((r["result"], r["seconds"]), ("ok", 0.1))

    def test_play_refusals_and_old_firmware_are_explained(self):
        for err, text in ((p.DeviceError(404, "Not Found"), "no audio bench"),
                          (p.DeviceError(503, '{"result":"unavailable","ms":100}'), "unavailable.*release build"),
                          (p.DeviceError(507, '{"result":"no_space"}'), "no_space.*memory")):
            class Fake:
                device = {"id": "0123456789ab"}
                def request(self, *a, **k): raise err
            with tempfile.TemporaryDirectory() as d, self.assertRaisesRegex(RuntimeError, text):
                p.voice_play(Fake(), self._wav(d))

    # ---- words said by the panel from its own pack (development builds, AUD-030) ----
    def test_say_sends_the_gap_then_the_ids(self):
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, method, endpoint, data=None, headers=None):
                self.call = (method, endpoint, data, headers)
                return b'{"result":"ok","gap_ms":100,"volume":60}'
        c = Fake()
        r = p.voice_say(c, ["0x0002", "0x0035", "112"], 100)
        self.assertEqual(c.call, ("POST", "say", "100 2 53 112", {"Content-Type": "text/plain"}))
        self.assertEqual((r["result"], r["gap_ms"], r["volume"]), ("ok", 100, 60))
        p.voice_say(c, ["1"])
        self.assertEqual(c.call[2], "0 1")  # 0: the pack's gap

    def test_say_refuses_bad_ids_and_gaps_before_contact(self):
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, *a, **k): raise AssertionError("must not contact the panel")
        for ids, gap in ((["0"], 0), (["0x10000"], 0), (["door"], 0), ([], 0), (["1"] * 9, 0),
                         (["1"], -1), (["1"], 251)):
            with self.assertRaises(ValueError):
                p.voice_say(Fake(), ids, gap)

    def test_volume_posts_and_returns_the_state_after(self):
        st = {"state": "ready", "pack_revision": 7, "volume": 60, "word_gap_ms": 180, "enabled": False}
        class Fake:
            device = {"id": "0123456789ab"}
            def request(self, method, endpoint, data=None, headers=None):
                self.call = (method, endpoint, data)
                return json.dumps(st).encode()
        c = Fake()
        self.assertEqual(p.voice_volume(c, 60), st)
        self.assertEqual(c.call, ("POST", "voice", "volume 60"))
        self.assertIn("Voice volume 60 %, 180 ms between words, announcements off", p.voice_status_text(st))
        for bad in (-1, 101):
            with self.assertRaises(ValueError):
                p.voice_volume(c, bad)
        class Old:
            device = {"id": "0123456789ab"}
            def request(self, *a, **k): raise p.DeviceError(405, "Method Not Allowed")
        with self.assertRaisesRegex(RuntimeError, "cannot set the Voice volume"):
            p.voice_volume(Old(), 50)

    def test_say_refusals_and_old_firmware_are_explained(self):
        for err, text in ((p.DeviceError(404, "Not Found"), "cannot say words"),
                          (p.DeviceError(503, '{"result":"quiet","gap_ms":40,"volume":0}'), "quiet.*Voice volume 0"),
                          (p.DeviceError(400, '{"result":"bad_words"}'), "bad_words.*lacks")):
            class Fake:
                device = {"id": "0123456789ab"}
                def request(self, *a, **k): raise err
            with self.assertRaisesRegex(RuntimeError, text):
                p.voice_say(Fake(), ["1"])

    def test_rollback_never_reported_as_success(self):
        class Fake:
            def info(self): return {"boot":"reboot", "elf":"oldelf"}
        with self.assertRaisesRegex(RuntimeError,"rollback"):
            p.wait_update(Fake(),{"old_boot":"old", "elf":"newelf", "operation":"op"})

    def test_stale_operation_never_confirmed(self):
        class Fake:
            def info(self): return {"boot":"reboot", "elf":"newelf", "operation":"other"}
        with self.assertRaisesRegex(RuntimeError,"different update"):
            p.wait_update(Fake(),{"old_boot":"old", "elf":"newelf", "operation":"op"})

    def test_scan_refuses_broad_or_public_network(self):
        for network in ("0.0.0.0/0", "8.8.8.0/24", "172.16.0.0/16", "::1/128"):
            with self.assertRaises(ValueError): p.discover(network)

    def test_browser_bridge_rejects_csrf_rebinding_and_missing_session(self):
        good={"Host":"127.0.0.1:4567","Origin":"http://127.0.0.1:4567","X-Session":"test-session"}
        self.assertTrue(p.bridge_authorized(good,4567,"test-session"))
        for field,value in (("Origin","https://evil.example"),("Origin","null"),
                            ("Host","evil.example:4567"),("Host","localhost:4567"),
                            ("X-Session",""),("X-Session","wrong")):
            self.assertFalse(p.bridge_authorized({**good,field:value},4567,"test-session"))

    def test_usb_wrong_identity_never_queries_or_flashes(self):
        from types import SimpleNamespace
        class Serial:
            def __init__(self,**_): self.writes=[]
            def open(self): pass
            def close(self): pass
            def reset_input_buffer(self): pass
            def write(self,b): self.writes.append(b)
            def read(self,_): return b"DEV ID aaaaaaaaaaaa IP 10.0.0.1 PORT 6055 ENABLED 1\nDEV CERT 00\n"
        port=Serial()
        with patch.dict("sys.modules", {"serial":SimpleNamespace(Serial=lambda **_:port)}):
            with self.assertRaisesRegex(RuntimeError,"different panel"):
                p.usb_info(SimpleNamespace(device={"id":"bbbbbbbbbbbb","fingerprint":"x"}),"COM99")
        self.assertEqual(port.writes,[b"dev status\r"])


if __name__ == "__main__": unittest.main()
