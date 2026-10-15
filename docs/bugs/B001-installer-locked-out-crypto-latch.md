# B001 — Crypto resource failure locks out installer entry / disarming

Status: **Fixed and tested; normal firmware flashed to COM4 at ~15:44 on 2026-09-20. Internal RAM pressure substantially reduced.** The exact historical 104 KB → 9 KB drop has not been reproduced. Do not describe that particular drain as a proven leak or a fully diagnosed trigger.
Opened: 2026-09-20. Target: CrowPanel P4 v1.3 on COM4, firmware 0.6.0.

## Original failure and evidence

After the code-review remediation was flashed, an incorrect installer code was entered a couple of times, then the correct code. The panel said **“Disarm first, and silence any alarm”** despite being disarmed and READY.

The earlier device investigation captured internal RAM dropping from 104 KB (largest block 31 KB) to 9 KB (largest block 7 KB), then:

```text
E code_hash: PBKDF2 self-test could not run (c=1, psa -141)
E code_hash: code hashing failed (psa -137): codes cannot be verified or changed
I alarm: #312 invalid_code p1 z0 u0
```

`-141` is `PSA_ERROR_INSUFFICIENT_MEMORY`; `-137` is `PSA_ERROR_BAD_STATE`.
The original `fail_closed()` cleared `s_ready`. Every subsequent code attempt then reran
all three known-answer tests, including 4096 PBKDF2 iterations, under the same resource
pressure. A transient failure became a persistent authentication outage. The service returned
`BAD_STATE`, which the installer UI incorrectly interpreted as an armed/alarming panel.

The first fix, flashed at 15:07, retained readiness and added `ALARM_ERR_UNAVAILABLE` plus
honest UI text. That addressed the latch but checked the failure counter **after** the engine
returned. It could still count resource failures toward keypad lockout, or store an unusable
new credential before reporting failure.

## Completed follow-up

- `alarm_ports_t.hash_faults` lets the engine sample the platform counter immediately around
  each hash on its owning task. A failure returns `UNAVAILABLE` before comparison, invalid-code
  events, lockout accounting, or credential mutation/persistence. Infallible host hashes may
  omit the callback; the ESP platform supplies it.
- Installer entry, authentication, disarm and other authenticated commands share this gate.
  Both user-code creation/change paths reject failed new hashes without touching the old code.
  A failed default-code warning check retains the previous warning.
- Removed service-level post-operation counter checks, which were too late and could include
  another caller's operation. Startup self-tests and default-user creation remain gated.
- A transient failure still leaves `code_hash_status()` ready. Each failure logs internal free
  bytes and the largest block to make another occurrence diagnosable.
- No stored hash format, salt, iteration count, user code, partition table, signing key or C6
  firmware was changed. Automation never guessed or submitted a real user code.

## Internal RAM changes

1. LVGL's C-library allocator sent thousands of small objects, strings and styles to internal
   RAM under the 4096-byte malloc preference threshold. A board-owned allocator now explicitly
   uses PSRAM for LVGL allocations and reallocations, including small ones. It does not consume
   internal RAM as a fallback. Display buffer capabilities remain driver-controlled.
2. The HA zone catalog (`s_ha`, 19,828 bytes) is explicitly placed in external BSS.
3. The device-list console command previously reserved 17,408 bytes of internal BSS permanently.
   It now allocates temporary PSRAM and frees it after use.
4. Task stacks, the alarm engine, synchronization objects, DMA descriptors and hardware SHA
   contexts remain internal. The MbedTLS allocation setting is unchanged: spare PSRAM alone
   does not make hardware SHA DMA allocations eligible for external memory.
5. `memory [check]` reports free/allocated bytes, largest block, minimum free bytes and allocation
   counts for both heaps, crypto readiness/faults, and optional heap integrity.

## Measurements and verification (2026-09-20)

| Test / configuration | Internal free | Largest internal block | Result |
|---|---:|---:|---|
| Original running firmware, saver decoded | 92 KB | 31 KB | Baseline captured without reboot |
| New allocator, initial connected idle | 290 KB | 204 KB | Ready, all health sources reporting |
| New allocator after five full page/saver walks | 295,551 bytes | 204,800 bytes | Heap integrity PASS; minimum 247,348 bytes |
| Reference C allocator after the same five walks | 143,335 bytes | 45,056 bytes | Heap integrity PASS; minimum 74,748 bytes |
| Final normal firmware after Home/flash stress | 295,623 bytes (288 KB) | 204,800 bytes (200 KB) | All health checks PASS; crypto ready, faults 0 |

