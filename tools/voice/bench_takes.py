"""Takes for trying the joins research's recommendations on the voice bench (docs/features/08
AUD-030) before a whole recording is made that way: whole names, and each word in the form its
place in a name asks for. Run by `voice.py bench-takes`; the pack is unchanged.

The research (2026-10-09) found that a word spoken alone ends the way a sentence does - long, its
pitch falling to the bottom of the voice - so a name joined from such words sounds like a list,
while a word cut from the middle of a sentence ("Front {} tripped.") comes out short and
unaccented, so a name ending in one sounds rushed. What the bench tries, as it recommends:
  - whole names: a common name spoken in one go, with no join at all; spoken alone ("Back door.",
    names/alone/) and at the end of an announcement ("Not ready. Check the zones. Back door.",
    names/leadin/). A whole name plays only at the end of a phrase, as the panel would play one;
  - word forms: a name's last word cut from the end of an announcement (long and falling,
    forms/final/), the words before it cut from a name spoken alone ("Back door." gives "back",
    forms/mid/). A word is cut where a stop's closure leaves a moment of silence beside it:
    "Patio" from "Back patio door.", a last word after "Back deck" ("... Back deck motion."). Cut
    where one word runs into the next ("patio motion", "kitchen window") it starts mid-sound and
    plays choppy (heard in "Back patio motion", 2026-10-09), so a take whose cut
    starts louder than CLEAN_DB under its loudest is picked only when no take starts cleanly.
Each sentence is spoken with --seeds seeds and cut as carrier.py cuts (MMS_FA and its quiet
points). The take picked for each name and form is one Whisper hears as written, starting cleanly,
whose length is nearest the research's target (TARGETS, or target() from the original takes').

  bench_takes.py speak --voice <recording> --bench <folder> [--seeds 8] [--only <sentence id> ...]
  bench_takes.py pick --voice <recording> --bench <folder>                (any; after asr_check.py)

speak (in the engine's environment) writes every take and its cuts to <bench>/takes/ with a
manifest.json for asr_check.py (--only: just these sentences, the others' takes kept);
pick copies the chosen cuts to <bench>/names/ and <bench>/forms/, with takes/report.json.
"""
import argparse
import json
import math
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio_edit as ae  # noqa: E402
import carrier  # noqa: E402
from forms import CLEAN_DB, TARGETS, onset_db, rank, target  # noqa: E402,F401

NAMES = ('Back door', 'Front door', 'Garage door', 'Kitchen window', 'Back patio motion')
LEAD = 'Not ready. Check the zones.'
# Where each form is cut from: a non-final form from a name spoken alone, a final form from the
# end of an announcement, after "Back deck" (the "k" a clean place to cut).
MID = {'back': 'Back door', 'front': 'Front door', 'garage': 'Garage door',
       'kitchen': 'Kitchen window', 'patio': 'Back patio door'}
FINAL = ('door', 'window', 'motion')
FINAL_AFTER = 'Back deck'



def slug(text):
    return text.lower().replace(' ', '_')


def spoken(name, lib):
    """A name as the engine is given it, with the library's respellings ("Pattio")."""
    say = {k.split('.', 1)[1]: v.rstrip('.!?').lower() for k, v in lib.get('say', {}).items()
           if k.startswith('word.')}
    text = ' '.join(say.get(w, w) for w in name.lower().split())
    return text[0].upper() + text[1:]


def sentences(lib):
    """Each sentence to speak: (id, text, its cuts: (kind, key, first word, word count))."""
    lead = len(carrier.words(LEAD))
    out = []
    for name in NAMES + tuple(src for src in MID.values() if src not in NAMES):
        words = name.lower().split()
        cuts = [('alone', name.lower(), 0, len(words))] if name in NAMES else []
        cuts += [('mid', w, words.index(w), 1) for w, src in MID.items() if src == name]
        out.append((f'alone.{slug(name)}', f'{spoken(name, lib)}.', cuts))
        if name in NAMES:
            out.append((f'leadin.{slug(name)}', f'{LEAD} {spoken(name, lib)}.',
                        [('leadin', name.lower(), lead, len(words))]))
    after = len(FINAL_AFTER.split())
    for w in FINAL:
        name = f'{FINAL_AFTER} {w}'
        out.append((f'final.{w}', f'{LEAD} {spoken(name, lib)}.', [('final', w, lead + after, 1)]))
    return out


def libraries():
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    aud = json.loads((HERE / 'audition.json').read_text(encoding='utf-8'))
    return lib, aud


