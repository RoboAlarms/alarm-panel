# 36 · Z-Wave: running the network

Part of [docs/features/36](README.md). What the panel does as the one and only controller of its
Z-Wave network: what it stores, how a device is added, interviewed, kept track of and removed,
and what the installer can do to the network. Frame layouts are in
[serial-api.md](serial-api.md) (Serial API), [command-classes.md](command-classes.md) (CCs) and
[security.md](security.md) (S2/S0).

## 1. The panel's role

- The panel is the **primary controller, SUC and SIS** of a network it started itself (node 1).
  No other controller is ever part of it: the panel does not join other networks (no learn
  mode), does not include controllers (section 4, step 3), has no secondary or inclusion
  controllers, no bridge or virtual nodes and no associations of its own.
- Every device reports **to the panel only** (its lifeline, section 6). The panel does not set
  up direct associations between devices (no "sensor turns on a light"): automations belong
  to Home Assistant (docs/features/23) or the Devices screen (docs/features/24).
- One Z-Wave task on the panel owns the module and all network state (README "Design"). The
  UI, the console and the Panel Manager ask it to do things through its request queue; only
  one **network operation** (add, remove, replace, remove-failed, route repair, region change,
  restart, reset, backup, update) runs at a time. The UI shows which one and refuses a second.
- Network operations need **installer mode with every partition disarmed** (the rule for
  changing zones, docs/features/22 ZSRC-012; docs/features/14 access). Reading the network needs nothing.

## 2. What lives where

| Kept by | What | Where |
|---|---|---|
| the ZAC93 | home ID, node list, each node's protocol info, routes, its own node ID, the region | its own flash (the module's "NVM") |
| the panel | the six network keys and the home ID they belong to | NVS `zwave` / `keys` |
| the panel | the **node cache**: what the interview learned about each node (below) | NVS `zwave` / `nodes` |
| the panel | the SmartStart provisioning list (phase 2) | NVS `zwave` / `prov` |
| the panel | settings: region, default check-in interval, S0 allowed, jam threshold | NVS `zwave` / `cfg` |
| the panel | which zone uses which device input | the binding blob (docs/features/22), `zones` / `bind` |
| RAM only | SPAN table, sequence numbers, awake/asleep state, last heard, RSSI, queues | |

**Node cache** - a self-describing blob like the configuration and binding blobs (ADR-013):
`ZWND | format | compat | header length | payload length | CRC-32`, then items `field u8 |
group u8 | len u8 | value`; field `00` of group `01` starts a node (`node ID u16`). Unknown
fields and groups are kept and written back; zero values are left out; the frozen fixtures
decode forever (`zw_nodes_fixture_v1.h`; `_v2.h` adds field `12`). Per node:

| Field | Content |
|---|---|
| `01` generation | u16, the panel's count of inclusions, fixed at inclusion (zones.md "Addresses") |
| `02` protocol | flags (bit 0 listening, bit 1 FLiRS, bit 2 routing, bit 3 Long Range), basic, generic, specific class (4 bytes) |
| `03` CCs | CC, version, flags (bit 0 supported, bit 1 secure) triples |
| `04` security | granted classes mask (security.md §1 bits), `FF` = not yet known |
| `05` DSK | 16 bytes (S2 nodes) |
| `06` identity | manufacturer, product type, product ID (6 bytes) |
| `07` Z-Wave Plus | role type, node type, icons |
| `08` wake-up | interval set, min, max, step (u32 each), notify node ID |
| `09` lifeline | group, set by the panel (flag) |
| `0A` endpoint | endpoint, generic/specific class, CCs (repeated per endpoint) |
| `0B` notification | endpoint, type, supported event mask (repeated) |
| `0C` binary sensor | endpoint, supported type mask (repeated per endpoint) |
| `0D` multilevel | endpoint, supported sensor type mask (repeated per endpoint) |
| `0E` interview | the last completed interview step (section 5) |
| `0F` quirk | the quirks table row that matched (zones.md), by model IDs: manufacturer, product type, product ID (6 bytes, as `06`) |
| `10` push/pull | endpoint, notification mode (0 pull, 1 push) (repeated per endpoint) |
| `12` alarm sensor | endpoint, supported Alarm Sensor type mask, bit t = type t (repeated per endpoint); an empty mask: asked, no types (the device did not answer) |

Limits of this firmware (`zw_nodes.h`): 64 nodes, 32 CCs per node, 4 endpoints (the device
and 3), 12 CCs and 8 notification types per endpoint, 4-byte masks, 128 bytes of unknown items
per node and 512 for the cache. An item beyond these limits (a later firmware may write one)
is kept raw like an unknown field and written back unchanged, and the node keeps everything
else: a rollback never loses a node's generation. Only damage refuses the blob: magic, CRC,
header lengths, an item past the end, a node field before a node start, a node twice, a length
that contradicts the field's own structure.

Written when an interview step completes (a set-up device read again: once, when the new
reading is complete, section 5), when a device is added, removed or replaced, and
never on reports (no flash wear from sensors). A node's battery, last-heard time and state live
in RAM; the zone source asks every sensor for fresh state instead (docs/features/22 ZSRC-015).

**At every start** (serial-api.md §5, step 19) the panel compares:

| Module | Panel | Meaning | Action |
|---|---|---|---|
| home ID = stored | keys for it | normal | reconcile: nodes the module lists but the cache lacks are interviewed; nodes in the cache the module no longer lists are forgotten (their zones show CHECK, "sensor removed") |
| home ID differs, no nodes | keys and a cache for the old home ID | the module was replaced or reset | Z-Wave page: "The Z-Wave radio was replaced or reset: add its N devices again." The old cache is kept until the installer adds devices or resets (so a mistaken swap can be undone) |
| any home ID | no keys at all | first start, or keys lost | if the module has no nodes: generate keys for this home ID, store them. If it has nodes: **use none of them** - the page says "This Z-Wave radio belongs to another system: reset it to use it here" |
| home ID differs, nodes | keys for another home ID | a module from another system | as above |

**Decided while building it (Z5, `zw_net`):** the generation counter is stored with the node
cache (the owner gets it with every cache write) and handed back at start; on loading, it is
raised to the highest generation any stored record carries, so it never goes back. "At every
start" means every time the link comes up, including after the module restarted by itself:
the node list is read again and reconciled. In the "replaced" row the old keys are never
used again: the first device added to the new network gets fresh keys (from the panel's
random source). The cache is also written when a device reports a changed wake-up interval.
A Version Report is not kept in the cache (it is information for the device page).

## 3. States of a device

| State | Meaning | Changed by |
|---|---|---|
| interviewing | added, questions still open | section 5 |
| ready | interview complete, inputs known | |
| awake / asleep | sleeping devices: listening now or not | Wake Up Notification, NIF, or a Get other than a Nonce Get (awake: §7 "Waking"); No More Information sent, NoAck (asleep) |
| alive / dead | listening devices | any ACK or frame (alive); 2 failed pings (dead) |
| unreachable | the zone source told the hub it cannot reach it | section 8 |
| gone | removed, reset locally, or missing from the module | section 10 |

## 4. Adding a device (classic inclusion)

Started from "Add a device" ([screens.md](screens.md)). The panel asks nothing of other nodes
while it runs.

1. **Listen.** `AddNodeToNetwork C1` (any, high power, network-wide). Expect status `01` Ready
   (10 s, else "The Z-Wave radio did not start listening"). The UI says "Put the device in
   pairing mode" and counts down **60 s** (a new tap on "Keep listening" restarts it). The
   window ends with `AddNodeToNetwork C5` (stop).
2. **Found.** Status `02` NodeFound: the countdown stops; the protocol now finishes by itself.
   Expect `03` AddingEndNode with the node's ID and NIF within 60 s.
