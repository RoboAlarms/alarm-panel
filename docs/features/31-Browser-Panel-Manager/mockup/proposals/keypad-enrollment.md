# Headless keypad enrollment — proposal

Status: draft for review with BL-048-keypads.js; not a production API or implemented feature.

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
mobile administration need their own designs; this proposal covers enrollment on an accessible,
configured main panel. It does not claim those other workflows are implemented.
