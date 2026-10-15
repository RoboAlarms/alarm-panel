# zwave_host

**Tier:** core (portable C11) · **Features:** [36 Z-Wave sensors through a ZAC93](../../../docs/features/36-Z-Wave-Sensors-ZAC93/README.md) ·
**Tests:** [`test/`](test/) (9 suites: `zw_crypto`, `zw_transport`, `zw_startup`, `zw_s2`,
`zw_inclusion`, `zw_nodes`, `zw_health`, `zw_interview`, `zw_house`)

The panel's Z-Wave host: everything between the Zooz ZAC93's Serial API and the zone source,
as portable C proved on a PC. It grows milestone by milestone (docs/features/36 "The
milestones"). So far it holds:
- the **S2 and S0 cryptography** (Z1) of
  [security.md](../../../docs/features/36-Z-Wave-Sensors-ZAC93/security.md) §2-3, §6 and §10:
  AES-CMAC, AES-CCM (13-byte nonce, 8-byte tag), AES-OFB and S0's CBC-MAC, the CTR_DRBG
  (AES-128, no derivation function), the CKDF key derivations (network key expansion,
  temporary key extract and expand, NoncePRK, mixed entropy input), the SPAN nonce generator,
  the X25519 key pair and shared secret, and S0's keys;
- the **Serial API transport and request scheduler** (Z2) of
  [serial-api.md](../../../docs/features/36-Z-Wave-Sensors-ZAC93/serial-api.md) §1 "Sending", §2,
  §6.8, §6.12 and §7: one command at a time through ACK, response and callbacks, with the
  retransmissions, timeouts, SendData's and SoftReset's own rules, a priority queue and a
  dispatch gate;
- the **host** (Z2): the link to the module and what keeps it up - the start-up sequence
  (serial-api.md §5) with the module checks (§4) on the way, recovery and the retry schedule
  (§7), the module restarting by itself (§2) and its bootloader (§5 step 1, and a fall into it
  while up, §7);
- the **S2 security manager** (Z4) of security.md §6-9: the network keys, each node's classes,
  the SPANs, nonces and sequence numbers, encryption and decryption, the layers of
  command-classes.md §1 and the rule that a node counts only at its highest class (§8), the
  Commands Supported answers (§9); with the Security 2 command class's wire format;
- **adding a device** (Z4) of network.md §4 and security.md §5: the protocol inclusion, its
  60 s window, timeouts and refusals, and the S2 key exchange with the installer's consent and
  PIN;
- the **node cache** (Z3) of network.md §2: what the interview learned about each node, as a
  versioned blob, and the "At every start" reconciliation as a pure function;
- the **interview** (Z5) of network.md §5-6: the steps, the lifeline, push or pull, resumable
  across restarts and sleep;
- **wake-ups and supervision** (Z5, `zw_health`) of network.md §7-8: the check-in interval, the
  wake-up queue, No More Information, pings and the unreachable clocks;
- **removing devices** (Z5) of network.md §10: exclusion, a failed device, Device Reset Locally;
- **`zw_net`**, all of the above wired together - with the check at every start (§2), the node
  cache's persistence through the owner, region and network reset (§12): what the zwave task
  runs.

It never allocates and never blocks; time comes in as milliseconds. The frames, every Serial API
payload and the command classes are `components/core/zwave_proto`'s (`zw_frame.h`, `zw_api.h`,
`zw_cc.h`); the host writes no codec of its own except Security 2's, which is the security
manager's (`zw_s2_cc.h`).

## The crypto port
Everything is built on three primitives the platform lends through `zw_crypto_port_t`: AES-128
encryption of one block, X25519 and a random number generator. The modes, derivations and
nonces on top are portable code here, so what the panel computes is what the PC tests prove.
- **Firmware** (milestone Z8, `components/services/zwave_service`): PSA on mbedTLS 4 -
  `PSA_ALG_ECB_NO_PADDING`, `psa_raw_key_agreement(PSA_ALG_ECDH)` on a Montgomery-255 key,
  `psa_generate_random`.
- **PC**: `test/zw_host_crypto.c` - AES-128 (FIPS-197) and X25519 (RFC 7748 §5) written from the
  standards for this project, a seedable SplitMix64 random generator and one-call fault
  injection. The host tests use it, and the PC simulator will (Z6). Not for a panel: its random
  numbers are predictable on purpose and its AES looks bytes up in a table.
- **Its own check** (`zw_crypto_port_check()`, ZW-008): a port is proved before anything relies
  on it - AES-128 on FIPS-197 C.1, X25519 on RFC 7748 §5.2's first vector and on §6.1's Alice
  from the base point (how public keys are made), two 16-byte random draws neither all zero nor
  equal; it stops at the first failure. `zwave_service` runs it at every start before the network
  is set up and keeps the radio off when it fails.

