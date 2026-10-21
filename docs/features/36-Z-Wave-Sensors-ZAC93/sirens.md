# Z-Wave sirens

**Status:** built in the portable core (`components/core/zwave_zones` `zw_siren`, wired into
`zwz_panel`) and the firmware's `zwave_service` (the engine's bell, the bell trouble, NVS, the
console), with its rows on the device page (`ui_zwave.c`, approved 2026-10-20); host-tested and
the firmware built, not tried on a panel or a real siren yet. The
requirements are docs/features/16's OUT-016..OUT-019 (Z-Wave sirens); this page is the design.
Open question 8 of the [README](README.md).

A Z-Wave siren is one more of the panel's bells. It follows the engine's bell outputs with the
same policy as every siren of the panel (`components/core/sirens` `siren.h`, and Zigbee's sirens,
docs/features/20 alarm-devices.md §2): its own partitions, other partitions whose alarms it
`also` sounds for, burglary (and audible panic) and/or fire (fire and CO); fire wins over CO, CO
over burglary, burglary over the bell test.

## 1. Which devices

A device whose setup is done and that lists, at the device itself or at an end point:

- **Sound Switch (CC 0x79)** - preferred: a tone per alarm kind, a volume (v2), chirps. Examples
  to try on the bench: Zooz ZSE50 (Siren & Chime), Aeotec Siren 6 (ZW164: its tones are offered
  per end point - the assignment names the end point), Zooz ZSE19 (Multisiren) if it lists 0x79.
- **Binary Switch (CC 0x25)** - the fallback: on and off, one sound (whatever the device plays
  when switched on). Older sirens, and sirens sold with other panels (the Ring Alarm siren, the
  Qolsys IQ Siren in its Z-Wave form) if that is all they list.

The panel picks Sound Switch when the end point lists it, else Binary Switch (the installer may
ask for Binary Switch on a Sound Switch device). An end point assigned as a siren is no longer a
switch: the device page's switch and Home Assistant's entity refuse it (the engine drives it,
nobody else).

## 2. How it sounds

The alarm task's `on_output` (`ALARM_OUT_BELL` and its pattern) goes through the start-up's
listener to `zwave_service_engine_output`, which keeps the latest per partition and posts it to
the zwave task (`zwz_panel_engine_output` -> `zws_output`). The strobe output is not used (Sound
Switch has none).

