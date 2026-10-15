/*
 * commands.c - commands received over MQTT: the panel's own command JSON and Home Assistant's
 * alarm panel commands (see proto.h and docs/features/10).
 */
#include <string.h>

#include "alarm/alarm_strings.h"
#include "alarm_proto/proto.h"

/* A code as the engine takes it: 1-10 digits (the engine judges length and validity, so a
 * short code counts as a wrong one toward the lockout, like on a keypad). */
static bool code_ok(const char *s)
{
    const size_t n = strlen(s);
    if (n == 0u || n > ALARM_CODE_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

/* "code": digits, "" or null (no code). */
static bool read_code(pj_reader_t *r, char out[ALARM_CODE_MAX_LEN + 1])
{
    out[0] = '\0';
    if (pj_peek(r) == PJ_NULL) {
        return pj_get_null(r);
    }
    char tmp[ALARM_CODE_MAX_LEN + 2];
    if (!pj_get_str(r, tmp, sizeof(tmp))) {
        return false;
    }
    if (tmp[0] == '\0') {
        return true;
    }
    if (!code_ok(tmp)) {
        return false;
    }
    memcpy(out, tmp, strlen(tmp) + 1u);
    return true;
}

static bool pick_name(const char *s, const char *const *names, unsigned n, unsigned *out)
{
    for (unsigned i = 0; i < n; i++) {
        if (strcmp(s, names[i]) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

static bool parse_level(const char *s, alarm_arm_level_t *out)
{
    if (strcmp(s, "home") == 0) { /* Home Assistant's name for Stay */
        *out = ALARM_LEVEL_STAY;
        return true;
    }
    for (unsigned l = ALARM_LEVEL_AWAY; l <= ALARM_LEVEL_NIGHT; l++) {
        if (strcmp(s, alarm_level_name((alarm_arm_level_t)l)) == 0) {
            *out = (alarm_arm_level_t)l;
            return true;
        }
    }
    return false;
}

static bool parse_panic(const char *s, alarm_panic_t *out)
{
    for (unsigned p = ALARM_PANIC_POLICE; p <= ALARM_PANIC_MEDICAL; p++) {
        if (strcmp(s, alarm_panic_name((alarm_panic_t)p)) == 0) {
            *out = (alarm_panic_t)p;
            return true;
        }
    }
    return false;
}

static bool parse_flags(pj_reader_t *r, uint8_t *out)
{
    static const char *const names[] = {"no_entry_delay", "silent_exit", "bypass_faulted"};
    static const uint8_t bits[] = {ALARM_ARM_NO_ENTRY_DELAY, ALARM_ARM_SILENT_EXIT,
                                   ALARM_ARM_BYPASS_FAULTED};
    *out = 0;
    if (pj_peek(r) != PJ_ARRAY) {
        pj_skip(r);
        return false;
    }
    bool ok = pj_enter_arr(r);
    while (pj_next_element(r)) {
        char s[20];
        unsigned i;
        if (!pj_get_str(r, s, sizeof(s)) || !pick_name(s, names, 3u, &i)) {
            ok = false;
            continue;
        }
        *out = (uint8_t)(*out | bits[i]);
    }
    return ok && !r->error;
}

alarm_result_t proto_cmd_parse(const char *json, size_t len, bool allow_panic, proto_cmd_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cmd.partition = 1;
    char level[12] = "";
    char panic[12] = "";
    int64_t zone = 0;
    bool bad = false;

    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return ALARM_ERR_BAD_ARG;
    }
    char key[16];
    while (pj_next_member(&r, key, sizeof(key))) {
        int64_t v;
        if (strcmp(key, "id") == 0) {
            if (!pj_get_str(&r, out->id, sizeof(out->id))) {
                bad = true;
            }
        } else if (strcmp(key, "action") == 0) {
            if (!pj_get_str(&r, out->action, sizeof(out->action))) {
                bad = true;
            }
        } else if (strcmp(key, "partition") == 0) {
            if (!pj_get_int(&r, &v) || v < 1 || v > ALARM_MAX_PARTITIONS) {
                bad = true;
            } else {
                out->cmd.partition = (uint8_t)v;
            }
        } else if (strcmp(key, "level") == 0) {
            if (!pj_get_str(&r, level, sizeof(level))) {
                bad = true;
            }
        } else if (strcmp(key, "zone") == 0) {
            if (!pj_get_int(&r, &zone) || zone < 1 || zone > ALARM_MAX_ZONES) {
                bad = true;
            }
        } else if (strcmp(key, "panic") == 0) {
            if (!pj_get_str(&r, panic, sizeof(panic))) {
                bad = true;
            }
        } else if (strcmp(key, "code") == 0) {
            if (!read_code(&r, out->cmd.code)) {
                bad = true;
            }
        } else if (strcmp(key, "flags") == 0) {
            if (!parse_flags(&r, &out->cmd.arm_flags)) {
                bad = true;
            }
        }
        /* anything else: skipped (newer clients may send more) */
    }
    if (!pj_finished(&r) || bad) {
        return ALARM_ERR_BAD_ARG;
    }

    alarm_cmd_t *c = &out->cmd;
    const char *a = out->action;
    if (strcmp(a, "arm") == 0) {
        c->type = ALARM_CMD_ARM;
        return parse_level(level, &c->level) ? ALARM_OK : ALARM_ERR_BAD_ARG;
    }
    if (c->arm_flags != 0u) {
        return ALARM_ERR_BAD_ARG; /* flags only go with arm */
    }
    if (strcmp(a, "disarm") == 0) {
        c->type = ALARM_CMD_DISARM;
    } else if (strcmp(a, "bypass") == 0 || strcmp(a, "unbypass") == 0) {
        c->type = (a[0] == 'b') ? ALARM_CMD_BYPASS : ALARM_CMD_UNBYPASS;
        if (zone == 0) {
            return ALARM_ERR_BAD_ARG;
        }
        c->zone = (uint16_t)zone;
    } else if (strcmp(a, "chime") == 0) {
        c->type = ALARM_CMD_CHIME;
    } else if (strcmp(a, "exit_restart") == 0) {
        c->type = ALARM_CMD_EXIT_RESTART;
    } else if (strcmp(a, "panic") == 0) {
        c->type = ALARM_CMD_PANIC;
        if (!parse_panic(panic, &c->panic)) {
            return ALARM_ERR_BAD_ARG;
        }
        if (!allow_panic) {
            return ALARM_ERR_DISABLED;
        }
    } else {
        return ALARM_ERR_BAD_ARG;
    }
    return ALARM_OK;
}

/* The result's fields into an already-open object (the Home Assistant link merges its
 * "t":"result" in front); the MQTT payload below stays byte-identical. */
void proto_cmd_result_fields(pj_writer_t *w, const proto_cmd_t *cmd, alarm_result_t result,
                             const alarm_cmd_result_t *res)
{
    pj_kstr(w, "id", (cmd != NULL && cmd->id[0] != '\0') ? cmd->id : NULL);
    pj_kstr(w, "action", (cmd != NULL && cmd->action[0] != '\0') ? cmd->action : NULL);
    pj_kstr(w, "result", alarm_result_name(result));
    if (res != NULL) {
        pj_knum(w, "user", res->user);
        pj_knum(w, "zone", res->not_ready_zone);
    }
}

void proto_cmd_result(pj_writer_t *w, const proto_cmd_t *cmd, alarm_result_t result,
                      const alarm_cmd_result_t *res)
{
    pj_obj(w);
    proto_cmd_result_fields(w, cmd, result, res);
    pj_obj_end(w);
}

alarm_result_t proto_ha_cmd_parse(const char *payload, size_t len, uint8_t partition,
                                  bool allow_trigger, alarm_cmd_t *out)
{
    memset(out, 0, sizeof(*out));
    out->partition = partition;
    char action[24] = "";

    size_t start = 0;
    while (start < len && (payload[start] == ' ' || payload[start] == '\t' ||
                           payload[start] == '\r' || payload[start] == '\n')) {
        start++;
    }
    if (start < len && payload[start] == '{') {
        pj_reader_t r;
        pj_read_init(&r, payload, len);
        if (!pj_enter_obj(&r)) {
            return ALARM_ERR_BAD_ARG;
        }
        bool bad = false;
        char key[12];
        while (pj_next_member(&r, key, sizeof(key))) {
            if (strcmp(key, "action") == 0) {
                if (!pj_get_str(&r, action, sizeof(action))) {
                    bad = true;
                }
            } else if (strcmp(key, "code") == 0) {
                if (!read_code(&r, out->code)) {
                    bad = true;
                }
            }
        }
        if (!pj_finished(&r) || bad) {
            return ALARM_ERR_BAD_ARG;
        }
    } else {
        size_t end = len;
        while (end > start && (payload[end - 1u] == ' ' || payload[end - 1u] == '\t' ||
                               payload[end - 1u] == '\r' || payload[end - 1u] == '\n')) {
            end--;
        }
        if (end - start >= sizeof(action)) {
            return ALARM_ERR_BAD_ARG;
        }
        memcpy(action, payload + start, end - start);
        action[end - start] = '\0';
    }

    if (strcmp(action, "ARM_AWAY") == 0) {
        out->type = ALARM_CMD_ARM;
        out->level = ALARM_LEVEL_AWAY;
    } else if (strcmp(action, "ARM_HOME") == 0) {
        out->type = ALARM_CMD_ARM;
        out->level = ALARM_LEVEL_STAY;
    } else if (strcmp(action, "ARM_NIGHT") == 0) {
        out->type = ALARM_CMD_ARM;
        out->level = ALARM_LEVEL_NIGHT;
    } else if (strcmp(action, "DISARM") == 0) {
        out->type = ALARM_CMD_DISARM;
    } else if (strcmp(action, "TRIGGER") == 0) {
        out->type = ALARM_CMD_PANIC;
        out->panic = ALARM_PANIC_POLICE;
        if (!allow_trigger) {
            return ALARM_ERR_DISABLED;
        }
    } else {
        return ALARM_ERR_BAD_ARG; /* ARM_VACATION, ARM_CUSTOM_BYPASS: not offered */
    }
    return ALARM_OK;
}
