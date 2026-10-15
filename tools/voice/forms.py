"""Every word of the voice in the two forms a spoken name needs (docs/features/08
voice-library.md "Word forms", AUD-031), as they were chosen by ear on the voice bench
(2026-10-09, bench_takes.py): a word's own clip, for the words before a name's last, and its
ending, for the last.

A word spoken alone ends the way a sentence does - long, its pitch falling - so a name joined
from such words sounds like a list; a word cut from the middle of a long sentence comes out short
and rushed. So each word is spoken twice, in library.json's carriers (carrier.plan):
  - its own clip in a short name, before a word that starts with a stop ("Back {} door.",
    "Back {} two."): it comes out as the first part of a name, and the stop's closure leaves a
    moment of silence to cut at;
  - its ending at the end of an announcement, after "Back deck" ("Not ready. Check the zones.
    Back deck {}.", library.json "endings"): long and falling, as a name's last word is. The "k"
    of "deck" gives the cut a clean start; cut where one word runs into the next ("patio motion"),
    a word starts mid-sound and plays choppy (heard 2026-10-09).
Each carrier sentence is spoken with library.json's "form_takes" seeds and cut as carrier.py cuts
(MMS_FA and its quiet points); words that share a sentence ("Zone two door." for "zone" and "two")
share its takes. The take kept is one Whisper hears as written, starting cleanly (its first 5 ms
at least CLEAN_DB under its loudest 5 ms), whose length is nearest its target (target(): a share
of the word's length spoken alone in the original recording, or the research's TARGETS).

Every word in both forms is twice the audio of one form, and the pack has a fixed room (948 KiB,
docs/features/08 audio-storage.md): at the targets' middles the words and endings come to about
242 s, and about 221 s fit (2026-10-09, measured). So pick then fits the budget (fit()): it
swaps picks for shorter takes that are as well heard and as clean, where they cost least - a
take's cost is the square of how far it falls under its target's middle, as a share of it - so
takes over the middle go first, and a long word loses a little before a short one loses as much.
It says how many clips it shortened and refuses, writing nothing, when even the shortest such
takes don't fit.

  forms.py speak --voice <recording> --out <folder> [--seeds N] [--only CLIP_ID ...] [--again]
                 [--part I/N]
  forms.py pick --voice <recording> --alone <original recording> --out <folder> [--budget B]

speak runs in the engine's environment and writes every take and its cuts to <out>/takes/ with
a manifest.json for asr_check.py, after every take; it keeps what an earlier run spoke, so a run
that stops can carry on (--again: speak everything again, or with --only just those clips'
sentences, their old takes removed from every part and the other sentences' kept; a sentence two
clips share, "Zone two door.", is cut for both); it speaks one take of every sentence before the
next, so a run stopped early leaves every word as many takes. --part I/N speaks every Nth take
of the whole run from the I-th into <out>/pI/, so N processes can share the GPU (it waits on the
model's steps, not its arithmetic: two run nearly twice as fast); a part restarted alone carries
on with its own share, when every part was started by this version of forms.py.
pick (any Python, after asr_check.py) copies each clip's chosen cut into the recording
(<clip>.wav; the whole take to carriers/) with its manifest entry, and writes <out>/report.json.
`voice.py forms` runs all three.
"""
import argparse
import json
import math
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio_edit as ae  # noqa: E402
import carrier  # noqa: E402
import record  # noqa: E402
import voicepack as vp  # noqa: E402

CLEAN_DB = -35.0  # a cut's first 5 ms at most this far under its loudest 5 ms
# The research's targets in seconds (the trimmed clip, no added silence); target() derives the rest.
TARGETS = {('mid', 'back'): (0.28, 0.35), ('mid', 'patio'): (0.45, 0.50),
           ('final', 'door'): (0.42, 0.50), ('final', 'motion'): (0.55, 0.63),
           ('name', 'back door'): (0.80, 0.90), ('name', 'back patio motion'): (1.35, 1.55)}
