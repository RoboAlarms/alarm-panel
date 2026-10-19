# espnow_bridge

**Tier:** services (ESP-IDF, co-processor only). **Requirements:** CP-003 in
[12](../../../docs/features/12-C6-Coprocessor-Firmware/README.md).

The C6 half of the panel link's ESP-NOW transport. `espnow_bridge_start()` registers
the versioned Hosted peer-data channel and starts a worker. Radio and RPC callbacks
copy into bounded queues; the worker owns peers and sends. A send has a 200 ms
completion deadline. On timeout the radio driver is deinitialised before another
completion slot can be used; the host's next INIT restores it.

The shared, header-only wire is `alarm_proto/espnow_bridge_proto.h`, included without
linking the alarm engine. It carries INIT/channel, add/remove peer, SEND, completion,
receive with RSSI/channel, STATE and bounded application-version INFO. Invalid lengths, operations and versions are
rejected before copying. Link encryption remains on the P4. No ESP-NOW LMKs are used.
Each peer uses the current station channel; a joined station is never retuned away
from its access point. The P4's network task provides a temporary disconnected scan
window when a keypad has lost its master.

The application initializes NVS before starting the bridge. INIT refuses to start
ESP-NOW until the Wi-Fi driver is running. The host also waits for that initialization;
calling ESP-NOW during Wi-Fi bring-up caused the radio to stop responding on hardware.
INFO carries the actual application revision: Hosted 3.0.9's app-description helper
only synthesizes its library version and cannot identify a bridge revision.

Dependencies: ESP-Hosted 3.0.9 peer data, ESP-IDF Wi-Fi and FreeRTOS. Compiles empty
on a host panel. The wire's host tests are `alarm_proto/test/test_espnow_bridge.c`;
actual SDIO/radio delivery and timeout recovery require both panels.
