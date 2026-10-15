"""Make audition samples with local text-to-speech models (docs/features/08 voice-library.md).

Nothing heavy goes into git: each engine gets its own Python environment under build/voice/
(made with uv, which also fetches Python 3.12), and the models download into the Hugging Face
cache in the user's home folder the first time they are used. Needs an NVIDIA GPU with CUDA.
Any Python 3.10+ can run this file; it uses the standard library only.

  python tools/voice/voice.py setup [engine ...]     # the environments (downloads PyTorch once)
  python tools/voice/voice.py audition [engine ...]  # -> build/voice/samples/<engine>/<voice>/
  python tools/voice/voice.py check                  # Whisper transcribes every sample
  python tools/voice/voice.py record [--again]       # the whole script, in the chosen voice
  python tools/voice/voice.py record --recut         # cut the carrier words again (carrier.py)
  python tools/voice/voice.py encode                 # library.json's codec (Opus 32 kbps), to hear
  python tools/voice/voice.py encode --codec lc3 --kbps 32   # LC3 (builds liblc3 the first time)
  python tools/voice/voice.py takes word.dog [--say Dawg.]   # other takes of a clip that sounds off
  python tools/voice/voice.py try "Front Door" "Back Patio Motion" [--takes original] [--gap MS]
      [--last original] [--level -6 [--limit]] [--names alone|leadin] [--forms]
      [--enhance light|strong] [--device NAME]
                        # the phrases joined as the panel joins them; played on a development
                        # panel's speaker with --device (panelctl voice play, docs/features/08 AUD-030)
  python tools/voice/voice.py forms [word.door ...] [--seeds N] [--again]  # every word and its
                        # ending cut from carriers, the takes checked and picked into the recording
                        # (forms.py; then encode, export and pack)
  python tools/voice/voice.py bench-takes [final.motion ...] [--seeds 8]  # whole names and
                        # word forms for try
                        # --names and --forms (bench_takes.py: the joins research's trials)
  (--reels on either also joins the clips into reels: every clip, examples, compare)

The tracked voice, assets/voice/<locale>/<engine>-<voice>/ (tools/voice/README.md):
  python tools/voice/voice.py export                 # the recording -> masters, packets, voice.json
  python tools/voice/voice.py verify [--pack FILE]   # the tracked voice (or a signed pack) is sound
  python tools/voice/voice.py pack --out FILE (--key PEM | --unsigned) [--allow-draft]
  python tools/voice/voice.py encode --reproduce     # the encoder still makes the tracked packets
  python tools/voice/voice.py record --reproduce [--only ID ...]  # the model still makes them
verify and pack need no model, no encoder and nothing outside the standard library.

Engines: kokoro, qwen3, voxcpm2 (all by default); `asr` is the checker's environment and
`codec` the encoder's; `lc3` downloads Google's liblc3 and builds it with the host gcc.
`record` speaks recording-script.csv with the voice library.json names, into
build/voice/library/<locale>/<engine>-<voice>/: the clips not made yet or whose text changed
(all of them with --again). Whisper then checks every clip and the ones it mishears are spoken
again, up to library.json's "rounds". `takes` speaks other takes of one clip (and of any
respellings) into <voice>/takes/<clip id>/ as .opus; library.json's "seeds" and "say" pin the
one chosen.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import venv
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
WORK = REPO / 'build' / 'voice'
SAMPLES = WORK / 'samples'
LIBRARY = WORK / 'library'
ENGINES = ('kokoro', 'qwen3', 'voxcpm2')
UV = 'uv==0.12.15'  # 0.12.24 (2026-10-08) can't link the Python it downloads on Windows
PYTHON = '3.12'
TORCH_INDEX = 'https://download.pytorch.org/whl/cu128'
TORCH = ('torch==2.11.0', 'torchaudio==2.11.0')  # the newest on the cu128 index
# Google's LC3 codec (Bluetooth LE Audio), Apache-2.0: built from this release for `encode --codec lc3`.
LC3 = 'v1.1.3'
LC3_SHA256 = '276752ff54ce6a77d54ec133397b9d7e71f90caf3d9afa32d8b0e891b8ecb8af'
LC3_LIB = WORK / f'liblc3-{LC3}' / ('liblc3.dll' if os.name == 'nt' else 'liblc3.so')


def use_work(folder):
    global WORK, SAMPLES, LIBRARY, LC3_LIB
    WORK, SAMPLES, LIBRARY = folder, folder / 'samples', folder / 'library'
    LC3_LIB = WORK / LC3_LIB.parent.name / LC3_LIB.name


def exe(env, name):
    return env / ('Scripts' if os.name == 'nt' else 'bin') / (name + ('.exe' if os.name == 'nt' else ''))


def run(cmd, **kw):
    cmd = [str(c) for c in cmd]
    print('+', ' '.join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def uv():
    env = WORK / 'uv'
    tool = exe(env, 'uv')
    have = tool.exists() and subprocess.run([str(tool), '--version'], capture_output=True,
                                            text=True).stdout.split()[1:2] == [UV.split('==')[1]]
    if not have:
        if not exe(env, 'python').exists():
            venv.create(env, with_pip=True)
        run([exe(env, 'python'), '-m', 'pip', 'install', '-q', UV])
    return tool


def setup(name):
    tool = uv()
    env = WORK / f'venv-{name}'
    py = exe(env, 'python')
    if not py.exists():
        run([tool, 'venv', '--seed', '--python', PYTHON, env])
    if name != 'codec':
        run([tool, 'pip', 'install', '--python', py, '--index-url', TORCH_INDEX, *TORCH])
    run([tool, 'pip', 'install', '--python', py, '-r', HERE / 'requirements' / f'{name}.txt'])
    if name != 'codec':
        run([py, '-c', 'import torch; assert torch.cuda.is_available(), "PyTorch has no CUDA"; '
                       'print("torch", torch.__version__, "on", torch.cuda.get_device_name(0))'])


def host_gcc():
    """gcc from PATH, or the MinGW that CLion bundles (as tools/host_env.ps1 finds it)."""
    gcc = shutil.which('gcc')
    if not gcc and os.name == 'nt':
        clion = Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs' / 'CLion' / 'bin' / 'mingw' / 'bin'
        gcc = str(clion / 'gcc.exe') if (clion / 'gcc.exe').exists() else None
    if not gcc:
        sys.exit('no gcc: install one, or CLion, to build liblc3')
    return gcc


def setup_lc3():
    """Download liblc3's release, check it, and build the shared library the encoder loads."""
    import hashlib
    import tarfile
    import urllib.request
    if LC3_LIB.exists():
        return
    tgz = LC3_LIB.parent / f'liblc3-{LC3}.tar.gz'
    src = LC3_LIB.parent / 'src'
    LC3_LIB.parent.mkdir(parents=True, exist_ok=True)
    url = f'https://codeload.github.com/google/liblc3/tar.gz/refs/tags/{LC3}'
    print('+ download', url, flush=True)
    urllib.request.urlretrieve(url, tgz)
    if hashlib.sha256(tgz.read_bytes()).hexdigest() != LC3_SHA256:
        tgz.unlink()
        sys.exit(f'{url} is not the archive this script was written for (SHA-256 differs)')
    with tarfile.open(tgz) as t:
        members = []
        for m in t.getmembers():
            if '/' in m.name:  # drop the top folder, liblc3-<version>/
                m.name = m.name.split('/', 1)[1]
                members.append(m)
        t.extractall(src, members=members, filter='data')
    gcc = host_gcc()
    env = dict(os.environ, PATH=str(Path(gcc).parent) + os.pathsep + os.environ['PATH'])
    pic = [] if os.name == 'nt' else ['-fPIC']
    run([gcc, '-O2', '-std=c11', '-shared', '-static-libgcc', *pic, '-I', src / 'include',
         *sorted((src / 'src').glob('*.c')), '-o', LC3_LIB, '-lm'], env=env)


