/*
 * zbz_wd.h - Zigbee sirens and strobes as the panel's bell (docs/features/20 alarm-devices.md
 * §2; ZB-023, BL-147): IAS WD (cluster 0x0502) Start Warning and Squawk, driven by the engine's
 * own bell and strobe outputs, supervised like any of the panel's devices.
 *
 * What makes it sound is the engine, never the radio: the alarm task's on_output (ALARM_OUT_BELL
 * with its pattern, ALARM_OUT_STROBE) is posted to the Zigbee task, which calls zbw_output. The
 * pattern picks the warning mode (zbw_mode_for): steady (burglary, audible panic) -> burglar,
 * temporal-3 (fire) -> fire, temporal-4 (CO) -> emergency (a siren has no CO sound of its own),
 * the bell test pulse -> a short burglar warning; off -> Stop. The bell's timeout and silencing
 * stay the engine's: a siren doesn't say when its warning ends, so the panel asks for the
 * shorter of its MaxDuration and ZBW_WARN_MAX_S and asks again ZBW_REFRESH_LEAD_MS before that
 * runs out (every retry of the refresh fits before the siren goes quiet) while the output is
 * still on - and sends Stop when the output goes off.
 *
 * Supervision (alarm-devices.md §4; decided 2026-10-02 without a review): a siren takes
 * no engine device number and nothing here writes the engine's device table - the 16 numbers are
 * the keypads' (zbz_ace.h), and the table is the installer's. A siren is supervised here: any
 * frame from it is heard (zbw_heard); silent past its limit (the installer's supervision_min, else
 * ZBW_SILENT_ROUTER_S for a mains siren, a router, or ZBW_SILENT_SLEEPY_S for a battery one)
 * while the radio works, a warning it didn't confirm after
 * ZBW_SEND_TRIES, or its own IAS Zone trouble, AC mains or battery-defect bits, raise
 * ALARM_TRBL_BELL through the port - a siren that may not sound is the bell trouble; no new
 * trouble type.
 *
 * The Warning Info byte (testing.md §4): spec layout mode bits 7-4, strobe bits 3-2, level bits
 * 1-0. A model flagged ZBZ_MODEL_WD_REVERSED gets the reversed layout (mode low nibble, strobe bit
 * 4, level bits 7-6). Golden, spec layout: burglar + strobe + high, 60 s, duty 50, strobe high is
 * 01 57 00 | 16 3C 00 32 02; Squawk armed, no strobe, very high is 01 57 01 | 03, disarmed with
 * strobe, high 01 57 01 | 1A.
 *
 * Built in ZB-M10 (src/zbz_wd.c; test/test_zbz_wd.c, the stored blob's frozen fixture
 * test/wd_fixture_v1.h). The decisions it took are alarm-devices.md §2 "Decided in ZB-M10".
 * Portable C11, caller-owned state, no heap. The owner serializes calls (the Zigbee task, zone
 * lock held).
 */
#ifndef ZBZ_WD_H
#define ZBZ_WD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "zigbee_zones/zbz_zcl.h"
#include "zone_hub/zsrc_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Commands and attributes. */
#define ZBW_CMD_START_WARNING   0x00u
#define ZBW_CMD_SQUAWK          0x01u
#define ZBW_ATTR_MAX_DURATION   0x0000u /* u16 seconds, default 240, 0..0xFFFE */
#define ZBW_MAX_DURATION_DEF    240u
#define ZBW_MAX_DURATION_BAD    0xFFFFu /* ZCL's invalid u16: read as "not read" */