3. **Controllers are refused:** status `04` AddingController → stop (`C5`) at once and report
   "That is a controller, not a sensor". If the protocol completes anyway, the node is kept
   with no keys, marked "another controller: remove it" (section 10).
4. **Protocol done.** Expect `05` ProtocolDone within `76 s + 217 ms x listening nodes +
   3.5 s x FLiRS nodes` (neighbour discovery). Then `AddNodeToNetwork C5` with a callback:
   expect `06` Done carrying the node ID; then `C5` once more with callback ID 0. Refuse node
   IDs `FF`/`FFF` (broadcast) and IDs already in the cache ("already added").
5. **Remember it** - node cache entry, next **generation** number, state "interviewing". The
   window is closed: one device per window.
6. **SUC return route** (classic, not Long Range): `AssignSUCReturnRoute(node)` so the device
   knows how to reach the panel.
7. **Security** (security.md): S2 if the NIF lists `9F` (or the device was included by
   SmartStart); else S0 if it lists only `98` **and** "Allow S0" is on; else none. A failed or
   refused S2 exchange leaves the device in the network without security (section 4.1).
8. **Interview** (section 5). The zone picker offers the device's inputs as soon as the input
   steps are done.

Any failure closes the window, sends `C5` and tells the installer what happened in words
(screens.md "Add: results"). Status `07` Failed at any time: "The device did not finish
joining. Remove it (it may still think it belongs to a network) and add it again."

**The radio restarts mid-window.** A restart closes the module's window: by itself (an unasked
`SerialAPIStarted`, serial-api.md §2), by serial-api.md §7's soft reset (a module that stopped
answering, a jammed radio's reset) or by a new start-up. Before the device has joined (steps 1-3, step 4 until `06` Done; a replacement once `ReplaceFailedNode` went, section 10) the panel ends the
operation at once with its own result, "radio restarted" (`ZW_INCL_RADIO_RESTARTED`), instead of
counting down a window that no longer exists and ending "nothing joined" 60 s later; the device's
button does nothing until a new window. The `C5` goes after the restart, so a start still queued
behind it is undone. A restart that ends the `AddNodeToNetwork` still waiting for its report is
the same. A restart between `05` ProtocolDone and `06` Done ends it the same way, as Z-Wave JS's
`performInclusion` does when `finishInclusion` gets no Done: no node added, no key exchange. The
module may already hold the device; the reconcile at the next link up (section 2) then takes it
in without S2 keys, as Z-Wave JS finds such a node at its next start, and the installer can
remove it and add it again. From step 5 on (the route, the key exchange) a restart changes nothing: the S2 keys
and SPANs stay, and the exchange carries on. A replacement whose check (pings, `IsFailedNode`) is
still running has sent nothing to the radio yet and goes on.

### 4.1 Security results

| Result | Zones | Words |
|---|---|---|
| S2 Access Control / Authenticated | trusted (ZSRC_CAP_AUTH, life safety) | "Secure" |
| S2 Unauthenticated | trusted: encrypted, but its key exchange could be intercepted by someone listening while it was added | "Secure (no PIN)" |
| S0 | not trusted for life safety; encrypted | "Older security" |
| none, device has no security | not trusted: "Its signal can be copied" | "Not encrypted" |
| none, S2 exchange failed | as above, amber | "Added without encryption: remove it and add it again" |
| S0 although it lists S2 | as none, amber | "Security was downgraded: remove it and add it again" |

**Decided while building it (Z4, `zw_incl`):** after the stop, `Done` is awaited 10 s;
AssignSUCReturnRoute's callback 30 s, and a route that fails does not stop the join. A
controller that joins is not removed by the panel (removing it needs the device's own
button): the panel stops at once and reports it with its node ID for the installer. Keeping
other traffic quiet during the exchange is the owner's job. S0 is never granted in phase 1, so
a node that lists Security 2 but asks for S0 only is reported as downgraded.

### 4.2 SmartStart (phase 2)

A device with a SmartStart QR code (or its 40-digit DSK) can be **pre-registered**; it then joins
by itself when powered, with S2 and no button presses. Required for Long Range.

- **Provisioning list** (NVS `prov`, at most 32 entries): DSK, granted classes (default: what
  the QR code requests), protocol (classic / Long Range), a name, status (active / inactive),
  failure count.
- QR code: a string of digits starting `90`, at least 52 long:

  | Offset | Digits | Field |
  |---|---|---|
  | 0 | 2 | `90` (lead-in) |
  | 2 | 2 | version: `00` S2 only, `01` SmartStart |
  | 4 | 5 | checksum: the first 2 bytes of SHA-1 over the digits from offset 9 to the end, as a 5-digit number |
  | 9 | 3 | requested keys (security.md §1 mask) as a 3-digit number |
  | 12 | 40 | DSK: 8 x 5 digits |
  | 52 | ... | TLV blocks: `type x 2 + critical (2 digits)`, `length (2 digits)`, data |

  TLV types: `00` product type (`generic x 256 + specific` 5 digits, installer icon 5 digits),
  `01` product ID (manufacturer, product type, product ID, application version - 5 digits each),
  `02` max inclusion interval (`n x 128 s`, 2 digits), `03` UUID16 (`format` 2 digits + 8 x 5
  digits), `04` supported protocols (bit 0 classic, bit 1 Long Range). Unknown non-critical
  blocks are skipped; unknown critical ones refuse the code. Product type and product ID are
  required. The checksum must match (whitespace removed first).
- The panel listens with `AddNodeToNetwork 49` (SmartStart listen, no callback) whenever the
  list has an active entry, no other network operation runs and every partition is disarmed.
- `ApplicationUpdate 85`/`87` (a node asks to join): find the active entry whose DSK gives this
  **NWI home ID** (security.md §4) and whose protocol matches (`87` = Long Range); none → ignore.
  Otherwise `AddNodeToNetwork 08` (+ `20` for Long Range) with the NWI and Auth home IDs from the
  DSK, then steps 2-8 above. S2 is forced even if the NIF does not list it; the classes granted
  are the entry's, the PIN is the DSK's first group, and the device's key must match the rest of
  the DSK.
- A SmartStart inclusion whose S2 exchange fails is **removed again** (`RemoveFailedNode`) - the
  device goes back to asking. After 5 failures the entry turns inactive and the Z-Wave page says
  so.
- Excluding a SmartStart device offers "Forget its code too" (else it would rejoin).

**Decided while building it (Z10, `zw_qr`, `zw_prov`, `zw_incl_smartstart`, `zw_net_smart.c`):**
- The checksum is SHA-1 over the ASCII digits from offset 9 (checked against a label's code with
  an independent SHA-1). A code of exactly 40 digits (spaces and dashes allowed) is a typed DSK;
  anything else is read as a QR code. An S2-only code (version 00) can't be registered: its
  device is added with "Add a device" and its PIN.
- An entry from a code grants what the code requested (S2 keys only; a code asking for none is
  refused); a typed DSK may be granted every S2 key - the panel grants only what the device asks
  for. The installer may narrow it on the entry's page, never to none. Long Range only when the
  code offers nothing else (phase 3 adds the choice).
- The panel listens (AddNode `49`, no callback) only while an entry is active and waiting, no
  network operation runs, the link is up and checked, the radio is ours, and the owner says every
  partition is disarmed; any other operation turns it off first (`AddNode` stop, no callback),
  and it comes back when nothing runs. A request heard while it should not listen is ignored:
  the device asks again.
- The join is a network operation of its own: no page follows it, it is logged as added by the
  panel (user 0) - only when it got its keys. Besides the forced S2, the device's public key must
  be its entry's DSK (bytes 2-15, and 0-1 when no PIN is asked for): a device that answers with
  another key fails (`ZW_INCL_S2_WRONG_DEVICE`), is removed as failed without a "left the
  network" log, and counts as one of its entry's 5 failures.
