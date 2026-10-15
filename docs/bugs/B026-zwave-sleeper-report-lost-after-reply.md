# B026: a sleeping Z-Wave contact's open or closed comes late or not at all

**Status: fixed and proven on board 3 2026-10-04** (see "On the panel"). After answering a
sleeping device's supervised report, the panel counted the device awake and sent it Wake Up No
More Information; a device already back asleep never heard it, and the panel's S2 nonce for that
frame put the two out of step, so the device's next report failed to decrypt. The panel now
answers and leaves the device asleep, as Z-Wave JS does.

## Symptom
On board 3 (ZAC93, US), a battery outdoor door contact added with S2 Authenticated did not always
follow its magnet: with the magnet against it, the zone could still read open, and it caught up
minutes later or at the next movement.

## Evidence
`zwave trace on` for four minutes while the magnet was moved many times (2026-10-04, the
panel ran 3d6d367's content). The contact is a sleeping device (wake-up every 4 h) that sends each
change as a Home Security notification inside a Supervision Get, and sends a Wake Up Notification
at the end of a burst.
- 23 frames from it, at -62 to -68 dBm (no range problem). After each report the panel sent its
  Supervision Report, then, about 750 ms later, a 2-byte command: Wake Up No More Information.
- After a report with no Wake Up Notification before it, the contact was already asleep: four of
  those No More Information frames got no ACK (the module retried for 4-5 s). Each time, the
  contact's next report failed to decrypt (the panel's S2 failures went 9 -> 13, one per
  unanswered frame), and the panel sent a Nonce Report (SOS) only after its own failed send
  ended.
- The contact then sent the report again on a new SPAN: once 50 ms later, once only 21 s later,
  during which the zone read open with the magnet closed. The node's S2 counters for the whole
  session: 95 decrypted, 14 failures.
- The No More Information frames after a real Wake Up Notification were all ACKed.

## Cause
`zw_net_send` put a reply (a Supervision Report) to a sleeping device in its wake-up queue and,
because the device was not counted awake, called `zw_net_woke` so the reply went at once
(network.md §7, "Decided while building it (hardening)"). That made the device count as awake:
after the reply, `zw_health` scheduled No More Information (and anything else queued for the device
would have gone). Z-Wave JS sends a Supervision Report on its Immediate queue and leaves the node's
status alone ("Do not update the node status when we only responded to a nonce/supervision request",
`Driver.ts`); a report is no wake-up (`Node.ts`), so it sends No More Information only after a real
wake-up.

S2's nonces come from one generator both ends share (the SPAN), and each encrypted frame takes the
next one. The panel took a nonce for its No More Information; the contact never received the frame,
so it took the same nonce for its next report, which the panel could no longer decrypt.

## Fix
`zw_net_send` (`components/core/zwave_host/src/zw_net.c`): a reply to a sleeping device that is
not counted awake goes straight to Security 2 (in the reply class), and the device stays asleep: no
No More Information, and its queue waits for its wake-up. During a real wake-up a reply still goes
first in the device's queue. network.md §7 says so.

Tests (`components/core/zwave_zones/test/test_zwave_zones.c`):
- `test_supervised_report_is_no_wake_up` (ZW-011): a sleeping S2 contact's supervised reports are
  each answered with no No More Information after them; with the contact asleep 100 ms after it
  speaks, every report decrypts the first time; its wake-up still ends with No More Information.
  On the old code it fails both ways: five No More Information for four reports, and 5 of 6
  reports failing to decrypt.
- `test_sleeping_end_points_asked_one_at_a_time`, `test_held_get_does_not_hold_the_answer`,
  `test_noack_get_does_not_block_the_answer` and `test_forty_sleepers_answered_and_asked` now wake
  the device before its held Gets go: they had used a supervised report as the wake-up.

## Verify on a panel
On board 3 with the contact: `zwave trace on`, move the magnet 10 to 20 times, some fast. Expect
the zone to follow each change, no 14-byte frame to the contact after a report unless a Wake Up
Notification came first, and `zwave node <n>`'s S2 failures not to grow.

## On the panel
Board 3 with the fix (a dev build of 5ea6091 plus this change), 2026-10-04: `zwave trace on` for
four minutes while the magnet was moved well over 20 times, some fast, and the contact's
cover was opened and closed.
- 168 frames from the contact, at -59 to -81 dBm; every one decrypted the first time, and no
  Nonce Report went either way. Every frame the panel sent was ACKed.
- A Supervision Report for a report from the asleep contact went about 15 ms after it (about
  500 ms before the fix).
- The panel sent its 14-byte frames only after the contact's Wake Up Notifications: 13 wake-ups,
  one each (two after the first wake-up since the restart), all ACKed.
- The cover's tamper showed as zone 1 trouble and cleared each time (five of each).
- At the end, `zwave node 2`: S2 decrypted 174, failures 1, nonce reports 1. That one came before
  the trace began: the contact's first frame after the restart, which fails by design because
  the SPAN table lives in RAM only (security.md). Zone 1 read normal, input Clear, tamper no.

## Follow-up: the answers during a wake-up
Also seen in that trace: during a real wake-up each Supervision Report went about 500 ms after the
one before was delivered. A send to a device holds the next one for 500 ms after delivery
(`ZW_S2_RESEND_MS`), watching for a Nonce Report that would mean the device could not decrypt it.
This contact holds its next report until it hears the answer to the last, so during a wake-up a
change showed up to half a second late; none was lost. Z-Wave JS sends the Report on its
Immediate queue.

Fix (proven on board 3 2026-10-04, below): the device's next frame ends that watch when it shows the device
decoded the panel's frame: it decrypts on the nonce right after the panel's own, and its sequence
number follows the last one (`zw_s2.c` `decrypt_on_span`, `end_decoded`; security.md §6 "after
sending"). A frame that crossed the panel's shows nothing, and the send still ends only after the
device's command has been handed over. Waits that only the old 500 ms had covered:
- The removal after a device's Device Reset Locally Notification pinged the device at once, while
  a device still finishing its notification answers, which refuses the removal. It now starts
  500 ms later, as Z-Wave JS does (`ZW_NET_GONE_WAIT_MS`, network.md §10).
- Two interview tests whose sleepers were to fall asleep mid-reading after 600 ms: the reading now
  ends sooner, so they sleep after 300 ms.

Tests: `test_nodes_next_frame_ends_the_wait` (test_zw_s2.c, ZW-007) and
`test_awake_reports_answered_at_once` (ZW-011, a supervised wake-up and six supervised reports,
each answered within 200 ms). Both fail on the code before it.

On board 3 with it, 2026-10-04: `zwave trace on` while the magnet was moved well over 100 times,
many in quick runs (two reports once 60 ms apart). 131 frames from the contact. Each of 128
Supervision Reports went 15 to 23 ms after the report it answered (median 16 ms), during its
four wake-ups too; in the first check they took up to 500 ms. No 14-byte frame went outside a
wake-up, and No More Information went about 750 ms after a wake-up's last answer. Every frame the
panel sent was ACKed. `zwave node 2`: S2 decrypted 130, failures 1, the contact's first frame
after the restart (security.md: the SPAN table lives in RAM only); zone 1 normal, input Clear.
