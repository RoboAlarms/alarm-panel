# 15 · Security and hardening

**Status:** In progress - signed firmware updates are in every build (M6, verified on the panel
2026-09-19), codes are hashed with PBKDF2 on PSA, the installer-gated Panel Manager and
developer access (docs/features/31) have been on a P4 since 2026-09-22, and the `release`
profile is built. Left: the `hardened` profile, the rest of the PSA helpers and a log scrubbing
check (BL-049), private vulnerability reporting on the public repository (BL-098) · **Milestone:** all · **Depends on:** 03, 10, 11, 13, 14

## Goal
Make the panel hard to defeat and safe to put on a home network, without cloud dependencies and
without making DIY development painful. Be honest about the limits of hobby hardware.

## Threat model

| Attacker | Goal | Main controls |
|---|---|---|
| Intruder at a keypad | Guess a code, silence the alarm | Lockout per keypad, entry time, disarm needs a code, duress code, tamper, UI never reveals silent alarms |
| Intruder who breaks or unplugs a keypad | Stop the alarm | Keypads are terminals: the master keeps running; keypad supervision trouble; master placed out of reach |
| Intruder who cuts power / Wi-Fi | Blind the system | Battery; ESP-NOW doesn't need the router (until it exists, a Wi-Fi keypad needs the router: put it on backup power, docs/features/11); `network`/`comm_fail` troubles; local siren still works |
| Intruder with a keypad in hand for a while | Read the link's keys, act as that keypad | The pairing key is in plain NVS (SEC-009): remove the keypad at the master and pair it again; a keypad holds no codes, and the master checks every code and locks the source out (docs/features/11 "Security notes"). Once BL-216 is built (designed 2026-10-16), the keys also reach that keypad's Smart Home: what Home Assistant's options allow, an unlock or an opening only with a valid code for its home partition (lockout after 5; the owner's decision 1, recommended, not decided), and a forged settings report that changes only what the master and Home Assistant show about that keypad (docs/features/11 LINK-047, LINK-048) |
| Someone who controls the paired Home Assistant | Change how a panel looks or sounds (designed 2026-10-16, BL-216) | No code: anyone at a panel may change these settings, on every panel, the master and each keypad. Only Screen and sound settings, never codes, role, pairing, Wi-Fi, time or updates (docs/features/11 LINK-052). The panel checks every value, refuses while a person has Screen and sound open, holds changes during an alarm, entry time or exit time, and changes how the screen looks at most once per 2 s (docs/features/07 UI-046). Beeps can go to 0, which silences entry beeps, never alarms. The master's own settings, changes while armed and Beeps at 0 are the owner's decisions 7, 4 and 6 (docs/features/11 Open questions): recommended as written here, not decided |
| Attacker on the home LAN | Sniff codes, spoof commands, fake status | MQTT over TLS with CA pinning + auth; link frames encrypted and replay-protected; commands need user codes; lockout for remote sources |
| RF attacker | Jam or replay keypad/sensor traffic | AEAD with sequence numbers; supervision (missing check-ins -> CHECK); jam detection on the sensor radio (M10) |
| Malicious firmware | Backdoor | Signed OTA; pinned dependencies; reproducible CI builds; optional Secure Boot v2 |
| A faulty or compromised master | Push a wrong, older or unsigned image to its keypads, or keep them busy | A keypad checks every image itself: the header before any flash write, then length, SHA-256, its own signing keys and the version named; nothing forced or older; no trust change without a person at the keypad; at most 3 failed runs a day (docs/features/11 LINK-033, LINK-035, LINK-036) |
| Thief with the panel | Read codes, Wi-Fi password, keys | Codes hashed; flash + NVS encryption in production builds |
| Curious household member | Read the event log / duress events | Silent events visible to master/installer only |

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| SEC-001 | The only defaults are the factory codes (installer 69420, master 123456). While either is unchanged the panel shows a reminder and reports it (UI banner, MQTT status). Wi-Fi, MQTT and other passwords have no defaults | `CORE-USR-019`, UI/MQTT review |
| SEC-002 | User codes stored as PBKDF2-HMAC-SHA256 with a random per-panel salt | unit |
| SEC-003 | All cryptography uses the PSA Crypto API (mbedTLS 4 in ESP-IDF 6.x) with hardware acceleration | review |
| SEC-004 | Link: AES-128-CCM, per-keypad directional keys from ECDH + HKDF, 64-frame replay window, SAS-confirmed pairing | host tests |
| SEC-005 | MQTT: TLS 1.2+ with CA pinning; plain MQTT only with an explicit insecure option | integration |
| SEC-006 | OTA: RSA-3072-signed images (signed apps without eFuse Secure Boot, verified by `esp_ota_end()` against the running app's key); the manifest is unsigned (it can only point at signed images, and older ones need a person's `force`); rollback on failed self-test ([13](../13-OTA-Updates/README.md)) | HIL 2026-09-19: tampered image and an image signed with another key both refused |
| SEC-007 | Normal firmware includes HTTPS Panel Manager and developer resources behind separate persistent Installer switches, default off. Web-off stops both services; Developer-off stops engineering access. Require authenticated sessions/mTLS, CSRF and existing authority. Release excludes the physical service console and diagnostic injection | release/link-map checks; dark HIL listener shutdown/session revocation and restore |
| SEC-008 | Secrets never logged; debug logs strip codes and keys | review + grep test |
| SEC-009 | Production profile: flash encryption (release mode) + NVS encryption (HMAC-based on the P4) + Secure Boot v2, documented with warnings (eFuse changes are irreversible) | doc + HIL on a spare board |
| SEC-010 | Dependencies pinned (`dependencies.lock`), CI builds from clean checkouts | CI |
| SEC-011 | Vulnerability reporting policy (`SECURITY.md`) before the first public release | doc |