- A joined device's entry keeps its node and generation. Removed by hand (exclusion or "reset,
  broken or gone") its code is forgotten too, and the removal's page may keep it; a device that
  left by itself (reset locally, missing) leaves its entry waiting, so it joins again. Registering
  the DSK of a device already in the network marks the entry joined. The network reset empties
  the list.

## 5. The interview

What the panel asks a new device, in order. Each step waits for its report for **1 s + the
round trip** (serial-api.md 6.12), is tried 3 times (each try is one transmission: a question
goes at most 3 times in all, ZW-037), and is recorded (node cache `0E`) when complete, so a
restart continues where it stopped. The whole interview is tried up to 5 times; a device that
never answers stays "interviewing" and its page says why.

**When it gives up (ZW-029, docs/bugs/B015).** After its attempts the interview ends (STALLED)
and nothing runs it until a restart or the installer: the device is not usable (no inputs) and
its page says "Its setup stopped at step N of 16" - "it didn't answer" for a device that answers
at any time (listening or FLiRS: never "went to sleep"), "Wake it, then try again" for a
sleeping one - with **Try its setup again**, which continues the interview from the step it
stopped at (`zw_net_resume_interview`; `zwave setup <n>` on the console, `setup` on the
developer link). Only a sleeping device's own wake-up wait is "asleep". "Stop and finish later"
while adding leaves the interview running but reports "Setup not finished", never "Added". A
stopped interview does not keep the device busy, so a listening or FLiRS device is pinged and
becomes unreachable in the usual time (§8) - but its removal does not wait for that (§10).

| # | Step | Commands | Result kept |
|---|---|---|---|
| 1 | protocol info | `GetNodeProtocolInfo` (serial) | listening / FLiRS / classes |
| 2 | ping | No Operation (listening devices only) | alive |
| 3 | node information | from the inclusion (`AddingEndNode`); otherwise `RequestNodeInfo` (sleeping devices: at their next wake-up) | CC list |
| 4 | secure CCs | Security 2 Commands Supported Get at the highest class (S2 nodes); S0 Commands Supported Get (S0) | secure CC list |
| 5 | identity | Manufacturer Specific Get | model, quirks row |
| 6 | versions | Version Get; Version CC Get for every CC the panel uses (command-classes.md §2) that the node lists | CC versions |
| 7 | wake-up | Interval Capabilities Get (v2+), Interval Get, **Interval Set** (section 7) | interval |
| 8 | endpoints | Multi Channel End Point Get, Capability Get per endpoint; for S2 nodes the Commands Supported Get per endpoint (Multi Channel encapsulated) | endpoints |
| 9 | Z-Wave Plus | Z-Wave Plus Info Get | role, icons |
| 10 | lifeline | section 6 | lifeline set |
| 11 | notification | per endpoint: Supported Get; Event Supported Get per type (v3+); push/pull (below); current state: Get `00 · type · 00` per type; **Notification Set `type · FF`** per type (push nodes) | inputs |
| 12 | binary and alarm sensor | Binary Sensor: Supported Get (v2+); Get per type. Alarm Sensor: Supported Get (heard but no answer after its tries, or one that does not parse: no types, kept as an empty `12`; never delivered: nothing kept, asked at the next start) | inputs |
| 13 | multilevel sensor | Supported Sensor Get (v5+); Get air temperature | inputs |
| 14 | battery | Battery Get | level |
| 15 | quirks | Configuration Set per the quirks table (zones.md), with Supervision | |
| 16 | done | node cache written; the zone source builds the device's inputs (zones.md) | ready |

Push or pull (step 11): a node without Association CC is "pull" (the panel would have to poll:
such nodes are **not offered as zone inputs**, zones.md); with Association Group Information, the
node is push if a group issues Notification Report; otherwise enable a type and read it - status
`FF` means push, `00`/`FE` pull; if nothing answers, assume push.

**Alarm Sensor types after an older interview.** Step 12 asks Alarm Sensor's types
since 2026-10-01 (BL-085), folded into the step so field `0E` keeps its numbers. A device whose
interview finished before that has an end point listing Alarm Sensor and no field `12`: at the
next start (with every other node's interview) it is asked only that question, per such end
point - no other step again, field `0E` stays 16, its inputs stay offered, and READY follows so
the zone source reads the types. A sleeping device's question waits for its wake-up; meanwhile
its check-in can still be changed on its page (only the interview proper refuses it,
`zw_iv_setting_up`), and both go at the wake-up. The answer, or "none" when the device heard the
question and gave no usable answer after its tries, is stored as field `12`, so it is asked once
(`zw_iv_alarm_types_missing`). When no try reached the device (powered off, or no route while
everything goes out at start), nothing is written and no READY follows: it is asked again at the
next network check, which runs only at link up (a start or a module restart), so it never loops.
The same holds inside a full interview: an undelivered Alarm Sensor question leaves field `12`
missing, the interview still finishes, and the next start asks it.

**Reading a set-up device again (BL-152, ZW-029; Z-Wave JS's refreshInfo).** "Set it up again"
(`UI_ZW_REQ_SETUP` on a finished device), a device's software update and `zwave interview <n>`
read a finished device again from step 1 (`zw_iv_restart`). What is true:
- **The old facts are kept and used until the new reading is complete.** The answers go into a
  scratch record, never into the device's own: its record in RAM and in NVS, and so its inputs,
  zones, keypad, feedback, outputs and the bridge, keep the old, complete facts the whole time,
  whatever step the reading is at (an end point emptied at step 8 and filled again at steps 11-13
  is the scratch's, not the zone's). Nothing is stored before then.
- **The new reading replaces the old facts** in one step when it is complete (READY), and the
  node cache is stored once: a CC the device no longer reports is gone, an end point it no longer
  has is gone, end point 0's notification types are the new ones (the CC list and the end points
  are read fresh, node information included). The new node information and secure CC list also
  replace the old in the security layer then (`zw_s2_set_nif_ccs`, `zw_s2_set_secure_ccs` with
  the committed record's lists, taken as at a start: a CC now listed only in the NIF goes at the
  device's highest class, one no longer listed in the clear; a step 4 that went unanswered leaves
  no secure CC, so a CC only that list named goes in the clear at once - the same class as after
  the next start), as Z-Wave JS forgets the CCs and the new interview adds them again; its
  granted classes stay. Kept from the old record: what
  the reading does not ask - the device's number, generation (so its zone bindings), security
  classes and DSK (granted at its inclusion) - the raw items this build cannot hold, and an end
  point's Alarm Sensor types when the new reading's question never reached the device (one it
  heard and left unanswered stores "none").
- **A sleeper that supports Wake Up has its re-read wait for its wake-up** before it asks
  anything (Z-Wave JS waits "so they continue to function"): its Wake Up Notification, its node
  information, or a Get from it - what Z-Wave JS's `markAsAwake` counts. A report or an S2 Nonce
  Get starts nothing. One awake when asked starts at once; any other device (a sleeper without
  Wake Up included) starts at once too. While it waits it holds no scratch record and keeps the
  device awake no longer than any wake-up (No More Information goes), and the page says it is read
  at its next wake-up.
- **Re-reads share a small pool** of scratch records (`ZW_IV_REREADS`, 2, in `zw_iv_t`, which
  lives in the PSRAM panel state). A sleeper's re-read whose question waits for the device (it
  fell asleep mid-reading) gives its record back when another device needs one - a listening
  device's re-read, or a sleeper's that wakes while both are in use - and starts again from step 1
  at its next wake-up; so does one whose attempt fails. A listening device's re-read while both
  are in use by readings under way is refused (busy; `zwave interview <n>` says busy too); a
  software update's is tried again every 5 s until it runs, never dropped.
