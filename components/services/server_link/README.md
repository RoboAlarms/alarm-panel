# server_link

The panel's link to an alarm-server (BL-225; the plan is alarm-server's
`docs/features/02-Panel-and-App-API/panel-link.md`). A second esp-mqtt client beside `net_mqtt`, with
its own topics, settings and delivered position.

- **Transport**: MQTT 5 over `wss://<server>:443/mqtt`, outbound only. TLS against the public bundle,
  or a certificate authority loaded with `server ca`.
- **Identity**: an Ed25519 key made once, in NVS (`srvlink`), never exported and not in backups. The
  panel's id is the first 16 hex digits of the SHA-256 of its public key. Each connect signs a counter
  that only goes up, written to NVS first, so esp-mqtt's own reconnect is off.
- **Never linked and not linking**, it holds no connection and asks nothing of Wi-Fi.
- **Events**: the event log is the queue (`proto_srvq_*`); only the server's `down/ack` moves the
  position. Every event record carries its uptime in milliseconds and what set the clock.
- **Commands** go through the remote ceiling and then `alarm_service_command_as()`, or
  `alarm_service_command()` with a sealed code. Never queued; the same id gets the same answer for 10
  minutes.

Console: `server` prints the state; `server help` lists the rest. Linking at the console needs an Owner
or Admin code, as the touchscreen will.

Everything that can be decided without a socket is in `alarm_proto` (`proto_server.h`,
`test/test_server.c`).
