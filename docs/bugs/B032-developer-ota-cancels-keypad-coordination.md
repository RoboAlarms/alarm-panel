# B032 — Developer OTA cancels while coordinating keypads

**Status:** Fixed and verified on a paired 7-inch master and 10-inch keypad (BL-048).

A signed developer upload verifies successfully, but installing it on a master with a keypad
returns to idle without booting the candidate. Normal release OTA uses the same coordination
planner and waits correctly.

`try_restart()` treated every refusal from `restart_allowed()` as a failed developer install.
The planner deliberately holds a newly staged version while it prepares its keypads; this
ordinary hold therefore canceled the very update the planner was preparing.

The restart gate now reports whether it is waiting only for coordination. Developer installs
wait in that case, while stops, cancellation and an unsafe alarm state still cancel. The alarm
state is checked before the coordination hold. The existing announcement, final alarm-task gate,
signature verification and probation requirements are unchanged.

Validation: compile the actual service gate with mocked inputs for all 16 combinations of
stop, cancel, system idle and coordination hold (`tools/tests/test_ota_restart_gate.py`). Both
panel firmware builds pass. A signed developer upload now waits while the keypad downloads its
matching image; both panels then restart and confirm their images after probation. Live checks
also passed code-free selection, locked-policy refusal, authorized alarm-command enforcement,
30-second idle return, Never, default reassignment and policy revocation.

Live partition verification also exposed a stale keypad-side opcode allowlist: the master
accepted GOTO/ACTIVITY, but `plink_keypad_command()` rejected them locally. It now forwards
both operations. A regression test exercises the real keypad encoder/transport with empty codes
and verifies the received operation and target partition, complementing master policy tests.


The partition switcher exposed a separate layout gap: the arming pane reserved a trailing flex
gap before hidden sibling panels. Spacing now belongs to visible banners, so the Night tile
and switcher share their bottom edge. The focused UI preview asserts that alignment.
