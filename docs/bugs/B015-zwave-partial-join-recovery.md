# B015 · A Z-Wave device that never finished joining could not be removed

**Status (2026-09-27):** fixed in code, host-tested and checked in the PC preview; P4 v1 dev firmware
built. **Not flashed, not tried on the panel.** Physical acceptance: the steps under "Verify on
the panel" below.

## Symptom (physical panel, 2026-09-26)
A battery keypad woken by the panel's call (FLiRS) partly joined as node 2, generation 1:

- `zwave node 2`: FLiRS, security classes `00`, **no command classes**, interview "step 2 of 16"
  and not moving.
- Its page said "Secured: Not encrypted" and gave the generic "it went to sleep" instructions.
- "Remove this device" did not remove the entry. One attempt left `operation: remove - done
  (stopped)` on the console while the page said "Nothing was removed. Check the device's steps
  and try again." A later attempt did start a listening countdown.
- "Remove it: it's broken or gone" (removal as failed) was not offered at all.

## Causes (found in the code; reproduced in the host tests and the preview)
1. **The recovery was hidden.** The device page offered the removal as failed only when the
   device was `unreachable`. A stalled interview keeps no watch on the device until it is pinged
   (15 + 15 min of silence for a listening or FLiRS device), and a device reset by hand may keep
   ACKing - so the one way to clear a device that can't take part in an exclusion was missing.
2. **Any page change stopped any removal.** `stop_if_leaving()` (`ui_zwave.c`) sent STOP for
   every running add or remove whenever a Settings page was rebuilt or left and the add/remove
   page wasn't on top - including one started from the console (`zwave remove`) while the
   touchscreen showed, say, the device page (which rebuilds itself when the device's state
   changes). The removal then ended "stopped".
3. **A stale result was shown as this visit's.** After such a stop the operation stayed
   `remove / done (stopped)`. `build_remove()` adopted any operation named REMOVE, so opening
   "Remove this device" again showed that old result - "Nothing was removed" - without opening a
   window; "Try again" then did (matches the observation). Every ending but "removed" also read
   "Nothing was removed. Check the device's steps".
4. **An empty node information was taken for "no Security 2".** `begin_security()`
   (`zw_incl.c`) skipped the key exchange whenever the AddingEndNode report's list lacked `9F` -
   also when the list was empty. The device joined "unencrypted" by default. A possible
   explanation of the physical keypad's `00`, not proven on hardware.
5. **Unknown security read "Not encrypted"**, and **every stalled interview read "went to
   sleep"** (`ZW_NET_EV_STALLED` set the same `asleep` flag as a sleeping device's wake-up wait).
6. With them (review of the same flows): "Stop and finish later" reported "Added" while the
   device was not set up; FLiRS devices showed "Mains powered" (`listening` stood for both);
   stopping before any device joined said "The device stopped the secure connection"; the PIN
   was accepted up to 99999 and then cut to 16 bits; a keypad's feedback could be set only from
   the console.

## Fix (docs/features/36 ZW-029; screens.md §4, §6, §7; network.md §5, §10; security.md §5)
- Every device page offers both ways to remove it, explained: **Remove this device** (the
  device takes part) and **Remove it: it's reset, broken or gone** (the radio pings it, checks
  its failed list and only then removes it - `zw_remove` unchanged in that, so no ghost is left in
  the ZAC93 and a device that answers is kept). Outcomes are distinct and stay on the page:
  removed, still answers, refused (`ZW_REMOVE_REFUSED`: a refusal bit, callback `02`, or not
  listed as failed), timed out (`ZW_REMOVE_TIMEOUT` now also for RemoveFailedNode), failed.
- The add and remove pages stop and clear only the operation they started (`s_owner`); each
  visit to the remove page opens its own window; each ending has its own words.
- An empty node information sends KEX Get anyway; no answer ends as `ZW_INCL_S2_NO_INFO`, and the
  pages say "Not confirmed" (`UI_ZW_SEC_UNCONFIRMED`) until the device's list is read;
  `UI_ZW_SEC_NONE_CAPABLE` for a device that lists S2 but has no key.
- A stalled interview is `UI_ZW_DEV_SETUP_STOPPED` with **Try its setup again**
  (`UI_ZW_REQ_SETUP`, `zw_net_resume_interview`); only a sleeping device's wait is "asleep".
- "Stop and finish later" is `UI_ZW_RES_SETUP_PAUSED`; the power source is `ui_zw_power_t`, never
  guessed from the radio; PINs above 65535 are refused in the UI, the console, the developer link
  and `panelctl`; a keypad's **Sounds and lights** is set on its page through the persisted
  output assignment (no incoming control).
- Console: `zwave setup <n>`, `zwave remove-failed <n>`; developer link: `setup`, and the device
  fields `flirs`, `power`, `setup_done`, `setup_total` (docs/features/30).

