# B021: a Zigbee temperature sensor joins, then leaves 15.7 s later, over and over

**Status: fixed in the panel and the H2's firmware (a836424), host-tested, on board 3
(2026-10-04); waits for the bench pairing.** The radio's pending table never let an address go,
and frames to a sleepy device didn't say more were coming; both are fixed, and the panel no longer
asks the device anything during its own key exchange. Backlog: BL-156.

## Symptom
A battery Zigbee temperature and humidity sensor sold as "Zigbee Temp X02" never finishes
pairing. Its light keeps blinking blue as if it were still looking for a network. The panel adds
it, says "setup stopped before it finished", and the device stays at setup step 1 (its node
descriptor). A SONOFF SNZB-04PR2 door contact paired to the same panel works.

## Evidence
**Bench trace, 2026-10-03.** `zigbee trace on` ran for four minutes while the sensor was paired. The panel ran 6da5940's content on channel 11. The channel's energy read -94 to -118 dBm,
with two short readings near -62 dBm.
- The sensor joins with the Zigbee 3.0 well-known key (`join=2`) and the panel as its parent. It
  sends its Device_annce (`mac=0x80`, a sleepy end device), so it has the network key.
- The stack never raises its authorization signal for it, neither success nor failure (no
  `why=` report). The panel's report of the join comes from the announce instead.
- The panel's Node_Desc_req times out after 5 s every time (`step=1 result=1`): six attempts in
  four cycles, none answered.