## The transport and its tasks
On the panel two tasks share the work (docs/features/36 README "Tasks and data flow"):
- **`zw_link`** owns the UART. It feeds a `zw_rx_t`, writes ACK or NAK for the module's frames
  itself at once, and passes the `zwave` task the complete frames, the ACK/NAK/CAN bytes it
  hears and the bytes the parser discarded while idle (text: the host looks for the
  bootloader's menu in it).
- **`zwave`** runs `zw_host_t`, which owns the `zw_transport_t`: `zw_host_on_control()`,
  `_on_frame()` and `_on_text()` for that news, `zw_host_poll()` to act and write, and
  `zw_host_next_deadline()` to know how long it may sleep. Only poll writes, through the port's
  `write` (to `zw_link`). Every call first expires what is due, so the outcome does not depend
  on how late the task polls.

Rules the transport keeps (§1 "Sending"): one command in flight; ACK within 1600 ms of the
write; after NAK or silence the 2nd transmission 100 ms later and the 3rd 1100 ms after that,
after a CAN 100 ms (the module's own frame is handled first), 3 transmissions in all; the
response is TYPE 01 with the same FUNC (10 s, or the request's timeout), a callback TYPE 00
with the same FUNC and the command's callback ID first (1..255, never 0); a per-request check
refuses a response, a per-request classifier tells intermediate callbacks (each restarts the
callback timeout) from the final one; frames nobody waits for go to the owner and are never
mistaken for a command's. A lost ACK: the response that comes while the ACK is still awaited
(or the callback, for a command whose only answer is its callback) proves the module took the
command - it counts as the ACK, and the command is never sent again (a SendData sent twice
would reach the node twice). SendData (§6.12): "not queued" is sent again 500 ms later, 3
times at most; SendDataAbort 20 s after "queued", failure at 30 s. SoftReset (§6.8): finished
by its SerialAPIStarted within 5 s; a lone byte with low nibble 6 (one that arrives by itself,
never a byte inside text such as the bootloader's menu) counts as its ACK once. A
SerialAPIStarted nobody asked for fails the command in flight and is reported;
`zw_transport_halt()` ends the command in flight at once, with nothing more of it sent (the
module fell into its bootloader). "Module not
answering" (§7) - 3 transmissions without ACK, a response missing at its timeout, the SendData
callback missing at 30 s, "not queued" 3 times - is reported to the owner; after reporting it
the transport starts nothing more in that poll, so the owner acts (closes the gate) before a
queued request can reach a dead module.

The scheduler: a fixed queue (`ZW_TRANSPORT_QUEUE_LEN`, 16) that refuses when full; priority
classes, most urgent first: nonce/Supervision replies, module commands, node commands,
interview, background, first come first served within a class; requests carry a copy of their
payload, what they wait for, their timeouts and callbacks, an owner context and a tag (cancel
queued requests by tag). The **dispatch gate** (`zw_transport_set_gate()`) lets out only the
classes up to a given one in the order module commands, replies, node commands, interview,
background: start-up and recovery close it to module commands (even nonce replies wait: they
are SendData), link up opens it. `zw_transport_fail_queued()` ends the queued requests of
chosen classes with a status, in the order they would have gone. A request's `done()` - and
its response check and classifier - may submit and cancel requests (`done()` may queue the
next step), but not feed link news or poll: those calls are refused while an owner callback
runs.

## The host
`zw_host_t` holds the transport and the link's state (`zw_host_link()`: off, starting, up,
recovering, down). The transport's callbacks only take notes; `zw_host_poll()` acts on them,
so every owner callback runs from poll or a link call. The owner hears through
`zw_host_port_t`: `link_up` with the module's facts (`zw_module_facts_t`), `link_down` with a
reason, the §5 step and a detail (`zw_link_down_t`), `restarted` (the module restarted by
itself while up; reason `ZW_HOST_RESTART_BOOTLOADER`, FF, when it fell into its bootloader) and
`frame` (module frames no command waited for: ApplicationCommandHandler, ApplicationUpdate, the
add and remove reports after their first status...).

**Start-up** (§5), from `zw_host_start()` - at boot, after a region change or a network reset:
- step 0, with a reset line (`port.reset_line`): 20 ms low, then up to 3 s for SerialAPIStarted
  (none: on all the same);
- step 1: a NAK, then 500 ms of listening. Text with "Gecko Bootloader" is the bootloader's
  menu: the host writes the digit of its "N. run" line, waits up to 5 s for SerialAPIStarted
  and starts again at step 1 - twice at most, then the link goes down (bootloader);
- steps 2-18, one command at a time, each built and read with `zw_api.h`; what a command waits
  for comes from `zw_api_func_info()`, where its callback ID goes from
  `zw_api_callback_id_offset()`. Step 10 (SetRFRegion) only when the region wish differs from
  the module's region; the soft reset of step 12 puts it in effect, and SerialAPIStarted says
  whether the region has Long Range. Steps 13 (the Long Range node list) and 14's 16-bit node
  IDs only with Long Range wished and available, else 8-bit IDs. Step 17's SetSUCNodeId only
  when there is no SUC, the panel is the primary controller and no SIS is present. For its
  own node it sends callback ID 0 and completes on the response; no callback is emitted by
  the physical ZAC93 (B013).
  SerialAPISetup sub-commands the module does not list (step 7) are skipped; refusals of
  SetRFRegion, SetNodeIDType and SetTxStatusReport are accepted, and the facts say what is in
  effect;
- the §4 checks on the way: a function §6 marks required missing (step 2), an end node or a
  chip other than the 800 series (step 3), a library other than a static or bridge controller
  (step 4) take the link down, naming the step and what was wrong; an SDK before 7.22.1 (step
  5) only sets `update_note` in the facts ("Update the Z-Wave radio's software");
- a failing step (no answer, or an answer it cannot use) is sent twice more, then §7's soft
  reset; if it fails three times again after that, the link goes down (not answering, or a bad
  answer, at that step);
- step 19: the link is up, the gate opens and `port.link_up` runs - the hook for comparing the
  home ID with the stored network, reconciling the node list, interviews (Z5) and SmartStart
  (Z10).

**Recovery** (§7): when the module stops answering while up, the gate closes and a SoftReset
goes; its SerialAPIStarted within 5 s brings steps 13-19 again. Without it, the reset line (20
ms low, then 3 s), and with none wired or the module still silent, the link goes down. While
down the whole start-up runs again 30 s after each failure, every 5 minutes once 10 in a row
have failed; the count starts again when the link has been up. A module that starts by itself
while the link is down (SerialAPIStarted) is started at once.

**Restarts** (§2): a SerialAPIStarted nobody asked for while up fails the command in flight,
reaches the owner (`port.restarted`), closes the gate and counts as step 12's answer: steps
13-19 run again, then the gate opens. While starting, past step 12 the start-up resumes at step
13, before it the same command goes again; a fourth restart in one attempt takes the link down
(restarting). Bounds for one attempt: 2 bootloader escapes, 1 recovery, 3 restarts.

**The bootloader while up** (serial-api.md §7): the host keeps the newest 256 bytes of text
(cleared at link up and after each "run"). The moment they hold the whole menu - "Gecko
Bootloader" and an "N. run" line, in however many reads - the module has crashed into its
bootloader. The command in flight ends with `MODULE_RESTARTED` and nothing more of it goes (every
byte is a menu key there), the gate closes, `port.restarted` hears FF, and "run" goes at once; its
SerialAPIStarted brings the start-up again from step 1, with the start's bounds (two escapes,
then the link down, "bootloader"). `stats.bootloader_falls` counts these.

**The owner's requests** (`zw_host_submit()`) are refused as invalid in the module class and
with the host's tags (0x5A57xxxx), with `ZW_SUBMIT_LINK_DOWN` while the link is down or not
started, and with `ZW_SUBMIT_FULL` when only the queue's last slot is left: that slot is the
host's, so a recovery's soft reset always finds room. While the link is starting or recovering
they wait behind the gate; when it goes down every one of them ends with `ZW_TX_LINK_DOWN`.

A `zw_host_t` is 6.7 KB on the P4 (5.6 KB of it the transport's queue of 16 requests with their
payloads): static storage, not a task's stack.

## The security manager
`zw_s2_t` (`zw_s2.h`) runs S2 for the whole network. The owner gives it the six network keys at
`zw_s2_start()` (the firmware generates and stores them, Z8; each is expanded once, security.md
§3) and registers every node with its granted classes and its node information's CCs
(`zw_s2_add_node`). Per node, in RAM, sized for `ZW_S2_PEERS` (64): the classes and the highest
one, the SPAN of §6 (none, local EI, remote EI, running - with its class), the sequence numbers
both ways (the panel's starts at random; the peer's last one drops duplicates), the panel's last
sending nonce for crossed frames (500 ms), one Nonce Report waiting at most (no floods;
transmit options 05, one attempt). A restarted panel knows no SPAN: the first frame of each
sensor fails, the Nonce Report brings it again.

- **Sending** (`zw_s2_send`): the class from §8 (`ZW_SEC_AUTO`: the node's highest for a secure
  CC, in the clear for a CC of its NIF) or given (the temporary key, a §9 answer, the interview's
  probe of a node whose classes are not all known at an explicit undecided class - network.md §5
  step 4). With a SPAN
  of that class the next nonce; with the peer's receiver EI a new SPAN and its extension; else a
  Nonce Get first (and 1 s plus the round trip for its report). A Nonce Report within 500 ms of
  the delivery means it was not decoded: once more on a new SPAN, then `ZW_S2_NOT_DECODED`.
  When a new SPAN's extension and the command do not fit the module's frame (46 bytes), the
  SPAN goes ahead in a frame of its own. Sends to one node go one at a time, in order. Multi
  Channel is a secure CC of every node with an S2 class, so its end points' traffic is at its
  highest class both ways (`zw_s2_class_for_endpoint()`: the class of Multi Channel for an end
  point). A Supervision Report is a reply (`zw_s2_command_is_reply()`): its frames go in the
  transport's reply class, as Nonce Reports do (`zw_s2_frame_is_reply()` tells zw_net by tag).
- **Receiving** (`zw_s2_receive`): unknown nodes dropped; Nonce Get and Report answered and taken;
  a Message Encapsulation parsed with §7's extension rules, its duplicates dropped, decrypted
  into a separate buffer - a SPAN extension on the panel's receiver EI with the temporary key
  first (then the class being exchanged) while bootstrapping, else the node's classes, highest
  first - for a node whose classes are not known (`zw_s2_set_classes_unknown`), every class not
  refused whose key the panel holds, the one that decrypts marked granted and the others left
  undecided, as Z-Wave JS's `decryptSinglecast`; without one on the running SPAN with up to 5 nonces (1 for a multicast follow-up),
  after the crossing nonce. A failure sends a Nonce Report. Then every layer is unwrapped with
  `zw_cc_unwrap()`: Security 2 only outermost (an encapsulation or a nonce command inside
  another layer drops the frame), every other layer at most once, Transport Service (counted
  unsupported, never a wrong level) and S0 dropped, a CRC-16 frame whose checksum fails counted
  malformed; §8: a secure CC unencrypted or below the node's highest class drops the frame, the
  temporary key carries Security 2 commands only; received broadcast or multicast
  (`zw_s2_receive_group`), a Get-type command (`zw_s2_command_is_get`: Z-Wave JS's 147 "Get" class
  names) is discarded. A node information frame's CCs (`zw_s2_add_nif_ccs`) are only added, and
  the owner passes only the interview's (before its node-information step, as Z-Wave JS's
  `updateNodeInfo`). Each command left reaches `port.received`
  with its class, endpoint, Supervision session, CRC-16 and Multi Command marks.
- **§9**: a Commands Supported Get at the node's highest class gets 86 72 85 59 8E 5A, at a lower
  S2 class an empty list, in the clear nothing; a node's own report at its highest class
  becomes its secure CCs (until then: everything outside its NIF).
- One addition to §6: the receiver EI the panel sent stays good for one SPAN extension until
  one uses it, even after the peer's own Nonce Report - when both sides resync at once, both
  frames get through.

A simulated device runs the same manager in device mode (`config.panel` false: no §8 drops, no
§9 answers), with the panel as its only peer. `zw_s2_t` is 14.4 KB on the P4 (PSRAM).

## Adding a device
`zw_incl_t` (`zw_incl.h`) is network.md §4 and the including side of security.md §5.
`zw_incl_start(allowed, ...)` takes ZW-005's check as an argument - installer mode with every
partition disarmed is the service's and the UI's to know - and refuses when it is false or
another inclusion runs. Then: AddNode(any) with Ready within 10 s (a module that skips Ready
goes on at its first report); the 60 s window
(`zw_incl_keep_listening` restarts it, `zw_incl_stop` ends it); NodeFound, AddingEndNode within
60 s, ProtocolDone within 76 s + 217 ms per listening node + 3.5 s per FLiRS node; AddNode(stop)
with its Done, then once more without a callback; a controller stopped at once (reported with
its node ID for the owner to mark), Failed at any time, the broadcast node ID and one already in
the cache refused; the SUC return route (a failed one does not stop the device joining).

Then, when its NIF lists Security 2: KEX Get (TA1), the KEX Report's checks (echo, exactly
scheme 1, exactly Curve25519, no CSA, a key the panel has), the installer's grant within TAI1
(`zw_incl_grant`: a non-empty subset of the request, or `zw_incl_cancel`), KEX Set, the
device's Public Key Report (TA2), the panel's at once, the PIN within TAI2 when Authenticated or
Access Control was granted (`zw_incl_pin`; the event shows the DSK, groups 2-8 as on the
label), the temporary key, the device's KEX Set echo (TAI2 from the Public Key Report: it must
be the KEX Set with echo 1), the KEX Report echo, and per granted key Network Key Get (TA3),
Report, Verify under that key (TA4), Transfer End; last the device's Transfer End (TA5). A
second decryption failure on an exchanged nonce is the wrong PIN. Every failure sends a KEX
Fail (the panel's own timeouts and refusals too), deletes the temporary key and leaves the node
with no class: `ZW_INCL_EV_DONE` says added, how (§4.1: Access Control, Authenticated,
Unauthenticated, none, failed, downgraded) and why. Downgrades are detected: a node listing
Security 2 that asks for S0 only, a KEX Set echo that differs from the grant, the device
refusing the panel's KEX Report echo.

A node that has joined (Done, network.md §4 step 5) is announced at once (`ZW_INCL_EV_JOINED`,
before the route and the key exchange): the owner remembers it then. `ZW_INCL_FOREIGN_RADIO` is
zw_net's refusal on a radio of another system.

## The interview
`zw_iv_t` (`zw_iv.h`, the steps in `src/zw_iv_steps.c`) asks what network.md §5's table lists,
in order, and records each step in the node's record (field `0E`) the moment it completes, so a
restart continues where it stopped: protocol info (a controller ends it: "not usable"), a ping
of listening devices, the node information when the inclusion did not give it, Security 2
Commands Supported at the highest class (for a node whose classes are unknown - found in the
module at start, or the panel restarted during its key exchange - only a class learnt by
decrypting, or else a probe of every class; network.md §5 step 4 has its rules),
identity (and the owner's quirks row), versions of the CCs the panel uses, wake-up (capabilities,
interval, then Interval Set to the owner's check-in within the device's limits, the panel as
destination; interval 0 left alone), end points (capabilities, and their own Commands Supported
through Multi Channel), Z-Wave Plus, the lifeline (§6: group 1 of Z-Wave Plus, else AGI group 1
with the Lifeline profile, else the quirk's group, else none; Multi Channel Association to node 1
end point 0 when the end points do not associate by themselves, else plain; read back),
Notification per end point (types, events, push or pull, current state, Notification Set for push
devices), Binary Sensor and Alarm Sensor's types (optional: silence from a device that heard the
question is no types, stored as an empty field `12`; a question no try of which was delivered
stores nothing), air temperature, battery, the quirk's Configuration Sets. A finished interview
whose record lacks Alarm Sensor types (`zw_iv_alarm_types_missing`: written before the interview
asked them, or the device was out of reach) asks only those at the next start, its inputs still
offered; `zw_iv_setting_up` is false for that catch-up, so a check-in Set may run beside it.