SYLLABLES = {'dryer': 2, 'fire': 1, 'foyer': 2}  # where counting vowel groups goes wrong
JOIN_S = 0.07  # the closure and release between two words of a name
KINDS = ('token', 'ending')
# A take's bytes in the pack, per second: framed Opus at 32 kbps after the speaker stage, measured
# on 1,072 forms takes (2026-10-09: 3,913 B/s on average, 3,322-5,144 for one take).
BYTES_PER_S = 3920
MARGIN = 8192  # the default budget's room for that estimate's error (export checks the real one)


def slug(text):
    return re.sub(r'[^a-z0-9]+', '_', text.lower()).strip('_')


def key_of(row):
    """The word a row is a form of, as target() and the original recording name it: "door",
    "living_room", "2"."""
    return row.get('base', row['clip_id']).split('.', 1)[1]


def form_of(row):
    return 'final' if row['kind'] == 'ending' else 'mid'


def syllables(text):
    """How many syllables `text` has, near enough to tell one from more: vowel groups, less a
    final silent "e", or SYLLABLES' count."""
    n = 0
    for w in re.findall(r"[a-z']+", text.lower()):
        if w in SYLLABLES:
            n += SYLLABLES[w]
            continue
        groups = re.findall(r'[aeiouy]+', w)
        k = len(groups)
        if k > 1 and groups[-1] == 'e' and w.endswith('e') and not w.endswith('le'):
            k -= 1
        n += max(1, k)
    return n