- **After a successful software update** the re-read starts as Z-Wave JS's does
  (`onNodeFirmwareUpdated`): after the time the device said its restart takes, plus a margin
  (none said: 5 s; under 20 s: +5 s; under 60 s: +10 s; else +30 s), and without waiting for a
  sleeper's wake-up (`refreshInfo({ waitForWakeup: false })`, `zw_net_interview_now`): it takes
  its scratch record then, and a sleeper's questions wait in its wake-up queue. A sleeper that wakes
  while both are in use by readings under way (none parked) waits for its next wake-up, and No
  More Information goes meanwhile.
- **A second request** for a re-read under way starts it again from step 1, keeping its scratch
  record, as Z-Wave JS's refreshInfo restarts a running interview; one waiting for its wake-up is
  left alone (Z-Wave JS ignores it). The touchscreen and the console say OK. A new request also
  forgets that the last re-read stopped.
- **A re-read that stops** (its attempts ran out, or the panel restarts) leaves the device on its
  old facts, READY, its zones supervised. The reading and the "stopped" note (`zw_iv_reread_stopped`)
  live in RAM only: a panel restart forgets both, and the device is READY on its old facts.
- **Three things are not kept back until READY:** step 4's secure CC list, when the device
  answers, goes to the security layer at once (it chooses the class each command goes at, and the
  device's own answer is the truth; an unanswered step 4 changes the security layer only at
  READY, above); a Wake Up Interval Report heard meanwhile updates the record, is stored as at any time
  (§7), and is kept in the scratch record too, so READY keeps it (step 7's own Set, later, still
  wins); and a check-in change that was waiting when the reading started, once the device takes
  it (the scratch then reads the same interval).

Where it still differs from Z-Wave JS, and why:
- Z-Wave JS forgets a node's facts as the re-interview starts; the panel keeps using them until
  the new reading is complete, because the device must keep protecting the house meanwhile.
- Z-Wave JS re-reads any number of nodes at once; the panel at most 2 at a time (a scratch record
  is 696 bytes, and the portable core has no heap): a third listening device is told busy while
  two readings are under way, and a sleeper that wakes then waits for its next wake-up (one whose
  question waits for its device's wake-up gives its record back instead, above).
- A sleeper's re-read that gives its scratch record back (another device needed it, or the attempt
  failed) starts again from step 1 at its next wake-up, where Z-Wave JS carries on from the stage
  it reached: the half-read answers live only in the scratch record (the pool, above).
- Z-Wave JS keeps a partly-done interview across a restart; the panel drops a re-read, because the
  old facts it keeps are complete and in use - the installer starts it again.
- A re-read cannot refresh the raw items this build cannot hold: fields it does not know, and end
  points 4 and up, CC lists, notification types and firmware targets past its limits, written by
  a later firmware. They are kept and written back as the stored format promises, because this
  build cannot read them again, and dropping them would delete a later firmware's facts.
- "Try its setup again" on a device whose first interview has not finished continues it from the
  step its record's field 0E holds (one running is left as it is), where Z-Wave JS would start
  over: the stored steps are complete answers, and starting over would only cost a sleeper more
  wake-ups ("Set it up again", from step 1, is for a finished device).

**Sleeping devices during the interview:** after inclusion most battery sensors stay awake
for a short while (often 10-30 s), then sleep. Everything that did not get through is queued
(section 7) and continues at the next wake-up; the UI says "Wake the device to finish:
press its button" (the model's own words from the quirks table when known, e.g. Zooz ZSE41:
"click the Z-Wave button 4 times"). The panel does **not** send No More Information while an
interview is asking it something; a re-read waiting for the device's wake-up asks nothing yet,
so No More Information goes ("Set it up again", below).

**Decided while building it (Z5, `zw_iv`):** at most 2 listening devices have an interview
question outstanding at a time; sleeping devices' questions (sent while they are awake) and
questions the module answers itself do not wait their turn. A failed attempt restarts its
step; a Set and the Get that reads it back share one step's tries, and the first matching
report counts. Step 4 follows Z-Wave JS's Security 2 interview (`Security2CC.ts`, BL-152):
- **A node whose classes are known** is asked at its highest class: up to 3 queries, 500 ms apart,
  each sent once (a missing answer is not resent). Silence keeps its classes and the interview goes
  on ("Let's hope for the best"); it never makes a known node insecure.
- **A node whose classes are unknown** (its cache lost): if a class was already learnt by
  decrypting one of its frames (security.md §8), only that class is asked, the same way, and the
  classes granted then are recorded; the others stay undecided in RAM. Else it asks
  Unauthenticated, then Authenticated, then Access Control, once each, at that explicit class and
  without narrowing what the node may use meanwhile (its frames at its real class still decrypt),
  skipping a class whose key it lacks (left undecided), and records each one: an answer means
  granted (an empty list at a lower class too); silence - no answer, or no Nonce Report to its
  Nonce Get - or a frame the node cannot decrypt means refused.
- **A query that was never delivered** (the radio's transmit failure) records nothing: the
  attempt fails, as Z-Wave JS's `Controller_CallbackNOK` aborts its interview attempt. The next
  attempt starts step 4 again: a class granted by then is asked alone, as above.
- Only the **first non-empty** Commands Supported Report sets the secure CCs
  (`hasReceivedSecureCommands`); an empty one sets nothing, and a report outside the interview -
  unasked, even at the node's highest class - sets nothing either (Security2CC.ts stores the list
  only in its interview).
- A class learnt by decrypting **after** step 4 is added to the record and stored at once, as Z-Wave
  JS stores every `setSecurityClass` (`ZW_S2_EV_CLASS_LEARNT`).
- A node information frame **without Security 2** at step 3 refuses only the classes still
  undecided (Z-Wave JS `Node.ts`): a class learnt by decrypting is kept and recorded; with none
  learnt the node has no security.
- Step 8 asks each end point of an S2 node once; silence goes on to the next end point (Z-Wave JS
  then marks its CCs secure; the panel sends every end-point command at the node's highest class
  already). A transmit failure there fails the attempt too.

security.md "Where the panel still differs" lists what the record cannot hold.

**Decided while building it (hardening):** step 6 does not believe a Version CC Report of
version 0 for a CC the node is shown to be using (Z-Wave JS's
ignoreCCVersion0ForKnownSupportedCCs test). Those CCs are:
- Security 2 while the node holds an S2 class, S0 while it holds S0;
- Supervision once the node sent the panel a Supervision Get or answered one;
- Version itself, since the node just answered through it;
- Manufacturer Specific once step 5 read its report.

Such a CC is kept at version 1. Any other CC reported as 0 is "not supported" as before
(command-classes.md §3). Questions to an end point of an S2 node go at the node's highest class,
whatever the end point's own CC list says (security.md §8).

### Interview and check-in hardening (2026-09-26)

- Lifeline read-back accepts split Association and Multi Channel Association reports. Each
  part renews the report timeout; finding the panel in any part confirms the lifeline.
- Endpoint-only Notification, Binary Sensor and Multilevel Sensor classes have their versions
  queried at the root without advertising those classes as root inputs. Questions and bridge
  Gets use the endpoint's security class.
- After a failed or downgraded S2 bootstrap, ask for node information again before interviewing
  the insecure device. The NIF from its secure inclusion may omit all its sensor classes.
- Adding a device pauses other interviews, including questions queued for sleeping nodes.
  Existing answers finish normally; questions resume when inclusion ends.
- Application Status replies to interview and check-in Sets are honored as described in
  command-classes.md. Busy retries are bounded; Rejected Request never confirms a new interval.
  Check-in and quirk Supervision reports must match the current session.


## 6. The lifeline

