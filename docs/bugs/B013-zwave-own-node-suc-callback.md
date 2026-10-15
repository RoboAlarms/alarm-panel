# B013: Z-Wave startup waits for a callback the radio never sends

**Status:** fixed and host-tested; installed on the development panel 2026-09-26.
First-start SUC assignment is not re-run on hardware because the module already retains it;
reproducing that state would require resetting the network. Normal radio restart and bench
verification are recorded in the local handoff. **Open hardware finding:** software restart
passes, but the configured reset GPIO produces no SerialAPIStarted. Wiring/module revision
need checking; the earlier tool's PASS was stale Working status, not evidence of a reset.

## Symptom and evidence

On the first ZAC93 bench, SDK 7.18.3 accepted SetSUCNodeId for the controller's own node
with response `01`, but sent no callback. The host waited 10 seconds, retried twice and
soft-reset the radio before startup completed. Later starts skipped assignment because
GetSUCNodeId already returned the controller's node. The original startup trace and hardware
identity remain in ignored local bench evidence.

## Cause and fix

`zw_host_steps.c` used the general SetSUCNodeId callback contract even for an own-node
assignment, which completes synchronously. The startup request now sends callback ID 0,
expects only the response and checks that response for success. The fake ZAC93 now also
omits the own-node callback, so it no longer hides the hardware behavior.

`test_zw_startup.c` freezes the corrected exchange and asserts no recovery; the failed
response regression retains bounded retries and the bad-answer result. `zwz_replay` also
replays a scrubbed physical startup recording (SUC already configured) including line noise
and a timed-out partial frame.

## Related fixes and verification

- The Region help is shortened to "Match your radio". The preview checks its height beside
  "United States with Long Range", the physical radio's region that the earlier US-only
  preview missed.
- `panelctl zwave check` now requires a checksum-valid, newly captured SerialAPIStarted
  frame plus Working status after each reset. An accepted request and stale Working status
  can no longer falsely pass the reset check; regressions cover both reset paths.
- 54 host suites, the 150-screen Z-Wave preview and the simulator's 33 smoke checks pass.
- No physical sensor has been paired yet. Contact security, interview, open/close,
  tamper, battery and supervision remain hardware acceptance work.
