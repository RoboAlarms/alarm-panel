#!/usr/bin/env python3
"""RoboAlarms remote development. Requires Python 3.10+; enrollment also uses
pyserial and cryptography (included in the ESP-IDF Python environment).
python tools/panelctl.py --help
Private identities and operations live under ignored secrets/panelctl/.
"""
from __future__ import annotations
import argparse
import contextlib
import hashlib
import hmac
import http.client
import http.server
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import webbrowser

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "secrets" / "panelctl"
try:
    VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
except OSError:
    VERSION = "unknown"

ZWAVE_REGIONS = ("eu", "us", "anz", "hk", "in", "il", "ru", "cn", "us_lr", "eu_lr", "jp", "kr")
ZWAVE_KEYS = ("access", "auth", "unauth", "s0")
ZWAVE_CHECK_INS = {"1h": 3600, "4h": 14400, "12h": 43200}


class DeviceError(RuntimeError):
    def __init__(self, status, message):
        self.status = status
        super().__init__(f"Device {status}: {message}")


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


def alias_path(name):
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,47}", name):
        raise ValueError("Device name must be 1-48 letters, numbers, underscore or hyphen")
    return REGISTRY / (name + ".json")


class PinnedConnection(http.client.HTTPSConnection):
    def __init__(self, host, port, fingerprint, cert, key, timeout=12):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE  # Exact out-of-band certificate pin below replaces CA/name validation.
        ctx.load_cert_chain(cert, key)
        super().__init__(host, port, context=ctx, timeout=timeout)
        self.fingerprint = fingerprint

    def connect(self):
        super().connect()
        actual = hashlib.sha256(self.sock.getpeercert(binary_form=True)).hexdigest()
        if not hmac.compare_digest(actual, self.fingerprint):
            self.close()
            raise ssl.SSLError("Panel identity changed: certificate pin mismatch")


class Client:
    def __init__(self, name, host=None):
        self.name = name
        self.path = alias_path(name)
        self.device = json.loads(self.path.read_text(encoding="utf-8"))
        self.host = host or self.device["host"]
        self.lock = threading.RLock()
        self.conn = None

    def close(self):
        if self.conn:
            self.conn.close()
            self.conn = None

    def request(self, method, endpoint, data=None, headers=None):
        with self.lock:
            if self.conn is None:
                self.conn = PinnedConnection(self.host, self.device["port"], self.device["fingerprint"],
                                             REGISTRY / self.device["cert"], REGISTRY / self.device["key"])
            if isinstance(data, str):
                data = data.encode("ascii")
            try:
                # Flash erase/write can backpressure the socket for much longer than a
                # status request. Firmware independently bounds uploads to five minutes.
                timeout = 330 if endpoint in ("upload", "voice", "asset") and method == "PUT" else 12
                self.conn.timeout = timeout
                if self.conn.sock:
                    self.conn.sock.settimeout(timeout)
                self.conn.request(method, "/dev/v1/" + endpoint, data, headers or {})
                response = self.conn.getresponse()
                result = response.read(4 * 1024 * 1024)
                if response.status >= 300:
                    raise DeviceError(response.status, result.decode('utf-8', 'replace'))
                return result
            except DeviceError:
                raise  # Complete application responses leave TLS reusable (e.g. busy frame).
            except Exception:
                self.close()
                raise  # Mutations are never retried automatically.

    def info(self):
        info = json.loads(self.request("GET", "info"))
        if info["id"] != self.device["id"] or info.get("api") != 1:
            self.close()
            raise RuntimeError("Wrong device identity or unsupported protocol")
        return info


