"""The voice's tracked audio: export it from a recording, verify it, pack it (AUD-023, AUD-026).

assets/voice/<locale>/<engine>-<voice>/ holds what every clone needs to rebuild the identical
pack payload without a model or an encoder (voice-first-build.md "Reproducible audio", D13):
  reference.wav           the designed voice every clip clones
  masters/<clip_id>.wav   the chosen takes as recorded: PCM16 mono 24 kHz
  packets/<clip_id>.opuspkt  the clip's Opus packets as the pack stores them (u16 LE length +
                          bytes each, no Ogg): the canonical encoded audio
  voice.json              each clip's numeric ID, text, seed, hashes, trim and listening review,
                          and the payload SHA-256 recorded for each pack revision
  provenance.json         what made the audio; NOTICE.md  the terms it may be shared under
recording-script.csv is the numeric-ID registry (D07) and library.json the settings. Standard
library only, like voicepack.py; `voice.py` runs it.
"""
import csv
import hashlib
import inspect
import json
import re
import shutil
import sys
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import audio_edit  # noqa: E402
import record  # noqa: E402
import speaker  # noqa: E402
import voicepack as vp  # noqa: E402

ASSETS = REPO / 'assets' / 'voice'
MASTER_RATE = 24000
REVIEWS = ('pending', 'accepted')
ID_TEXT = re.compile(r'0x[0-9A-F]{4}\Z')     # the registry writes every ID as 0x + 4 hex digits
TOKEN_KINDS = ('token',)
NOTICE = """\
# Notice: the {locale} voice "{voice_id}"

This voice is synthetic. Every recording in this folder (`reference.wav`, `masters/`,
`packets/`) was generated on a computer with the Qwen3-TTS text-to-speech models: the
VoiceDesign model made `reference.wav` from a written description of a voice, and the Base model
spoke each line of the recording script by cloning that reference. It is not a recording of any
real person, and no real person's voice was used to make it.

The Qwen3-TTS models are published under the Apache License 2.0 (their model card on Hugging
Face); no model is distributed here. These audio files are part of this repository and may be
redistributed under the repository's licence (`LICENSE` at its root). `provenance.json` records
the model revisions, settings and software that made them.
"""


def voice_id(lib):
    return f"{lib['engine']}-{lib['voice']}"


