# B017 · Every start aborted with the Panel Manager on: one handler slot short

**Status: fixed and proven on the bench panel 2026-09-29.** Found while preparing BL-027 (the
Home Assistant acceptance): a network update of current `main` to the bench panel was rolled
back twice.

## Symptom
`panelctl flash --device bench-panel ...` reported "Device restarted into another image
(rollback); candidate not accepted". The panel's event log showed the new image start, restart
five seconds later, then `firmware_update arg 2` (rolled back); `ota` said "the update to 0.6.0
failed its self-test". The rollback worked: the panel came back on its previous image, disarmed
and healthy.

## Evidence
The serial console, captured during a second attempt (git-ignored `build/bl027/crash-serial.log`):

```
I esp_https_server: Server listening on port 443
W httpd_uri: httpd_register_uri_handler: no slots left for registering handler
ESP_ERROR_CHECK failed: esp_err_t 0xb001 (ESP_ERR_HTTPD_HANDLERS_FULL) at 0x400992f0
file: ".../components/services/panel_web/src/panel_web.c" line 1945
func: route
expression: httpd_register_uri_handler(server, &u)
abort() was called at PC 0x4ff0c96b on core 0
```

## Root cause
The HTTPS server had `max_uri_handlers = 10`. At its start it registers the remote keypad's
WebSocket and ten `route()`s; the Backup and Restore work (`9f8ab94`, BL-002, 2026-09-29) added
the two `/api/v1/backup` routes, making eleven. `route()` wrapped the registration in
`ESP_ERROR_CHECK`, so the eleventh aborted the panel on every start with the web server on. No
host test starts the real ESP-IDF server, and no panel had run an image built after `9f8ab94`
(the bench panel was last flashed on 2026-09-28).

## Fix
- `WEB_URI_HANDLERS` (16) sizes the server, with room to spare.
- `route()` and `web_screen_register()` log a handler that doesn't fit and carry on: a missing
  page must never cost the alarm.
- `tools/tests/test_panel_web_routes.py` (CI runs it) counts the routes registered at the
  server's start and the WebSocket against `WEB_URI_HANDLERS`, checks that neither registration
  aborts, and would have failed on the table that crashed.

## Verified
The fixed dev image (ELF `45404c47…`) installed on the bench panel by signed network update on
2026-09-29, passed its probation and stayed: `web` reports HTTPS 443 enabled, display 0 late
frames and 0 guard trips, no crypto faults.