def environ():
    e = dict(os.environ, PYTHONUTF8='1', HF_HUB_DISABLE_SYMLINKS_WARNING='1')
    e.pop('PYTHONPATH', None)
    return e


def python(name):
    py = exe(WORK / f'venv-{name}', 'python')
    if not py.exists():
        sys.exit(f'no environment for {name}: run "voice.py setup {name}" first')
    return py


def check(folder):
    """Whisper's verdicts for a folder: the clip ids it didn't hear as written."""
    run([python('asr'), HERE / 'asr_check.py', folder], env=environ())
    results = json.loads((folder / 'check.json').read_text(encoding='utf-8'))['results']
    return [r['clip_id'] for r in results if not r['ok']]


def chosen_voice():
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    return lib, LIBRARY / lib['locale'] / f"{lib['engine']}-{lib['voice']}"


def record(again, reels, recut=False):
    lib, out = chosen_voice()
    if recut:
        # The takes stay as they are: cut them again, then let Whisper report what it hears.
        run([python(lib['engine']), HERE / 'record.py', '--out', out, '--recut']
            + ['--reels'] * reels, env=environ())
        misses = check(out)
        print(f'{out}: ' + (f'misheard: {" ".join(misses)}' if misses
                            else 'every clip heard as written'))
        return
    ref = out / 'reference.wav'
    if not ref.exists():
        # The reference is the voice: designing it again would give a different one.
        chosen = SAMPLES / lib['engine'] / lib['voice'] / 'reference.wav'
        if not chosen.exists():
            sys.exit(f'{chosen} is missing: it is the chosen voice, and designing it again '
                     'would make a different one')
        out.mkdir(parents=True, exist_ok=True)
        shutil.copy2(chosen, ref)
    py = python(lib['engine'])
    opt = ['--reels'] * reels
    run([py, HERE / 'record.py', '--out', out] + ['--again'] * again + opt, env=environ())
    for rnd in range(1, lib['rounds'] + 2):
        misses = check(out)
        if not misses or rnd > lib['rounds']:
            break
        run([py, HERE / 'record.py', '--out', out, '--round', rnd, *opt, '--only', *misses],
            env=environ())
    print(f'{out}: ' + (f'still misheard after {lib["rounds"]} rounds: {" ".join(misses)}'
                        if misses else 'every clip heard as written'))