/* Warning modes (bits 7-4 of Warning Info, spec layout). */
#define ZBW_MODE_STOP            0u
#define ZBW_MODE_BURGLAR         1u
#define ZBW_MODE_FIRE            2u
#define ZBW_MODE_EMERGENCY       3u
#define ZBW_MODE_POLICE_PANIC    4u
#define ZBW_MODE_FIRE_PANIC      5u
#define ZBW_MODE_EMERGENCY_PANIC 6u
/* Siren and strobe levels. */
#define ZBW_LEVEL_LOW       0u
#define ZBW_LEVEL_MEDIUM    1u
#define ZBW_LEVEL_HIGH      2u
#define ZBW_LEVEL_VERY_HIGH 3u
/* Squawk modes. */
#define ZBW_SQUAWK_ARMED    0u
#define ZBW_SQUAWK_DISARMED 1u

#define ZBW_LAYOUT_SPEC     0u
#define ZBW_LAYOUT_REVERSED 1u

#define ZBW_SIRENS_MAX   4u
#define ZBW_WARN_MAX_S   600u   /* the longest warning asked for at once */
#define ZBW_REFRESH_S    10u    /* ask again at least this long before the last one runs out */
#define ZBW_TEST_S       2u     /* "Test it" */
#define ZBW_SEND_TRIES   3u     /* an unconfirmed warning is sent again this often, then BELL */
#define ZBW_RETRY_MS     2000u
/* Added in ZB-M10: a send whose end the radio never told counts as a failed try after this long;
 * once ZBW_SEND_TRIES have failed (the bell trouble) the command is tried again this slowly while
 * it is still wanted, until the siren confirms one. */
#define ZBW_CONFIRM_MS    15000u
#define ZBW_RETRY_SLOW_MS 10000u
/* Added in ZB-M10's review: the refresh goes this long before the running warning ends (at least
 * ZBW_REFRESH_S, at most half the warning), so that every try of it - each unanswered for up to
 * ZBW_CONFIRM_MS, ZBW_RETRY_MS apart - and the bell trouble after the last one fall before the
 * siren goes quiet. */
#define ZBW_REFRESH_LEAD_MS (ZBW_SEND_TRIES * (ZBW_CONFIRM_MS + ZBW_RETRY_MS))
/* Added for ZB03: a siren whose MaxDuration reads 0 (it would sound for no time at all - a value
 * nothing can time a warning by) is asked for warnings this long, each refreshed half way: a
 * siren that does sound keeps sounding whatever its real limit is past ZBW_REFRESH_S. A Start
 * Warning never asks for 0 s while the bell is on. */
#define ZBW_MAX_DURATION_ZERO_S (2u * ZBW_REFRESH_S)
#define ZBW_STROBE_DUTY  50u    /* % of each second the strobe is lit while it runs */
#define ZBW_KEY          "wd"   /* stored in the registry's namespace */
#define ZBW_COMPAT       1u
#define ZBW_TAG          0x57000000u /* the sirens' sends: top byte 'W' */
#define ZBW_TAG_MASK     0xFF000000u
/* Silence that is the bell trouble: the zone source's supervision of the same kind of device and
 * the hub's grace after it (devices.md §7: 30 min a router, 2 h a sleepy device, then 1 h). */
#define ZBW_SILENT_ROUTER_S  (90u * 60u)
#define ZBW_SILENT_SLEEPY_S  (180u * 60u)
#define ZBW_SUPERVISION_MIN_MIN 60u /* the shortest limit the installer may set (minutes) */
/* The stored value at its longest (zbw_encode): compat, count, entries of 18 bytes with their
 * length byte - for the NVS budget (zbz_panel.h ZBZ_NVS_WORST). */
#define ZBW_ENTRY        18u
#define ZBW_BLOB_MAX     (2u + ZBW_SIRENS_MAX * ZBW_ENTRY)

typedef struct {
    uint8_t mode;          /* ZBW_MODE_* */
    bool strobe;
    uint8_t level;         /* ZBW_LEVEL_* */
    uint16_t duration_s;
    uint8_t duty;          /* 0..100 in steps of 10 */
    uint8_t strobe_level;  /* ZBW_LEVEL_* */
    uint8_t reserved;      /* added in ZB-M10: the byte's bits no field names, in place (spec
                            * layout 0x08, the strobe field's high bit; reversed 0x20), kept so
                            * every byte decodes and encodes back to itself; 0 when built */
} zbw_warning_t;