The device's reports must reach the panel. Group 1 is the lifeline of every Z-Wave Plus device
(it lists `5E`); for others, Association Group Information group 1 with profile `0001`; else the
quirks table names the group; else none (the device is listed as "Does not report to the
panel", no inputs offered).

- Node has Multi Channel **and** endpoints **and** no endpoint supports (Multi Channel)
  Association itself **and** the group is within the Multi Channel Association group count:
  Multi Channel Association Set `group · 00 · 01 · 00` (node 1, endpoint 0), so endpoint reports
  arrive Multi Channel encapsulated.
- Otherwise: Association Set `group · 01`.
- Then read it back (Get) and record "lifeline set". Remove other destinations only when a quirk
  says the factory default sends somewhere unwanted.
- Some devices name the reports' group per CC (AGI Command List): the lifeline suffices for
  every sensor the panel supports; others are listed with their inputs missing.

**Decided while building it (Z5):** "no Association" means the node supports neither
Association (`85`) nor Multi Channel Association (`8E`).

## 7. Sleeping devices and the wake-up queue

- **Interval.** The panel sets each sleeping device's wake-up interval to the installer's
  "check-in" setting (default **1 hour**; choices 1, 4, 12 hours, per device on its page),
  clamped to the device's minimum and maximum and rounded to its step; destination = the panel.
  A device already configured with interval 0 ("wake only by hand") is left alone and gets no
  supervision (its zone page says "Not supervised: it never checks in"). The interval is also
  the device's **check-in**: section 8.
- **Queue.** Commands for an asleep device wait in its queue (at most 8; a newer command of the
  same kind replaces an older one; interview steps first). A SendData to a sleeping device that
  gets `NoAck` marks it asleep and requeues the command.
- **Waking** (the rule's one home; security.md §8 and serial-api.md point here). An asleep
  device listens now - send its queue - when Z-Wave JS's `markAsAwake` would take it from a
  frame (BL-152): its Wake Up Notification, its node information frame, or a Get from it other
  than Security's and Security 2's Nonce Get ("Nonces can be sent while asleep", `Node.ts`; a
  Commands Supported Get is a Get). A report is no wake-up, even with something queued for it.
  Z-Wave JS also marks a sleeper awake when a send to it succeeds and after a re-interview's wait
  for its wake-up; neither arises here, because the panel sends a sleeper nothing outside a
  wake-up but replies it owes (its queued commands wait, and a sleeper's re-read starts at once
  only while it is awake). Its third, a just-joined node whose protocol info shows it alive, the
  panel follows: the interview's protocol step counts the new node awake (`zw_health_wakeup`). A broadcast Wake Up
  Notification (a device with no wake-up destination yet) passes the same security check as a
  singlecast one (ZW-042, as Z-Wave JS): from a device without keys it is a wake-up like a
  singlecast one; from an S2 device, in the clear, it is dropped as a wrong level. A broadcast
  report from a device without keys reaches its zones like a singlecast one and is no wake-up. **250 ms** after the last command
  completes with nothing left, send **Wake Up No More Information** so it can sleep (battery).
  Not while its interview is asking it something (a re-read waiting for its wake-up asks nothing),
  not during route repair, not when its interval is 0.
- **Battery re-read.** A device with the Battery CC whose last level (or last ask) is a day old gets one
  Battery Get, by the bridge's poll (`zwave_zones`): a sleeper's waits in its queue for the next
  wake-up, a listening one's goes at once. One a day per device, none while a command for it waits;
  its report clears Low through the usual path (Z-Wave JS refreshes at 7 days). A device whose Get
  still waits (a sleeper that has not woken for a day) is not due again until it has gone.
- A wake-up that comes more than 5 minutes later than its interval: read the interval again
  (the device may have been reset or reconfigured). A changed answer is kept (the record, the
  stored cache, `zw_health`) and reaches its zones at once (§8), as a confirmed check-in of its own
  does: `ZW_NET_EV_CHECK_IN` with `took`, never during its interview (whose end describes it anyway).

**Decided while building it (Z5, `zw_health`, `zw_net`):** a newer queued command of the same
kind also replaces one already out, and the owner still hears how the old one ended. A
sleeper counts as awake (its queue goes) only as "Waking" above says (BL-152). With S2, No More Information goes out about 750 ms after the last command was
delivered: the 250 ms above start after S2's 500 ms wait for a Nonce Report (security.md §6),
or after a frame from the device that ends that wait sooner.

**Decided while building it (hardening):** a Supervision Report the panel owes a sleeping
device is a reply, not a command for its next wake-up. The owner sends it with `zw_net_send` to
answer the device's Supervision Get, and the device that sent the Get listens now (Z-Wave JS's
s0EncapsulationTwoNodes test, in S2 terms).
- A device not counted awake (the Report answers a report it supervised, no wake-up) gets the
  Report at once, straight from Security 2, and stays asleep: no No More Information follows, and
  its queue waits for its wake-up. Z-Wave JS sends it on its Immediate queue and leaves the node's
  status alone ("Do not update the node status when we only responded to a nonce/supervision
  request", `Driver.ts`). Counting it awake here sent No More Information to a device already
  back asleep; its unheard frame took the shared SPAN's next nonce, and the device's next report
  failed to decrypt ([B026](../../bugs/B026-zwave-sleeper-report-lost-after-reply.md)).
- A device awake (its wake-up, or a Get from it) gets the Report first in its queue under a kind of
  its own, so nothing replaces it and it replaces nothing; No More Information waits behind it.
- Its SendData goes in the reply class, ahead of node commands already queued for the module.
  Nonce Reports already went that way.
- A Report the device did not hear (`NoAck`) is not requeued: at its next wake-up it would mean
  nothing. It ends FAILED, and the owner hears so.

**The bridge's outbox** (`zwave_zones`, BL-152): the bridge gives `zw_net` one fresh-state Get
per device at a time and holds it until `zw_net` tells its end. For a sleeper that is its next
wake-up; a Get it did not hear (`NoAck`) waits for a later one. One at a time, because `zw_net`'s
queue keys a command by its first bytes: a sleeper's Notification Gets, or all its Multi Channel
ones, would replace each other there.
- The outbox has a slot for every device's Get (64, the zone source's devices) and 16 more that
  only answers take. 64 watched sleepers after a start still leave room for every answer.
- Answers (Supervision Reports, Time and the other command-classes.md §5 answers) are never
  held in the bridge behind a Get. Each goes to `zw_net_send` at the bridge's next poll, oldest
  first, and the reply rules above take it from there.
- A report from an asleep sleeper sends nothing of its queue (B026): its answer goes alone. During
  a wake-up a queued Get can still be out ahead of an answer; if that Get gets no ACK, the device
  counts as asleep again and the answer waits in `zw_net`'s queue for its next wake-up. That
  narrower gap is still open (Z-Wave JS sends Supervision Reports on its Immediate queue, ahead of
  a sleeper's wake-up messages).
- A Get that finds no room is asked again at a later poll (100 ms on), never skipped, and is not
  counted as dropped; it does not hold back the answers. An answer that finds no room, or that
  `zw_net` refuses, is counted (`replies_lost`), never dropped silently.

**A device's own check-in** (its page, "Changes when it next wakes up"; `zw_net_set_check_in`,
Z7): the installer's choice (10 min to 24 h; the page offers 1, 4 and 12 hours) waits in the
device's queue for its next wake-up; a newer choice replaces one not yet confirmed. It goes as
the interview's Interval Set does: clamped and rounded, the panel as destination, at its highest
class, with Supervision exactly when the interview's Sets have it (none for a model whose quirks
row says `no_supervision`, zones.md §8). Only when the device confirms it - delivered without
Supervision, else its Supervision Report "done" (or "no support", as the interview) - do its
record, the stored cache and §8's clock take the new interval; until then the old one counts,
the device still waking at it. A Set it did not hear waits for its next wake-up; "fail", no
Report within 1 s plus the round trip (No More Information waits for it), or a Set that cannot
go, end it with nothing changed, not sent again. The owner hears how it ended either way, and
keeps the wanted value itself: the interview asks it for it (step 7). Not for listening devices,
devices at interval 0, or ones still interviewing.

