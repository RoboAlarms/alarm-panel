# 36 · Z-Wave: the command classes the panel speaks

Part of [docs/features/36](README.md). The application layer: what travels inside `SendData` and
`ApplicationCommandHandler` ([serial-api.md](serial-api.md) 6.11-6.12). Only the command
classes (CCs) a security-sensor controller needs; each one is complete enough to write its
codec. How reports become zone events is [zones.md](zones.md); when the panel sends what is
[network.md](network.md).

Conventions as in serial-api.md: hex, big-endian. A command is `CC | command | parameters`;
CC IDs `>= F1` take two bytes (none of ours do). **Every parser checks lengths before reading**
and treats a short or malformed command as "not understood" (counted, never a crash); extra
trailing bytes are allowed (newer versions append fields).

## 1. Encapsulation

Commands can be wrapped. **Receiving**, the panel unwraps from the outside in, in whatever
order it finds them:

| Layer | CC | What to do |
|---|---|---|
| Security 2 / Security 0 | `9F` / `98` | decrypt; note the security class ([security.md](security.md) §7-8) |
| Transport Service | `55` | not supported in v1: drop and count (sensors do not segment their reports) |
| Supervision Get | `6C 01` | remember the session; handle the inner command; answer (section 4.14) |
| Multi Channel | `60 0D` | note the source endpoint; handle the inner command for that endpoint |
| CRC-16 | `56 01` | check the CRC, drop on mismatch; handle the inner command |
| Multi Command | `8F 01` | handle each inner command in order |

Two rules on top of the order: **Security 2 / Security 0 count only as the outermost layer** -
one found inside another layer is dropped, because the layers around it are not authenticated
(a forged Multi Channel wrapper could otherwise move a secure report to another endpoint); and
**every other layer appears at most once** - a frame that repeats one is dropped. The codec
(`zw_cc_unwrap()`) peels one layer per call; the host's receive loop applies both rules. "Security 2"
here means its Message Encapsulation and its nonce commands; the other Security 2 commands
(Commands Supported Get/Report) are ordinary commands wherever they stand.

**Built (BL-152, ZW-009): the level, as Z-Wave JS.** From an S2 device every command must come at
its highest class, with the exceptions in security.md §8 (Driver.ts isSecurityLevelTooLow). To it,
supported CCs and Basic go encrypted, unsupported ones in the clear (isCCSecure). A CRC-16 layer
is unwrapped and its content judged. The rule in full is security.md §8. Proven by `test_zw_s2`
(`test_unlisted_cc_below_class_dropped`, `test_stale_nif_report_dropped`,
`test_crc16_around_s2`, `test_s0_node_rule`) and `test_zwz_fuzz`.

