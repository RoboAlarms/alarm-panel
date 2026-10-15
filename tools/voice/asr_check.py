"""Transcribe every clip with Whisper and flag the ones that don't say their text.

Runs inside the `asr` environment (voice.py check, and voice.py record after each round). A pass
doesn't prove a clip sounds right, only that a recognizer hears the right words: listening
decides. Writes check.json in the folder it checks.
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio_edit as ae  # noqa: E402

MODEL = 'openai/whisper-large-v3-turbo'
# Words that sound the same (and British spellings): a recognizer can't tell them apart, and
# neither can a listener hearing the word alone, so each counts as the first of its group.
HOMOPHONES = [('suite', 'sweet'), ('four', 'for'), ('fourth', 'forth'), ('two', 'to', 'too'),
              ('eight', 'ate'), ('one', 'won'), ('hall', 'haul'), ('main', 'maine', 'mane'),
              ('gym', 'jim'), ('cellar', 'seller'), ('night', 'knight'), ('great', 'grate'),
              ('break', 'brake'), ('ment', 'meant'), ('dow', 'dough', 'doe', 'dao'),
              ('high', 'hi'), ('low', 'lo', 'lowe'), ('stairs', 'stares'),
              ('theater', 'theatre'), ('sink', 'sync'), ('dryer', 'drier'),
              ('presence', 'presents')]
SAME = {w: g[0] for g in HOMOPHONES for w in g}


def norm(text):
    return ''.join(SAME.get(w, w) for w in re.findall(r'[a-z0-9]+', text.lower()))


def wanted(folder, m):
    """(path, text, clip id) for every clip a manifest lists: an audition's or a library's."""
    if 'clips' in m:
        for cid, c in m['clips'].items():
            yield folder / c['file'], c['text'], cid
        return
    for w in m['words']:
        yield folder / 'words' / (w.lower().replace(' ', '_') + '.wav'), w, None
    for k in ('joined', 'whole'):
        for name in m[k]:
            yield folder / k / (name.lower().replace(' ', '_') + '.wav'), name, None


def heard_as(heard, text, cid):
    """Whether a transcript says the text: a number may come back as digits ("12"). The clip
    ID may name a take ("number.12@s1234") or an ending ("number.12.end")."""
    h = norm(heard)
    return h == norm(text) or bool(cid and cid.startswith('number.')
                                   and h == cid.split('@')[0].split('.')[1])


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('folder', type=Path, help='the samples, or one voice of the library')
    args = a.parse_args()
    import numpy as np
    import torch
    import torchaudio.functional as F
    from transformers import pipeline
    asr = pipeline('automatic-speech-recognition', model=MODEL, device=0, torch_dtype=torch.float16)
    results, bad = [], 0
    for man in sorted(args.folder.glob('**/manifest.json')):
        m = json.loads(man.read_text(encoding='utf-8'))
        for path, text, cid in wanted(man.parent, m):
            x, rate = ae.read_wav(path)
            t = torch.tensor(x, dtype=torch.float32)
            if rate != 16000:
                t = F.resample(t, rate, 16000)
            heard = asr({'raw': t.numpy().astype(np.float32), 'sampling_rate': 16000},
                        generate_kwargs={'language': 'en'})['text'].strip()
            ok = heard_as(heard, text, cid)
            bad += not ok
            rel = path.relative_to(args.folder).as_posix()
            results.append({'clip': rel, 'clip_id': cid, 'want': text, 'heard': heard, 'ok': ok})
            print(f'{"ok  " if ok else "MISS"} {rel}: {heard!r}', flush=True)
    (args.folder / 'check.json').write_text(json.dumps({'model': MODEL, 'results': results},
                                                       indent=2) + '\n', encoding='utf-8')
    print(f'{len(results) - bad}/{len(results)} heard as written')


if __name__ == '__main__':
    main()
