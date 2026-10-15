/*
 * alarm_keyseq.h - keypad key sequences.
 *
 * Codes are 4-10 digits and may differ in length per user (iPhone-style), so a
 * code can't end after a fixed number of digits the way it does on a classic
 * keypad (where "code + 2" means AWAY). The commands are therefore dedicated
 * keys, labelled like a classic keypad:
 *
 *   code + OFF       disarm / silence / clear memory / acknowledge troubles
 *   code + AWAY      arm away              code + MAX      away, no entry delay
 *   code + STAY      arm stay              code + INSTANT  stay, no entry delay
 *   code + NIGHT     arm night             code + TEST     walk test on/off
 *   code + CHIME     chime on/off          code + * + p    GOTO partition p
 *   code + BYPASS + zzz zzz ... [# or *]   bypass zones (3 digits when > 99 zones)
 *   code + BYPASS + #                      quick bypass (all faulted zones)
 *   code + CODE + uu + new code + #        add / change user uu
 *   code + CODE + uu + # 0                 delete user uu
 *   OFF              silence fire/CO/aux and trouble beeps (no code)
 *   AWAY/STAY/NIGHT alone, # 2, # 3        quick arm (if enabled)
 *   # 9              quick exit            *   show faults / exit restart in stay
 *   A / B / C        panic keys
 *
 * The classic keypad view and the serial console feed these keys. The touch UI builds the
 * same alarm_cmd_t values directly (it offers options no key sequence has, such as
 * bypass-and-arm), so every path ends in alarm_engine_command() with the same result.
 */
#ifndef ALARM_KEYSEQ_H
#define ALARM_KEYSEQ_H

#include "alarm/alarm_config.h"
#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keys ('0'-'9' are digits). */
#define ALARM_KEY_OFF     'O'
#define ALARM_KEY_AWAY    'W'
#define ALARM_KEY_STAY    'S'
#define ALARM_KEY_NIGHT   'N'
#define ALARM_KEY_MAX     'M'
#define ALARM_KEY_INSTANT 'I'
#define ALARM_KEY_TEST    'T'
#define ALARM_KEY_BYPASS  'Y'
#define ALARM_KEY_CODE    'U'
#define ALARM_KEY_CHIME   'H'
#define ALARM_KEY_READY   '*'
#define ALARM_KEY_ENTER   '#'
#define ALARM_KEY_PANIC_A 'A'
#define ALARM_KEY_PANIC_B 'B'
#define ALARM_KEY_PANIC_C 'C'

#define ALARM_KEYSEQ_KEY_TIMEOUT_MS 10000u /* abandon a partial sequence */

typedef enum {
    ALARM_KS_COMMAND = 1, /* cmd is ready for alarm_engine_command() */
    ALARM_KS_USER_SET,    /* cmd.code = actor code; user_no + new_code */
    ALARM_KS_USER_DELETE, /* cmd.code = actor code; user_no */
    ALARM_KS_GOTO,        /* switch this keypad to 'partition' */
    ALARM_KS_STAR,        /* lone '*': show faults, or exit restart when armed stay */
    ALARM_KS_ERROR,       /* invalid sequence: sound the error beep */
} alarm_keyseq_kind_t;

typedef struct {
    alarm_keyseq_kind_t kind;
    alarm_cmd_t cmd;
    uint8_t user_no;
    uint8_t partition;
    char new_code[ALARM_CODE_MAX_LEN + 1];
} alarm_keyseq_result_t;

typedef struct {
    uint8_t zone_digits;
    uint8_t partition;           /* keypad's current partition; commands target it */
    alarm_panic_t panic_keys[3]; /* what keys A, B, C do */
    uint8_t mode;                /* private */
    char code[ALARM_CODE_MAX_LEN + 1];
    char buf[ALARM_CODE_MAX_LEN + 4];
    uint8_t len;
    alarm_ms_t last_key;
} alarm_keyseq_t;

/* Reads panic_keys from the system options. */
void alarm_keyseq_init(alarm_keyseq_t *ks, const alarm_options_t *opt, uint8_t partition);

/* Feed one key. Returns true and fills *out when a sequence completes. */
bool alarm_keyseq_key(alarm_keyseq_t *ks, char key, alarm_ms_t now, alarm_keyseq_result_t *out);

/* Drop a sequence abandoned for ALARM_KEYSEQ_KEY_TIMEOUT_MS. */
void alarm_keyseq_tick(alarm_keyseq_t *ks, alarm_ms_t now);

/* When alarm_keyseq_tick needs to run next, ALARM_TIME_NEVER if idle. */
alarm_ms_t alarm_keyseq_deadline(const alarm_keyseq_t *ks);

/* Digits typed so far for the code being entered (shown as dots, never as digits). */
uint8_t alarm_keyseq_digits(const alarm_keyseq_t *ks);

void alarm_keyseq_clear(alarm_keyseq_t *ks);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_KEYSEQ_H */