**Sending**, the panel builds from the inside out: command → Multi Channel (only when talking
to an endpoint other than 0) → Supervision (SET-type commands to nodes that support it) →
Security (the node's highest class, if any). The panel does not use CRC-16 or Multi Command
when sending. A reply to a command (a Supervision Report, a Report answering a Get) goes back
with the **same** encapsulation the request had: same security class, same endpoint pair
(swapped).

## 2. CC identifiers used

| CC | ID | Panel supports | Panel controls (sends to nodes) | Phase |
|---|---|---|---|---|
| No Operation | `00` | | yes (ping) | 1 |
| Basic | `20` | | yes (receive Set/Report from sensors) | 1 |
| Binary Sensor | `30` | | yes | 1 |
| Multilevel Sensor | `31` | | yes (temperature) | 1 |
| CRC-16 Encapsulation | `56` | yes | | 1 |
| Association Group Information | `59` | yes (answers) | yes | 1 |
| Device Reset Locally | `5A` | yes | | 1 |
| Central Scene | `5B` | | yes (buttons, key fobs) | 2 |
| Z-Wave Plus Info | `5E` | yes | yes | 1 |
| Multi Channel | `60` | | yes | 1 |
| Supervision | `6C` | yes | yes | 1 |
| Entry Control | `6F` | | yes (an assigned keypad's Notifications, ZW-031) | 2 |
| Configuration | `70` | | yes (quirks only) | 2 |
| Notification | `71` | | yes | 1 |
| Manufacturer Specific | `72` | yes | yes | 1 |
| Wake Up | `84` | | yes | 1 |
| Association | `85` | yes | yes | 1 |
| Version | `86` | yes | yes | 1 |
| Indicator | `87` | | yes (identify) | 2 |
| Time | `8A` | yes (answers) | | 2 |
| Multi Channel Association | `8E` | yes | yes | 1 |
| Multi Command | `8F` | yes (receive) | | 1 |
| Alarm Sensor (legacy) | `9C` | | yes | 2 |
| Security 0 | `98` | optional | optional | 3 |
| Security 2 | `9F` | yes | yes | 1 |
| Battery | `80` | | yes | 1 |
| Hail (legacy) | `82` | | yes (receive) | 2 |

Anything else a node sends is ignored (logged once per node and CC in `zwave node <n>`).

## 3. Node information and versions

A node's **node information frame** (NIF) lists its supported CCs ([serial-api.md](serial-api.md)
6.13). For each CC the panel uses, it asks the node's version with Version CC (4.20) during the
interview and parses reports by that version. A CC the node lists with version 0 is treated as
not supported. Nodes included with S2 list more CCs in their Security 2 Commands Supported
Report than in their NIF; the union is what the node supports, and the CCs from the S2 report
are its **secure** CCs.

## 4. The command classes

### 4.1 No Operation (`00`)
The whole command is the single byte `00`. Sent to a listening node as a ping: an ACK in the
SendData callback means it is alive. Never sent to sleeping nodes.

### 4.2 Basic (`20`)

| Cmd | Name | Payload |
|---|---|---|
| `01` | Set | `value` (`00` off/idle, `FF` or `01..63` on/active) |
| `02` | Get | - |
| `03` | Report | `current value` · [v2: `target value` · `duration`] |

Many sensors also send **Basic Set** to their lifeline when they trigger. The panel uses Basic
only as described in zones.md "Basic" (a sensor with nothing better, or a quirk); otherwise a
Basic Set from a sensor is ignored, so one event is not counted twice.

### 4.3 Binary Sensor (`30`)

| Cmd | Name | Payload |
|---|---|---|
| `01` | Supported Get (v2) | - |
| `04` | Supported Report | bit mask: bit 0 of byte 0 is reserved, bit `n` (counting from 0 across bytes) = sensor type `n` |
| `02` | Get | [v2: `sensor type`, `FF` = the node's default] |
| `03` | Report | `value` (`00` idle, `FF` detected) · [v2: `sensor type`] |

Sensor types: `01` general purpose, `02` smoke, `03` CO, `04` CO2, `05` heat, `06` water,
`07` freeze, `08` tamper, `09` aux, `0A` door/window, `0B` tilt, `0C` motion, `0D` glass break.

### 4.4 Multilevel Sensor (`31`)

| Cmd | Name | Payload |
|---|---|---|
| `01` | Supported Sensor Get (v5+) | - |
| `02` | Supported Sensor Report | bit mask, bit 0 of byte 0 = type 1 |
| `03` | Supported Scale Get (v5+) | `sensor type` |
| `06` | Supported Scale Report | `sensor type` · `scale mask` (bits 3..0: scale 0..3) |
| `04` | Get | [v5+: `sensor type` · `scale << 3`] |
| `05` | Report | `sensor type` · `precision (bits 7..5) · scale (bits 4..3) · size (bits 2..0)` · `value` (size = 1, 2, 3 or 4 bytes, signed) |

Value = signed integer / 10^precision. The panel uses **air temperature** (type `01`; scale 0
= °C, 1 = °F, converted to hundredths of °C) for freeze and heat zones; other types
(humidity `05`, luminance `03`...) are shown on the device page only.

### 4.5 Notification (`71`)

| Cmd | Name | Payload |
|---|---|---|
| `07` | Supported Get (v2+) | - |
| `08` | Supported Report | `flags`: bit 7 = the node also sends v1 alarms, bits 4..0 = mask length `n` · type mask (`n` bytes; bit 0 of byte 0 unused, bit `t` = notification type `t`) |
| `01` | Event Supported Get (v3+) | `notification type` |
| `02` | Event Supported Report | `notification type` · `mask length (bits 4..0)` · event mask (bit 0 unused, bit `e` = event `e`) |
| `04` | Get | `v1 alarm type (00)` · `notification type` · [v3: `event`] - the panel asks `00 · type · 00` for "the current state of this type" |
| `05` | Report | below |
| `06` | Set (v2+) | `notification type` · `status` (`FF` on / `00` off): turns the node's unsolicited reports for that type on |

**Report** (`71 05`):

| Offset | Field |
|---|---|
| 0 | v1 alarm type (vendor-specific; `00` when unused) |
| 1 | v1 alarm level |
| 2 | reserved (was a source node ID) |
| 3 | notification status (`FF` on, `00` off, `FE` "no pending notification" answering a Get) |
| 4 | notification type |
| 5 | event (`00` = idle, below) |
| 6 | bit 7: a sequence number follows; bits 4..0: event parameter length `k` |
| 7 | `k` bytes of event parameters |
| 7+k | sequence number (optional) |

A report shorter than 7 bytes is a **v1 alarm**: only the first two bytes mean anything
(ZW-043). The panel reads it, or a longer report whose notification type is `00`, like Z-Wave JS
(GH#1034, `compat.alarmMapping`): when the device's Supported Reports list the alarm type as a
notification type and the alarm level as one of its events, as that event; else through the
first row of its quirk's alarm map that matches the alarm type and level (zones.md §8). Alarm
type `00` is never read; anything else is dropped. One narrowing: a report that names its own
notification type is never re-read from its first two bytes (Z-Wave JS re-reads any report from
a v2+ device), and the map applies to any Notification version when the type is `00` (Z-Wave JS
uses it for v1 devices only). A longer report with status `FE` is never read either.

**Idle.** Event `00` is "idle": with a 1-byte parameter, the state variable that parameter's
event belongs to returns to idle; without parameters, **every** state of that notification type
returns to idle. A state variable changes when a new event of the same variable arrives (motion
detected, then idle). Some variables have **no idle** state: they only change between their two
events (door open `16` / closed `17`; mains lost `02` / restored `03`).

**Events** the panel understands (the rest of the Z-Wave Alliance table is ignored). "State"
rows belong to a state variable; "event" rows are momentary.

| Type | Event | Meaning | Kind | Variable |
|---|---|---|---|---|
| `01` Smoke | `01`, `02` | smoke detected (01 with location) | state | sensor |
| | `03` | smoke alarm test | state | alarm |
| | `04`, `05` | replacement required, end of life | state | maintenance |
| | `06` | alarm silenced | state | alarm |
| | `07` | maintenance due (periodic inspection) | state | periodic inspection |
| | `08` | dust in device | state | dust |
| `02` CO | `01`, `02` | CO detected | state | sensor |
| | `03` | CO test (parameter `01` test OK, `02` test failed) | state (no idle) | test |
| | `04`, `05` | replacement required, end of life | state | maintenance |
| | `06` | alarm silenced | state | alarm |
| | `07` | maintenance due (periodic inspection) | state | periodic inspection |
| `04` Heat | `01`, `02` | overheat detected | state | heat sensor |
| | `05`, `06` | underheat detected | state | heat sensor |
| | `03`, `04` | rapid temperature rise | event | |
| | `07` | heat alarm test | state | alarm |
| | `08` | replacement required, end of life | state | maintenance |
| | `0A` | maintenance due: dust in the device | state | dust |
| | `0B` | maintenance due: periodic inspection | state | periodic inspection |
| `05` Water | `01`, `02` | water leak detected | state | sensor |
| | `03`, `04` | water level dropped | event | |
| | `06`..`09` | flow, pressure, temperature, level alarm | state | their own |
| | `0A`, `0B` | sump pump active, sump pump failure | state | pump |
| `06` Access Control | `16` | window/door open (parameter `00` regular, `01` tilted) | state (no idle) | door |
| | `17` | window/door closed | state (no idle) | door |
| | `18`, `19` | handle open, handle closed | state (no idle) | handle |
| | `0B` | lock jammed | state | lock |
| `07` Home Security | `01`, `02` | intrusion | state | sensor |
| | `03` | tampering, cover removed | state | cover |
| | `04` | tampering, invalid code | event | |
| | `05`, `06` | glass breakage | event | |
| | `07`, `08` | motion detected | state | motion |
| | `09` | tampering, product moved | event | |
| | `0A` | impact detected | event | |
| | `0B` | magnetic field interference | state | magnetic |
| | `0C` | RF jamming detected | event | |
| `08` Power Management | `02`, `03` | AC mains disconnected / re-connected | state (no idle) | mains |
| | `0A`, `0B` | replace battery soon / now | state | battery |
| | `0E`, `0F` | charge battery soon / now | state | battery level |
| | `10`, `12` | backup battery low / disconnected | state | backup battery |
| `09` System | `01`, `03` | hardware failure (`03` with a manufacturer's code) | state | hardware |
| | `02`, `04` | software failure (`04` with a manufacturer's code) | state | software |
| | `05` | heartbeat | event | |
| | `06` | tampering, cover removed | state | cover |
| `0A` Emergency | `01` | contact police | event | |
| | `02` | contact fire service | event | |
| | `03` | contact medical service | event | |
| | `04` | panic alert | event | |
| `10` Weather | `03` | freeze alarm | state | freeze |
| `12` Gas | `01`, `02` | combustible gas detected | state | combustible |
| | `03`, `04` | toxic gas detected | state | toxic |
| | `05` | gas alarm test | state | alarm |
| | `06` | replacement required | state | maintenance |

### 4.6 Battery (`80`)

| Cmd | Name | Payload |
|---|---|---|
| `02` | Get | - |
| `03` | Report | `level` (`00..64` = 0-100 %, `FF` = **battery low warning**) · [v2: `flags1` · `flags2`] |

v2 `flags1`: bits 7..6 charging (`00` discharging, `01` charging, `10` maintaining), bit 5
rechargeable, bit 4 backup battery, bit 3 overheating, bit 2 low fluid, bits 1..0 replace or
recharge (`01` soon, `10` now). `flags2`: bit 1 low temperature, bit 0 disconnected. A level
above `64` other than `FF` is invalid (drop the report).

### 4.7 Wake Up (`84`)

| Cmd | Name | Payload |
|---|---|---|
| `04` | Interval Set | `interval` (3 bytes, seconds; 0 = only woken by hand) · `node ID to notify` (1 byte) |
| `05` | Interval Get | - |
| `06` | Interval Report | `interval` (3) · `node ID` (1) |
| `07` | Wake Up Notification | - (the node is awake now; it listens for a few seconds) |
| `08` | No More Information | - (the panel is done: the node may sleep) |
| `09` | Interval Capabilities Get (v2+) | - |
| `0A` | Interval Capabilities Report | `min` (3) · `max` (3) · `default` (3) · `step` (3) · [v3: bit 0 wake-up on demand] |

### 4.8 Association (`85`)

| Cmd | Name | Payload |
|---|---|---|
| `01` | Set | `group` · node IDs... |
| `02` | Get | `group` |
| `03` | Report | `group` · `max nodes` · `reports to follow` · node IDs... |
| `04` | Remove | `group` · node IDs... (none = all) |
| `05` | Supported Groupings Get | - |
| `06` | Supported Groupings Report | `number of groups` |
| `0B` | Specific Group Get (v2) | - |
| `0C` | Specific Group Report | `group` |

Group 1 of a Z-Wave Plus device is its **lifeline**: where it sends its reports. The panel puts
itself there (network.md "Interview").

### 4.9 Multi Channel Association (`8E`)

Same commands and numbers as Association (`01` Set, `02` Get, `03` Report, `04` Remove, `05`
/`06` Supported Groupings). Destinations: `node IDs... | 00 (marker) | (node ID, endpoint)...`,
endpoint byte: bit 7 set = bit mask of endpoints 1..7, else one endpoint 0..127. The Report is
`group · max nodes · reports to follow · destinations`. The panel sets `group 1 → 00 marker
01 00` ("node 1, endpoint 0") when the node has endpoints and no endpoint supports associations
itself; otherwise a plain Association to node 1.

### 4.10 Association Group Information (`59`)

| Cmd | Name | Payload |
|---|---|---|
| `01` | Name Get | `group` |
| `02` | Name Report | `group` · `length` · UTF-8 name |
| `03` | Info Get | `flags` (bit 7 refresh cache, bit 6 list mode) · `group` (0 in list mode) |
| `04` | Info Report | `flags` (bit 7 list mode, bit 6 dynamic, bits 5..0 count) · per group 7 bytes: `group · mode (00) · profile (2) · reserved · event code (2)` |
| `05` | Command List Get | `flags` · `group` |
| `06` | Command List Report | `group` · `length` · CC/command pairs |

Profile `0001` = "General: Lifeline". A non-Plus device without a known lifeline: ask Info
Get for group 1; profile `0001` makes it the lifeline.

### 4.11 Version (`86`)

| Cmd | Name | Payload |
|---|---|---|
| `11` | Get | - |
| `12` | Report | `library type` · `protocol major` · `minor` · `firmware 0 major` · `minor` · [v2: `hardware version` · `number of other firmware targets n` · n x (`major` · `minor`)] |
| `13` | Command Class Get | `CC` |
| `14` | Command Class Report | `CC` · `version` (0 = not supported) |
| `15` | Capabilities Get (v3) | - |
| `16` | Capabilities Report | bit 0 Version, bit 1 CC, bit 2 Z-Wave Software Get |

### 4.12 Manufacturer Specific (`72`)

| Cmd | Name | Payload |
|---|---|---|
| `04` | Get | - |
| `05` | Report | `manufacturer ID` (2) · `product type` (2) · `product ID` (2) |

The three numbers identify the model for the quirks table and the model name (zones.md).

### 4.13 Z-Wave Plus Info (`5E`)

| Cmd | Name | Payload |
|---|---|---|
| `01` | Get | - |
| `02` | Report | `version` · `role type` · `node type` · `installer icon` (2) · `user icon` (2) |

Role types: `00` central static controller, `01` sub static controller, `02` portable
controller, `03` portable reporting controller, `04` portable end node, `05` always-on end node,
`06` **sleeping reporting end node** (most battery sensors), `07` sleeping listening end node,
`08` network-aware end node. Node type `00` node, `02` IP gateway.

### 4.14 Supervision (`6C`)

| Cmd | Name | Payload |
|---|---|---|
| `01` | Get | `flags` (bit 7 status updates wanted, bits 5..0 session ID) · `length` · encapsulated command |
| `02` | Report | `flags` (bit 7 more updates follow, bit 6 wake-up request, bits 5..0 session ID) · `status` · `duration` |

Status: `00` no support, `01` working, `02` fail, `FF` success.
- **Received** (a sensor sends its report inside a Supervision Get and waits for our answer):
  handle the inner command, then send a Report with the same session ID, more-updates 0,
  status `FF` (or `00` when the inner command was not understood, `02` when handling failed),
  duration `00`, **immediately** and with the Get's encapsulation. A sensor that gets no answer
  repeats the same Get (same session ID): answer again; handling it again is harmless because
  zone inputs only react to changes.
- **Sent** (the panel configures a node): SET-type commands to nodes that support Supervision
  are wrapped in a Get with a new session ID (per node, a wrapping counter 1..63 starting at a
  random value), status updates not requested; the Report's status is the result.

A Supervision Report with status `00` (no support) answering a Set the panel supervised ends
that Set as done (decided while building Z5: the status is not defined for Sets; the value is
read back where it matters).

### 4.15 Multi Channel (`60`)

| Cmd | Name | Payload |
|---|---|---|
| `07` | End Point Get | - |
| `08` | End Point Report | `flags` (bit 7 dynamic, bit 6 identical) · `individual count` (bits 6..0) · [`aggregated count`] |
| `09` | Capability Get | `endpoint` |
| `0A` | Capability Report | `flags · endpoint` (bit 7 dynamic, bits 6..0 endpoint) · `generic class` · `specific class` · CC list |
| `0D` | Command Encapsulation | `source endpoint` (bits 6..0) · `destination` (bit 7: bit mask; bits 6..0: endpoint) · command |

A multi-sensor with endpoints reports each input from its own endpoint; the zone input address
carries the endpoint (zones.md "Addresses").

### 4.16 CRC-16 Encapsulation (`56`)
`56 01 | command | CRC (2)`. CRC-16-CCITT (polynomial `1021`, initial `1D0F`, no reflection,
no final XOR) over `56 01 | command`. Example: `56 01 20 02` → `4D26`.

### 4.17 Multi Command (`8F`)
`8F 01 | count | count x (length · command)`. Sleeping nodes may bundle their wake-up
reports this way.

### 4.18 Indicator (`87`, phase 2)

| Cmd | Name | Payload |
|---|---|---|
| `01` | Set | v1: `value`; v2+: `indicator 0 value (00)` · `count` (bits 4..0) · count x (`indicator ID` · `property ID` · `value`) |
| `04` | Supported Get | `indicator ID` (`00` = first) |
| `05` | Supported Report | `indicator ID` · `next ID` · `mask length` (bits 4..0) · property mask |

**Identify** (v3+, "Blink its light" on the device page): `87 01 00 03 50 03 08 50 04 03 50
05 06` - indicator `50` "node identify": on/off period 0.8 s, 3 cycles, on time 0.6 s. The
blinking is on the device, not the panel's screen.

### 4.19 Device Reset Locally (`5A`)
`5A 01` (Notification): the device was factory-reset by hand and has left the network. The
panel marks it gone (network.md "Removing a device").

### 4.20 Configuration (`70`, phase 2, quirks only)

| Cmd | Name | Payload |
|---|---|---|
| `04` | Set | `parameter` · `flags` (bit 7 = restore default, bits 2..0 = size 1/2/4) · `value` (size bytes, signed) |
| `05` | Get | `parameter` |
| `06` | Report | `parameter` · `size` (bits 2..0) · `value` |

The panel never shows a parameter editor: only the quirks table sets parameters a model needs
to behave as a security sensor (zones.md).

### 4.21 Central Scene (`5B`, phase 2)
`5B 03` Notification: `sequence number` · `key attributes` (bits 2..0: `0` pressed once, `1`
released, `2` held, `3`..`6` pressed 2..5 times) · `scene number`. For key fobs and panic
buttons (zones.md). A notification repeating the previous sequence number is a retransmission:
ignore it (one press must not count twice). As built (Z10): decoded for every device; only a
quirk row's `panic_scenes` turns one into an input (zones.md §8).

### 4.22 Alarm Sensor (`9C`, legacy, phase 2)
`9C 01` Get: `sensor type`; `9C 02` Report: `source node` · `sensor type` · `state` (`00` idle,
`01..64` severity, `FF` alarm) · `seconds` (2). Types: `00` general, `01` smoke, `02` CO,
`03` CO2, `04` heat, `05` water. `9C 03` Supported Get; `9C 04` Supported Report: `number of
mask bytes` · mask, bit t = type t (bit 0 of byte 0 is the general type) - asked in the
interview's step 12 (network.md §5). The zones' fresh state asks Get per listed type (zones.md
§3); a Report, asked for or not, feeds the slot of its type: `00` the general input, `01` smoke,
`02` CO, `04` heat, `05` water (ZW-043); `03` CO2 feeds none.

### 4.23 Hail (`82`, legacy)
`82 01`: "something changed, ask me" - the panel reads the node's inputs (Notification Get /
Binary Sensor Get / Alarm Sensor Get).

### 4.24 Time (`8A`, phase 2, answered only)
`8A 01` Time Get → `8A 02` Time Report: `hour` (bits 4..0; bit 7 = clock not set) · `minute` ·
`second`. `8A 03` Date Get → `8A 04` Date Report: `year` (2) · `month` · `day`. Local time,
from the panel's clock; bit 7 set while the panel has no trusted time (docs/features/35).
As built (Z10): read when the question arrives (`zwz_set_clock`; the firmware's local time once
its clock is set - from 2025 on, the touch UI's rule). Every answer of §5 counts as a reply the
panel owes: to a sleeping device it goes while that device listens, never at its next wake-up.

### 4.25 Application Status (`22`, v1)

`01` Application Busy: `status | wait seconds` (missing wait treated as zero).
`02` Application Rejected Request: `status`. These replies apply only to an outstanding Set
from the same node and endpoint. Busy status `01` supplies the delay; other statuses use the
default delay. Neither reply is treated as successful delivery of the setting.

### 4.26 Entry Control (`6F`, v1; portable foundation)

All layouts include `6F | command`. Notification `01` carries `sequence | data type (low
2 bits) | event | data length | data`. Data is at most 32 bytes: None `00` has no bytes,
Raw `01` has 1-32, ASCII `02` has 7-bit characters with trailing `FF` padding, and MD5 `03`
has exactly 16. Builders pad ASCII to a 16-byte block; readers also accept unpadded ASCII.
Views reference the input buffer; callers own its lifetime and credential erasure.

Key Supported Get `02` / Report `03`: `mask length | mask`. Event Supported Get `04` /
Report `05`: `data-type mask length (low 2 bits) | mask | event mask length (low 5 bits) |
mask | minimum cache | maximum cache | minimum timeout | maximum timeout`. Configuration
Set `06` / Get `07` / Report `08`: Set/Report carry `cache size | timeout seconds`.
Cache limits are 1-32, timeout limits 1-10; minimum must not exceed maximum.

The portable adapter maps events `03` Disarm All, `05` Away and `06` Home to the assigned
partition's existing alarm authority with a numeric user PIN. `10` Fire, `11` Police and
`13` Medical require explicit emergency policy. Cached keys, Enter, proximity and other
events grant no authority. Duplicate handling and integration limits are in README ZW-027.
The firmware does not advertise Entry Control. Since ZW-031 it acts on the Notifications of a
keypad the installer assigned (`zw_keyctl`, at its assigned S2 class) and on nothing else.

Reference: upstream [EntryControlCC.ts](https://github.com/zwave-js/node-zwave-js/blob/master/packages/cc/src/cc/EntryControlCC.ts).

## 5. What the panel answers

The panel is node 1 of its network; devices sometimes ask it questions. It answers:

| Asked | Answer |
|---|---|
| Security 2 Commands Supported Get | security.md §9 |
| Version Get | library type `01`, the protocol version from `GetProtocolVersion`, firmware 0 = the panel's firmware version (major.minor), hardware version `01`, no other targets |
| Version Command Class Get | the version the panel implements (it answers every command of that version, no more): Z-Wave Plus Info 2, Security 2 1, Supervision 1, CRC-16 1, Multi Command 1, Version 2, Manufacturer Specific 1, Association 2, Association Group Information 1, Multi Channel Association 2, Device Reset Locally 1, Time 1 (phase 2), Security 1 only once S0 is allowed (phase 3; `0` until then); everything else 0 |
| Association Specific Group Get | group 0 |
| Association Group Information Command List Get (1) | group 1, an empty list |
| Manufacturer Specific Get | `FFFF FFFF FFFF` until the project has a manufacturer ID (open question) |
| Z-Wave Plus Info Get | version 2, role `00` central static controller, node type `00`, icons `0500` / `0500` (generic gateway) |
| Association Supported Groupings Get | 1 |
| Association Get (group 1) | group 1, max nodes 1, no destinations (the panel reports to nobody) |
| Association Set / Remove | accepted and ignored (Supervision: `FF`) |
| Multi Channel Association Get / Supported Groupings Get / Set / Remove | as Association (Set / Remove accepted and ignored) |
| Association Group Information Name Get (1) / Info Get | "Lifeline" / group 1, profile `0001` (Info Get names only the panel's one group, whatever it asks, echoing its list mode) |
| Time Get / Date Get (phase 2) | 4.24 |
| anything else | nothing (inside Supervision: status `00`, no support) |

Answers are sent with the request's encapsulation, once, without retries beyond the normal
SendData attempts. A Get-type command received broadcast or multicast is discarded before it reaches here (ZW-042,
Z-Wave JS's shouldDiscardCC; counted as "not singlecast") - except inside a Multi Command, which
shouldDiscardCC does not open: its Gets are delivered like singlecast ones.

## 6. Which answer belongs to which question

When the panel sends a Get it waits (1 s + round trip, serial-api.md 6.12) for **the** answer:
from the same node and endpoint, with the command below and the matching field. Anything else
arriving meanwhile is handled as an unsolicited report; a matching report that arrives
unsolicited while the Get waits counts as its answer (and is also handled as a report).

| Question | Answer | Must match |
|---|---|---|
| No Operation | the SendData ACK | - |
| Basic Get | Basic Report | - |
| Binary Sensor Get (type t) | Binary Sensor Report | type t (a v1 report has no type: accept it) |
| Binary Sensor Supported Get | Supported Report | - |
| Multilevel Sensor Get (type t) | Multilevel Sensor Report | type t |
| Multilevel Supported Sensor / Scale Get | the matching Supported Report | the type for the scale report |
| Notification Get (type t) | Notification Report | type t (or a v1-only report) |
| Notification Supported Get / Event Supported Get (t) | the matching Supported Report | type t for events |
| Battery Get | Battery Report | - |
| Wake Up Interval Get / Capabilities Get | Interval Report / Capabilities Report | - |
| Association Get (g) / Supported Groupings Get | Association Report / Supported Groupings Report | group g (collect "reports to follow") |
| Multi Channel Association Get (g) | its Report | group g |
| AGI Name / Info / Command List Get (g) | the matching Report | group g |
| Version Get / Command Class Get (cc) / Capabilities Get | Version Report / CC Report / Capabilities Report | cc |
| Manufacturer Specific Get | Manufacturer Specific Report | - |
| Entry Control Key / Event Supported / Configuration Get | the matching Report | valid payload, masks and limits |
| Z-Wave Plus Info Get | Z-Wave Plus Info Report | - |
| Multi Channel End Point Get / Capability Get (e) | End Point Report / Capability Report | endpoint e |
| Configuration Get (p) | Configuration Report | parameter p |
| Indicator Supported Get (i) | Indicator Supported Report | indicator i |
| Supervision Get (session s) | Supervision Report | session s |
| Security 2 Nonce Get | Nonce Report | - |
| Security 2 KEX Get | KEX Report (echo 0) | - |
| Security 2 Commands Supported Get | Commands Supported Report | - |
| Security (S0) Nonce Get / Scheme Get / Commands Supported Get | Nonce Report / Scheme Report / Commands Supported Report | - |

A **Set** without Supervision is normally done when the SendData callback says OK (and,
for S2, when no Nonce Report came back within 500 ms, security.md §6). For a node advertising
Application Status, interview and check-in Sets wait another 1 s plus the round trip: a
Rejected Request fails the attempt; Busy retries after its requested delay (default 1 s,
maximum 30 s, three tries). Only silence after that window means accepted. With Supervision
the matching session's Report gives the result; Application Status can still refuse or defer it.