def takes(clip_id, say, count):
    lib, out = chosen_voice()
    run([python(lib['engine']), HERE / 'takes.py', clip_id, '--out', out, '--count', count]
        + (['--say', *say] if say else []), env=environ())
    run([python('codec'), HERE / 'encode.py', out, '--kbps', lib['kbps'], '--takes', clip_id],
        env=environ())


# The files whose SHA-256 provenance.json records at export.
TOOL_FILES = ('tools/voice/library.json', 'tools/voice/audition.json', 'tools/voice/record.py',
              'tools/voice/audition.py', 'tools/voice/engines.py', 'tools/voice/audio_edit.py',
              'tools/voice/encode.py', 'tools/voice/speaker.py', 'tools/voice/carrier.py',
              'docs/features/08-Audio-and-Annunciation/recording-script.csv')


def in_env(name, *args):
    """Run reproduce.py in an environment and read the JSON it writes."""
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / 'out.json'
        run([python(name), HERE / 'reproduce.py', *args, '--out', out], env=environ())
        return json.loads(out.read_text(encoding='utf-8'))


def registry(lib):
    import tracked
    rows, problems = tracked.read_registry(REPO / lib['script'], lib)
    if problems:
        sys.exit('recording-script.csv:\n  ' + '\n  '.join(problems))
    return rows