## Build profiles

**Feature 31 runtime access:** normal firmware includes panel-hosted HTTPS
administration and separately gated developer resources. Fresh Web server and Developer mode
settings default off. Authenticated local Installer controls them; enabled modes persist, and
disabling web stops both listeners and revokes browser sessions. Existing explicitly enrolled
Feature 30 panels migrate enabled to preserve remote access.

Browser sessions require system-master/installer authentication, bounded lifetime, same-origin
HTTPS, CSRF and account proof revalidation. Developer upload, workstation enrollment and logs
additionally require Developer mode. Enrollment uses a public client certificate; its private
key stays on the workstation. An enrolled mTLS developer can request a 60-second, one-use
browser grant. Optional named helper accounts/passkeys remain future work.

The `legacy`, `dev` and `release` profiles retain distinct signed project identities. Network
updates require an exact current profile match; version forcing cannot bypass it. Release
excludes the physical service console and diagnostic injection. Runtime developer access is
allowed, and successful OTA retains trust and certificates. Dark release images are allowed
for remote acceptance, but the packaging checker refuses one. No profile enables eFuses or
flash encryption. This supersedes Feature 30's original release-service exclusion.

| Profile | Signed OTA | Flash/NVS encryption | eFuse Secure Boot | Console |
|---|---|---|---|---|
| `legacy` (default) | required | off | off | existing configuration |
| `dev` (explicit) | required | off | off | enabled, physical USB enrollment/revocation |
| `release` (explicit) | required | off | off | excluded |
| future `hardened` | required | intended release mode | intended v2 | disabled |

No hardened provisioning is implemented or performed by these profiles. eFuse changes require
the owner's explicit instruction and spare-board acceptance; selecting release never burns
an eFuse. Run `tools/check_release_profile.py` before packaging a release. See docs/features/30 for
hardware evidence, exact enrollment policy, artifact checks and remaining acceptance.

