/* web_json_merge.c - see web_json_merge.h. */
#include "web_json_merge.h"

static bool move_all(cJSON *into, cJSON *from, bool object)
{
    bool ok = into != NULL && from != NULL &&
              (object ? cJSON_IsObject(into) && cJSON_IsObject(from)
                      : cJSON_IsArray(into) && cJSON_IsArray(from));
    cJSON *item;
    while (ok && (item = from->child) != NULL) {
        cJSON *moved = cJSON_DetachItemViaPointer(from, item);
        /* cJSON_AddItemToObject copies the name before it frees the member's own: passing
         * moved->string is safe, and on failure the member is left as it was. */
        const bool added = object ? moved->string != NULL &&
                                        cJSON_AddItemToObject(into, moved->string, moved)
                                  : cJSON_AddItemToArray(into, moved);
        if (!added) {
            cJSON_Delete(moved);
            ok = false;
        }
    }
    cJSON_Delete(from); /* NULL-safe */
    return ok;
}

bool web_json_move_members(cJSON *into, cJSON *from)
{
    return move_all(into, from, true);
}

bool web_json_move_elements(cJSON *into, cJSON *from)
{
    return move_all(into, from, false);
}
