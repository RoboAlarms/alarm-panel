"""The speaker stage: the level a master is encoded at for the panel.

The masters stay as recorded (trimmed, levelled to -20 dB of voiced RMS under a -3 dBFS peak:
audio_edit.level). Spoken at that level the announcements were about 10 dB under the chime at the
same volume (heard on a panel, 2026-10-09). From pack revision 7 (decided 2026-10-10)
this stage trims each master's quiet ends to SETTINGS['lead_ms'] before its first sound and
SETTINGS['tail_ms'] after its last (trimmed(): the masters keep 30 and 80 ms; the panel's word gap
makes the pause between words), then raises it to SETTINGS['target_rms_db'] (-15 dB) of voiced
RMS with one gain, capped so that its peak stays under SETTINGS['peak_db'] (room for the codec's
overshoot), and limits nothing: a word with sharp peaks stops short of -15 dB. The panel makes
speech louder itself, with its Voice volume and a limiter there (AUD-033).
Revision 6 raised and limited each clip, up to six times, to about -8 dB ('limit': True, still
made by the bench's --limit: limit()): what the limiter didn't reach - a word's lead-in and tail,
a breath, the start of an "f" or an "s" - rose about 15 dB more than the voice, and it was
heard as harsh at every volume. Pack revision 2's compressor and limiter raised the breath,
the room and the "s" sounds by 9-15 dB and a hiss was heard: a pack made with a louder
stage is listened to for it too. The balance between voice and chime is the panel's Voice and
Chime volumes (AUD-032).

In plain Python floats: the same input gives the same output on any machine (AUD-023);
`voice.py encode --reproduce` checks the packets.
"""
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_edit  # noqa: E402

SETTINGS = {
    'target_rms_db': -15.0,      # voiced RMS (the masters are at -20); the panel makes it louder
    'peak_db': -1.0,             # never above, here and at 16 kHz (encode.ceiling): Opus decodes
                                 # limited speech at most about 0.6 dB over it (measured)
    'limit': False,              # the gain capped at the peak (True: the peaks limited to reach
                                 # the level, as revision 6)
    'lead_ms': 10,               # the quiet kept before a word's first sound and after its last
    'tail_ms': 20,               # (the masters keep 30 and 80): the panel's word gap makes the pause
}


def voiced_db(x, rate):
    """The voiced RMS in dB, as audio_edit.level measures it."""
    rms = audio_edit._frame_rms(x, max(1, rate // 100))
    top = max(rms)
    voiced = [r for r in rms if r >= top * 10.0 ** (-30.0 / 20.0)]
    return 20 * math.log10(math.sqrt(sum(r * r for r in voiced) / len(voiced)))


def limit(x, rate, peak_db, attack_ms=2.0, release_ms=60.0):
    """`x` with its peaks held under `peak_db`: a gain that falls smoothly into each peak over
    about attack_ms and recovers over about release_ms, never letting a sample over the ceiling."""
    c = 10.0 ** (peak_db / 20.0)
    g = [min(1.0, c / abs(s)) if s else 1.0 for s in x]
    ka = 1.0 - math.exp(-1.0 / (rate * attack_ms / 1000.0))
    kr = 1.0 - math.exp(-1.0 / (rate * release_ms / 1000.0))
    for i in range(len(g) - 2, -1, -1):  # backwards: down into the peak
        g[i] = min(g[i], g[i + 1] + (1.0 - g[i + 1]) * ka)
    for i in range(1, len(g)):  # forwards: back up after it
        g[i] = min(g[i], g[i - 1] + (1.0 - g[i - 1]) * kr)
    return [s * k for s, k in zip(x, g)]


def trimmed(x, rate, s=SETTINGS):
    """The master `x` with its quiet ends cut to s['lead_ms'] before the first sound and
    s['tail_ms'] after the last (audio_edit.trim, 5 ms fades), when the settings give them: the
    panel's word gap makes the pause between words (decided 2026-10-10)."""
    if s.get('lead_ms') is None or not any(x):
        return list(x)
    return audio_edit.trim(x, rate, lead_ms=s['lead_ms'], tail_ms=s['tail_ms'])


def for_speaker(x, rate, s=SETTINGS):
    """The master `x` (floats, -1..1, at `rate`) as the panel's pack carries it: trimmed()
    and at the level."""
    x = trimmed(x, rate, s)
    if not s.get('limit'):
        return audio_edit.level(x, rate, target_db=s['target_rms_db'], peak_db=s['peak_db'])
    if not any(x):
        return list(x)
    y = x
    for _ in range(6):  # limiting lowers the level a little: raise it and limit again
        y = limit(audio_edit.level(y, rate, target_db=s['target_rms_db'], peak_db=60.0), rate,
                  s['peak_db'])
        if voiced_db(y, rate) > s['target_rms_db'] - 0.3:
            break
    return y