def prepare_network_enrollment(args):
    """Generate a workstation identity without needing a USB connection."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
    import datetime
    dest = alias_path(args.name)
    pending = dest.with_suffix('.pending.json')
    if dest.exists() or pending.exists():
        raise RuntimeError('Alias already exists; do not replace its trust or private key')
    REGISTRY.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'panelctl-' + args.name)])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(datetime.datetime(2020, 1, 1))
            .not_valid_after(datetime.datetime(2070, 1, 1)).add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), False).sign(key, hashes.SHA256()))
    stem = args.name + '-' + secrets.token_hex(4)
    cert_path, key_path = REGISTRY / (stem + '.crt.pem'), REGISTRY / (stem + '.key.pem')
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    os.chmod(key_path, 0o600)
    save(pending, {'cert': cert_path.name, 'key': key_path.name})
    print('Paste this PUBLIC certificate into Panel Manager > Developer > Enroll a workstation:')
    print(cert_path)
    print('Then use import-enrollment with the downloaded panel-enrollment JSON. The private key stays here.')


def import_network_enrollment(args):
    from cryptography import x509
    dest = alias_path(args.name)
    if dest.exists():
        raise RuntimeError('Alias already enrolled; never replace trust implicitly')
    pending = dest.with_suffix('.pending.json')
    identity = json.loads(pending.read_text())
    receipt = json.loads(Path(args.file).read_text())
    if not re.fullmatch(r'[a-f0-9]{12}', receipt.get('id', '')) or receipt.get('port') != 6055:
        raise ValueError('Invalid panel enrollment receipt')
    der = bytes.fromhex(receipt['certificate_der'])
    x509.load_der_x509_certificate(der)
    host = args.host or receipt.get('host', '')
    if not host or len(host) > 253 or any(c in host for c in '/\\@ \r\n\t'):
        raise ValueError('Specify the panel IP or DNS name with --host')
    value = {**identity, 'id': receipt['id'], 'host': host, 'port': 6055,
             'fingerprint': hashlib.sha256(der).hexdigest(), 'enrollment': 'complete'}
    # Receipt came from the authenticated panel-hosted browser. Verify the mTLS
    # pairing before making it the selected registry identity.
    connection = PinnedConnection(host, 6055, value['fingerprint'], REGISTRY / identity['cert'], REGISTRY / identity['key'])
    try:
        connection.request('GET', '/dev/v1/info')
        response = connection.getresponse()
        info = json.loads(response.read())
        if response.status != 200 or info.get('id') != value['id']:
            raise RuntimeError('The enrolled panel identity did not match the receipt')
    finally:
        connection.close()
    save(dest, value)
    pending.unlink()
    print(json.dumps({'enrolled': args.name, 'id': value['id'], 'host': host}))


def enroll(args):
    import serial
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
    import datetime
    dest = alias_path(args.name)
    if dest.exists():
        raise RuntimeError("Alias already enrolled; revoke/remove it explicitly before replacing trust")
    REGISTRY.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "panelctl-" + args.name)])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(datetime.datetime(2020, 1, 1))
            .not_valid_after(datetime.datetime(2070, 1, 1)).add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), False).sign(key, hashes.SHA256()))
    # Save unique credentials first: a serial disconnect after commit must not lose the key.
    stem = args.name + "-" + secrets.token_hex(4)
    cert_path, key_path = REGISTRY / (stem + ".crt.pem"), REGISTRY / (stem + ".key.pem")
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    os.chmod(key_path, 0o600)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    def command(text, expected, timeout=10):
        port.reset_input_buffer()
        port.write((text + "\r").encode())
        buf = b""
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            buf += port.read(4096)
            match = re.search(expected, buf.decode("ascii", "ignore"))
            if match:
                return match
        raise RuntimeError("Serial command failed or timed out: " + text.split()[0])
    try:
        result = command("dev status", r"DEV ID ([a-f0-9]{12}) IP ([0-9.]+) PORT (\d+) ENABLED [01]\s+DEV CERT ([a-f0-9]+)\r?\n")
        device_id, host, service_port, servercert = result.groups()
        save(dest, {"id": device_id, "host": args.host or host, "port": int(service_port),
                    "fingerprint": hashlib.sha256(bytes.fromhex(servercert)).hexdigest(),
                    "cert": cert_path.name, "key": key_path.name, "usb_port": args.port, "enrollment": "pending"})
        command("dev begin", r"DEV BEGIN OK")
        der = cert.public_bytes(serialization.Encoding.DER)
        for pos in range(0, len(der), 80):
            end = min(pos + 80, len(der))
            command("dev chunk " + der[pos:end].hex(), rf"DEV CHUNK {end}\b")
        command("dev commit", r"DEV ENROLLED")
    finally:
        port.close()
    value = json.loads(dest.read_text())
    value["enrollment"] = "complete"
    save(dest, value)
    print(json.dumps({"enrolled": args.name, "id": value["id"], "host": value["host"]}))


def target(args):
    if args.device:
        return Client(args.device, args.host)
    found = []
    for path in REGISTRY.glob("*.json"):
        value = json.loads(path.read_text())
        if "fingerprint" in value and (not args.host or value.get("host") == args.host):
            found.append(path.stem)
    if len(found) != 1:
        raise RuntimeError("Select one enrolled --device (or a uniquely registered --host); discovery never grants trust")
    return Client(found[0], args.host)


def image_info(data):
    if len(data) < 288 or data[0] != 0xE9 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise ValueError("Expected an ESP-IDF application image, not a whole-flash image")
    return {"project": data[80:112].split(b"\0")[0].decode("ascii"), "elf": data[176:208].hex(),
            "version": data[48:80].split(b"\0")[0].decode("ascii"), "chip": struct.unpack_from("<H", data, 12)[0],
            "min_revision": struct.unpack_from("<H", data, 15)[0], "max_revision": struct.unpack_from("<H", data, 17)[0]}


def build_image(args, info):
    rev = "v1" if info["chip_revision"] < 300 else "v3"
    profile = info.get("profile", "dev")
    if profile not in ("dev", "release", "legacy"):
        raise RuntimeError("Unknown firmware profile: " + profile)
    if info["board"] != "crowpanel_p4":
        raise RuntimeError("No build recipe registered for " + info["board"])
    if os.name == "nt":
        cmd = ["pwsh", "-NoProfile", "-File", str(ROOT / "tools/idf.ps1"), "-Profile", profile, "-ChipRev", rev]
        if args.dark: cmd += ["-Dark"]
    else:
        cmd = ["bash", str(ROOT / "tools/idf.sh"), "--profile", profile, "--chip-rev", rev]
        if args.dark: cmd += ["--dark"]
    subprocess.run(cmd + ["build"], cwd=ROOT, check=True)
    suffix = "" if profile == "legacy" else "_" + profile
    app_suffix = "" if profile == "legacy" else "-" + profile
    return ROOT / f"firmware/crowpanel_p4/build_{rev}{suffix}{'_dark' if args.dark else ''}/alarm_crowpanel_p4{app_suffix}.bin"


def usb_info(client, port_name):
    import serial
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False; port.rts = False; port.port = port_name
    port.open()
    def query(command, pattern):
        port.reset_input_buffer(); port.write((command + "\r").encode())
        buf = b""; end = time.monotonic() + 8
        while time.monotonic() < end:
            buf += port.read(4096)
            found = re.search(pattern, buf.decode("ascii", "ignore"))
            if found: return found
        raise RuntimeError("USB identity/status timed out; install dev firmware and enroll first")
    try:
        match = query("dev status", r"DEV ID ([a-f0-9]{12}) IP [0-9.]+ PORT \d+ ENABLED [01]\s+DEV CERT ([a-f0-9]+)\r?\n")
        if match[1] != client.device["id"] or not hmac.compare_digest(hashlib.sha256(bytes.fromhex(match[2])).hexdigest(), client.device["fingerprint"]):
            raise RuntimeError("USB port belongs to a different panel; no reset or flash attempted")
        status = json.loads(query("dev describe", r"DEV INFO (\{[^\r\n]+\})\r?\n")[1])
        if status["id"] != client.device["id"]: raise RuntimeError("USB identity changed")
        return status
    finally: port.close()


def usb_flash(args, client, port):
    status = usb_info(client, port)
    if not status["idle"] or status["probation"]: raise RuntimeError("Panel must be idle/disarmed and outside probation")
    path = build_image(args, status) if args.build else Path(args.image)
    candidate = image_info(path.read_bytes())
    if candidate["project"] != "alarm_crowpanel_p4-dev": raise RuntimeError("panelctl flashes dev apps only; use the explicit release transition procedure")
    # Recheck after a potentially long build. The panel also rechecks before reboot.
    if not usb_info(client, port)["idle"]: raise RuntimeError("Panel is no longer idle")
    operation = {"transport":"usb", "id":status["id"], "elf":candidate["elf"],
                 "old_boot":status["boot"], "result":"uploading", "image":str(path.resolve())}
    save(REGISTRY / (client.name + ".operation.json"), operation)
    cmd = [sys.executable, str(ROOT / "tools/ota_serial.py"), "-p", port, str(path), "--watch", "75"]
    if args.allow_older: cmd += ["--force"]
    subprocess.run(cmd, cwd=ROOT, check=True)
    result = usb_info(client, port)
    if result["elf"] != candidate["elf"] or result["boot"] == status["boot"] or result["probation"]:
        raise RuntimeError("USB update not confirmed; inspect device before retrying")
    operation.update(result="confirmed", boot=result["boot"])
    save(REGISTRY / (client.name + ".operation.json"), operation)
    return result


def wait_update(client, operation, timeout=220):
    deadline = time.monotonic() + timeout
    old_boot = operation["old_boot"]
    while time.monotonic() < deadline:
        try:
            status = client.info()
        except (OSError, http.client.HTTPException, RuntimeError):
            time.sleep(2)
            continue
        if status["boot"] == old_boot:
            time.sleep(1)
            continue
        if status["elf"] != operation["elf"]:
            raise RuntimeError("Device restarted into another image (rollback); candidate not accepted")
        if status["operation"] != operation["operation"]:
            raise RuntimeError("Device boot has a different update operation")
        if status["confirm_required"]:
            client.request("POST", "confirm", operation["operation"] + " " + status["boot"])
        if not status["probation"]:
            operation["result"] = "confirmed"
            save(REGISTRY / (client.name + ".operation.json"), operation)
            return status
        time.sleep(2)
    raise RuntimeError("Update outcome unknown: use update-status; no automatic reflash")


def flash(args, client):
    status = client.info()
    if not status["idle"] or status["probation"]:
        raise RuntimeError("Panel must be idle/disarmed and outside probation")
    path = build_image(args, status) if args.build else Path(args.image)
    data = path.read_bytes()
    info = image_info(data)
    profile = status.get("profile", "dev")
    project = "alarm_crowpanel_p4" + ("" if profile == "legacy" else "-" + profile)
    if profile not in ("dev", "release", "legacy") or info["project"] != project or status["board"] != "crowpanel_p4":
        raise RuntimeError("Network flash requires the matching board and firmware profile")
    if len(data) > status["slot_size"] or status["chip_revision"] < info["min_revision"] or (info["max_revision"] and status["chip_revision"] > info["max_revision"]):
        raise RuntimeError("Image does not fit this device's slot or silicon revision")
    operation = {"operation": secrets.token_hex(16), "id": status["id"], "elf": info["elf"],
                 "sha256": hashlib.sha256(data).hexdigest(), "old_boot": status["boot"], "result": "uploading", "image": str(path.resolve())}
    save(REGISTRY / (client.name + ".operation.json"), operation)
    headers = {"Content-Type": "application/octet-stream", "X-Image-SHA256": operation["sha256"],
               "X-Operation": operation["operation"], "X-Allow-Older": "yes" if args.allow_older else "no"}
    print("Uploading signed app to " + client.name, file=sys.stderr)
    staged = json.loads(client.request("PUT", "upload", data, headers))
    if staged["staged_elf"] != operation["elf"] or staged["staged_operation"] != operation["operation"]:
        raise RuntimeError("Unexpected staged image; install refused")
    operation["result"] = "staged"
    save(REGISTRY / (client.name + ".operation.json"), operation)
    client.request("POST", "install", operation["operation"])
    print("Waiting for authenticated reconnect and health probation", file=sys.stderr)
    return wait_update(client, operation)


def png565(raw, width, height):
    import zlib
    if len(raw) != width * height * 2:
        raise ValueError("Incomplete screen frame")
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            v = struct.unpack_from("<H", raw, (y * width + x) * 2)[0]
            rows.extend((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def bundle(client, output):
    import zipfile
    info = client.info()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("info.json", json.dumps(info, indent=2))
        archive.writestr("diagnostics.txt", client.request("GET", "diagnostics"))
        archive.writestr("service-log.txt", client.request("GET", "logs"))
        try:
            raw = client.request("GET", "frame")
            archive.writestr("screen.png", png565(raw, info["capture_width"], info["capture_height"]))
        except DeviceError as exc:
            if exc.status not in (423,503): raise
            archive.writestr("screen-unavailable.txt", str(exc))
    return {"saved":str(Path(output).resolve()), "id":info["id"]}


def bridge_authorized(headers, port, token):
    host = f"127.0.0.1:{port}"
    origin = headers.get("Origin")
    return (headers.get("Host") == host and (origin is None or origin == "http://" + host)
            and hmac.compare_digest(headers.get("X-Session", ""), token))


def viewer(args, client):
    token = secrets.token_urlsafe(32)
    document = (ROOT / "tools/panelctl_web.html").read_bytes()
    info = client.info()
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_): pass  # Never log session credentials or input.
        def reply(self, code, payload, mime="text/plain"):
            with contextlib.suppress(BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                self.send_response(code)
                self.send_header("Content-Type", mime)
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.send_header("Referrer-Policy", "no-referrer")
                self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' blob:; frame-ancestors 'none'")
                self.end_headers()
                self.wfile.write(payload)
        def valid(self):
            return bridge_authorized(self.headers, self.server.server_port, token)
        def do_GET(self):
            if self.path == "/" and self.headers.get("Host") == f"127.0.0.1:{self.server.server_port}":
                return self.reply(200, document, "text/html; charset=utf-8")
            if not self.valid(): return self.reply(403, b"Unauthorized")
            try:
                if self.path == "/info": return self.reply(200, json.dumps(client.info()).encode(), "application/json")
                if self.path == "/frame": return self.reply(200, client.request("GET", "frame"), "application/octet-stream")
                return self.reply(404, b"Not found")
            except DeviceError as exc: return self.reply(exc.status, str(exc).encode())
            except Exception as exc: return self.reply(503, str(exc).encode())
        def do_POST(self):
            if not self.valid() or self.path != "/control": return self.reply(403, b"Unauthorized")
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 < size < 160: return self.reply(400, b"Invalid length")
                data = self.rfile.read(size)
                return self.reply(200, client.request("POST", "control", data))
            except DeviceError as exc: return self.reply(exc.status, str(exc).encode())
            except Exception as exc: return self.reply(409, str(exc).encode())
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    url = f"http://127.0.0.1:{server.server_port}/#{token}"
    print(f"Viewer for {client.name} running at http://127.0.0.1:{server.server_port}/ (session opened in browser)")
    if args.url_file: Path(args.url_file).write_text(url, encoding="utf-8")
    if not args.no_browser: webbrowser.open(url)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: server.server_close()  # Device lease releases after two seconds without input.


def discover(subnet=None, interface=None):
    """Use optional zeroconf for multicast; an explicit VPN address needs no discovery package."""
    if subnet:
        from concurrent.futures import ThreadPoolExecutor
        network = ipaddress.ip_network(subnet, strict=False)
        if network.version != 4 or network.num_addresses > 256 or not network.is_private:
            raise ValueError("Specify a private IPv4 subnet no larger than /24")
        def probe(address):
            try:
                with socket.create_connection((str(address), 6055), timeout=0.4):
                    return {"addresses":[str(address)], "port":6055, "trusted":False, "note":"TCP candidate; select an enrolled identity to authenticate"}
            except OSError: return None
        with ThreadPoolExecutor(max_workers=12) as pool:
            return [result for result in pool.map(probe, network.hosts()) if result]
    try:
        from zeroconf import Zeroconf, ServiceBrowser, ServiceListener
    except ImportError as exc:
        raise RuntimeError("Install tools/panelctl-requirements.txt for mDNS discovery; --host works without it") from exc
    results = {}
    class Listener(ServiceListener):
        def add_service(self, zc, type_, name):
            info = zc.get_service_info(type_, name, timeout=1500)
            if info:
                results[name] = {"name": name, "addresses": info.parsed_addresses(), "port": info.port,
                                 "id": info.properties.get(b"id", b"").decode("ascii", "replace"), "trusted": False}
        def update_service(self, zc, type_, name): self.add_service(zc, type_, name)
        def remove_service(self, *_): pass
    with Zeroconf(**({"interfaces":[interface]} if interface else {})) as zc:
        browser = ServiceBrowser(zc, "_roboalarms-dev._tcp.local.", Listener())
        time.sleep(4)
        browser.cancel()
    return list(results.values())


# --- Z-Wave (docs/features/36 Z9): GET zwave / GET zwave/trace / POST zwave on the developer link ---

def zwave_request(client, method, endpoint, data=None, headers=None, no_route=""):
    """GET/POST with the contract's 404/503 handling: 404 means the route doesn't exist on this
    firmware (state/requests need any Z-Wave build; trace needs a dev build); 503 means the panel
    could not build the answer right now, worth a couple of quick retries."""
    for attempt in range(3):
        try:
            return client.request(method, endpoint, data, headers)
        except DeviceError as exc:
            if exc.status == 404:
                raise RuntimeError(no_route or "This route does not exist on this panel's firmware") from exc
            if exc.status == 503 and attempt < 2:
                time.sleep(1)
                continue
            raise


def zwave_status(client):
    return json.loads(zwave_request(client, "GET", "zwave",
                       no_route="This firmware has no Z-Wave (not built in, or the radio option is off)"))


def zwave_post(client, op, **fields):
    body = json.dumps({"op": op, **fields})
    return json.loads(zwave_request(client, "POST", "zwave", body, {"Content-Type": "application/json"},
                       no_route="This firmware has no Z-Wave (not built in, or the radio option is off)"))


def zwave_act(client, op, **fields):
    """Run one network op, print its result word and exit 1 when it isn't ok."""
    result = zwave_post(client, op, **fields)
    print(result["result"])
    if result["result"] != "ok":
        sys.exit(1)