typedef struct {
    uint8_t mode;          /* ZBW_SQUAWK_* */
    bool strobe;
    uint8_t level;
    uint8_t reserved;      /* added in ZB-M10: the reserved bit in place (spec 0x04, reversed
                            * 0x20); 0 when built */
} zbw_squawk_t;

/* ---- the frames (pure; golden-tested, testing.md §4) ------------------------------------------- */

uint8_t zbw_warning_info(const zbw_warning_t *w, uint8_t layout);
/* All 256 bytes decode (a reserved mode stays as it is) and encode back to themselves. */
void zbw_warning_info_decode(uint8_t info, uint8_t layout, zbw_warning_t *out);
void zbw_start_warning(const zbw_warning_t *w, uint8_t layout, zsrc_zb_zcl_t *out);
zbz_zcl_result_t zbw_decode_start_warning(const zsrc_zb_zcl_t *f, uint8_t layout,
                                          zbw_warning_t *out);
uint8_t zbw_squawk_byte(const zbw_squawk_t *s, uint8_t layout);
void zbw_squawk_decode(uint8_t byte, uint8_t layout, zbw_squawk_t *out);
void zbw_squawk(const zbw_squawk_t *s, uint8_t layout, zsrc_zb_zcl_t *out);

/* The warning mode for an engine bell pattern on a model (ZBZ_MODEL_WD_ONE_TONE: emergency for
 * every sound); STOP for ALARM_PAT_OFF. */
uint8_t zbw_mode_for(alarm_pattern_t pattern, uint16_t model_flags);

/* ---- the policy ---------------------------------------------------------------------------------- */

/* An assignment (the installer's, page 14), as stored. */
typedef struct {
    bool used;
    uint16_t id;           /* the registry's id */
    uint64_t ieee;
    uint8_t endpoint;      /* its IAS WD endpoint */
    uint8_t partitions;    /* bit 0 = partition 1 */
    bool burglary;         /* burglary and audible panic */
    bool fire;             /* fire and CO */
    bool strobe;
    bool squawk;
    uint8_t volume;        /* ZBW_LEVEL_* */
    uint8_t device;        /* always 0: a siren takes no device number (alarm-devices.md §4);
                            * written 0, ignored when read */
    uint16_t model_flags;  /* ZBZ_MODEL_* (not stored: from the model table) */
    uint16_t max_duration_s; /* its model's cap (zbz_model_t; not stored), 0 none. Changed for
                              * ZB03: it caps the MaxDuration read (the shorter wins) and stands
                              * alone only when nothing was read */
    bool sleepy;           /* not stored: a sleepy end device (the registry's), for its silence
                            * limit (ZBW_SILENT_SLEEPY_S) */
    uint16_t supervision_min; /* the installer's silence limit in minutes, ZBW_SUPERVISION_MIN_MIN
                               * and up; 0 the default (ZBW_SILENT_*). Never off: a siren that
                               * may not sound must be known */
    /* Added for ZB03 (not stored here: the registry's record keeps it, zbz_registry.h
     * ZBZ_F_WD_MAX_DURATION): the MaxDuration the siren answered at its setup. max_read false:
     * not read (ZBW_MAX_DURATION_DEF, or the model's cap); true with 0 is a valid 0
     * (ZBW_MAX_DURATION_ZERO_S). */
    bool max_read;
    uint16_t max_read_s;   /* 0..0xFFFE */
} zbw_assign_t;

