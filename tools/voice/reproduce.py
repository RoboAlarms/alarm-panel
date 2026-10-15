"""Run inside a voice environment by voice.py: describe it, and check the tracked audio against it.

  reproduce.py env --out <json>               the environment: Python, every package, and PyAV's
                                              FFmpeg and libopus (codec) or torch, CUDA and the GPU
  reproduce.py lengths <folder> --out <json>  each script clip's length after encode.py's
                                              resampler (the samples the encoder was given)
  reproduce.py encode <voice dir> [--only ID ...]   re-encode the tracked masters with the pinned
                                              PyAV pipeline; compare packets, pre-skip and samples
  reproduce.py record <voice dir> [--only ID ...]   speak the clips again with the recorded seeds
                                              (no retakes); compare PCM with the masters
Either check prints every difference and exits 1 on any; neither writes the tracked folder.
"""
import argparse
import hashlib
import json
import os
import platform
import sys
import tempfile
import wave
from importlib import metadata
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio_edit as ae  # noqa: E402
import voicepack as vp  # noqa: E402


def environment():
    env = {'python': sys.version.split()[0], 'platform': platform.platform(),
           'packages': sorted(f"{d.metadata['Name']}=={d.version}" for d in metadata.distributions()
                              if d.metadata['Name'])}
    try:
        import av
        import ctypes
        env['pyav'] = av.__version__
        env['ffmpeg'] = getattr(av, 'ffmpeg_version_info', None)
        env['libraries'] = {k: '.'.join(map(str, v)) for k, v in av.library_versions.items()}
        libs = Path(av.__file__).resolve().parent.parent / 'av.libs'
        found = sorted(libs.glob('libopus*')) if libs.is_dir() else []
        if found:
            if os.name == 'nt':
                os.add_dll_directory(str(libs))
            opus = ctypes.CDLL(str(found[0]))
            opus.opus_get_version_string.restype = ctypes.c_char_p
            env['libopus'] = opus.opus_get_version_string().decode()
            env['libopus_file_sha256'] = hashlib.sha256(found[0].read_bytes()).hexdigest()
        else:
            env['libopus'] = 'linked into libavcodec: no separate library to ask'
    except ImportError:
        pass
    try:
        import torch
        env['torch'] = torch.__version__
        env['cuda'] = torch.version.cuda
        env['cudnn'] = torch.backends.cudnn.version()
        if torch.cuda.is_available():
            env['gpu'] = torch.cuda.get_device_name(0)
            env['gpu_capability'] = '.'.join(map(str, torch.cuda.get_device_capability(0)))
    except ImportError:
        pass
    return env


def wanted(meta, only):
    clips = [c for c in meta['clips'] if not only or c['clip_id'] in only]
    unknown = sorted(set(only or ()) - {c['clip_id'] for c in meta['clips']})
    if unknown:
        sys.exit(f'not in voice.json: {" ".join(unknown)}')
    return clips


def lengths(folder, out):
    import encode
    import record
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    n = {}
    for r in record.clips_wanted(lib):
        if r['category'] != 'extra':
            x, rate = ae.read_wav(folder / r['file'])
            n[r['clip_id']] = len(encode.for_pack(x, rate))
    out.write_text(json.dumps(n), encoding='utf-8')


def reencode(folder, only):
    import encode
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    meta = json.loads((folder / 'voice.json').read_text(encoding='utf-8'))
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for c in wanted(meta, only):
            cid = c['clip_id']
            x, rate = ae.read_wav(folder / 'masters' / f'{cid}.wav')
            ogg = Path(tmp) / f'{cid}.opus'
            encode.encode(x, rate, ogg, lib['kbps'])
            try:
                packets, pre, valid = vp.opus_stream(ogg.read_bytes(), len(encode.for_pack(x, rate)))
            except vp.FormatError as e:
                print(f'{cid}: {e}')
                bad += 1
                continue
            framed = vp.frame_packets(packets)
            got = (hashlib.sha256(framed).hexdigest(), pre, valid, len(packets))
            want = (c['packets_sha256'], c['pre_skip'], c['valid_samples'], c['packet_count'])
            if got != want:
                bad += 1
                old = vp.unframe((folder / 'packets' / f'{cid}.opuspkt').read_bytes())
                first = next((i for i, (a, b) in enumerate(zip(old, packets)) if a != b),
                             min(len(old), len(packets)))
                print(f'{cid}: differs from packet {first} (pre-skip {pre} vs {c["pre_skip"]}, '
                      f'samples {valid} vs {c["valid_samples"]}, packets {len(packets)} vs '
                      f'{c["packet_count"]})')
    n = len(wanted(meta, only))
    print(f'encode --reproduce: {n - bad} of {n} clips re-encode to the tracked packets')
    return bad


def rerecord(folder, only):
    import array
    import carrier
    import engines
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    aud = json.loads((HERE / 'audition.json').read_text(encoding='utf-8'))
    meta = json.loads((folder / 'voice.json').read_text(encoding='utf-8'))
    prov = json.loads((folder / 'provenance.json').read_text(encoding='utf-8'))
    voice = next(v for v in aud['voices'][lib['engine']] if v['id'] == lib['voice'])
    eng = engines.ENGINES[lib['engine']]()
    stale = bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        ref = Path(tmp) / 'reference.wav'
        ref.write_bytes((folder / 'reference.wav').read_bytes())
        eng.prepare(voice, aud['reference_text'], Path(tmp))
    for repo, rev in eng.models.items():
        if prov['generation']['models'].get(repo) != rev:
            print(f'{repo}: the cached snapshot is {rev}, the audio was made with '
                  f'{prov["generation"]["models"].get(repo)}')
            stale += 1
    for c in wanted(meta, only):
        cid = c['clip_id']
        x, rate = eng.say(c['spoken'], c['seed'])
        y = (carrier.finish(carrier.as_saved(x), rate, c['cut']) if 'cut' in c
             else carrier.finish(x, rate))
        got = array.array('h', (max(-32768, min(32767, round(s * 32767))) for s in y))
        with wave.open(str(folder / 'masters' / f'{cid}.wav'), 'rb') as w:
            want = array.array('h')
            want.frombytes(w.readframes(w.getnframes()))
            want_rate = w.getframerate()
        if rate != want_rate or got != want:
            bad += 1
            first = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b),
                         min(len(got), len(want)))
            print(f'{cid}: differs from sample {first} ({len(got)} samples at {rate} Hz, the '
                  f'master has {len(want)} at {want_rate} Hz)')
        else:
            print(f'{cid}: the same PCM', flush=True)
    n = len(wanted(meta, only))
    print(f'record --reproduce: {n - bad} of {n} clips regenerate the master')
    return bad + stale


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('command', choices=('env', 'lengths', 'encode', 'record'))
    a.add_argument('folder', type=Path, nargs='?')
    a.add_argument('--out', type=Path)
    a.add_argument('--only', nargs='+')
    args = a.parse_args()
    if args.command == 'env':
        args.out.write_text(json.dumps(environment()), encoding='utf-8')
    elif args.command == 'lengths':
        lengths(args.folder, args.out)
    elif args.command == 'encode':
        sys.exit(1 if reencode(args.folder, args.only) else 0)
    else:
        sys.exit(1 if rerecord(args.folder, args.only) else 0)


if __name__ == '__main__':
    main()
