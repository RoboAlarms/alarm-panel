"""Run inside the codec environment by `voice.py try`: phrases joined from a recording's takes, as
the panel would join them, into one WAV for the panel's audio bench (docs/features/08 AUD-030).

  bench.py <takes folder> <out.wav> "Front Door" "Back Door" [--gap MS] [--between 800]
           [--level -6 [--limit] | --raw] [--last <takes folder>] [--words <folder>]
           [--names <folder>] [--forms <folder>] [--enhance light|strong]

Each word is looked up in the recording script (the longest match first: "Living room" before
"Living"), or in --words (<word>.wav, words spoken for the bench only), brought to the pack's
level (speaker.py: the quiet ends trimmed, -15 dB of voiced RMS, the gain capped at a -1 dBFS
peak; --level sets another voiced RMS in dB, the gain capped at the same peak, and --limit limits
the peaks instead, as revision 6's pack did; --raw keeps the master's) and resampled to 16 kHz as encode.py does; a phrase's words
are --gap ms apart (the pack's, library.json's gap_ms, by default) and the phrases --between ms.
--last takes each phrase's last word from another recording: a word in its phrase-final form (its
falling end and final lengthening) after words in their non-final form. --names adds whole names
(<name>.wav, "back_door.wav") that play only at the end of a phrase, as the panel would play one;
--forms takes a phrase's last word from <forms>/final/<word>.wav and the others from mid/, where
there is one (bench_takes.py makes both). --enhance tries what the panel could do to speech so it
stays crisp with its volume turned down (ENHANCE, enhance()): the bench's trial of it, before any
firmware. The codec is left out:
Opus at 32 kbps changes neither the timing nor the words.
"""
import argparse
import json
import math
import re
import sys
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio_edit as ae  # noqa: E402
import encode  # noqa: E402
import record  # noqa: E402
import speaker  # noqa: E402

RATE = 16000


def vocabulary(lib, extra=None, names=None):
    """Each script word's text (lower case) -> its row: the zone words first, then the rest, then
    the bench's own words (extra/<word>.wav), then whole names (names/<name>.wav, final only)."""
    rows = [r for r in record.clips_wanted(lib) if r['category'] != 'extra']
    words = {}
    for r in sorted(rows, key=lambda r: r['kind'] != 'token'):
        words.setdefault(r['text'].lower().rstrip('.!?'), r)
    for f in sorted(extra.glob('*.wav')) if extra and extra.is_dir() else []:
        words.setdefault(f.stem.replace('_', ' '), {'clip_id': f'bench.{f.stem}', 'file': f.resolve()})
    for f in sorted(names.glob('*.wav')) if names and names.is_dir() else []:
        words.setdefault(f.stem.replace('_', ' '),
                         {'clip_id': f'name.{f.stem}', 'file': f.resolve(), 'final_only': True})
    return words


def parse(phrase, words):
    """The rows a phrase is joined from, the longest match first (a final-only one only where it
    ends the phrase); a word the script lacks stops."""
    toks = re.findall(r"[a-z0-9']+", phrase.lower())
    out, i = [], 0
    while i < len(toks):
        for j in range(len(toks), i, -1):
            row = words.get(' '.join(toks[i:j]))
            if row and (j == len(toks) or not row.get('final_only')):
                out.append(row)
                i = j
                break
        else:
            sys.exit(f'"{toks[i]}" (in "{phrase}") is not a word of the recording script')
    return out


def with_forms(rows, forms):
    """The rows with each script word in its form from `forms` where it has one: the last word's
    from final/, the others' from mid/; any other word keeps its take."""
    if not forms:
        return rows
    out = []
    for i, row in enumerate(rows):
        where = 'final' if i == len(rows) - 1 else 'mid'
        f = forms / where / f"{row.get('text', '').lower().replace(' ', '_')}.wav"
        use = row.get('text') and f.is_file()
        out.append(dict(row, clip_id=f'{where}.{f.stem}', file=f.resolve()) if use else row)
    return out


