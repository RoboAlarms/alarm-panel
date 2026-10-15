"""Speak the audition words and labels with one engine (run by voice.py inside its environment).

For each voice in audition.json it writes, under <out>/<voice>/:
  words/   each word on its own, trimmed and levelled
  joined/  each label made from those word clips, as the panel would play it
  whole/   each label spoken in one go by the model, to compare with the joined one
  reel.wav all of the above in that order, to listen to in one go
  manifest.json  every clip's text, length, seed and rejected takes
"""
import argparse
import json
import sys
import time
from importlib import metadata
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio_edit as ae  # noqa: E402
import engines  # noqa: E402


def slug(text):
    return text.lower().replace(' ', '_')


def speak(eng, text, seed, cfg, seconds):
    """Take the first take whose trimmed length is plausible; a silent or runaway take is retried
    with the next seed. Returns (samples, rate, entry for the manifest)."""
    lo, hi = seconds
    rejected = []
    for take in range(cfg['max_takes']):
        x, rate = eng.say(text, seed + take)
        y = ae.trim(x, rate)
        s = len(y) / rate
        if lo <= s <= hi:
            break
        rejected.append({'seed': seed + take, 'seconds': round(s, 3)})
    y = ae.level(y, rate)
    return y, rate, {'text': text, 'seed': seed + take, 'seconds': round(len(y) / rate, 3),
                     'rejected': rejected, 'accepted': lo <= s <= hi}


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('engine', choices=sorted(engines.ENGINES))
    a.add_argument('--out', type=Path, required=True)
    a.add_argument('--voice', action='append', help='only these voice ids')
    args = a.parse_args()
    cfg = json.loads((HERE / 'audition.json').read_text(encoding='utf-8'))
    seed = cfg['seed']
    t0 = time.time()
    eng = engines.ENGINES[args.engine]()
    for voice in cfg['voices'][args.engine]:
        if args.voice and voice['id'] not in args.voice:
            continue
        out = args.out / voice['id']
        for sub in ('words', 'joined', 'whole'):
            (out / sub).mkdir(parents=True, exist_ok=True)
        eng.prepare(voice, cfg['reference_text'], out)
        manifest = {'engine': args.engine, 'voice': voice, 'words': {}, 'joined': {}, 'whole': {}}
        clips = {}
        for w in cfg['words']:
            y, rate, entry = speak(eng, cfg['word_text'].format(w), seed, cfg, cfg['word_seconds'])
            clips[w] = y
            ae.write_wav(out / 'words' / f'{slug(w)}.wav', y, rate)
            manifest['words'][w] = entry
            print(f'{voice["id"]}: {w!r} {entry["seconds"]} s', flush=True)
        joined, whole = [], []
        for label in cfg['labels']:
            name = ' '.join(label)
            j = ae.join([clips[w] for w in label], rate, cfg['gap_ms'])
            ae.write_wav(out / 'joined' / f'{slug(name)}.wav', j, rate)
            manifest['joined'][name] = {'words': label, 'gap_ms': cfg['gap_ms'],
                                        'seconds': round(len(j) / rate, 3)}
            joined.append(j)
            text = cfg['label_text'].format(name[0] + name[1:].lower())
            y, rate, entry = speak(eng, text, seed, cfg, cfg['label_seconds'])
            ae.write_wav(out / 'whole' / f'{slug(name)}.wav', y, rate)
            manifest['whole'][name] = entry
            whole.append(y)
        reel = []
        for section in ([clips[w] for w in cfg['words']], joined, whole):
            for c in section:
                reel += c + ae.silence(rate, 700)
            reel += ae.silence(rate, 1000)
        ae.write_wav(out / 'reel.wav', reel, rate)
        manifest['rate'] = rate
        (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    prov = provenance(args.engine, eng, t0)
    (args.out / 'provenance.json').write_text(json.dumps(prov, indent=2) + '\n', encoding='utf-8')
    print(f'{args.engine}: done in {prov["seconds_taken"]} s -> {args.out}')


def provenance(engine, eng, t0):
    """What made the clips: the model revisions, the package versions and the GPU."""
    versions = {}
    for p in ('torch', 'transformers', 'kokoro', 'misaki', 'qwen-tts', 'voxcpm'):
        try:
            versions[p] = metadata.version(p)
        except metadata.PackageNotFoundError:
            pass
    import torch
    return {'engine': engine, 'models': eng.models, 'packages': versions,
            'gpu': torch.cuda.get_device_name(0) if torch.cuda.is_available() else None,
            'seconds_taken': round(time.time() - t0, 1), 'made': time.strftime('%Y-%m-%d %H:%M')}


if __name__ == '__main__':
    main()