### The signing key
`secrets/ota_signing_key.pem` (RSA-3072, git-ignored) signs every build. Panels accept only
updates signed with the key of the firmware they run, so: back it up offline; never commit it;
use one key per fleet. Losing it means reflashing each panel over USB. Without eFuse Secure Boot
the bootloader doesn't check signatures, so someone with the USB port can still flash anything:
the case and the location of the panel are the physical protection until the `hardened` profile.

## Honest limits
- **The factory codes are published in this repository.** Anyone can look them up, so change
  both at installation. The reminder stays on until you do.
- A 4-digit code space is small; lockout is what protects it, not hashing. Longer codes (6-10
  digits) are allowed and much stronger.
- A DIY panel isn't a listed/certified alarm system, and there's no professional monitoring
  unless you add it.
- Wi-Fi and ESP-NOW can be jammed. Supervision makes jamming visible; it can't prevent it. Wired
  sirens and a battery keep the local alarm working.

## Credentials stored on the panel

The Wi-Fi password, the MQTT password, the ntfy access token and the Pushover user key and
application token are held in NVS **in plain text**. This panel deliberately runs without eFuse
flash encryption or Secure Boot (ADR-023, and AGENTS.md forbids enabling either unasked), so
anyone who can read the flash can read them. The UI and the service console never show a stored
credential back - a page says "Set", the console says `set` (docs/features/26 NOTE-022) - which keeps
them off the screen and out of a shoulder-surfer's reach, but it is not encryption and the
feature documents do not imply otherwise.

What that costs, per credential: the Wi-Fi password lets someone onto the network; an ntfy topic
or token lets them read and send the panel's alerts; a Pushover token spends the owner's monthly
allowance. None of them disarms the panel - codes are hashed with a salt, and remote arming is a
separate installer option.

The panel link (docs/features/11) keeps its pairing keys in NVS `link`, in plain text too: on the
master one per keypad, on a keypad its master's. They never go into a backup (LINK-022), a
factory reset erases them, and a session's keys live only in RAM. With a keypad's pairing key
someone can act as that keypad - send commands with codes, which the master checks - and read
what it shows; recorded traffic stays secret (each session's keys come from a fresh X25519).

## Tasks
- Duplicate of docs/features/03 (Tasks: "PBKDF2 `hash_code` port using the PSA API"):
  `components/services/platform_esp/src/code_hash.c` code_hash_pbkdf2, with its known-answer
  self-test in code_hash_init. There is no host test: the host builds use
  stand-in hashes.
- [x] The panel link's crypto (SEC-004): AES-128-CCM, HMAC/HKDF-SHA256 and X25519 on a port of
  three PSA primitives, host-tested against the RFCs and checked at every start
  (`panel_link` `plink_crypto`, `plink_service` `plink_psa.c`) (BL-048, e59c7f6)
- [ ] The rest of the PSA-based crypto helpers, with host tests: ECDSA verify (M7), and a host
  (unit) test of the PBKDF2
  code hash (SEC-002) unless the maintainers accept the start-up known-answer test instead
  (docs/features/03 USR-003 says "platform") (BL-049)
- [x] Signed OTA in the default build (M6)
- [x] `release` profile in an sdkconfig overlay (`sdkconfig.defaults.release`, `-Profile release`,
  `tools/check_release_profile.py`) (0e5a1ab)
- [ ] `hardened` profile (SEC-009): its sdkconfig overlay with flash encryption (release mode),
  NVS encryption and Secure Boot v2, its provisioning documented with the eFuse warnings, then
  tried on a spare board only when asked for (BL-049)
- [ ] Log scrubbing check (SEC-008, M5): a grep test that no log call prints a code, key, token
  or password (`tools/zwtrace_scrub.py` only makes recorded Z-Wave traces safe to commit as
  fixtures; nothing checks the firmware's log calls) (BL-049)
- [x] `SECURITY.md`: private reporting through GitHub, scope, known limits.
- [ ] Turn on private vulnerability reporting in the public repository's settings when it is
  created (SEC-011) (BL-098)
