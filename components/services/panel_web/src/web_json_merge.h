/* web_json_merge - moves one cJSON document's members into another, and says when it couldn't
 * (docs/features/31; code review finding 6). Portable C on cJSON alone, so the host test
 * (test/test_web_json_merge.c) can run it with an allocator that fails. */
#pragma once

#include <stdbool.h>

#include "cJSON.h"

/* Every member of the object `from` into the object `into`, under its own name. */
bool web_json_move_members(cJSON *into, cJSON *from);
/* Every element of the array `from` onto the end of the array `into`. */
bool web_json_move_elements(cJSON *into, cJSON *from);
/* Both: `from` is consumed (deleted) whatever happens, and a member that couldn't be added is
 * freed, never left detached. false when either is NULL or not the expected kind, or an
 * allocation failed: `into` then holds part of `from` and the caller drops its whole document
 * (the route answers 503 "Memory busy"). */