def voice_dir(lib):
    return ASSETS / lib['locale'] / voice_id(lib)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(obj):
    return (json.dumps(obj, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def load_library():
    return json.loads((HERE / 'library.json').read_text(encoding='utf-8'))


# ------------------------------------------------------------------------------ the registry

def read_registry(path, lib):
    """The script rows of recording-script.csv with their numeric IDs: (rows, problems)."""
    with open(path, newline='', encoding='utf-8') as f:
        rows = list(csv.DictReader(f))
    rows += [{k: v for k, v in r.items() if k != 'file'} for r in record.endings(rows, lib)]
    problems = []
    for r in rows:
        t = r.get('numeric_id') or ''
        if not ID_TEXT.match(t):
            problems.append(f"{r['clip_id']}: numeric_id {t!r} is not 0x and four hex digits")
            r['numeric_id'] = None
        else:
            r['numeric_id'] = int(t, 16)
        r['spoken'] = record.spoken(r, lib)
    return rows, problems + registry_problems(rows)


def registry_problems(rows):
    """D07's rules for the registry: unique IDs in their namespaces, the 16 fixed ones."""
    p, seen, keys = [], {}, set()
    for r in rows:
        cid, nid, cat = r['clip_id'], r['numeric_id'], r['category']
        if cid in keys:
            p.append(f'{cid}: the clip ID repeats')
        keys.add(cid)
        if nid is None:
            continue
        if nid in seen:
            p.append(f'{cid}: numeric ID {nid:#06x} is also {seen[nid]}')
        seen[nid] = cid
        p += [f'{cid}: {e}' for e in vp.id_problems(nid, cat)]
        if (r['kind'] in TOKEN_KINDS) != (cat in vp.SELECTABLE):
            p.append(f'{cid}: a {r["kind"]} row in category {cat}')
    for cid, nid in vp.REQUIRED_IDS.items():
        row = next((r for r in rows if r['clip_id'] == cid), None)
        if row is None:
            p.append(f'{cid} is missing')
        elif row['numeric_id'] is not None and row['numeric_id'] != nid:
            p.append(f'{cid} is {row["numeric_id"]:#06x}, D07 makes it {nid:#06x}')
    if not any(r['category'] in vp.SELECTABLE for r in rows):
        p.append('no selectable token')
    return p


# -------------------------------------------------------------------------- the tracked files

def wav_problems(data, what, rate=MASTER_RATE):
    """(samples, problems) of a WAV that must be PCM16 mono at `rate` with some sound in it."""
    import io
    try:
        with wave.open(io.BytesIO(data), 'rb') as w:
            fmt = (w.getnchannels(), w.getsampwidth(), w.getframerate())
            n = w.getnframes()
            pcm = w.readframes(n)
    except (wave.Error, EOFError) as e:
        return 0, [f'{what}: not a WAV file ({e})']
    p = []
    if fmt != (1, 2, rate):
        p.append(f'{what}: {fmt[0]} channels, {8 * fmt[1]} bits, {fmt[2]} Hz; want mono PCM16 '
                 f'{rate} Hz')
    if len(pcm) != 2 * n:
        p.append(f'{what}: {len(pcm)} bytes of audio for {n} samples')
    if n == 0 or not any(pcm):
        p.append(f'{what}: no audio')
    return n, p


def load_meta(folder):
    return json.loads((folder / 'voice.json').read_text(encoding='utf-8'))


def is_draft(meta):
    return any(c['listening_review'] != 'accepted' for c in meta['clips'])


def payload_from(folder, meta, lib):
    """The unsigned RVOC payload the tracked files make (raises voicepack.FormatError)."""
    clips = []
    for c in meta['clips']:
        clips.append(dict(numeric_id=c['numeric_id'], category=c['category'], text=c['text'],
                          key=c['clip_id'],
                          data=(folder / 'packets' / f"{c['clip_id']}.opuspkt").read_bytes(),
                          valid_samples=c['valid_samples'], packet_count=c['packet_count'],
                          pre_skip=c['pre_skip']))
    return vp.build_payload(clips, revision=lib['pack_revision'], draft=is_draft(meta),
                            locale=lib['locale'], voice_id=voice_id(lib),
                            word_gap_ms=lib['gap_ms'], part_gap_ms=lib['pause_ms'],
                            nominal_bitrate=lib['kbps'] * 1000)


def voice_rows(meta, rows):
    """The registry rows a voice holds: all of them, or without the endings for a voice made
    before word forms (AUD-031). A voice has every word's ending or none."""
    if any(c.get('category') == 'ending' for c in meta.get('clips', [])):
        return rows
    return [r for r in rows if r['category'] != 'ending']


def recorded_hash(meta, revision):
    return next((r['payload_sha256'] for r in meta.get('pack_revisions', [])
                 if r['revision'] == revision), None)


def verify(folder, rows, lib):
    """Check the tracked voice against the registry and library.json: (problems, report).

    Inventory, IDs, every SHA-256, PCM properties, sample counts, packet framing and counts,
    trim, coverage, the automatic utterances' length, the payload and its signed size, and the
    payload hash recorded for library.json's pack_revision."""
    p = []
    try:
        meta = load_meta(folder)
    except (OSError, ValueError) as e:
        return [f'voice.json: {e}'], {}
    if (meta.get('format'), meta.get('locale'), meta.get('voice_id')) != (
            1, lib['locale'], voice_id(lib)):
        p.append('voice.json is not format 1 of library.json\'s locale and voice')
    ref = meta.get('reference', {})
    try:
        data = (folder / 'reference.wav').read_bytes()
        n, wp = wav_problems(data, 'reference.wav')
        p += wp
        if sha256(data) != ref.get('sha256') or n != ref.get('samples'):
            p.append('reference.wav: SHA-256 or length differs from voice.json')
    except OSError:
        p.append('reference.wav is missing')
    rows = voice_rows(meta, rows)
    by_row = {r['clip_id']: r for r in rows}
    clips = meta.get('clips', [])
    ids = [c['clip_id'] for c in clips]
    if ids != [r['clip_id'] for r in rows]:
        missing = sorted(set(by_row) - set(ids))
        extra = sorted(set(ids) - set(by_row))
        p.append(f'voice.json clips differ from the script: missing {missing}, extra {extra}'
                 if missing or extra else 'voice.json clips are not in script order')
    for sub, ext in (('masters', '.wav'), ('packets', '.opuspkt')):
        have = {f.name for f in (folder / sub).glob('*')} if (folder / sub).is_dir() else set()
        want = {cid + ext for cid in ids}
        if have != want:
            p.append(f'{sub}/: missing {sorted(want - have)}, extra {sorted(have - want)}')
    valid = {}
    for c in clips:
        cid = c['clip_id']
        row = by_row.get(cid)
        if row is None:
            continue
        for k in ('numeric_id', 'category', 'text', 'spoken'):
            if c.get(k) != row[k]:
                p.append(f'{cid}: {k} {c.get(k)!r} in voice.json, {row[k]!r} in the script')
        if c.get('listening_review') not in REVIEWS:
            p.append(f'{cid}: listening_review {c.get("listening_review")!r}')
        if not isinstance(c.get('seed'), int):
            p.append(f'{cid}: no seed')
        cut = c.get('cut')
        if cut is not None and not (isinstance(cut, list) and len(cut) == 2
                                    and all(isinstance(v, int) for v in cut)
                                    and 0 <= cut[0] < cut[1]):
            p.append(f'{cid}: cut {cut!r} is not [first sample, end sample]')
        try:
            m = (folder / 'masters' / f'{cid}.wav').read_bytes()
            pk = (folder / 'packets' / f'{cid}.opuspkt').read_bytes()
        except OSError:
            continue
        n, wp = wav_problems(m, f'masters/{cid}.wav')
        p += wp
        if sha256(m) != c.get('master_sha256'):
            p.append(f'masters/{cid}.wav: SHA-256 differs from voice.json')
        if n != c.get('master_samples'):
            p.append(f'masters/{cid}.wav: {n} samples, voice.json says {c.get("master_samples")}')
        if sha256(pk) != c.get('packets_sha256'):
            p.append(f'packets/{cid}.opuspkt: SHA-256 differs from voice.json')
        try:
            packets = vp.unframe(pk)
        except vp.FormatError as e:
            p.append(f'packets/{cid}.opuspkt: {e}')
            continue
        if len(packets) != c.get('packet_count'):
            p.append(f'{cid}: {len(packets)} packets, voice.json says {c.get("packet_count")}')
        p += [f'{cid}: {e}' for e in vp.check_packets(packets, c.get('pre_skip', -1),
                                                     c.get('valid_samples', 0))]
        # The encoder was given the master as the speaker stage keeps it (its quiet ends
        # trimmed), resampled to 16 kHz: two samples for every three. A master wav_problems()
        # found wrong is reported there, not read here.
        if not wp:
            kept = len(speaker.trimmed(*audio_edit.read_wav(folder / 'masters' / f'{cid}.wav')))
            if abs(3 * c.get('valid_samples', 0) - 2 * kept) >= 3:
                p.append(f'{cid}: {c.get("valid_samples")} valid samples for the {kept} samples '
                         'the speaker stage keeps of its master')
        valid[cid] = c.get('valid_samples', 0)
    part_gap = vp.RATE * lib['pause_ms'] // 1000
    for parts in vp.UTTERANCES:
        if all(x in valid for x in parts):
            total = sum(valid[x] for x in parts) + part_gap * (len(parts) - 1)
            if total > vp.VALID_MAX:
                p.append(f'{" + ".join(parts)}: {total} samples, over {vp.VALID_MAX} (10 s)')
    report = {'clips': len(clips),
              'pending': sum(c.get('listening_review') != 'accepted' for c in clips),
              'pack_revision': lib['pack_revision']}
    report['draft'] = report['pending'] > 0
    if p:
        return p, report
    try:
        payload = payload_from(folder, meta, lib)
        vp.parse_payload(payload)
    except vp.FormatError as e:
        return p + [f'the pack: {e}'], report
    report.update(payload_length=len(payload), payload_sha256=sha256(payload),
                  signed_size=vp.signed_size(len(payload)), limit=vp.FILE_MAX)
    if report['signed_size'] > vp.FILE_MAX:
        p.append(f'the signed pack would be {report["signed_size"]} bytes, over {vp.FILE_MAX}')
    want = recorded_hash(meta, lib['pack_revision'])
    if want is None:
        p.append(f'pack revision {lib["pack_revision"]} has no recorded payload: '
                 '`voice.py export` records it')
    elif want != report['payload_sha256']:
        p.append(f'the payload is not the one recorded for pack revision {lib["pack_revision"]}: '
                 'the audio or its review changed, so raise "pack_revision" in library.json '
                 'and export again (a revision is an immutable identity)')
    return p, report


def describe(report):
    lines = [f"{report.get('clips', 0)} clips, {report.get('pending', 0)} awaiting the "
             f"listening review" + (' (a DRAFT pack)' if report.get('draft') else '')]
    if 'payload_length' in report:
        lines.append(f"pack revision {report['pack_revision']}: payload "
                     f"{report['payload_length']:,} bytes (SHA-256 {report['payload_sha256'][:16]}...), signed "
                     f"{report['signed_size']:,} of {report['limit']:,} bytes "
                     f"({report['limit'] - report['signed_size']:,} left)")
    return '\n'.join(lines)


def check_built_pack(folder, rows, lib, data, signed=True):
    """Problems with a built pack file: its envelope, and its payload against the tracked one."""
    try:
        payload = vp.check_envelope(data, signed=signed)
        vp.parse_payload(payload)
    except vp.FormatError as e:
        return [str(e)]
    problems, _ = verify(folder, rows, lib)
    if problems:
        return ['the tracked voice fails verify'] + problems
    if payload != payload_from(folder, load_meta(folder), lib):
        return ['its payload is not the one the tracked files build']
    return []


def pack(folder, rows, lib, out, key=None, allow_draft=False):
    """Write the pack: signed with `key`, or with key None the bare payload (the simulator's).
    Returns the problems that refused it (nothing is written then)."""
    problems, report = verify(folder, rows, lib)
    if problems:
        return problems
    if report['draft'] and not allow_draft:
        return [f"{report['pending']} clips await the listening review: the pack would "
                'be a DRAFT, which needs --allow-draft (dev panels and the simulator only)']
    payload = payload_from(folder, load_meta(folder), lib)
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if key is None:
        out.write_bytes(payload)
        return []
    sys.path.insert(0, str(HERE.parent))
    import i18n
    try:
        i18n.sign_pack(payload, key, out)
    except RuntimeError as e:
        return [str(e)]
    return check_built_pack(folder, rows, lib, out.read_bytes())


# ---------------------------------------------------------------------------------- export

def trim_level_settings():
    """audio_edit's trim and level defaults, as record.py used them."""
    out = {}
    for fn in (audio_edit.trim, audio_edit.level):
        out[fn.__name__] = {k: v.default for k, v in inspect.signature(fn).parameters.items()
                            if v.default is not inspect.Parameter.empty}
    return out


def pipeline_text(s=None):
    """What encode.py does to a master, in words, from the speaker stage's settings."""
    s = speaker.SETTINGS if s is None else s
    trim = ('its quiet ends trimmed to lead_ms before the first sound and tail_ms after the '
            'last, then ' if s.get('lead_ms') is not None else '')
    level = ('the peaks limited to reach it' if s.get('limit') else
             'the gain capped at peak_db, nothing limited')
    return ('tools/voice/encode.py: the master read as floats, then the speaker stage '
            '(tools/voice/speaker.py for_speaker(), the settings below): ' + trim +
            'raised to target_rms_db of voiced RMS, ' + level + '; FFmpeg\'s resampler to '
            '16 kHz mono s16, its peaks held under peak_db again (encode.ceiling); libopus at '
            'the bitrate below, Ogg Opus; the Ogg framing removed for packets/')


def export(src, dst, rows, lib, input_samples, encoder_env=None, inference_env=None,
           reference_info=None, tool_files=()):
    """Copy one recording (build/voice/library/<locale>/<engine>-<voice>/) into the tracked
    folder `dst`. `input_samples` maps each clip ID to the length its master had after the
    encoder's resampler (the codec environment measures it). Returns the problems that refused
    it; nothing is written then. A clip whose master or packets changed goes back to
    listening_review "pending"; a payload that differs from the one recorded for library.json's
    pack_revision is refused (raise the revision)."""
    p = []
    manifest = json.loads((src / 'manifest.json').read_text(encoding='utf-8'))
    prov = json.loads((src / 'provenance.json').read_text(encoding='utf-8'))
    old = load_meta(dst) if (dst / 'voice.json').exists() else {}
    old_clips = {c['clip_id']: c for c in old.get('clips', [])}
    opus_dir = src / f"opus{lib['kbps']}"
    ref = (src / 'reference.wav').read_bytes()
    ref_n, wp = wav_problems(ref, 'reference.wav')
    p += wp
    files, records = {}, []
    for r in rows:
        cid = r['clip_id']
        m = manifest['clips'].get(cid)
        if m is None or m['text'] != r['text'] or m['spoken'] != r['spoken']:
            p.append(f'{cid}: the recording does not speak the script\'s {r["spoken"]!r}: '
                     'record it again')
            continue
        master = (src / f'{cid}.wav').read_bytes()
        n, wp = wav_problems(master, f'{cid}.wav')
        p += wp
        if cid not in input_samples:
            p.append(f'{cid}: no resampled length')
            continue
        try:
            packets, pre, valid = vp.opus_stream((opus_dir / f'{cid}.opus').read_bytes(),
                                                 input_samples[cid])
        except (OSError, vp.FormatError) as e:
            p.append(f'{opus_dir.name}/{cid}.opus: {e}')
            continue
        framed = vp.frame_packets(packets)
        rec = {'clip_id': cid, 'numeric_id': r['numeric_id'], 'category': r['category'],
               'text': r['text'], 'spoken': r['spoken'], 'seed': m['seed'],
               **({'cut': m['cut']} if 'cut' in m else {}),
               'master_sha256': sha256(master), 'master_samples': n,
               'packets_sha256': sha256(framed), 'pre_skip': pre, 'valid_samples': valid,
               'packet_count': len(packets), 'listening_review': 'pending'}
        was = old_clips.get(cid)
        if was and all(was.get(k) == rec[k] for k in rec if k != 'listening_review'):
            rec['listening_review'] = was['listening_review']
        records.append(rec)
        files[f'masters/{cid}.wav'] = master
        files[f'packets/{cid}.opuspkt'] = framed
    if p:
        return p
    meta = {'format': 1, 'locale': lib['locale'], 'voice_id': voice_id(lib),
            'master_rate': MASTER_RATE, 'packet_rate': vp.RATE,
            'reference': {'file': 'reference.wav', 'sha256': sha256(ref), 'samples': ref_n},
            'pack_revisions': list(old.get('pack_revisions', [])), 'clips': records}
    # The payload as `pack` will build it, from the bytes about to be written.
    try:
        payload = vp.build_payload(
            [dict(numeric_id=c['numeric_id'], category=c['category'], text=c['text'],
                  key=c['clip_id'], data=files[f"packets/{c['clip_id']}.opuspkt"],
                  valid_samples=c['valid_samples'], packet_count=c['packet_count'],
                  pre_skip=c['pre_skip']) for c in records],
            revision=lib['pack_revision'], draft=is_draft(meta), locale=lib['locale'],
            voice_id=voice_id(lib), word_gap_ms=lib['gap_ms'], part_gap_ms=lib['pause_ms'],
            nominal_bitrate=lib['kbps'] * 1000)
    except vp.FormatError as e:
        return [f'the pack: {e}']
    have = recorded_hash(meta, lib['pack_revision'])
    if have is None:
        meta['pack_revisions'].append({'revision': lib['pack_revision'],
                                       'payload_sha256': sha256(payload)})
    elif have != sha256(payload):
        return [f'pack revision {lib["pack_revision"]} was recorded with another payload: the '
                'audio or its review changed, so raise "pack_revision" in library.json']
    provenance = {
        'voice': manifest['voice'],
        'reference': {'file': 'reference.wav', 'sha256': sha256(ref), 'samples': ref_n,
                      **(reference_info or {})},
        'generation': {'engine': prov['engine'], 'models': prov['models'],
                       'packages': prov['packages'], 'gpu': prov['gpu'], 'made': prov['made'],
                       'settings': {k: lib[k] for k in ('token_text', 'seed', 'max_takes',
                                                        'token_seconds', 'phrase_seconds',
                                                        'rounds', 'say', 'seeds', 'carriers',
                                                        'carrier_cut') if k in lib},
                       **({'aligner': prov['aligner']} if prov.get('aligner') else {}),
                       'trim_level': trim_level_settings(),
                       'environment_at_export': inference_env},
        'encoder': {'pipeline': pipeline_text(),
                    'speaker': speaker.SETTINGS,
                    'bitrate': lib['kbps'] * 1000, 'sample_rate': vp.RATE,
                    'frame_ms': vp.FRAME_MS, **(encoder_env or {})},
        'pack': {'gap_ms': lib['gap_ms'], 'pause_ms': lib['pause_ms']},
        'files_at_export': {f: sha256((REPO / f).read_bytes()) for f in tool_files},
    }
    for sub, ext in (('masters', '.wav'), ('packets', '.opuspkt')):
        (dst / sub).mkdir(parents=True, exist_ok=True)
        for f in (dst / sub).glob('*'):
            if f'{sub}/{f.name}' not in files:
                f.unlink()
    for name, data in files.items():
        if not (dst / name).exists() or (dst / name).read_bytes() != data:
            (dst / name).write_bytes(data)
    (dst / 'reference.wav').write_bytes(ref)
    (dst / 'voice.json').write_bytes(json_bytes(meta))
    (dst / 'provenance.json').write_bytes(json_bytes(provenance))
    (dst / 'NOTICE.md').write_bytes(NOTICE.format(locale=lib['locale'],
                                                  voice_id=voice_id(lib)).encode('utf-8'))
    return []


def copy_reference(folder, to):
    """The reference alone, for an engine to clone from without touching the tracked folder."""
    to.mkdir(parents=True, exist_ok=True)
    shutil.copy2(folder / 'reference.wav', to / 'reference.wav')