typedef struct {
    zbw_assign_t a;
    /* live */
    uint8_t mode;          /* the warning it was last asked for, STOP none */
    bool strobe_only;      /* ALARM_OUT_STROBE alone */
    uint64_t until;        /* when that warning runs out in the siren */
    uint64_t retry_at;
    uint32_t tag;
    uint8_t tries;
    bool trouble;          /* what the port was last told for it */
    uint64_t heard_at;     /* its last frame, or the radio's start or its assignment if later */
    bool silent;           /* not heard past its limit (part of `trouble`) */
    /* added in ZB-M10 */
    uint8_t info;          /* the Warning Info byte of the warning it was last asked for */
    bool pending;          /* that warning (or Stop) isn't confirmed yet: sent again at retry_at */
    bool unconfirmed;      /* ZBW_SEND_TRIES of it failed (part of `trouble`) */
    bool status_bad;       /* its IAS Zone trouble, AC mains or battery-defect bit (part of
                            * `trouble`) */
    bool fresh;            /* just assigned or read back: its silence counts from the next call
                            * that carries the time */
} zbw_siren_t;

typedef struct {
    void *ctx;
    /* A command to the siren (the radio's zcl_command); false: not queued. */
    bool (*send)(void *ctx, uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *cmd,
                 uint32_t tag);
    /* ALARM_TRBL_BELL on or off, for the owner to post to the alarm task (any siren in trouble). */
    void (*bell_trouble)(void *ctx, bool active);
    /* Not called (decided 2026-10-02): a siren takes no device number and checks in nowhere but
     * here (its silence is the bell trouble). Kept so a port filled for the first contract still
     * compiles; leave it NULL. */
    void (*checkin)(void *ctx, uint8_t device);
} zbw_port_t;

typedef struct {
    zbw_port_t port;
    zbw_siren_t siren[ZBW_SIRENS_MAX];
    uint8_t bell[ALARM_MAX_PARTITIONS];  /* alarm_pattern_t of each partition's bell */
    bool strobe[ALARM_MAX_PARTITIONS];   /* each partition's strobe output */
    uint16_t seq;
    bool trouble;                        /* the bell trouble as last told */
    bool dirty;                          /* the assignments are to be stored */
    bool radio_up;                       /* silence counts only while the radio works */
    bool armed[ALARM_MAX_PARTITIONS];    /* added in ZB-M10: each partition armed (zbw_armed,
                                          * zbw_set_armed), for zbw_test's refusal */
} zbw_t;

typedef enum {
    ZBW_OK = 0,
    ZBW_ERR_FULL,
    ZBW_ERR_ARG,           /* no partition, a volume out of range, a supervision time under
                            * ZBW_SUPERVISION_MIN_MIN (0 is the default, not "off") */
    ZBW_ERR_IN_USE,        /* not returned since sirens take no device number (kept: values) */
    ZBW_ERR_UNKNOWN,       /* no such siren */
    ZBW_ERR_SEND,
    ZBW_ERR_ARMED,         /* added in ZB-M10: zbw_test with one of its partitions armed */
    ZBW_ERR_BUSY,          /* added in ZB-M10: zbw_test while it is giving a warning */
} zbw_result_t;

void zbw_init(zbw_t *w, const zbw_port_t *port);
zbw_result_t zbw_assign(zbw_t *w, const zbw_assign_t *a);
zbw_result_t zbw_clear(zbw_t *w, uint16_t id);
const zbw_siren_t *zbw_find(const zbw_t *w, uint16_t id);

/* The engine's output changed (posted from the alarm task's on_output). */
void zbw_output(zbw_t *w, uint64_t now, uint8_t partition, alarm_output_t out,
                alarm_pattern_t pattern);
/* A partition armed or disarmed: a squawk from each siren of it that wants one (one that is
 * giving a warning, or whose last command isn't confirmed, doesn't squawk). */
void zbw_armed(zbw_t *w, uint64_t now, uint8_t partition, bool armed);
/* Added in ZB-M10: the partition's armed state without a squawk (the owner's start-up, from the
 * engine's snapshot). */
