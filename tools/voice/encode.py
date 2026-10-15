"""Compress the library as the panel would store it (Opus or LC3), and measure it.

Runs inside the `codec` environment (voice.py encode), which has PyAV: FFmpeg with libopus.
LC3 uses Google's liblc3, built by `voice.py setup lc3` and passed in as --lc3-lib.
Every clip is levelled for the panel (speaker.py), resampled to 16 kHz mono and encoded at
--kbps; for Opus, under <voice>/opus<kbps>/
  <clip files>.opus  each clip in Ogg, which media players such as VLC play (the container is
                     only for listening: the pack stores the raw packets, so the bytes counted
                     are the packets plus a length for each)
  reel/examples/compare.opus  with --reels: the reels to listen to, joined from the recorded
                     clips and encoded once at the same bitrate
  sizes.json         the bytes the script's clips need, against the pack's 950 KiB budget
For LC3, under <voice>/lc3_<kbps>/: each clip decoded again as <clip>.wav (media players don't
play LC3), the reels likewise with --reels, and sizes.json. LC3's frames are all one size, so
the pack needs no length per frame: the bytes counted are the frames.
With --takes <clip id>, it encodes the takes voice.py takes spoke into <voice>/takes/<clip id>/
instead (Opus only), beside 0-current.opus, the clip as the library has it.
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio_edit as ae  # noqa: E402
import record  # noqa: E402
import speaker  # noqa: E402

RATE = 16000                 # the first comparison decodes at 16 kHz (voice-library.md)
LENGTH_BYTES = 2             # each packet's length in the pack
BUDGET = 950 * 1024          # one copy of the supplied pack (audio-storage.md)
HEADROOM = 64 * 1024         # pack index, notices and headers (voice-library.md)


def resample(x, src, dst):
    """Floats at src Hz to an int16 numpy array at dst Hz, through FFmpeg's resampler."""
    import av
    import numpy as np
    pcm = np.clip(np.round(np.asarray(x) * 32767), -32768, 32767).astype(np.int16)
    frame = av.AudioFrame.from_ndarray(pcm.reshape(1, -1), format='s16', layout='mono')
    frame.sample_rate = src
    rs = av.AudioResampler(format='s16', layout='mono', rate=dst)
    parts = [f.to_ndarray().reshape(-1) for f in rs.resample(frame) + rs.resample(None)]
    return np.concatenate(parts) if parts else np.zeros(0, np.int16)


def ceiling(pcm):
    """16 kHz int16 samples (any sequence) as a list, their peaks held under the speaker stage's
    ceiling again at RATE: the resampler that brings a levelled master down to RATE overshoots it
    (up to about 0.4 dB on limited speech, measured 2026-10-09). Samples under it come back as
    they were."""
    y = speaker.limit([int(v) / 32767.0 for v in pcm], RATE, speaker.SETTINGS['peak_db'])
    return [max(-32768, min(32767, round(v * 32767))) for v in y]


def for_pack(x, rate):
    """What the pack stores, before the codec: the master as the speaker stage leaves it (its
    quiet ends trimmed, at its level), resampled to RATE (int16) and its peaks held under the
    stage's ceiling there (ceiling(), which keeps the length)."""
    import numpy as np
    return np.asarray(ceiling(resample(speaker.for_speaker(x, rate), rate, RATE)), np.int16)


def encode(x, rate, path, kbps):
    """Encode one master to Ogg Opus; returns (packet bytes, packet count)."""
    import av
    pcm = for_pack(x, rate)
    with av.open(str(path), 'w', format='ogg') as out:
        st = out.add_stream('libopus', rate=RATE, layout='mono')
        st.bit_rate = kbps * 1000
        n = st.codec_context.frame_size or RATE // 50
        fifo = av.AudioFifo()
        frame = av.AudioFrame.from_ndarray(pcm.reshape(1, -1), format='s16', layout='mono')
        frame.sample_rate = RATE
        fifo.write(frame)
        while fifo.samples:
            out.mux(st.encode(fifo.read(min(n, fifo.samples))))
        out.mux(st.encode(None))
    with av.open(str(path)) as inp:
        sizes = [p.size for p in inp.demux(audio=0) if p.size]
    return sum(sizes), len(sizes)


