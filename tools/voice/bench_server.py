"""The voice bench: hear a voice on a development panel by clicking (docs/features/08 AUD-030).

  python tools/voice/bench_server.py --device <panel> [--port 8546]

Serves http://127.0.0.1:<port>/ on this computer only (the page is bench_page.html). Everything
you hear is made on the panel, as it makes a zone's name: this page turns the words you pick into
the voice pack's clip numbers (recording-script.csv) and sends only those and the pause between
words; the panel takes each word from its own installed voice pack, joins them with that pause and
plays them at its own Voice volume (its loudness made there, AUD-033). The page shows the panel's
state (firmware, pack, Voice volume, pause; the speaker when the firmware has the bench silence
switch, AUD-025), sets its Voice volume - saved on the panel
as its Settings slider saves it - and after each phrase says what was sent and what the panel did.
Talks to the panel through panelctl (dev builds from 0.8.14), over one kept connection. A word the
script lacks is refused here, before the panel. To join a recording's takes on the PC instead,
`voice.py try` stays. Standard library only.
"""
import argparse
import http.client
import json
import math
import ssl
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import bench  # noqa: E402
import panelctl  # noqa: E402
import tracked  # noqa: E402

GAPS = (40, 60, 100, 140, 180, 220, 250)  # panelctl.SAY_GAP_MAX_MS: 250
BOOST_DB = 9.0     # components/core/sounder speech_boost.h SB_BOOST_DB: the gain at Voice 100 %
UNITY = 70         # SB_UNITY_VOLUME: the pack as it is; 3 dB for every 10 %
CEILING_DB = -1.0  # SB_CEILING_DB: over this gain its limiter holds the peaks
# What a gain over the ceiling is heard as, as the limiter takes part of it back: the median change
# of voiced RMS over pack revision 7's 237 words through speech_boost.c (ported to Python),
# measured 2026-10-10. (gain dB, heard dB), straight lines between.
HEARD = ((-1.0, -1.0), (0.0, -0.1), (3.0, 2.3), (6.0, 3.9), (9.0, 4.7))
RATE = 16000       # the pack's samples
# Zone names to try with one click; each is checked against the script when the server starts.
QUICK = ('Front door', 'Back door', 'Garage door', 'Kitchen window', 'Back patio motion',
         'Living room motion', 'Upstairs bedroom window', 'Basement water leak',
         'Garage side door', 'Zone twelve')
# A stale kept connection fails with one of these before the panel has read the request.
STALE = (http.client.RemoteDisconnected, ConnectionResetError, ConnectionAbortedError,
         BrokenPipeError, ssl.SSLEOFError)


def gain(volume):
    """What the panel does to the pack at this Voice volume: (dB, its limiter can act), as
    speech_boost.c's sb_gain() and sb_begin() work it out; None for silent."""
    if volume <= 0:
        return None
    db = BOOST_DB * (min(volume, 100) - UNITY) / (100 - UNITY)
    return db, db > CEILING_DB


def heard(db):
    """About how much louder (or quieter) than the pack a gain of `db` is heard (HEARD)."""
    if db <= HEARD[0][0]:
        return db
    for (g0, h0), (g1, h1) in zip(HEARD, HEARD[1:]):
        if db <= g1:
            return h0 + (h1 - h0) * (db - g0) / (g1 - g0)
    return HEARD[-1][1]


class Voice:
    """The tracked voice: its words (text -> registry row) and each clip's length."""

    def __init__(self):
        self.lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
        meta = json.loads((tracked.voice_dir(self.lib) / 'voice.json').read_text(encoding='utf-8'))
        self.samples = {c['clip_id']: c['valid_samples'] for c in meta['clips']}
        self.words = {k: r for k, r in bench.vocabulary(self.lib).items()
                      if r.get('clip_id') in self.samples}

    def parse(self, text):
        """The rows a phrase is said with (the longest match first), or a ValueError naming the
        word the pack lacks."""
        if not text.strip() or len(text) > 80:
            raise ValueError('Type one to eight words')
        try:
            rows = bench.parse(text, self.words)
        except SystemExit as e:
            raise ValueError(str(e).replace('of the recording script', 'in the voice pack')) from None
        if not 0 < len(rows) <= 8:
            raise ValueError('One to eight words at a time')
        return rows

    def seconds(self, rows, gap):
        return (sum(self.samples[r['clip_id']] for r in rows) / RATE
                + gap / 1000 * (len(rows) - 1))


