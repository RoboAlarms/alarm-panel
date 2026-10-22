"""The page's server (FLS-018): 127.0.0.1 only, a random port, a per-run token, a Host check.

The window (or a browser) loads http://127.0.0.1:<port>/#<token>; the page sends the token in
an X-Flasher-Token header with every API call, so another page or program on this computer
cannot drive the Flasher. The same pattern as `panelctl screen` (tools/panelctl.py).

  GET  /                 the page (index.html, app.css, app.js, logo.png)
  GET  /api/state        everything the page shows
  GET  /api/events       server-sent events (read with fetch, so the header can be sent)
  POST /api/<action>     JSON in, JSON out ({"ok": true, ...} or {"ok": false, "error": {...}})
  POST /api/bundle       a bundle file as the raw body (X-Filename header)
"""
import hmac
import json
import queue
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from .jobs import JobFailed
from .paths import RESOURCES
from .serialio import PortError

WEB = RESOURCES / "web"
STATIC = {"/": "index.html", "/index.html": "index.html", "/app.css": "app.css", "/app.js": "app.js",
          "/logo.png": "logo.png", "/theme.js": "theme.js"}
TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
         ".js": "text/javascript; charset=utf-8", ".png": "image/png"}
MAX_BODY = 64 * 1024 * 1024
HEADERS = {"Cache-Control": "no-store", "X-Content-Type-Options": "nosniff",
           "Referrer-Policy": "no-referrer", "X-Frame-Options": "DENY",
           "Content-Security-Policy": "default-src 'self'; img-src 'self' data:; style-src 'self'; "
                                      "script-src 'self'; connect-src 'self'; frame-ancestors 'none'"}


def _actions(c):
    """The page's actions: name -> fn(body) returning a dict (or None)."""
    def port(b):
        p = str(b.get("port", ""))
        if not p:
            raise JobFailed("No port given.", "")
        return p
    return {
        "probe": lambda b: c.probe(port(b), alone=b.get("alone") is True),
        "identify": lambda b: c.identify(port(b), alone=b.get("alone") is True),
        "select_image": lambda b: c.select_image(port(b), str(b.get("key", ""))),
        "select_board": lambda b: c.select_board(port(b), str(b.get("board", ""))),
        "job": lambda b: {"job": c.start_job(str(b.get("kind", "")), port(b),
                                             **{k: v for k, v in b.items() if k not in ("kind", "port")})},
        "cancel": lambda b: {"cancelled": c.cancel_job(str(b.get("id", "")))},
        "console_open": lambda b: c.console_open(port(b)),
        "console_close": lambda b: c.console_close(port(b)),
        "console_send": lambda b: c.console_send(port(b), b.get("text", "")),
        "console_restart": lambda b: c.console_restart(port(b)),
        "console_save": lambda b: {"path": c.console_save(port(b))},
        "rf_start": lambda b: c.rf_start(port(b)),
        "rf_stop": lambda b: c.rf_stop(port(b)),
        "rf_listen": lambda b: c.rf_listen(port(b), bool(b.get("on"))),
        "rf_clear": lambda b: c.rf_clear(port(b)),
        "rf_capture": lambda b: c.rf_capture(port(b), int(b.get("ms", 3000))),
        "rf_pin": lambda b: c.rf_set_pin(port(b), None if b.get("gpio") is None else int(b["gpio"])),
        "rf_module": lambda b: c.rf_set_module(port(b), str(b.get("mode", ""))),
        "clock_status": lambda b: c.clock_status(port(b)),
        "clock_set": lambda b: c.clock_set(port(b)),
        "parts_read": lambda b: c.parts_read(port(b)),
        "parts_apply": lambda b: c.parts_apply(port(b), b.get("parts") or {}),
        "parts_zones": lambda b: c.parts_zones(port(b), b.get("parts") or {}),
        "parts_check": lambda b: {"problems": c.parts_check(port(b), b.get("parts") or {})},
        # The Zigbee module (FLS-030): its job is "job" with kind "radio" (and alone, backup).
        "radio_identify": lambda b: c.radio_identify(port(b), alone=b.get("alone") is True),
        "radio_read": lambda b: {"radio": c.radio_read(port(b), alone=b.get("alone") is True)},
        "zigbee_status": lambda b: {"zigbee": c.zigbee_status(port(b))},
        # A board without a screen (FLS-035): its setup Wi-Fi's label, read from `web setup`.
        "setup_label": lambda b: {"setup": c.setup_label(port(b))},
        # ... and set up over USB instead of from a phone (FLS-036).
        "setup_provision": lambda b: {"result": c.setup_provision(port(b), str(b.get("step", "")),
                                                                  [str(a) for a in (b.get("args") or [])])},
        "open_folder": lambda b: c.open_folder(str(b.get("which", ""))),
        # The firmware server (FLS-032 to FLS-034): its channel, check again, download a board's
        # release now, back from a file to the server, and the newer Flasher's download page.
        "firmware_channel": lambda b: c.firmware_channel(str(b.get("channel", "")), str(b.get("board", "")) or None),
        "firmware_refresh": lambda b: c.firmware_refresh(),
        "firmware_get": lambda b: c.firmware_get(str(b.get("board", ""))),
        "firmware_use_server": lambda b: c.firmware_use_server(),
        "open_flasher_download": lambda b: c.open_flasher_download(),
    }


class FlasherServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, controller, port=0):
        self.controller = controller
        self.token = secrets.token_urlsafe(24)
        self.actions = _actions(controller)
        super().__init__(("127.0.0.1", port), Handler)

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_address[1]}/#{self.token}"

    def serve_in_thread(self):
        t = threading.Thread(target=self.serve_forever, name="http", daemon=True)
        t.start()
        return t


class Handler(BaseHTTPRequestHandler):
    server_version = "RoboAlarmsFlasher"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    # -- checks ---------------------------------------------------------------------------------
    def _host_ok(self):
        port = self.server.server_address[1]
        return self.headers.get("Host", "") in (f"127.0.0.1:{port}", f"localhost:{port}")

    def _token_ok(self):
        return hmac.compare_digest(self.headers.get("X-Flasher-Token", ""), self.server.token)

    def _send(self, status, body, ctype="application/json; charset=utf-8"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
        self.send_response(status)
        for k, v in HEADERS.items():
            self.send_header(k, v)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        if self.close_connection:
            self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def _refuse(self, status, text):
        self._send(status, {"ok": False, "error": {"title": text, "advice": ""}})

    # -- GET ------------------------------------------------------------------------------------
    def do_GET(self):
        if not self._host_ok():
            return self._refuse(403, "wrong host")
        path = self.path.split("?", 1)[0]
        if path in STATIC:
            f = WEB / STATIC[path]
            # Fixed types: Windows' registry can map .js to text/plain, which nosniff then blocks.
            ctype = TYPES.get(f.suffix, "application/octet-stream")
            return self._send(200, f.read_bytes(), ctype)
        if not self._token_ok():
            return self._refuse(403, "missing or wrong token")
        if path == "/api/state":
            return self._send(200, {"ok": True, "state": self.server.controller.snapshot()})
        if path == "/api/events":
            return self._events()
        return self._refuse(404, "not found")

    def _events(self):
        hub = self.server.controller.events
        q = hub.subscribe()
        try:
            self.send_response(200)
            for k, v in HEADERS.items():
                self.send_header(k, v)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            self.wfile.write(b": hello\n\n")
            self.wfile.flush()
            while True:
                try:
                    event = q.get(timeout=15)
                    chunk = b"data: " + json.dumps(event).encode("utf-8") + b"\n\n"
                except queue.Empty:
                    chunk = b": keep-alive\n\n"
                self.wfile.write(chunk)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            pass
        finally:
            hub.unsubscribe(q)

    # -- POST -----------------------------------------------------------------------------------
    def do_POST(self):
        # The body is read before any answer, refusals included: closing a socket with unread
        # bytes makes Windows reset the connection, and the page would see an error, not the reply.
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length < 0 or length > MAX_BODY:
            self.close_connection = True
            return self._refuse(413, "too large")
        body = self.rfile.read(length) if length else b""
        if not self._host_ok():
            return self._refuse(403, "wrong host")
        if not self._token_ok():
            return self._refuse(403, "missing or wrong token")
        name = self.path.split("?", 1)[0].removeprefix("/api/")
        c = self.server.controller
        try:
            if name == "bundle":
                fname = Path(self.headers.get("X-Filename", "bundle.roboflash")).name
                b = c.load_bundle_bytes(body, fname)
                return self._send(200, {"ok": True, "bundle": b.to_dict()})
            action = self.server.actions.get(name)
            if action is None:
                return self._refuse(404, "unknown action")
            try:
                data = json.loads(body or b"{}")
            except ValueError:
                return self._refuse(400, "bad JSON")
            if not isinstance(data, dict):
                return self._refuse(400, "bad JSON")
            result = action(data) or {}
            return self._send(200, {"ok": True, **result})
        except JobFailed as e:
            return self._send(200, {"ok": False, "error": {"title": e.title, "advice": e.advice}})
        except PortError as e:
            return self._send(200, {"ok": False, "error": {"title": e.message, "advice": e.advice}})
        except Exception as e:  # a bug: say so rather than hang the page
            c.log(f"api {name} failed: {e!r}")
            return self._send(200, {"ok": False, "error": {"title": f"Something went wrong: {e}",
                                                           "advice": "Open logs has the details."}})
