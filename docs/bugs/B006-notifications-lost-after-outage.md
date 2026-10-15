# B006 · Phone notifications from an outage could be written off unsent

**Status (2026-09-22): committed and pushed as `8367725`; fixed in code, built for every profile, not yet flashed.** Found by
code review of `net_notify.c`, `net_wifi.c` and `app_main.c`; not observed.

## Symptoms

1. After a power cut the router hands out an address before its uplink or DNS works. The
   panel's notifier saw "Wi-Fi connected", digested the outage's events, posted them - every
   post failed - and then marked the whole backlog delivered, the burglary alarm from the
   outage included. Nothing was sent later and no trouble was raised.
2. Three ordinary lines in that outage digest ("Mains power lost", a bypass, a restore)
   overflowed the 96-byte digest body on the notify task's stack: `snprintf` returns the
   length it wanted, the running total was not clamped, and the final "and N more" was
   written past the end with a size that had wrapped.
3. Reports the event log could not keep (LOG-010) were dequeued and dropped while the
   network was down.
4. The owner disarming while Wi-Fi was down left Pushover's emergency repeat ringing the
   phones for up to 30 minutes: the cancel was tied to a notification bit that had already
   been consumed.
5. MQTT switching itself off cleared the "network" trouble that the notifications or the
   Home Assistant link still depended on (`net_wifi_set_required` was last-writer-wins).
6. `esp_wifi_connect()` itself failing (co-processor busy) scheduled no retry: no disconnect
   event follows a call that never started, so the panel sat in "connecting" for good.
7. An unreadable network-trial record, or any failure of `net_wifi_start()`, the connection
   check or a console registration aborted start-up **after** the engine had started - a
   reboot into the same abort with the service console (the recovery path) never reached.
8. 24.9 days after the last remote view, the 32-bit millisecond deadline for low-latency Wi-Fi
   went "active" again and switched power saving off for weeks at a time.
9. Pushover's quota header was looked up among the request's own headers and, had it ever
   matched, freed memory the HTTP client still owned.

## Fixes

- `catch_up()` returns whether the service took the digest; nothing is marked delivered
  otherwise and the digest is retried 30 s later (`digest_retry_at`).
- `append_text()` clamps every appended line; the tail is only written when room remains.
  The alarm roll-up uses the same helper.
- Live reports are peeked and only dequeued once a provider took them, or when no provider
  wants them; they wait while offline.
- The cancel is owed until the service was reached (`cancel_repeats()` reports that); the
  partition bits are exchanged and restored atomically.
- `net_wifi_set_required(who, on)` keeps one bit per requester (MQTT, notifications, HA).
- `connect_now()` asks the net task for a retry with the usual backoff when the call fails.
- Start-up after the engine: `START_OR_LOG()` logs a part that did not start and carries on;
  an unreadable trial record is cleared, not fatal.
- The low-latency deadline is forgotten once it has passed.
- The quota lookup is gone (the quota stays "not told", as it effectively always was).

Also moved: `ha_link`'s state was split so that only its plain buffers (~13 KB) are in
PSRAM; the flags it exchanges with atomic instructions stay in internal RAM.

## Not changed, recorded as open questions

- The keypad sounder follows partition 1 only (`app_main.c` `on_snapshot`); an alarm in
  partitions 2-4 is silent at this panel. Fine for the single-partition house; decide before
  a multi-partition install.
- `CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE` restarts the whole panel when the C6
  link fails. The alternative (staying up without network until someone restarts) needs an
  `ESP_HOSTED_EVENT` handler and a decision.
- Three owners share the single `ALARM_TRBL_STORAGE` bit and can clear each other's.
- The MQTT acknowledgement queue (32) can overflow on a big discovery burst and delay one
  report by its 30 s acknowledgement timeout.

## Verify on hardware

With ntfy or Pushover on: block the panel's internet at the router (Wi-Fi stays up), cause
a few events (chime toggles, a bypass), unblock: the phone gets the digest once, and
`events` shows no `report_lost`. Switch MQTT off with notifications on and pull the router's
plug for six minutes: the "network" trouble still appears.