def onset_db(path):
    """How loud a cut starts: its first 5 ms against its loudest 5 ms, in dB. A word that starts
    in a pause and rises fast (an "m" after a closure) passes; one cut mid-sound does not."""
    x, rate = ae.read_wav(path)
    n = max(1, rate // 200)
    rms = ae._frame_rms(x, n)
    return 20.0 * math.log10(max(rms[0], 1e-9) / max(max(rms), 1e-9)) if rms else 0.0


def target(kind, key, alone, text=None):
    """(low, high) seconds for a cut: the research's, or from `alone` (word -> its length spoken
    alone in the original takes): a non-final word 0.6-0.75 of it (0.72-0.82 with more than one
    syllable, which shortens less; `text` is what it says, the key's own words without it), a
    final word 0.85-1.0, a whole name its words and joins."""
    k = ('name' if kind in ('alone', 'leadin') else kind, key)
    if k in TARGETS:
        return TARGETS[k]
    if kind == 'mid':
        one = syllables(text if text is not None else key.replace('_', ' ')) == 1
        lo, hi = (0.6, 0.75) if one else (0.72, 0.82)
        return alone[key] * lo, alone[key] * hi
    if kind == 'final':
        return alone[key] * 0.85, alone[key]
    words = key.split()
    parts = [target('mid', w, alone) for w in words[:-1]] + [target('final', words[-1], alone)]
    joins = JOIN_S * (len(words) - 1)
    return sum(p[0] for p in parts) + joins, sum(p[1] for p in parts) + joins


HEARD = {True: 0, None: 1, False: 2}  # heard as written, not checked, misheard


def rank(c, lo, hi, heard):
    """Heard as written first (then not checked, then misheard), then starting cleanly, then
    inside the target, then nearest its middle."""
    s = c['seconds']
    return (HEARD[heard.get(c['file'])], c['onset_db'] > CLEAN_DB, not lo <= s <= hi,
            abs(s - (lo + hi) / 2))


def pack_bytes(seconds):
    """About what a clip of `seconds` takes in the pack, aligned as it is there."""
    return vp.align4(round(seconds * BYTES_PER_S))


def shortfall(c, at):
    """How far a take falls under `at` seconds, as a share of it (0 at or over it)."""
    return max(0.0, at - c['seconds']) / at


def fit(chosen, options, room):
    """Swap picks for shorter takes until their bytes fit `room`. chosen: {clip: take};
    options: {clip: (the middle of its target, the takes it may swap to)}. A take costs the square
    of how far it falls under that middle, as a share of it; each step takes the swap that adds
    the least cost for the bytes it saves (the most bytes when it adds none). So a take over the
    middle goes first, a long word loses a little before a short one loses as much, and no word
    loses much while others could lose a little. Returns (chosen, bytes); bytes stays over `room`
    when no swap is left."""
    chosen = dict(chosen)
    used = sum(pack_bytes(c['seconds']) for c in chosen.values())
    while used > room:
        best = None
        for cid, (mid, takes) in options.items():
            cur = chosen[cid]
            was = shortfall(cur, mid) ** 2
            for t in takes:
                save = pack_bytes(cur['seconds']) - pack_bytes(t['seconds'])
                if save <= 0:
                    continue
                k = (round((shortfall(t, mid) ** 2 - was) / save, 12), -save, cid)
                if best is None or k < best[0]:
                    best = (k, cid, t, save)
        if best is None:
            break
        _, cid, t, save = best
        chosen[cid] = t
        used -= save
    return chosen, used


def library():
    return json.loads((HERE / 'library.json').read_text(encoding='utf-8'))


def plan(lib, only=None):
    """{carrier sentence: [(row, first word, word count), ...]} for every word and ending (or just
    the clip IDs in `only`), in script order."""
    rows = [r for r in record.clips_wanted(lib) if r['kind'] in KINDS]
    if only:
        unknown = set(only) - {r['clip_id'] for r in rows}
        if unknown:
            sys.exit(f'no such word or ending: {" ".join(sorted(unknown))}')
        rows = [r for r in rows if r['clip_id'] in only]
    out = {}
    for r in rows:
        text, first, count = carrier.plan(r, lib)
        out.setdefault(text, []).append((r, first, count))
    return out


def share(todo, wanted, done, part, again=False):
    """The (sentence, seed) takes part I/N still has to speak: every sentence of `todo` with
    every seed of `wanted`, a seed at a time (a run stopped early leaves each word as many takes),
    every Nth from the I-th - its share fixed before what is `done` is left out, so a part
    restarted alone carries on with its own takes, whatever the others have spoken. `again`:
    todo's sentences are spoken again, done or not."""
    if again:
        done = {d for d in done if d[0] not in todo}
    every = [(t, s) for s in wanted for t in todo]
    return [x for x in every[part[0]::part[1]] if x not in done]


def forget(out, sentences):
    """Remove these sentences' takes from every manifest under `out` (its parts' included) and
    their folders, so the takes spoken again are the only ones."""
    for man in out.glob('**/manifest.json'):
        clips = json.loads(man.read_text(encoding='utf-8'))['clips']
        keep = {k: c for k, c in clips.items() if c['sentence'] not in sentences}
        if len(keep) != len(clips):
            tmp = man.with_name('manifest.json.tmp')
            tmp.write_text(json.dumps({'clips': keep}, indent=1) + '\n', encoding='utf-8')
            os.replace(tmp, man)
        for text in sentences:
            shutil.rmtree(man.parent / 'takes' / slug(text), ignore_errors=True)


def manifests(out):
    """Every take speak wrote under `out`, its parts' included, keyed as in their manifests, each
    file relative to `out`."""
    clips = {}
    for man in sorted(out.glob('**/manifest.json')):
        for k, c in json.loads(man.read_text(encoding='utf-8'))['clips'].items():
            clips[k] = dict(c, file=(man.parent / c['file']).relative_to(out).as_posix())
    return clips


def speak(voice_folder, out, seeds, only=None, again=False, part=(0, 1)):
    import engines
    lib = library()
    aud = json.loads((HERE / 'audition.json').read_text(encoding='utf-8'))
    voice = next(v for v in aud['voices'][lib['engine']] if v['id'] == lib['voice'])
    full = plan(lib)
    todo = {t: full[t] for t in plan(lib, only)}  # every clip a chosen sentence holds is cut
    if again and part[1] > 1:
        sys.exit('--again with --part: clear the folder once, then start the parts')
    if again and not only:
        shutil.rmtree(out, ignore_errors=True)
    elif again:
        forget(out, todo)
    home = out / f'p{part[0]}' if part[1] > 1 else out
    takes = home / 'takes'
    man = home / 'manifest.json'
    clips = json.loads(man.read_text(encoding='utf-8'))['clips'] if man.exists() else {}
    wanted = [lib['seed'] + 100 * k for k in range(seeds)]
    done = {(c['sentence'], c['seed']) for c in manifests(out).values()} if out.exists() else set()
    left = share(todo, wanted, done, part, again)  # with --only, the others' takes are kept
    print(f'{len(todo)} sentences x {seeds} takes: {len(left)} to speak', flush=True)
    if not left:
        return
    eng = engines.ENGINES[lib['engine']]()
    with tempfile.TemporaryDirectory() as tmp:  # the reference only: nothing written beside it
        shutil.copy(voice_folder / 'reference.wav', Path(tmp) / 'reference.wav')
        eng.prepare(voice, aud['reference_text'], Path(tmp))
    aligner = carrier.Aligner()

    def save():  # every take, through a new file: a stop never leaves half a manifest
        tmp = man.with_name('manifest.json.tmp')
        tmp.write_text(json.dumps({'clips': clips}, indent=1) + '\n', encoding='utf-8')
        os.replace(tmp, man)

    for n, (text, seed) in enumerate(left, 1):
        x, rate = eng.say(text, seed)
        x = carrier.as_saved(x)
        spans = aligner.spans(x, rate, text)
        folder = takes / slug(text) / f's{seed}'
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True)
        ae.write_wav(folder / 'take.wav', x, rate)
        for row, first, count in todo[text]:
            cut = carrier.cut_points(x, rate, spans, first, count, lib['carrier_cut'],
                                     carrier.words(text))
            y = carrier.finish(x, rate, cut)
            f = folder / f"{row['clip_id']}.wav"
            ae.write_wav(f, y, rate)
            score = sum(s[2] for s in spans[first:first + count]) / count
            clips[f"{row['clip_id']}@s{seed}"] = {
                'file': f.relative_to(home).as_posix(), 'text': row['text'].lower(),
                'clip_id': row['clip_id'], 'sentence': text, 'seed': seed, 'cut': list(cut),
                'seconds': round(len(y) / rate, 3),
                'align_score': round(score, 3) if math.isfinite(score) else None}
        save()
        print(f'[{n}/{len(left)}] {text} seed {seed}', flush=True)