**What the panel does with it** (`zwz_panel`, `zwave_zones`, Z8): the value `zw_net` sets is
stored (`chk`), and the page says "Changes when it next wakes up" until the device's record has
it. One the device does not take is dropped: the page shows the interval the device kept, with
nothing to come, and nothing is logged (a later interview sets new devices' check-in). One not
confirmed before the panel restarts lived in `zw_net`'s memory only: it is set again at the first
check that finds the network ours (not when `zw_net` still carries it - only the module
restarted). A confirmed one moves the zones' grace (§8: `T + 10 min`) at once, without asking the
device's inputs again.

## 8. Supervision: is the device still there?

The zone source uses this to say "unreachable" (docs/features/22 ZSRC-007) and the hub turns that
into the zone's CHECK after the binding's grace.

| Device | Heard from | Declared unreachable | Grace (binding default) | CHECK after silence of |
|---|---|---|---|---|
| sleeping, interval `T` | every frame (wake-up, report, nonce request) | `T + 10 min` without any frame | `T + 10 min` | `2T + 20 min` (default T = 1 h: 2 h 20 min) |
| listening (mains) | every frame and every ACK | 2 failed pings: a No Operation is sent when nothing was heard for 15 min; each ping gets the normal 3 attempts; the second failure 15 min later | 30 min | about 1 h |
| interval 0 | - | never | - | never (not supervised; no life-safety use) |

Why two stages: after the panel restarts, or the module restarts, nothing heard before counts
(ZSRC-015) and a sleeping sensor cannot be asked - it speaks at its next wake-up, up to `T`
later. The grace of `T + 10 min` gives it that chance; only a sensor that also misses the next
one shows CHECK. The panel's default is strict, and the installer can loosen it per device (a
longer interval is longer battery life).

When a sleeper's interval changes - a check-in of its own it confirmed (§7), or the interval read
again after a late wake-up - its zones follow at once, not at the panel's next restart: the grace
becomes the new `T + 10 min`. Read again as 0, it no longer checks in: `zw_health` stops
supervising it and its zones are no longer supervised (no life-safety use, ZW-015) - never CHECK
for its silence, a CHECK already shown cleared, and its page says it never checks in. A device
read as 0 is not read again at its wake-ups (there is no interval to be late against): it is
supervised again only once an interview (a re-interview) finds a non-zero interval.

A report the hub gets unchanged counts as a check-in at most once a minute (ZSRC-006).
The module's own "failed node" list (`IsFailedNode`) is used only by the remove-failed flow.

**Built (BL-152, ZW-009): what counts as hearing from a device.** An accepted frame (security.md
§8 "Heard": the panel follows Z-Wave JS's `markAsAlive`, not its `lastSeen`; BL-157 is the open
question) and an ACK of the panel's own send, as a successful transmit report does in Z-Wave
JS. Proven by `test_zw_interview`
(`test_heard_only_accepted_frames`),
`test_zwave_zones` (`test_kex_and_commands_supported_get_heard`) and `test_zwz_fuzz`
(`test_bridge_*`).

## 9. Jamming (phase 2)

- **Background RSSI.** Every 5 s `GetBackgroundRSSI` (serial-api.md 6.20). Jammed when the
  **quietest** channel stays at or above the threshold (default **-60 dBm**, bench-tuned;
  `zwave jam <dBm>`) for **30 s** - one busy channel is ordinary traffic, all channels loud at
  once is not. A stale sample never proves a jam: a missing answer resets the timer.
- **Transmit failures.** A SendData that ends `Fail` with 0 transmit ticks (the module could not
  get the air) is one jam sample; 3 within 30 s count as jammed.
- **Device reports.** A Home Security "RF jamming detected" event from a device is logged and
  shown on its page (it is that device's view, not proof for the whole network).
- While jammed, the hub hears the source's link down at once (`zsrc_zw_jammed`, the hub's
  `link_now`: the detector already waited its 30 s, so the link's 90 s grace doesn't apply): one
  `sensor_radio` trouble, never a CHECK per zone. The source itself keeps its link: what still
  gets through counts - a door that opens is told to its zone, a supervised report is answered
  at once (its device would never send it again), a Time Get is answered. When the jam clears
  (30 s after the RSSI falls back) the hub hears the link up - if the real link is up - and
  every watched device is asked for its state afresh (frames may have been lost); what the
  devices said before and during the jam is kept, unlike Zigbee, which forgets state at a jam's
  end (ZB-006). A real link down (the module gone, a restart, a pulled radio, ZW-032) keeps its
  own rule: reports are not taken, its trouble follows the link's grace, and the link-up that
  follows forgets every device's state (ZSRC-015). The module gone or pulled also forgets the
  jam (nothing is known of the air without the radio); a module restart during a jam keeps it,
  so when that link returns the hub hears it down again at once, until the air clears.

**Decided while building it (Z10, `zw_jam`):** the threshold is kept in the stored settings
(`cfg`'s fourth byte, signed dBm, -100..-20; 00 - what older firmware writes - means the default).
Saturated channels count as loud, "below sensitivity" as quiet, "not available" not at all; a
reading with nothing usable is no reading. While jammed, a missing reading neither ends the jam
nor proves it. The jam clears when the noise has been below the threshold for 30 s and no burst
of transmit failures is that recent. At the jam's end the hub's CHECK rules are the source's
(ZSRC-015): the trouble ends with each sensor's next word (its first frame counts as its
check-in, however recent the last one was), as after a radio that stopped answering. With
jamming watched, the reading every 5 s is also what notices a pulled radio (the idle probe then
has nothing to do). The bench recordings replayed by the tests predate it and replay with it off.
A device's own report (Z10, `zwz_panel.c`): Notification v2+ `07`/`0C` with status on is kept
per device (the newest, 8 devices) for its page's "Interference" row and logged as
`zwave_jamming_reported` once an hour at most; it never counts toward the panel's own jam.

## 10. Removing a device

**Remove (exclusion)** - "Remove a device", works for any device, also one from another
network (it is reset either way):
1. `RemoveNodeFromNetwork C1`; expect `01` Ready (10 s); count down 60 s; `C5` ends it.
2. `02` NodeFound; `03` RemovingEndNode with the node ID (60 s); `06` Done (60 s); then
   `RemoveNodeFromNetwork C5` with callback 0.
3. Node ID 0: "Removed a device that belonged to another system: it can be added now."
   A known node: forget it (cache, provisioning entry if the installer chooses), its zones
   show CHECK "sensor removed" until rebound or deleted (their bindings are kept, docs/features/22
   ZSRC-010). The event is logged.
4. The radio restarts before `06` Done (as in section 4): the removal ends at once, "radio
   restarted" (`ZW_REMOVE_RADIO_RESTARTED`), with `C5` after the restart. Whether a device past
   RemovingEndNode left is the module's node list's to say: the check at the link up that follows
   reads it again (section 2). A failed device's removal is not a window and carries on.

**Device reset locally** (`5A 01` from a device): it left by itself. Mark it gone, wait 500 ms
(`ZW_NET_GONE_WAIT_MS`, as Z-Wave JS's DeviceResetLocallyCC.ts: a device still finishing its
notification would answer the first ping, which refuses the removal), ping it until the module
lists it as failed, then `RemoveFailedNode`.

