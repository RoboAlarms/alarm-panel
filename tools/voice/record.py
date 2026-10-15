"""Speak the whole recording script with the chosen voice (run by voice.py inside its environment).

library.json names the voice (one from audition.json) and the script, recording-script.csv.
Under <out> it writes:
  <clip_id>.wav    each word ("Front.") or phrase, trimmed and levelled
  extra/           clips outside the script, for trying splits ("Living" + "Room")
  carriers/        each word's whole carrier take, the sentence it was cut from
  manifest.json    every clip's text, seed, length and rejected takes
  provenance.json  the model revisions and package versions
and with --reels, built from every clip on disk:
  reel.wav         every script clip in order, to listen to in one go
  examples.wav     names and status messages joined from the clips, as the panel would say them
  compare.wav      each "compare" pair: spoken whole, then joined from parts
Only the clips not made yet, or whose text changed, are spoken (all of them with --again);
clips the script no longer has are removed. With --only, just those clips are spoken again, with
seeds no earlier round used; with --recut, every carrier word is cut again from its take in
carriers/ with library.json's "carrier_cut" (no model, no new takes). A word with a carrier in
library.json ("{} contact tripped." for a position word, "Armed away." for "Away") is spoken
inside it and cut out (carrier.py); any other text that ends in punctuation is spoken as
written, and any other word on its own, "Front.".
library.json's "say" respells a clip the model mispronounces ("Pattio" for Patio), and its
"seeds" pins the take chosen from `voice.py takes` (the same text and seed give the same
take). In the examples, words are joined by the pack's word gap ("Armed" "Away") and "|" is its
part gap, before a new sentence ("Armed" "Night" | "No entry delay.").
"""
import argparse
import csv
import json
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import audio_edit as ae  # noqa: E402
import carrier  # noqa: E402
import engines  # noqa: E402
import voicepack as vp  # noqa: E402
from audition import provenance, speak  # noqa: E402


def endings(rows, lib):
    """Each script word's ending, when library.json has "endings": the word as it sounds at the
    end of a spoken name (voicepack.py, category 7), clip ID <word>.end, numeric ID the word's
    ending_id(). The rows' numeric IDs are text ("0x0001"), as the script writes them."""
    if not lib.get('endings'):
        return []
    out = []
    for r in rows:
        if r['kind'] != 'token':
            continue
        e = vp.ending_id(int(r['numeric_id'], 16))
        out.append(dict(r, clip_id=f"{r['clip_id']}.end", kind='ending', category='ending',
                        base=r['clip_id'], numeric_id=f'0x{e:04X}' if e else '',
                        file=f"{r['clip_id']}.end.wav"))
    return out


def clips_wanted(lib):
    """Every clip to make: the script's rows, the words' endings, then the extra ones."""
    with open(REPO / lib['script'], newline='', encoding='utf-8') as f:
        rows = [dict(r, file=f"{r['clip_id']}.wav") for r in csv.DictReader(f)]
    rows += endings(rows, lib)
    for text in lib['extra']:
        slug = re.sub(r'[^a-z0-9]+', '_', text.lower()).strip('_')
        rows.append({'clip_id': f'extra.{slug}', 'kind': 'extra', 'category': 'extra',
                     'text': text, 'file': f'extra/{slug}.wav'})
    for r in rows:
        r['spoken'] = spoken(r, lib)
    return rows


def spoken(row, lib):
    """What the model is asked to say for a script row: its carrier sentence, its respelling,
    its own sentence, or the word with library.json's token_text ("Front.")."""
    planned = carrier.plan(row, lib)
    if planned:
        return planned[0]
    return lib.get('say', {}).get(row['clip_id']) or (
        row['text'] if row['text'][-1] in '.!?' else lib['token_text'].format(row['text']))


def joined(texts, by_text, clips, lib, rate):
    """Join clips as the panel would: a short gap between words, the pause at a "|"."""
    out, gap = [], 0
    for t in texts:
        if t == '|':
            gap = lib['pause_ms']
            continue
        if out:
            out += ae.silence(rate, gap or lib['gap_ms'])
        out += clips[by_text[t.lower()]['clip_id']]
        gap = 0
    return out


def read_clips(folder, rows):
    clips, rate = {}, None
    for r in rows:
        clips[r['clip_id']], rate = ae.read_wav(folder / r['file'])
    return clips, rate


def listening(rows, clips, lib, rate):
    """The reels to listen to: {'reel': every script clip, 'examples': ..., 'compare': ...}."""
    by_text = {r['text'].lower(): r for r in rows}
    reel, last = [], None
    for r in rows:
        if r['category'] == 'extra':
            continue
        if last and r['category'] != last:
            reel += ae.silence(rate, 1000)
        reel += clips[r['clip_id']] + ae.silence(rate, 700)
        last = r['category']
    ex = []
    for words in lib['examples']:
        ex += joined(words, by_text, clips, lib, rate) + ae.silence(rate, 1200)
    cmp = []
    for whole, parts in lib['compare']:
        cmp += clips[by_text[whole.lower()]['clip_id']] + ae.silence(rate, 600)
        cmp += joined(parts, by_text, clips, lib, rate) + ae.silence(rate, 1400)
    return {'reel': reel, 'examples': ex, 'compare': cmp}