def zwave_generation(status, node):
    for device in status.get("devices", []):
        if device["node"] == node:
            return device["generation"]
    raise RuntimeError(f"Node {node} is not a known Z-Wave device on this panel")


def zwave_status_text(status):
    r = status["radio"]
    lines = [f"Radio: {r['state']} · place {r['place']} · chip {r['chip']} · region {r['region']} · "
             f"home {r['home_id']} · node {r['node_id']}",
             f"Firmware {r['firmware']} · SDK {r['sdk']} · crypto self-test: {r['crypto']} · "
             f"{r['devices']} device(s) · default check-in {r['check_in_s']}s" +
             (" · update advised" if r.get("update_advised") else "")]
    devices = status.get("devices", [])
    lines.append(f"Devices ({len(devices)}):" if devices else "Devices: none")
    for d in devices:
        battery = "unknown" if d["battery"] == -1 else ("low" if d.get("battery_low") else f"{d['battery']}%")
        heard = "never" if d["heard_s"] == -1 else f"{d['heard_s']}s ago"
        zones = ",".join(str(z) for z in d.get("zones", [])) or "none"
        flag = " · UNREACHABLE" if d.get("unreachable") else ""
        lines.append(f"  node {d['node']} (gen {d['generation']}) {d['model']} · {d['security']} · "
                     f"{d['state']} · battery {battery} · heard {heard} · zones [{zones}]{flag}")
    p = status.get("progress", {})
    if p.get("op", "none") == "none":
        lines.append("Operation: none")
    else:
        bits = [f"{p['op']} · {p['step']}"]
        if p.get("seconds_left"):
            bits.append(f"{p['seconds_left']}s left")
        if p.get("node"):
            bits.append(f"node {p['node']} (gen {p.get('generation', 0)})")
        if p.get("result", "none") != "none":
            bits.append(f"result {p['result']}")
        if p.get("requested"):
            bits.append("requested " + ",".join(p["requested"]))
        if p.get("dsk"):
            bits.append(f"dsk {p['dsk']}")
        lines.append("Operation: " + " · ".join(bits))
    return "\n".join(lines)


def zwave_trace_poll(client, since):
    return json.loads(zwave_request(client, "GET", f"zwave/trace?since={since}",
                       no_route="This firmware has no Z-Wave trace (dev builds only)"))


def zwave_started_frame(frame):
    """A complete, checksum-valid SerialAPIStarted from the radio, not stale status."""
    if frame.get("dir") != "rx":
        return False
    try:
        raw = bytes.fromhex(frame["hex"])
    except (KeyError, TypeError, ValueError):
        return False
    if len(raw) < 11 or raw[0] != 1 or raw[1] + 2 != len(raw) or raw[2:4] != b"\x00\x0a":
        return False
    checksum = 0xff
    for byte in raw[1:]:
        checksum ^= byte
    return checksum == 0


def zwave_trace_line(frame):
    return f"{frame['seq']} {frame['t']} {frame['dir']} {frame['hex']}"


def zwave_trace_header(client):
    """The zwtrace file's header (version 1): tool version and module facts, never IDs."""
    r = zwave_status(client)["radio"]
    return ["zwtrace 1", f"# panelctl, RoboAlarms {VERSION}",
            f"# module firmware {r['firmware']} sdk {r['sdk']} chip {r['chip']}",
            f"# captured {time.strftime('%Y-%m-%d')}"]


class TraceSink:
    """Appends trace lines to a file, writing the zwtrace header once for a new/empty file."""
    def __init__(self, path, header):
        path = Path(path)
        fresh = not path.exists() or path.stat().st_size == 0
        path.parent.mkdir(parents=True, exist_ok=True)
        self.file = path.open("a", encoding="utf-8", newline="\n")
        if fresh:
            self.write(header)

    def write(self, lines):
        if lines:
            self.file.write("\n".join(lines) + "\n")
            self.file.flush()

    def close(self):
        self.file.close()


def zwave_trace_fetch(client, since):
    """One poll: returns (next_since, printable_lines, doc). A since past the newest entry means
    the panel restarted (sequence numbers restart at 1 each boot); when next or the frames' seq
    go backwards from what we asked for, note it with a '# panel restarted' line and carry on."""
    doc = zwave_trace_poll(client, since)
    lines = []
    if doc["next"] < since:
        lines.append("# panel restarted")
    if doc.get("lost"):
        lines.append(f"lost {doc['lost']}")
    lines += [zwave_trace_line(f) for f in doc["frames"]]
    return doc["next"], lines, doc


def zwave_trace_cmd(args, client):
    sink = TraceSink(args.output, zwave_trace_header(client)) if args.output else None
    try:
        # Default: skip whatever the panel already buffered and watch only from here on;
        # --all replays the buffer too (as far as it still goes back).
        since = 0
        if not args.all:  # find the end: drain what's buffered (64 entries an answer)
            doc = zwave_trace_poll(client, 0)
            while len(doc["frames"]) >= 64:
                doc = zwave_trace_poll(client, doc["next"])
            since = doc["next"]
        while True:
            since, lines, doc = zwave_trace_fetch(client, since)
            for line in lines:
                print(line, flush=True)
            if sink:
                sink.write(lines)
            if len(doc["frames"]) >= 64:
                continue  # the answer came back full: more is likely still buffered, poll at once
            if not args.follow:
                return
            time.sleep(1)
    finally:
        if sink:
            sink.close()


