# B019 · Home Assistant ignored the firmware state: `release_url` was null

**Status: fixed and proven on the bench panel 2026-09-29.** Found during BL-026 (MQTT with a real
broker and Home Assistant).

## Symptom
The panel's firmware `update` entity, made by MQTT discovery, stayed "unknown" in Home Assistant,
with no installed version, although the panel published its firmware state (retained) and the
discovery document was right.

## Evidence
Home Assistant 2026.9.3's log, for every firmware state the panel published:

```
WARNING homeassistant.components.mqtt.update: Schema violation after processing payload
'{"installed_version":"0.6.0","latest_version":"0.6.0","title":"AlarmSystem firmware",
"release_url":null,"in_progress":false,"update_percentage":null}' on topic
'alarmpanel/<id>/firmware' for entity 'update.alarm_panel_firmware': invalid url at 'release_url'
```

Home Assistant's schema for that payload takes `release_url` as optional but checks it with
`cv.url`, so a null fails and the whole payload is dropped. `update_percentage` may be null.

## Root cause
`proto_fw_state()` (`components/core/alarm_proto/src/fw.c`) wrote `"release_url":null` when the
panel knows no release (no release source set, the default), and the golden test expected it.

## Fix
`release_url` is left out when there is none; the golden payload in `test_fw.c` (OTA-010) changed
with it and checks that an empty URL leaves the key out too. The topic table in docs/features/10
and the interface in docs/features/13 say so. A consumer that read `release_url` as null now
finds the key missing; the keys themselves are unchanged.

## Verified
Bench panel, dev image `b6bce342…`, 2026-09-29: the retained payload has no `release_url`,
Home Assistant shows the update entity "off" (up to date) with installed and latest 0.6.0, and no
schema warning since the new image started. Host tests: 77 of 77 pass.
