# 03 · Users, codes and authority

**Status:** Implemented v1 - codes, authority levels, lockout and duress in the core (M1) and
the Users pages on the touchscreen (M4), host- and preview-tested; on a panel since 2026-09-19
(docs/features/07), the installer managing every user not yet tried there (BL-024). Left: PSA
fault injection beyond the hash clone (BL-032), user management over MQTT (BL-075). · **Milestone:** M1 (core), M4 (UI) · **Depends on:** 01, 07, 14, 15

## Goal
Classic user codes with authority levels, per-partition access, duress, lockout and
safe code storage.

## Reference behaviour
A classic panel has an installer code (user 01), one system master (user 02), partition
masters, and users with authority levels 0 = standard, 1 = arm only,
2 = guest, 3 = duress, 4 = partition master. Guests may only disarm what they armed. A classic
installer code arms and disarms what it armed; here it never arms or disarms (decided
2026-10-07: the installer programs, the household arms; docs/features/01 deliberate choice 16). Users are added with
`master code + CODE (8) + user number + new code`.

## Authority levels

| Level | Arm | Disarm | Bypass | Chime | Walk test | Manage users |
|---|---|---|---|---|---|---|
| Installer (01) | no | no (ends a walk test) | yes (while disarmed) | yes | yes | everyone |
| System master (02) | yes | yes | yes | yes | yes | everyone except 01 |
| Partition master | yes | yes | yes | yes | yes | standard/arm-only/guest/duress in own partitions; own code |
| Standard | yes | yes | yes | yes | yes | no |
| Arm only | yes | no | no | yes | no | no |
| Guest | yes | only if guest armed | no | yes | no | no |
| Duress | yes, **plus a silent duress report** | yes, **plus a silent duress report** | yes | yes | no | no |

Masters and the installer have access to every partition. Everyone else has a partition mask.

## Requirements
| ID | Requirement | Verified by |
|---|---|---|
| USR-001 | Authority rules as in the table above; the installer's code, sign-in, automations and buttons never arm or disarm, from any source (CORE-USR-003) | `CORE-USR-001..003, 009, 011, 012` |
| USR-002 | Codes are unique and **4-10 digits, chosen per user** (iPhone-style). `code_min_len` (4-10, default 4) sets the minimum for new or changed codes; existing shorter codes keep working | `CORE-USR-008, 017, 018`, `CORE-CFG-002` |
| USR-003 | Codes are stored only as hashes: PBKDF2-HMAC-SHA256 with a per-panel random salt (PSA API) | platform (M2) |
| USR-010 | Hashing that fails never passes for hashing that worked: a broken PSA yields an unpredictable, never-all-zero value, so a check fails closed; the panel refuses to create or change a code while hashing is broken, and refuses to start at all rather than come up with codes nobody could enter (`platform_esp/src/code_hash.c`, [06](../06-CrowPanel-P4-Platform/README.md) P4-015) | platform; `test_users.c` (CORE-USR-024) and the console's `crypto_test` at the hash clone (06 P4-015); fault injection at the PSA calls other than the hash clone still to do (BL-032) |
| USR-004 | **Factory codes**: installer (01) **69420**, system master (02) **123456** (classic panels ship short four-digit ones). They're public (open source), so the panel reports them via `alarm_engine_default_codes()` and the UI shows a "change the default codes" reminder until both are changed. The first-boot wizard offers the change but doesn't force it | `CORE-USR-019, 020`, wizard M4 |
| USR-005 | N invalid codes from one source lock that source for T (default 5 / 300 s); lockout is reported; panic keys still work | `CORE-USR-005, 006` |
| USR-006 | Duress arms and disarms normally (a duress report whenever it arms or disarms); the event is silent locally and in Home Assistant's panel state, but published as a `duress` event | `CORE-USR-004, 026`, `test_proto.c` (MQTT-010) |
| USR-007 | Users can be added, changed and deleted from the touch UI, from classic keystrokes (`code + CODE + uu + new code + #`, `code + CODE + uu + # 0`) and over MQTT by an authorised code; a name or authority changes without a new code (`alarm_engine_user_update`) | `CORE-USR-007, 013, 021, 022`, preview; MQTT not built (left out of M5, BL-075) |
| USR-008 | Each user has a name (<= 23 chars) shown in events | core |
| USR-009 | Keyfob/keyswitch zones act as an assigned user | `CORE-USR-015, 016` |

## Design
- **Core**: `alarm_engine_user_set()`, `alarm_engine_user_delete()` and
  `alarm_engine_authenticate()` enforce all rules. The core never sees plain codes after
  hashing: `ports.hash_code(salt, code)` is called per attempt.
- **Hash on device**: PBKDF2-HMAC-SHA256, 32-byte output. The iteration count trades attempt
  latency (keep it under ~50 ms on the ESP32) against offline guessing. An honest note: a
  4-digit code space is tiny, so hashing only stops casual reading of backups and exports;
  a 6-8 digit code is 100-10,000 times harder to guess, which is why codes may be longer.
  Real protection for stolen hardware is flash and NVS encryption ([15](../15-Security-and-Hardening/README.md)).
