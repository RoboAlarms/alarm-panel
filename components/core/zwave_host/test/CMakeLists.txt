# The Z-Wave host's S2/S0 crypto (docs/features/36, ZW-008; vectors security.md section 11).
alarm_add_test(zw_crypto SOURCES test_zw_crypto.c zw_host_crypto.c LIBS zwave_host unity)
# The Serial API transport and scheduler against the fake ZAC93 on a fake clock (ZW-003).
alarm_add_test(zw_transport SOURCES test_zw_transport.c fake_zac93.c fake_zdev.c fake_zdev_cc.c
               zw_host_crypto.c LIBS zwave_host zwave_proto unity)
# The host: start-up, §4's checks, recovery and restarts against the fake ZAC93; a jammed
# module's retries and soft reset through zw_net (ZW-001, ZW-004, ZW-037).
alarm_add_test(zw_startup SOURCES test_zw_startup.c net_harness.c fake_zac93.c fake_zdev.c
               fake_zdev_cc.c zw_host_crypto.c LIBS zwave_host zwave_proto unity)
# The S2 security manager: a panel and a device over a pretend radio (ZW-007, ZW-009).
alarm_add_test(zw_s2 SOURCES test_zw_s2.c zw_host_crypto.c LIBS zwave_host zwave_proto unity)
# Adding a device end to end: zw_net against the fake ZAC93 and a simulated sensor with a real
# joining-side S2 (ZW-005, ZW-006, ZW-007, ZW-009).
alarm_add_test(zw_inclusion SOURCES test_zw_inclusion.c fake_zac93.c fake_zdev.c fake_zdev_cc.c
               zw_host_crypto.c LIBS zwave_host zwave_proto unity)
# The node cache: codec, its frozen fixture and reconciliation (network.md §2; ZW-017).
alarm_add_test(zw_nodes SOURCES test_zw_nodes.c LIBS zwave_host unity)
# Wake-up handling and supervision per node, on a fake clock (network.md §7-§8; ZW-011, ZW-012).
alarm_add_test(zw_health SOURCES test_zw_health.c LIBS zwave_host unity)
# Keeping track of devices: the interview, the node cache at every start, sleeping devices and
# supervision wired, removing, region and reset, against simulated devices (network.md §2, §5-§8,
# §10, §12; ZW-002, ZW-010, ZW-011, ZW-012, ZW-016, ZW-017, ZW-018). One test hands zw_net's news to
# the bridge to the zones (zwave_zones over zone_hub's zsrc_zwave) to see what it asks devices.
alarm_add_test(zw_interview SOURCES test_zw_interview.c net_harness.c fake_zac93.c fake_zdev.c
               fake_zdev_cc.c zw_host_crypto.c LIBS zwave_zones zone_hub zwave_host zwave_proto unity)
# The virtual house: six simulated devices through restarts, a pulled module and a removal over
# hours of fake time (the Z5 "Done when"; ZW-010, ZW-011, ZW-012, ZW-016, ZW-017).
alarm_add_test(zw_house SOURCES test_zw_house.c net_harness.c fake_zac93.c fake_zdev.c
               fake_zdev_cc.c zw_host_crypto.c LIBS zwave_host zwave_proto unity)
# SmartStart end to end (network.md §4.2; ZW-024): the provisioning list, the listen mode, a
# device that asks to join with its DSK, forced S2 on the entry's terms, a failed join removed.
alarm_add_test(zw_smartstart SOURCES test_zw_smartstart.c net_harness.c fake_zac93.c fake_zdev.c
               fake_zdev_cc.c zw_host_crypto.c LIBS zwave_host zwave_proto unity)
# ZW-033: an end device firmware transfer (Firmware Update Meta Data) against a scripted device.
alarm_add_test(zw_fwupd SOURCES test_zw_fwupd.c LIBS zwave_host zwave_proto unity)
# What a region change does to the devices: families, Long Range, how many stop (network.md §12;
# ZW-045).
alarm_add_test(zw_region SOURCES test_zw_region.c LIBS zwave_host unity)
alarm_add_test(zw_jam SOURCES test_zw_jam.c LIBS zwave_host zwave_proto unity)
# Rebuilding routes (network.md §11; ZW-024): every device in turn against the fake ZAC93, a
# sleeper at its wake-up, retries, a dead device skipped, stopping.
alarm_add_test(zw_routes SOURCES test_zw_routes.c net_harness.c fake_zac93.c fake_zdev.c
               fake_zdev_cc.c zw_host_crypto.c LIBS zwave_host zwave_proto unity)
# Replacing a failed device (network.md §10; ZW-024): the new device keeps the node ID and
# generation, gets its own keys, is interviewed; refusals, stop, an empty window.
alarm_add_test(zw_replace SOURCES test_zw_replace.c net_harness.c fake_zac93.c fake_zdev.c
               fake_zdev_cc.c zw_host_crypto.c LIBS zwave_host zwave_proto unity)
# The radio's transmit power (network.md §12, phase 3; ZW-025): read, changed within the cap the
# owner keeps, applied by the soft reset of the start-up that follows, read again then.
alarm_add_test(zw_power SOURCES test_zw_power.c net_harness.c fake_zac93.c fake_zdev.c
               fake_zdev_cc.c zw_host_crypto.c LIBS zwave_host zwave_proto unity)
# The radio's own firmware over the wire (serial-api.md §8, network.md §14; ZW-025): the host
# against the fake ZAC93's Gecko bootloader and XMODEM receiver.
alarm_add_test(zw_otw SOURCES test_zw_otw.c fake_zac93.c fake_zdev.c fake_zdev_cc.c
               zw_host_crypto.c LIBS zwave_host zwave_proto unity)