The reference build retained the catalog/console improvements and changed only LVGL malloc /
realloc back to the original C-library policy; both comparison builds kept the backlight off.
This isolates roughly **152 KB of internal heap consumption** attributable to LVGL allocation
placement. UI pages and rendering also create temporary demand. Reference-build edits were
removed after testing.

- **20 host suites passed**, including four new CORE-USR-024 regression cases: two wrong codes
  followed by eight resource failures and successful installer entry, failed disarm followed by
  successful disarm, and failed new hashes preserving existing credentials through both APIs.
  The failing test hash deliberately returns a matching digest to prove the failure signal wins.
- **100 on-device crypto tests passed**, including 75 injected `PSA_ERROR_INSUFFICIENT_MEMORY`
  results at inner clone, outer clone and the last clone of a 1024-iteration hash. Every next
  hash matched its successful reference. No sustained per-hash heap loss; integrity PASS.
- **80-screen host UI preview passed.**
- Both v1 and v3 firmware builds passed. Full dependency rebuilds emitted existing unused
  FreeRTOS atomic-helper warnings; no new application compiler warnings.
- New-allocator dark build: 20-second flash stress erased/wrote 191 sectors, 55.9 frames/s,
  **zero late frames and zero guard trips**. Both Home and the decoded saver were exercised.
- Five page/saver walks with each allocator stabilized; no accumulating navigation leak was
  observed. These walks visit unauthenticated Settings pages and do not submit real codes.
- Final normal image has the backlight enabled and both diagnostic flags disabled. Under a
  10-second Home-screen/flash stress test, the longest redraw was 147.9 ms versus 144.3 ms in
  the reference allocator build, with zero late frames/guard trips. The normal image reported
  zero crypto faults, intact heaps, working Wi-Fi/time and every health source reporting.
  `devices` printed successfully with its new temporary PSRAM buffer.
- During the final serial capture the live UI was exercised: a wrong-code event was
  followed by a successful disarm (user 2), then installer entry and exit (user 1). Internal
  free memory stayed around 295,783 bytes and the crypto fault count stayed zero. UI activity
  grew PSRAM usage instead of consuming the internal heap. Evidence: `b001-final-soak.log`,
  events #332–336. Real codes were neither requested nor captured.

## Reproducing the diagnostic tests

Normal builds exclude fault injection. For bench use enable `CONFIG_ALARM_CRYPTO_DIAGNOSTICS=y`.
For automatic page walking also enable `CONFIG_BOARD_BACKLIGHT_FORCE_OFF=y`. Rebuild the v1
image and flash only while disarmed. Use `tools/panel_console.py` with enough capture time:

```text
memory
crypto_test 100
memory check
ui_memory_test 5
display stress 20
health
```

`crypto_test` runs isolated test credentials on the alarm task; it never uses the live engine's
stored codes. Cases cycle through normal operation and clone failure positions 1, 2 and 2048.
`ui_memory_test` is available only with the backlight forced off and refuses armed,
authenticated or active installer sessions. Restore both diagnostic flags to off afterward.

Local evidence (ignored build artifacts): `build/b001-baseline.log`,
`b001-dark-start.log`, `b001-crypto-test.log`, `b001-ui-psram.log`,
`b001-ui-reference.log`, `b001-display-reference.log`, `b001-reference-home-stress.log`,
`b001-host-tests.log`, `b001-ui-preview.log`, `b001-build-final-v1.log` / `v3.log`,
`b001-final-smoke.log` and `b001-final-soak.log`.

## Still open / limits of the result

The historical loss of about 95 KB is **not reproduced or fully attributed**. The measurements
prove excessive steady UI use of internal RAM and substantially better headroom after the move;
they do not prove that LVGL caused that exact historical drop. The tested hash and page paths
show no sustained leak. The saver intentionally retains one decoded ~1.2 MB image in PSRAM.

Earlier notes suspected once-per-second ESP-Hosted RSSI RPC failures. Current captures still
show RSSI 0 and the existing host 3.0.7 / C6 2.12.3 version mismatch, but do not reproduce the
reported stream of transmit errors or establish an associated leak. The inspected transmit
failure branch frees its packed buffer. C6 firmware was deliberately left unchanged.

A 24-hour soak has not been verified. Live disarm and installer entry were observed after the
final flash, but the historical drain remains unreproduced. If memory falls again, capture
`memory check`, `display`, `health` and the crypto failure log
before rebooting. Keep this residual investigation open; the authentication failure behavior
and the internal RAM allocation policy no longer rely on reproducing that trigger.