def reels(out, rows, lib):
    clips, rate = read_clips(out, rows)
    for name, x in listening(rows, clips, lib, rate).items():
        ae.write_wav(out / f'{name}.wav', x, rate)
    return clips, rate


def recut(out, rows, manifest, lib):
    """Cut every carrier word again from its saved take, keeping the take and its seed."""
    aligner = carrier.Aligner()
    n = 0
    for r in rows:
        planned = carrier.plan(r, lib)
        entry = manifest['clips'].get(r['clip_id'])
        whole = out / 'carriers' / f"{r['clip_id']}.wav"
        if not planned or not entry or entry.get('spoken') != planned[0] or not whole.exists():
            continue
        x, rate = ae.read_wav(whole)
        text, first, count = planned
        spans = aligner.spans(x, rate, text)
        cut = carrier.cut_points(x, rate, spans, first, count, lib['carrier_cut'],
                                 carrier.words(text))
        y = carrier.finish(x, rate, cut)
        ae.write_wav(out / r['file'], y, rate)
        entry.update(cut=list(cut), seconds=round(len(y) / rate, 3))
        n += 1
    prov = out / 'provenance.json'
    if prov.exists():
        p = json.loads(prov.read_text(encoding='utf-8'))
        p['aligner'] = aligner.info
        prov.write_text(json.dumps(p, indent=2) + '\n', encoding='utf-8')
    print(f'{n} carrier words cut again', flush=True)


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('--out', type=Path, required=True)
    a.add_argument('--again', action='store_true', help='speak every clip again')
    a.add_argument('--only', nargs='+', help='speak just these clip ids again')
    a.add_argument('--round', type=int, default=0, help='retake round; each one uses fresh seeds')
    a.add_argument('--reels', action='store_true', help='also write the reels to listen to')
    a.add_argument('--recut', action='store_true',
                   help="cut every carrier word again from carriers/, with no new takes")
    args = a.parse_args()
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    aud = json.loads((HERE / 'audition.json').read_text(encoding='utf-8'))
    voice = next(v for v in aud['voices'][lib['engine']] if v['id'] == lib['voice'])
    rows = clips_wanted(lib)
    man_path = args.out / 'manifest.json'
    manifest = {'locale': lib['locale'], 'engine': lib['engine'], 'voice': voice, 'clips': {}}
    if man_path.exists() and not args.again:
        manifest = json.loads(man_path.read_text(encoding='utf-8'))
    wanted = {r['clip_id']: r for r in rows}
    pins = lib.get('seeds', {})
    for cid in [c for c in manifest['clips'] if c not in wanted]:
        (args.out / manifest['clips'].pop(cid)['file']).unlink(missing_ok=True)
    if args.only:
        todo = [r for r in rows if r['clip_id'] in args.only]
    else:
        have = manifest['clips']
        todo = [r for r in rows if args.again or r['clip_id'] not in have
                or have[r['clip_id']]['spoken'] != r['spoken']
                or have[r['clip_id']]['seed'] != pins.get(r['clip_id'], have[r['clip_id']]['seed'])
                or not (args.out / r['file']).exists()]
    t0 = time.time()
    if args.recut:
        recut(args.out, rows, manifest, lib)
        todo = []
    if todo:
        eng = engines.ENGINES[lib['engine']]()
        (args.out / 'extra').mkdir(parents=True, exist_ok=True)
        eng.prepare(voice, aud['reference_text'], args.out)
        seed = lib['seed'] + 100 * args.round
        aligner = None
        for r in todo:
            planned = carrier.plan(r, lib)
            if planned:
                aligner = aligner or carrier.Aligner()
                y, rate, entry, whole = carrier.speak(eng, aligner, planned,
                                                      pins.get(r['clip_id'], seed), lib)
                (args.out / 'carriers').mkdir(exist_ok=True)
                ae.write_wav(args.out / 'carriers' / f"{r['clip_id']}.wav", whole, rate)
            else:
                phrase = r['kind'] != 'token' and ' ' in r['text']
                y, rate, entry = speak(eng, r['spoken'], pins.get(r['clip_id'], seed), lib,
                                       lib['phrase_seconds'] if phrase else lib['token_seconds'])
            ae.write_wav(args.out / r['file'], y, rate)
            entry.update(spoken=entry.pop('text'), text=r['text'], kind=r['kind'],
                         category=r['category'], file=r['file'], round=args.round)
            manifest['clips'][r['clip_id']] = entry
            print(f"{r['clip_id']}: {r['spoken']!r} {entry['seconds']} s", flush=True)
    clips, rate = reels(args.out, rows, lib) if args.reels else read_clips(args.out, rows)
    script = [r for r in rows if r['category'] != 'extra']
    manifest['rate'] = rate
    manifest['script_seconds'] = round(sum(len(clips[r['clip_id']]) for r in script) / rate, 2)
    man_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    if todo:
        prov = provenance(lib['engine'], eng, t0)
        old = args.out / 'provenance.json'
        was = json.loads(old.read_text(encoding='utf-8')).get('aligner') if old.exists() else None
        if aligner or was:
            prov['aligner'] = aligner.info if aligner else was
        (args.out / 'provenance.json').write_text(json.dumps(prov, indent=2) + '\n',
                                                  encoding='utf-8')
    print(f"{len(todo)} clips spoken; {len(script)} script clips, {manifest['script_seconds']} s "
          f"of speech -> {args.out}")


if __name__ == '__main__':
    main()
