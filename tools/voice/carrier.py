"""Words cut from a carrier sentence, so a word sounds like part of a name, not a sentence of its own.

Spoken alone ("Front."), a word ends the way a sentence does: its pitch falls and its last sound
stretches, and a name joined from such words sounds like a list. So a script word is spoken inside
a short sentence, library.json's "carriers" (chosen in BL-202): a template for its
category ("{} contact tripped." for a position word) or a sentence of its own for a status part
("Armed away." for "Away"). A forced aligner (torchaudio's MMS_FA, a wav2vec2 model trained for
alignment; its checkpoint downloads to the torch hub cache, never into the repository) finds where
each word of the sentence starts and ends, and the word is cut out at the quiet point between it
and its neighbours (or, where a vowel or an "h" runs into it, at the aligner's boundary:
cut_points), then trimmed, faded and levelled like any clip (audio_edit). `record` keeps each
whole carrier take beside the clips (carriers/), so a cut can be made again without the model.

The cut is recorded with the clip (`cut`: the first and the end sample of the model's output), so
`voice.py record --reproduce` repeats it from the text and the seed alone, without the aligner.
plan() and cut_points() use the standard library only; Aligner needs the engine's environment.
"""
import hashlib
import math
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_edit as ae  # noqa: E402


def words(text):
    """The words of a text as the aligner spells them: lower case, letters and apostrophes."""
    out = []
    for w in text.lower().split():
        w = re.sub(r"[^a-z']", '', w)
        if w:
            out.append(w)
    return out


def plan(row, lib):
    """(carrier sentence, index of the clip's first word in it, its word count) for a script row,
    or None for a row spoken on its own (a sentence, an extra clip). A carrier for the clip's ID
    comes before its category's; a category's applies to single words (kind "token") only, and a
    word's ending (kind "ending", record.endings) is spoken in library.json's "endings"."""
    carriers = lib.get('carriers', {})
    t = carriers.get(row['clip_id'])
    if t is None and row['kind'] == 'token':
        t = carriers.get(row['category'])
    if t is None and row['kind'] == 'ending':
        t = lib.get('endings')
    if t is None:
        return None
    if '{}' in t:
        word = lib.get('say', {}).get(row.get('base', row['clip_id']), row['text']).rstrip('.!?')
        text = t.format(word.lower())
        return text[0].upper() + text[1:], len(words(t.split('{}')[0])), len(words(word))
    target, w = words(row['text']), words(t)
    for i in range(len(w) - len(target) + 1):
        if w[i:i + len(target)] == target:
            return t, i, len(target)
    raise ValueError(f"{row['clip_id']}: {row['text']!r} is not in its carrier {t!r}")


def _rms(x, c, half):
    f = x[max(0, c - half):c + half]
    return math.sqrt(sum(s * s for s in f) / max(1, len(f)))


# Onsets the aligner places late and that run into the word before: a vowel or an "h" (the "h" of
# "home" begins up to 100 ms before the aligner says).
SOFT = set('aeiouh')