def gpu_driver():
    try:
        return subprocess.run(['nvidia-smi', '--query-gpu=driver_version', '--format=csv,noheader'],
                              capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def reference_info(lib):
    """How the reference was made: the audition run that designed it recorded the models."""
    aud = json.loads((HERE / 'audition.json').read_text(encoding='utf-8'))
    voice = next(v for v in aud['voices'][lib['engine']] if v['id'] == lib['voice'])
    info = {'text': aud['reference_text'], 'design_seed': voice.get('seed', 1)}
    prov = SAMPLES / lib['engine'] / 'provenance.json'
    if voice.get('kind') == 'design' and prov.exists():
        p = json.loads(prov.read_text(encoding='utf-8'))
        info['design_models'] = {k: v for k, v in p['models'].items() if 'VoiceDesign' in k}
        info['design_models_from'] = (f"the audition run's provenance (build/voice/samples/"
                                      f"{lib['engine']}/provenance.json, made {p['made']})")
    return info


def export():
    import tracked
    lib, src = chosen_voice()
    rows = registry(lib)
    lengths = in_env('codec', 'lengths', src)
    encoder = in_env('codec', 'env')
    inference = in_env(lib['engine'], 'env')
    inference['gpu_driver'] = gpu_driver()
    dst = tracked.voice_dir(lib)
    problems = tracked.export(src, dst, rows, lib, lengths, encoder, inference,
                              reference_info(lib), TOOL_FILES)
    if problems:
        sys.exit('export refused, nothing written:\n  ' + '\n  '.join(problems))
    print(f'exported {len(rows)} clips -> {dst.relative_to(REPO).as_posix()}')
    verify(None)


def verify(pack_file):
    import tracked
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    rows = registry(lib)
    folder = tracked.voice_dir(lib)
    if pack_file:
        problems = tracked.check_built_pack(folder, rows, lib, Path(pack_file).read_bytes())
        if problems:
            sys.exit(f'{pack_file}:\n  ' + '\n  '.join(problems))
        print(f'{pack_file}: a signed pack of the tracked voice, envelope and payload as built')
        return
    problems, report = tracked.verify(folder, rows, lib)
    print(tracked.describe(report))
    if problems:
        sys.exit(f'{len(problems)} problems:\n  ' + '\n  '.join(problems))
    print('verify: the tracked voice is sound')


def pack(out, key, allow_draft):
    import tracked
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    rows = registry(lib)
    problems = tracked.pack(tracked.voice_dir(lib), rows, lib, out, key, allow_draft)
    if problems:
        sys.exit('pack refused:\n  ' + '\n  '.join(problems))
    size = Path(out).stat().st_size
    print(f"{out}: {size:,} bytes, {'signed' if key else 'unsigned (the PC simulator only)'}")


def takes_folder(takes):
    """'cut' (the recording now), 'original' (the words spoken alone, before the carriers: BL-202,
    pack revisions 1-3) or a folder."""
    lib, folder = chosen_voice()
    if takes == 'original':
        folder = folder.with_name(folder.name + '.r3-backup')
    elif takes != 'cut':
        folder = Path(takes)
    if not (folder / 'manifest.json').exists():
        sys.exit(f'{folder}: no recording there')
    return folder


def try_phrases(phrases, takes, gap, between, raw, out, device, last=None, level=None,
                limited=False, names=None, forms=False, enhance=None, send_db=None):
    """Join the phrases from a recording's takes (bench.py) and play them on a panel; words the
    script lacks come from build/voice/bench/words/<word>.wav, if spoken there. `names` (alone or
    leadin) and `forms` use bench-takes' whole names and word forms; `enhance` (light or strong)
    the low-volume enhancement the panel could do; `send_db` moves the finished audio up or down."""
    bench = WORK / 'bench'
    for want, folder in ((names, bench / 'names' / str(names)), (forms, bench / 'forms')):
        if want and not folder.is_dir():
            sys.exit(f'{folder}: none yet; run "voice.py bench-takes" first')
    name = (Path(takes).name + (f'-last-{Path(last).name}' if last else '')
            + ('-forms' if forms else '') + (f'-names-{names}' if names else '')
            + (f'-enhance-{enhance}' if enhance else '') + (f'-sent{send_db:+g}' if send_db else ''))
    if gap is None:  # the pack's
        gap = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))['gap_ms']
    out = Path(out) if out else bench / f'{name}-gap{gap}.wav'
    run([python('codec'), HERE / 'bench.py', takes_folder(takes), out, *phrases, '--gap', gap,
         '--between', between, '--words', bench / 'words'] + ['--raw'] * raw
        + (['--last', takes_folder(last)] if last else [])
        + (['--level', level] if level is not None else []) + ['--limit'] * limited
        + (['--names', bench / 'names' / names] if names else [])
        + (['--forms', bench / 'forms'] if forms else [])
        + (['--enhance', enhance] if enhance else [])
        + (['--send-db', send_db] if send_db else []), env=environ())
    if device:
        run([sys.executable, REPO / 'tools' / 'panelctl.py', '--device', device, 'voice', 'play', out])


