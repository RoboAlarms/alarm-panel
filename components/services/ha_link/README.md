# ha_link

**Tier:** services (ESP-IDF, board-agnostic) · **Features:** [23 Home Assistant integration](../../../docs/features/23-Home-Assistant-Integration/README.md) ([protocol](../../../docs/features/23-Home-Assistant-Integration/protocol.md)), [22 Zone sources](../../../docs/features/22-Zone-Sources/README.md) (entities as zones), [24 Devices](../../../docs/features/24-Home-Control/README.md) ·
**Tests:** the protocol is portable and host-tested in `alarm_proto`: [test_link.c](../../core/alarm_proto/test/test_link.c) (framing, pairing code, messages; `HAI-002`, `-003`, `-009`, `-013`) and [test_pair.c](../../core/alarm_proto/test/test_pair.c) (the pairing state machine, `HAI-003`). The Home Assistant entities as zones are in [test_zone_hub.c](../../core/zone_hub/test/test_zone_hub.c) and the device source in [test_home_control.c](../../core/home_control/test/test_home_control.c). The client side (`aioroboalarms`, the HACS integration) is tested in `../ha-addon`. This component has no host test of its own. On a panel: the `ha` console command; evidence in [validation.md](../../../docs/features/23-Home-Assistant-Integration/validation.md).

The panel's side of the native Home Assistant integration. It advertises `_roboalarms._tcp`
over mDNS, serves one TLS connection at a time on port 6054 (mbedTLS server over lwIP), and
pairs a Home Assistant only after both screens show the same 6-digit code and a person allows
it on the panel. After pairing both sides pin each other's certificate fingerprint. It
sends snapshots, deltas and events, executes commands, and carries the Home Assistant catalog,
entity states and device actions to `zone_service` (entities as zones) and `device_service`
(the Devices screen). While paired it suppresses MQTT discovery (HAI-010). The panel's
certificate is a self-signed P-256 identity generated on the panel (`ha_cert.c`), not a
credential from an authority. The message formats are `alarm_proto`'s; this component is the
transport, the storage and the task around them.

## Public API
| Header | What it offers |
|---|---|
| `include/ha_link.h` | `ha_link_listener()` (register before `alarm_service_start()`), `ha_link_start()`, `ha_link_get_status()`, `ha_link_set_enabled()`, pairing (`ha_link_pair_begin()`, `_decide()`, `_cancel()`), `ha_link_unpair()` (durable or it says so), `ha_link_console_register()` |
| `ha_link_internal.h` (private) | `ha_cert_ensure()`: load the panel's certificate and key from NVS, generating them on first use |

## Depends on
- Project components: `platform_esp` (`alarm_service`, `net_wifi`, `zone_service`, `device_service`), `alarm_proto` (link framing, messages, pairing), `net_mqtt` (discovery suppression), `panel_ota` (update status for the telemetry), `alarm_mem` (slow memory), `alarm_core` types.
- ESP-IDF: `console`, `esp_app_format`, `lwip`, `mbedtls` (TLS server, X.509 writing, PSA key generation), `nvs_flash`; `espressif/mdns` `^1.8.0` from the component registry (`idf_component.yml`).

## Options
| Kconfig | Default | Effect |
|---|---|---|
| `ALARM_FEATURE_HA` (`firmware/crowpanel_p4/main/Kconfig.projbuild`) | y | Off: `ha_link.c` compiles to nothing, `app_main.c` neither registers the listener nor starts the task, `ui_port_esp.c` leaves the link entries out; `ALARM_ZSRC_HA` and `ALARM_HOME_CONTROL` depend on it |
| `HA_LINK_DIAGNOSTICS` (`Kconfig`) | n | Adds `ha storefail on\|off`, a simulated trust-store write failure. Development only: the firmware's `CMakeLists.txt` refuses a release profile with it on |

The build also needs `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` (fingerprint pinning; `#error`
at `ha_link.c:58`) and `CONFIG_MBEDTLS_X509_CREATE_C` / `X509_CRT_WRITE_C`, both set in
`sdkconfig.defaults`.

## Resources
- Task `ha_link`: 12288-byte internal stack, priority 5 (`ha_link.c:1308`); the TLS handshakes run on it. It polls flags and deadlines every 500 ms (`POLL_MS`, `:69`).
- One connection at a time: a TLS handshake is abandoned after 8 s and a silent connection after 90 s (`ha_link.c:70-71`).
- Slow memory (`alarm_mem`, PSRAM on the P4): a 64 KiB receive buffer per connection (`PROTO_LINK_FRAME_MAX`, `ha_link.c:1026`), a 48 KiB transmit buffer while enabled (`PROTO_LINK_SNAPSHOT_MAX`, `:1175`), the certificate, key, configuration and snapshots (`SB`, `ALARM_SLOW_BSS`, `:186`) and the watch list (`:703`). The shared flags stay in internal RAM (`S`, `:174`) because they are exchanged with atomic instructions.
- Queues: 32 events from the alarm task, 8 device calls (`ha_link.c:73-74`).
- NVS namespace `halink` (`ha_link.c:1286`): the trust record, the panel's key and certificate. An unpair that missed flash is retried every 5 s (`:83`).

## Rules
- Nothing the client does may own the link task: every read and write has a deadline (HAI-002). Other tasks reach the task only through queues and atomic flags, with zero-timeout hand-overs (`ha_link.c`, header comment).
- What is paired is only what reached flash; never tell a user a client is gone on anything but `ESP_OK` from `ha_link_unpair()` (HAI-015).
- The task writes NVS, so its stack stays in internal RAM (CONTRIBUTING.md, "Rules for the firmware").
- The protocol is API 1 and changes are additive (docs/features/23, protocol.md); keep `alarm_proto`'s golden vectors in step with `../ha-addon`.
- Only Settings > Integrations, the integration's own page and a zone's source picker name Home Assistant on screen (CONTRIBUTING.md, "Rules for the UI").
- Planned (docs/features/33 step 6): the firmware requires this component only when `ALARM_FEATURE_HA` is on.