def zwave_confirm(prompt):
    try:
        return input(prompt + " [y/N] ").strip().lower() in ("y", "yes")
    except EOFError:
        return False


def zwave_ask(prompt):
    try:
        return input(prompt + " (Enter to continue, 's' to skip) ").strip().lower() != "s"
    except EOFError:
        return False


def zwave_check_window(client, pump, record, op, nothing_result):
    """Steps (c)/(d) of the bench check: open a listen window, stop it, dismiss."""
    result = zwave_post(client, op)
    if result["result"] != "ok":
        record(f"{op}: starts", False, result["result"])
        return
    time.sleep(1)
    pump()
    progress = zwave_status(client)["progress"]
    pump()
    record(f"{op}: listening", progress.get("op") == op and progress.get("step") == "listening",
           f"step={progress.get('step')}")
    time.sleep(3)
    pump()
    stop_result = zwave_post(client, "stop")
    pump()
    record(f"{op}: stop", stop_result["result"] == "ok", stop_result["result"])
    time.sleep(1)
    pump()
    progress = zwave_status(client)["progress"]
    pump()
    # an operation that ended reads "done" with its result until it is dismissed
    ended_ok = progress.get("step") == "done" and progress.get("result") in (nothing_result, "stopped")
    record(f"{op}: ended", ended_ok, f"step={progress.get('step')} result={progress.get('result')}")
    dismiss_result = zwave_post(client, "dismiss")
    pump()
    record(f"{op}: dismiss", dismiss_result["result"] == "ok", dismiss_result["result"])