**Remove a failed device** - for a broken, lost or reset device, or one that never finished
joining, which cannot take part in an exclusion: ping it up to 3 times 2 s apart; any answer
refuses the removal ("It answered, so it was kept"). Then `IsFailedNode`: only a node the module
lists as failed goes on to `RemoveFailedNode` (serial-api.md 6.18). The panel forgets the node
only after the module's `01` (removed): the module's own list is what is removed from, so no
ghost stays in the ZAC93 and no entry is dropped from the panel alone. Each ending is its own
result (ZW-029): removed (`01`); still answers (a ping answered, or the module's callback `00`);
refused (not listed as failed, the response a refusal bit, or the callback `02` "not removed");
timed out (no response or no callback in time: it may still be listed); failed (the radio went
away). It is offered on every device page, not only once the device is unreachable: a device
that never finished joining, or was reset by hand, would otherwise wait up to an hour (§8) or
forever (a stopped interview). **A sleeping device never answers a ping**, so the page warns
first: "A sleeping sensor can't answer that check: use the second way only if it was reset or
is really gone."

**Replace a failed device** (phase 2) - the new device takes the old one's place. It is checked
first, exactly as for "Remove a failed device": ping it up to 3 times 2 s apart (any answer
refuses it: "The old device answered"), then `IsFailedNode`. The module takes
`ReplaceFailedNode` only for a node on its failed list, and a node gets there only after a
SendData to it got no ACK - a sleeping sensor whose battery died, the most common case, was sent
nothing since its last check-in, so without the pings it would never be listed and could never
be replaced (Z-Wave JS pings first too). Then `ReplaceFailedNode(node)`; status `03` means
"include the replacement now" (the UI shows the pairing prompt); `04` done (`05` failed).
Then security and interview as for a new device. The node keeps its **generation**, so its
zones follow the new device without rebinding (zones.md "Addresses"), provided the new device
offers the same kind of input; otherwise the zone shows CHECK "the new sensor has no door
contact".

**Decided while building it (Z10, `zw_incl_replace`):** the request ends at the module's first
report ("ready", or why not) and the rest arrive as the module's own frames, as for AddNode - so
nothing else waits behind it for the whole window. "Ready" opens the usual 60 s window (no
"Keep listening": the module's window is its own); `zw_incl_stop` ends it with AddNode(stop).
**The check (BL-152):** `zw_net_replace_failed` takes the network operation for the whole
check (nothing else starts), then runs `zw_remove_check_failed` - remove-as-failed's pings and
`IsFailedNode` without `RemoveFailedNode` - and hands its end to `zw_incl_replace_checked`.
Meanwhile the pages say only that it is checking (the touchscreen "Checking the old device is
gone", the Panel Manager "Starting") and Stop: the radio isn't listening yet, so a button
pressed now would be lost, and the pairing prompt waits for "ready". Stop ends the check at
once (`STOPPED`, nothing sent). Each ending keeps its own result: a ping answered or the
report `00` "the node answered" is `STILL_ANSWERS` ("The old device answered");
`IsFailedNode` no or the response bit `08` (not in the failed list) is `NOT_FAILED` ("doesn't
count the old device as failed yet"); bit `10` is `BUSY` and the other bits `REFUSED` (both
"it could not be done"). When the new device joins, the old one's S2 state is dropped and the
new one starts with none (its node information isn't known, so KEX Get asks); its node record
starts again (classes, identity, security, interview) with the same node ID and generation,
and its old interview, supervision and check-in are forgotten. The pages follow it as an
addition (`replacing`): the same security question and PIN, "Replaced ... its zones follow
it".

**Nodes the module lists that the panel cannot use** (another controller, a device from a
restored backup without keys): listed on the Z-Wave page with "Remove it"; they feed no zone.

**Decided while building it (Z5, `zw_remove`):** a device that answers after Device Reset
Locally is checked with IsFailedNode up to 3 rounds before it is removed as failed;
RemoveFailedNode's callback is awaited 30 s, HardReset's 10 s. Asking twice before a network
reset is the caller's job (the service and the screens); the library does what it is told.

## 11. Route repair (phase 2)

Z-Wave routes around obstacles through mains-powered devices; routes go stale when devices
move. "Rebuild routes" on a device page, or for all devices on the Z-Wave page:
1. `RequestNodeNeighborUpdate(node)` (up to 5 tries);
2. `AssignSUCReturnRoute(node)`;
3. `DeleteReturnRoute(node)` (clears stale priority routes; up to 5 tries).
Sleeping devices wait for their wake-up (the page asks the installer to wake them); a device
known dead is pinged first and skipped if it does not answer. Long Range devices do not route
and are skipped. All devices: listening ones in order, sleeping ones as they wake, one at a
time; the page shows progress in words ("3 of 7 done"), changing at most once a second.

**Decided while building it (Z10, `zw_routes`):** it is a network operation of its own (adding,
removing, SmartStart wait). A neighbour update that the module reports failed, or that doesn't
end in time (76 s + 217 ms a listening node + 3.5 s a FLiRS node), is tried again - after 5 the
device counts as failed and the job goes on. A failed SUC return route is noted, not fatal; stale
return routes that can't be deleted after 5 tries leave the device done (its neighbours are new).
A sleeper is repaired when its Wake Up Notification comes - kept awake until done (its No More
Information held back). A device the panel counts unreachable is pinged first; silent, it is
skipped. The installer's Stop skips the rest; the radio going away ends it.

## 12. Region, power, reset

- **Region** (installer): the list comes from the module (`GetSupportedRegions`, serial-api.md
  6.7); the page names them in words ("United States", "United States with Long Range",
  "Europe"...). Changing it: `SetRFRegion`, soft reset, start-up again. Zooz sells ZAC93 variants
  per frequency: a US module cannot become a European one (the list only offers what the module
  supports), so the usual change is United States <-> United States Long Range. What a change does
  to the devices (ZW-045, `zw_region_change()` in zwave_host's `zw_region.h`, reported per offered
  region in `ui_zw_status_t.region_change` / `region_stop`): regions come in families, as Z-Wave
  JS groups them (`getSupportedRFRegions`: a Long Range region includes its classic one) - United
  States with United States Long Range (`01`, `09`, and `0A`, the US Long Range backup channel,
  kept in the US family although Z-Wave JS's `RFRegion` enum dropped it), Europe with Europe Long
  Range (`00`, `0B`, and `FF`, the default) - and every other region is its own. Within a family the classic band stays: every classic device keeps working, and the
  radio only restarts. Leaving a Long Range region for its classic one drops the Long Range
  devices (node IDs from 256; they exist only in a Long Range region). Another family is another
  band: every device stops until it is added again, and its zones show CHECK. A region the module
  never reported counts as another band. A change that stops devices names how many and takes the
  installer code once more, as the network reset does (screens.md §8); the Panel Manager shows the
  region but cannot change it.
- **Restart the radio** (installer, radio details, screens.md §8): soft reset and start-up again
  (serial-api.md §5) - as a region change, without SetRFRegion. A network operation (§1): one at
  a time, refused while another runs. With the radio not answering it means "try now": the
  start-up begins at once instead of at the next retry (serial-api.md §7). Devices, keys and the
  node cache stay; the check at the link up (§2) finds them. The zones keep their state and give
  their sensors their grace again (docs/features/22 ZSRC-015): no CHECK, no trouble when they
  check in.
- **Power**: left at the module's factory values (Zooz sets the legal limit). Phase 3 may add
  the setting (serial-api.md 6.7) with the legal limit as the maximum.

  **Decided while building it (Z11, `zw_net_power.c`):** nothing about power is read at
  start-up (the recorded start-ups stay as they are): the installer's page or `zwave power`
  asks for it - GetPowerlevel16Bit when the module lists it, else GetPowerlevel, and the Long
  Range maximum when the module lists 05. The legal limit is not known to the panel, so the
  first power it reads is kept as the cap (`cfg`'s bytes 8-12, absent in older blobs: no cap
  yet) and a change may only go down from it, and back up to it; a radio found replaced
  (§2) forgets it. Classic power -10 to +20 dBm in tenths (a signed byte without the 16-bit
  form), the Long Range maximum 10, 14 or 20 dBm (Z-Wave JS's limits). A change is a network
  operation: the Set goes with the calibration as read; one the module takes is followed by the
  start-up again (step 12's soft reset applies it) and a read once the link is up and checked; a
  refused one restarts nothing. Never automatic, never raised above what the radio came with.
- **Reset the Z-Wave network** (installer, disarmed, confirmed twice): `HardReset` (serial-api.md
  6.14). Every device loses the panel and must be reset and added again; new home ID; the panel
  generates new keys, clears the node cache and the provisioning list; zones bound to Z-Wave
  show CHECK. Logged.

## 13. Long Range (phase 3)

With region `09` (US LR) or `0B` (EU LR, ZAC93 firmware 2.10+), the module also speaks Z-Wave
Long Range: a star network (no routing) with ranges up to a mile outdoors, for detached garages,
gates and sheds.
- Node IDs `0100`..`0FA0` (256-4000); 16-bit node IDs on the Serial API (serial-api.md §5 step
  14); `GetLongRangeNodes` for the list.
- **SmartStart only**, S2 Authenticated or Access Control only, with the separate Long Range
  keys; AAD node IDs are 2 bytes (security.md §7).
- No SUC return routes, no route repair, no neighbour discovery; TX reports carry TX power and
  noise floor (shown on the device page as signal).
- Payload limit from `GetLongRangeMaximumPayloadSize`.

## 14. Updating the radio and backing it up (phase 3)

- **Firmware update** of the ZAC93 (serial-api.md §8): only a file the installer chose, whose
  `.gbl` header matches the ZAC93 (checked before anything is written), only while disarmed,
  with arming refused until the module answers again. Never automatic. The Z-Wave page shows
  the running firmware and says when Zooz firmware older than 2.20 (SDK < 7.22.1) is in use.
**Decided while building it (Z11, `zw_host.c` OTW, `zw_gbl.c`, `zwz_panel.c`, `zw_fwsvc.c`):**
- **Checking, as Z-Wave JS does:** the radio is asked about exactly like a device and like Z-Wave
  JS's `getAvailableFirmwareUpdates(ownNodeId)` - its manufacturer, product type and product ID
  from GetSerialApiCapabilities, its firmware version and SDK - through the same update-job code
  as a device (`zwfj_check_radio`, node 0). Only when the installer asks (touchscreen Radio
  details, Panel Manager Radio page, `zwave radio-update check`). The service needs no API key,
  only a descriptive User-Agent (the firmware-updates README, 2026-09-27).
- **The region asked** (2026-10-20): the installer's region when one was chosen, else the one the
  radio reports (`zwfj_region`). The service lists the ZAC93's files per region (US, Europe) and
  offers none of them to a request without one; before this the panel sent none unless the
  installer had chosen a region, and every ZAC93 read "doesn't list this radio yet".
- **The ZAC93's own files, built in** (2026-10-20, `zwfc_radio_builtin`): the service's ZAC93
  entry (`firmwares/zooz/ZAC93-V02.json`) matches only a radio already on 2.0-2.255, while Zooz's
  change log says 1.x radios take 2.00 and later. When the service answers but offers nothing, a
  radio that is exactly Zooz's ZAC93 (027A:0004:0611), older than the newest file, in the US or
  Europe region, is offered that file - Zooz's own URL and the SHA-256 of the service's entry,
  built into the firmware (2.30, SDK 7.24.2, about 135 KB). A newer Zooz release reaches the
  panel through the service, or through the list in a panel update. The installer's own file (an
  SD card, the Panel Manager's upload, `PUT /api/v1/zwave-radio`) stays the way for anything else.
- **On hardware** (2026-10-20, the 7" bench panel, a hardware-2 ZAC93 on J7): the radio on 1.10 /
  SDK 7.18.3 was offered 2.30 from the built-in list (the service offered nothing), downloaded,
  checked and installed over the wire in about 3 minutes (1079 blocks, no resend); it came back on
  2.30 / SDK 7.24.2 with its home ID, node ID, region and device, and the next check found it
  "up to date" through the service - so on 2.30 it still reports an identity the service's entry
  matches (Zooz's change log says 2.20 changed the version and product type the bootloader
  checks).
- **Installing:** the service's file is checked against its SHA-256; any file is checked as a
  whole Gecko bootloader file (its tag chain and the end tag's CRC-32) before the radio is
  touched. Then serial-api.md §8, as Z-Wave JS's `firmwareUpdateOTW`: the host takes the link
  down (reason "updating": the owner's requests fail as for any link down; the pages say "Radio ·
  Updating"), writes EnterBootloader, chooses the menu's "upload gbl", sends 128-byte XMODEM-CRC
  blocks (one NAKed or unanswered for 1 s is sent again, 3 sends at most, then CAN), EOT, waits
  for "Serial upload complete" and the menu, chooses "run" and waits 10 s for SerialAPIStarted;
  then the start-up from step 0. An abort (CAN) keeps the bootloader's reason ("error 0x18"); an
  error 0x2X starts the upload again (3 uploads), others end it. Outcomes follow Z-Wave JS's
  statuses (OK, timeout, retries, aborted, not supported) plus "didn't start" (the new firmware
  left the radio in its bootloader). A radio left in its bootloader can be updated again from
  there (the link down "bootloader", its menu heard). Devices, keys and the node cache stay: the
  check at the link up finds them.
- **Arming held** (CORE-ARM-013): from the start until the radio is back and ours, or 15 minutes
  after the update ended, whichever is first - a radio that never comes back must not stop
  arming for good (its link-down trouble says it instead). Arming requests meanwhile fail
  `held` ("Wait: a sensor radio is updating").
- **Logged** as `zwave_device_software` with the radio's own stages (arg2 3-5, device 0):
  started (by whom), updated, failed.
- **From an SD card** (2026-10-20, the "Z-Wave updates" follow-up of BL-037): the touchscreen's
  Radio details offer "Install from an SD card" in installer mode: the one `.gbl` at the card's top
  or in a folder named `zwave` is read by a worker task (never the UI's) into PSRAM and handed to
  the same `zwave_service_radio_file` as the Panel Manager's upload, with the same checks. No card,
  no file, two files or a file over 1 MB each say so in a sentence. When the radio's software is
  older than Zooz advises (`update_advised`, below 2.20) the page says to tap Check for an update;
  when a check finds nothing, it says the maker's own file can still go in from an SD card or the
  Panel Manager (both words approved by the owner 2026-10-20). Tried in the preview only (a card without a file).
- **Not yet:** the NVM backup below.

- **Backup** of the module's NVM (serial-api.md 6.22) to the SD card or the Panel Manager, with
  the panel's keys and node cache alongside in one encrypted file (docs/features/14 CFG-005 backups).
  Restoring is only supported onto the **same model with the same firmware**; moving a network
  to a different module or firmware needs a conversion of the NVM format the panel does not
  implement.

## 15. Logged events

New `alarm_event_type_t` values are **appended** (the event log is a stored contract) with
their sentences in `components/core/notify` (the single copy, docs/features/26):

| Event | Sentence (example) |
|---|---|
| Z-Wave device added | "Z-Wave device added · Zooz ZSE41 · secure" |
| Z-Wave device added without security | "Z-Wave device added without encryption · ..." |
| Z-Wave device removed | "Z-Wave device removed · Front door" |
| Z-Wave device gone (reset locally / missing) | "Z-Wave device left the network · Front door" |
| Z-Wave network reset | "Z-Wave network reset · by Installer" |
| Z-Wave radio restarted by itself (the first at once, then once an hour at most with the count since: ZW-041) | "Z-Wave radio restarted · It restarted by itself 3 times; the panel set it up again" |

Zone troubles (CHECK, tamper, low battery) and the `sensor_radio` trouble use the existing
events; nothing new is needed for them.
