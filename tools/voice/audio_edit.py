"""Trim, level and join speech clips for the voice auditions (docs/features/08 voice-library.md).

Standard library only, so the tool tests run without the engines' environments. Samples are
floats in [-1, 1], mono.
"""
import array
import math
import wave


def _frame_rms(x, n):
    out = []
    for i in range(0, len(x), n):
        f = x[i:i + n]
        out.append(math.sqrt(sum(s * s for s in f) / len(f)))
    return out


def trim(x, rate, floor_db=-45.0, lead_ms=30, tail_ms=80, fade_ms=5):
    """Cut the silence around the speech.

    Keeps everything from `lead_ms` before the first 10 ms frame within `floor_db` of the
    loudest frame to `tail_ms` after the last one, so quiet consonants at either end stay,
    then fades both ends over `fade_ms` so a cut never clicks. Returns [] for silence.
    """
    n = max(1, rate // 100)
    rms = _frame_rms(x, n)
    if not rms or max(rms) == 0.0:
        return []
    floor = max(rms) * 10.0 ** (floor_db / 20.0)
    loud = [i for i, r in enumerate(rms) if r >= floor]
    start = max(0, loud[0] * n - rate * lead_ms // 1000)
    end = min(len(x), (loud[-1] + 1) * n + rate * tail_ms // 1000)
    y = list(x[start:end])
    f = min(len(y) // 2, rate * fade_ms // 1000)
    for i in range(f):
        g = i / f
        y[i] *= g
        y[-1 - i] *= g
    return y


def level(x, rate, target_db=-20.0, peak_db=-3.0):
    """Scale speech to `target_db` RMS over its voiced frames, never peaking above `peak_db`.

    Voiced frames are those within 30 dB of the loudest, so pauses don't pull the level up.
    """
    if not x:
        return []
    rms = _frame_rms(x, max(1, rate // 100))
    top = max(rms)
    if top == 0.0:
        return list(x)
    voiced = [r for r in rms if r >= top * 10.0 ** (-30.0 / 20.0)]
    now = math.sqrt(sum(r * r for r in voiced) / len(voiced))
    g = 10.0 ** (target_db / 20.0) / now
    peak = max(abs(s) for s in x)
    g = min(g, 10.0 ** (peak_db / 20.0) / peak)
    return [s * g for s in x]


def silence(rate, ms):
    return [0.0] * (rate * ms // 1000)


def join(clips, rate, gap_ms):
    """Play clips one after another with `gap_ms` of silence between them (none at the ends)."""
    out = []
    for i, c in enumerate(clips):
        if i:
            out += silence(rate, gap_ms)
        out += c
    return out


def write_wav(path, x, rate):
    """Write 16-bit mono PCM."""
    pcm = array.array('h', (max(-32768, min(32767, round(s * 32767))) for s in x))
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def read_wav(path):
    """Read 16-bit mono PCM back as floats; returns (samples, rate)."""
    with wave.open(str(path), 'rb') as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise ValueError(f'{path}: not 16-bit mono')
        rate = w.getframerate()
        pcm = array.array('h')
        pcm.frombytes(w.readframes(w.getnframes()))
    return [s / 32768.0 for s in pcm], rate
