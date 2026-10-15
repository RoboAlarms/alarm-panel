# backup

**Tier:** core (portable C11) · **Features:** [14 Configuration and installer programming](../../../docs/features/14-Configuration-and-Installer-Programming/README.md) (CFG-005, CFG-006) ·
**Tests:** [`test/`](test/) (1 suite: `backup`)

The panel's backup file: one password-encrypted file with everything needed to set a panel up
again, or a replacement board of the same kind, and nothing that is the panel itself.

- **The file** (`backup.h`): a 44-byte header (`"RABK"`, format, PBKDF2 iterations, salt,
  nonce, payload length), then the payload encrypted with AES-256-GCM under a key from
  PBKDF2-HMAC-SHA256 of the installer's password. The header is the AEAD's associated data, so
  a changed iteration count or length fails like a wrong password. `backup_seal` takes a fresh
  salt and nonce for every file; `backup_open` gives nothing back unless the tag matches, and
  refuses a file asking for more work than a panel will do before deriving anything.
- **The payload**: records - one META (when, which firmware, which kind of panel), one ENTRY per
  NVS entry (namespace, key, NVS type, value), END with the count. Unknown record types are
  skipped, so a later format can add some.
- **The policy** (`backup_policy.c`): which NVS entries a backup holds, as a table. The same
  table decides what a restore replaces: an entry it keeps is erased on the panel unless the
  file has it; one it doesn't keep - the panel's identity and keys, the developer enrollment,
  counters, anything in flight - is never read, written or erased. An entry older firmware
  kept and this one doesn't (`backup_policy_retired`: "ota"/"src") is accepted in a file and
  left out of the restore; `backup_restore_entry` says what a restore writes for each entry -
  the update server's record ("ota"/"trust") comes back without its accepted signing key, which
  the installer confirms again (docs/features/13 OTA-020).
- **The panel's entries** (`backup_store.c`, a port `backup_store_t` over NVS):
  `backup_collect` reads a namespace's kept entries into a backup, `backup_erasable` lists what a
  restore erases first. Only "nothing there" is absence: a namespace that can't be opened,
  listed or read fails the step, never reads as empty.
- **The check** (`backup_check.h`): a decrypted file checked in full before a restore writes
  anything - well formed, made on this kind of panel, only entries the policy keeps, each at
  most once, the configuration decoding and validating as the engine would take it
  (`alarm_config_decode`), the zone bindings decoding (`zone_hub_decode`). It says what the
  file would bring back (zones, users, the Wi-Fi network, Z-Wave keys).

The cryptography is a port (`backup_crypto_t`): the panel's is PSA
(`components/services/backup_service`), the host tests' is written from the standards in
`test/backup_host_crypto.c`.

## Public API
| Header | What it offers |
|---|---|
| `include/backup/backup.h` | the format's constants, `backup_crypto_t`, `backup_writer_*`, `backup_reader_*`, `backup_seal`, `backup_seal_with`, `backup_open`, `backup_peek`, `backup_password_ok`, `backup_policy_keeps`, `backup_policy_namespaces`, `backup_policy_retired`, `backup_restore_entry`, `backup_store_t`, `backup_collect`, `backup_erasable`, `backup_result_name` |
| `include/backup/backup_check.h` | `backup_check` (with `backup_scratch_t` and `backup_summary_t`) |

## Depends on
- Project components: `alarm_core` (the configuration codec), `zone_hub` (the binding codec),
  `alarm_proto` (the update server's trust record, private).
- ESP-IDF: none (portable).

## Options
None.

## Rules
- Plain C11: no malloc, no globals, no RTOS or ESP-IDF headers; the host build stays warning-free.
- The file format is an external contract: a file one firmware writes must restore on a later
  one. Never renumber a header field, a record type or a value type; add record types. When the
  format changes, add a new frozen fixture next to `test/backup_fixture_v1.h` (never edit one).
- A new NVS namespace a service stores something in: add it to the policy only when an
  installer would want it back on a replacement board, never when it is about this board or
  this moment, and extend the policy test.

## Tests
`backup` (`test_backup.c`, tags `CFG-005`, `CFG-006`): PBKDF2 against RFC 7914's vectors, AES-256
and GCM against FIPS-197 and the GCM specification's test cases (both also computed with
Python's `hashlib` and `cryptography`); every NVS type in and out; the writer refusing what NVS
couldn't take back; damaged payloads refused and later record types skipped; seal and open,
fresh salt and nonce per file, the password rules; the wrong password or any changed byte
giving nothing; files that aren't backups refused before any work; a failing crypto port
making no file; the frozen format-1 file (made by Python) opened and made again byte for byte;
the policy (the installation in, the identity out; the update server and channel in, the old
source retired); the check accepting a panel and an old file with "ota"/"src", and refusing
another board, an identity entry, a repeated entry, a bad or missing configuration; the update
server restored without its key; reading and listing through a fake store that fails at open,
at the first or a later entry, or at a read (S04).
