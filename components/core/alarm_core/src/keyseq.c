/*
 * keyseq.c - key sequences -> structured commands.
 * Codes are 4-10 digits; a command key ends the code (see alarm_keyseq.h).
 */
#include <string.h>

#include "alarm/alarm_keyseq.h"

enum {
    KS_IDLE = 0,
    KS_HASH,      /* '#' pressed: quick function digit follows */
    KS_CODE,      /* collecting code digits until a command key */
    KS_BYPASS,    /* code + BYPASS: collecting zone numbers */
    KS_USER_NO,   /* code + CODE: collecting a 2-digit user number */
    KS_USER_CODE, /* code + CODE + uu: collecting the new code until '#' */
    KS_USER_DEL,  /* code + CODE + uu + '#': expecting '0' */
    KS_GOTO,      /* code + '*': expecting a partition digit */
};

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_command_key(char c)
{
    switch (c) {
    case ALARM_KEY_OFF:
    case ALARM_KEY_AWAY:
    case ALARM_KEY_STAY:
    case ALARM_KEY_NIGHT:
    case ALARM_KEY_MAX:
    case ALARM_KEY_INSTANT:
    case ALARM_KEY_TEST:
    case ALARM_KEY_BYPASS:
    case ALARM_KEY_CODE:
    case ALARM_KEY_CHIME:
        return true;
    default:
        return false;
    }
}

void alarm_keyseq_clear(alarm_keyseq_t *ks)
{
    ks->mode = KS_IDLE;
    ks->len = 0u;
    memset(ks->code, 0, sizeof(ks->code));
    memset(ks->buf, 0, sizeof(ks->buf));
}

void alarm_keyseq_init(alarm_keyseq_t *ks, const alarm_options_t *opt, uint8_t partition)
{
    memset(ks, 0, sizeof(*ks));
    ks->zone_digits = (ALARM_MAX_ZONES > 99) ? 3u : 2u;
    ks->partition = partition;
    memcpy(ks->panic_keys, opt->panic_keys, sizeof(ks->panic_keys));
    alarm_keyseq_clear(ks);
}

static void command(const alarm_keyseq_t *ks, alarm_keyseq_result_t *r, alarm_cmd_type_t type,
                    bool with_code)
{
    memset(r, 0, sizeof(*r));
    r->kind = ALARM_KS_COMMAND;
    r->cmd.type = type;
    r->cmd.partition = ks->partition;
    if (with_code) {
        memcpy(r->cmd.code, ks->code, sizeof(r->cmd.code));
    }
}

static void arm(const alarm_keyseq_t *ks, alarm_keyseq_result_t *r, alarm_arm_level_t level,
                uint8_t flags, bool with_code)
{
    command(ks, r, ALARM_CMD_ARM, with_code);
    r->cmd.level = level;
    r->cmd.arm_flags = flags;
}

static bool simple(alarm_keyseq_result_t *r, alarm_keyseq_kind_t kind)
{
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    return true;
}

static bool error(alarm_keyseq_t *ks, alarm_keyseq_result_t *r)
{
    alarm_keyseq_clear(ks);
    return simple(r, ALARM_KS_ERROR);
}

static unsigned parse_num(const char *s, unsigned n)
{
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++) {
        v = v * 10u + (unsigned)(s[i] - '0');
    }
    return v;
}

/* A command key, after a code (with_code) or on its own (quick functions). */
static bool command_key(alarm_keyseq_t *ks, char key, alarm_keyseq_result_t *r, bool with_code)
{
    switch (key) {
    case ALARM_KEY_OFF:
        /* OFF alone silences fire/CO/aux and trouble beeps. */
        command(ks, r, with_code ? ALARM_CMD_DISARM : ALARM_CMD_SILENCE, with_code);
        break;
    case ALARM_KEY_AWAY:
        arm(ks, r, ALARM_LEVEL_AWAY, 0u, with_code);
        break;
    case ALARM_KEY_STAY:
        arm(ks, r, ALARM_LEVEL_STAY, 0u, with_code);
        break;
    case ALARM_KEY_NIGHT:
        arm(ks, r, ALARM_LEVEL_NIGHT, 0u, with_code);
        break;
    case ALARM_KEY_MAX:
        arm(ks, r, ALARM_LEVEL_AWAY, ALARM_ARM_NO_ENTRY_DELAY, with_code);
        break;
    case ALARM_KEY_INSTANT:
        arm(ks, r, ALARM_LEVEL_STAY, ALARM_ARM_NO_ENTRY_DELAY, with_code);
        break;
    case ALARM_KEY_TEST:
        if (!with_code) {
            return error(ks, r);
        }
        command(ks, r, ALARM_CMD_WALK_TEST, true);
        break;
    case ALARM_KEY_CHIME:
        /* Like the arming keys: the key alone passes a command with no code, and the engine
         * accepts it only while chime_needs_code is off (CORE-CHIME-005). */
        command(ks, r, ALARM_CMD_CHIME, with_code);
        break;
    case ALARM_KEY_BYPASS:
        if (!with_code) {
            return error(ks, r);
        }
        ks->mode = KS_BYPASS;
        ks->len = 0u;
        return false;
    case ALARM_KEY_CODE:
        if (!with_code) {
            return error(ks, r);
        }
        ks->mode = KS_USER_NO;
        ks->len = 0u;
        return false;
    default:
        return error(ks, r);
    }
    alarm_keyseq_clear(ks);
    return true;
}

void alarm_keyseq_tick(alarm_keyseq_t *ks, alarm_ms_t now)
{
    if (ks->mode != KS_IDLE && now >= ks->last_key + ALARM_KEYSEQ_KEY_TIMEOUT_MS) {
        alarm_keyseq_clear(ks); /* abandoned sequence: nothing was submitted */
    }
}

