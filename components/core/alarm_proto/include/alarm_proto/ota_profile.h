#pragma once
#include <stdbool.h>
#include <string.h>

/* Signed esp_app_desc.project_name is the board + profile contract. DEV-004/015.
 * Release is deliberately exact-match; a version override cannot change this.
 * Part of alarm_proto's firmware-update helpers since docs/features/33 step 8 (it was in
 * components/services/panel_ota); tested in test/test_fw.c. */
static inline bool ota_profile_accepts(const char *running, const char *candidate)
{
    if (strcmp(running, candidate) == 0) return true;
    const size_t n = strlen(running);
    return n > 4 && strcmp(running + n - 4, "-dev") == 0 &&
           strlen(candidate) == n + 4 && strncmp(running, candidate, n - 4) == 0 &&
           strcmp(candidate + n - 4, "-release") == 0;
}
