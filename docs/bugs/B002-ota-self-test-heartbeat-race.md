# B002 · The update self-test could report a working part as missing

**Status (2026-09-28): fixed and verified on the bench panel** (the replacement board, P4 v1.3,
dev profile): all four steps under "Verify on hardware" passed, step 2 after
[B016](B016-dev-upload-reply-lost-to-lru-purge.md) - which one of these fixes caused - was fixed.
Committed and pushed as `8367725` (2026-09-22). Found by code review, not by a failure on the
panel; the failure it describes is rare per tick and was never observed, but it is not a
theoretical one.

## Symptom (what would have happened)

- During the 60 s probation after a firmware update, the panel rolls a good image back with
  "self-test failed: … not running" although every part was reporting.
- Outside probation, the panel raises the "sensors stopped reporting" trouble (the same one a
  lost source link raises) for one health tick, then clears it.

## Root cause

`panel_ota.c` `health_missing()` computed the age of each heartbeat as
`(uint32_t)(now_ms - s_health_last[b])`. `now_ms` is read once per tick of the update task;
the heartbeats are stamped by other tasks (the alarm task, the LVGL task, the health messenger)
on the other core, and `probation_tick()` may wait on the OTA lock before the check. A heartbeat
stamped even 1 ms after `now_ms` was read made the unsigned difference wrap to about 4.29e9 ms,
which is greater than the 15 s staleness limit, so the part counted as missing. On probation
that is an immediate `esp_ota_mark_app_invalid_rollback_and_reboot()`.

## Fix

The age is signed: `(int32_t)(now_ms - last) > STALE` treats a heartbeat from "the future" as
fresh. Related state bugs found in the same review and fixed alongside:

- **A restart could boot a slot another upload was rewriting.** An image waiting for the
  system to go idle was not "busy", so `panel_ota_begin()` accepted a new upload into the same
  slot while `try_restart()` was between its two idle checks; the restart then set the boot
  partition on a half-written slot. `panel_ota_begin()` now refuses while an image waits, and
  `try_restart()` re-checks under the lock that the same verified image is still waiting
  before it marks the phase as restarting.
- **A cancelled remote install left `armed=1` in NVS.** The next boot of that same image, by
  whatever route (console `ota apply`, serial upload), then waited for a remote confirmation
  nobody would send and rolled back after 180 s. Dropping a remote install now clears the
  flag (`remote_disarm()`), and revoking or switching off developer access cancels a waiting
  remote install (`panel_ota_cancel_remote()`) and clears the staging record.
- **An image refused for a profile mismatch stayed "ready".** `panel_ota_cancel()` did nothing
  in the idle phase, so the status showed the refused image as ready and the console's
  `ota apply` would have installed it. Both the dev service and the Panel Manager now call
  `panel_ota_discard()`.
- Smaller: the dev HTTPS listener purges idle sessions (`lru_purge_enable`), `dev commit`
  rejects trailing bytes after the certificate (they would have broken the fingerprint match
  silently), the `dev` console command's 2 KiB texts moved off the 6 KiB console stack, and a
  first-enable identity write cut short by a power loss (key or certificate missing, the
  other present) is regenerated instead of disabling the development service and the Panel
  Manager for good.

## Verify on hardware

1. A normal dev image (`pwsh tools/idf.ps1 -Profile dev build`).
2. Signed network update with the tools in docs/features/30; probation must pass with all parts
   reporting (`panelctl info`: `health_stale` 0), with a browser viewing the keypad during it
   (that is the busiest the heartbeats get).
3. `ota status` at the console after a refused cross-profile upload: nothing "ready".
4. Prepare a remote install, cancel it (`/cancel`), then `ota apply` the same image at the
   console: the boot must not wait for a remote confirmation.

### Results (bench panel, 2026-09-28)
1. Passed: the dev profile (`alarm_crowpanel_p4-dev`), 0.6.x test images built with `-Profile dev
   -Version`.
2. Passed after B016's fix: 0.6.8-test installed with `panelctl flash` while `panelctl screen`
   streamed the keypad through upload, restart, reconnect and the 60 s self-test; `health_stale` 0,
   0 late frames, 0 guard trips. Every self-test on the bench that day (a dozen, with and without
   the viewer) passed with all parts reporting, and each fault image failed on exactly its part.
   Before the fix the upload's reply was lost whenever the viewer streamed (B016).
3. Passed: a release image (0.6.9-test) sent through the development route was refused with 409
   "Network updates must match the running board and firmware profile"; afterwards `staged_operation`
   was empty and `ota` showed no "Ready" line. (Over the serial console a legacy image onto the dev
   panel is refused on its first line, "Firmware for another device", with nothing ready either.)
4. Passed: 0.6.5-test staged, installed and cancelled 0.4 s later (still on the old image, idle,
   `confirm_required` false, the image ready); `ota apply` at the console then booted it with the
   normal 60 s self-test ("first start of 0.6.5-test: confirming after a 60 s self-test"), confirmed
   at 60 s and still running at 205 s - no wait for a remote confirmation, no rollback at 180 s.
