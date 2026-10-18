# Keypad enrollment in Panel Manager

Status: owner approved 2026-10-18; implemented for normal and headless masters. Physical headless hardware acceptance is still pending.

The owner requires a main panel without a screen to be fully operable through Panel Manager,
with a future mobile app able to use the same controls. Keypad enrollment must therefore not
require a master touchscreen, console command or physical button on the main panel. This
changes the current LINK-004 requirement for installer confirmation at the master touchscreen;
its two-sided code comparison and physical-keypad confirmation remain required.

## Review flow

Partitions and devices > Add device > Keypad opens a popup with Cancel throughout.
1. Prepare the physical keypad in Keypad mode; configure Wi-Fi there if that transport is used.
2. The signed-in installer starts the master's two-minute pairing window in Panel Manager.
3. Start pairing on the physical keypad. Panel Manager shows the discovered peer and comparison code.
4. Compare the six digits on the browser and physical keypad. Confirm both, in either order.
   Rejecting the comparison or timing out before completion leaves no enrollment.
5. Only successful confirmation on both sides creates an enrolled device. Then configure its name,
   default partition and switching policy. Closing this setup keeps the enrolled keypad at the
   shown default partition, with switching locked; it does not undo successful pairing.

The mockup exposes explicitly labelled simulated keypad events for reviewing the flow. These
controls never belong in the production app. Scenario shortcuts show each state directly.
The headless example omits the local panel keypad; a panel with a display retains it.

Mismatch, timeout and concurrent attempts terminate without creating a device. Lost browser
connectivity is different: completion may have happened, so the UI says status is unknown and
reconciles with the panel before allowing another attempt. Never claim that a lost connection
means nothing was saved. Armed systems and full capacity refuse new enrollment.

## Shared service for Panel Manager and a future mobile app

The firmware owns the attempt, peer, comparison code, expiry, both confirmations and final
result. Both clients use the same authenticated service to start, read status, confirm, reject
and cancel. They must not duplicate the pairing cryptography or determine success locally.
Installer authority, applicable recent-password requirements, disarmed state and capacity are
checked by the server. Confirmations are bound to the exact attempt and peer; stale attempts
and conflicting controllers cannot confirm a replacement peer. Repeated requests are idempotent.

A reconnect reads the existing attempt/result and device list. Expiry and cancellation are
panel-owned, so a closed browser cannot leave pairing open indefinitely. Existing LINK-016
limits, collision handling and verified dual confirmation still apply. Persistent records are
written only after the protocol succeeds; settings are a subsequent authenticated operation.

Removal must revoke the pairing relationship as well as update the displayed device list.
Keypad supervision is automatic and is not exposed as a user-editable timer. No audio settings
are copied during enrollment or assignment.

Initial headless provisioning/account setup, recovery without connectivity, and the rest of
mobile administration need their own designs; this design covers enrollment on an accessible,
configured main panel. It does not claim those other workflows are implemented.

## Implemented API

`GET /api/v1/keypads` returns the enrolled keypads and (only on a display-equipped panel)
`local` policy. Installer access is required. It also returns the current installer's last
`enrollment` and whether a pairing window is busy.

`POST /api/v1/keypads` requires an installer password session, a recent password and a disarmed
system. `op: start` uses a client-generated random 32-hex `attempt` id so a lost response can be
retried. `confirm` carries that id, the displayed `code` and `sequence`, and boolean `allow`; `cancel` carries
the id. The service binds these to the initiating installer, rechecks the comparison on the
link task, retains the result for reconnects, and closes after two minutes even without a browser.
A restart clears the transient attempt; reconcile against the persistent device list.

`save` carries `device` (0 is the built-in keypad), `home`, `allowed`, `never`, `seconds` (1–3600),
and `name`. The built-in keypad label is an appended `ui/keypad_name` preference; existing
display and sound preference writers leave it alone. `remove` revokes the remote pairing. Neither operation changes
audio preferences. Expander/receiver entries use the existing revision-checked alarm device
configuration; those labels do not introduce new stored device kinds. Actual limits are four
partitions, sixteen shared device slots and eight link-capable keypad slots. The built-in keypad
is separate from those device slots.

Validation: the production service has normal/headless host tests; browser flow tests use a
mock physical peer. Initial headless provisioning and recovery remain outside this change.

Live acceptance on a display-equipped master and remote keypad verifies the authenticated API:
removal, matching peer codes, stale-sequence refusal, browser confirmation insufficient on its
own, keypad-side confirmation, reconnect after completion, and late cancel retaining success.
Keypad-side confirmation was driven through the physical keypad's development console; this
does not replace a person checking the comparison screens. Idempotent start, cancellation and
server-owned expiry with the browser disconnected also pass. Partition assignment, switching,
30-second idle return and Never were exercised on the physical keypad. Audio stays local.

The live save/readback check found that partition policy requests returned before publishing
their new state. They now use the same publish-before-reply path as other keypad changes, so
immediately reopening the settings does not show the preceding policy.