## Verified (software only)
- Host: `test_zwz_panel` (the partial join end to end: empty node information, silent device,
  NO_INFO, not confirmed, stalled without "asleep", SETUP_STOPPED, SETUP_PAUSED, retry, removal
  as failed out of the radio's list; every removal-as-failed outcome; PIN 00000/65535/65536/
  99999; the device page's feedback fit), `test_zw_inclusion` (`test_empty_node_information`),
  `test_zw_feedback` (`test_fit_and_slot_choice`). Reverting the NIF fix or the SETUP_PAUSED fix
  makes three of them fail (checked).
- PC preview (`-ZWave`): shots 154-164, including the stale-result regression (stop, reopen:
  it listens again).

## Simulator usability passes (2026-09-27)
An end-user pass through the simulator (script mode in `host/ui_preview`) found 25 issues, then a
second pass rechecked them and tried the new device software screens (docs/features/36 ZW-033).
Fixed in software, checked by the host tests and the `-ZWave` preview:
- UX-001: declining encryption froze the add page (the step was overwritten after a cancel
  that ended within the call); a declined S2 device's node information is read again.
- UX-002/026/030: a radio pulled while idle is noticed within about 3 minutes (the module is
  asked for its identity after 60 s of silence) and stays "Not answering" while the panel keeps
  retrying it; its details say "Not known yet" instead of 0.0.
- UX-003/004/005/007/008/009: a device waiting for its security answer says so on its row and page;
  add and remove pages start only when opened, and a refused start says why on the page
  (UX-029); one box at a time; the same rows while a setup is tried again ("Tried again. Its
  setup stopped..."), on the device page and the add page; keypads named as such.
- UX-028: a page rebuilt by a state change keeps its scroll position.
- UX-031/032/033/035/036/037/040/042: the device software section re-reads its version in place
  and says "Updated: it runs version 1.20 now"; a stop asked as the update ended is withdrawn;
  a download can be stopped; a sleeper's wait shows minutes left; the device list says an update
  runs; removal is hidden during one; "It doesn't take software updates by radio".

A third pass (2026-09-27) confirmed those fixes and tried the new switch, relay and garage door
screens; fixed from it: the garage door's state is only ever its sensor's word ("Pressed to open:
waiting for its sensor", never "Opening" from the press alone, UX-043); a door that got there may
be pressed again at once, and a press during a move is explained on the page, not as a busy radio
(UX-044); the sensor picker offers only door, window, garage and plain contact zones, with their
sensors named and Z-Wave ones first (UX-045); what to do when the sensor never reported (UX-046);
the buttons stay in place (UX-047); a switch row says it switches (UX-048); removal warns about the
garage (UX-050); names and subtitles by kind (UX-038, UX-051, UX-056); wording (UX-049, 053-055).

A fourth pass confirmed the garage fixes and tried a dimmer; its leftovers are fixed too: the
radio's own failure said at once (UX-049), the removal question names the garage (UX-050), add
results and subtitles by kind (UX-056, UX-060), a dimmer at the top reads "Full" (UX-058), the
security question fits the screen (UX-059), a pending command says "Setting it..." (UX-061), the
picker doesn't repeat a zone's default name (UX-057). Left: UX-062 (a switch's list row shows no
state).

Open, by decision or for the maintainers:
- UX-027: after the radio comes back, "Sensor radio problem" stays until every sensor has
  reported again (a sleeper at its next check-in, up to its interval). That is ZSRC-015 as
  specified (what a sensor said before the break is not trusted); the Z-Wave page could say how
  many sensors are still to report. Decided 2026-09-28: the behaviour stays, and the
  Z-Wave page says how many sensors are still to report (BL-067).
- UX-034: nothing on Home says a zone's sensor is getting new software, and arming is allowed.
  The update needs installer mode to start, but runs on after it ends. Options: a notice on
  Home, or the zone shown as CHECK during the transfer. Decided 2026-09-28:
  the zone shows CHECK while its sensor's transfer runs - a sensor getting new software can't
  report, so the panel says so; arming treats it like any other fault (BL-067).
- UX-039 (the Activity entry names "Device 2"). UX-052 is fixed with words true of any device
  (its kind isn't known before its interview).

## Verify on the panel
Disarmed, installer mode. Recheck `zwave node 2` first: the entry may have changed since.
1. Open the device: its page shows "Security · Not confirmed" and "Its setup stopped at step 2 of
   16: it didn't answer" (not "went to sleep"), and both removal rows.
2. **Remove it: it's reset, broken or gone** → the page says "Checking...", then either goes back
   to the list (removed: `zwave nodes` no longer lists node 2) or says why it was kept.
3. If it says "It answered, so it was kept": the keypad still belongs to this network - use
   **Remove this device** and its pairing button instead (exclusion).
4. Then add it again with its PIN and confirm the page says "Secure (S2 ...)".
