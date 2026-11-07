/*
 * reset_policy.c - when the service console may factory-reset the panel (reset_policy.h, F2).
 */
#include "reset_policy.h"

#include <stddef.h>

const char *reset_policy_refusal(bool engine_running, bool locked, bool installer_code,
                                 bool erase_log)
{
    if (erase_log) {
        return "the event log is erased only from the touchscreen";
    }
    if (!engine_running) {
        return NULL; /* nothing armed to protect, no code to check: the way out */
    }
    if (locked) {
        return "refused while anything is armed, counting down or sounding";
    }
    if (!installer_code) {
        return "the installer code is needed";
    }
    return NULL;
}