void zbw_set_armed(zbw_t *w, uint8_t partition, bool armed);
/* Added in ZB-M10, the policy as a pure step (zbz_panel_output's wiring calls zbw_output, which
 * applies it): the warning `s` should be giving now for the partitions' bell and strobe outputs
 * in `w` - the most urgent bell of its partitions it sounds for (fire, then CO, then burglary),
 * its strobe with it when chosen, or strobe-only (mode STOP, strobe) while a partition's strobe
 * output is on - with the duration the siren is asked for (its MaxDuration, at most
 * ZBW_WARN_MAX_S). false: nothing (it should be quiet). */
bool zbw_wanted(const zbw_t *w, const zbw_siren_t *s, zbw_warning_t *out);
/* Added for ZB03: the warning length `a` is asked for, in seconds - its MaxDuration as read (a
 * read 0: ZBW_MAX_DURATION_ZERO_S), at most its model's cap; not read: the model's cap, else
 * ZBW_MAX_DURATION_DEF; never more than ZBW_WARN_MAX_S. The refresh goes before it runs out
 * (ZBW_REFRESH_LEAD_MS, at most half of it). */
uint16_t zbw_warn_seconds(const zbw_assign_t *a);
/* Added for ZB03: the MaxDuration in an IAS WD Read Attributes Response (`f` as zbz_zcl_decode_global
 * gives it, or as the radio's ZSRC_ZB_REP_ZCL report carries it: cluster 0x0502, command 0x01,
 * server to client, its records byte for byte). True only for attribute 0x0000 answered SUCCESS
 * with type uint16 and a value 0..0xFFFE; false for anything else - another command or cluster,
 * a failed read, another type, a record cut short, 0xFFFF (ZCL's invalid value). */
bool zbw_max_duration_of(const zsrc_zb_zcl_t *f, uint16_t *out);
/* "Test it": ZBW_TEST_S of its burglar sound (refused while any of its partitions is armed). */
zbw_result_t zbw_test(zbw_t *w, uint64_t now, uint16_t id);
/* The radio's news: a send ended; a frame from the siren; its IAS Zone status. */
bool zbw_send_done(zbw_t *w, uint64_t now, uint32_t tag, zsrc_zb_done_t result);
void zbw_heard(zbw_t *w, uint64_t now, uint64_t ieee);
void zbw_zone_status(zbw_t *w, uint64_t now, uint64_t ieee, uint16_t status);
/* The radio started working (true) or stopped (false): while it is down no siren is silent (the
 * radio's own trouble, sensor_radio, says it); coming up, every siren's silence counts from now. */
void zbw_radio(zbw_t *w, uint64_t now, bool up);
void zbw_poll(zbw_t *w, uint64_t now);
/* When to poll again: 0 at once (a siren assigned or changed since), UINT64_MAX not before news. */
uint64_t zbw_next_deadline(const zbw_t *w);
/* How long a siren may stay unheard before it is silent (the bell trouble), in ms: the installer's
 * supervision_min, else ZBW_SILENT_ROUTER_S or ZBW_SILENT_SLEEPY_S (additive, the review of wave
 * 6: zbz_panel reads a quiet mains siren well inside it, ZB-023). */
uint64_t zbw_silent_limit_ms(const zbw_assign_t *a);

/* The store: compat (1) | count | count x (entry length u8 | id u16 | ieee u64 | endpoint |
 * partitions | flags (bit 0 burglary, 1 fire, 2 strobe, 3 squawk) | volume | device (written 0,
 * ignored when read) | supervision u16 minutes (0 the default)). A shorter entry reads its
 * missing fields as 0. zbw_decode is all or nothing: a damaged blob returns false and leaves the
 * sirens as they were; a good one replaces them without a Stop (a start-up call: at run time the
 * owner clears sounding sirens first). */
size_t zbw_encode(const zbw_t *w, uint8_t *out, size_t cap);
bool zbw_decode(zbw_t *w, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZBZ_WD_H */
