# net_notify

**Tier:** services (ESP-IDF, board-agnostic) · **Features:** [26 Notifications](../../../docs/features/26-Notifications/README.md), [27 ntfy](../../../docs/features/27-ntfy/README.md), [28 Pushover](../../../docs/features/28-Pushover/README.md) ·
**Tests:** everything that can be decided without a socket is portable and host-tested in `notify`: [test_notify.c](../../core/notify/test/test_notify.c) (the words, the flood gate and the request builders; `NOTE-NNN`, `NTFY-NNN`, `PUSH-NNN`). The per-service store-and-forward is `alarm_proto`'s reporter, tested in [test_report.c](../../core/alarm_proto/test/test_report.c). This component has no host test of its own. On a panel: the `notify` console command (`notify help`, `notify test`). ntfy delivered end to end on the reference panel on 2026-09-21 (docs/features/27); Pushover has not been run against the real service (docs/features/28).

The transport for push notifications to a phone. One task listens to the engine, asks
`notify` what to say and whether to say it (categories, rate limits, the daily cap, alarm
rollup), and posts the result with `esp_http_client` over HTTPS to ntfy (hosted or
self-hosted) or Pushover. The event log is the queue, as for MQTT (ADR-018): each service keeps
its own delivered position in NVS, so an alarm raised during a Wi-Fi outage still reaches the
phone later, and one failing service never holds up the other. A test notification and the
Pushover key check run on the task and are collected by polling, because the caller is the UI
thread. It does not choose the wording: the sentences are `notify`'s, shared with the
Activity list.

## Public API
| Header | What it offers |
|---|---|
| `include/net_notify.h` | `net_notify_listener()` (register before `alarm_service_start()`), `net_notify_start()`, ntfy and Pushover settings get/set (tokens and keys are never returned, only whether one is stored), `net_notify_new_topic()`, `net_notify_get_status()`, `net_notify_test_start()` / `net_notify_test_result()`, `net_notify_set_ca()` (a self-hosted ntfy's CA), `net_notify_console_register()` |

## Depends on
- Project components: `platform_esp` (`alarm_service` listener and configuration, `panel_log` as the queue and its lost-report handler, `net_wifi` for the network requirement), `notify` (words, gate, request builders), `alarm_proto` (`proto_report` delivery positions and replay), `alarm_mem` (slow memory).
- ESP-IDF: `console`, `esp_http_client`, `esp-tls` (certificate bundle), `esp_timer`, `nvs_flash`.

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `ALARM_FEATURE_NOTIFY` (`firmware/crowpanel_p4/main/Kconfig.projbuild`) | y | Off: `net_notify.c` compiles to nothing, `app_main.c` neither registers the listener nor starts the task, and `ui_port_esp.c` leaves the notification entries out, which hides the Notifications group |

Which services are on, the ntfy server and topic, categories, the Pushover credentials, sound,
device and emergency repeat are runtime settings in NVS.

## Resources
- Task `notify`: 8192-byte internal stack, priority 4 (`net_notify.c:1289`, `TASK_STACK` at `:46`). The HTTPS client and the TLS handshake run inline on this stack.
- One HTTPS request in flight at a time, at least 1.5 s apart per service, and a failed one waits a growing, jittered backoff (`NOTIFY_SEND_GAP_MS`, `notify_backoff()` in `components/core/notify` `notify_http.h`, NOTE-035/014); a 10 s timeout (`:50`). Public servers are verified with the certificate bundle (`:344`).
- Slow memory (`alarm_mem`, PSRAM on the P4): a configuration copy, the request, a 512-byte response scratch and a 4096-byte CA buffer (`net_notify.c:1252-1255`); the per-service state and the snapshot (`ALARM_SLOW_BSS`, `:118`, `:125`). The status the UI polls stays in internal RAM (`:141`).
- A live-report queue of 8 events (`LIVE_QUEUE`, `net_notify.c:58`).
- NVS namespaces `notify` (delivered positions), `ntfy`, `pushover` (`net_notify.c:42-44`).

## Rules
- The task keeps its own copy of the settings and publishes a status snapshot: never hold its lock across a post, and never wait for a post on the LVGL thread (docs/features/26).
- The listener callbacks only queue and never block; only the alarm task touches the engine (`alarm_service.h`).
- The task writes NVS, so its stack stays in internal RAM (CONTRIBUTING.md, "Rules for the firmware").
- Tokens, the Pushover user key and the application token are never printed or returned; the console shows only whether one is set (NOTE-022).
- Change a sentence in `notify` (`notify_words.c`), not here (CONTRIBUTING.md, "Rules for the UI").
- Planned (docs/features/33 step 6): the firmware requires this component only when `ALARM_FEATURE_NOTIFY` is on.