def forms(seeds, only=None, again=False):
    """Speak every word and its ending in its carriers, check the takes with Whisper and pick
    the best into the recording (forms.py)."""
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    voice = LIBRARY / lib['locale'] / f"{lib['engine']}-{lib['voice']}"
    out = WORK / 'forms' / lib['locale'] / f"{lib['engine']}-{lib['voice']}"
    run([python(lib['engine']), HERE / 'forms.py', 'speak', '--voice', voice, '--out', out]
        + (['--seeds', seeds] if seeds else []) + (['--only', *only] if only else [])
        + (['--again'] if again else []), env=environ())
    run([python('asr'), HERE / 'asr_check.py', out], env=environ())
    run([sys.executable, HERE / 'forms.py', 'pick', '--voice', voice, '--alone',
         takes_folder('original'), '--out', out])


def bench_takes(seeds, only=None):
    """Whole names and word forms for the bench (bench_takes.py): spoken in the original takes'
    voice, checked by Whisper, the take nearest each target picked."""
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    voice, bench = takes_folder('original'), WORK / 'bench'
    run([python(lib['engine']), HERE / 'bench_takes.py', 'speak', '--voice', voice, '--bench',
         bench, '--seeds', seeds] + (['--only', *only] if only else []), env=environ())
    run([python('asr'), HERE / 'asr_check.py', bench / 'takes'], env=environ())
    run([sys.executable, HERE / 'bench_takes.py', 'pick', '--voice', voice, '--bench', bench])


