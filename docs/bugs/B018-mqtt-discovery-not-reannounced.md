# B018 · MQTT discovery not announced again on the same connection

**Status: fixed and proven on the bench panel 2026-09-29** (pair and unpair; a zone set Not used
and back again, the other half, is owed with BL-026). Found during BL-026 (MQTT with a real broker
and Home Assistant).

## Symptom
With the panel on MQTT and Home Assistant using MQTT discovery: pairing the native Home Assistant
integration retracted the panel's 12 discovery documents, as it should (HAI-010), and Home
Assistant removed its MQTT entities. Unpairing again (console `ha unpair`) announced nothing: the
console said "not paired", the broker received no discovery document, and Home Assistant stayed
without the panel's MQTT entities until the panel reconnected to the broker (a restart did it).
The first unpair after a fresh connection had announced them.

## Evidence
Broker log after the second unpair: heartbeats only, no `homeassistant/.../config` publish.
The retraction just before it had been complete (empty retained documents for `p1`, `z1`..`z8`,
`trouble`, `event`, `firmware`), and a new subscriber found no retained discovery left.

## Root cause
`net_mqtt.c` keeps a CRC per discovery document (`s_disc_crc`) so an unchanged document isn't
sent twice during a connection; the cache is cleared only on connect. A retraction published the
empty document but left the CRC, so the same document wanted again later on the same connection
matched its CRC and was skipped. The same applied to a zone's retained state
(`s_zone_crc`), cleared when a zone is taken out of use: a zone set Not used and back would have
come back without its state until it changed. The Z-Wave devices' path (`zw_sync`) already
cleared its CRCs on retraction.

## Fix
`publish_config()` clears the document's CRC slot when it retracts, and every retraction in
`discovery_sync()` passes its slot; clearing a zone's retained state clears `s_zone_crc` too.
Affected: pairing and unpairing the native link, discovery off and on, a zone taken out of use
and back, all on one MQTT connection.

No host test: `net_mqtt` is ESP-IDF only and has no host harness; the proof is the bench run.

## Verified
Bench panel, dev image `b6bce342…`, 2026-09-29, Home Assistant 2026.9.3 with MQTT discovery:
pair - all 12 retracted and Home Assistant removed the entities and the device; unpair on the
same connection - 24 config publishes (12 retractions, 12 announcements) and all 12 entities
back with their states (disarmed, zones closed, trouble off, firmware up to date).
Still to see: a zone set Not used and back at the panel (installer), on one connection.
