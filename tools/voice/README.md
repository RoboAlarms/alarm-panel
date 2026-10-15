# Voice auditions

Scripts that make listening samples of the spoken words with local text-to-speech models, so
a voice can be chosen (docs/features/08 [voice-library.md](../../docs/features/08-Audio-and-Annunciation/voice-library.md),
A0, BL-015). Speech is made on a computer, never on the panel (ADR-052).

Only these scripts are in git. The Python environments, the models and the audio are not:

| What | Where |
|---|---|
| uv and one Python 3.12 environment per engine, the checker and the encoder | `build/voice/` (git-ignored) |
| The models (several GB each) | the Hugging Face cache, `~/.cache/huggingface` |
| The samples | `build/voice/samples/<engine>/<voice>/` |

Needs an NVIDIA GPU with CUDA (16 GB runs every engine) and any Python 3.10+ to start the
driver, which uses the standard library only. On Windows the ESP-IDF Python will do: it only
starts the scripts and gets no packages.

```
python tools/voice/voice.py setup              # once: uv, Python 3.12, PyTorch, each engine
python tools/voice/voice.py audition           # every engine; or name some: audition kokoro
python tools/voice/voice.py check              # Whisper transcribes every sample
```

[audition.json](audition.json) holds the words, the labels joined from them, and the voices of
each engine: Kokoro-82M (presets), Qwen3-TTS 1.7B (a designed voice cloned for every clip, and
the Aiden preset) and VoxCPM2 (designed voices, cloned the same way). A designed voice is spoken
once to `reference.wav` and every clip is made from that reference, so all clips share one
voice; delete the reference to design it again.

Each voice's folder has `words/` (each word alone, trimmed and levelled), `joined/` (the labels
made from those words, as the panel would play them), `whole/` (the labels spoken in one go, to
compare), `reel.wav` (all of it in that order) and `manifest.json` (seeds, lengths, rejected
takes). `provenance.json` beside the voices records the model revisions and package versions.

A take that comes out silent or far too long is retried with the next seed; `check` then flags
any clip a recognizer doesn't hear as written. Neither replaces listening.

## The library

```
python tools/voice/voice.py record             # the whole recording script, in the chosen voice
```

[library.json](library.json) names the chosen voice (Qwen3-TTS `designed_female`, picked by ear
on 2026-10-08) and points at the script,
[recording-script.csv](../../docs/features/08-Audio-and-Annunciation/recording-script.csv).
`record` copies that voice's `reference.wav` from its audition folder (it refuses to design a
new one, which would sound different) and writes `build/voice/library/en-US/qwen3-designed_female/`:
one `<clip_id>.wav` per row, `extra/` (words outside the script, for trying splits such as
"Living" + "Room") and `manifest.json`. With `--reels` it also joins the clips into
`reel.wav` (every row in order), `examples.wav` (status messages and sensor names, as the panel
would join them) and `compare.wav` (each pair in library.json: spoken whole, then joined from
its parts). Whisper then checks every clip and the misheard
ones are spoken again with fresh seeds, up to three rounds; words that sound alike ("Main" and
"Maine") count as heard. Run again, `record` speaks only the clips that are new or whose text
changed, and removes the ones the script dropped; `--again` speaks everything.

**From revision 7** every word is recorded alone, as written, and its original take goes into
the pack (decided 2026-10-10: the clearest, and the word forms took too much room): library.json
has no `"carriers"`, `"endings"`, `"form_takes"` or `"carrier_cut"`, so `record` lists no `.end`
clips and `record --recut` has nothing to cut again. The carriers and the word forms below are how
revisions 4-6 were made, the BL-202 joins research: `voice.py forms` and `bench-takes` need those
keys back (revision 6's library.json, in git history) and stop with a message without them.

**The carriers** (`carrier.py`). Spoken alone ("Front."), a word ends the way a sentence does,
and a name joined from such words sounds like a list. So each word is spoken inside a carrier
sentence from library.json's `"carriers"` - a template for its category, a short name (`"{}
door."` for a position, `"Back {} door."` for a place, `"Back {} two."` for an object, `"Zone {}
door."` for a number) - or a sentence for one clip (`"Armed away."` for status.away) - and cut
out; a word's ending is cut from library.json's `"endings"` ("Word forms" below). torchaudio's
MMS_FA aligner (its 1.2 GB checkpoint downloads to the torch hub cache the first time, never into
the repository) finds each word's start and end, roughly: a held sound at a word's end ("Low",
"Den") goes on 50-70 ms past it, and a vowel or an "h" begins before it. So the cut is made at
the quiet point between two words (within 6 dB of the quietest within 250 ms, at least 35 dB
under the word's loudest, from one aligner frame inside the word, never past the neighbour's
start); the words after the target start with a stop ("door", "two") whose closure is that
quiet point. A word with no quiet point before it (a quiet "f" straight after "Front")
starts where the previous word ends. Where a vowel or an "h" follows ("Armed home"), the words
run together, so the word ends 10 ms after the aligner's end, and one that starts with a vowel
or an "h" starts where the previous word ends (`"carrier_cut"`). `record` keeps each whole
carrier take in `carriers/`, and `record --recut` cuts them again without the model. The
manifest and voice.json keep each clip's `spoken` carrier and its `cut` (the first and end
sample of the take), so `record --reproduce` repeats the cut without the aligner. Sentences
("You may exit now.") have no carrier and are spoken as written; `takes` cuts its takes the
same way.