- **When hashing breaks** (USR-010). `ports.hash_code` returns nothing, so the platform cannot
  tell the core "that failed". Instead `code_hash.c` hands back an unpredictable value that is
  never all zeros - it can match no stored hash, so a check fails closed, and it can never match
  a credential that was itself created while the crypto was broken. Since B001 a
  failure is counted (`code_hash_faults()`), not latched: the engine samples that count around
  each hash on its own task and answers `ALARM_ERR_UNAVAILABLE` ("Codes can't be checked right
  now. Try again") before it counts a wrong code, locks a keypad or stores anything
  (CORE-USR-024). `alarm_service` refuses to set a code unless hashing has passed its
  known-answer self-test (`crypto_ready`, which retries the test only while it has never
  passed), the engine's per-hash check keeps a failed hash from being stored, and
  `alarm_service_start()` refuses to start rather than come up with codes nobody could enter.
- **Lockout** per command source (keypad device number, MQTT client). The touchscreen's pad counts
  the time left down ("Keypad locked. Try again in 4:59", from `alarm_engine_lockout_left()` and
  the snapshot's `local_lockout_until`; KPD-008).

### UI flows (as built, M4: `components/ui/alarm_ui/src/ui_settings.c`)
- **Settings > Users** asks for a master, partition-master or installer code once; it is kept
  in RAM only while Settings is open and wiped when Settings closes or after 5 idle minutes (one code
  per visit is enough on a panel inside the home, and the engine still checks it on every change). The same code opens Wi-Fi and network,
  the time zone and the language (Time and language), Integrations and the event log (docs/features/14, "Who can change what"). **The
  installer code opens Users too and manages every user** (decided; a classic rule
  would limit it to the installer and master codes).
- The list shows name, number and authority; the installer and the system master see everyone,
  a partition master itself and the users of its partitions. Who can open whom is
  `alarm_config_user_manages()` (CORE-USR-025), the rule the engine and the Panel Manager use.
- **Partitions** (a user of a panel with more than one partition in use): which partitions
  their code works in, one switch each (the Panel Manager could too).
- **Browser sign-in** (docs/features/31): on the page of every user who can have one - the installer,
  a system or partition master, a standard user; not arm-only, guest or duress codes. A password
  set for somebody else at the panel is temporary: they choose their own at their first sign-in.
  In the Panel Manager the person setting it chooses: temporary, or one they keep.
- **Add a user**: the new code twice (4-10 digits), then the user page opens (a standard user
  of this keypad's partition, named "User NN"). Rename it on the on-screen keyboard.
- **User page**: name, what this code can do (standard, arm only, guest, duress, and partition
  master for system masters; fixed for 01, 02 and your own entry), change the code (twice),
  delete (confirmation; not 01, 02 or yourself).
- **Change your code** (the factory-code banner): current code, then the new one twice.
- **Setup**: shows both factory codes and offers to change them (each new code twice); skipping
  keeps them and the status bar banner stays.
- **Installer > Installer and master codes**: set either without the old one while in installer
  mode, which is how a lost installer code is replaced after the power-up recovery ([14](../14-Configuration-and-Installer-Programming/README.md)).
- **Code pad**: one dot per typed digit, iPhone-style; the check key submits, since users can
  have different code lengths.

## Test and acceptance
- Core: `components/core/alarm_core/test/test_users.c` (CORE-USR-*, the hash faults of
  CORE-USR-024 included), `test_partitions.c` and `test_arming.c` (the CORE-USR tags that concern
  them), `test_config.c` (CORE-CFG-002).
- MQTT: `components/core/alarm_proto/test/test_proto.c` (MQTT-010, the duress event).
- UI: the PC preview's Users, installer and setup shots (`host/ui_preview/main.c`).
- Panel: the console's `crypto_test` (06 P4-015).

## Open questions
- Allow standard users to change their own code (some panels do, some don't)? Currently off.
- Temporary / scheduled users: handled in [18](../18-Schedules-and-Auto-Arm/README.md) (BL-046).
- User limit: 64 on the P4, 32 on the ESP32 (Kconfig).

## Tasks
- [x] Authority model, lockout, duress, user API in the core
- [x] PBKDF2 `hash_code` port using the PSA API (M2: `platform_esp/src/code_hash.c`, 1024 iterations on PSA SHA-256)
- [x] Hashing fails closed: every PSA result checked, operations aborted, temporaries zeroized,
      credential changes gated on working crypto (USR-010)
- [x] B001: transient hash failures keep startup readiness and return `unavailable` before
      wrong-code counting, lockout or writes; the engine checks the failure counter around each
      hash (CORE-USR-024), tested for installer entry, disarm and both code-changing paths
- [x] Factory codes (69420 / 123456) + default-code reporting in the core
- [x] Variable-length codes (4-10 digits) + minimum-length policy in the core
- [x] Setup: optional code change + reminder banner (M4)
- [x] Users screens, user update without a new code (M4)
- [x] Installer sets the installer and master codes without the old ones; power-up recovery (M4)
- [ ] MQTT user management commands (optional; not in M5: codes over the network stay limited to arming and disarming) (BL-075)
- Duplicate of docs/features/06 ("Prove P4-013 and P4-014 with fault injection ... and P4-015 at
  the PSA calls other than the clone", BL-032): what USR-010 still owes.