- **15.66 s after each announce**, the sensor leaves without rejoin (a leave indication of type
  reset, `asked=0`: the panel didn't ask it to). About 4 s later it joins again with a new short
  address. The cycle repeats while joining is open.
- The radio co-processor logged `ChannelAccessFailure` twice, each within 3 s of a Node_Desc_req
  the panel sent.
- An earlier attempt the same evening got one answer through: its node descriptor gave
  manufacturer code 4742 (0x1286, the eWeLink family, as on the SONOFF contact) and stack
  revision 22 (`server_mask 0x2C00`).

## What it points to
**The sensor's own key exchange fails, and it leaves (high confidence).** A joiner at stack
revision 21 or later runs the Trust Center link-key exchange after its Device_annce (BDB v3.0.1
8.2 steps 11-12 and 10.2.5). It asks the trust center for its node descriptor, waits 5 s, and tries
3 times. Then it asks for a key of its own and verifies it. On failure it leaves with Rejoin=FALSE
([BDB v3.0.1](https://csa-iot.org/wp-content/uploads/2022/12/16-02828-012-PRO-BDB-v3.0.1-Specification.pdf)).
3 x 5 s fits the 15.66 s seen here; Telink's SDK (about 21 s), TI's Z-Stack (about 20 s) and a ZBOSS
joiner (20 s) don't, so the sensor's stack is unknown. In esp-zigbee-lib 2.0.4 the authorization
signal fires only when the trust center receives Verify Key, with no timeout or failure path
(found by disassembly). Its absence means Verify Key never arrived; the panel needs its own timer
to notice a failed exchange.

**The trust center's policy is not the cause (high confidence).** Every coordinator that
Zigbee2MQTT and ZHA drive leaves the exchange optional and answers key requests:
- herdsman's ZBOSS adapter sends `LINK_KEY_REQUIRED` 0
  ([driver.ts](https://github.com/Koenkk/zigbee-herdsman/blob/03495312e131c705a7cabc07fd3c403f2ddf7e42/src/adapter/zboss/driver.ts#L118-L123));
- zigpy-zboss has `tc_policy_unique_tclk_required` false;
- Ember (herdsman and bellows) sets `ALLOW_TC_KEY_REQUESTS_AND_SEND_CURRENT_KEY` with no
  requirement;
- TI's Z-Stack has `BDB_DEFAULT_TC_REQUIRE_KEY_EXCHANGE FALSE`, and the Z2M firmware patch keeps it.

esp-zigbee-lib's trust center already writes "not required" for itself and answers Request Key
on its own. The panel's policy is the same as theirs.

**What differs is the split radio (medium-high confidence).** Those coordinators run the MAC and
the queue for sleepy devices on the radio chip. The panel runs ZBOSS on the P4 and the H2 as an
OpenThread RCP over Spinel. Espressif's tracker documents frame-delivery faults to sleepy devices
in that layout:
- **Queued frames go out late.** 45-55 ms after the device's poll with library 2.0.3, against
  about 20 ms that devices listen
  ([esp-zigbee-sdk#864](https://github.com/espressif/esp-zigbee-sdk/issues/864)).
- **Frame Pending is missing from MAC data frames in v2.x** (Espressif's words). Their RCP patch
  made a key exchange complete, and a fix is promised for the release after 2.0.4
  ([esp-zigbee-sdk#893](https://github.com/espressif/esp-zigbee-sdk/issues/893)). In #829's sniffer
  capture, Confirm Key never reached a device that polled about once a second.
- **The RCP's source-match table only grows.** The library's single-entry clears link to ESP-IDF
  v6.1.0's weak stubs, which do nothing. The RCP's add reports success even when the H2's 20 slots
  are full. Once full, new addresses get Frame Pending 0, and every failed attempt takes a new
  address. ESP-IDF release/v6.1 has the real clears (commit
  [68d2d8c4](https://github.com/espressif/esp-idf/commit/68d2d8c4e17989c09e51f2cb8b70be89553236d4)).
  A restart empties the table. The bench's attempt at 18:36 seems to have been the first join
  since the panel restarted at about 18:27, and it failed too. If so, this isn't the only cause.
  (Corrected 2026-10-04: the do-nothing clears are esp-zigbee-lib's own weak stubs, in its
  `esp_zigbee_plat_radio_spinel.c`, and they return `ESP_ERR_NOT_SUPPORTED`.)

`ChannelAccessFailure` doesn't prove interference: the H2's driver reports it when asked to send
while it is receiving or sending an acknowledgement.

**How the interviews differ.** Zigbee2MQTT tries Node_Desc_req up to 6 times. zigpy waits 28 s
for an end device's answer. zigpy-znp waits up to 5 s for the announce. Koenkk's Z-Stack firmware
holds frames for sleepy devices longer and retries more. The panel sends one Node_Desc_req right
after the announce, and the library ends it after 5 s. That request also takes the sensor's
polls while the sensor waits for the trust center's answers.

## Fix (2026-10-04, all four candidates below but the library update)
Confirmed by disassembling esp-zigbee-lib 2.0.4: its MAC (`mac_indirect_transmit`) adds a sleepy
device's address to the radio's table when it queues a frame and, if the add fails, switches to
Frame Pending on every poll; `mac_handle_tx_done` takes the address out once the frame is out, and
puts it back if another waits. The take-out reached the stubs, so every address stayed for good.
- **The panel clears single addresses** (candidate 1, without a second ESP-IDF install):
  `radio_port.c` defines `esp_radio_spinel_clear_short_entry` / `_extended_entry` as ESP-IDF's
  commit 68d2d8c4 does, on the RadioSpinel it learns from wraps of the add calls. The map shows
  the library's stubs discarded and its clears calling ours.
- **The H2 says when its table is full** (candidate 2, `firmware/radios/h2_rcp/main/rcp_pending.c`):
  the add is refused, so the panel's stack falls back to Frame Pending on every poll.
- **Frame Pending in the H2's frames** (candidate 3): a frame to an address still in the table
  goes out with Frame Pending set, as Espressif's #893 patch, from a copy of the table kept beside
  the driver's private one; a secured frame is left alone. Only with the clears above, as the
  candidate said.
- **The panel stays out of the exchange** (candidate 4): a join told from the announce before the
  stack's authorization is `key_pending`; the setup sends nothing for 16 s or until the
  authorization (`ZSRC_ZB_REP_AUTHORIZED`, new). No retries were added: the setup's own tries
  and a sleepy device's wake waits stand. No new words on any screen.
- Tests: `test_zbz_panel.c` (`test_setup_waits_for_the_key_exchange`,
  `test_a_failed_key_exchange_is_left_alone`); the radio halves can only be tried on the bench.
- On board 3, 2026-10-04 ~04:55: both images flashed (the H2 over its USB-C), `zigbee status`
  ready with the new RCP, three `zigbee restart`s clean. No device joined yet.

## What to change (candidates, one at a time on the bench)
1. Build the P4 against ESP-IDF release/v6.1's head (the real single-entry clears). The map file
   must then show `esp_radio_spinel_clear_short_entry` from libopenthread, not the library.
2. In the H2's firmware, make the RCP's source-match add fail when the table is full, so the
   host's fallback (Frame Pending on every poll) runs.
3. Espressif's frame-pending patch (#893) in the H2's firmware: only together with 1, or every
   address ever queued keeps the bit. Never `ESP_IEEE802154_AUTO_PENDING_ZIGBEE`: it inverts
   the table's meaning.
   A stopgap instead of 2 and 3: keep the RCP in `ESP_IEEE802154_AUTO_PENDING_DISABLE` (Frame
   Pending on every poll), at a battery cost to sleepy devices.
4. Stay out of the exchange: hold the panel's Node_Desc_req until authorization or about 16 s
   after the announce, retry like Zigbee2MQTT or zigpy, and report a leave within about 16 s of
   the announce as a failed key exchange.
5. The esp-zigbee-lib release after 2.0.4, once it ships the frame-pending fix.

## What to find out
- Whether the sensor's attempt right after a restart fails (power-cycle the panel and the H2,
  pair it before anything else joins): a pass points to the full table.
- With a sniffer (a second H2 running ot_rcp, Wireshark with the well-known and network keys):
  - which frame is lost (Node_Desc_rsp, Transport Key, Confirm Key);
  - whether its polls use the short or extended address;
  - the Frame Pending bit in the acknowledgements and data frames;
  - how long after a poll the panel's frame goes out.
- Why the SNZB-04PR2 works: re-pair it with the same logging.
- What the sensor is: its Basic cluster's manufacturer and model, once it stays long enough. A
  rebadged Telink "TH01" is a guess, not a finding.

## How to verify
On the bench, the sensor pairs, finishes setup and reports temperature and humidity; it is
re-paired five times, and the door contact still works afterwards. `zigbee trace on` while it
joins: `key_pending=1` on its JOINED, then an AUTHORIZED report (rep=16) when its exchange ends,
or the setup's first request 16 s after the announce. If it still leaves at 15.7 s, the sniffer
questions above are next.
