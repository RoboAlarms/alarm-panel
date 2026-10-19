# CrowPanel C6 co-processor

Radio-only ESP-IDF 6.1 project `crowpanel_c6_cp`; the folder is shortened to `c6_cp`
to stay within Windows' ESP-Hosted path limit. Uses pinned ESP-Hosted 3.0.9 with Wi-Fi,
SDIO and the [ESP-NOW bridge](../../components/services/espnow_bridge). Bluetooth is off.

Build: `pwsh tools/idf.ps1 -Project c6_cp build` (POSIX: `tools/idf.sh --project c6_cp build`).
The signed application is `build/crowpanel_c6_cp.bin`. The existing panel signing key
is used; no eFuses, secure boot at boot time or flash encryption are enabled.

Install only with the owner's explicit C6-update authorization. Stage the application
in the P4's existing `cp_fw` partition and use its co-processor updater while disarmed.
The P4 verifies the signature before streaming. **Do not flash this project's bootloader,
partition table or initial OTA data to a deployed C6.** SDIO OTA keeps its factory
partition table and writes the inactive application slot. The build's 4 MB table only
bounds the image; the running C6 updater also enforces its actual inactive-slot size.

Acceptance must check ordinary Wi-Fi, bridge initialization, both directions of LAN/radio
failover, commands with duplicates suppressed, radio relay and an interrupted C6 transfer.
Build success and host tests alone do not establish hardware acceptance.