**Word forms** (`forms.py`, AUD-031). Every word has two clips: its own (`word.door`), cut
from a short name as above, for the words before a name's last, and its ending
(`word.door.end`, ID 0xC000 + the word's), cut from the end of library.json's `"endings"`
sentence ("Not ready. Check the zones. Back deck door."), for the last word of a name. `record`
lists both (`record.endings()`), `tracked` and `voicepack` carry the endings (vocabulary schema
2), and the panel plays a name's last word as its ending. `voice.py forms` speaks every carrier
sentence with library.json's `"form_takes"` seeds (8) in the engine's environment, checks every
cut with Whisper (`asr_check.py`), and picks into the recording the take Whisper hears as
written that starts cleanly (its first 5 ms at least 35 dB under its loudest) and whose length
is nearest its target (a share of the word's length in the original recording, `--alone`).
Then `pick` fits the pack's room: when the chosen takes would make a payload over the limit
less 8 KiB (`--budget`; about 3,920 bytes a second, measured), it swaps in shorter takes that
Whisper heard as well and that start as cleanly, each step the one that adds the least cost for
the bytes it saves (the cost: how far a take falls under its target's middle, as a share of it,
squared), and refuses, writing nothing, when even the shortest don't fit. It prints how many
clips it shortened and by how much (voice-library.md "Word forms"). `forms.py speak --part I/N`
lets two processes share the GPU (about 11 takes a minute on a 16 GB card), each with its own
share of the takes; a run that stops carries on where it was (the manifest is written after every
take), and since it speaks one take of every sentence before the next, a run stopped early leaves
every word as many takes. `--again` speaks everything again, or with `--only` just those clips,
the other takes kept. `pick` keeps `<out>/report.json`: each clip's takes, their lengths, onsets
and whether Whisper heard them; a take Whisper hasn't checked ranks after one it heard as written.

```
python tools/voice/voice.py forms              # speak, check and pick: the word forms
python tools/voice/voice.py setup codec        # once: PyAV (FFmpeg with libopus), no PyTorch
python tools/voice/voice.py encode             # library.json's codec: Opus 32 kbps; or --kbps 24
python tools/voice/voice.py encode --codec lc3 --kbps 32   # LC3, into lc3_32/
```

For LC3, `voice.py` downloads Google's liblc3 release (pinned, its SHA-256 checked) into
`build/voice/liblc3-<version>/` and builds it with gcc from PATH or CLion's MinGW, the
toolchain the host tests use. Media players don't play LC3, so `lc3_<kbps>/` holds each clip
decoded again as a `.wav`; the bytes counted are the fixed-size frames.

`encode` levels every clip for the panel (the speaker stage), resamples it to 16 kHz mono and
encodes it with libopus into `opus<kbps>/` beside the clips: one `.opus` file per clip (VLC and other players open them), with `--reels`
the three reels as `.opus` (joined from the recorded clips, then encoded once at the same
bitrate), and `sizes.json` (the bytes the script's clips take as raw packets with a 2-byte length each,
against the 950 KiB budget). Files of clips the script has dropped, and reels not asked for, are
removed, so the folder holds only what the last run made.

**The speaker stage** (`speaker.py`). The masters stay as recorded, at -20 dB of voiced RMS;
spoken at that level the announcements were about 10 dB under the chime at the same volume. Before
the codec (pack revision 7, 2026-10-10), each master's quiet ends are trimmed to 10 ms before its
first sound and 20 ms after its last (`speaker.trimmed()`; the masters keep 30 and 80, and the
panel's word gap makes the pause), and it is raised to -15 dB of voiced RMS with the gain capped so
its peak stays under -1 dBFS, nothing limited: a word with sharp peaks stops short of -15. The
panel makes speech louder itself with its Voice volume (docs/features/08 AUD-033). Revisions 3-5
were also at -15 dB, capped; revision 6 raised each master to -6 dB and limited its peaks, again
and again ("Louder -6, Limited", 2026-10-09), which lifted a word's lead-in and tail, a breath and
the start of an "f" or an "s" about 15 dB more than the voice. After resampling to 16 kHz the
encoder holds the ceiling again (`encode.ceiling()`: the resampler overshoots it by up to 0.4 dB),
and Opus decodes speech at most about 0.6 dB over it (measured 2026-10-09). Pack revision 2 (a
compressor and limiter to -8 dB) brought the quiet sounds up 9-15 dB and a hiss was heard on
the panel, so listen for it in a new pack. `speaker.SETTINGS` goes into
provenance.json; a change to it changes the packets, so it needs a new `pack_revision`.

## A clip that sounds off

```
python tools/voice/voice.py takes word.dog                 # three other takes of it
python tools/voice/voice.py takes word.coop --say Koop.    # and of a respelling
```

Each take is one random draw of the model (it samples its sound tokens, temperature 0.9), so
the same word with another seed can sound quite different: a short word has few tokens, and
one odd draw changes all of it. `takes` writes `takes/<clip id>/` beside the clips:
`0-current.opus` (the library's take) and the new ones, named with their spelling and seed. The
same text and seed give the same take again, so pin the one chosen in
[library.json](library.json): its seed under `"seeds"` (`"word.dog": 1334`) and a respelling
under `"say"`. `record` then speaks that clip again with them; Whisper's check still runs.

## Trying joins on a panel

```
python tools/voice/voice.py try "Front Door" "Back Patio Motion" --takes original --gap 60 --device <panel>
```

`try` joins each phrase from a recording's takes as the panel joins a name (`bench.py`: the
script's words, the longest match first, at the pack's level, resampled to 16 kHz; `--gap` ms
between a phrase's words, `--between` ms between the phrases) into `build/voice/bench/`, and with
`--device` plays it on a development panel's speaker (`panelctl voice play`, docs/features/08
AUD-030): no pack, no install, no approval. `--takes cut` is the recording as it is now,
`--takes original` the words spoken alone (the recording before the carriers, kept as
`<voice>.r3-backup`), or a folder. The words are 60 ms apart, the pack's gap (library.json
`gap_ms`), unless `--gap` says otherwise. Without `--level` they are at the pack's level (the
speaker stage: the quiet ends trimmed, -15 dB of voiced RMS with the gain capped at a -1 dBFS peak); `--level` sets another voiced RMS in dB (the masters' is
-20) with the gain capped at that -1 dBFS peak, so a word with sharp peaks stops short of a loud
level, and `--limit` brings it near the level by limiting its peaks, as revision 6 did.
`--send-db` moves the finished audio up or down: a development panel from 0.8.13 makes speech up
to 9 dB louder with its Voice volume (docs/features/08 AUD-033; from 0.8.15 the pack's level at
70 %, +9 dB at 100 %), so revision 6's "Louder -6, Limited" sent 9 dB down plays there at 100 % as
revision 6 did on the panels before. A word the
script lacks comes from `build/voice/bench/words/<word>.wav`, spoken for the bench only. The codec is left out: Opus at 32 kbps changes neither the timing nor the words.

`python tools/voice/voice.py bench-takes [--seeds 8]` makes the joins research's trials
(`bench_takes.py`) for five test names (Back door, Front door, Garage door, Kitchen window, Back
patio motion) in the original takes' voice: each name spoken whole, alone and at the end of "Not
ready. Check the zones.", and each word in the form its place asks for (the last word cut from the
end of "Not ready. Check the zones. Back deck motion.", long and falling, after the "k" of deck,
whose closure leaves a clean place to cut; the words before it from the name spoken alone). A word
cut where the one before runs into it ("patio motion") starts mid-sound and plays choppy, so a take
that starts mid-sound is picked only when no take starts cleanly. Whisper checks every take and,
of the clean ones it hears as written, the one nearest the research's length targets is picked, into
`build/voice/bench/names/` and `forms/` (`takes/report.json` lists them all). `try --names alone`
(or `leadin`) plays a whole name where a phrase ends with one; `try --forms` uses the forms.
`try --enhance light` (or `strong`) tries a low-volume clarity stage (`bench.py` `enhance()`: a
high-pass, a gentle lift of the soft sounds, a presence lift at 3 kHz, then the same level and the
peaks limited); the panel no longer has one (AUD-033).