def speak(voice_folder, bench, seeds, only=None):
    lib, aud = libraries()
    if 'carrier_cut' not in lib:
        sys.exit("bench-takes needs library.json's carriers, endings, form_takes and carrier_cut (revision 6, in git history): from revision 7 every word is recorded alone")
    import engines
    voice = next(v for v in aud['voices'][lib['engine']] if v['id'] == lib['voice'])
    eng = engines.ENGINES[lib['engine']]()
    with tempfile.TemporaryDirectory() as tmp:  # the reference only: nothing written beside it
        shutil.copy(voice_folder / 'reference.wav', Path(tmp) / 'reference.wav')
        eng.prepare(voice, aud['reference_text'], Path(tmp))
    aligner = carrier.Aligner()
    out = bench / 'takes'
    plan = sentences(lib)
    if only:
        unknown = set(only) - {sid for sid, _, _ in plan}
        if unknown:
            sys.exit(f'no such sentence: {" ".join(sorted(unknown))}')
        man = out / 'manifest.json'
        clips = json.loads(man.read_text(encoding='utf-8'))['clips'] if man.exists() else {}
        clips = {cid: c for cid, c in clips.items() if cid.split('.s')[0] not in only}
        for sid in only:
            shutil.rmtree(out / sid, ignore_errors=True)
        plan = [p for p in plan if p[0] in only]
    else:
        shutil.rmtree(out, ignore_errors=True)
        clips = {}
    out.mkdir(parents=True, exist_ok=True)
    for sid, text, cuts in plan:
        for k in range(seeds):
            seed = lib['seed'] + 100 * k
            x, rate = eng.say(text, seed)
            x = carrier.as_saved(x)
            spans = aligner.spans(x, rate, text)
            folder = out / sid / f's{seed}'
            folder.mkdir(parents=True)
            ae.write_wav(folder / 'take.wav', x, rate)
            for kind, key, first, count in cuts:
                cut = carrier.cut_points(x, rate, spans, first, count, lib['carrier_cut'],
                                         carrier.words(text))
                y = carrier.finish(x, rate, cut)
                f = folder / f'{kind}.{slug(key)}.wav'
                ae.write_wav(f, y, rate)
                score = sum(s[2] for s in spans[first:first + count]) / count
                clips[f'{sid}.s{seed}.{kind}.{slug(key)}'] = {
                    'file': f.relative_to(out).as_posix(), 'text': key, 'kind': kind, 'key': key,
                    'sentence': text, 'seed': seed, 'cut': list(cut),
                    'seconds': round(len(y) / rate, 3), 'align_score': round(score, 3)}
            print(f'{text} seed {seed}: ' + ', '.join(
                f"{c['kind']} {c['key']} {c['seconds']:.2f} s" for c in clips.values()
                if c['sentence'] == text and c['seed'] == seed), flush=True)
    (out / 'manifest.json').write_text(json.dumps({'clips': clips}, indent=1) + '\n',
                                       encoding='utf-8')


def pick(voice_folder, bench):
    takes = bench / 'takes'
    clips = json.loads((takes / 'manifest.json').read_text(encoding='utf-8'))['clips']
    check = takes / 'check.json'
    heard = ({r['clip']: r['ok'] for r in json.loads(check.read_text(encoding='utf-8'))['results']}
             if check.exists() else {})
    orig = json.loads((voice_folder / 'manifest.json').read_text(encoding='utf-8'))['clips']
    alone = {cid.split('.', 1)[1]: c['seconds'] for cid, c in orig.items()
             if cid.startswith('word.')}
    lib, _ = libraries()
    planned = {(text, kind, key) for _, text, cuts in sentences(lib) for kind, key, _, _ in cuts}
    groups = {}
    for c in clips.values():
        if (c['sentence'], c['kind'], c['key']) in planned:  # takes of an older plan stay unused
            c = dict(c, onset_db=round(onset_db(takes / c['file']), 1))
            groups.setdefault((c['kind'], c['key']), []).append(c)
    for d in ('names', 'forms'):
        shutil.rmtree(bench / d, ignore_errors=True)
    report = []
    for (kind, key), cands in sorted(groups.items()):
        lo, hi = target(kind, key, alone)
        best = min(cands, key=lambda c: rank(c, lo, hi, heard))
        dest = bench / ('names' if kind in ('alone', 'leadin') else 'forms') / kind
        dest.mkdir(parents=True, exist_ok=True)
        shutil.copy(takes / best['file'], dest / f'{slug(key)}.wav')
        good = [c for c in cands if heard.get(c['file']) is not False and lo <= c['seconds'] <= hi
                and c['onset_db'] <= CLEAN_DB]
        report.append({'kind': kind, 'key': key, 'target': [round(lo, 3), round(hi, 3)],
                       'picked': dict(best, heard=heard.get(best['file'])),
                       'in_target': len(good),
                       'takes': [[c['seed'], c['seconds'], heard.get(c['file']), c['onset_db']]
                                 for c in cands]})
        flag = {False: ' MISHEARD', None: ' (not checked)'}.get(heard.get(best['file']), '')
        flag += ' STARTS MID-SOUND' if best['onset_db'] > CLEAN_DB else ''
        print(f"{kind:6} {key:18} target {lo:.2f}-{hi:.2f} s: seed {best['seed']}, "
              f"{best['seconds']:.2f} s, starts {best['onset_db']:.0f} dB{flag}; "
              f"{len(good)}/{len(cands)} takes clean and in the target")
    (takes / 'report.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('step', choices=('speak', 'pick'))
    a.add_argument('--voice', type=Path, required=True,
                   help="the recording whose reference voice speaks and whose word lengths set "
                        "the targets (the original takes)")
    a.add_argument('--bench', type=Path, required=True, help='build/voice/bench')
    a.add_argument('--seeds', type=int, default=8, help='speak: takes of each sentence')
    a.add_argument('--only', nargs='+', metavar='ID',
                   help="speak: just these sentences (final.motion), keeping the others' takes")
    args = a.parse_args()
    if args.step == 'speak':
        speak(args.voice, args.bench, args.seeds, args.only)
    else:
        pick(args.voice, args.bench)


if __name__ == '__main__':
    main()