def fixed_bytes(lib, recording):
    """About what the pack takes besides the words and endings: its header, index and strings
    and the other clips (the status and walk-test words, from the recording)."""
    rows = [r for r in record.clips_wanted(lib) if r['kind'] in KINDS + ('part', 'phrase')]
    strings = sum(len(r['text'].encode('utf-8')) + len(r['clip_id']) + 2 for r in rows)
    others = sum(pack_bytes(recording['clips'][r['clip_id']]['seconds']) for r in rows
                 if r['kind'] not in KINDS and r['clip_id'] in recording['clips'])
    return vp.HEADER_SIZE + vp.ENTRY_SIZE * len(rows) + vp.align4(strings) + others


def pick(voice_folder, alone_folder, out, budget=None):
    """Copy each word's and ending's chosen take into the recording at voice_folder, the
    payload kept under `budget` bytes (by default the pack's limit less MARGIN)."""
    lib = library()
    clips = manifests(out)
    check = out / 'check.json'
    heard = ({r['clip']: r['ok'] for r in json.loads(check.read_text(encoding='utf-8'))['results']}
             if check.exists() else {})
    if check.exists():
        unchecked = [c for c in clips.values() if c['file'] not in heard]
        if unchecked:
            print(f'{len(unchecked)} takes are not in check.json: run asr_check.py again, or '
                  'they rank after the takes Whisper heard as written')
    orig = json.loads((alone_folder / 'manifest.json').read_text(encoding='utf-8'))['clips']
    alone = {cid.split('.', 1)[1]: c['seconds'] for cid, c in orig.items()
             if cid.startswith(('word.', 'number.'))}
    rec_path = voice_folder / 'manifest.json'
    recording = json.loads(rec_path.read_text(encoding='utf-8'))
    groups = {}
    for text, cuts in plan(lib).items():
        for row, _, _ in cuts:
            groups[row['clip_id']] = (row, [])
    for c in clips.values():
        g = groups.get(c['clip_id'])
        if g and c['sentence'] == carrier.plan(g[0], lib)[0]:  # takes of an older plan stay unused
            g[1].append(dict(c, onset_db=round(onset_db(out / c['file']), 1)))
    missing = [cid for cid, (_, cands) in groups.items() if not cands]
    if missing:
        sys.exit(f'no takes for {len(missing)} clips: {" ".join(missing)} (speak them first)')
    # The best take of each clip, then the budget: shorter takes as well heard and as clean.
    chosen, options, targets = {}, {}, {}
    for cid, (row, cands) in groups.items():
        said = lib['say'].get(row.get('base', row['clip_id']), row['text'])
        lo, hi = target(form_of(row), key_of(row), alone, said)
        best = min(cands, key=lambda c: rank(c, lo, hi, heard))
        b = rank(best, lo, hi, heard)[:2]
        chosen[cid], targets[cid] = best, (lo, hi)
        options[cid] = ((lo + hi) / 2, [c for c in cands if rank(c, lo, hi, heard)[:2] <= b])
    budget = vp.PAYLOAD_MAX - MARGIN if budget is None else budget
    fixed = fixed_bytes(lib, recording)
    first = sum(pack_bytes(c['seconds']) for c in chosen.values())
    fitted, used = fit(chosen, options, budget - fixed)
    if fixed + used > budget:
        sys.exit(f'the words and endings do not fit: about {fixed + used} bytes of payload with '
                 f'the shortest takes allowed, over the budget of {budget} (the pack holds '
                 f'{vp.PAYLOAD_MAX}); nothing written')
    shortened = [cid for cid in chosen if fitted[cid] is not chosen[cid]]
    under0 = sum(1 for cid in chosen if shortfall(chosen[cid], targets[cid][0]) > 0)
    under = [shortfall(fitted[cid], targets[cid][0]) for cid in chosen]
    under = [u for u in under if u > 0]
    lost = [shortfall(fitted[cid], sum(targets[cid]) / 2) for cid in shortened]
    report = []
    (voice_folder / 'carriers').mkdir(exist_ok=True)
    for cid, (row, cands) in groups.items():
        kind = form_of(row)
        lo, hi = targets[cid]
        best = fitted[cid]
        shutil.copy(out / best['file'], voice_folder / row['file'])
        shutil.copy(out / Path(best['file']).parent / 'take.wav',
                    voice_folder / 'carriers' / f'{cid}.wav')
        good = [c for c in cands if heard.get(c['file']) is not False and lo <= c['seconds'] <= hi
                and c['onset_db'] <= CLEAN_DB]
        fitted_note = {'for_budget': True} if cid in shortened else {}
        recording['clips'][cid] = {
            'spoken': best['sentence'], 'seed': best['seed'], 'cut': best['cut'],
            'align_score': best['align_score'], 'seconds': best['seconds'], 'rejected': [],
            'accepted': True, 'text': row['text'], 'kind': row['kind'],
            'category': row['category'], 'file': row['file'], 'round': 0,
            'form': {'heard': heard.get(best['file']), 'onset_db': best['onset_db'],
                     'target': [round(lo, 3), round(hi, 3)], 'takes': len(cands),
                     'clean_in_target': len(good), **fitted_note}}
        report.append({'clip_id': cid, 'form': kind, 'target': [round(lo, 3), round(hi, 3)],
                       'picked': dict(best, heard=heard.get(best['file'])),
                       'best_before_budget': chosen[cid]['seconds'],
                       'clean_in_target': len(good),
                       'takes': [[c['seed'], c['seconds'], heard.get(c['file']), c['onset_db']]
                                 for c in cands]})
        flag = {False: ' MISHEARD', None: ' (not checked)'}.get(heard.get(best['file']), '')
        flag += ' STARTS MID-SOUND' if best['onset_db'] > CLEAN_DB else ''
        flag += '' if lo <= best['seconds'] <= hi else ' OUTSIDE ITS TARGET'
        flag += ' (shorter, for the budget)' if cid in shortened else ''
        print(f"{cid:28} {best['seconds']:.2f} s ({lo:.2f}-{hi:.2f}), seed {best['seed']}, "
              f"starts {best['onset_db']:.0f} dB{flag}; {len(good)}/{len(cands)} clean in target")
    rec_path.write_text(json.dumps(recording, indent=2) + '\n', encoding='utf-8')
    (out / 'report.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
    misheard = [r['clip_id'] for r in report if r['picked']['heard'] is False]
    unchecked = [r['clip_id'] for r in report if r['picked']['heard'] is None]
    choppy = [r['clip_id'] for r in report if r['picked']['onset_db'] > CLEAN_DB]
    print(f'{len(report)} clips picked into {voice_folder}; misheard: {" ".join(misheard) or "none"}; '
          f'starting mid-sound: {" ".join(choppy) or "none"}; not checked by Whisper: '
          + (str(len(unchecked)) if len(unchecked) > 20 else " ".join(unchecked) or 'none'))
    print(f'payload about {fixed + used} bytes of {budget} ({fixed + first} with the best takes); '
          f'{len(shortened)} clips shorter for the budget; {len(under)} under their targets '
          f'({under0} with the best takes)'
          + (f' by {100 * sum(under) / len(under):.1f} % on average, at most '
             f'{100 * max(under):.1f} %' if under else '')
          + (f'; the shortened {100 * sum(lost) / len(lost):.1f} % under their targets\' middles '
             f'on average, at most {100 * max(lost):.1f} %' if lost else ''))


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('step', choices=('speak', 'pick'))
    a.add_argument('--voice', type=Path, required=True,
                   help='the recording: speak clones its reference, pick writes into it')
    a.add_argument('--out', type=Path, required=True, help='the takes: build/voice/forms/...')
    a.add_argument('--alone', type=Path,
                   help="pick: the original recording, whose words spoken alone set the targets")
    a.add_argument('--seeds', type=int, help="speak: takes of each sentence (library.json's "
                                             "form_takes)")
    a.add_argument('--only', nargs='+', metavar='CLIP_ID', help='speak: just these clips')
    a.add_argument('--again', action='store_true',
                   help="speak again: everything, or just --only's clips")
    a.add_argument('--budget', type=int, metavar='BYTES',
                   help=f'pick: the payload at most (default {vp.PAYLOAD_MAX - MARGIN})')
    a.add_argument('--part', default='0/1', metavar='I/N',
                   help='speak: every Nth take from the I-th, into <out>/pI/ (I from 0)')
    args = a.parse_args()
    missing = [k for k in ('carriers', 'endings', 'form_takes', 'carrier_cut') if k not in library()]
    if missing:
        sys.exit(f"forms needs library.json's carriers, endings, form_takes and carrier_cut (revision 6, in git history): from revision 7 every word is recorded alone; this one lacks {', '.join(missing)}")
    if args.step == 'speak':
        i, n = (int(v) for v in args.part.split('/'))
        if not 0 <= i < n:
            a.error('--part I/N needs 0 <= I < N')
        speak(args.voice, args.out, args.seeds or library()['form_takes'], args.only, args.again,
              (i, n))
    else:
        if args.alone is None:
            a.error('pick needs --alone')
        pick(args.voice, args.alone, args.out, args.budget)


if __name__ == '__main__':
    main()