`python tools/voice/bench_server.py --device <panel>` is the voice bench, a page on this computer
(http://127.0.0.1:8546/, `bench_page.html`) where everything heard is made on the panel, as it
makes a zone's name: the page sends only the words' clip IDs and the pause between them
(`panelctl voice say`), and the panel takes each word from its own installed pack, joins them and
plays them at its own Voice volume. The page shows the panel's firmware, pack, Voice volume, pause
and announcements (and the speaker, when the firmware has the bench silence switch, AUD-025); sets the Voice volume (`panelctl voice volume`, saved on the panel as
its Settings slider saves it); suggests the pack's words as you type and refuses one it lacks; has
zone names to click, all of them in turn, a repeat every few seconds (to listen from another
room), and a list of what was sent and what the panel did. It needs a development build from
0.8.14 (`say` from 0.8.13).

## The tracked voice

The chosen takes are build inputs (AUD-023, ADR-052; voice-first-build.md D13), so they are in
git under [assets/voice/en-US/qwen3-designed_female/](../../assets/voice/README.md):

| File | What it is |
|---|---|
| `reference.wav` | the designed voice every clip clones |
| `masters/<clip_id>.wav` | the 237 script clips exactly as recorded: PCM16 mono 24 kHz |
| `packets/<clip_id>.opuspkt` | the clip's Opus packets as the pack stores them: each a u16 little-endian length and its bytes, no Ogg |
| `voice.json` | per clip: numeric ID, category, text, spoken text, seed, master SHA-256 and samples, packets SHA-256, pre-skip, valid samples and packets at 16 kHz, `listening_review`; and the payload SHA-256 of each pack revision |
| `provenance.json` | the model revisions, packages, GPU and settings that made the audio, the reference's making, and the encoder (PyAV, FFmpeg, libopus) |
| `NOTICE.md` | a synthetic voice, and the terms it is shared under |

The numeric IDs (D07) are the `numeric_id` column of
[recording-script.csv](../../docs/features/08-Audio-and-Annunciation/recording-script.csv),
written as `0x` and four hex digits: tokens `0x0001`..`0x00DD` in row order, the status clips
`0x8001`..`0x800A` and the walk-test clips `0x8101`..`0x8106`. An ID is never renumbered or
reused: a retired word keeps its row. A test pins today's mapping.

```
python tools/voice/voice.py export             # the recording in build/voice/library/ -> assets/
python tools/voice/voice.py verify             # every file, hash, ID and length; the pack's size
python tools/voice/voice.py pack --out build/voice/en-US.robovoice --key <the OTA signing key>
python tools/voice/voice.py pack --out build/voice/en-US-draft.rvoc --unsigned --allow-draft
python tools/voice/voice.py verify --pack build/voice/en-US.robovoice
python tools/voice/voice.py encode --reproduce
python tools/voice/voice.py record --reproduce --only word.front word.dog
```

`export` (needs the `codec` and engine environments) copies the masters, takes the packets out
of `opus<kbps>/*.opus` and checks D08's rules on every clip: single-frame 20 ms packets of
1-1275 bytes, the pre-skip and samples whole at 16 kHz, enough packets for both and less than
one packet more. The valid samples are the length of the master as the speaker stage trims it
(`speaker.trimmed()`), after the encoder's resampler, which
the codec environment measures; the Ogg file's final granule must agree with it exactly
(FFmpeg's Ogg muxer writes one pre-skip more than RFC 7845 for libopus: both forms are
accepted, nothing looser). A clip whose master or packets changed goes back to
`listening_review: "pending"`. Export also records the payload's SHA-256 for library.json's
`pack_revision`, and refuses when that revision already has another one.

`verify` and `pack` use only the standard library: no model, no encoder, no build folder.
`pack` builds RVOC format 1 (voice-first-build.md D08: header, index sorted by ID, strings,
4-byte aligned clips) the same, byte for byte, on any clone, and refuses:
- anything `verify` finds;
- a DRAFT (any clip not `accepted` in the listening review) without `--allow-draft`, which is for dev
  panels and the simulator; release tooling refuses a DRAFT;
- a payload that is not the one recorded for library.json's `pack_revision`: a revision is an
  immutable identity on the panel, so changed audio, or an acceptance (it clears the
  DRAFT flag), needs a new revision: raise `pack_revision` and run `export`.

A take is accepted by setting its `"listening_review"` to `"accepted"` in `voice.json`
after listening to it (and to representative joins); `export` keeps that only while the take's
master and packets stay the same.

`--key` signs as language packs are signed (`espsecure sign_data --version 2`, i18n.py's
`sign_pack`): 0xFF padding to 4096 bytes, then one signature sector, at most 970752 bytes in
all. `--unsigned` writes the bare payload for the PC simulator; the firmware never accepts it.
`verify --pack` checks a signed file's envelope and that its payload is the one the tracked
files build.

What "the same audio" promises: the tracked masters and packets are authoritative. Any clone
rebuilds the identical payload with `verify` and `pack`; re-signing makes a new envelope around
it. `encode --reproduce` re-encodes the masters with the pinned PyAV pipeline and must give the
tracked packets, pre-skip and samples; that needs the encoder `provenance.json` records.
`record --reproduce` speaks clips again with their recorded seeds (no retakes) and must give
the masters' PCM; that needs the recorded GPU, CUDA, driver and packages, and Qwen3-TTS gives no
promise of the same waveform on other hardware. Either one reports the first difference and
fails; neither ever replaces tracked audio. New takes come only through `record`, listening and
`export`.