class Panel:
    """The panel, through panelctl's client: one kept connection, one request at a time."""

    def __init__(self, device):
        self.device = device
        self.client = panelctl.Client(device)
        self.lock = threading.Lock()

    def _call(self, fn):
        with self.lock:
            try:
                return fn(self.client)
            except STALE:  # the panel closed the idle connection: once more on a new one
                self.client.close()
                return fn(self.client)

    def status(self):
        def get(c):
            info = c.info()
            return {'version': info.get('version'), 'profile': info.get('profile'),
                    'speaker': info.get('speaker'), 'idle': info.get('idle'),
                    'voice': panelctl.voice_status(c)}
        return self._call(get)

    def volume(self, value):
        return self._call(lambda c: panelctl.voice_volume(c, value))

    def say(self, ids, gap):
        return self._call(lambda c: panelctl.voice_say(c, ids, gap))


def failure(e):
    """A panel or panelctl error as one plain sentence."""
    if isinstance(e, (ValueError, RuntimeError)):
        return str(e)
    if isinstance(e, panelctl.DeviceError):
        return f'The panel refused it ({e})'
    if isinstance(e, (OSError, ssl.SSLError)):
        return f'The panel is not answering ({e.__class__.__name__}): is it on the network?'
    return f'{e.__class__.__name__}: {e}'


def page(device, voice):
    """bench_page.html with what the page needs from here."""
    quick = []
    for q in QUICK:
        voice.parse(q)  # a quick phrase the pack can't say is a mistake here
        quick.append(q)
    gap = voice.lib['gap_ms'] if voice.lib['gap_ms'] in GAPS else GAPS[0]  # the pack's at first
    boot = {'device': device, 'gaps': GAPS, 'gap': gap, 'packGap': voice.lib['gap_ms'],
            'boostDb': BOOST_DB, 'unity': UNITY, 'ceilingDb': CEILING_DB, 'heard': HEARD,
            'quick': quick,
            'words': sorted(voice.words)}
    html = (HERE / 'bench_page.html').read_text(encoding='utf-8')
    return html.replace('/*BOOT*/null', json.dumps(boot)).encode('utf-8')


def make_handler(device, panel=None, voice=None):
    voice = voice or Voice()
    panel = panel or Panel(device)
    page(device, voice)  # a quick phrase the pack can't say stops the server here

    class Handler(BaseHTTPRequestHandler):
        def _send(self, status, data, kind):
            self.send_response(status)
            self.send_header('Content-Type', kind)
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(data)

        def _json(self, result):
            self._send(200, json.dumps(result).encode(), 'application/json')

        def _request(self):
            n = int(self.headers.get('Content-Length', 0) or 0)
            return json.loads(self.rfile.read(n) or b'{}') if 0 <= n <= 4096 else {}

        def do_GET(self):
            if self.path == '/':
                return self._send(200, page(device, voice), 'text/html; charset=utf-8')
            if self.path == '/api/status':
                try:
                    return self._json({'ok': True, **panel.status()})
                except Exception as e:  # noqa: BLE001 - every failure is shown on the page
                    return self._json({'ok': False, 'error': failure(e)})
            self._send(404, b'not found', 'text/plain')

        def do_POST(self):
            try:
                req = self._request()
            except (ValueError, json.JSONDecodeError):
                return self._json({'ok': False, 'error': 'The page sent something unreadable'})
            if self.path == '/api/volume':
                try:
                    st = panel.volume(int(req.get('volume', -1)))
                    return self._json({'ok': True, 'voice': st})
                except Exception as e:  # noqa: BLE001
                    return self._json({'ok': False, 'error': failure(e)})
            if self.path == '/api/say':
                text = str(req.get('text', ''))
                try:
                    gap = int(req.get('gap', -1))
                    if gap not in GAPS:
                        raise ValueError('Pick a pause between words')
                    rows = voice.parse(text)
                except (ValueError, TypeError) as e:
                    return self._json({'ok': False, 'text': text, 'error': str(e)})
                sent = {'text': text, 'words': [r['text'] for r in rows],
                        'ids': [r['numeric_id'] for r in rows], 'seconds': voice.seconds(rows, gap)}
                try:
                    r = panel.say(sent['ids'], gap)
                except Exception as e:  # noqa: BLE001
                    return self._json({'ok': False, **sent, 'error': failure(e)})
                g = gain(r.get('volume') or 0)
                return self._json({'ok': True, **sent, 'gap_ms': r.get('gap_ms'),
                                   'volume': r.get('volume'),
                                   'gain_db': None if g is None else round(g[0], 1),
                                   'heard_db': None if g is None else round(heard(g[0]), 1),
                                   'limiter': bool(g and g[1]),
                                   'seconds': voice.seconds(rows, r.get('gap_ms') or gap)})
            self._send(404, b'not found', 'text/plain')

        def log_message(self, *a):
            pass

    return Handler


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('--device', required=True, help='the enrolled development panel to play on')
    a.add_argument('--port', type=int, default=8546)
    args = a.parse_args()
    server = ThreadingHTTPServer(('127.0.0.1', args.port), make_handler(args.device))
    print(f'voice bench for {args.device}: http://127.0.0.1:{args.port}/', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