def _dip(x, lo, hi, rate, cfg, quiet, last):
    """The point in [lo, hi] (1 ms steps, 5 ms windows) within cfg["near_db"] of the quietest one,
    the last such point or the first; None if even the quietest is louder than `quiet` (a quiet
    "f" or "th" runs straight on from the word before it, with no silence to cut at)."""
    half, step = max(1, rate // 400), max(1, rate // 1000)
    if hi <= lo:
        return None
    frames = [(c, _rms(x, c, half)) for c in range(lo, hi + 1, step)]
    least = min(e for _, e in frames)
    if least > quiet:
        return None
    limit = max(least * 10.0 ** (cfg['near_db'] / 20.0), 1e-9)
    near = [c for c, e in frames if e <= limit]
    return near[-1] if last else near[0]


def cut_points(x, rate, spans, first, count, cfg, words):
    """Where to cut words[first:first+count] out of `x`: (start, end) in samples.

    `spans` is each word's (start, end) in seconds, as the aligner found them. The aligner's
    word edges are only rough: a held sound at a word's end ("Low", "Den") goes on 50-70 ms past
    it, and a vowel or an "h" at a word's start begins before it. So a cut is made at the quiet
    point between two words - the last point before the word, or the first after it, within
    cfg["near_db"] of the quietest within cfg["reach_ms"], and at least cfg["quiet_db"] under the
    word's loudest (a pause, or the closure of a stop such as the "c" of "contact" and the "t" of
    "tripped", the carriers' next words) - and never past the neighbour's start. The search
    starts cfg["slack_ms"] (one aligner frame) inside the word, because the aligner can put a
    word's last letter at the end of a silence (the unreleased "t" of "test" before "started").
    Without such a point (a quiet "f" straight after "Front"), the word starts where the previous
    word ends. Where a vowel or an "h" follows, the words run together with no quiet point between
    them, so the cut is the aligner's: the word ends cfg["tail_ms"] after its end, and a word that
    starts with one starts where the previous word ends ("Armed home").
    """
    reach, tail = rate * cfg['reach_ms'] // 1000, rate * cfg['tail_ms'] // 1000
    slack = rate * cfg['slack_ms'] // 1000
    ws = round(spans[first][0] * rate)
    we = max(ws + 1, round(spans[first + count - 1][1] * rate))
    last = first + count >= len(spans)
    pe = min(ws, round(spans[first - 1][1] * rate)) if first > 0 else None
    ns = None if last else min(len(x), max(we, round(spans[first + count][0] * rate)))
    half, step = max(1, rate // 400), max(1, rate // 1000)
    peak = max(_rms(x, c, half) for c in range(ws, we, step))
    quiet = peak * 10.0 ** (-cfg['quiet_db'] / 20.0)
    start = None
    if pe is None or words[first][0] not in SOFT:
        start = _dip(x, max(pe or 0, ws - reach), min(we, ws + slack), rate, cfg, quiet, last=True)
    if start is None:
        start = pe if pe is not None else max(0, ws - reach)
    end = None
    if last or words[first + count][0] not in SOFT:
        hi = min(len(x), we + reach, len(x) if last else ns + tail)
        end = _dip(x, max(start + 1, we - slack), hi, rate, cfg, quiet, last=False)
    if end is None:
        end = len(x) if last else min(ns, we + tail)
    return max(0, start), min(len(x), max(end, start + 1))


def as_saved(x):
    """A take as carriers/ keeps it (16-bit, audio_edit.write_wav then read_wav): every cut is
    made from this, so `record`, `record --recut` and `record --reproduce` give the same master."""
    return [max(-32768, min(32767, round(s * 32767))) / 32768.0 for s in x]


def finish(x, rate, cut=None):
    """A take as a master: the cut (if any), trimmed and faded, then levelled (record and
    `record --reproduce` both make masters this way)."""
    y = list(x[cut[0]:cut[1]]) if cut else list(x)
    return ae.level(ae.trim(y, rate), rate)


class Aligner:
    """torchaudio's MMS_FA forced aligner, on the GPU when there is one."""

    def __init__(self):
        import torch
        import torchaudio
        self.torch, self.ta = torch, torchaudio
        b = torchaudio.pipelines.MMS_FA
        self.dev = 'cuda' if torch.cuda.is_available() else 'cpu'
        self.model = b.get_model(with_star=False).to(self.dev).eval()
        self.tokenizer, self.aligner, self.rate = b.get_tokenizer(), b.get_aligner(), b.sample_rate
        url = b._path
        ckpt = Path(torch.hub.get_dir()) / 'checkpoints' / os.path.basename(url)
        self.info = {'model': 'torchaudio.pipelines.MMS_FA', 'url': url,
                     'sha256': hashlib.sha256(ckpt.read_bytes()).hexdigest()
                     if ckpt.exists() else None,
                     'torchaudio': torchaudio.__version__, 'device': self.dev}

    def spans(self, x, rate, text):
        """Each word of `text`: (start s, end s, mean probability of its letters)."""
        torch = self.torch
        wav = torch.tensor([float(s) for s in x], dtype=torch.float32)[None]
        wav = self.ta.functional.resample(wav, rate, self.rate)
        with torch.inference_mode():
            emission, _ = self.model(wav.to(self.dev))
            tokens = self.aligner(emission[0], self.tokenizer(words(text)))
        sec = wav.size(1) / emission.size(1) / self.rate
        out = []
        for t in tokens:
            n = sum(len(s) for s in t)
            out.append((t[0].start * sec, t[-1].end * sec, sum(s.score * len(s) for s in t) / n))
        return out


def speak(eng, aligner, planned, seed, lib):
    """Say the carrier and cut the word out, retaking (the next seed) a take whose sentence or
    cut word is an implausible length. Returns (samples, rate, entry for the manifest, the whole
    carrier take)."""
    text, first, count = planned
    cfg = lib['carrier_cut']
    lo, hi = lib['phrase_seconds']
    clo, chi = cfg['seconds']
    rejected = []
    for take in range(lib['max_takes']):
        x, rate = eng.say(text, seed + take)
        x = as_saved(x)
        whole = len(ae.trim(x, rate)) / rate
        spans = aligner.spans(x, rate, text)
        cut = cut_points(x, rate, spans, first, count, cfg, words(text))
        y = finish(x, rate, cut)
        ok = lo <= whole <= hi and clo <= len(y) / rate <= chi
        if ok:
            break
        rejected.append({'seed': seed + take, 'seconds': round(whole, 3),
                         'cut_seconds': round(len(y) / rate, 3)})
    score = sum(s[2] for s in spans[first:first + count]) / count
    return y, rate, {'text': text, 'seed': seed + take, 'cut': list(cut),
                     'align_score': round(score, 3) if math.isfinite(score) else None,
                     'seconds': round(len(y) / rate, 3), 'rejected': rejected,
                     'accepted': ok}, x