Every question waits 1 s plus its round trip after its send ended (serial-api.md 6.12), is tried 3
times (Security 2 Commands Supported Get once per query, as Z-Wave JS: network.md §5 step 4), and
the whole interview 5 times; a device that never answers stays interviewing
(`ZW_IV_EV_STALLED` says at which step). A Set and its read-back share their tries. SET commands to
a node with Supervision go wrapped, its Report the result - not when its quirks row says
`no_supervision` (`zw_iv_supervised()`, the one rule for every Set, zw_net's too). §8's class per
question comes from its CC (`zw_s2_class_for()`), for an end point from Multi Channel's
(`zw_s2_class_for_endpoint()`: an S2 node's highest, whatever the end point's own list says). A
version 0 for a CC the node is shown to use (S2 or S0 while it holds that class, Supervision once
it used it, Version, Manufacturer Specific once read) is kept as version 1, not "not supported". A
sleeping device's questions are held by the owner until it is awake (zw_net: its wake-up queue):
that costs no try, and `ZW_IV_EV_WAKE_NEEDED` says "Wake the device to finish" once per sleep; a
sleeping device's failed attempt waits for its next wake-up. At most two questions of listening
devices are out at once; a sleeping device's go whenever it is awake, and so does the question for
a device's protocol info (the module answers it; a device that just joined may be a sleeper).
`zw_iv_inputs()` tells the zone source what a record offers at an end point: inputs, not ready, no
lifeline ("Does not report to the panel") or pull - no inputs from the last two (ZW-010).

## Sleeping devices and supervision
`zw_health_t` (`zw_health.h`) is network.md §7-8 as a pure decision module: per node the check-in
interval and the wake-up queue (8 commands per node from a pool of 128, a newer one of a kind
replacing the older), No More Information 250 ms after the last one (never while its interview asks it
something, never at interval 0), a late wake-up's interval read again, the listening devices' pings after
15 min of silence and UNREACHABLE after two failed ones 15 min apart, the sleeping devices'
UNREACHABLE at T + 10 min, "nothing heard before counts" after a restart. It returns actions;
zw_net carries them out.

## Removing devices
`zw_remove_t` (`zw_remove.h`) is network.md §10: "Remove a device" for any device (RemoveNode's
Ready in 10 s, the 60 s window, NodeFound, RemovingEndNode with the node ID in 60 s, Done in 60 s,
the stop; node ID 0: another network's device), "Remove a failed device" (No Operation up to 3
times 2 s apart - any answer refuses it; then IsFailedNode, and RemoveFailedNode only for a node
the module lists as failed) and Device Reset Locally (the same, pinging round after round until
the module lists it failed). "Replace a failed device" runs the same check first, alone
(`zw_remove_check_failed`, kind `ZW_REMOVE_BY_REPLACE`: the pings and `IsFailedNode`, no
`RemoveFailedNode`, its end `LISTED` / `STILL_ANSWERS` / `NOT_FAILED` to the caller's callback;
`zw_remove_cancel_check` drops it when the replacement ends first). What a removal means for the
cache and the zones is zw_net's.

## zw_net
`zw_net_t` (`zw_net.h`, the nodes' side in `src/zw_net_nodes.c`) owns all of the above and wires
them. Routing: AddNode and RemoveNode status reports to the inclusion and the removal;
ApplicationCommandHandler frames that are singlecast and from this network to the security
manager (others counted and dropped), whose key-exchange commands go to the inclusion and
everything else to the interview, supervision and the owner; ApplicationUpdate's node
information to the interview and supervision, and "added / removed by another controller" (40,
20) to the owner only, counted (`other_controller`: nothing changes, serial-api.md §6.13); the
manager's frames out as SendData (Nonce and Supervision Reports in the reply class).

- **At every link up** the module's node list is read again (GetSerialApiInitData: a module that
  restarted by itself only gets start-up steps 13-19) and reconciled with the cache and the keys'
  home ID (network.md §2): a first start binds fresh keys (the crypto port's random) to the
  module's home ID and hands them to the owner (`store_keys`); a normal start forgets the nodes the
  module no longer lists (`ZW_NET_EV_NODE_GONE`, their zones CHECK "sensor removed") and
  interviews the ones the cache lacks (a new generation, classes probed); a replaced or reset
  module is reported with the devices to add again, its cache kept until the installer adds one -
  then fresh keys for the new home ID, never the old network's; a module of another system is
  reported and nothing of it used (adding refused). The security manager starts on the module's
  home and node IDs; a link up on the same ones keeps the SPANs. A first start binds only keys
  the crypto port drew and the security manager runs on - it always starts the manager on the set
  it binds (a network reset whose HardReset failed comes back on the same IDs with a new set drawn
  while the manager ran the old one): when the draw (tried at the link up and again at the check)
  or the manager's start fails, the start stops - nothing bound or stored, the manager stopped on
  no network (`zw_s2_stop()`), the radio not checked, `s2_start_failures` counted - and the check
  runs again `ZW_NET_KEY_RETRY_MS` later: drawing keys then if the draw failed, starting the
  manager again on the unbound set it kept if the manager's start failed (ZW01).
- **Persistence**: the owner loads the node cache with `zw_net_load_nodes()` before
  `zw_net_start()` and stores it through `store_nodes` (with the inclusion counter) when a device
  joins, when its key exchange ends, when an interview step completes, when devices are removed or
  forgotten - never because of a report. The inclusion counter never goes back below a stored
  device's generation.
- **Sleeping devices**: commands to a registered sleeping device - the interview's, the owner's
  `zw_net_send()` - go to its zw_health queue with their class and origin; the queue is sent when
  the device wakes (a Wake Up Notification, its node information, or a Get from it other than a Nonce Get - network.md §7; a report is no wake-up), one at a time, a NoAck putting it back; a newer command of a kind supersedes
  one already out. Its kind is its CC, command and first parameter - for Multi Channel
  encapsulation the destination end point and the inner command's, so end points never replace
  each other's (ZW13); an encapsulation it does not look into (Multi Instance v1, CRC-16,
  Supervision Get, Multi Command, Security, Transport Service, several end points at once)
  replaces nothing. The owner of one replaced before it went hears its end at once
  (`ZW_S2_CANCELLED`); a command it sends from there that would replace another is refused
  (`ZW_S2_BUSY`). A Supervision Report the owner owes a sleeping device is a reply, not a
  command for its next wake-up (network.md §7): first in its queue under a kind of its own, the
  device counted awake so it goes at once, No More Information behind it; one the device did not
  hear ends FAILED, never requeued. zw_health hears every frame, every ACK of a listening
  device, every queued command's end; its pings go as No Operation with 3 attempts; UNREACHABLE
  and REACHABLE reach the owner with the node's generation.
- **A device's own check-in** (`zw_net_set_check_in()`, network.md §7): its Wake Up Interval Set
  (clamped and rounded, the panel as destination, at its highest class, supervised as the
  interview's Sets are) waits in its queue for its next wake-up, a newer call replacing it; only
  the device's word - delivered without Supervision, else a Report that is not "fail" - makes the
  record, the stored cache and zw_health take the interval. A NoAck waits for the next wake-up;
  "fail", no Report in 1 s + the round trip (No More Information held until then) or a Set that
  cannot go end it with nothing changed. `ZW_NET_EV_CHECK_IN` says how it ended (`took`,
  `check_in_s`); keeping the choice across a restart of the panel is the owner's (`zwave_zones`'
  `zwz_panel` sets it again). Not a network operation: refused only when not allowed, the link not up,
  `ZW_NET_INVALID` (not a ready sleeping device of ours at an interval, or outside 600..86400 s)
  and, BUSY, when the device's wake-up queue is full.
- **Network operations** (network.md §1), one at a time, each refused when `allowed` (the
  caller's installer-mode and disarmed check) is false, when the link is not up and checked, or
  when another runs: add, remove (`zw_net_remove_device()`), remove failed
  (`zw_net_remove_failed()`), region (`zw_net_set_region()`: one the module offers; SetRFRegion,
  the soft reset and the start-up again), restart (`zw_net_restart()`: the start-up again from
  step 0 - its step 12 the soft reset - with the devices, keys, SPANs and cache kept; with the
  link down not refused but "try now", at once instead of the next retry) and reset
  (`zw_net_reset_network()`: HardReset, the cache cleared and stored, new keys - given, or the
  crypto port's - bound to the new home ID at the start-up that follows; "confirmed twice" is the
  caller's). A Device Reset Locally marks the device gone at once and removes it as failed when
  nothing else runs.
- **Events** (`zw_net_event_t`): the radio's standing, a node added, ready (with its inputs), a
  wake-up needed, stalled, not usable, removed, gone, a removal's progress, unreachable,
  reachable, reset, region, restart, a check-in's end.

`config.interview` turns the interview and supervision on (the firmware); off, nodes stay as the
inclusion left them (the lower layers' tests). `zw_net_t` is 83.5 KB on the P4 - the node cache
42.5 KB, the security manager 14.4 KB, zw_health 12.8 KB, the host 6.7 KB, the interview 3.7 KB,
the check-ins 2 KB: PSRAM, static storage.

## The node cache
`zw_nodes.h`'s `zw_node_cache_t` holds up to `ZW_NODES_MAX` (64, matching zsrc_zwave's own device
ceiling, zones.md §7) node records, each with every field network.md §2's table lists: protocol
info, command classes (top-level and per Multi Channel endpoint), security class, DSK,
manufacturer/product identity, Z-Wave Plus role, wake-up interval, the lifeline, per-endpoint
notification/binary-sensor/multilevel-sensor support, the last completed interview step and the
matched quirks-table row. The other limits (CCs per node and per endpoint, endpoints, notification
entries, mask bytes) are sized from what the project's own sensors (zones.md §8) actually carry,
not the Z-Wave specification's ceilings - `zw_nodes.h`'s own comment states the reasoning next to
each one.

`zw_node_cache_encode()`/`_decode()` follow the same shape as `zone_hub/src/bind_codec.c`: `ZWND |
format | compat | header length | payload length | CRC-32`, then `field | group | len | value`
items, field `00` of group `01` starting a node. Zero values are left out (field `04`'s security
mask is the one exception: its own default, "not yet known", is the sentinel `0xFF`, not `0`, so
`0x00` - confirmed no security - is written like any other real value). Three things are kept raw
rather than decoded, none of them refusing the blob: a field this build has never heard of (kept
against the node it belongs to); an entirely unrecognised group, and any node beyond
`ZW_NODES_MAX` distinct ones (kept for the cache as a whole); and a *well-formed* item of a field
this build does know but whose count or index is beyond this firmware's own fixed limit - more CCs
than `ZW_NODE_CC_MAX`, an endpoint or a mask beyond its own ceiling, a 5th notification type on one
endpoint - written by a later firmware with bigger limits, so a rollback does not have to throw
the whole node away (its generation is part of the zone binding address, zones.md "Addresses").
That last kind is counted separately, in `over_limit`, and is superseded rather than duplicated
once this firmware later decodes (or a fresh interview fills) an in-limits value for the same
field - matched on field alone, or field and endpoint for the repeated per-endpoint fields
(`0A`/`0B`/`0C`/`0D`/`10`). Either bounded raw area drops the newest item that no longer fits
rather than failing the decode, counting the bytes lost in `unknown_lost`. Every other kind
of damage - a bad magic, CRC, or length, an item running past the end, a field before any node
start, a node ID twice, or a length that contradicts a field's own structure (field `03`'s CC list
not a multiple of 3, an endpoint item's declared count not matching its own length) - refuses the whole
blob and leaves the cache passed in exactly as it was: decoding is two passes over the same bytes
(a validate-only pass using only small stack scratch, then an apply pass that cannot fail once the
first one has agreed), never a partial write to a caller's cache.

`zw_node_reconcile()` is a pure function (no I/O) for network.md §2's "At every start" table:
given the module's home ID and node list, the stored keys' home ID (or none), and the cache, it
returns which of the table's four cases applies and, for the normal case, which nodes to
interview and which to forget.

## The fake ZAC93
`test/fake_zac93.c` is the module for the host tests, and later for the PC simulator (Z6), as
`alarm_core/test/sim.c` is for the engine. It parses the host's frames with a `zw_rx_t` and answers
as the module does: an ACK, then what its script says for that FUNC (a response and callbacks after
given delays, or "not queued" n times) or, for every start-up command, what its identity says
(`fake_identity_t`, answered with `zw_api.h`'s builders). The default identity is a ZAC93 on SDK
7.22.1: firmware 1.50, 027A/0004/0611, chip 08, a static controller, primary, node 1 of home
C0FFEE01, no SUC, region USA with USA Long Range on offer. SetRFRegion takes only a region it lists
and applies it at the next restart; SoftReset restarts it after 20 ms, the reset line after 200 ms;
SendDataAbort can provoke the pending SendData callback; its own frames wait for the host's ACK and
are sent again after a NAK. Faults: silence that the reset line revives, or a dead module it does
not; NAK the next n frames, CAN the next one, lose or delay the next ACK, corrupt a byte it sends; a
function answered never (or n times), or n times with a response too short to be its answer; a
SerialAPISetup sub-command refused; SoftReset ignored; a boot into the Gecko bootloader (its menu,
in one read or split in two a few milliseconds apart; "run" leaves it, or, sticky, the
application falls back into it at the next NAK) and a crash into it while running
(`fake_zac93_enter_bootloader`: what was in flight forgotten, the menu printed); wrong identities
(an end node, another chip or library, a missing function, an old protocol); any frame, raw text or
restart at a given time - the test's script, which survives the module's own restarts. Its wire,
`fake_wire_t`, plays `zw_link` between the host and the module on a fake clock that stops at every
deadline, and can tap every byte both ways.

Its radio carries **simulated devices** (`test/fake_zdev.c`, `fake_zac93_add_device`). AddNode
lists: Ready, and when a device's button is pressed (`fake_zac93_press`) NodeFound, AddingEndNode
(or AddingController) with the next node ID and the device's NIF, ProtocolDone, then Done at the
stop; SendData to a device reaches it and is called back OK (or NoAck); AssignSUCReturnRoute
answers; a device's frames come as ApplicationCommandHandler. Faults: the protocol stalling or
Failed after a status, a given node ID, a failing route. A device runs the joining side of
security.md §5 with its own key pair and printed DSK and PIN, its S2 on a `zw_s2_t` in device
mode, then sends reports encrypted at its highest class (or in the clear). Its faults: silence
at any step of the key exchange, a KEX Fail of any type instead, a CSA request or other schemes
and curves, a man in the middle changing its KEX Report or the panel's KEX Set, a request for a
key it was not granted, a Verify under the wrong key, a damaged echo, frames lost or repeated
either way, a report crossing the panel's next frame, a restart.

For the network tests the module also answers GetNodeProtocolInfo from the device, RequestNodeInfo
with ApplicationUpdate 84 (81 when the device sleeps or is gone), RemoveNodeFromNetwork
(`fake_zac93_press_remove`: NodeFound, RemovingEndNode with the node ID - 0 for another network's
device - and Done), IsFailedNode from its failed list (a SendData without an ACK puts a node on it -
or, as a fault, never) and RemoveFailedNode, HardReset (a new home ID, no nodes, a restart); a
SendData to nobody or to a sleeping device ends NoAck. `fake_zac93_join` puts a device in the
network without an inclusion, `fake_zac93_node_info` sends a device's own node information. A device
answers the interview from its model (`fake_model_t`, `test/fake_zdev_cc.c`, built with
zwave_proto's codecs, at the class and from the end point the question came, a supervised Set with
its Supervision Report): identity, versions, its secure CCs, Wake Up (capabilities, interval, the
Set), end points, Z-Wave Plus, Association and Multi Channel Association groups, AGI, Notification
(push or pull), Binary Sensor, Alarm Sensor (its types, and a Get per type: `m.alarm_active`),
air temperature, battery, Configuration; `deaf_cc` makes it miss the panel's plain frames of a CC.
A sleeper really sleeps
(`m.sleeps`): awake after joining, a wake-up or while spoken to, then deaf until its next wake-up
(by itself at its interval, its Wake Up Notification going where the panel set it) or No More
Information; it can reset itself (Device Reset Locally). An end point ignores its own secure CCs
in the clear, as a real one does; a device counts the Multi Channel frames it gets, secure and in
the clear, and remembers the last Supervision Report it got (session, status, class, time). Its
faults: a question never answered (n times), Supervision "fail", a lifeline read back without the
panel, an end point's answer coming from another end point. `test/net_harness.c` is a panel with
an owner that keeps the cache and keys across a restart, grants and types PINs (or a wrong one),
records every event and the module frames it is handed, and can answer a device's Supervision Get
with a Supervision Report through `zw_net_send()`, as zwave_zones does.

## Public API
| Header | What it offers |
|---|---|
| `include/zwave_host/zw_crypto.h` | `zw_crypto_port_t`; `zw_aes_cmac()`, `zw_aes_ccm_encrypt()` / `zw_aes_ccm_decrypt()`, `zw_aes_ofb()`, `zw_s0_cbc_mac()`; `zw_ctr_drbg_t` with `zw_ctr_drbg_instantiate()` / `zw_ctr_drbg_generate()`; `zw_s2_network_key_expand()`, `zw_s2_temp_key_extract()`, `zw_s2_temp_key_expand()`, `zw_s2_nonce_prk()`, `zw_s2_mei()`; `zw_s2_span_instantiate()`, `zw_s2_span_next_nonce()`; `zw_s2_keypair()`, `zw_s2_shared_secret()`, `zw_s0_keys()`; `zw_crypto_port_check()`; `zw_ct_equal()`, `zw_wipe()` |
| `include/zwave_host/zw_transport.h` | `zw_transport_t`, `zw_transport_port_t` (write, frame, text, event); `zw_request_t`, `zw_tx_result_t`, `zw_tx_outcome_t` (with `ZW_TX_LINK_DOWN`), `zw_prio_t`, `ZW_PRIO_BIT()` / `ZW_PRIO_ALL`, `ZW_EXPECT_RESPONSE` / `ZW_EXPECT_CALLBACK`; `zw_transport_init()`, `zw_transport_submit()`, `zw_transport_cancel()`, `zw_transport_halt()`, `zw_transport_set_gate()`, `zw_transport_fail_queued()`, `zw_transport_on_control()` / `_on_frame()` / `_on_text()`, `zw_transport_poll()`, `zw_transport_next_deadline()`, `zw_transport_busy()`, `zw_transport_queued()`; `zw_transport_stats_t` |
| `include/zwave_host/zw_host.h` | `zw_host_t`, `zw_host_port_t` (write, reset_line, link_up, link_down, restarted, frame), `ZW_HOST_RESTART_BOOTLOADER`, `zw_host_config_t` (region wish, Long Range); `zw_module_facts_t`, `zw_link_down_t` / `zw_down_reason_t`, `zw_link_state_t`, `zw_host_stats_t`; `zw_host_init()`, `zw_host_start()`, `zw_host_set_config()`, `zw_host_on_control()` / `_on_frame()` / `_on_text()`, `zw_host_poll()`, `zw_host_next_deadline()`, `zw_host_submit()`, `zw_host_cancel()`, `zw_host_recover()` (the owner's soft reset, §7), `zw_host_link()`, `zw_host_facts()`, `zw_host_down_reason()`, `zw_host_stats()`, `zw_host_transport_stats()` |
| `include/zwave_host/zw_s2_cc.h` | the Security 2 CC: `zw_s2_kex_t` with the KEX Get/Report/Set/Fail codecs, Public Key Report, Network Key Get/Report/Verify, Transfer End, Nonce Get/Report, `zw_s2_encap_t` / `zw_s2_encap_parse()` / `zw_s2_encap_header_build()`, `zw_s2_ext_parse()` (§7's rules), `zw_s2_aad_build()`, Commands Supported Get/Report; the key bits, flags and fail types |
| `include/zwave_host/zw_s2.h` | `zw_s2_t`, `zw_s2_port_t` (send, received, send_done, event), `zw_s2_config_t` (panel or device, the module's frame size), `zw_s2_keys_t`, `zw_sec_class_t`, `zw_s2_rx_t`, `zw_s2_result_t`, `zw_s2_event_t`, `zw_s2_span_state_t`, `zw_s2_node_stats_t` (with `retries`: frames sent again after a NoAck), `zw_s2_tx_t` (the SendData callback's status), `zw_s2_attempts_fn` (transmissions per frame), `zw_s2_jammed_fn` / `zw_s2_jam_t` with `ZW_S2_JAM_WAIT_MS` and `ZW_S2_JAM_TRIES` (a jammed module's frames); `zw_s2_init()`, `zw_s2_set_attempts()`, `zw_s2_set_jammed()`, `zw_s2_start()`, `zw_s2_stop()`, `zw_s2_set_key()`, `zw_s2_network_key()`; `zw_s2_add_node()`, `_add_nif_ccs()`, `_set_classes_unknown()`, `_set_class_result()`, `_refused()`, `_classes_unknown()`, `_remove_node()`, `_has_node()`, `_set_granted()`, `_granted()`, `_highest()`, `_class_for()`, `_class_for_endpoint()`, `_set_secure_ccs()`, `_node_stats()`, `_span_state()`; `zw_s2_set_temp_key()`, `_set_exchange_class()`, `_clear_temp_key()`, `_forget_span()`; `zw_s2_send()`, `zw_s2_sent()`, `zw_s2_sent_tx()`, `zw_s2_receive()`, `zw_s2_receive_group()`, `zw_s2_command_is_get()`, `zw_s2_poll()`, `zw_s2_next_deadline()`; `zw_s2_command_is_reply()`, `zw_s2_frame_is_reply()` |
| `include/zwave_host/zw_incl.h` | `zw_incl_t`, `zw_incl_port_t` (submit, s2_send, known, event), `zw_incl_options_t`, `zw_incl_event_t` (listening, found, adding, joined, grant, PIN, done), `zw_incl_state_t`, `zw_incl_result_t` (a replacement's `STILL_ANSWERS`, `NOT_FAILED`), `zw_incl_security_t`, `zw_incl_s2_failure_t`; `zw_incl_init()`, `zw_incl_start()`, `zw_incl_replace()`, `zw_incl_replace_checked()`, `zw_incl_keep_listening()`, `zw_incl_stop()`, `zw_incl_grant()`, `zw_incl_pin()`, `zw_incl_cancel()`, `zw_incl_module_frame()`, `zw_incl_s2_command()`, `zw_incl_s2_event()`, `zw_incl_link_down()`, `zw_incl_poll()`, `zw_incl_next_deadline()`, `zw_incl_state()`, `zw_incl_node()` |
| `include/zwave_host/zw_iv.h` | `zw_iv_t`, `zw_iv_port_t` (send, node_info, submit, changed, event, quirk, wake_interval), `zw_iv_node_t`, `zw_iv_step_t`, `zw_iv_event_t`, `zw_iv_quirk_t`, `zw_iv_inputs_t`, `zw_iv_stats_t`; `zw_iv_init()`, `zw_iv_link_up()` / `_link_down()`, `zw_iv_start()`, `_restart()`, `_stop()`, `_running()`, `_setting_up()`, `_step()`, `zw_iv_received()`, `zw_iv_node_info()` / `_node_info_failed()`, `zw_iv_sent()`, `zw_iv_asleep()`, `zw_iv_went()`, `zw_iv_awake()`, `zw_iv_is_tag()`, `zw_iv_poll()`, `zw_iv_next_deadline()`, `zw_iv_stats()`; `zw_iv_inputs()`, `zw_iv_has_cc()`, `zw_iv_endpoint_has_cc()`, `zw_iv_sleeping()`, `zw_iv_supervised()`, `zw_iv_alarm_types_missing()`, `zw_iv_add_cc()` |
| `include/zwave_host/zw_health.h` | `zw_health_t`, `zw_health_action_t` (ping, unreachable, reachable, send queue, No More Information, re-read interval); `zw_health_choose_interval()`; `zw_health_add()`, `_remove()`, `_set_interval()`; `zw_health_heard()`, `_acked()`, `_wakeup()`, `_sent()`, `_set_busy()`, `_restart()`; `zw_health_queue_put()`, `_cancel()`, `_peek()`, `_len()`, `_take()`; `zw_health_poll()`, `zw_health_next_deadline()` |
| `include/zwave_host/zw_remove.h` | `zw_remove_t`, `zw_remove_port_t` (submit, ping, event), `zw_remove_event_t`, `zw_remove_state_t`, `zw_remove_result_t` (removal as failed: `REFUSED`, `TIMEOUT`, `STILL_ANSWERS`, `NOT_FAILED` apart from `DONE`; the replacement's check: `LISTED`), `zw_remove_kind_t` (with `ZW_REMOVE_BY_REPLACE`), `zw_remove_checked_t`; `zw_remove_init()`, `zw_remove_start()`, `_keep_listening()`, `_stop()`, `zw_remove_failed()`, `zw_remove_reset_locally()`, `zw_remove_check_failed()`, `zw_remove_cancel_check()`, `zw_remove_module_frame()`, `zw_remove_ping_done()`, `zw_remove_link_down()`, `zw_remove_poll()`, `zw_remove_next_deadline()`, `zw_remove_state()`, `zw_remove_is_tag()` |
| `include/zwave_host/zw_net.h` | `zw_net_t`, `zw_net_port_t` (the link, frames, sends, the inclusion's and the network's events, store_nodes, store_keys, quirk, check_in), `zw_net_config_t` (host config, keys and their home ID, the inclusion counter, check-in, interview), `zw_net_event_t`, `zw_net_radio_t`, `zw_net_result_t`, `zw_net_stats_t` (with `jam_resets`), `zw_net_checkin_t`; `zw_net_init()`, `zw_net_load_nodes()`, `zw_net_start()`, `zw_net_on_control()` / `_on_frame()` / `_on_text()`, `zw_net_poll()`, `zw_net_next_deadline()`, `zw_net_add_node()`, `zw_net_send()`, `zw_net_add_device()`, `zw_net_remove_device()`, `zw_net_remove_failed()`, `zw_net_replace_failed()`, `zw_net_set_region()`, `zw_net_reset_network()`, `zw_net_restart()`, `zw_net_set_check_in()`, `zw_net_interview()`, `zw_net_resume_interview()`, `zw_net_host()`, `zw_net_s2()`, `zw_net_incl()`, `zw_net_rm()`, `zw_net_iv()`, `zw_net_health()`, `zw_net_nodes()`, `zw_net_radio()`, `zw_net_stats()` |
| `include/zwave_host/zw_nodes.h` | `zw_node_cache_t`, `zw_node_record_t`, `zw_node_endpoint_t`, `zw_node_cc_t`, `zw_node_notification_t` and the `ZW_NODE_*_MAX`/`_BYTES` limits; `zw_node_cache_init()`, `zw_node_find()` / `_find_const()`, `zw_node_add()`, `zw_node_remove()`, `zw_node_count()`, `zw_node_at()` / `_at_const()`; `zw_node_cache_encode()` / `_decode()`; `zw_nodes_crc32()`; `zw_reconcile_case_t`, `zw_reconcile_result_t`, `zw_node_reconcile()` |

Every crypto function takes the port first and returns false when an argument is invalid or a
port call failed; its outputs are then zeroed, and a generator is left as it was (a failed draw
consumes no nonce). The crypto results, `zw_transport_submit()`, `zw_host_submit()`,
`zw_s2_start()` and `zw_s2_set_key()` are marked must-check: GCC and Clang warn when one is
ignored (an error in the host build).

## Depends on
- Project components: `zwave_proto` (the frames, their parser, the Serial API codecs and the
  command-class codecs). The Z-Wave library depends on nothing else in this repository
  (docs/features/36 "Separation rules").
- ESP-IDF: none (portable).

## Options
None. `ZW_TRANSPORT_QUEUE_LEN` (16) may be defined for the whole build; the host keeps its last
slot for itself.

## Rules
- Plain C11: no malloc, no globals, no RTOS, ESP-IDF, mbedTLS or PSA headers; the host build
  stays warning-free.
- Only the `zwave` task touches a `zw_net_t` (its host, transport, security manager and
  inclusion); other tasks send it requests through a queue. Callbacks never feed link news or
  frames back in.
- Keys are never shown or reported: they go only into a Network Key Report during a key
  exchange (security.md §1).
- Secrets: intermediate keys, keystreams and MAC chains are wiped with `zw_wipe()` before a
  function returns; tags and the all-zero X25519 result are checked in constant time; no branch
  depends on secret data. Callers wipe what they keep (keys, a `zw_ctr_drbg_t`).
- CCM decryption lets no unauthenticated byte out: on any failure its output is zeroed.
- The expected values are security.md §11, serial-api.md's and the standards': when one
  disagrees, find which side is wrong - never edit an expected value to make a test pass.
- The node cache is written only when network.md §2 says (a device joins, a key exchange ends,
  an interview step completes, devices are removed or forgotten), never because of a report.
- Tests reference `ZW-SEC-NNN` (the parts of ZW-008), `ZW-001` to `ZW-007`, `ZW-009` to
  `ZW-012`, `ZW-015` to `ZW-018` and `ZW-023`. (CONTRIBUTING.md, "Rules for the portable
  components")

## Tests
Run all host tests with `pwsh tools/host_test.ps1` (Windows) or
`cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`; one suite with `ctest --test-dir build/host -R <suite>`.
The suites with the fake module link `fake_zac93.c`, `fake_zdev.c`, `fake_zdev_cc.c` and
`zw_host_crypto.c`; `zw_interview` and `zw_house` add `net_harness.c`.

`zw_house` (`test_zw_house.c`, 1 test: the virtual house, milestone Z5's "Done when"; ZW-010,
ZW-011, ZW-012, ZW-016, ZW-017): six simulated devices - a door contact (sleeping, S2
Authenticated), a motion sensor that never sends idle (sleeping, S2 Unauthenticated), a mains
multi-sensor with two end points (S2 Access Control, Multi Channel Association lifeline), an
insecure mains leak sensor, an insecure sleeping smoke detector, a tilt sensor (sleeping, S2 Access
Control) - added and interviewed, reporting (each from its node, end point and class), through two
hours of check-ins (No More Information after each, pings for the quiet mains devices, nobody
unreachable), a panel restart (nothing lost, nobody interviewed again, nobody unreachable for what
the panel did not hear, reports resync), a module restart (the SPANs kept), the module pulled for
twenty minutes (link down, then back, reachability afresh), the smoke detector removed; then a dead
mains device UNREACHABLE after two failed pings 15 min apart and a dead sleeper UNREACHABLE at T +
10 min to the millisecond, REACHABLE at its next wake-up; a last restart with the stored cache
intact.

`zw_interview` (`test_zw_interview.c`, 52 tests; ZW-002, ZW-004, ZW-006, ZW-007, ZW-009, ZW-010,
ZW-011, ZW-012, ZW-015, ZW-016, ZW-017, ZW-018): a secure door sensor's interview, every step's result in its record, every Set
supervised, stored, No More Information after it (and after its node information); the interview
resumed after a panel restart without asking again what was done, and paused by a module restart
without spending a try; a device dozing off mid-interview, no try spent, continued at its wake-ups;
a sleeper's failed attempt waiting for its next wake-up; a sleeper (and a joined device's protocol
info) not waiting for a turn while two listening devices hold both; never more than two listening
devices asked at once; a supervised Set reported failed (a failed try, never read back); an answer
from another end point (waited for again); the lifeline from AGI, from the quirks table, none
("Does not report to the panel"), through Multi Channel Association (end points' reports from their
end points), a read-back without the panel (stalled after 5 x 3 Sets); push or pull probed (FF
push, 00 pull - no inputs, no answer push without a failed attempt) and a node without Association
(pull, no lifeline); a question's 1 s plus round trip, 3 tries, 5 attempts, stalled; a controller
not usable; the first start's keys (and a failed key draw, DRBG seed or key expansion stopping
it until its retry, the security manager on no network, then one set active, owned and stored;
a network reset whose HardReset failed binding the set the manager runs on); the "At every start" cases: normal (a node added with a new
generation, one forgotten with its generation, the node ID's next owner another generation, also
when the stored counter fell behind the records), replaced (the cache kept, then new keys - not the
old ones - when a device is added, and it is interviewed), foreign (nothing used, adding refused,
nothing stored, also without keys); the cache lost: a secure node found, probed at Access Control,
and one with Unauthenticated alone (each class it lacks sent once and resent once); the node list
read again after a module restart (3 tries on garbled answers, else the start-up's); exclusion of
an own device (forgotten, stored, its generation told) and of another network's (node ID 0), the
window's 60 s, refusals; remove failed refusing a device that answers and one the module does not
list as failed, removing a dead one after 3 pings 2 s apart, IsFailedNode and RemoveFailedNode;
Device Reset Locally (gone at once, removed as failed; never listed failed: 3 rounds, kept); the
region (refusals, SetRFRegion, the start-up again, devices kept); the network reset (HardReset,
cache cleared, new keys for the new home ID, a device added after); a listening device's two failed
pings and REACHABLE again; a sleeper unreachable at T + 10 min, a command held for it, its late
wake-up re-reading the interval, then No More Information; a queued command replacing one already
on its way (whose end the owner still hears); Multi Channel commands to two end points both kept,
the same end point's replaced with its owner told once; encapsulations it does not look into and
end point 127's FF FF FF kept; an owner sending again from its CANCELLED refused, not recursing. Restarting the radio: the soft reset and the start-up
again, nobody forgotten, added or interviewed again, keys and SPANs kept (a report taken at once at
its class), refused when not allowed and while adding or removing, nothing added while it starts;
with the radio not answering, the start-up
at once instead of the next retry. A device's own check-in: nothing sent while it sleeps, the old
interval counting until it confirms (UNREACHABLE at T + 10 min meanwhile), then at its wake-up the
Set at its highest class inside Supervision and No More Information, the record, the stored cache
and zw_health at 12 h (not unreachable at 1 h + 10 min, at 12 h + 10 min to the millisecond); the
last of two choices sent alone, clamped and rounded; every refusal, never BUSY while adding or
removing; a Set not heard waiting for the next wake-up, "fail" and no Report (No More Information
held until the 1 s + round trip) changing nothing and not sent again; a quirks row's
`no_supervision` followed by the interview and the check-in alike. Scenarios from Z-Wave JS's
tests: a device whose key exchange failed (the wrong PIN) present and insecure after a panel
restart, never probed or treated as secure; a version 0 not believed for Supervision a sensor
has used (its Sets still supervised), but taken at its word for Supervision never used; an end
point's questions at the S2 node's highest class though its own list omits Security 2, its report
in the clear dropped, encrypted counted; a sleeping sensor's Supervision Report sent at once, not
at its next wake-up, and ahead of a queued command while the Serial API is busy, and one it no
longer hears ending FAILED, not kept for its next wake-up; another controller's "node added" and
"node removed" changing nothing, counted and handed to the owner.

`zw_health` (`test_zw_health.c`, 14 tests; ZW-011, ZW-012): zw_health alone on a fake clock -
the interval choice, the sleeping and listening clocks to the millisecond, interval 0, a
restart, the queue's limit, order and replacement, a NoAck requeueing, No More Information at
250 ms and held back while busy, a late wake-up.

`zw_inclusion` (`test_zw_inclusion.c`, 40 tests, ZW-005, ZW-006, ZW-007, ZW-009, ZW-015, ZW-023): `zw_net`
end to end against the fake module and a simulated door sensor, on a fake clock. Joins with S2
Access Control, Authenticated (the PIN typed from the label, groups 2-8 checked; and typed 30 s
late, the device's echoes dropped meanwhile) and Unauthenticated (no PIN), with less granted
than asked for, and without S2; the refusals before starting (not allowed, busy); a module that
skips Ready; no Ready in 10 s; the 60 s window and "Keep listening"; stopped by
the installer; a controller; Failed; no AddingEndNode in 60 s, no ProtocolDone in 76 s + 217 ms
per listening + 3.5 s per FLiRS node; a node ID already known and the broadcast one; a failed
route; the wrong PIN; every timer to the millisecond (TA1-TA5, TAI1, TAI2 from the Public Key
Report even with a late PIN); the device's KEX Fail at each step; the KEX Report refused (echo,
scheme, curve, CSA, no key); grants refused beyond the request or empty; one damaged echo not a
wrong PIN; keys asked for without a grant, a Verify under the wrong key, a Network Key Get in
the clear, a last Transfer End without "complete"; the installer cancelling; the link going
down mid-exchange; downgrades (S0 only asked for, the KEX Report or the KEX Set changed on the
way, an echo with a reserved bit). At work: a report at Access Control and a "door closed" in
the clear dropped; frames from unknown nodes, other networks and broadcasts dropped; resyncs
after the panel restarts and after the sensor does; a module restart keeping the SPANs; a
command undelivered, a nonce that never comes (1 s + the round trip), a stale SendData end after
a resend; nonce reports ahead of queued commands; 4 lost reports bridged, a repeat dropped;
crossed frames. Scenarios from Z-Wave JS's tests: a Nonce Report the sensor never heard (sent
once, options 05) not wedging it, its next Nonce Get answered and report taken; a device listing
Security 2 added without security holding no class at all (none granted, stored as 00 with no
DSK, every CC in the clear, a named class refused, an S2 frame from it dropped); a frame whose
command runs past its end ACKed, counted as malformed and handed on to nobody.

`zw_s2` (`test_zw_s2.c`, 32 tests, ZW-007, ZW-009): two managers, a panel and a device, over a
pretend radio that loses, repeats, holds and crosses frames. The Security 2 codecs; §7's
extension rules one by one; §11's encapsulation frames decrypted by the manager on §11's
receiver EI (and a multicast follow-up getting one nonce only); first contact and its SPAN
extension; a command that does not fit with the extension going behind a SPAN frame of its own;
the panel restarting, 4 and 5 lost frames, crossed frames (in 500 ms, after it, with a skipped
sequence number), duplicates, nonce reports without floods, a Nonce Report forgetting the last
sequence number, the device restarting (the panel sends once more), a frame not decoded twice, a
Nonce Report that never comes, sends in order, the choice of class and the refusals; §8 (a clear
"door closed", a clear Supervision around a secure report, a lower class, unknown nodes);
command-classes.md §1 (S2 inside Multi Channel, Supervision inside Supervision, Multi Channel
inside Multi Channel dropped; S2 -> Supervision -> Multi Channel taken with its session and
endpoint; Multi Command, CRC-16, Transport Service, S0 outside and inside S2); §9's answers and
a node's own report; the temporary key and the class being exchanged; no key, no nonce; which
failures count for the wrong PIN. Scenarios from Z-Wave JS's tests: a frame in the clear posing
as the last part of an encrypted two-part answer dropped, both genuine parts taken; a SPAN at a
lower class never carrying the panel's own commands, which still go at the highest class.

`zw_startup` (`test_zw_startup.c`, 37 tests, ZW-001 and ZW-004), on a fake
clock through the fake module's wire: the whole start-up exchange with a ZAC93, every byte both
ways, frozen and checked against §5 step by step (to regenerate it after a deliberate change,
run `ZW_DUMP_STARTUP=1 build/host/test_zw_startup`, which prints it as the constant, and check
it against §5 again before freezing it); the facts at link up; each §4 refusal (an end node,
then swapped for a controller at the next attempt; another chip; a missing function; another
library, and a bridge accepted) and the protocol note; the region different, the same, and one
the module lacks; Long Range with and without a Long Range region, and SetNodeIDType refused;
the SUC left alone (one already, a secondary controller either way, a SIS elsewhere); a step
failing twice then passing, three times into the soft reset, and on into link down; an answer
it cannot use; the reset line reviving a hung module at start and while up; a silent module
without a reset line, and a dead one with it; the 30 s x 10 then 5 min schedule, and 30 s
again after the link was up; an unexpected restart while up (node commands held by the gate,
sent after steps 13-19), while starting after step 12 and before it, and again and again; the
start-up begun again while one of its commands waits; the bootloader left with "run", stuck,
and "run" without a start (then the module starting by itself while down); its menu arriving in
pieces at step 1; a crash into it while up, recognised the moment the menu is whole (the SendData
in flight ended, nothing more of it sent, "restarted" FF, "run" at once, the start-up again from
step 1 and the link up, never down), and an application that keeps falling back into it (two
escapes, then the link down, "bootloader"), even after more stray text than the host keeps; link
down failing queued requests with LINK_DOWN; requests waiting while starting; the host's queue
slot.

`zw_transport` (`test_zw_transport.c`, 40 tests, ZW-003, ZW-004): a command with a
response, with a callback, with both; callback IDs wrapping past 255 to 1; NAK, NAK, NAK at
0/100/1200 ms then "not answering"; the ACK timeout at exactly 1600 ms; a response or a
callback before its ACK counting as the ACK (a SendData whose ACK was lost goes once), but not
the callback of a command that waits for a response too; an ACK after its 1600 ms not taken;
CAN's 100 ms with the module's frame handled first; the response timeout (reported as not
answering) and the callback timeout; module frames interleaved with a command, never
mismatched; intermediate then final callbacks and the timeout between them; AddNode ending at
its first status; SendData "not queued" 3 times and twice; SendDataAbort at 20 s, failure at
30 s, the abort provoking the callback, the next command held until the abort's ACK; an
unexpected SerialAPIStarted, busy and idle; SoftReset with its SerialAPIStarted, its damaged
ACK (any of the 15 other high nibbles, taken only as a lone byte: the bootloader's menu, or a
'v' glued to other bytes, is no ACK), and without a restart; priorities; the gate holding all but module commands; failing the
queued requests of chosen classes; a full queue and malformed requests; cancel by tag; a
corrupted byte NAKed and resent; the discarded text reaching the owner; stray ACK/NAK/CAN; a
failed write; what `done()` may do.

`zw_crypto` (`test_zw_crypto.c`, 31 tests, ZW-008):

| ID | What | Vectors |
|---|---|---|
| ZW-SEC-001 | AES-128, one block through the PC port | FIPS-197 C.1; SP 800-38A F.1.1 |
| ZW-SEC-002 | AES-CMAC | RFC 4493 §4, all four examples |
| ZW-SEC-003 | AES-CCM: encryption, decryption, in place, any changed bit of tag, ciphertext, AAD, nonce or key refused with the output zeroed, lengths and arguments refused | RFC 3610 packets #1-#6 and #13-#18 (the 8-byte-tag ones); edge and longest lengths computed with OpenSSL |
| ZW-SEC-004 | X25519 and key pairs: the all-zero result refused | RFC 7748 §5.2 (iterated 1 time, and 100 times computed with OpenSSL) and §6.1; small-order points |
| ZW-SEC-005 | CTR_DRBG: multi-block draws, the 128-bit counter's carry, a failed call leaving the state; NIST's own vectors (Instantiate, Reseed, Generate twice) | security.md §11's SPAN nonces; draws computed with `vectors/s2_reference.py`; NIST CAVP CTR_DRBG `[AES-128 no df]`, prediction resistance off, no additional input, personalization string 0 and 256 bits: COUNT 0 and 14 of the first and last block of each (the response file as Z-Wave JS carries it) |
| ZW-SEC-006 | AES-OFB and S0's CBC-MAC | SP 800-38A F.4.1; §11's real-traffic CBC-MAC |
| ZW-SEC-007 | network key expansion | §11 |
| ZW-SEC-008 | temporary key: key pairs, shared secret, PRK, temp KeyCCM and PString | §11 (RFC 7748 §6.1's keys) |
| ZW-SEC-009 | SPAN: NoncePRK, MEI, nonces 1-3 | §11 |
| ZW-SEC-010 | S2 Message Encapsulation: AAD from §7's layout, both frames, decrypted back | §11 |
| ZW-SEC-011 | S0: AuthKey/EncKey (and the all-zero key's), OFB, CBC-MAC, the frame | §11 |
| ZW-008 | every function false with its outputs zeroed when the port fails - each call it makes, one at a time, or a port missing a primitive | - |
| ZW-008 | the port's own check (`zw_crypto_port_check`): the PC port passes in five calls; a wrong AES, an X25519 wrong on either vector, a random source stuck, all zero, zero on either draw or writing nothing, a failing call, a missing primitive or no port refused | FIPS-197 C.1; RFC 7748 §5.2 (first vector) and §6.1 (Alice's public key) |

Every standards vector was read from the published text (the RFCs, FIPS-197, SP 800-38A and
NIST's CAVP response file) and checked against Python's `cryptography` (OpenSSL) before it went
in.

`zw_nodes` (`test_zw_nodes.c`, 17 tests, ZW-017): the CRC-32 against the standard's own published
check value; the accessors including the `ZW_NODES_MAX` ceiling; a full cache (every field kind
this codec knows) round-tripped through encode then decode; zero values left out except field
`04`'s own `0xFF`-is-the-default convention; a hand-built blob with an unknown field and an
unknown group decoding correctly and re-encoding byte for byte; an unknown area overflowing
without refusing the decode; every kind of damage (bad magic, compat, CRC, header/payload length,
a truncated item, a field before any node, a duplicate node ID, a length that contradicts a
field's own structure) refused with the cache passed in left exactly as it was; the CC-list and
per-endpoint-notification limits right at the ceiling, accepted normally; five over-limit cases
(a CC list, an endpoint's own CC list, an endpoint index, a mask, a 5th notification type, each
beyond this firmware's own fixed limit but otherwise well formed) each decoding with the rest of
the node intact, kept raw and round-tripping byte for byte, then superseded - not duplicated -
once an in-limits value for the same field arrives (field 0B's per-endpoint, not per-type,
matching means its case is instead shown being absorbed by its own siblings immediately); the
frozen fixture (`zw_nodes_fixture_v1.h`, regenerate with
`ZW_DUMP_NODES=1 build/host/test_zw_nodes`) decoding to exactly the expected records and
re-encoding byte for byte, and `zw_nodes_fixture_v2.h` (made by hand) the same for field 12, an
endpoint's Alarm Sensor types; every row of network.md §2's "At every start" table; and, under
ASan/UBSan in CI, the decoder run over thousands of random byte strings and thousands of randomly
mutated copies of a valid blob, checking it never crashes and a refusal never half-applies.

## Sizes on the P4
RISC-V, `-Os`: the interview 10.8 KB of code (`zw_iv.c` 4.7, `zw_iv_steps.c` 6.1), `zw_net` 14.3
KB (`zw_net.c` 4.2, `zw_net_nodes.c` 10.2), `zw_remove.c` 2.0 KB, `zw_health.c` 2.8 KB, the security
manager 7.3 KB, the inclusion 4.8 KB, the host and transport 9.6 KB. State: `zw_net_t` 83.5 KB in
all (PSRAM, see zw_net).

## Later milestones
- **Z6** devices as zones: `zsrc_zwave` over `zw_net`'s events and `zw_iv_inputs()`, the quirks
  table behind the `quirk` port, Supervision Gets answered, the panel's own answers
  (command-classes.md §5); the PC simulator.
- **Z8** the zwave task: NVS behind `store_nodes` / `store_keys` / `zw_net_load_nodes()`, the
  region and check-in settings, the crypto port on PSA.
