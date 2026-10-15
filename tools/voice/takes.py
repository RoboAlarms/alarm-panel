"""Speak other takes of one clip to choose from (run by voice.py inside the engine's environment).

A take is one random draw of the model: the same text with another seed sounds different, and a
short word can come out wrong in one draw and right in the next. Writes
<voice>/takes/<clip_id>/<n>-<spelling>-seed<seed>.wav: --count takes of the clip's own text and
of each --say respelling; a word with a carrier is spoken in it and cut out, as `record` does
(carrier.py). voice.py then encodes them for listening. Pin the one chosen in
library.json: its seed in "seeds" and, for a respelling, the spelling in "say"; the same text and
seed give the same take again.
"""
import argparse
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio_edit as ae  # noqa: E402
import carrier  # noqa: E402
import engines  # noqa: E402
from audition import speak  # noqa: E402
from record import clips_wanted  # noqa: E402


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('clip_id')
    a.add_argument('--out', type=Path, required=True, help='the voice folder of the library')
    a.add_argument('--say', nargs='+', default=[], help='respellings to try too ("Dawg.")')
    a.add_argument('--count', type=int, default=3, help='takes of each spelling')
    args = a.parse_args()
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    aud = json.loads((HERE / 'audition.json').read_text(encoding='utf-8'))
    voice = next(v for v in aud['voices'][lib['engine']] if v['id'] == lib['voice'])
    row = next((r for r in clips_wanted(lib) if r['clip_id'] == args.clip_id), None)
    if row is None:
        sys.exit(f'{args.clip_id} is not a clip of the library')
    out = args.out / 'takes' / args.clip_id
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    eng = engines.ENGINES[lib['engine']]()
    eng.prepare(voice, aud['reference_text'], args.out)
    phrase = row['kind'] != 'token' and ' ' in row['text']
    bounds = lib['phrase_seconds'] if phrase else lib['token_seconds']
    man = json.loads((args.out / 'manifest.json').read_text(encoding='utf-8'))
    now = man['clips'].get(args.clip_id, {})  # the take the library has: not offered again
    aligner = carrier.Aligner() if carrier.plan(row, lib) else None
    n = 0
    for say in [None] + args.say:
        spelled = dict(lib, say={**lib.get('say', {}), row['clip_id']: say}) if say else lib
        planned = carrier.plan(row, spelled)
        text = planned[0] if planned else say or row['spoken']
        seeds = [lib['seed'] + 100 * k for k in range(1, args.count + 2)]
        seeds = [s for s in seeds if (text, s) != (now.get('spoken'), now.get('seed'))]
        for seed in seeds[:args.count]:
            if planned:
                y, rate, entry, _ = carrier.speak(eng, aligner, planned, seed, lib)
            else:
                y, rate, entry = speak(eng, text, seed, lib, bounds)
            n += 1
            slug = re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')
            name = f"{n}-{slug}-seed{entry['seed']}.wav"
            ae.write_wav(out / name, y, rate)
            print(f"{name}: {entry['seconds']} s", flush=True)


if __name__ == '__main__':
    main()