def zwave_write_report(out_dir, steps, status_before, status_after):
    verdict = "FAIL" if any(s["result"] == "FAIL" for s in steps) else \
              ("WARN" if any(s["result"] == "WARN" for s in steps) else "PASS")
    lines = [f"# Z-Wave bench check - {time.strftime('%Y-%m-%d %H:%M:%S')}", "", f"Overall: **{verdict}**", "",
             "| Step | Result | Reason |", "|---|---|---|"]
    lines += [f"| {s['step']} | {s['result']} | {s['reason'].replace('|', '/')} |" for s in steps]
    (out_dir / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (out_dir / "steps.json").write_text(json.dumps(steps, indent=2) + "\n", encoding="utf-8")
    (out_dir / "status_initial.json").write_text(json.dumps(status_before or {}, indent=2) + "\n", encoding="utf-8")
    (out_dir / "status_final.json").write_text(json.dumps(status_after or {}, indent=2) + "\n", encoding="utf-8")
    return verdict


def zwave_check_sensor(client, pump, record):
    """--sensor: a guided pass for one device, prompting on the console; 's' skips a step."""
    print()
    print("Sensor pass: one device, guided. Every step can be skipped with 's'.")
    if not zwave_ask("Put the sensor in pairing mode now"):
        return record("sensor: add", "WARN", "skipped")
    result = zwave_post(client, "add")
    if result["result"] != "ok":
        return record("sensor: add starts", False, result["result"])
    node = generation = 0
    added_result = "none"
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline:
        pump()
        progress = zwave_status(client)["progress"]
        step = progress.get("step")
        if step == "ask_allow":
            keys = progress.get("requested") or list(ZWAVE_KEYS[:3])
            print(f"The device asked for: {', '.join(keys)}")
            if zwave_ask("Grant them"):
                zwave_post(client, "allow", keys=keys)
            else:
                zwave_post(client, "stop")
                return record("sensor: secure connection", "WARN", "skipped by installer")
        elif step == "ask_pin":
            print(f"Compare with the sensor's label; the rest of the key: {progress.get('dsk', '')}")
            pin = input("Enter its 5-digit PIN ('s' to skip): ").strip()
            if pin.lower() == "s":
                zwave_post(client, "stop")
                return record("sensor: PIN", "WARN", "skipped by installer")
            zwave_post(client, "pin", pin=pin)
        elif step == "done" and progress.get("result", "none") != "none":
            node, generation, added_result = progress.get("node", 0), progress.get("generation", 0), progress["result"]
            break
        time.sleep(1)
    if added_result not in ("added", "added_insecure", "downgraded"):
        return record("sensor: add", False, f"result={added_result or 'timed out'}")
    record("sensor: added", True, f"node {node}, result={added_result}")

    if zwave_ask("Wait for its interview to finish (up to 10 minutes; it may need waking)"):
        deadline, state = time.monotonic() + 600, "setting_up"
        while time.monotonic() < deadline:
            pump()
            match = next((d for d in zwave_status(client)["devices"] if d["node"] == node), None)
            state = match["state"] if match else "gone"
            if state == "ready":
                break
            time.sleep(2)
        record("sensor: interview finished", state == "ready", f"state={state}")
    else:
        record("sensor: interview finished", "WARN", "skipped")

    if zwave_ask("Open then close the sensor"):
        before = next((d for d in zwave_status(client)["devices"] if d["node"] == node), None)
        heard_before = before["heard_s"] if before else -1
        time.sleep(3)
        doc = pump()
        after = next((d for d in zwave_status(client)["devices"] if d["node"] == node), None)
        heard_after = after["heard_s"] if after else -1
        rx_seen = any(f["dir"] == "rx" for f in doc.get("frames", []))
        heard = rx_seen or heard_before == -1 or (heard_after != -1 and heard_after < heard_before)
        record("sensor: heard", heard, f"heard_s {heard_before}->{heard_after}, rx frames seen={rx_seen}")
    else:
        record("sensor: heard", "WARN", "skipped")

    if zwave_ask("Change its check-in to 4 hours, then wake it by hand"):
        want = ZWAVE_CHECK_INS["4h"]
        result = zwave_post(client, "check_in", node=node, generation=generation, seconds=want)
        match = next((d for d in zwave_status(client)["devices"] if d["node"] == node), None)
        waiting = match is not None and match.get("check_in_next_s") == want
        record("sensor: check-in waits for its wake-up", result["result"] == "ok" and waiting,
               f"result={result['result']}, check_in_next_s={match.get('check_in_next_s') if match else '-'}")
        input("Press Enter once you've woken the sensor: ")
        deadline, took = time.monotonic() + 30, False
        while time.monotonic() < deadline and not took:
            pump()
            match = next((d for d in zwave_status(client)["devices"] if d["node"] == node), None)
            took = match is not None and match.get("check_in_s") == want and not match.get("check_in_next_s")
            if not took:
                time.sleep(1)
        record("sensor: check-in changed", took,
               f"check_in_s={match.get('check_in_s') if match else '-'} (want {want})")
    else:
        record("sensor: check-in changed", "WARN", "skipped")

    if zwave_ask("Put the sensor in pairing mode again to remove it"):
        result = zwave_post(client, "remove")
        removed_result = "none"
        if result["result"] != "ok":
            record("sensor: remove starts", False, result["result"])
        else:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                pump()
                progress = zwave_status(client)["progress"]
                if progress.get("step") == "done":
                    removed_result = progress.get("result", "none")
                    break
                time.sleep(1)
            zwave_post(client, "dismiss")
            pump()
            gone = not any(d["node"] == node for d in zwave_status(client)["devices"])
            record("sensor: removed", removed_result in ("removed", "removed_foreign") and gone,
                   f"result={removed_result}, gone={gone}")
    else:
        record("sensor: removed", "WARN", "skipped")


def zwave_check(args, client):
    print("This restarts the Z-Wave radio and opens the add and remove windows for a few seconds; "
          "the panel must be disarmed.")
    if not args.yes and not zwave_confirm("Continue?"):
        raise RuntimeError("Bench check refused: pass --yes or answer 'y' to the prompt")
    out_dir = Path(args.output_dir) if args.output_dir else ROOT / "build" / ("zwave-check-" + time.strftime("%Y-%m-%d"))
    out_dir.mkdir(parents=True, exist_ok=True)
    header = zwave_trace_header(client)
    sink = TraceSink(out_dir / "check.zwtrace", header)
    since = zwave_trace_poll(client, 0)["next"]
    steps = []
    status0 = final = None

    def pump(extra=None):
        nonlocal since
        frames = []
        while True:
            since, lines, doc = zwave_trace_fetch(client, since)
            sink.write(lines)
            if extra:
                extra.write(lines)
            frames += doc["frames"]
            if len(doc["frames"]) < 64:
                return {"frames": frames}
            # The answer came back full: more is likely still buffered, drain it before returning.

    def record(name, ok, reason=""):
        word = "WARN" if ok == "WARN" else ("PASS" if ok else "FAIL")
        steps.append({"step": name, "result": word, "reason": reason})
        print(f"{word:<4} {name}" + (f" - {reason}" if reason else ""))

    def wait_for_restart(extra=None):
        deadline, state, started = time.monotonic() + 30, "", False
        while time.monotonic() < deadline:
            state = zwave_status(client)["radio"]["state"]
            frames = pump(extra)["frames"]
            started = started or any(zwave_started_frame(f) for f in frames)
            if state == "working" and started:
                return True, state, started
            time.sleep(1)
        return False, state, started

    try:
        try:
            # (a) status
            status0 = zwave_status(client); pump()
            radio = status0["radio"]
            record("status: radio state", radio["state"] == "working", radio["state"])
            record("status: crypto self-test", radio["crypto"] == "passed", radio["crypto"])
            record("status: chip is 800-series", radio["chip"] == 8, f"chip {radio['chip']}")
            try:
                sdk_ok = tuple(int(p) for p in radio["sdk"].split(".")) >= (7, 22, 1)
            except (ValueError, AttributeError, KeyError):
                sdk_ok = False
            record("status: firmware/SDK", True if sdk_ok else "WARN",
                   f"firmware {radio.get('firmware')} sdk {radio.get('sdk')}")
            record("status: node id", radio["node_id"] == 1, f"node {radio['node_id']}")
            record("status: region known", radio["region"] in ZWAVE_REGIONS, radio["region"])

            # (b) restart
            home0, count0 = radio["home_id"], radio["devices"]
            startup_sink = TraceSink(out_dir / "startup.zwtrace", header)
            result = zwave_post(client, "restart")
            if result["result"] != "ok":
                record("restart: request", False, result["result"])
            else:
                returned, state, started = wait_for_restart(startup_sink)
                pump(startup_sink)
                record("restart: back to working", returned,
                       f"state={state}, SerialAPIStarted={started}")
                if returned:
                    after = zwave_status(client)["radio"]
                    record("restart: network unchanged", after["home_id"] == home0 and after["devices"] == count0,
                           f"home {home0}->{after['home_id']}, devices {count0}->{after['devices']}")
            startup_sink.close()

            # (c)/(d) add and remove windows
            zwave_check_window(client, pump, record, "add", "nothing_joined")
            zwave_check_window(client, pump, record, "remove", "nothing_removed")

            # (e) pulse-reset
            pump()  # no earlier SerialAPIStarted may satisfy this reset
            result = zwave_post(client, "pulse_reset")
            if result["result"] == "unsupported":
                record("pulse-reset", "WARN", "not supported by this hardware revision (skipped)")
            elif result["result"] != "ok":
                record("pulse-reset: request", False, result["result"])
            else:
                returned, state, started = wait_for_restart()
                record("pulse-reset: radio returned", returned,
                       f"state={state}, SerialAPIStarted={started}")

            # (f) final status
            final = zwave_status(client); pump()
            record("final status", final["radio"]["state"] == "working", final["radio"]["state"])

            if args.sensor:
                zwave_check_sensor(client, pump, record)
        finally:
            sink.close()
    except Exception as exc:
        steps.append({"step": "check", "result": "FAIL", "reason": f"aborted: {exc}"})
        zwave_write_report(out_dir, steps, status0, final)
        raise

    verdict = zwave_write_report(out_dir, steps, status0, final)
    print(f"Report: {out_dir / 'report.md'} ({verdict})")
    if any(s["result"] == "FAIL" for s in steps):
        sys.exit(1)


def zwave_command(args, client):
    cmd = args.zwave_command
    if cmd == "status":
        status = zwave_status(client)
        print(json.dumps(status, indent=2) if args.json else zwave_status_text(status))
        return
    if cmd == "trace":
        return zwave_trace_cmd(args, client)
    if cmd == "check":
        return zwave_check(args, client)
    if cmd == "allow":
        if not args.keys:
            # Absent "keys" means the panel grants what the device asked for.
            return zwave_act(client, "allow")
        keys = [k.strip() for k in args.keys.split(",") if k.strip()]
        bad = [k for k in keys if k not in ZWAVE_KEYS]
        if bad:
            raise ValueError("Unknown key(s) " + ",".join(bad) + "; choose from " + ",".join(ZWAVE_KEYS))
        return zwave_act(client, "allow", keys=keys)
    if cmd == "pin":
        if not re.fullmatch(r"\d{5}", args.pin) or int(args.pin) > 65535:
            raise ValueError("PIN must be exactly 5 digits, 00000-65535 (the key's first group)")
        return zwave_act(client, "pin", pin=args.pin)
    if cmd == "remove-failed":
        generation = zwave_generation(zwave_status(client), args.node)
        return zwave_act(client, "remove_failed", node=args.node, generation=generation)
    if cmd == "setup":  # continue a device's setup (interview) from where it stopped
        generation = zwave_generation(zwave_status(client), args.node)
        return zwave_act(client, "setup", node=args.node, generation=generation)
    if cmd == "ping":
        generation = zwave_generation(zwave_status(client), args.node)
        return zwave_act(client, "ping", node=args.node, generation=generation)
    if cmd == "check-in":
        seconds = ZWAVE_CHECK_INS[args.duration]
        if args.node == "new":
            return zwave_act(client, "check_in", node=0, generation=0, seconds=seconds)
        try:
            node = int(args.node)
        except ValueError:
            raise ValueError("check-in node must be a number or 'new'")
        generation = zwave_generation(zwave_status(client), node)
        return zwave_act(client, "check_in", node=node, generation=generation, seconds=seconds)
    if cmd == "region":
        return zwave_act(client, "region", region=args.region)
    if cmd == "reset-network":
        if not args.confirm:
            raise RuntimeError("reset-network needs --confirm (refused locally without it)")
        return zwave_act(client, "reset_network", confirm=True)
    return zwave_act(client, cmd.replace("-", "_"))


# --- Zigbee (docs/features/20 devices.md §12, BL-143): GET zigbee / POST zigbee on the developer link ---

ZIGBEE_NO_ROUTE = "This firmware has no Zigbee (not built in, or the panel answers no Zigbee)"
ZIGBEE_CHANNELS = range(11, 27)  # IEEE 802.15.4 channels 11..26 (ALARM_ZB_CHANNEL_MIN..MAX)


SPEAKER_NO_ROUTE = "This firmware has no speaker switch (development builds have it from 0.8.0)"


def speaker(client, state=None):
    """The bench silence (docs/features/08 AUD-025): 'off' keeps the speaker off, alarms included.
    A 500 that still answers 'off' means silent now but not kept across a restart."""
    try:
        if state is not None:
            client.request("POST", "speaker", state)
        now = client.request("GET", "speaker").decode("ascii", "replace").strip()
    except DeviceError as exc:
        if exc.status == 404:
            raise RuntimeError(SPEAKER_NO_ROUTE) from exc
        raise
    return {"speaker": now, "id": client.device["id"]}


# --- Voice pack (docs/features/08 voice-first-build.md D12): PUT/GET voice on the developer link ---

VOICE_NO_ROUTE = "This firmware has no voice-pack route (it comes with the voice feature)"
VOICE_MIN, VOICE_MAX, VOICE_SECTOR = 8192, 970752, 4096
VOICE_RESULTS = {
    "ok": "Installed. Announcements stay as they were (switch them on in Settings or the Panel Manager).",
    "unavailable": "This build has no voice feature, or the panel has no pack store.",
    "too_big": "The panel refused the file's size.",
    "bad_signature": "The panel refused the file: it is not signed for this panel.",
    "bad_pack": "The panel refused the file: signed, but not a valid voice pack.",
    "locked": "Not now: the panel is armed, in an alarm, entry or exit, a walk or fire test, or maintenance. Disarm and leave those first.",
    "busy": "Not now: an update or another voice install is running. Try again in a minute.",
    "no_space": "The panel has no room to hold the new pack beside the working one.",
    "failed": "The panel could not write the pack to its flash; the working pack is unchanged.",
}


def voice_check(path):
    """The file's bytes, after the checks the panel makes first (size and envelope); a bad file is
    refused here, before the panel is contacted. The signature itself is the panel's to check."""
    data = Path(path).read_bytes()
    n = len(data)
    if n % VOICE_SECTOR or not VOICE_MIN <= n <= VOICE_MAX:
        raise RuntimeError(f"{path}: {n} bytes is not a voice pack (a multiple of {VOICE_SECTOR} in {VOICE_MIN}..{VOICE_MAX})")
    if data[:4] != b"RVOC":
        raise RuntimeError(f"{path}: not a .robovoice file (no RVOC header)")
    length = struct.unpack_from("<I", data, 8)[0]
    if -(-length // VOICE_SECTOR) * VOICE_SECTOR + VOICE_SECTOR != n:
        raise RuntimeError(f"{path}: the payload length {length} does not fit a {n}-byte signed file")
    return data


def voice_install(client, path):
    data = voice_check(path)
    try:
        body = client.request("PUT", "voice", data, {"Content-Type": "application/octet-stream"})
    except DeviceError as exc:
        if exc.status == 404:
            raise RuntimeError(VOICE_NO_ROUTE) from exc
        found = re.search(r'"result"\s*:\s*"(\w+)"', str(exc))
        word = found.group(1) if found else None
        if word in VOICE_RESULTS:
            raise RuntimeError(f"{word}: {VOICE_RESULTS[word]}") from exc
        raise
    word = json.loads(body).get("result")
    return {"result": word, "message": VOICE_RESULTS.get(word, "The panel answered " + str(word)), "id": client.device["id"]}


def voice_status(client):
    try:
        return json.loads(client.request("GET", "voice"))
    except DeviceError as exc:
        if exc.status == 404:
            raise RuntimeError(VOICE_NO_ROUTE) from exc
        raise


def voice_status_text(st):
    if st.get("state") != "ready":
        text = "Voice: " + str(st.get("state"))
        return text + (" (" + st["why"] + ")" if st.get("why") else "")
    text = (f"Voice: ready, {st.get('locale')} {st.get('voice_id')}, revision {st.get('pack_revision')}, "
            f"{st.get('clips')} clips, {st.get('file_bytes')} bytes" + (", DRAFT" if st.get("draft") else ""))
    if "volume" in st:  # firmware from 0.8.14
        text += (f"; Voice volume {st['volume']} %, {st.get('word_gap_ms')} ms between words, "
                 f"announcements {'on' if st.get('enabled') else 'off'}")
    return text


def voice_volume(client, volume):
    """Development builds: the panel's Voice volume (0-100), saved as its Settings slider saves it;
    the voice's state after it."""
    if not 0 <= int(volume) <= 100:
        raise ValueError("The Voice volume is 0 to 100")
    try:
        return json.loads(client.request("POST", "voice", f"volume {int(volume)}", {"Content-Type": "text/plain"}))
    except DeviceError as exc:
        if exc.status in (404, 405):
            raise RuntimeError("This firmware cannot set the Voice volume (a development build from 0.8.14 can)") from exc
        raise


BENCH_RATE, BENCH_MAX_SECONDS = 16000, 30
BENCH_RESULTS = {
    "ok": "Playing on the panel's speaker (after anything it is saying now).",
    "unavailable": "Not now: a release build, or the panel has no voice pack loaded yet.",
    "too_long": f"The panel takes 16 kHz audio of at most {BENCH_MAX_SECONDS} s.",
    "no_space": "The panel has no memory free for the audio.",
}


def bench_pcm(path):
    """The samples of a 16-bit mono 16 kHz WAV (tools/voice/voice.py try writes them), refused
    here before the panel is contacted."""
    import wave
    with wave.open(str(path), "rb") as w:
        shape = (w.getnchannels(), w.getsampwidth(), w.getframerate())
        if shape != (1, 2, BENCH_RATE):
            raise RuntimeError(f"{path}: {shape[0]} channel(s), {8 * shape[1]}-bit, {shape[2]} Hz; "
                               f"the panel plays 16-bit mono {BENCH_RATE} Hz")
        data = w.readframes(w.getnframes())
    if not data or len(data) > 2 * BENCH_RATE * BENCH_MAX_SECONDS:
        raise RuntimeError(f"{path}: {len(data) // 2 / BENCH_RATE:.1f} s; the panel plays up to {BENCH_MAX_SECONDS} s")
    return data


def voice_play(client, path):
    """Development builds (docs/features/08 AUD-030): the WAV through the panel's speech path."""
    data = bench_pcm(path)
    try:
        body = client.request("PUT", "play", data, {"Content-Type": "application/octet-stream"})
    except DeviceError as exc:
        if exc.status == 404:
            raise RuntimeError("This firmware has no audio bench (a development build with the voice feature has it)") from exc
        found = re.search(r'"result"\s*:\s*"(\w+)"', str(exc))
        word = found.group(1) if found else None
        if word in BENCH_RESULTS:
            raise RuntimeError(f"{word}: {BENCH_RESULTS[word]}") from exc
        raise
    word = json.loads(body).get("result")
    return {"result": word, "message": BENCH_RESULTS.get(word, "The panel answered " + str(word)),
            "seconds": len(data) / 2 / BENCH_RATE}


SAY_GAP_MAX_MS = 250  # panel_voice.h PANEL_VOICE_BENCH_GAP_MAX_MS
SAY_RESULTS = {
    "ok": "Saying it on the panel (after anything more important).",
    "unavailable": "Not now: a release build, or the panel has no voice pack loaded yet.",
    "bad_words": "The panel's pack lacks one of those clip IDs.",
    "too_long": "Over 10 s with its gaps.",
    "quiet": "Nothing plays: Voice volume 0, power saving, an alarm, or four more important requests waiting.",
}


def say_ids(ids):
    """Clip IDs as the panel takes them (decimal or 0x hex, 1 to 8, each 1-0xFFFF); ValueError
    otherwise, before the panel is contacted."""
    out = []
    for text in ids:
        try:
            value = int(str(text), 0)
        except ValueError:
            raise ValueError(f"Not a clip ID: {text}") from None
        if not 0 < value <= 0xFFFF:
            raise ValueError(f"Not a clip ID: {text}")
        out.append(value)
    if not 0 < len(out) <= 8:
        raise ValueError("One to eight clip IDs")
    return out


def voice_say(client, ids, gap=0):
    """Development builds (docs/features/08 AUD-030): clip IDs said by the panel from its own pack,
    with its own renderer and Voice volume; `gap` ms between the words (kept until the next say; 0
    the pack's)."""
    values = say_ids(ids)
    if not 0 <= int(gap) <= SAY_GAP_MAX_MS:
        raise ValueError(f"The gap is 0 to {SAY_GAP_MAX_MS} ms")
    data = " ".join(str(v) for v in [int(gap)] + values)
    try:
        body = client.request("POST", "say", data, {"Content-Type": "text/plain"})
    except DeviceError as exc:
        if exc.status == 404:
            raise RuntimeError("This firmware cannot say words for the bench (a development build from 0.8.13 can)") from exc
        found = re.search(r'"result"\s*:\s*"(\w+)"', str(exc))
        word = found.group(1) if found else None
        if word in SAY_RESULTS:
            raise RuntimeError(f"{word}: {SAY_RESULTS[word]}") from exc
        raise
    answer = json.loads(body)
    word = answer.get("result")
    return {"result": word, "message": SAY_RESULTS.get(word, "The panel answered " + str(word)),
            "gap_ms": answer.get("gap_ms"), "volume": answer.get("volume")}


# --- Asset packs (docs/features/39 asset-packs.md "The panel", OPT-006): PUT/GET asset on the developer link ---

ASSET_NO_ROUTE = "This firmware has no asset-pack route (it comes with the asset packs, BL-124)"
ASSET_SECTOR, ASSET_MIN = 4096, 8192
ASSET_KINDS = {3: ("letters", 256 * 1024), 4: ("picture", 512 * 1024), 5: ("certificates", 128 * 1024)}
ASSET_RESULTS = {
    "ok": "Installed.",
    "off": "This build has no pack store (it comes with language packs).",
    "size": "The panel refused the file's size for its kind.",
    "signature": "The panel refused the file: it is not signed for this panel.",
    "format": "The panel refused the file: signed, but not a valid asset pack of its kind.",
    "kind": "The panel refused the file: a kind or name this firmware does not take.",
    "draft": "The panel refused the file: a draft pack, and the panel runs a release firmware.",
    "locked": "Not now: the panel is armed or an alarm sounds. Disarm first.",
    "busy": "Not now: an update is running. Try again in a minute.",
    "no_space": "The panel has no room to hold the new pack beside the working one.",
    "flash": "The panel could not write the pack to its flash; the working pack is unchanged.",
}


def asset_check(path):
    """The file's bytes, after the checks the panel makes first (size, envelope, kind); a bad file is
    refused here, before the panel is contacted. The signature and the format are the panel's."""
    data = Path(path).read_bytes()
    n = len(data)
    if n % ASSET_SECTOR or n < ASSET_MIN:
        raise RuntimeError(f"{path}: {n} bytes is not an asset pack (a multiple of {ASSET_SECTOR}, at least {ASSET_MIN})")
    if data[:4] != b"RAST":
        raise RuntimeError(f"{path}: not a .roboasset file (no RAST header)")
    length = struct.unpack_from("<I", data, 8)[0]
    if length < 128 or -(-length // ASSET_SECTOR) * ASSET_SECTOR + ASSET_SECTOR != n:
        raise RuntimeError(f"{path}: the payload length {length} does not fit a {n}-byte signed file")
    kind = data[12]
    if kind not in ASSET_KINDS:
        raise RuntimeError(f"{path}: kind {kind} is not one the panel takes (3 letters, 4 picture, 5 certificates)")
    word, bound = ASSET_KINDS[kind]
    if n > bound:
        raise RuntimeError(f"{path}: {n} bytes is over the {word} pack's bound of {bound}")
    return data


def asset_install(client, path):
    data = asset_check(path)
    try:
        body = client.request("PUT", "asset", data, {"Content-Type": "application/octet-stream"})
    except DeviceError as exc:
        if exc.status == 404:
            raise RuntimeError(ASSET_NO_ROUTE) from exc
        found = re.search(r'"result"\s*:\s*"(\w+)"', str(exc))
        word = found.group(1) if found else None
        if word in ASSET_RESULTS:
            raise RuntimeError(f"{word}: {ASSET_RESULTS[word]}") from exc
        raise
    answer = json.loads(body)
    word = answer.get("result")
    message = ASSET_RESULTS.get(word, "The panel answered " + str(word))
    if word == "ok" and answer.get("state") == "needs_restart":
        message += " Used from the panel's next start."
    elif word == "ok" and answer.get("state") not in (None, "", "ready"):
        message += f" The panel's {answer.get('kind')} pack is {answer.get('state')}: see `asset status`."
    return {"result": word, "kind": answer.get("kind"), "state": answer.get("state"), "message": message,
            "id": client.device["id"]}


def asset_status(client):
    try:
        return json.loads(client.request("GET", "asset"))
    except DeviceError as exc:
        if exc.status == 404:
            raise RuntimeError(ASSET_NO_ROUTE) from exc
        raise


def asset_status_text(st):
    kinds = st.get("kinds") or []
    if not kinds:
        return "Asset packs: no pack store (every kind uses its built-in fallback)"
    lines = ["Asset packs:"]
    for k in kinds:
        line = f"  {k.get('kind', '?'):<12} {k.get('state', '?')}"
        if k.get("bytes"):
            line += f", version {k.get('version') or '?'}, match {k.get('match')}, {k.get('bytes')} bytes"
            line += ", DRAFT" if k.get("draft") else ""
        if k.get("why"):
            line += f" ({k['why']})"
        lines.append(line)
    lines.append(f"Free in the store: {st.get('free_bytes', 0)} bytes")
    return "\n".join(lines)


def asset_command(args, client):
    if args.asset_command == "install":
        result = asset_install(client, args.file)
        print(result["message"]); return
    st = asset_status(client)
    print(json.dumps(st, indent=2) if args.json else asset_status_text(st))


def voice_command(args, client):
    if args.voice_command == "volume":
        st = voice_volume(client, args.volume) if args.volume is not None else voice_status(client)
        print(f"Voice volume {st.get('volume')} %" if "volume" in st else voice_status_text(st)); return
    if args.voice_command == "say":
        result = voice_say(client, args.ids, args.gap)
        print(f"{result['message']} {result['gap_ms']} ms between the words, Voice {result['volume']} %"); return
    if args.voice_command == "install":
        result = voice_install(client, args.file)
        print(result["message"]); return
    if args.voice_command == "play":
        result = voice_play(client, args.file)
        print(f"{result['seconds']:.1f} s: {result['message']}"); return
    st = voice_status(client)
    print(json.dumps(st, indent=2) if args.json else voice_status_text(st))


def zigbee_status(client):
    return json.loads(zwave_request(client, "GET", "zigbee", no_route=ZIGBEE_NO_ROUTE))


def zigbee_post(client, op, **fields):
    body = json.dumps({"op": op, **fields})
    return json.loads(zwave_request(client, "POST", "zigbee", body, {"Content-Type": "application/json"},
                                    no_route=ZIGBEE_NO_ROUTE))


def zigbee_act(client, op, **fields):
    """Run one op, print its result word (never the fields: an install code may be among them)
    and exit 1 when it isn't ok."""
    result = zigbee_post(client, op, **fields)
    print(result["result"])
    if result["result"] != "ok":
        sys.exit(1)


def zigbee_ieee(text):
    """An IEEE address as the panel takes it: 16 hex digits, ':' or '-' between them, an optional
    0x; returned as 16 lower-case digits. ValueError otherwise (nothing is sent)."""
    t = text.strip()
    if t[:2].lower() == "0x":
        t = t[2:]
    digits = t.replace(":", "").replace("-", "")
    if not re.fullmatch(r"[0-9A-Fa-f]{16}", digits):
        raise ValueError("An IEEE address is 16 hex digits, e.g. 02ab:cd00:0000:0001")
    return digits.lower()


def zigbee_status_text(status):
    r = status["radio"]
    channel = "none" if r.get("channel") is None else r["channel"]
    noise = "unknown" if r.get("noise") is None else f"{r['noise']} dBm"
    lines = [f"Radio: {r['state']}" + (f" (paused: {r['paused']})" if r.get("paused", "none") != "none" else "") +
             f" · channel {channel} · version {r.get('version') or 'unknown'} · noise {noise}" +
             (" · JAMMED" if r.get("jammed") else "") +
             (" · STORAGE FULL" if r.get("storage_full") else ""),
             f"{r.get('devices', 0)} device(s), {r.get('not_heard', 0)} not heard · "
             f"{r.get('codes_waiting', 0)} install code(s) waiting · restarts {r.get('restarts', 0)} · "
             f"task stack {r.get('stack_free', 0)} of {r.get('stack_size', 0)} bytes never used · "
             f"NVS free entries {r.get('nvs_free', 0)}" +
             # the radio's queue since start (zigbee_service README "Rules"); absent before it
             (f" · radio events dropped {r['events_dropped']}" if r.get("events_dropped") else "") +
             (f" · ALARM EVENTS LOST {r['alarm_lost']}" if r.get("alarm_lost") else "")]
    devices = status.get("devices", [])
    lines.append(f"Devices ({len(devices)}):" if devices else "Devices: none")
    for d in devices:
        battery = "unknown" if d["battery"] == -1 else ("low" if d.get("low") else f"{d['battery']}%")
        heard = "never" if d["heard_s"] == -1 else f"{d['heard_s']}s ago"
        zones = ",".join(str(z) for z in d.get("zones", [])) or "none"
        flag = " · UNREACHABLE" if d.get("unreachable") else ""
        name = d.get("name") or d.get("model") or "unnamed"
        lines.append(f"  {d['id']} {d.get('ieee', '')} {name} · {d['kind']} · {d['state']} · "
                     f"signal {d['signal']} · battery {battery} · heard {heard} · joined {d['join']} · "
                     f"zones [{zones}]{flag}")
    p = status.get("progress", {})
    if p.get("op", "none") == "none":
        lines.append("Operation: none")
    else:
        bits = [f"{p['op']} · {p['step']}"]
        if p.get("seconds"):
            bits.append(f"{p['seconds']}s left")
        if p.get("id"):
            bits.append(f"device {p['id']}")
        if p.get("result", "none") != "none":
            bits.append(f"result {p['result']}")
        if p.get("joined"):
            bits.append("joined " + ",".join(str(j["id"]) for j in p["joined"]))
        lines.append("Operation: " + " · ".join(bits))
    codes = status.get("codes", [])
    if codes:
        lines.append("Install codes waiting for: " + ", ".join(c["ieee"] for c in codes))
    return "\n".join(lines)


def zigbee_command(args, client):
    cmd = args.zigbee_command
    if cmd == "status":
        status = zigbee_status(client)
        print(json.dumps(status, indent=2) if args.json else zigbee_status_text(status))
        return
    if cmd == "reset-network":
        # ZB-017: the network reset needs the installer code typed at the panel (its touchscreen
        # or console); the code never travels the network, so the developer link has no reset.
        raise RuntimeError("The Zigbee network reset is done at the panel (Settings, or the console's "
                           "`zigbee reset confirm <installer code>`): never over the network")
    if cmd in ("remove", "replace", "ping", "identify", "setup"):
        return zigbee_act(client, cmd, id=args.id)
    if cmd == "forget":
        if not args.confirm:
            raise RuntimeError("forget needs --confirm (refused locally without it)")
        return zigbee_act(client, "forget", id=args.id, confirm=True)
    if cmd == "rename":
        return zigbee_act(client, "rename", id=args.id, name=args.name)
    if cmd in ("add-code", "code-add"):
        if not args.code.strip():
            raise ValueError("An install code is needed (its QR text, or its hex digits)")
        fields = {"code": args.code.strip()}
        if args.ieee:
            fields["ieee"] = zigbee_ieee(args.ieee)
        return zigbee_act(client, cmd.replace("-", "_"), **fields)
    if cmd == "code-forget":
        return zigbee_act(client, "code_forget", ieee=zigbee_ieee(args.ieee))
    if cmd == "channel":
        if args.channel not in ZIGBEE_CHANNELS:
            raise ValueError("A Zigbee channel is 11..26")
        if not args.confirm:
            raise RuntimeError("channel needs --confirm: every device must follow the move")
        return zigbee_act(client, "channel", channel=args.channel, confirm=True)
    return zigbee_act(client, cmd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device")
    parser.add_argument("--host", help="Explicit routed IP/DNS; needs an enrolled identity")
    parser.add_argument("--usb", nargs="?", const="auto", help="Use enrolled USB port (optionally specify COM/tty); info/flash only")
    sub = parser.add_subparsers(dest="command", required=True)
    en = sub.add_parser("enroll"); en.add_argument("--port", required=True); en.add_argument("--name", required=True)
    en = sub.add_parser('prepare-enrollment'); en.add_argument('--name', required=True)
    en = sub.add_parser('import-enrollment'); en.add_argument('--name', required=True); en.add_argument('--file', required=True)
    discovery = sub.add_parser("discover")
    discovery.add_argument("--subnet", help="Explicit private /24 or smaller TCP scan when multicast is unavailable")
    discovery.add_argument("--interface", help="Local interface IPv4 address for mDNS on multi-interface PCs")
    sub.add_parser("info")
    sub.add_parser("devices")
    sub.add_parser("diagnostics")
    evidence = sub.add_parser("bundle"); evidence.add_argument("--output", required=True)
    sub.add_parser("update-status")
    speakerp = sub.add_parser("speaker", help="bench silence (development builds): off keeps the speaker off, alarms included")
    speakerp.add_argument("state", nargs="?", choices=("on", "off"))
    voicep = sub.add_parser("voice", help="the voice pack: install a signed .robovoice file, show the voice's state, or play a WAV (development builds)")
    voicesub = voicep.add_subparsers(dest="voice_command", required=True)
    vi = voicesub.add_parser("install"); vi.add_argument("file")
    vp_ = voicesub.add_parser("play", help="development builds: play a 16-bit mono 16 kHz WAV on the panel's speaker")
    vp_.add_argument("file")
    vsay = voicesub.add_parser("say", help="development builds: the panel says clip IDs from its own pack, with its own word gap and Voice volume")
    vsay.add_argument("ids", nargs="+", help="clip IDs (recording-script.csv numeric_id: 0x0002 or 2), up to 8")
    vsay.add_argument("--gap", type=int, default=0,
                      help=f"ms between the words, 1-{SAY_GAP_MAX_MS}, kept until the next say; 0 (the default): the pack's")
    vvol = voicesub.add_parser("volume", help="development builds: show or set the panel's Voice volume (as its Settings slider)")
    vvol.add_argument("volume", nargs="?", type=int, help="0-100; without it, the volume now")
    vs = voicesub.add_parser("status"); vs.add_argument("--json", action="store_true")
    for vleaf in voicesub.choices.values():
        vleaf.add_argument("--device", default=argparse.SUPPRESS)
        vleaf.add_argument("--host", default=argparse.SUPPRESS)
    assetp = sub.add_parser("asset", help="the asset packs (letters, picture, certificates): install a signed .roboasset file or show each kind's state")
    assetsub = assetp.add_subparsers(dest="asset_command", required=True)
    ai = assetsub.add_parser("install"); ai.add_argument("file")
    ast = assetsub.add_parser("status"); ast.add_argument("--json", action="store_true")
    for aleaf in assetsub.choices.values():
        aleaf.add_argument("--device", default=argparse.SUPPRESS)
        aleaf.add_argument("--host", default=argparse.SUPPRESS)
    logs = sub.add_parser("logs"); logs.add_argument("--follow", action="store_true")
    screenshot = sub.add_parser("screenshot"); screenshot.add_argument("--output", required=True)
    screen = sub.add_parser("screen"); screen.add_argument("--no-browser", action="store_true"); screen.add_argument("--url-file")
    flashp = sub.add_parser("flash")
    choice = flashp.add_mutually_exclusive_group(required=True)
    choice.add_argument("--build", action="store_true"); choice.add_argument("--image")
    flashp.add_argument("--allow-older", action="store_true"); flashp.add_argument("--dark", action="store_true")
    zwavep = sub.add_parser("zwave")
    zsub = zwavep.add_subparsers(dest="zwave_command", required=True)
    zst = zsub.add_parser("status"); zst.add_argument("--json", action="store_true")
    ztr = zsub.add_parser("trace")
    ztr.add_argument("--follow", action="store_true"); ztr.add_argument("--all", action="store_true"); ztr.add_argument("--output")
    for verb in ("add", "remove", "stop", "keep-listening", "insecure", "restart", "dismiss", "pulse-reset"):
        zsub.add_parser(verb)
    zal = zsub.add_parser("allow")
    zal.add_argument("--keys", help="comma-separated access,auth,unauth,s0 (default: what it asked for)")
    zpn = zsub.add_parser("pin"); zpn.add_argument("pin")
    zrf = zsub.add_parser("remove-failed"); zrf.add_argument("node", type=int)
    zsu = zsub.add_parser("setup"); zsu.add_argument("node", type=int)
    zpi = zsub.add_parser("ping"); zpi.add_argument("node", type=int)
    zci = zsub.add_parser("check-in"); zci.add_argument("node"); zci.add_argument("duration", choices=sorted(ZWAVE_CHECK_INS))
    zrg = zsub.add_parser("region"); zrg.add_argument("region", choices=ZWAVE_REGIONS)
    zrn = zsub.add_parser("reset-network"); zrn.add_argument("--confirm", action="store_true")
    zck = zsub.add_parser("check")
    zck.add_argument("--output-dir"); zck.add_argument("--sensor", action="store_true"); zck.add_argument("--yes", action="store_true")
    for zleaf in zsub.choices.values():
        zleaf.add_argument("--device", default=argparse.SUPPRESS)
        zleaf.add_argument("--host", default=argparse.SUPPRESS)
        zleaf.add_argument("--usb", nargs="?", const="auto", default=argparse.SUPPRESS)
    zigbeep = sub.add_parser("zigbee")
    gsub = zigbeep.add_subparsers(dest="zigbee_command", required=True)
    gst = gsub.add_parser("status"); gst.add_argument("--json", action="store_true")
    for verb in ("add", "keep", "stop", "survey", "restart", "retry", "dismiss", "reset-network"):
        gsub.add_parser(verb)
    for verb in ("remove", "replace", "ping", "identify", "setup"):
        gsub.add_parser(verb).add_argument("id", type=int)
    gfg = gsub.add_parser("forget"); gfg.add_argument("id", type=int); gfg.add_argument("--confirm", action="store_true")
    grn = gsub.add_parser("rename"); grn.add_argument("id", type=int); grn.add_argument("name")
    for verb in ("add-code", "code-add"):
        gac = gsub.add_parser(verb); gac.add_argument("code", help="the QR code's text, or the code's hex digits")
        gac.add_argument("--ieee", help="the device's address (left out: read from the QR text)")
    gcf = gsub.add_parser("code-forget"); gcf.add_argument("ieee")
    gch = gsub.add_parser("channel"); gch.add_argument("channel", type=int); gch.add_argument("--confirm", action="store_true")
    for gleaf in gsub.choices.values():
        gleaf.add_argument("--device", default=argparse.SUPPRESS)
        gleaf.add_argument("--host", default=argparse.SUPPRESS)
        gleaf.add_argument("--usb", nargs="?", const="auto", default=argparse.SUPPRESS)
    # Accept target selectors before or after the verb without overwriting earlier values.
    for command_parser in sub.choices.values():
        command_parser.add_argument("--device", default=argparse.SUPPRESS)
        command_parser.add_argument("--host", default=argparse.SUPPRESS)
        command_parser.add_argument("--usb", nargs="?", const="auto", default=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.command == "enroll": return enroll(args)
    if args.command == 'prepare-enrollment': return prepare_network_enrollment(args)
    if args.command == 'import-enrollment': return import_network_enrollment(args)
    if args.command == "discover": print(json.dumps(discover(args.subnet,args.interface), indent=2)); return
    if args.command == "devices":
        rows = []
        for path in REGISTRY.glob("*.json"):
            value = json.loads(path.read_text())
            if "fingerprint" in value: rows.append({"name":path.stem, **{k:value.get(k) for k in ("id","host","usb_port","enrollment")}})
        print(json.dumps(rows, indent=2)); return
    client = target(args)
    try:
        if args.usb:
            if args.command not in ("info", "flash"): raise RuntimeError("--usb supports info/flash; other operations use the network")
            port = client.device.get("usb_port") if args.usb == "auto" else args.usb
            if not port: raise RuntimeError("Specify the enrolled USB port")
            result = usb_info(client, port) if args.command == "info" else usb_flash(args, client, port)
        elif args.command == "info": result = client.info()
        elif args.command == "diagnostics":
            print(client.request("GET", "diagnostics").decode(), end=""); return
        elif args.command == "bundle": result = bundle(client, args.output)
        elif args.command == "flash": result = flash(args, client)
        elif args.command == "zwave": return zwave_command(args, client)
        elif args.command == "zigbee": return zigbee_command(args, client)
        elif args.command == "update-status":
            operation = json.loads((REGISTRY / (client.name + ".operation.json")).read_text())
            if operation.get("transport") == "usb":
                raise RuntimeError("Last update used USB (saved result: " + operation["result"] + "); verify current firmware with info --usb")
            result = wait_update(client, operation)
        elif args.command == "voice": return voice_command(args, client)
        elif args.command == "speaker": result = speaker(client, args.state)
        elif args.command == "asset": return asset_command(args, client)
        elif args.command == "screen": return viewer(args, client)
        elif args.command == "screenshot":
            info = client.info()
            Path(args.output).write_bytes(png565(client.request("GET", "frame"), info["capture_width"], info["capture_height"]))
            result = {"saved": str(Path(args.output).resolve()), "id": info["id"]}
        elif args.command == "logs":
            seen = set()
            while True:
                lines = client.request("GET", "logs").decode().splitlines()
                for line in lines:
                    if line not in seen: print(line, flush=True)
                seen = set(lines)
                if not args.follow: return
                time.sleep(2)
        print(json.dumps(result, indent=2))
    finally: client.close()


if __name__ == "__main__":
    try: main()
    except KeyboardInterrupt: pass
    except Exception as exc:
        print(f"panelctl: {exc}", file=sys.stderr)
        sys.exit(1)
