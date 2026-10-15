# backup_service

**Tier:** services (ESP-IDF, board-agnostic) · **Features:**
[14 Configuration and installer programming](../../../docs/features/14-Configuration-and-Installer-Programming/README.md)
(CFG-005, CFG-006), [31 Browser Panel Manager](../../../docs/features/31-Browser-Panel-Manager/README.md) ·
**Tests:** the format, policy and check are host-tested in [`components/core/backup`](../../core/backup);
the Panel Manager's side in `components/services/panel_web/test/panel_pages.test.cjs`; on a
panel, `backup check`

The panel's backup file on the panel: `backup_service_make` reads every NVS entry the policy
keeps (`backup_policy_keeps`) into a payload and seals it with the installer's password;
`backup_service_restore` refuses unless every partition is disarmed and quiet, decrypts and
checks the whole file (`backup_check`, against this board's id), and only then logs
`backup_restored`, stops the engine's own NVS writes (as a factory reset does), replaces every
entry the policy keeps namespace by namespace, and restarts two seconds later. Making one logs
`backup_made`. The crypto port is PSA: PBKDF2-HMAC-SHA256 through `psa_key_derivation`,
AES-256-GCM through `psa_aead_*`, the salt and nonce from `psa_generate_random`.

The Panel Manager is the only caller (`POST` / `PUT /api/v1/backup`, the installer with their
own code: `components/services/panel_web`).

## Public API
| Header | What it offers |
|---|---|
| `include/backup_service.h` | `backup_service_set_board()` (the app passes `board_info()->id`), `backup_service_make()`, `backup_service_restore()`, `backup_service_self_test()`, `backup_service_console_register()` (`backup check`) |

## Depends on
- Project components: public `alarm_core`, `backup`; private `platform_esp` (the alarm and zone
  services, the event log's flush).
- ESP-IDF: private `console`, `esp_app_format`, `esp_timer`, `mbedtls` (PSA), `nvs_flash`,
  `esp_system`.

## Options
None: every firmware can make and restore its backup.

## Resources
- Runs on the caller's task (the Panel Manager's worker). Making a backup: a 256 KB payload
  buffer and a 64 KB value buffer in PSRAM for the call, then the file. Restoring: the payload,
  a `backup_scratch_t` (a configuration and a zone hub) and a 64 KB string buffer in PSRAM.
  Every buffer that held secrets is zeroed before it is freed.
- The key derivation (`BACKUP_ITER_DEFAULT` iterations) takes seconds; `backup check` says how
  long on this panel.

## Rules
- Nothing is written before the whole file checked out; once writing starts the panel restarts
  whatever happens (the engine no longer stores its state), and a restore that failed part way
  is restored again.
- Never widen what is read or written here past `backup_policy_keeps`: the policy is the
  contract (docs/features/14 CFG-005).
- A power cut while the entries are written leaves some namespaces restored and others not;
  the window is well under a second, and the file restores again.