voiced_db = speaker.voiced_db
limit = speaker.limit  # the pack's own limiter (the speaker stage)


# Low-volume enhancement, as the panel would do it to speech before its volume turns it down: a
# voice turned down loses its soft consonants and its highs before it gets quiet. Each setting:
# (high-pass Hz, presence lift in dB at 3 kHz, compression ratio).
ENHANCE = {'light': (120.0, 3.0, 1.6), 'strong': (180.0, 6.0, 2.5)}
PRESENCE_HZ, PRESENCE_Q = 3000.0, 0.9


def biquad(x, coeffs):
    """`x` through one biquad section (b0, b1, b2, a1, a2; a0 = 1), as the firmware would run it."""
    b0, b1, b2, a1, a2 = coeffs
    out, x1, x2, y1, y2 = [], 0.0, 0.0, 0.0, 0.0
    for s in x:
        y = b0 * s + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, s, y1, y
        out.append(y)
    return out


def highpass(rate, f0, q=0.7071):
    """A 2nd-order high-pass (the Audio EQ Cookbook's): the boom a small speaker can't play."""
    w = 2.0 * math.pi * f0 / rate
    al, c = math.sin(w) / (2.0 * q), math.cos(w)
    a0 = 1.0 + al
    return ((1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0, -2.0 * c / a0,
            (1.0 - al) / a0)


def peaking(rate, f0, q, gain_db):
    """A peaking filter (the Audio EQ Cookbook's): `gain_db` at f0, none far from it."""
    big = 10.0 ** (gain_db / 40.0)
    w = 2.0 * math.pi * f0 / rate
    al, c = math.sin(w) / (2.0 * q), math.cos(w)
    a0 = 1.0 + al / big
    return ((1.0 + al * big) / a0, -2.0 * c / a0, (1.0 - al * big) / a0, -2.0 * c / a0,
            (1.0 - al / big) / a0)


def compress(x, rate, voice_db, ratio, rms_ms=10.0, attack_ms=5.0, release_ms=60.0):
    """Soft sounds brought nearer the loud ones. Above voice_db - 15 dB the gain falls at `ratio`;
    from there down to voice_db - 40 dB everything is lifted by what the voice's own level lost
    (the make-up), so the voice keeps its level and its consonants come up. Further down - breath,
    the room, hiss - the lift fades out over 10 dB, so the quiet between words is not raised (pack
    revision 2's compressor raised it and a hiss was heard). The level is a plain RMS over
    about rms_ms; the gain follows its target down in about attack_ms and back up in release_ms."""
    knee, floor = voice_db - 15.0, voice_db - 40.0
    makeup = 15.0 * (1.0 - 1.0 / ratio)
    k = lambda ms: 1.0 - math.exp(-1.0 / (rate * ms / 1000.0))  # noqa: E731
    km, ka, kr = k(rms_ms), k(attack_ms), k(release_ms)
    env, g, out = 0.0, 0.0, []
    for s in x:
        env += (s * s - env) * km
        lvl = 10.0 * math.log10(env) if env > 1e-12 else -120.0
        if lvl >= knee:
            want = makeup - (lvl - knee) * (1.0 - 1.0 / ratio)
        elif lvl >= floor:
            want = makeup
        else:
            want = makeup * max(0.0, 1.0 - (floor - lvl) / 10.0)
        g += (want - g) * (ka if want < g else kr)
        out.append(s * 10.0 ** (g / 20.0))
    return out


def enhance(x, rate, setting):
    """Speech as the panel would play it with the setting on: the boom cut, the soft sounds lifted,
    the consonants' band raised, then back to the same voiced level (so a comparison hears the
    clarity, not more volume) with its peaks limited under the pack's cap."""
    hp, presence, ratio = ENHANCE[setting]
    voice = voiced_db(x, rate)
    y = biquad(x, highpass(rate, hp))
    y = compress(y, rate, voice, ratio)
    y = biquad(y, peaking(rate, PRESENCE_HZ, PRESENCE_Q, presence))
    for _ in range(6):  # limiting lowers the level a little: raise it and limit again
        y = limit(ae.level(y, rate, target_db=voice, peak_db=60.0), rate,
                  speaker.SETTINGS['peak_db'])
        if voiced_db(y, rate) > voice - 0.3:
            break
    return y


def clip(folder, row, level, raw, limited=False):
    """The clip at 16 kHz, and its voiced level in dB."""
    x, rate = ae.read_wav(folder / row['file'])
    if not raw:
        s = dict(speaker.SETTINGS)  # the pack's level, or --level capped or --limit limited
        if level is not None:
            s.update(target_rms_db=level, limit=limited)
        x = speaker.for_speaker(x, rate, s)
        return encode.ceiling(encode.resample(x, rate, RATE)), voiced_db(x, rate)  # as the pack
    return [int(v) for v in encode.resample(x, rate, RATE)], voiced_db(x, rate)


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('takes', type=Path)
    a.add_argument('out', type=Path)
    a.add_argument('phrases', nargs='+')
    a.add_argument('--gap', type=int,
                   help="ms between a phrase's words (default: the pack's, library.json gap_ms)")
    a.add_argument('--between', type=int, default=800, help='ms between the phrases')
    a.add_argument('--raw', action='store_true', help="the masters' level, not the pack's")
    a.add_argument('--last', type=Path, help="each phrase's last word from these takes")
    a.add_argument('--level', type=float, help="the voiced RMS in dB (the pack's is -15, the gain capped)")
    a.add_argument('--words', type=Path, help='a folder of <word>.wav the script lacks')
    a.add_argument('--limit', action='store_true',
                   help='every word at --level, its peaks limited instead of the gain capped')
    a.add_argument('--names', type=Path, help='a folder of whole names, played at a phrase end')
    a.add_argument('--forms', type=Path, help='word forms: <forms>/final/ and <forms>/mid/')
    a.add_argument('--enhance', choices=sorted(ENHANCE),
                   help='low-volume enhancement, as the panel would do it (enhance())')
    a.add_argument('--send-db', type=float, default=0.0,
                   help='the finished audio this many dB up or down (the bench sends revision '
                        "6's sound 9 dB down, so the panel's Voice boost brings it back)")
    args = a.parse_args()
    if args.limit and (args.level is None or args.raw):
        sys.exit('--limit goes with --level, not --raw')
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    gap = lib['gap_ms'] if args.gap is None else args.gap
    words = vocabulary(lib, args.words, args.names)
    pcm = []
    for n, phrase in enumerate(args.phrases):
        if n:
            pcm += [0] * (RATE * args.between // 1000)
        rows = with_forms(parse(phrase, words), args.forms)
        levels = []
        for i, row in enumerate(rows):
            if i:
                pcm += [0] * (RATE * gap // 1000)
            last = args.last if args.last and i == len(rows) - 1 else args.takes
            y, db = clip(last, row, args.level, args.raw, args.limit)
            pcm += y
            levels.append(f"{row['clip_id']} {db:.1f} dB")
        print(f"{phrase}: {', '.join(levels)}")
    if args.enhance:
        x = enhance([v / 32768.0 for v in pcm], RATE, args.enhance)
        print(f'low volume {args.enhance}: voiced {voiced_db(x, RATE):.1f} dB, '
              f'peak {20 * math.log10(max(abs(v) for v in x)):.1f} dBFS')
        pcm = [round(v * 32768.0) for v in x]
    if args.send_db:
        g = 10.0 ** (args.send_db / 20.0)
        pcm = [round(v * g) for v in pcm]
        print(f'sent {args.send_db:+.1f} dB')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(args.out), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(max(-32768, min(32767, s)).to_bytes(2, 'little', signed=True)
                               for s in pcm))
    print(f'{args.out}: {len(pcm) / RATE:.1f} s')


if __name__ == '__main__':
    main()
