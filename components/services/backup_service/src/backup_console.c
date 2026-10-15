/*
 * backup_console.c - `backup check`: the PSA port against published known answers, and how long
 * the default key derivation takes on this panel (docs/features/14 "Test and acceptance").
 */
#include <stdio.h>
#include <string.h>

#include "backup_internal.h"
#include "backup_service.h"
#include "esp_console.h"
#include "esp_timer.h"

static int cmd_backup(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "check") != 0) {
        printf("backup check\n");
        return 1;
    }
    const bool ok = backup_service_self_test();
    printf("known answers (PBKDF2 RFC 7914, AES-256-GCM case 16): %s\n", ok ? "ok" : "FAILED");
    uint8_t key[BACKUP_KEY_LEN];
    static const uint8_t salt[BACKUP_SALT_LEN] = {0};
    const int64_t t0 = esp_timer_get_time();
    const bool derived = backup_psa_port()->pbkdf2_sha256(
        NULL, (const uint8_t *)"Harbor House 2026", 17u, salt, sizeof(salt), BACKUP_ITER_DEFAULT,
        key, sizeof(key));
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    printf("key derivation, %u iterations: %s in %lld ms\n", (unsigned)BACKUP_ITER_DEFAULT,
           derived ? "done" : "FAILED", (long long)ms);
    return ok && derived ? 0 : 1;
}

esp_err_t backup_service_console_register(void)
{
    const esp_console_cmd_t cmd = {.command = "backup",
                                   .help = "Backup file: check the crypto and time the key "
                                           "derivation",
                                   .hint = "check",
                                   .func = cmd_backup};
    return esp_console_cmd_register(&cmd);
}
