"""The Flasher's firmware from the firmware server (docs/features/34 FLS-032 to FLS-034; the update
server's API): the client's cached, resumed and checked downloads, which release is
chosen, what is trusted, the packs that go with a release, the board descriptions and the
newer-Flasher notice, and the controller's use of them."""
import hashlib
import io
import json
import struct
import sys
import tempfile
import threading
import time
import unittest
import urllib.request
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from roboflash import boards as boards_mod  # noqa: E402
from roboflash import bundle as bundle_mod  # noqa: E402
from roboflash import fwserver, packstore, testkit  # noqa: E402
from roboflash.boards import ChipInfo  # noqa: E402
from roboflash.controller import Controller, Device  # noqa: E402
from roboflash.imagefile import signature_key_digest, verified_key_digest  # noqa: E402
from roboflash.jobs import JobFailed  # noqa: E402
from roboflash.probe import ProbeResult  # noqa: E402

TRUST = {testkit.TEST_KEY_DIGEST: "Test key"}
MB = 1024 * 1024


def wait(cond, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if cond():
            return True
        time.sleep(0.02)
    return False


def rsa_key():
    from cryptography.hazmat.primitives.asymmetric import rsa
    return rsa.generate_private_key(public_exponent=65537, key_size=3072)


def sign_v2(body, key):
    """`body` (whole 4 KiB sectors) and the sector espsecure sign_data --version 2 appends: one RSA-3072
    block with the digest, the key little-endian and an RSA-PSS signature, its CRC-32 right."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding, utils
    assert len(body) % 4096 == 0
    digest = hashlib.sha256(body).digest()
    sig = key.sign(digest, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                   utils.Prehashed(hashes.SHA256()))
    n = key.public_key().public_numbers()
    blk = bytearray(1216)
    blk[0:4] = b"\xe7\x02\x00\x00"
    blk[4:36] = digest
    blk[36:420] = n.n.to_bytes(384, "little")
    blk[420:424] = n.e.to_bytes(4, "little")
    blk[812:1196] = sig[::-1]
    struct.pack_into("<I", blk, 1196, zlib.crc32(bytes(blk[:1196])))
    sector = bytearray(b"\xff" * 4096)
    sector[:1216] = blk
    return bytes(body) + bytes(sector)


def key_digest(key):
    n = key.public_key().public_numbers()
    return hashlib.sha256(n.n.to_bytes(384, "little") + n.e.to_bytes(4, "little")).hexdigest()


def p4_images(profile="release", version="0.6.0", board="crowpanel_p4", key=testkit.TEST_KEY):
    revs = [(1, 199), (301, 399)] if board == "crowpanel_p4" else [(1, 199)]
    return [testkit.synthetic_image(board=board, min_rev=lo, max_rev=hi, profile=profile, version=version,
                                    app_size=64 * 1024, key=key) for lo, hi in revs]


def packs(version="0.6.0", dev=False):
    return {"languages": [("es-US", testkit.language_file("es-US", version, draft=dev)),
                          ("fr-CA", testkit.language_file("fr-CA", version, draft=dev))],
            "voices": [("en-US", testkit.voice_file(2, "en-US")), ("es-US", testkit.voice_file(1, "es-US", "es_test"))],
            "assets": [(name, testkit.asset_file(kind, version)) for name, kind in
                       (("letters", 3), ("picture", 4), ("certificates", 5))]}


def server(**kw):
    srv = testkit.FakeServer(**kw)
    srv.add("crowpanel_p4", "stable", "0.6.0", testkit.bundle_bytes(p4_images()), **packs())
    srv.add("crowpanel_p4", "dev", "0.6.1-dev", testkit.bundle_bytes(p4_images("dev", "0.6.1-dev")),
            **packs("0.6.1-dev", dev=True))
    return srv


class Source:
    """A ServerSource on a FakeServer."""

    def __init__(self, srv, tmp=None, **kw):
        kw.setdefault("trusted", TRUST)
        kw.setdefault("verify", signature_key_digest)
        self.notes = []
        self.src = fwserver.ServerSource(srv, on_change=self.notes.append, **kw)

    def ready(self, board="crowpanel_p4", channel=None, fetch=True):
        t = self.src.want(board, channel, fetch=fetch)
        assert wait(lambda: t.state in ("ready", "failed", "none") and not t.queued), t.state
        return t


# -- the client over HTTP --------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        srv = self.server
        srv.requests.append((self.path, dict(self.headers)))
        body = srv.routes.get(self.path)
        if isinstance(body, tuple):                 # ("redirect", code, Location)
            self.send_response(body[1])
            self.send_header("Location", body[2])
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if body is None:
            self.send_response(404)
            data = json.dumps({"error": "no such thing"}).encode()
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if isinstance(body, dict):
            data = json.dumps(body).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        etag = '"%s"' % hashlib.sha256(body).hexdigest()
        rng = self.headers.get("Range")
        if rng and self.headers.get("If-Range") == etag:
            start = int(rng.split("=")[1].split("-")[0])
            part = body[start:]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(body) - 1}/{len(body)}")
        else:
            part = body
            self.send_response(200)
        self.send_header("ETag", etag)
        self.send_header("Content-Length", str(len(part)))
        self.end_headers()
        self.wfile.write(part)


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.routes, self.httpd.requests = {}, []
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        self.client = fwserver.Client(self.url, fwserver.Cache(Path(self.tmp.name) / "cache"), timeout=5)

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.tmp.cleanup()

    def test_addresses(self):  # FLS-032
        self.assertEqual(fwserver.check_url("https://firmware.example/"), "https://firmware.example")
        self.assertEqual(fwserver.check_url("http://localhost:8092"), "http://localhost:8092")
        for bad in ("http://firmware.example", "ftp://firmware.example", "firmware.example",
                    "https://firmware.example/?x=1", ""):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                fwserver.check_url(bad)

    def test_an_answer_is_remembered_for_when_the_server_cant_be_reached(self):  # FLS-032
        self.httpd.routes["/api/v1/boards"] = {"boards": [{"id": "crowpanel_p4"}]}
        self.assertEqual(self.client.get_json("/api/v1/boards"), ({"boards": [{"id": "crowpanel_p4"}]}, False))
        with self.assertRaises(fwserver.ServerError) as cm:
            self.client.get_json("/api/v1/nothing")
        self.assertIn("404", cm.exception.title)
        self.assertEqual(cm.exception.advice, "no such thing")       # the server's own words
        self.httpd.shutdown()
        self.httpd.server_close()
        again = fwserver.Client(self.url, fwserver.Cache(Path(self.tmp.name) / "cache"), timeout=2)
        self.assertEqual(again.get_json("/api/v1/boards"), ({"boards": [{"id": "crowpanel_p4"}]}, True))
        with self.assertRaises(fwserver.ServerError) as cm:
            again.get_json("/api/v1/releases", {"device": "crowpanel_p4"})
        self.assertIn("can't reach", cm.exception.title)

    def test_a_download_is_checked_kept_and_resumed(self):  # FLS-032
        data = bytes(range(256)) * 400
        sha = hashlib.sha256(data).hexdigest()
        self.httpd.routes["/dl/a"] = data
        got = []
        self.assertEqual(self.client.fetch("/dl/a", len(data), sha, progress=lambda n, size: got.append(n)), data)
        self.assertEqual(got[-1], len(data))
        n = len(self.httpd.requests)
        self.assertEqual(self.client.fetch("/dl/a", len(data), sha), data)       # from the cache
        self.assertEqual(len(self.httpd.requests), n)
        # A download that stopped half way goes on from where it stopped.
        self.client.cache.path(sha).unlink()
        self.client.cache.part_path(sha).write_bytes(data[:5000])
        self.assertEqual(self.client.fetch("/dl/a", len(data), sha), data)
        path, headers = self.httpd.requests[-1]
        self.assertEqual((headers.get("Range"), headers.get("If-Range")), ("bytes=5000-", f'"{sha}"'))
        # A file that isn't what the server says is never kept.
        self.httpd.routes["/dl/b"] = data[:-1] + b"x"
        with self.assertRaises(fwserver.ServerError) as cm:
            self.client.fetch("/dl/b", len(data), hashlib.sha256(b"other").hexdigest())
        self.assertIn("damaged", cm.exception.title)
        self.assertEqual(sorted(p.name for p in self.client.cache.root.iterdir() if not p.name.startswith("answers")), [sha])
        # A damaged file in the cache is dropped and downloaded again.
        self.client.cache.path(sha).write_bytes(b"\0" * len(data))
        self.assertEqual(self.client.fetch("/dl/a", len(data), sha), data)

    def test_never_anywhere_but_https(self):  # FLS-032
        with self.assertRaises(fwserver.ServerError) as cm:
            self.client.fetch("http://firmware.example/dl/a", 10, "0" * 64)
        self.assertIn("won't go", cm.exception.title)
        for size, sha in ((0, "0" * 64), (-1, "0" * 64), (10, "nope"), (fwserver.MAX_FILE + 1, "0" * 64)):
            with self.assertRaises(fwserver.ServerError):
                self.client.fetch("/dl/a", size, sha)

    def test_every_redirect_meets_the_transport_rule(self):  # FLS2-005 (FLS-032)
        """A redirect is checked before it is followed: plain http only to this computer, and never
        from https down to http. The server here is bound to 127.0.0.1; nothing else is reached."""
        port = self.httpd.server_address[1]
        data = b"firmware bytes " * 300
        sha = hashlib.sha256(data).hexdigest()
        self.httpd.routes["/finish"] = data
        self.httpd.routes["/finish.json"] = {"releases": []}
        for code in (301, 302, 303, 307, 308):
            for target in (f"http://localhost.:{port}/finish", f"//localhost.:{port}/finish",
                           "http://192.0.2.1/finish", f"ftp://127.0.0.1:{port}/finish"):
                with self.subTest(code=code, target=target):
                    self.httpd.routes["/go"] = ("redirect", code, target)
                    self.httpd.routes["/api"] = ("redirect", code, target + ".json")
                    n = len(self.httpd.requests)
                    with self.assertRaises(fwserver.ServerError) as cm:
                        self.client.fetch("/go", len(data), sha)
                    self.assertIn("won't go", cm.exception.title)
                    self.assertIn("redirect", cm.exception.advice)
                    with self.assertRaises(fwserver.ServerError) as cm:
                        self.client.get_json("/api")
                    self.assertIn("won't go", cm.exception.title)
                    self.assertEqual([p for p, _ in self.httpd.requests[n:]], ["/go", "/api"])
        # Hops that keep to the rule are followed, several in a row.
        self.httpd.routes["/hop1"] = ("redirect", 302, f"http://127.0.0.1:{port}/hop2")
        self.httpd.routes["/hop2"] = ("redirect", 308, "/finish")
        self.httpd.routes["/hop3"] = ("redirect", 301, "/finish.json")
        self.assertEqual(self.client.fetch("/hop1", len(data), sha), data)
        self.assertEqual(self.client.get_json("/hop3"), ({"releases": []}, False))

    def test_an_https_redirect_never_goes_down_to_http(self):  # FLS2-005 (FLS-032)
        handler = fwserver.PolicyRedirects()
        req = urllib.request.Request("https://firmware.example/api/v1/releases")
        for target in ("http://127.0.0.1:8092/api/v1/releases", "http://localhost/x", "http://cdn.example/x"):
            with self.subTest(target=target), self.assertRaises(fwserver.ServerError) as cm:
                handler.redirect_request(req, io.BytesIO(), 302, "Found", {}, target)
            self.assertIn("won't go", cm.exception.title)
        new = handler.redirect_request(req, io.BytesIO(), 307, "Temporary Redirect", {}, "https://cdn.example/x")
        self.assertEqual(new.full_url, "https://cdn.example/x")
        local = urllib.request.Request("http://127.0.0.1:8092/api")       # a test server on this computer
        self.assertEqual(handler.redirect_request(local, io.BytesIO(), 302, "Found", {},
                                                  "http://localhost:8092/b").full_url, "http://localhost:8092/b")

    def test_the_cache_keeps_the_newest_within_its_limit(self):  # FLS-032
        cache = fwserver.Cache(Path(self.tmp.name) / "small", limit=10_000)
        for i in range(4):
            data = bytes([i]) * 4000
            sha = hashlib.sha256(data).hexdigest()
            part = cache.part_path(sha)
            part.write_bytes(data)
            cache.put(sha, part)
            time.sleep(0.02)
        kept = [p for p in cache.root.iterdir() if len(p.name) == 64]
        self.assertEqual(len(kept), 2)


# -- what is chosen --------------------------------------------------------------------------------
class ChooseTests(unittest.TestCase):
    def answer(self):
        def rel(version, channel, *bundles):
            return {"version": version, "channel": channel, "bundles": list(bundles)}
        p = "alarm_crowpanel_p4-release"
        return {"releases": [rel("0.9.0", "dev", {"project": "alarm_crowpanel_p4-dev"}),
                             rel("0.8.0", "alpha", {"project": p, "rules": {"requires": ["zac93"]}, "url": "z"},
                                 {"project": p, "rules": {}, "url": "plain"}),
                             rel("0.7.0", "beta", {"project": p, "rules": {"requires": ["zac93"]}, "url": "only"}),
                             rel("0.6.0", "stable", {"project": p, "url": "s"})]}

    def test_a_channel_sees_itself_and_the_steadier_ones(self):  # FLS-033
        p = "alarm_crowpanel_p4-release"
        pick = lambda ch: fwserver.choose(self.answer(), p, ch)  # noqa: E731
        self.assertEqual((pick("stable")[0]["version"], pick("stable")[1]["url"]), ("0.6.0", "s"))
        self.assertEqual((pick("beta")[0]["version"], pick("beta")[1]["url"]), ("0.7.0", "only"))
        self.assertEqual((pick("alpha")[0]["version"], pick("alpha")[1]["url"]), ("0.8.0", "plain"))  # rules last
        self.assertEqual(fwserver.choose(self.answer(), p, "dev"), (None, None))   # dev sees dev builds only
        self.assertEqual(fwserver.choose(self.answer(), "alarm_crowpanel_p4-dev", "dev")[0]["version"], "0.9.0")
        self.assertEqual(fwserver.choose({"releases": []}, p, "stable"), (None, None))

    def test_rules_against_the_parts_chosen(self):
        self.assertEqual(fwserver.peripherals_of({"rf": "module", "clock": True, "zigbee": True, "zwave": True}),
                         ["rf433", "ds3231", "crowpanel_h2", "zac93"])
        self.assertEqual(fwserver.peripherals_of({"rf": "cc1101"}), ["cc1101"])
        self.assertIsNone(fwserver.peripherals_of(None))
        self.assertEqual(fwserver.rules_problem({}, None), "")
        self.assertEqual(fwserver.rules_problem({"requires": ["zac93"]}, ["zac93", "rf433"]), "")
        self.assertIn("needs zac93", fwserver.rules_problem({"requires": ["zac93"]}, []))
        self.assertIn("doesn't work with cc1101", fwserver.rules_problem({"excludes": ["cc1101"]}, ["cc1101"]))
        self.assertIn("Connected parts", fwserver.rules_problem({"requires": ["zac93"]}, None))
        self.assertIn("board revisions", fwserver.rules_problem({"board_revs": ["V1.1"]}, []))

    def test_platforms_and_versions(self):
        self.assertRegex(fwserver.platform_id(), r"^(windows|macos|linux)-(x86_64|arm64)$")
        self.assertEqual(fwserver.semver("1.2.3"), (1, 2, 3))
        self.assertIsNone(fwserver.semver("1.2.3-dev"))


# -- a board's release on a channel --------------------------------------------------------------
class SourceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.srv = server()

    def tearDown(self):
        self.tmp.cleanup()
        boards_mod.use_document(None)

    def test_a_release_is_downloaded_checked_and_tagged(self):  # FLS-032
        s = Source(self.srv, self.tmp.name)
        s.src.start(["crowpanel_p4"])
        t = s.ready()
        self.assertEqual((t.state, t.version, t.project, t.error), ("ready", "0.6.0", "alarm_crowpanel_p4-release", None))
        b = t.bundle
        self.assertEqual([i.key for i in b.images], ["crowpanel_p4-v1-release", "crowpanel_p4-v3-release"])
        self.assertEqual([l.locale for l in b.languages], ["es-US", "fr-CA"])
        self.assertEqual(sorted(v.locale for v in b.voices), ["en-US", "es-US"])
        self.assertEqual(sorted(a.name for a in b.assets), ["certs", "letters", "saver"])  # the saver's picture always
        origins = {x.origin for x in b.images + b.languages + b.voices + b.assets}
        self.assertEqual(origins, {"crowpanel_p4 0.6.0 (stable)"})
        self.assertEqual(t.done, t.size)
        self.assertIn(("/api/v1/languages", {"device": "crowpanel_p4", "chip_rev": "v1", "version": "0.6.0",
                                             "project": "alarm_crowpanel_p4-release"}), self.srv.asked)

    def test_looking_up_downloads_nothing(self):  # FLS-032
        s = Source(self.srv, self.tmp.name)
        t = s.src.want("crowpanel_p4", fetch=False)
        s.src.start()
        self.assertTrue(wait(lambda: t.state == "available"))
        self.assertEqual((t.version, self.srv.fetched), ("0.6.0", []))
        s.src.want("crowpanel_p4")
        self.assertTrue(wait(lambda: t.state == "ready"))

    def test_an_untrusted_key_is_refused_before_anything_is_downloaded(self):  # FLS-032, decided 2026-10-12
        srv = testkit.FakeServer()
        srv.add("crowpanel_p4", "stable", "0.6.0", testkit.bundle_bytes(p4_images()),
                key={"name": "Someone else", "flasher_sha256": "ab" * 32})
        s = Source(srv, self.tmp.name)
        s.src.start()
        t = s.ready()
        self.assertEqual(t.state, "failed")
        self.assertIn("doesn't trust (Someone else)", t.error["title"])
        self.assertEqual(srv.fetched, [])

    def test_a_listing_that_lies_about_the_key_is_caught_by_the_signatures(self):  # FLS-032
        srv = testkit.FakeServer()
        other = bytes(reversed(testkit.TEST_KEY))
        srv.add("crowpanel_p4", "stable", "0.6.0", testkit.bundle_bytes(p4_images(key=other)))
        s = Source(srv, self.tmp.name)
        s.src.start()
        t = s.ready()
        self.assertEqual(t.state, "failed")
        self.assertIn("not signed with a key this Flasher trusts", t.error["title"])
        srv = testkit.FakeServer()
        srv.add("crowpanel_p4", "stable", "0.6.0", testkit.bundle_bytes(p4_images()),
                assets=[("picture", testkit.asset_file(4, key=other))])
        s = Source(srv, Path(self.tmp.name) / "2")
        s.src.start()
        t = s.ready()
        self.assertEqual(t.state, "failed")
        self.assertIn("picture pack is not signed", t.error["title"])

    def test_real_signatures_are_verified(self):  # FLS-032
        key = rsa_key()
        digest = key_digest(key)
        images = p4_images()
        for image in images:
            app = image.part("app")
            app.data = sign_v2(app.data[:-4096], key)
            image.signing_key_sha256 = signature_key_digest(app.data)
        self.assertEqual(verified_key_digest(images[0].part("app").data), digest)
        tampered = bytearray(images[0].part("app").data)
        tampered[300] ^= 1
        self.assertEqual(verified_key_digest(bytes(tampered)), "")
        self.assertEqual(signature_key_digest(bytes(tampered)), digest)       # it only names the key
        srv = testkit.FakeServer()
        srv.add("crowpanel_p4", "stable", "0.6.0", testkit.bundle_bytes(images),
                key={"name": "Owner", "flasher_sha256": digest})
        s = Source(srv, self.tmp.name, trusted={digest: "Owner"}, verify=verified_key_digest)
        s.src.start()
        self.assertEqual(s.ready().state, "ready")
        bad = p4_images()
        app = bad[0].part("app")
        app.data = sign_v2(app.data[:-4096], key)
        body = bytearray(app.data)
        body[400] ^= 1
        app.data = bytes(body)
        srv = testkit.FakeServer()
        srv.add("crowpanel_p4", "stable", "0.6.0", testkit.bundle_bytes(bad), key={"name": "Owner", "flasher_sha256": digest})
        s = Source(srv, Path(self.tmp.name) / "2", trusted={digest: "Owner"}, verify=verified_key_digest)
        s.src.start()
        t = s.ready()
        self.assertEqual(t.state, "failed")
        self.assertIn("not signed with a key this Flasher trusts", t.error["title"])

    def test_no_release_on_the_channel(self):  # FLS-033
        s = Source(self.srv, self.tmp.name)
        s.src.start()
        t = s.ready("crowpanel_10")
        self.assertEqual((t.state, t.bundle), ("none", None))

    def looked_up(self, srv, board="crowpanel_p4"):
        s = Source(srv)
        s.src.start([board])
        assert wait(lambda: all(not t.queued and t.state not in ("waiting", "checking")
                                for t in list(s.src.targets.values()))), s.src.to_dict()
        return s

    def test_stable_else_beta_else_alpha_dev_only_offered(self):  # FLS-033, decided 2026-10-12
        def on(*releases):
            srv = testkit.FakeServer()
            for channel, version in releases:
                profile = "dev" if channel == "dev" else "release"
                srv.add("crowpanel_p4", channel, version, testkit.bundle_bytes(p4_images(profile, version)))
            s = self.looked_up(srv)
            return s.src.channel_for("crowpanel_p4"), s.src.dev_offered("crowpanel_p4")
        self.assertEqual(on(("stable", "0.6.0"), ("beta", "0.7.0"), ("alpha", "0.8.0"), ("dev", "0.9.0-dev")),
                         ("stable", False))                        # the steadiest, not the newest
        self.assertEqual(on(("beta", "0.7.0"), ("alpha", "0.8.0"), ("dev", "0.9.0-dev")), ("beta", False))
        self.assertEqual(on(("alpha", "0.8.0"), ("dev", "0.9.0-dev")), ("alpha", False))
        self.assertEqual(on(("dev", "0.9.0-dev")), ("dev", True))    # offered, never taken by itself
        self.assertEqual(on(), ("dev", False))                       # nothing at all

    def test_the_order_waits_for_each_answer(self):  # FLS-033
        self.srv.delay = 0.5
        s = Source(self.srv)
        s.src.start(["crowpanel_p4"])
        self.assertEqual(s.src.channel_for("crowpanel_p4"), "stable")    # still asking Stable
        self.assertFalse(s.src.dev_offered("crowpanel_p4"))
        self.srv.offline = True                                          # Stable can't be asked
        s2 = self.looked_up(self.srv, "crowpanel_10")
        self.assertEqual((s2.src.channel_for("crowpanel_10"), s2.src.target("crowpanel_10").state), ("stable", "failed"))

    def test_a_channel_shows_which_channel_its_release_is_on(self):  # FLS-033
        s = self.looked_up(self.srv)
        on = {t.channel: (t.state, t.release_channel, t.version) for t in s.src.targets.values()}
        self.assertEqual(on["stable"], ("available", "stable", "0.6.0"))
        self.assertEqual(on["beta"], ("available", "stable", "0.6.0"))      # Beta sees Stable: none of its own
        self.assertEqual(on["dev"], ("available", "dev", "0.6.1-dev"))
        self.assertEqual(self.srv.fetched, [])                               # looked up, nothing downloaded

    def test_the_persons_pick_is_for_this_run_only(self):  # FLS-033
        s = self.looked_up(self.srv)
        self.assertEqual((s.src.pick, s.src.channel_for("crowpanel_p4")), (None, "stable"))
        s.ready()
        s.src.set_channel("dev")
        self.assertEqual(s.src.channel_for("crowpanel_p4"), "dev")
        t = s.ready()
        self.assertEqual((t.version, t.project, t.bundle.images[0].profile), ("0.6.1-dev", "alarm_crowpanel_p4-dev", "dev"))
        self.assertTrue(all(l.draft for l in t.bundle.languages))
        self.assertEqual([x.channel for x in s.src.ready()], ["dev"])        # only the chosen channel's
        self.assertIsNone(Source(self.srv).src.pick)                          # the next run: automatic again
        s.src.set_channel("auto")
        self.assertEqual((s.src.pick, [x.channel for x in s.src.ready()]), (None, ["stable"]))
        # The version list chooses for one board: the next panel isn't moved to Dev by it.
        s.src.set_channel("dev", "crowpanel_10")
        self.assertEqual((s.src.channel_for("crowpanel_10"), s.src.channel_for("crowpanel_p4")), ("dev", "stable"))
        s.src.set_channel("beta")                                             # Software: every board
        self.assertEqual((s.src.channel_for("crowpanel_10"), s.src.picked("crowpanel_p4")), ("beta", "beta"))
        with self.assertRaises(ValueError):
            s.src.set_channel("nightly")

    def test_offline_what_was_downloaded_is_used(self):  # FLS-032
        s = Source(self.srv, self.tmp.name)
        s.src.start()
        t = s.ready()
        self.srv.offline = True
        s.src.refresh()
        self.assertTrue(wait(lambda: s.src.offline and not t.queued and t.offline))
        self.assertEqual((t.state, t.version), ("ready", "0.6.0"))
        # A newer release replaces it once the server answers again; until it is downloaded the old one stays.
        self.srv.offline = False
        self.srv.add("crowpanel_p4", "stable", "0.6.2", testkit.bundle_bytes(p4_images(version="0.6.2")),
                     **packs("0.6.2"))
        s.src.refresh()
        self.assertTrue(wait(lambda: t.ready_version == "0.6.2"))
        self.assertFalse(t.offline)

    def test_board_descriptions_from_the_server(self):  # FLS-034
        doc = boards_mod.builtin_document()
        doc["version"] = "2099.1.1"
        doc["boards"]["crowpanel_new"] = dict(doc["boards"]["crowpanel_10"], name="CrowPanel New")
        ap = testkit._tool("asset_pack")

        def pack(d, key=testkit.TEST_KEY, version=None):
            raw = json.dumps(d).encode()
            payload = ap.build_payload(packstore.KIND_BOARDS, "boards", version or d["version"],
                                       [("boards", packstore.PART_JSON, raw)])
            return payload + b"\xff" * (-len(payload) % 4096) + testkit.signature_sector(key)

        def offer(data, version):
            self.srv.flasher["boards"] = {"version": version, "format": 1, **self.srv.file(data)}

        for data, version, why in ((pack(doc, bytes(reversed(testkit.TEST_KEY))), "2099.1.1", "untrusted"),
                                   (pack(dict(doc, boards={})), "2099.1.1", "no boards"),
                                   (pack(dict(doc, format=9)), "2099.1.1", "a newer format"),
                                   (pack(doc), "2099.1.2", "another version than the server says"),
                                   (pack(dict(doc, version="2000.1.1")), "2000.1.1", "older")):
            with self.subTest(why=why):
                offer(data, version)
                s = Source(self.srv, self.tmp.name)
                s.src.start()
                self.assertTrue(wait(lambda: s.notes))
                self.assertNotIn("crowpanel_new", boards_mod.load_catalogue())
                self.assertEqual(s.src.boards_version, "")
        offer(pack(doc), "2099.1.1")
        s = Source(self.srv, self.tmp.name)
        with tempfile.TemporaryDirectory() as root:
            c = Controller(lister=lambda: [], backups_dir=root, captures_dir=root, logs_dir=root, source=s.src)
            c.start()
            try:
                self.assertTrue(wait(lambda: "crowpanel_new" in c.catalogue))
                self.assertEqual(c.runner.catalogue, c.catalogue)
                self.assertEqual(s.src.boards_version, "2099.1.1")
                self.assertTrue(wait(lambda: s.src.target("crowpanel_new") is not None))   # its release looked up
            finally:
                c.stop()
        # Offline next time: the same descriptions, from what was downloaded.
        boards_mod.use_document(None)
        self.srv.offline = True
        s = Source(self.srv, self.tmp.name)
        s.src.start()
        self.assertTrue(wait(lambda: "crowpanel_new" in boards_mod.load_catalogue()))

    def test_a_newer_flasher_is_only_pointed_at(self):  # FLS-034
        me = fwserver.platform_id()

        def notice(programs, frozen=True):
            self.srv.flasher["programs"] = programs
            s = Source(self.srv, self.tmp.name, frozen=frozen)
            s.src.start()
            self.assertTrue(wait(lambda: s.notes))
            return s.src.program
        newer = {"platform": me, "version": "9.0.0", "url": "/dl/flasher-9", "size": 1, "sha256": "0" * 64}
        self.assertEqual(notice([newer]), {"version": "9.0.0", "url": "https://firmware.example/dl/flasher-9"})
        self.assertIsNone(notice([newer], frozen=False))            # run from the source: no notice
        self.assertIsNone(notice([dict(newer, version="0.0.1")]))
        self.assertIsNone(notice([dict(newer, platform="plan9-mips")]))
        self.assertIsNone(notice([dict(newer, url="http://elsewhere.example/x")]))
        self.assertEqual(notice([newer, dict(newer, version="9.1.0", url="/dl/flasher-91")])["version"], "9.1.0")


# -- the controller ------------------------------------------------------------------------------
class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.srv = server()
        self.s = Source(self.srv, self.tmp.name)
        root = self.tmp.name
        self.c = Controller(lister=lambda: [], backups_dir=root, captures_dir=root, logs_dir=root, source=self.s.src)
        self.c.start()
        self.started = []
        fake_job = lambda kind: lambda port, image, **kw: (self.started.append((kind, image, kw))  # noqa: E731
                                                            or SimpleNamespace(to_dict=lambda: {"kind": kind}))
        self.c.runner.install = fake_job("install")
        self.c.runner.update = fake_job("update")

    def tearDown(self):
        self.c.stop()
        self.tmp.cleanup()

    def new_panel(self, port="COM5", board="crowpanel_p4"):
        self.c.devices[port] = Device(port, chip=ChipInfo("esp32p4", 103, 16 * MB, "aa"))
        self.c.select_board(port, board)

    def running(self, port, profile, version="0.5.9"):
        self.c.devices[port] = Device(port, probe=ProbeResult(port=port, answered=True, board="crowpanel_p4", idle=True,
                                                              chip_revision=103, profile=profile, version=version))

    def ready(self, port):
        assert wait(lambda: (self.c.device_dict(port)["firmware"] or {}).get("state") in ("ready", "failed", "none")
                    and self.c.device_dict(port)["image_key"] != ""), self.c.device_dict(port)["firmware"]
        return self.c.device_dict(port)

    def test_every_panel_gets_the_steadiest_release(self):  # FLS-032, FLS-033, decided 2026-10-12
        self.new_panel()
        self.running("COM6", "dev")
        self.running("COM7", "release")
        self.assertEqual(self.ready("COM5")["image_key"], "crowpanel_p4-v1-release")
        self.assertEqual(self.ready("COM6")["image_key"], "crowpanel_p4-v1-release")  # running dev doesn't choose
        self.assertEqual(self.ready("COM7")["firmware"]["channel"], "stable")
        snap = self.c.snapshot()["firmware"]
        self.assertEqual((snap["pick"], snap["chosen"]["crowpanel_p4"]), (None, "stable"))
        self.c.firmware_channel("dev")
        self.assertEqual(self.ready("COM6")["image_key"], "crowpanel_p4-v1-dev")

    def test_going_back_never_installs_the_other_channels_build(self):  # FLS-033
        self.c.firmware_channel("dev")
        self.running("COM6", "dev")
        self.ready("COM6")
        self.srv.delay = 1.0
        self.srv.add("crowpanel_p4", "stable", "0.6.2", testkit.bundle_bytes(p4_images(version="0.6.2")), **packs("0.6.2"))
        self.c.firmware_channel("auto")
        self.c.firmware_refresh()
        d = self.c.device_dict("COM6")
        self.assertNotEqual(d["image_key"], "crowpanel_p4-v1-dev")
        if d["image_key"] == "":
            with self.assertRaises(JobFailed) as cm:
                self.c.start_job("update", "COM6")
            self.assertRegex(cm.exception.title, "still (downloading|getting)")
            self.assertEqual(self.started, [])
        self.assertTrue(wait(lambda: self.c.device_dict("COM6")["firmware"]["ready_version"] == "0.6.2"))
        self.assertEqual(self.c.device_dict("COM6")["image_key"], "crowpanel_p4-v1-release")

    def test_a_development_build_only_when_the_person_chooses_it(self):  # FLS-033, decided 2026-10-12
        srv = testkit.FakeServer()
        srv.add("crowpanel_10", "dev", "0.6.1-dev", testkit.bundle_bytes(p4_images("dev", "0.6.1-dev", board="crowpanel_10")),
                **packs("0.6.1-dev", dev=True))
        self.c.stop()
        self.s = Source(srv)
        root = self.tmp.name
        self.c = Controller(lister=lambda: [], backups_dir=root, captures_dir=root, logs_dir=root, source=self.s.src)
        self.c.start()
        self.c.runner.install = lambda port, image, **kw: self.started.append(image.key) or SimpleNamespace(to_dict=dict)
        self.new_panel(board="crowpanel_10")
        self.assertTrue(wait(lambda: (self.c.device_dict("COM5")["firmware"] or {}).get("dev_offered")))
        d = self.c.device_dict("COM5")
        self.assertEqual((d["firmware"]["version"], d["image_key"], d["options"]), ("0.6.1-dev", "", []))
        self.assertEqual(srv.fetched, [])                                     # offered: looked up only
        self.assertEqual(self.c.snapshot()["firmware"]["dev_offered"], ["crowpanel_10"])
        with self.assertRaisesRegex(JobFailed, "Only a development version"):
            self.c.start_job("install", "COM5", backup=False)
        with self.assertRaisesRegex(JobFailed, "Only a development version"):
            self.c.firmware_get("crowpanel_10")
        self.assertEqual(self.started, [])
        self.c.firmware_channel("dev")
        self.assertEqual(self.ready("COM5")["image_key"], "crowpanel_10-v1-dev")
        self.assertFalse(self.c.device_dict("COM5")["firmware"]["dev_offered"])
        self.c.start_job("install", "COM5", backup=False)
        self.assertEqual(self.started, ["crowpanel_10-v1-dev"])

    def test_install_takes_its_releases_packs_english_by_default(self):  # FLS-032, ADR-053
        self.new_panel()
        self.ready("COM5")
        self.c.start_job("install", "COM5", backup=False)
        kind, image, kw = self.started[-1]
        self.assertEqual((image.key, kw["language"]), ("crowpanel_p4-v1-release", None))
        self.assertEqual(kw["voice"].locale, "en-US")
        self.assertEqual(sorted(a.name for a in kw["assets"]), ["certs", "letters", "saver"])
        self.c.start_job("install", "COM5", backup=False, language="es-US")
        _, _, kw = self.started[-1]
        self.assertEqual((kw["language"].locale, kw["voice"].locale), ("es-US", "es-US"))
        self.c.start_job("install", "COM5", backup=False, language="fr-CA")
        _, _, kw = self.started[-1]
        self.assertEqual(kw["language"].locale, "fr-CA")
        self.assertIsNone(kw["voice"])                           # never another language's voice
        with self.assertRaisesRegex(JobFailed, "no language"):
            self.c.start_job("install", "COM5", backup=False, language="de-DE")

    def test_update_takes_only_its_own_releases_packs(self):  # FLS-032
        self.new_panel()
        self.ready("COM5")
        self.c.firmware_channel("dev")
        self.running("COM6", "dev")
        self.ready("COM6")
        self.c.start_job("update", "COM6")
        _, image, kw = self.started[-1]
        self.assertEqual(image.version, "0.6.1-dev")
        self.assertEqual({l.version for l in kw["languages"]} | {a.version for a in kw["assets"]}, {"0.6.1-dev"})

    def test_a_release_with_rules_needs_the_parts_it_names(self):
        srv = testkit.FakeServer()
        srv.add("crowpanel_p4", "stable", "0.6.0", testkit.bundle_bytes(p4_images()), rules={"requires": ["zac93"]})
        self.c.stop()
        self.s = Source(srv, Path(self.tmp.name) / "r")
        root = self.tmp.name
        self.c = Controller(lister=lambda: [], backups_dir=root, captures_dir=root, logs_dir=root, source=self.s.src)
        self.c.start()
        self.c.runner.install = lambda port, image, **kw: self.started.append(kw) or SimpleNamespace(to_dict=dict)
        self.new_panel()
        self.ready("COM5")
        with self.assertRaisesRegex(JobFailed, "needs zac93"):
            self.c.start_job("install", "COM5", backup=False, parts={"zwave": False})
        self.c.start_job("install", "COM5", backup=False, parts={"zwave": True})
        self.assertEqual(len(self.started), 1)

    def test_failures_and_no_release_say_why(self):  # FLS-032, FLS-033
        self.c.devices["COM5"] = Device("COM5", chip=ChipInfo("esp32p4", 103, 16 * MB, "aa"))
        self.c.select_board("COM5", "crowpanel_10")
        self.assertTrue(wait(lambda: (self.c.device_dict("COM5")["firmware"] or {}).get("state") == "none"))
        with self.assertRaisesRegex(JobFailed, "no RoboAlarms release for this board yet"):
            self.c.start_job("install", "COM5", backup=False)
        self.c.firmware_channel("alpha")
        self.assertTrue(wait(lambda: (self.c.device_dict("COM5")["firmware"] or {}).get("channel") == "alpha"
                             and self.c.device_dict("COM5")["firmware"]["state"] == "none"))
        with self.assertRaisesRegex(JobFailed, "no RoboAlarms release for this board on Alpha"):
            self.c.start_job("install", "COM5", backup=False)
        self.c.stop()
        srv = server()
        srv.offline = True                     # never reached, nothing from before
        self.s = Source(srv)
        root = self.tmp.name
        self.c = Controller(lister=lambda: [], backups_dir=root, captures_dir=root, logs_dir=root, source=self.s.src)
        self.c.start()
        self.new_panel()
        self.assertTrue(wait(lambda: (self.c.device_dict("COM5")["firmware"] or {}).get("state") == "failed"))
        self.assertEqual(self.c.device_dict("COM5")["firmware"]["channel"], "stable")   # not on to Alpha or Dev
        with self.assertRaisesRegex(JobFailed, "can't reach"):
            self.c.start_job("install", "COM5", backup=False)

    def test_a_file_by_hand_then_back_to_the_server(self):  # FLS-015, FLS-032
        self.new_panel()
        self.ready("COM5")
        self.c.load_bundle_bytes(testkit.bundle_bytes(testkit.standard_images()), "mine.roboflash")
        snap = self.c.snapshot()
        self.assertTrue(snap["firmware"]["from_file"])
        self.assertEqual(snap["bundle"]["name"], "mine.roboflash")
        d = self.c.device_dict("COM5")
        self.assertEqual((d["image_key"], d["firmware"]), ("crowpanel_p4-v1-dev", None))
        self.c.firmware_use_server()
        self.assertFalse(self.c.snapshot()["firmware"]["from_file"])
        self.assertEqual(self.c.device_dict("COM5")["image_key"], "crowpanel_p4-v1-release")

    def test_without_a_server_nothing_is_offered(self):  # FLS-032
        with tempfile.TemporaryDirectory() as root:
            c = Controller(lister=lambda: [], backups_dir=root, captures_dir=root, logs_dir=root)
            self.assertIsNone(c.snapshot()["firmware"])
            with self.assertRaises(JobFailed):
                c.firmware_channel("beta")
            with self.assertRaises(JobFailed):
                c.open_flasher_download()


if __name__ == "__main__":
    unittest.main()
