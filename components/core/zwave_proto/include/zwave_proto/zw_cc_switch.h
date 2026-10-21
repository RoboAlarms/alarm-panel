/*
 * zw_cc_switch.h - Binary Switch (25) and Multilevel Switch (26): what a switch, a dimmer or a
 * relay does (docs/features/36 ZW-034, command-classes.md §4.26-§4.27). The panel builds Sets and
 * Gets and reads Reports; the simulated devices do the other side.
 *
 * Values as the spec has them: Binary Switch 00 off, FF on (01..63 read as on), FE "unknown" in a
 * Report; Multilevel Switch 00 off, 01..63 the level (1-99 %), FF "on at the last level" in a Set,
 * FE "unknown" in a Report. A duration (v2+) is the spec's encoding: 00 at once, 01..7F seconds,
 * 80..FE minutes minus one, FF the device's default.
 */
#ifndef ZW_CC_SWITCH_H
#define ZW_CC_SWITCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_cc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_CMD_SWITCH_SET    0x01u /* the same numbers in both CCs */
#define ZW_CMD_SWITCH_GET    0x02u
#define ZW_CMD_SWITCH_REPORT 0x03u

#define ZW_SWITCH_OFF      0x00u
#define ZW_SWITCH_ON       0xFFu
#define ZW_SWITCH_UNKNOWN  0xFEu
#define ZW_SWITCH_LEVEL_MAX 0x63u /* 99 % */

/* Sound Switch (79, v1-v2; a siren's tones, docs/features/36 sirens.md): Tone Play Set carries a
 * tone id (00 stop, FF the device's default tone) and, from v2, a volume (00 the configured
 * volume, 01..64 %, FF the last one used); a v1 device ignores the extra byte. Its Report answers
 * Tone Play Get with the tone playing (00 none) and, from v2, the volume. */
#define ZW_CMD_SOUND_TONE_PLAY_SET    0x08u
#define ZW_CMD_SOUND_TONE_PLAY_GET    0x09u
#define ZW_CMD_SOUND_TONE_PLAY_REPORT 0x0Au
#define ZW_SOUND_TONE_STOP            0x00u
#define ZW_SOUND_TONE_DEFAULT         0xFFu

typedef struct {
    uint8_t value;
    bool has_duration; /* v2: sent / present */
    uint8_t duration;
} zw_cc_switch_set_t;

typedef struct {
    uint8_t current;
    bool has_target;   /* Binary v2 / Multilevel v4 */
    uint8_t target;
    uint8_t duration;
} zw_cc_switch_report_t;

/* `cc` is ZW_CC_BINARY_SWITCH or ZW_CC_MULTILEVEL_SWITCH; anything else builds nothing and parses
 * as false. A Binary Switch Set takes only 00 or FF; a Multilevel Switch Set 00..63 or FF. */
size_t zw_cc_switch_set_build(uint8_t *out, size_t cap, uint8_t cc, const zw_cc_switch_set_t *in);
bool zw_cc_switch_set_parse(const uint8_t *p, size_t len, uint8_t cc, zw_cc_switch_set_t *out);
size_t zw_cc_switch_get_build(uint8_t *out, size_t cap, uint8_t cc);
bool zw_cc_switch_get_parse(const uint8_t *p, size_t len, uint8_t cc);
size_t zw_cc_switch_report_build(uint8_t *out, size_t cap, uint8_t cc,
                                 const zw_cc_switch_report_t *in);
bool zw_cc_switch_report_parse(const uint8_t *p, size_t len, uint8_t cc,
                               zw_cc_switch_report_t *out);

/* A Report's current value as the panel keeps it: -1 unknown, else 0..100 % (Binary: 0 or 100;
 * Multilevel: 99 read as 100, a stray 64..FD as unknown). */
int zw_cc_switch_percent(uint8_t cc, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_SWITCH_H */