def reproduce(what, only):
    import tracked
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    env = 'codec' if what == 'encode' else lib['engine']
    cmd = [python(env), HERE / 'reproduce.py', what, tracked.voice_dir(lib)]
    cmd += ['--only', *only] if only else []
    print('+', ' '.join(map(str, cmd)), flush=True)
    sys.exit(subprocess.run([str(c) for c in cmd], env=environ()).returncode)


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('command', choices=('setup', 'audition', 'check', 'record', 'encode', 'takes',
                                       'try', 'forms', 'bench-takes', 'export', 'verify',
                                       'pack'))
    a.add_argument('engines', nargs='*', help='engines; for takes, the clip id; for try, the '
                   'phrases; for forms, the clips to speak (all by default); for bench-takes, '
                   'the sentences to speak again (all by default)')
    a.add_argument('--again', action='store_true', help='record, forms: speak every clip again')
    a.add_argument('--recut', action='store_true',
                   help='record: cut the carrier words again from their takes (no model run)')
    a.add_argument('--codec', choices=('opus', 'lc3'), help="encode: the codec (library.json's)")
    a.add_argument('--kbps', type=int, help="encode: the bitrate (library.json's)")
    a.add_argument('--reels', action='store_true', help='record, encode: also make the reels')
    a.add_argument('--say', nargs='+', help='takes: respellings to try too ("Dawg.")')
    a.add_argument('--count', type=int, default=3, help='takes: takes of each spelling')
    a.add_argument('--reproduce', action='store_true',
                   help='record, encode: compare with the tracked masters / packets, write nothing')
    a.add_argument('--only', nargs='+', metavar='ID', help='--reproduce: just these clips')
    a.add_argument('--pack', metavar='FILE', help='verify: check a built, signed pack file')
    a.add_argument('--out', help='pack, try: the file to write')
    a.add_argument('--takes', default='cut',
                   help="try: 'cut' (the recording now), 'original' (the words spoken alone) or a folder")
    a.add_argument('--gap', type=int,
                   help="try: ms between a phrase's words (default: the pack's, library.json gap_ms)")
    a.add_argument('--between', type=int, default=800, help='try: ms between the phrases')
    a.add_argument('--raw', action='store_true', help="try: the masters' level, not the pack's")
    a.add_argument('--last', help="try: each phrase's last word from these takes (as --takes)")
    a.add_argument('--level', type=float,
                   help="try: the voiced RMS in dB (revision 6's is -6, limited)")
    a.add_argument('--limit', action='store_true',
                   help='try: every word at --level, its peaks limited as the pack does '
                        '(without it the gain is capped at the peak)')
    a.add_argument('--names', choices=('alone', 'leadin'),
                   help="try: bench-takes' whole names (spoken alone or after a lead-in), "
                        "at a phrase's end")
    a.add_argument('--forms', action='store_true',
                   help="try: bench-takes' word forms (final for the last word, mid before it)")
    a.add_argument('--enhance', choices=('light', 'strong'),
                   help='try: low-volume enhancement, as the panel could do it (bench.py)')
    a.add_argument('--send-db', type=float,
                   help='try: the finished audio this many dB up or down before it is sent')
    a.add_argument('--seeds', type=int, help="forms, bench-takes: takes of each sentence "
                   "(library.json's form_takes, 8 in revision 6)")
    a.add_argument('--device', help="try: play it on this enrolled development panel")
    k = a.add_mutually_exclusive_group()
    k.add_argument('--key', help='pack: sign with this key (espsecure sign_data --version 2)')
    k.add_argument('--unsigned', action='store_true',
                   help='pack: the payload alone, for the PC simulator')
    a.add_argument('--allow-draft', action='store_true',
                   help='pack: allow takes the listening review has not accepted (a DRAFT pack)')
    a.add_argument('--work', type=Path,
                   help="the build/voice folder with the environments and the recording "
                        "(default: this checkout's; another checkout's from a worktree)")
    args = a.parse_args()
    if args.work:
        use_work(args.work.resolve())
    if args.reproduce:
        if args.command not in ('record', 'encode'):
            sys.exit('--reproduce goes with record or encode')
        reproduce(args.command, args.only)
    if args.command == 'try':
        try_phrases(args.engines or ['Front Door', 'Back Door', 'Back Patio Motion'], args.takes,
                    args.gap, args.between, args.raw, args.out, args.device, args.last,
                    args.level, args.limit, args.names, args.forms, args.enhance, args.send_db)
        return
    if args.command == 'forms':
        forms(args.seeds, args.engines, args.again)
        return
    if args.command == 'bench-takes':
        bench_takes(args.seeds or 8, args.engines)
        return
    if args.command == 'export':
        export()
        return
    if args.command == 'verify':
        verify(args.pack)
        return
    if args.command == 'pack':
        if not args.out or not (args.key or args.unsigned):
            sys.exit('pack needs --out and one of --key or --unsigned')
        pack(args.out, args.key, args.allow_draft)
        return
    if args.command == 'takes':
        if len(args.engines) != 1:
            sys.exit('takes needs one clip id, such as word.dog')
        takes(args.engines[0], args.say, args.count)
        return
    if args.command == 'check':
        check(SAMPLES)
        return
    if args.command == 'record':
        record(args.again, args.reels, args.recut)
        return
    if args.command == 'encode':
        lib, out = chosen_voice()
        codec, kbps = args.codec or lib['codec'], args.kbps or lib['kbps']
        if codec == 'lc3':
            setup_lc3()
        run([python('codec'), HERE / 'encode.py', out, '--codec', codec, '--kbps', kbps,
             '--lc3-lib', LC3_LIB] + ['--reels'] * args.reels, env=environ())
        return
    helpers = ('asr', 'codec', 'lc3')
    names = args.engines or (ENGINES + helpers if args.command == 'setup' else ENGINES)
    for n in names:
        if n not in ENGINES + helpers or (args.command == 'audition' and n in helpers):
            sys.exit(f'unknown engine {n}')
        if args.command == 'setup' and n == 'lc3':
            setup_lc3()
        elif args.command == 'setup':
            setup(n)
        else:
            run([python(n), HERE / 'audition.py', n, '--out', SAMPLES / n], env=environ())


if __name__ == '__main__':
    main()
