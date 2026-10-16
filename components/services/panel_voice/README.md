# panel_voice

**Tier:** services (ESP-IDF) · **Features:** [08 Audio and annunciation](../../../docs/features/08-Audio-and-Annunciation/README.md)
([voice, first build](../../../docs/features/08-Audio-and-Annunciation/voice-first-build.md)) ·
**Tests:** [`test/`](test/) on the PC (AUD-028)

The panel's voice: spoken zone names from a signed voice pack (BL-202). The service
(`panel_voice.h`) keeps the installed pack ("primary", kind 2 of the shared assets store,
`pack_assets`) as one immutable PSRAM copy with leases, decodes on one worker and mixes speech
into the sounder's output; the Opus adapter `voice_codec` and the renderer `voice_render` are
plain C11 shared with the PC builds. Option: `ALARM_FEATURE_VOICE` (default on with PSRAM); off,
the service answers with stubs (status `off`, installs `unavailable`).

## Public API
| Header | What it offers |
|---|---|
| `panel_voice.h` | `panel_voice_set_source` (a link keypad's), `_start`, `_status`, `_ready`; the preferences `_set_prefs`, `_set_volume`, `_set_power_saving`; the engine's facts `_engine_event/_sound/_output/_snapshot` (panel_app's sounder listener, or a link keypad's link task) and a source producer's `_engine_baseline/_pause`; leases `_pack_acquire/_release`; `_preview`, `_preview_stop`; `_bench_play`, `_bench_say` (dev builds); `_install`, `_install_name`; `_console_register` |
| `voice_render.h` | the utterance renderer: `vr_begin`, `vr_pull`, `vr_total_samples_32k`, and the upsampler `vr_up_*` |
| `voice_codec.h` | `vc_state_bytes`, `vc_init`, `vc_reset`, `vc_packet_check`, `vc_decode_20ms`, and the clip trim `vc_trim_begin` / `vc_trim_take` / `vc_trim_done` |

`voice_codec.c` is plain C11 over libopus, with no ESP-IDF calls, no heap and no globals: the
host tests (and later the simulator and the preview) compile the same file. The caller owns the
decoder state (`vc_state_bytes()` = `opus_decoder_get_size(1)`, refused above 64 KiB; on the
panel in PSRAM) and every buffer. Each clip is decoded from a reset state; the trim drops the
clip's pre-skip, passes exactly its valid samples and drops the final padding (D08). Every
packet must describe exactly 320 samples (20 ms at 16 kHz), checked from its table of contents
before libopus decodes it; a refused packet cancels the clip, the decoder is reset before the
next one.

## The service
- **Start-up** (`panel_voice_start`, in panel_app after the sounder, the engine and the stored
  preferences): a short-lived task with an internal-RAM stack reads "primary" through the broker
  into PSRAM, checks its signature (`pa_verify`) and the whole pack (`vp_open_signed`), then
  allocates the decoder state and the worker (D10) and marks the voice ready. Any failure leaves
  the state `unavailable` with a reason and the tones untouched; boot carries on.
- **The worker** (priority 4, 256 KiB PSRAM stack for libopus's alloca; never touches flash or
  NVS): `ann_next` -> `vr_begin` -> `vr_pull` 640 samples at a time -> the Voice volume
  (`components/core/sounder` `speech_boost`, AUD-033: up to +9 dB with a limiter, set at the
  utterance's start; its 63 held samples follow the last pull) -> `sounder_speech_write`
  while `ann_current` holds and the ring has room -> `sounder_speech_finish`, then `ann_done`
  once the sounder reports it played (or rejected, underran, or 15 s passed). It drains the tone
  tickets (`ann_ticket`), tells the sounder each round whether speech is due (`ann_due` ->
  `sounder_speech_due`: the exit beeps wait for the words, AUD-034), ends the bell test wait 2 s after the pulse, and re-reads the zones'
  spoken names when the configuration's generation changes (until then a zone has no name: the
  policy is never fed stale words). It waits on a notification (10 ms while speaking, 20 ms while
  anything waits).
- **The engine's facts** come from panel_app's existing sounder listener (the engine's
  listeners are full): one-shots play with `sounder_play_tracked` and their tickets, events,
  outputs and every snapshot (as the policy's state, with the engine's current `exit_sounds`) go
  into a preallocated queue with a zero-tick send - no lock, never an engine call - and the worker
  applies them in order. The silent baseline is taken on the alarm task by `panel_voice_start`
  before the listeners copy anything; a full queue silences the voice until the next snapshot.
- **On a link keypad** (docs/features/11 LINK-028): no engine, so `panel_app` sets a source
  before the start (`panel_voice_set_source`): a baseline is asked of the link
  (`plink_service_voice_resync`), the zones' words come from the master's configuration copy
  (`plink_service_keypad_config`, with its generation: a copy older than the snapshots' never
  speaks), an install answers locked (no install path reaches a keypad until BL-205), and the D07
  settling check is skipped (the master's isn't on the wire). The link task is the one producer:
  `plink_vfeed` decides what it hands on, `panel_voice_engine_baseline(home, snap)` makes the next
  item the silent baseline of that partition (`ann_set_keypad` when the home moved) and
  `panel_voice_engine_pause()` stops speech and collects nothing until the next one. The keypad's
  own Announcements switches and Voice volume apply, as on any panel.
- **Development builds** log one line per utterance as it starts, `says <kind>[, zone <n>]
  (partition <p>, Voice <v> %)`, and `voice` names the partition it speaks for: what a muted
  bench reads.
- **The audio bench** (`panel_voice_bench_play`, dev builds only, AUD-030): 16 kHz PCM from the
  dev link's `PUT /dev/v1/play`, upsampled by D11's rule (`vr_up`), through the Voice volume's
  boost (`speech_boost`, AUD-033) and written to the speech ring by the worker between
  utterances, as an utterance is written; a newer one replaces it, an alarm refuses it, and it is
  cancelled 5 s past its own length. Release builds answer `unavailable`.
- **Words said from the pack for the bench** (`panel_voice_bench_say`, dev builds only, AUD-030):
  up to eight clip IDs from the dev link's `POST /dev/v1/say`, said as a Preview is (the
  annunciator, the renderer, the boost, the mixer), with the gap between words the bench gives
  (1-250 ms, `PANEL_VOICE_BENCH_GAP_MAX_MS`; 0 the pack's): `ann_set_pack` with the pack's
  revision and that gap, so nothing waiting is dropped and every utterance uses it until the next
  call, a restart or a new pack. `panel_voice_status` reports the volume, the gap and whether
  announcements are on. Release builds answer `unavailable`.
- **Install** (`panel_voice_install`, D12): refused while loading, locked (armed, entry/exit,
  alarm, walk test, installer mode) or busy. The service keeps two copy slots and reserves the
  spare one, and prepares the decoder, before it allocates anything: BUSY while a retired copy is
  still leased or a new one waits. A PSRAM copy of the file goes through `pa_install`
  (signature, envelope with 0xFF padding, `vp_open_signed`, then the panel's acceptance and the
  pack's identity, then the same gate before the erase and the commit), and `done` swaps it in:
  speech stops, the policy gets the new revision, the old copy is freed when its last lease ends.
  If the panel locked between the gate and `done`, the stored pack waits in the reserved slot and
  the install answers `locked`: the working voice keeps playing, and a RAM-only swap happens once
  the panel is open again (a 1 s timer, a status call or the next install). Never changes a
  preference. A caller whose stack is in PSRAM has the flash work done on a short-lived
  internal-stack task.
- **What it accepts** (`vp_accept`, D08): only en-US `qwen3-designed_female`; a DRAFT pack only
  on a dev build (a release build refuses it as `bad_pack`). The boot load applies the same check.
- **Identities** (D08, `vp_accept.h`): locale, voice ID and revision name a pack for good, with
  the SHA-256 of its unpadded payload. The install records each new identity in NVS (namespace
  `voice_id`, one 88-byte blob per identity under a key hashed from it, on the internal-stack
  install task, committed before the store's table) and keeps every record through replacement,
  removal and reboot. Refused as `bad_pack`: a known identity with other audio (approved spoken
  names would change sound silently) or a key holding another identity; as `no_space`: a 17th
  identity; as `failed`: an NVS error. At boot a pack with no record (one the Flasher wrote over
  USB, or one installed before the records existed), signed for this panel and accepted, has its
  identity recorded then, as an install records it; one whose identity is known with a different
  digest is `unavailable` ("reinstall the pack").
- **Console** `voice` (alarm panels and link keypads): the state, the pack, the partition, the
  counters and the ring's underruns; `voice preview <zone>` (the zone's saved words, as the
  editor's Preview); `voice say <clip-id>...` (raw IDs, hex or decimal, status and walk clips too).

## The decoder
esphome/micro-opus **0.4.1** (`idf_component.yml`, exact; `firmware/*/dependencies.lock`),
which is commit `8354085908683c6130e32a832aeec8a7ca115c51` of esphome-libs/micro-opus with libopus
at gitlink `22244de5a79bd1d6d623c32e72bf1954b56235be` (v1.6.1). micro-opus reports its own
version string, `libopus 1.4-fixed`, whatever the gitlink. The PC builds fetch the same commit
(`cmake/alarm_opus.cmake`); never pick another libopus.

Firmware options (`firmware/crowpanel_p4/sdkconfig.defaults`): fixed point, no Xtensa code (the
P4 is RISC-V), `CONFIG_OPUS_USE_ALLOCA=y` - the decoder's scratch lives on the voice worker's own
256 KiB PSRAM stack, which never writes flash or NVS - and libopus's own allocations PSRAM only.
Linked in, the decoder adds about 64 KiB to the image.

## The renderer
`voice_render.c` (plain C11 like the codec: no ESP-IDF, no heap, no globals; the PC builds it from
`cmake/alarm_opus.cmake`) turns one utterance - up to `VR_ITEMS_MAX` (16) items of
`{clip_id, gap_before_ms}` from an opened `vp_pack_t` - into 32 kHz mono PCM, pulled
incrementally: `vr_begin(r, pack, items, n, codec_state, codec_bytes)`, then `vr_pull(r, out, max)`
until it returns 0 (finished); a negative return cancels the utterance for good. Only one packet is
decoded ahead (the panel's worker pulls 320-640 samples at a time), and any pull size gives the same
samples. The first item's gap is ignored; a gap is exact silence (20 ms = 640 samples at 32 kHz).
`vr_total_samples_32k(r)` is the exact length, 2 x (sum of valid samples + gaps at 16 kHz), known at
`vr_begin`, which refuses an utterance over 10 s (`VR_ERR_TOO_LONG`) or a missing clip
(`VR_ERR_CLIP`). Each clip follows the codec's loop (reset, pre-skip and final trim from the index
entry) and is upsampled by D11's rule: `x[n]`, then `(x[n]+x[n+1])/2` in int32 truncating toward
zero, one sample of lookahead carried across packets, the final sample repeated, reset at each clip.
There is no gain or normalisation: the Voice volume's boost after it (`speech_boost`, AUD-033)
owns it. The `vr_t` is about 1 KiB and the caller's.
`test/test_voice_render.c` runs the real en-US voice, packed at build time by `tools/voice/voice.py`
(ignored where there is no Python 3).