class Lc3:
    """liblc3 through ctypes: 10 ms frames at RATE, a fixed number of bytes each."""
    S16 = 0       # enum lc3_pcm_format: LC3_PCM_FORMAT_S16
    DT_US = 10000

    def __init__(self, path, kbps):
        import ctypes as C
        lib = C.CDLL(str(path))
        lib.lc3_setup_encoder.restype = lib.lc3_setup_decoder.restype = C.c_void_p
        lib.lc3_setup_encoder.argtypes = lib.lc3_setup_decoder.argtypes = [C.c_int, C.c_int,
                                                                         C.c_int, C.c_void_p]
        lib.lc3_encode.argtypes = [C.c_void_p, C.c_int, C.c_void_p, C.c_int, C.c_int, C.c_void_p]
        lib.lc3_decode.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_void_p, C.c_int]
        self.C, self.lib = C, lib
        self.n = lib.lc3_frame_samples(self.DT_US, RATE)
        self.nbytes = lib.lc3_frame_bytes(self.DT_US, kbps * 1000)
        self.delay = lib.lc3_delay_samples(self.DT_US, RATE)

    def round_trip(self, pcm):
        """Encode int16 samples and decode them again; returns (frames, decoded int16 samples)
        lined up with the input (the codec's delay removed)."""
        import numpy as np
        C, lib, n = self.C, self.lib, self.n
        emem = C.create_string_buffer(lib.lc3_encoder_size(self.DT_US, RATE))
        dmem = C.create_string_buffer(lib.lc3_decoder_size(self.DT_US, RATE))
        enc = lib.lc3_setup_encoder(self.DT_US, RATE, RATE, emem)
        dec = lib.lc3_setup_decoder(self.DT_US, RATE, RATE, dmem)
        frames = (len(pcm) + self.delay + n - 1) // n
        x = np.zeros(frames * n, np.int16)
        x[:len(pcm)] = pcm
        y = np.zeros(frames * n, np.int16)
        buf = C.create_string_buffer(self.nbytes)
        for i in range(frames):
            if lib.lc3_encode(enc, self.S16, x[i * n:].ctypes.data, 1, self.nbytes, buf):
                raise RuntimeError('lc3_encode failed')
            if lib.lc3_decode(dec, buf, self.nbytes, self.S16, y[i * n:].ctypes.data, 1) < 0:
                raise RuntimeError('lc3_decode failed')
        return frames, y[self.delay:self.delay + len(pcm)]


def main():
    a = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    a.add_argument('voice', type=Path, help='one voice of the library')
    a.add_argument('--codec', choices=('opus', 'lc3'), default='opus')
    a.add_argument('--kbps', type=int, default=24)
    a.add_argument('--lc3-lib', type=Path, help="liblc3's shared library")
    a.add_argument('--reels', action='store_true', help='also encode the reels to listen to')
    a.add_argument('--takes', metavar='CLIP_ID', help="encode one clip's takes to choose from")
    args = a.parse_args()
    lib = json.loads((HERE / 'library.json').read_text(encoding='utf-8'))
    rows = record.clips_wanted(lib)
    if args.takes:
        folder = args.voice / 'takes' / args.takes
        row = next(r for r in rows if r['clip_id'] == args.takes)
        x, rate = ae.read_wav(args.voice / row['file'])
        encode(x, rate, folder / '0-current.opus', args.kbps)
        for wav in sorted(folder.glob('*.wav')):
            x, rate = ae.read_wav(wav)
            encode(x, rate, wav.with_suffix('.opus'), args.kbps)
            wav.unlink()  # the same text and seed speak it again
        print(f'{len(list(folder.glob("*.opus")))} takes to choose from -> {folder}')
        return
    opus = args.codec == 'opus'
    out = args.voice / (f'opus{args.kbps}' if opus else f'lc3_{args.kbps}')
    (out / 'extra').mkdir(parents=True, exist_ok=True)
    clips, rate = record.read_clips(args.voice, rows)
    lc3 = None if opus else Lc3(args.lc3_lib, args.kbps)
    suffix = '.opus' if opus else '.wav'
    written = set()

    def coded(x, path):
        """Encode one clip (or reel) to path; returns (stored bytes, packets or frames)."""
        path = path.with_suffix(suffix)
        written.add(path)
        if opus:
            payload, packets = encode(x, rate, path, args.kbps)
            return payload + LENGTH_BYTES * packets, packets
        frames, y = lc3.round_trip(for_pack(x, rate))
        ae.write_wav(path, (y / 32768.0).tolist(), RATE)
        return frames * lc3.nbytes, frames

    sizes, total, seconds = {}, 0, 0.0
    for r in rows:
        x = clips[r['clip_id']]
        stored, count = coded(x, out / r['file'])
        if r['category'] != 'extra':
            sizes[r['clip_id']] = {'seconds': round(len(x) / rate, 3), 'frames': count,
                                   'stored': stored}
            total += stored
            seconds += len(x) / rate
    if args.reels:
        for name, x in record.listening(rows, clips, lib, rate).items():
            coded(x, out / name)
    for old in out.rglob('*' + suffix):  # clips the script dropped, reels not asked for
        if old not in written:
            old.unlink()
    name = (f'Opus {args.kbps} kbps, mono {RATE} Hz, 20 ms frames' if opus else
            f'LC3 {args.kbps} kbps, mono {RATE} Hz, 10 ms frames of {lc3.nbytes} bytes')
    summary = {'codec': name,
               'clips': len(sizes), 'seconds': round(seconds, 2), 'stored_bytes': total,
               'budget_bytes': BUDGET, 'headroom_bytes': HEADROOM,
               'left_bytes': BUDGET - HEADROOM - total, 'per_clip': sizes}
    (out / 'sizes.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(f"{name}: {len(sizes)} clips, {seconds:.1f} s, {total:,} bytes "
          f"({total / 1024:.0f} KiB) stored; {summary['left_bytes'] / 1024:.0f} KiB of the "
          f"{BUDGET // 1024} KiB budget left after {HEADROOM // 1024} KiB headroom -> {out}")


if __name__ == '__main__':
    main()
