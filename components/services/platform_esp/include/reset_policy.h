/*
 * reset_policy.h - when the service console may factory-reset the panel (F2, docs/features/15).
 * Someone with a laptop and a USB cable in the entry delay must not be able to wipe the panel back
 * to its factory codes, with nothing armed and no history. So, at the console:
 *   - never while anything is armed, counting or sounding (alarm_snapshot_locked());
 *   - only with the system master's or the installer's code, as on the touchscreen (the
 *     owner's decision of 2026-11-08: no code outranks the Owner);
 *   - never the event log ("log"): the touchscreen's reset keeps its own rules for that;
 *   - and, the one way out, without a code when the engine isn't running (its storage can't be
 *     read): then there is nothing armed to protect and no code to check.
 * Portable (host test: test/test_reset_policy.c).
 */
#ifndef RESET_POLICY_H
#define RESET_POLICY_H

#include <stdbool.h>

/* NULL: allowed. Else why not, in the console's words. */
const char *reset_policy_refusal(bool engine_running, bool locked, bool installer_code,
                                 bool erase_log);

#endif