| Engine | Sound Switch (Tone Play Set `79 08 tone [volume]`) | Binary Switch |
|---|---|---|
| `ALARM_PAT_STEADY` (burglary, audible panic) | its burglary tone (default `FF`: the device's own) | `25 01 FF` |
| `ALARM_PAT_TEMPORAL3` (fire) | its fire tone (0: the burglary tone) | `25 01 FF` |
| `ALARM_PAT_TEMPORAL4` (CO) | its CO tone (0: the burglary tone) | `25 01 FF` |
| `ALARM_PAT_TEST` (the walk test's bell test) | the burglary tone for `SIREN_TEST_MS`, then stop | on, then off |
| `ALARM_PAT_OFF` (silenced, timed out, disarmed) | stop: tone `00` | `25 01 00` |

The volume byte (0..100 %) is sent only when set (0 is the device's own volume) and never with a
stop; a Sound Switch v1 device ignores it. The bell's timeout and silencing stay the engine's.

**Confirmed, retried** (OUT-017). A Set is done when the radio says the siren took it (delivered:
its ACK, and Supervision's report where the transport adds it). Not delivered, not queued, or no
end told within `ZWS_CONFIRM_MS` (15 s): again `ZWS_RETRY_MS` (2 s) later; after `ZWS_SEND_TRIES`
(3) failures it is the bell trouble, and it goes again every `ZWS_RETRY_SLOW_MS` (10 s) while it is
still wanted, until one is delivered (the trouble clears). A stop nobody takes is given up after
`ZWS_STOP_TRIES` (about two minutes, the trouble stays) and sent again as soon as anything is heard
from the siren.

**Kept up while sounding.** Every `ZWS_CHECK_MS` (60 s) while it should sound the siren is asked
(Tone Play Get `79 09` / Switch Get `25 02`); a Report that says it is quiet (tone 0, switch off)
while its sound is wanted sends the sound again - many sirens stop on their own timer (Zooz's
default is a few minutes) while the engine's bell runs longer.

**Squawk** (OUT-019). With `squawk`, a Sound Switch siren plays its arm tone when one of its own
partitions arms (any state but disarmed, exit delay included) and its disarm tone when it
disarms - never for `also`, never while it sounds or a command of it waits, sent once and never
retried. Tone 0 plays nothing; a Binary Switch siren never squawks. The first state the panel
learns after a start is not a change (no chirp at boot).

**Test it.** `ZWS_TEST_MS` (2 s) of its burglary tone, then stop; refused while one of its own
partitions is armed, while it sounds, or while the radio is down.

## 3. Supervision (OUT-018)

Any frame from the siren's node counts as heard (its reports, its ACKs as the network sees them
through Supervision reports and Gets' answers, its own notifications). Unheard for its limit -
`supervision_min`, default `ZWS_SUPERVISION_DEF_MIN` = 240 min (4 h, the usual check-in of a
Z-Wave siren, as the Qolsys panels supervise theirs), at least 60 min - is the bell trouble
(`ALARM_TRBL_BELL` through `alarm_service_bell_trouble(ALARM_BELL_OWNER_ZWAVE, ...)`, shared with
the other sirens: it stands while any owner holds it); heard again clears it. While the Z-Wave
radio is down silence does not count (the radio's own trouble says it), and coming up every
siren's silence counts from then. A mains siren that is never asked anything may be silent for
hours: the keep-up Gets run only while it sounds, so a quiet siren relies on its own reports
(battery, notifications, Supervision) - see §5.

## 4. What is stored

NVS `zwave`/`siren` (`ZWZ_STORE_KEY_SIREN`, written by `zwz_store_run` with the namespace's other
values; counted in `ZWZ_NVS_WORST`): `'Z' 'S'` | format 1 | count | per siren: length | slot | node
u16 BE | generation u16 BE | end point | kind (1 Sound Switch, 2 Binary Switch) | partitions | also |
flags (b0 burglary, b1 fire, b2 squawk) | tones burglary, fire, CO, arm, disarm | volume |
supervision u16 BE minutes | CRC-16/CCITT-FALSE (`siren_crc16`) BE. A shorter entry reads its
missing fields as 0, a longer one is skipped past; all or nothing. Frozen fixture:
`components/core/zwave_zones/test/zws_fixture_v1.h` (CRC computed apart from the C code). Up to
`ZWS_SIRENS` (4). A siren whose device left the network (removed, forgotten, or its node ID given
to another generation while the panel was off) loses its assignment. The backup takes it with the
rest of `zwave` (`backup_policy.c`).

## 5. Unverified, to check on a bench

- Each model's command classes (Sound Switch at the root or per end point, its version), its tone
  ids and what `FF` plays; whether a v1 device really ignores the volume byte.
- Whether a siren reports Tone Play Report by itself when its own timer stops it (then the keep-up
  Get is only a backstop), and how long each model plays before it stops by itself.
- Whether chirps are short enough on each model with the tone ids it offers (some sirens' shortest
  tones last several seconds: the installer then leaves squawk off).
- How often a quiet mains siren is heard at all (OUT-018's default 4 h assumes it reports
  something - battery, a notification, a Supervision answer - at least that often; a siren that
  never speaks unasked would need a periodic Get when quiet, not built).
- Security: Sound Switch and Binary Switch Sets go at the class the device was granted
  (`zw_s2_class_for_endpoint`); sirens joined without S2 are accepted (a siren is an output, not a
  sensor whose word could be forged), to be reviewed with docs/features/36 security.md.

## 6. Where it lives

| What | Where |
|---|---|
| Policy, retries, supervision, the blob | `components/core/zwave_zones/include/zwave_zones/zw_siren.h`, `src/zw_siren.c`; tests `test/test_zw_siren.c` (OUT-016..OUT-019) |
| The panel's network: sends, reports, heard, removal, NVS, assignment API | `zwz_panel.h` (`zwz_panel_engine_output`, `zwz_panel_siren_armed`, `zwz_panel_siren_set/clear/test`, `zwz_panel_siren`, `zwz_panel_load/take_siren`); test `test_zwz_panel.c` `test_siren_follows_the_bell` |
| The firmware | `zwave_service_engine_output` (the start-up's listener calls it), the bell trouble, `zwave siren` on the console |
| The screens | pending (docs/features/24's Devices pages; another person's work) |