alarm_ms_t alarm_keyseq_deadline(const alarm_keyseq_t *ks)
{
    return (ks->mode != KS_IDLE) ? ks->last_key + ALARM_KEYSEQ_KEY_TIMEOUT_MS : ALARM_TIME_NEVER;
}

uint8_t alarm_keyseq_digits(const alarm_keyseq_t *ks)
{
    if (ks->mode == KS_CODE) {
        return ks->len;
    }
    if (ks->mode == KS_USER_CODE) {
        return (uint8_t)(ks->len - 2u);
    }
    return 0u;
}

bool alarm_keyseq_key(alarm_keyseq_t *ks, char key, alarm_ms_t now, alarm_keyseq_result_t *out)
{
    alarm_keyseq_tick(ks, now);
    ks->last_key = now;

    /* Panic keys work from any state and discard a half-typed code. */
    if (key == ALARM_KEY_PANIC_A || key == ALARM_KEY_PANIC_B || key == ALARM_KEY_PANIC_C) {
        alarm_keyseq_clear(ks);
        command(ks, out, ALARM_CMD_PANIC, false);
        out->cmd.panic = ks->panic_keys[key - ALARM_KEY_PANIC_A];
        return true;
    }

    switch (ks->mode) {
    case KS_IDLE:
        if (is_digit(key)) {
            ks->mode = KS_CODE;
            ks->code[0] = key;
            ks->len = 1u;
            return false;
        }
        if (key == ALARM_KEY_ENTER) {
            ks->mode = KS_HASH;
            return false;
        }
        if (key == ALARM_KEY_READY) {
            return simple(out, ALARM_KS_STAR);
        }
        return command_key(ks, key, out, false);

    case KS_HASH: /* quick functions after # */
        alarm_keyseq_clear(ks);
        if (key == '2') {
            arm(ks, out, ALARM_LEVEL_AWAY, 0u, false);
        } else if (key == '3') {
            arm(ks, out, ALARM_LEVEL_STAY, 0u, false);
        } else if (key == '9') {
            command(ks, out, ALARM_CMD_EXIT_RESTART, false);
        } else {
            return simple(out, ALARM_KS_ERROR);
        }
        return true;

    case KS_CODE:
        if (is_digit(key)) {
            if (ks->len >= ALARM_CODE_MAX_LEN) {
                return error(ks, out);
            }
            ks->code[ks->len++] = key;
            return false;
        }
        if (ks->len < ALARM_CODE_MIN_LEN) {
            return error(ks, out);
        }
        if (key == ALARM_KEY_READY) {
            ks->mode = KS_GOTO;
            return false;
        }
        if (is_command_key(key)) {
            return command_key(ks, key, out, true);
        }
        return error(ks, out);

    case KS_BYPASS:
        if (key == ALARM_KEY_ENTER && ks->len == 0u && ks->buf[0] == '\0') {
            command(ks, out, ALARM_CMD_QUICK_BYPASS, true);
            alarm_keyseq_clear(ks);
            return true;
        }
        if ((key == ALARM_KEY_ENTER || key == ALARM_KEY_READY) && ks->len == 0u) {
            alarm_keyseq_clear(ks); /* done entering zones */
            return false;
        }
        if (!is_digit(key)) {
            return error(ks, out);
        }
        ks->buf[ks->len++] = key;
        if (ks->len == ks->zone_digits) {
            command(ks, out, ALARM_CMD_BYPASS, true);
            out->cmd.zone = (uint16_t)parse_num(ks->buf, ks->zone_digits);
            ks->len = 0u; /* stay in bypass mode for more zones; buf[0] marks "zones entered" */
            return true;
        }
        return false;

    case KS_USER_NO:
        if (!is_digit(key)) {
            return error(ks, out);
        }
        ks->buf[ks->len++] = key;
        if (ks->len == 2u) {
            ks->mode = KS_USER_CODE; /* buf[0..1] keeps the user number */
        }
        return false;

    case KS_USER_CODE: {
        const unsigned typed = ks->len - 2u;
        if (key == ALARM_KEY_ENTER) {
            if (typed == 0u) {
                ks->mode = KS_USER_DEL;
                return false;
            }
            if (typed < ALARM_CODE_MIN_LEN) {
                return error(ks, out);
            }
            memset(out, 0, sizeof(*out));
            out->kind = ALARM_KS_USER_SET;
            out->cmd.partition = ks->partition;
            memcpy(out->cmd.code, ks->code, sizeof(out->cmd.code));
            out->user_no = (uint8_t)parse_num(ks->buf, 2u);
            memcpy(out->new_code, ks->buf + 2, typed);
            alarm_keyseq_clear(ks);
            return true;
        }
        if (!is_digit(key) || typed >= ALARM_CODE_MAX_LEN) {
            return error(ks, out);
        }
        ks->buf[ks->len++] = key;
        return false;
    }

    case KS_USER_DEL:
        if (key != '0') {
            return error(ks, out);
        }
        memset(out, 0, sizeof(*out));
        out->kind = ALARM_KS_USER_DELETE;
        out->cmd.partition = ks->partition;
        memcpy(out->cmd.code, ks->code, sizeof(out->cmd.code));
        out->user_no = (uint8_t)parse_num(ks->buf, 2u);
        alarm_keyseq_clear(ks);
        return true;

    case KS_GOTO:
        if (key < '1' || key > '9') {
            return error(ks, out);
        }
        simple(out, ALARM_KS_GOTO);
        out->partition = (uint8_t)(key - '0');
        alarm_keyseq_clear(ks);
        return true;

    default:
        return error(ks, out);
    }
}
