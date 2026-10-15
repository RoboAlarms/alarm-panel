# B014: RF contact stops updating after bypass; closed status hidden

**Status:** closed-state text fixed, host-tested, preview-checked and installed on the panel. The reported
loss of RF reception is not reproduced or resolved. Physical contact testing is pending.

## Report and evidence

A working 433 MHz door contact was reported to stop showing open/close after bypassing
and unbypassing its zone. While bypassed, its contact state was also unclear.

The physical panel retained the RF binding. Its CC1101 was receiving, with no SPI errors,
register verification errors, resets, capture overruns or jamming. Capture counters advanced,
but decoded frame counts did not during inspection. Since the current boot it had decoded
only a different, unbound transmitter. This does not establish why the bound contact was not
heard, or whether the earlier bypass caused it. Raw captures and sensor identifiers
remain in ignored local files.

## Findings and change

`cmd_bypass()` only changes the engine's bypass flag; it neither removes the source binding
nor stops the RF watch. A new source -> hub -> engine regression confirms repeated open/close
updates while bypassed and after unbypass, including immediate not-ready status when an open
zone is unbypassed. No speculative receiver or alarm-engine change was made.

`notify_zone_state()` already displayed `Bypassed · Open`, but an inactive contact said only
`Bypassed`. It now says `Bypassed · Closed` (or the device class's own inactive words), keeping
the existing bypass colour. The touchscreen and Panel Manager share this formatter.

## Verification and remaining work

- Host suites: 54/54 pass, including the new RF bypass integration regression and closed-state
  words for doors and motion sensors.
- Preview: 153 scenes and six minimal-UI checks pass. The new scenes drive an RF zone through
  the on-screen bypass/unbypass controls and check closed/open labels on both sides of
  unbypass. All three new screenshots were visually inspected; both bypass labels fit.
- P4 v1 dev firmware builds without warnings; signed image 2,625,536 bytes (55.6% of slot).
  Normal standalone preview, v3 firmware and physical bypass cycle not run in this pass.
- Installed while disarmed by signed OTA; authenticated reconnect and health probation pass.
  Final receiver RX without errors or jamming, binding preserved, all panel health working.
- Operate the same contact while capturing fresh RF frames. Compare its
  identity/code with its saved binding before considering a rebind. Then repeat the physical
  bypass/unbypass cycle and check both the visible state and receiver evidence.
