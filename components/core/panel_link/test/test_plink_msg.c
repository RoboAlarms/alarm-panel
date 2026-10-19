/*
 * test_plink_msg.c - the panel link's byte writer and reader and every message codec
 * (plink_wire.c, plink_msg.c; docs/features/11 "Messages", "STATUS", "Zone record", "Event
 * record", "Commands"): golden bytes for every message, written by hand from the table (and
 * built again independently by vectors_plink_msg.py with Python's struct module; `--check`
 * compares the two); round trips; sections and records a newer firmware lengthened (the extra
 * bytes skipped) or wrote shorter (the missing fields read 0); truncated payloads refused; names
 * cleaned before anything shows them; codes that are digits only and padded to one size; STATUS
 * split over small frames, ZONES and EVENTS paged; the command digest without the codes; and
 * 200000 random and mutated payloads into every decoder (the host CI runs this under ASan and
 * UBSan: a decoder that reads past a payload fails there).
 * The update, Smart Home and settings messages (docs/features/11 "Updates through the link",
 * "Smart Home and Home Assistant on keypads"): their goldens and the record helpers', every one at
 * its longest within a 250-byte frame, each section grown and cut, HEARTBEAT's section read only
 * when bytes remain (and ignored by v1's decoder), DEV_ACT one size, its digest without the code,
 * a PREFS fields body carried whole from proto_prefs_encode to proto_prefs_decode and the same
 * prefs_crc on both sides; and the frozen v1 fixture (plink_wire_fixture_v1.h) read as the values
 * frozen with it, written again byte for byte, and its numbers compared with plink_msg.h's.
 * Requirements: LINK-004, LINK-005, LINK-017, LINK-018, LINK-043, LINK-047, LINK-050, LINK-051
 */
#include <stddef.h>
#include <string.h>

#include "alarm_proto/proto_prefs.h"
#include "home_control/device_source.h"
#include "panel_link/plink_frame.h"
#include "panel_link/plink_msg.h"
#include "panel_link/plink_wire.h"
#include "plink_wire_fixture_v1.h"
#include "ui_backend/ui_backend.h"
#include "unity.h"
#include "plink_wifi_fixture_v1.h"
#include "status_goto_fixture_v1.h"
#include "status_policy_fixture_v1.h"

/* Numbers plink_msg.h copies from the components that own them (docs/features/11 "Frozen
 * values"): a DEV_ACK result 0..5 is ui_dev_result_t's, a DEV_ACT mode device_source.h's bit. */
_Static_assert((int)PLINK_DEV_OK == (int)UI_DEV_OK &&
                   (int)PLINK_DEV_NEED_CODE == (int)UI_DEV_NEED_CODE &&
                   (int)PLINK_DEV_CODE_REFUSED == (int)UI_DEV_CODE_REFUSED &&
                   (int)PLINK_DEV_BUSY == (int)UI_DEV_BUSY &&
                   (int)PLINK_DEV_UNREACHABLE == (int)UI_DEV_UNREACHABLE &&
                   (int)PLINK_DEV_REFUSED == (int)UI_DEV_REFUSED,
               "DEV_ACK's results are ui_dev_result_t's");
_Static_assert(PLINK_DEV_MODE_OFF == DEV_MODE_OFF && PLINK_DEV_MODE_HEAT == DEV_MODE_HEAT &&
                   PLINK_DEV_MODE_COOL == DEV_MODE_COOL && PLINK_DEV_MODE_AUTO == DEV_MODE_AUTO,
               "DEV_ACT's modes are device_source.h's bits");

void setUp(void) {}

void tearDown(void) {}

/* ---- golden payloads ----------------------------------------------------------------------
 * Written by hand from docs/features/11 "Messages"; vectors_plink_msg.py builds the same bytes
 * independently (python vectors_plink_msg.py --check). */

static const uint8_t G_PAIR_REQ[] = {
    0x44, 0x33, 0x22, 0x11,             /* attempt 0x11223344 */
    0x01,                               /* proto 1 */
    0x5A, 0xA5,                         /* caps 0xA55A */
    0x01,                               /* kind: a panel */
    0x05, 0x04, 0x80, 0x00, 0x40, 0x10, /* capacities: 4 partitions, 128 zones, 64 users, 16 dev */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
    0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
    0x1E, 0x1F,                         /* pk_k */
    0x04, 0x48, 0x61, 0x6C, 0x6C,       /* name "Hall" */
    0x05, 0x31, 0x2E, 0x32, 0x2E, 0x33, /* fw "1.2.3" */
};

static const uint8_t G_PAIR_COMMIT[] = {
    0x04, 0x03, 0x02, 0x01, /* attempt 0x01020304 */
    0x01,                   /* proto */
    0x03, 0x00,             /* caps 0x0003 */
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E,
    0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D,
    0x3E, 0x3F, /* pk_m */
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E,
    0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D,
    0x5E, 0x5F, /* commit */
    0x0C, 0x48, 0x61, 0x72, 0x62, 0x6F, 0x72, 0x20, 0x48, 0x6F, 0x75, 0x73, 0x65, /* name */
    0x06, 0x61, 0x31, 0x62, 0x32, 0x63, 0x33, /* panel id "a1b2c3" */
};

static const uint8_t G_PAIR_NONCE[] = {
    0xEF, 0xBE, 0xAD, 0xDE, /* attempt 0xDEADBEEF */
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E,
    0x6F, /* n */
};

static const uint8_t G_PAIR_CONFIRM[] = {
    0x07, 0x00, 0x00, 0x00, /* attempt 7 */
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE,
    0xAF, /* mac_k */
};

static const uint8_t G_PAIR_DONE[] = {
    0x07, 0x00, 0x00, 0x00, /* attempt 7 */
    0x03,                   /* device 3 */
    0x02,                   /* home 2 */
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
    0xBF, /* mac_m */
};

static const uint8_t G_PAIR_FAIL[] = {
    0xCD, 0xAB, 0x00, 0x00, /* attempt 0xABCD */
    0x07,                   /* reason: overlap */
};

static const uint8_t G_HELLO[] = {
    0x01,                               /* proto */
    0x02, 0x01,                         /* caps 0x0102 */
    0x05, 0x02, 0x40, 0x01, 0x20, 0x08, /* capacities: 2, 320 zones, 32 users, 8 devices */
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
    0x1F, /* n_k */
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D,
    0x4E, 0x4F, /* e_k */
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE,
    0xFF, /* mac1 */
};

static const uint8_t G_WELCOME[] = {
    0x01,       /* proto */
    0x00, 0x00, /* caps */
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
    0x1F, /* n_m */
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D,
    0x4E, 0x4F, /* e_m */
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE,
    0xFF, /* mac2 */
};

static const uint8_t G_REKEY[] = {0xEF, 0xCD, 0xAB, 0x89}; /* sid 0x89ABCDEF */

static const uint8_t G_STATUS[] = {
    /* system section: 59 bytes */
    0x3B,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, /* epoch_ms 0x0102030405060708 */
    0x05,                   /* partitions 1 and 3 */
    0x01,                   /* home 1 */
    0x09, 0x00, 0x00, 0x00, /* sys_troubles */
    0x02, 0x00, 0x00, 0x00, /* sys_restored */
    0x04, 0x00, 0x00, 0x00, /* dev_troubles */
    0x03,                   /* default_codes */
    0x03,                   /* flags: program, exit sounds */
    0xC0, 0xD4, 0x01, 0x00, /* recovery_left_ms 120000 */
    0x30, 0x75, 0x00, 0x00, /* lockout_left_ms 30000 */
    0x02,                   /* report: online */
    0x10, 0x00,             /* zones_used 16 */
    0xD2, 0x04, 0x00, 0x00, /* last_event_seq 1234 */
    0xBE, 0xBA, 0xFE, 0xCA, /* cfg_crc */
    0x34, 0x12, 0x00, 0x00, /* cfg_len */
    0x78, 0x56, 0x34, 0x12, /* zones_crc */
    0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00, 0x00, /* mono_ms 0x112233445566 */
    /* partition 1: 52 bytes */
    0x01, 0x34,
    0x03,                   /* state: entry delay */
    0x01,                   /* level: away */
    0x02,                   /* arm_flags */
    0x05,                   /* armed_by */
    0x01, 0x03, 0x00,       /* arm source: keypad 3 */
    0x0E,                   /* ready_levels */
    0x2D,                   /* ready, walk test, triggered, trouble_unack */
    0x04,                   /* sounder: entry */
    0x01,                   /* bell */
    0x18, 0x79, 0x00, 0x00, /* delay_left_ms 31000 */
    0x02, 0x80,             /* memory */
    0x02, 0x00,             /* faults */
    0x01, 0x00,             /* bypassed */
    0x03, 0x00,             /* alarms */
    0x04, 0x00,             /* troubles */
    0x60, 0xEA, 0x00, 0x00, /* walk_left_ms 60000 */
    0xE8, 0x03, 0x00, 0x00, /* walk_age_ms 1000 */
    0xEF, 0xCD, 0xAB, 0x00, /* walk_id */
    0x11, 0x00, 0x00, 0x00, /* walk_seq */
    0x05, 0x00,             /* walk_tested */
    0x09, 0x00,             /* walk_total */
    0x01, 0x02, 0x00, 0x00, /* walk_flags, started_by, ended_by, end_how */
    0x01, 0x04, 0x00,       /* walk source: keypad 4 */
    /* partition 3: 52 bytes, ready */
    0x03, 0x34,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x01, 0x00, 0x00, /* .. ready_levels, flags */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t G_ZONES[] = {
    0x0D, 0xF0, 0xAD, 0x0B, /* zones_crc 0x0BADF00D */
    0x05, 0x00,             /* first: zone 5 */
    0x02, 0x00,             /* count 2 */
    0x07,                   /* rec_len */
    0x01, 0x02, 0x01, 0x05, 0x2C, 0x01, 0x02, /* zone 5: flags 0x0201, trouble, walk, 300 s, 2 */
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* zone 6: bypassed */
};

static const uint8_t G_ZONES_REQ[] = {0x01, 0x00, 0x80, 0x00}; /* zones 1..128 */

static const uint8_t G_CFG_REQ[] = {
    0xBE, 0xBA, 0xFE, 0xCA, /* cfg_crc */
    0x00, 0x04, 0x00, 0x00, /* offset 1024 */
    0x00, 0x04,             /* max 1024 */
};

static const uint8_t G_CFG_PAGE[] = {
    0xBE, 0xBA, 0xFE, 0xCA, /* cfg_crc */
    0x10, 0x00, 0x00, 0x00, /* total 16 */
    0x0C, 0x00, 0x00, 0x00, /* offset 12 */
    0x04, 0x00,             /* len 4 */
    0xDE, 0xAD, 0xBE, 0xEF, /* the blob's last 4 bytes */
};

static const uint8_t G_EV_REQ[] = {0xD2, 0x04, 0x00, 0x00, 0x32}; /* after 1234, at most 50 */

static const uint8_t G_EVENTS[] = {
    0x00, 0x02, 0x20, /* flags, count 2, rec_len 32 */
    /* armed away by user 5 at keypad 3 */
    0xD1, 0x04, 0x00, 0x00,                         /* seq 1233 */
    0x40, 0xE2, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, /* mono_ms 123456 */
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, /* epoch_ms */
    0x01, 0x00,                                     /* type: armed */
    0x01,                                           /* partition */
    0x00, 0x00,                                     /* zone */
    0x05,                                           /* user */
    0x01, 0x03, 0x00,                               /* source: keypad 3 */
    0x01, 0x02, 0x01,                               /* arg, arg2, flags */
    /* entry delay from zone 7 */
    0xD2, 0x04, 0x00, 0x00,                         /* seq 1234 */
    0xA4, 0xE2, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, /* mono_ms 123556 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, /* epoch_ms -1 */
    0x06, 0x00,                                     /* type: entry delay */
    0x01,                                           /* partition */
    0x07, 0x00,                                     /* zone 7 */
    0x00,                                           /* user */
    0x03, 0x07, 0x00,                               /* source: zone 7 */
    0x00, 0x00, 0x00,                               /* arg, arg2, flags */
};

static const uint8_t G_CMD[] = {
    0xEE, 0xFF, 0xC0, 0x00, /* req 0x00C0FFEE */
    0x01,                   /* op: a command */
    0x09, 0x00, 0x02, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, /* arm partition 2 stay instant */
    0x04, 0x31, 0x32, 0x33, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* code "1234", padded */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* no new code */
};

static const uint8_t G_CMD_CHANGE[] = {
    0x2A, 0x00, 0x00, 0x00, /* req 42 */
    0x03,                   /* op: change the code */
    0x09, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* partition 1 */
    0x06, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x00, 0x00, 0x00, /* "123456" */
    0x0A, 0x39, 0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31, 0x30, /* "9876543210" */
};

static const uint8_t G_RESULT[] = {
    0xEE, 0xFF, 0xC0, 0x00, /* req */
    0x06,                   /* not ready */
    0x05,                   /* user 5 */
    0x02, 0x01,             /* not_ready_zone 258 */
    0x01,                   /* may_disarm */
};

static const uint8_t G_SOUND[] = {0x02, 0x0C, 0x0A, 0x00}; /* partition 2, chime, zone 10 */

static const uint8_t G_HEARTBEAT[] = {
    0x05,                         /* mains lost, battery there */
    0xC3,                         /* rssi -61 */
    0x80, 0x51, 0x01, 0x00,       /* uptime 86400 s */
    0xFF, 0xFF,                   /* no status yet */
    0x05, 0x30, 0x2E, 0x39, 0x2E, 0x31, /* fw "0.9.1" */
};

static const uint8_t G_TIME[] = {
    0x00, 0xC0, 0x2C, 0xC8, 0x99, 0x01, 0x00, 0x00, /* epoch_ms 1760000000000 */
    0x16, 0x45, 0x53, 0x54, 0x35, 0x45, 0x44, 0x54, 0x2C, 0x4D, 0x33, 0x2E, 0x32, 0x2E, 0x30,
    0x2C, 0x4D, 0x31, 0x31, 0x2E, 0x31, 0x2E, 0x30, /* "EST5EDT,M3.2.0,M11.1.0" */
};

static const uint8_t G_BYE[] = {0x01}; /* removed at the master */

static const uint8_t G_PAIR_REVEAL[] = {
    0xEF, 0xBE, 0xAD, 0xDE, /* attempt 0xDEADBEEF */
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E,
    0x7F, /* n_m */
};

/* G_HEARTBEAT in a session with PLINK_CAP_PREFS: its appended section. */
static const uint8_t G_HEARTBEAT_PREFS[] = {
    0x05, 0xC3, 0x80, 0x51, 0x01, 0x00, 0xFF, 0xFF, 0x05, 0x30, 0x2E, 0x39, 0x2E, 0x31, /* v1's */
    0x04, 0xE0, 0xFE, 0x0F, 0x0C, /* section: prefs_crc 0x0C0FFEE0 */
};

/* ---- updates through the link (docs/features/11 "Updates through the link", "Messages") ---- */

static const uint8_t G_FW_INFO[] = {
    0x43,                         /* one section: 67 bytes */
    0x07,                         /* can: fetch, relay, key accept */
    0x0B, 0x63, 0x72, 0x6F, 0x77, 0x70, 0x61, 0x6E, 0x65, 0x6C, 0x2D, 0x37, /* "crowpanel-7" */
    0x03, 0x31, 0x2E, 0x33,       /* board_rev "1.3" */
    0x03, 0x31, 0x2E, 0x30,       /* chip_rev "1.0" */
    0x64, 0x00,                   /* chip_rev_full 100 */
    0x0A, 0x72, 0x6F, 0x62, 0x6F, 0x61, 0x6C, 0x61, 0x72, 0x6D, 0x73, /* "roboalarms" */
    0x05, 0x31, 0x2E, 0x34, 0x2E, 0x30, /* version "1.4.0" */
    0x00, 0x00, 0x48, 0x00,       /* slot 0x00480000 */
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, /* own_key8 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* accepted_key8: none */
    0x06, 0x32, 0x2E, 0x31, 0x32, 0x2E, 0x33, /* cp_version "2.12.3" */
};

static const uint8_t G_FW_ORDER[] = {
    0x50,                   /* order section: 80 bytes */
    0x04, 0x03, 0x02, 0x01, /* order 0x01020304 */
    0x04,                   /* op: relay */
    0x00,                   /* kind: the P4 application */
    0x05, 0x31, 0x2E, 0x34, 0x2E, 0x30, /* version "1.4.0" */
    0x80, 0xEE, 0x36, 0x00, /* size 3600000 */
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E,
    0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D,
    0x5E, 0x5F, /* sha256 */
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E,
    0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D,
    0x9E, 0x9F, /* key */
    0x1E,       /* fetch section: 30 bytes */
    0x16, 0x68, 0x74, 0x74, 0x70, 0x73, 0x3A, 0x2F, 0x2F, 0x66, 0x77, 0x2E, 0x65, 0x78, 0x61,
    0x6D, 0x70, 0x6C, 0x65, 0x2E, 0x63, 0x6F, 0x6D, /* server "https://fw.example.com" */
    0x06, 0x73, 0x74, 0x61, 0x62, 0x6C, 0x65, /* channel "stable" */
    0x05,       /* relay section */
    0x00, 0x04, /* page 1024 */
    0x10,       /* count 16 */
    0x32, 0x00, /* gap_ms 50 */
};

static const uint8_t G_FW_STATE[] = {
    0x1E,                   /* run section: 30 bytes */
    0x04, 0x03, 0x02, 0x01, /* order */
    0x04,                   /* op: relay */
    0x03,                   /* state: receiving */
    0x01,                   /* flags: paused */
    0x36,                   /* why 54: no status */
    0x00, 0x00, 0x10, 0x00, /* bytes 1 MiB */
    0x80, 0xEE, 0x36, 0x00, /* size 3600000 */
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, /* id8 */
    0x05, 0x31, 0x2E, 0x34, 0x2E, 0x30, /* target "1.4.0" */
    0x09,                   /* last section */
    0x00,                   /* probation */
    0x02,                   /* last: rolled back */
    0x05, 0x31, 0x2E, 0x33, 0x2E, 0x39, /* last_version "1.3.9" */
    0x05,                   /* failed_mask */
    0x15,                   /* probe section: 21 bytes */
    0x01,                   /* asked */
    0x0D,                   /* why 13: another version */
    0x10, 0x0E, 0x00, 0x00, /* age_s 3600 */
    0x05, 0x31, 0x2E, 0x35, 0x2E, 0x30, /* offered "1.5.0" */
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, /* offered_key8 */
    0x01,                   /* key_trusted */
};

static const uint8_t G_FW_REQ[] = {
    0x0F,                                           /* one section: 15 bytes */
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, /* id8 */
    0x00, 0x00, 0x01, 0x00,                         /* offset 65536 */
    0x00, 0x04,                                     /* page 1024 */
    0x10,                                           /* count 16 */
};

static const uint8_t G_FW_PAGE[] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, /* id8 */
    0x80, 0xEE, 0x36, 0x00,                         /* total 3600000 */
    0x00, 0x00, 0x01, 0x00,                         /* offset 65536 */
    0x04, 0x00,                                     /* len 4 */
    0xDE, 0xAD, 0xBE, 0xEF,
};

/* ---- Smart Home on keypads (docs/features/11 "Smart Home and Home Assistant on keypads") --- */

static const uint8_t G_DEV_LIST_REQ[] = {
    0x02, 0xB0, 0xAD, 0x1B, /* list_crc 0x1BADB002 */
    0x00, 0x00, 0x00, 0x00, /* offset 0 */
    0x00, 0x04,             /* max 1024 */
};

static const uint8_t G_DEV_LIST_PAGE[] = {
    0x02, 0xB0, 0xAD, 0x1B, /* list_crc */
    0x10, 0x00, 0x00, 0x00, /* total 16 */
    0x0C, 0x00, 0x00, 0x00, /* offset 12 */
    0x04, 0x00,             /* len 4 */
    0xCA, 0xFE, 0xF0, 0x0D, /* the blob's last 4 bytes */
};

/* A thermostat's record section: also DEV_STATE's second record. */
static const uint8_t G_DEV_REC[] = {
    0x18,                   /* 24 bytes */
    0x03,                   /* flags: online, pending */
    0x0A,                   /* action: set the temperature */
    0x06,                   /* vflags: setpoint, temp */
    0x00,                   /* level */
    0x0B,                   /* modes: off, heat, auto */
    0x02,                   /* hvac_action: heating */
    0x66, 0x08, 0x00, 0x00, /* setpoint 21.50 */
    0xDA, 0xFD, 0xFF, 0xFF, /* temp -5.50 */
    0x00, 0x78, 0xE7, 0x68, /* since 1760000000 */
    0x80,                   /* caps: known */
    0x04, 0x68, 0x65, 0x61, 0x74, /* "heat" */
};

static const uint8_t G_DEV_STATE[] = {
    0x09,                   /* head section */
    0x02, 0xB0, 0xAD, 0x1B, /* list_crc */
    0xED, 0x5E, 0xED, 0x5E, /* states_crc 0x5EED5EED */
    0x02,                   /* count */
    0x00,                   /* index 0: a dimmed light */
    0x16, 0x01, 0x00, 0x09, 0x4B, 0x00, 0x00, /* online; level known and settable, 75 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* setpoint .. since */
    0x88,                   /* caps: dim, known */
    0x02, 0x6F, 0x6E,       /* "on" */
    0x05,                   /* index 5: G_DEV_REC */
    0x18, 0x03, 0x0A, 0x06, 0x00, 0x0B, 0x02, 0x66, 0x08, 0x00, 0x00, 0xDA, 0xFD, 0xFF, 0xFF,
    0x00, 0x78, 0xE7, 0x68, 0x80, 0x04, 0x68, 0x65, 0x61, 0x74,
};

static const uint8_t G_DEV_STATE_REQ[] = {0x02, 0xB0, 0xAD, 0x1B, 0x00, 0x00}; /* all of them */

static const uint8_t G_DEV_ACT[] = {
    0x01, 0xF0, 0xEE, 0x0B,                         /* req 0x0BEEF001 */
    0x07, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* act section: unlock */
    0x0F, 0x6C, 0x6F, 0x63, 0x6B, 0x2E, 0x66, 0x72, 0x6F, 0x6E, 0x74, 0x5F, 0x64, 0x6F, 0x6F,
    0x72, /* ref "lock.front_door", then zeros to 95 bytes */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x31, 0x32, 0x33, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* code "1234", padded */
};

static const uint8_t G_DEV_ACT_MODE[] = {
    0x03, 0xF0, 0xEE, 0x0B,                         /* req 0x0BEEF003 */
    0x07, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, /* act section: set the mode, heat */
    0x0C, 0x63, 0x6C, 0x69, 0x6D, 0x61, 0x74, 0x65, 0x2E, 0x68, 0x61, 0x6C,
    0x6C, /* ref "climate.hall", then zeros to 95 bytes */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* no code */
};

static const uint8_t G_DEV_ACK[] = {
    0x01, 0xF0, 0xEE, 0x0B, /* req */
    0x02,                   /* the code was refused ... */
    0x03,                   /* ... why */
};

static const uint8_t G_DEV_ACK_REC[] = {
    0x02, 0xF0, 0xEE, 0x0B, /* req 0x0BEEF002 */
    0x00, 0x00,             /* ok */
    0x20,                   /* optional section: 32 bytes */
    0x02, 0xB0, 0xAD, 0x1B, /* list_crc */
    0x07,                   /* index 7 */
    0x1A,                   /* its record section: 26 bytes */
    0x03, 0x05, 0x00, 0x00, 0x00, 0x00, /* online, pending; unlocking */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* setpoint .. since */
    0x00,                   /* caps */
    0x06, 0x6C, 0x6F, 0x63, 0x6B, 0x65, 0x64, /* "locked" */
};

/* One device of the list blob. */
static const uint8_t G_DEV_CAT[] = {
    0x22, /* 34 bytes */
    0x03, /* kind: a lock */
    0x01, /* source: Home Assistant */
    0x0F, 0x6C, 0x6F, 0x63, 0x6B, 0x2E, 0x66, 0x72, 0x6F, 0x6E, 0x74, 0x5F, 0x64, 0x6F, 0x6F,
    0x72,                                                       /* ref "lock.front_door" */
    0x0A, 0x46, 0x72, 0x6F, 0x6E, 0x74, 0x20, 0x64, 0x6F, 0x6F, 0x72, /* name "Front door" */
    0x04, 0x48, 0x61, 0x6C, 0x6C,                               /* area "Hall" */
};

/* ---- a keypad's settings ------------------------------------------------------------------ */

static const uint8_t G_PREFS[] = {
    0x04, 0x77, 0x07, 0x00, 0x00, /* answer section: req 0x777 */
    0x0C,                         /* info section */
    0x06, 0x61, 0x31, 0x62, 0x32, 0x63, 0x33, /* panel id "a1b2c3" */
    0x01,                         /* voice: ready */
    0xFF, 0xFF, 0x07, 0x00,       /* accepts fields 1..19 */
    0x03, 0x01, 0x0B, 0x03,       /* refusals: one, field 11 (appearance) in use */
    0x0E, 0x03,                   /* fields section: three */
    0x01, 0x01, 0x28,             /* 1 brightness: 40 */
    0x06, 0x02, 0x28, 0x05,       /* 6 night_start: 1320 (22:00) */
    0x03, 0x04, 0xC0, 0x27, 0x09, 0x00, /* 3 saver_after_ms: 600000 */
};

static const uint8_t G_PREFS_REQ[] = {0x0D, 0xF0, 0xED, 0xFE}; /* have_crc 0xFEEDF00D */

static const uint8_t G_PREFS_SET[] = {
    0x77, 0x07, 0x00, 0x00, /* req 0x777 */
    0x01,                   /* origin: Home Assistant */
    0x07, 0x02,             /* fields section: two */
    0x01, 0x01, 0x28,       /* 1 brightness: 40 */
    0x0B, 0x01, 0x02,       /* 11 appearance: automatic */
};

/* SHA-256 over op and the op section of G_CMD and G_CMD_CHANGE, computed independently with
 * Python 'cryptography' (vectors_plink_msg.py). */
static const uint8_t DIGEST_ARM[PLINK_SHA256_LEN] = {
    0xF2, 0x4A, 0x95, 0xA5, 0x81, 0x9B, 0x7C, 0x08, 0x32, 0x42, 0x9E, 0x95, 0x6F, 0x5D, 0x99, 0x66,
    0x59, 0x03, 0xFE, 0x13, 0xB0, 0x43, 0xB4, 0xF2, 0x6B, 0x77, 0x7B, 0xE9, 0x26, 0xD0, 0x1E, 0xB8,
};
static const uint8_t DIGEST_CHANGE[PLINK_SHA256_LEN] = {
    0x71, 0xAB, 0xD3, 0xA8, 0x7F, 0x82, 0x85, 0xAF, 0x30, 0x26, 0xAC, 0x71, 0x00, 0x63, 0xC1, 0x04,
    0x33, 0x49, 0xF1, 0x83, 0x6B, 0xC5, 0x07, 0x5F, 0x30, 0xFF, 0x12, 0x36, 0x55, 0xBB, 0x80, 0x8D,
};
/* SHA-256 over G_DEV_ACT's act section and padded ref (vectors_plink_msg.py). */
static const uint8_t DIGEST_DEV_ACT[PLINK_SHA256_LEN] = {
    0x17, 0xC7, 0xD8, 0xEF, 0x49, 0x80, 0x80, 0xE6, 0x96, 0x2F, 0x2D, 0xD1, 0xDD, 0x6C, 0x25, 0x66,
    0xFF, 0x30, 0x23, 0xAC, 0xF0, 0xD1, 0x02, 0xA2, 0xA1, 0x78, 0x6C, 0x9F, 0x3A, 0xD3, 0x0F, 0x42,
};

/* ---- helpers ------------------------------------------------------------------------------ */

#define EVENTS_MAX 40u /* events a test decode keeps */

typedef struct {
    alarm_event_t ev[EVENTS_MAX];
    size_t n;
    uint8_t flags;
} events_msg_t;

/* Any decoded message: the table below runs every codec the same way. */
typedef union {
    plink_pair_req_t pair_req;
    plink_pair_commit_t pair_commit;
    plink_pair_nonce_t pair_nonce;
    plink_pair_confirm_t pair_confirm;
    plink_pair_done_t pair_done;
    plink_pair_fail_t pair_fail;
    plink_hello_t hello;
    plink_rekey_t rekey;
    plink_status_t status;
    plink_zones_t zones;
    plink_zones_req_t zones_req;
    plink_cfg_req_t cfg_req;
    plink_cfg_page_t cfg_page;
    plink_ev_req_t ev_req;
    events_msg_t events;
    plink_cmd_t cmd;
    plink_result_t result;
    plink_sound_t sound;
    plink_heartbeat_t heartbeat;
    plink_time_t time;
    plink_bye_t bye;
    plink_fw_info_t fw_info;
    plink_fw_order_t fw_order;
    plink_fw_state_t fw_state;
    plink_fw_req_t fw_req;
    plink_fw_page_t fw_page;
    plink_dev_list_req_t dev_list_req;
    plink_dev_list_page_t dev_list_page;
    plink_dev_state_t dev_state;
    plink_dev_state_req_t dev_state_req;
    plink_dev_act_t dev_act;
    plink_dev_ack_t dev_ack;
    plink_prefs_t prefs;
    plink_prefs_req_t prefs_req;
    plink_prefs_set_t prefs_set;
    plink_dev_rec_t dev_rec;
    plink_dev_cat_t dev_cat;
} any_msg_t;

typedef bool (*dec_fn)(const uint8_t *p, size_t len, any_msg_t *m);
typedef bool (*enc_fn)(const any_msg_t *m, uint8_t *out, size_t cap, size_t *len);

#define CODEC(name)                                                                                \
    static bool d_##name(const uint8_t *p, size_t len, any_msg_t *m)                               \
    {                                                                                              \
        return plink_dec_##name(p, len, &m->name);                                                 \
    }                                                                                              \
    static bool e_##name(const any_msg_t *m, uint8_t *out, size_t cap, size_t *len)                \
    {                                                                                              \
        return plink_enc_##name(&m->name, out, cap, len);                                          \
    }

CODEC(pair_req)
CODEC(pair_commit)
CODEC(pair_nonce)
CODEC(pair_confirm)
CODEC(pair_done)
CODEC(pair_fail)
CODEC(rekey)
CODEC(zones_req)
CODEC(cfg_req)
CODEC(cfg_page)
CODEC(ev_req)
CODEC(cmd)
CODEC(result)
CODEC(sound)
CODEC(heartbeat)
CODEC(time)
CODEC(bye)
CODEC(fw_info)
CODEC(fw_order)
CODEC(fw_state)
CODEC(fw_req)
CODEC(fw_page)
CODEC(dev_list_req)
CODEC(dev_list_page)
CODEC(dev_state)
CODEC(dev_state_req)
CODEC(dev_act)
CODEC(dev_ack)
CODEC(prefs)
CODEC(prefs_req)
CODEC(prefs_set)

/* The section helpers: one record section, read from the start of a payload. */
static bool d_dev_rec(const uint8_t *p, size_t len, any_msg_t *m)
{
    return plink_dec_dev_rec(p, len, &m->dev_rec, NULL);
}

static bool e_dev_rec(const any_msg_t *m, uint8_t *out, size_t cap, size_t *len)
{
    return plink_enc_dev_rec(&m->dev_rec, out, cap, len);
}

static bool d_dev_cat(const uint8_t *p, size_t len, any_msg_t *m)
{
    return plink_dec_dev_cat(p, len, &m->dev_cat, NULL);
}

static bool e_dev_cat(const any_msg_t *m, uint8_t *out, size_t cap, size_t *len)
{
    return plink_enc_dev_cat(&m->dev_cat, out, cap, len);
}

static bool d_hello(const uint8_t *p, size_t len, any_msg_t *m)
{
    return plink_dec_hello(p, len, true, &m->hello);
}

static bool e_hello(const any_msg_t *m, uint8_t *out, size_t cap, size_t *len)
{
    return plink_enc_hello(&m->hello, true, out, cap, len);
}

static bool d_welcome(const uint8_t *p, size_t len, any_msg_t *m)
{
    return plink_dec_hello(p, len, false, &m->hello);
}

static bool e_welcome(const any_msg_t *m, uint8_t *out, size_t cap, size_t *len)
{
    return plink_enc_hello(&m->hello, false, out, cap, len);
}

static bool d_status(const uint8_t *p, size_t len, any_msg_t *m)
{
    return plink_dec_status(p, len, &m->status);
}

static bool e_status(const any_msg_t *m, uint8_t *out, size_t cap, size_t *len)
{
    uint8_t written = 0;
    if (plink_enc_status(&m->status, 0, out, cap, len, &written) && written == m->status.count) {
        return true;
    }
    *len = 0; /* not every record fit: not this message */
    return false;
}

static bool d_zones(const uint8_t *p, size_t len, any_msg_t *m)
{
    return plink_dec_zones(p, len, &m->zones);
}

static bool d_events(const uint8_t *p, size_t len, any_msg_t *m)
{
    return plink_dec_events(p, len, m->events.ev, EVENTS_MAX, &m->events.n, &m->events.flags);
}

static bool e_events(const any_msg_t *m, uint8_t *out, size_t cap, size_t *len)
{
    size_t written = 0;
    return plink_enc_events(m->events.ev, m->events.n, m->events.flags, out, cap, len, &written);
}

typedef struct {
    const char *name;
    const uint8_t *golden;
    size_t golden_len;
    dec_fn dec;
    enc_fn enc; /* NULL: ZONES, whose records are read by plink_view.c */
} codec_t;

#define ROW(label, g, d, e) {label, g, sizeof(g), d, e}

static const codec_t CODECS[] = {
    ROW("PAIR_REQ", G_PAIR_REQ, d_pair_req, e_pair_req),
    ROW("PAIR_COMMIT", G_PAIR_COMMIT, d_pair_commit, e_pair_commit),
    ROW("PAIR_NONCE", G_PAIR_NONCE, d_pair_nonce, e_pair_nonce),
    ROW("PAIR_REVEAL", G_PAIR_REVEAL, d_pair_nonce, e_pair_nonce),
    ROW("PAIR_CONFIRM", G_PAIR_CONFIRM, d_pair_confirm, e_pair_confirm),
    ROW("PAIR_DONE", G_PAIR_DONE, d_pair_done, e_pair_done),
    ROW("PAIR_FAIL", G_PAIR_FAIL, d_pair_fail, e_pair_fail),
    ROW("HELLO", G_HELLO, d_hello, e_hello),
    ROW("WELCOME", G_WELCOME, d_welcome, e_welcome),
    ROW("REKEY", G_REKEY, d_rekey, e_rekey),
    ROW("STATUS", G_STATUS, d_status, e_status),
    ROW("ZONES", G_ZONES, d_zones, NULL),
    ROW("ZONES_REQ", G_ZONES_REQ, d_zones_req, e_zones_req),
    ROW("CFG_REQ", G_CFG_REQ, d_cfg_req, e_cfg_req),
    ROW("CFG_PAGE", G_CFG_PAGE, d_cfg_page, e_cfg_page),
    ROW("EV_REQ", G_EV_REQ, d_ev_req, e_ev_req),
    ROW("EVENTS", G_EVENTS, d_events, e_events),
    ROW("CMD", G_CMD, d_cmd, e_cmd),
    ROW("CMD change", G_CMD_CHANGE, d_cmd, e_cmd),
    ROW("RESULT", G_RESULT, d_result, e_result),
    ROW("SOUND", G_SOUND, d_sound, e_sound),
    ROW("HEARTBEAT", G_HEARTBEAT, d_heartbeat, e_heartbeat),
    ROW("HEARTBEAT prefs", G_HEARTBEAT_PREFS, d_heartbeat, e_heartbeat),
    ROW("TIME", G_TIME, d_time, e_time),
    ROW("BYE", G_BYE, d_bye, e_bye),
    ROW("FW_INFO", G_FW_INFO, d_fw_info, e_fw_info),
    ROW("FW_ORDER", G_FW_ORDER, d_fw_order, e_fw_order),
    ROW("FW_STATE", G_FW_STATE, d_fw_state, e_fw_state),
    ROW("FW_REQ", G_FW_REQ, d_fw_req, e_fw_req),
    ROW("FW_PAGE", G_FW_PAGE, d_fw_page, e_fw_page),
    ROW("DEV_LIST_REQ", G_DEV_LIST_REQ, d_dev_list_req, e_dev_list_req),
    ROW("DEV_LIST_PAGE", G_DEV_LIST_PAGE, d_dev_list_page, e_dev_list_page),
    ROW("DEV_STATE", G_DEV_STATE, d_dev_state, e_dev_state),
    ROW("DEV_STATE_REQ", G_DEV_STATE_REQ, d_dev_state_req, e_dev_state_req),
    ROW("DEV_ACT", G_DEV_ACT, d_dev_act, e_dev_act),
    ROW("DEV_ACT mode", G_DEV_ACT_MODE, d_dev_act, e_dev_act),
    ROW("DEV_ACK", G_DEV_ACK, d_dev_ack, e_dev_ack),
    ROW("DEV_ACK rec", G_DEV_ACK_REC, d_dev_ack, e_dev_ack),
    ROW("PREFS", G_PREFS, d_prefs, e_prefs),
    ROW("PREFS_REQ", G_PREFS_REQ, d_prefs_req, e_prefs_req),
    ROW("PREFS_SET", G_PREFS_SET, d_prefs_set, e_prefs_set),
    ROW("dev record", G_DEV_REC, d_dev_rec, e_dev_rec),
    ROW("catalog record", G_DEV_CAT, d_dev_cat, e_dev_cat),
};
#define CODEC_COUNT (sizeof(CODECS) / sizeof(CODECS[0]))

static const codec_t *codec(const char *name)
{
    for (size_t i = 0; i < CODEC_COUNT; i++) {
        if (strcmp(CODECS[i].name, name) == 0) {
            return &CODECS[i];
        }
    }
    TEST_FAIL_MESSAGE("no such codec");
    return NULL;
}

static any_msg_t M1, M2;
static uint8_t OUT[PLINK_FRAME_MAX], OUT2[PLINK_FRAME_MAX];

/* Decodes p and encodes it again: the bytes must be expect (the decoded values are exactly the
 * ones expect carries, whatever p added or left out around them). */
static void expect_canonical(const codec_t *c, const uint8_t *p, size_t len, const uint8_t *expect,
                             size_t expect_len)
{
    TEST_ASSERT_TRUE_MESSAGE(c->dec(p, len, &M1), c->name);
    size_t n = 0;
    TEST_ASSERT_TRUE_MESSAGE(c->enc(&M1, OUT, sizeof(OUT), &n), c->name);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(expect_len, n, c->name);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(expect, OUT, expect_len, c->name);
}

/* Copies g into out with ins (n bytes) inserted at offset at; returns the new length. */
static size_t splice(uint8_t *out, const uint8_t *g, size_t g_len, size_t at, const uint8_t *ins,
                     size_t n)
{
    memcpy(out, g, at);
    memcpy(out + at, ins, n);
    memcpy(out + at + n, g + at, g_len - at);
    return g_len + n;
}

/* Copies g into out without the n bytes at offset at; returns the new length. */
static size_t cut(uint8_t *out, const uint8_t *g, size_t g_len, size_t at, size_t n)
{
    memcpy(out, g, at);
    memcpy(out + at, g + at + n, g_len - at - n);
    return g_len - n;
}

static void fill_seq(uint8_t *out, size_t n, uint8_t first)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)(first + i);
    }
}

/* SplitMix64: a fixed seed, so every run sees the same inputs. */
static uint64_t RNG;

static uint64_t rnd(void)
{
    uint64_t z = (RNG += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint8_t rnd8(void)
{
    return (uint8_t)rnd();
}

static uint16_t rnd16(void)
{
    return (uint16_t)rnd();
}

static uint32_t rnd32(void)
{
    return (uint32_t)rnd();
}

static size_t rnd_below(size_t n)
{
    return n > 0u ? (size_t)(rnd() % n) : 0u;
}

static void rnd_bytes(uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = rnd8();
    }
}

/* A random printable ASCII string of up to max bytes. */
static void rnd_text(char *out, size_t max)
{
    const size_t n = rnd_below(max + 1u);
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)(0x20u + rnd_below(0x5Fu));
    }
    out[n] = '\0';
}

/* A random code of 0..ALARM_CODE_MAX_LEN digits. */
static void rnd_code(char *out)
{
    const size_t n = rnd_below(ALARM_CODE_MAX_LEN + 1u);
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)('0' + rnd_below(10));
    }
    out[n] = '\0';
}

/* ---- plink_wire --------------------------------------------------------------------------- */

/* LINK-017: numbers are little-endian; a writer that runs out of room stops for good. */
static void test_wire_numbers(void)
{
    uint8_t buf[32];
    plink_w_t w;
    plink_w_init(&w, buf, sizeof(buf));
    plink_w_u8(&w, 0x12);
    plink_w_u16(&w, 0x3456);
    plink_w_u32(&w, 0x789ABCDEu);
    plink_w_u64(&w, 0x0102030405060708ull);
    plink_w_i64(&w, -2);
    static const uint8_t expect[] = {
        0x12, 0x56, 0x34, 0xDE, 0xBC, 0x9A, 0x78, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    TEST_ASSERT_TRUE(w.ok);
    TEST_ASSERT_EQUAL_size_t(sizeof(expect), w.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, buf, sizeof(expect));

    plink_r_t r;
    plink_r_init(&r, buf, w.len);
    TEST_ASSERT_EQUAL_HEX8(0x12, plink_r_u8(&r));
    TEST_ASSERT_EQUAL_HEX16(0x3456, plink_r_u16(&r));
    TEST_ASSERT_EQUAL_HEX32(0x789ABCDEu, plink_r_u32(&r));
    TEST_ASSERT_EQUAL_HEX64(0x0102030405060708ull, plink_r_u64(&r));
    TEST_ASSERT_EQUAL_INT64(-2, plink_r_i64(&r));
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_size_t(0, plink_r_left(&r));
    TEST_ASSERT_EQUAL_HEX8(0, plink_r_u8(&r)); /* past the end: 0 and not ok */
    TEST_ASSERT_FALSE(r.ok);

    /* The extremes of i64 survive the trip. */
    const int64_t extremes[] = {INT64_MIN, INT64_MIN + 1, -1, 0, 1, INT64_MAX};
    for (size_t i = 0; i < sizeof(extremes) / sizeof(extremes[0]); i++) {
        plink_w_init(&w, buf, sizeof(buf));
        plink_w_i64(&w, extremes[i]);
        plink_r_init(&r, buf, w.len);
        TEST_ASSERT_EQUAL_INT64(extremes[i], plink_r_i64(&r));
    }

    /* Out of room: nothing written, and nothing after it either, even what would fit. */
    plink_w_init(&w, buf, 3);
    plink_w_u32(&w, 1);
    TEST_ASSERT_FALSE(w.ok);
    TEST_ASSERT_EQUAL_size_t(0, w.len);
    plink_w_u8(&w, 1);
    TEST_ASSERT_EQUAL_size_t(0, w.len);
    plink_w_init(&w, NULL, 10); /* no buffer: no room */
    plink_w_u8(&w, 1);
    TEST_ASSERT_FALSE(w.ok);

    /* A reader that ran short stays short, and its bytes read 0. */
    static const uint8_t three[] = {1, 2, 3};
    plink_r_init(&r, three, sizeof(three));
    TEST_ASSERT_EQUAL_HEX32(0, plink_r_u32(&r));
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_HEX8(0, plink_r_u8(&r));
    TEST_ASSERT_EQUAL_size_t(0, plink_r_left(&r));
    uint8_t got[4] = {9, 9, 9, 9};
    plink_r_bytes(&r, got, sizeof(got));
    TEST_ASSERT_EACH_EQUAL_HEX8(0, got, sizeof(got));
    TEST_ASSERT_NULL(plink_r_take(&r, 0));
}

/* LINK-017: a section carries its length; a reader takes the fields it knows, skips what a newer
 * writer appended and reads the fields an older one left out as 0. */
static void test_wire_sections(void)
{
    uint8_t buf[300];
    plink_w_t w;
    plink_w_init(&w, buf, sizeof(buf));
    const size_t at = plink_w_section(&w);
    plink_w_u16(&w, 0xBEEF);
    plink_w_u8(&w, 7);
    plink_w_section_end(&w, at);
    plink_w_u8(&w, 0x55);
    static const uint8_t expect[] = {0x03, 0xEF, 0xBE, 0x07, 0x55};
    TEST_ASSERT_TRUE(w.ok);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, buf, sizeof(expect));

    /* A reader that knows one more field: zeros past the section's end, still ok. */
    plink_r_t r;
    plink_r_init(&r, buf, w.len);
    plink_r_t s = plink_r_section(&r);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, plink_r_u16(&s));
    TEST_ASSERT_EQUAL_HEX8(7, plink_r_u8(&s));
    TEST_ASSERT_EQUAL_HEX32(0, plink_r_u32(&s));
    TEST_ASSERT_TRUE(s.ok);
    TEST_ASSERT_EQUAL_HEX8(0x55, plink_r_u8(&r)); /* r moved past the whole section */
    TEST_ASSERT_TRUE(r.ok);

    /* A reader that knows fewer fields: the rest of the section is skipped. */
    plink_r_init(&r, buf, w.len);
    s = plink_r_section(&r);
    TEST_ASSERT_EQUAL_HEX8(0xEF, plink_r_u8(&s));
    TEST_ASSERT_EQUAL_HEX8(0x55, plink_r_u8(&r));

    /* A field cut by the section's end reads its missing bytes as 0. */
    static const uint8_t two[] = {0x02, 0x34, 0x12, 0x99};
    plink_r_init(&r, two, sizeof(two));
    s = plink_r_section(&r);
    TEST_ASSERT_EQUAL_HEX32(0x1234, plink_r_u32(&s));
    TEST_ASSERT_TRUE(s.ok);
    TEST_ASSERT_EQUAL_size_t(0, plink_r_left(&s));
    TEST_ASSERT_NULL(plink_r_take(&s, 1)); /* take never pads */
    TEST_ASSERT_FALSE(s.ok);

    /* A section inside a section that ends first: cut where its parent ends, zeros after. */
    static const uint8_t nested[] = {0x03, 0x04, 0x11, 0x22, 0x77};
    plink_r_init(&r, nested, sizeof(nested));
    s = plink_r_section(&r);
    plink_r_t inner = plink_r_section(&s);
    TEST_ASSERT_TRUE(s.ok);
    TEST_ASSERT_TRUE(inner.ok);
    TEST_ASSERT_EQUAL_HEX32(0x2211, plink_r_u32(&inner));
    TEST_ASSERT_EQUAL_HEX8(0x77, plink_r_u8(&r));
    TEST_ASSERT_TRUE(r.ok);
    plink_r_init(&r, nested, sizeof(nested));
    TEST_ASSERT_EQUAL_HEX8(0x03, plink_r_u8(&r));
    TEST_ASSERT_EQUAL_PTR(nested + 1, plink_r_take(&r, 2));
    TEST_ASSERT_EQUAL_size_t(3, r.pos);

    /* A section that claims more than the payload holds: the payload is refused. */
    static const uint8_t over[] = {0x05, 0x01, 0x02};
    plink_r_init(&r, over, sizeof(over));
    s = plink_r_section(&r);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_FALSE(s.ok);
    TEST_ASSERT_EQUAL_HEX8(0, plink_r_u8(&s));
    /* ... and an empty payload has no section at all. */
    plink_r_init(&r, over, 0);
    (void)plink_r_section(&r);
    TEST_ASSERT_FALSE(r.ok);

    /* A section of 255 bytes is the most a length byte says; 256 is refused. */
    plink_w_init(&w, buf, sizeof(buf));
    size_t big = plink_w_section(&w);
    for (size_t i = 0; i < 255u; i++) {
        plink_w_u8(&w, (uint8_t)i);
    }
    plink_w_section_end(&w, big);
    TEST_ASSERT_TRUE(w.ok);
    TEST_ASSERT_EQUAL_HEX8(0xFF, buf[0]);
    plink_w_init(&w, buf, sizeof(buf));
    big = plink_w_section(&w);
    for (size_t i = 0; i < 256u; i++) {
        plink_w_u8(&w, (uint8_t)i);
    }
    plink_w_section_end(&w, big);
    TEST_ASSERT_FALSE(w.ok);
    plink_w_init(&w, buf, sizeof(buf));
    plink_w_section_end(&w, 0); /* no section started there */
    TEST_ASSERT_FALSE(w.ok);
}

/* LINK-004 (docs/features/11 "Messages"): a string goes as a length byte and at most max bytes,
 * cut on a whole character; the reader cleans it and fits it into its buffer. */
static void test_wire_strings(void)
{
    uint8_t buf[300];
    plink_w_t w;
    plink_w_init(&w, buf, sizeof(buf));
    plink_w_str(&w, "Hall", PLINK_NAME_MAX);
    plink_w_str(&w, NULL, PLINK_NAME_MAX);
    plink_w_str(&w, "abc\xE2\x82\xAC", 5); /* "abc€": the euro sign doesn't fit whole in 5 */
    plink_w_str(&w, "abc\xE2\x82\xAC", 6);
    static const uint8_t expect[] = {0x04, 0x48, 0x61, 0x6C, 0x6C, 0x00, 0x03, 0x61, 0x62, 0x63,
                                     0x06, 0x61, 0x62, 0x63, 0xE2, 0x82, 0xAC};
    TEST_ASSERT_TRUE(w.ok);
    TEST_ASSERT_EQUAL_size_t(sizeof(expect), w.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, buf, sizeof(expect));

    /* At most 255 bytes, whatever max says. */
    char long_s[300];
    memset(long_s, 'x', 299);
    long_s[299] = '\0';
    plink_w_init(&w, buf, sizeof(buf));
    plink_w_str(&w, long_s, 1000);
    TEST_ASSERT_TRUE(w.ok);
    TEST_ASSERT_EQUAL_HEX8(255, buf[0]);
    TEST_ASSERT_EQUAL_size_t(256, w.len);

    char out[PLINK_NAME_MAX + 1];
    plink_r_t r;
    plink_r_init(&r, expect, sizeof(expect));
    plink_r_str(&r, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Hall", out);
    plink_r_str(&r, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    plink_r_str(&r, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("abc", out);
    plink_r_str(&r, out, 5); /* a smaller buffer: cut before the euro sign */
    TEST_ASSERT_EQUAL_STRING("abc", out);
    TEST_ASSERT_TRUE(r.ok);

    /* A string longer than the payload: refused. */
    static const uint8_t short_s[] = {0x05, 0x41, 0x42};
    plink_r_init(&r, short_s, sizeof(short_s));
    plink_r_str(&r, out, sizeof(out));
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("", out);
    /* ... inside a section, what is there (the section's end reads as zeros, which are dropped). */
    static const uint8_t in_section[] = {0x03, 0x05, 0x41, 0x42};
    plink_r_init(&r, in_section, sizeof(in_section));
    plink_r_t s = plink_r_section(&r);
    plink_r_str(&s, out, sizeof(out));
    TEST_ASSERT_TRUE(s.ok);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("AB", out);
}

static void expect_clean(const char *in, size_t in_len, size_t cap, const char *expect)
{
    char out[64];
    memset(out, 0x5A, sizeof(out));
    plink_clean_text(in, in_len, out, cap);
    TEST_ASSERT_EQUAL_STRING(expect, out);
}

/* LINK-004 (docs/features/11 "Messages"): a name from the network is shown on a screen, so only
 * valid UTF-8 is kept, without controls, DEL or bidi controls, cut on a whole character. */
static void test_clean_text(void)
{
#define CLEAN(lit, cap, expect) expect_clean(lit, sizeof(lit) - 1u, cap, expect)
    CLEAN("Hall", 64, "Hall");
    /* C0 controls (NUL, ESC, US), DEL and C1 controls (NEL, CSI, APC) dropped; NBSP kept. */
    CLEAN("A\x00" "B\x1B[31m\x1F" "C\x7F" "D\xC2\x85" "E\xC2\x9B" "F\xC2\x9F" "G\xC2\xA0" "H", 64,
          "AB[31mCDEFG\xC2\xA0H");
    /* Every bidi control dropped (ALM, LRM, RLM, LRE..RLO, LRI..PDI); their neighbours kept. */
    CLEAN("a\xD8\x9C" "b\xE2\x80\x8E" "c\xE2\x80\x8F" "d\xE2\x80\xAA" "e\xE2\x80\xAB"
          "f\xE2\x80\xAC" "g\xE2\x80\xAD" "h\xE2\x80\xAE" "i\xE2\x81\xA6" "j\xE2\x81\xA7"
          "k\xE2\x81\xA8" "l\xE2\x81\xA9" "m",
          64, "abcdefghijklm");
    CLEAN("\xE2\x80\x8D\xE2\x80\xA9\xE2\x80\xAF\xE2\x81\xA5\xE2\x81\xAA", 64,
          "\xE2\x80\x8D\xE2\x80\xA9\xE2\x80\xAF\xE2\x81\xA5\xE2\x81\xAA"); /* the neighbours */
    /* Not UTF-8: lone continuation bytes, overlong forms, surrogates, past U+10FFFF, bytes that
     * never appear, sequences cut short. */
    CLEAN("X\x80\xBF\xC0\xAF\xC1\xBF\xE0\x80\xAF\xF0\x80\x80\xAF\xED\xA0\x80\xED\xBF\xBF"
          "\xF4\x90\x80\x80\xF5\x80\x80\x80\xF8\xFE\xFF\xC3Y",
          64, "XY");
    CLEAN("A\xE2\x82" "B\xF0\x9F\x94" "C\xE2", 64, "ABC");
    /* Valid characters of every length kept, the edges of the ranges included. */
    CLEAN("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x94\x94\xED\x9F\xBF\xEE\x80\x80\xF4\x8F\xBF\xBF\xC2\x80"
          "\xDF\xBF\xE0\xA0\x80\xF0\x90\x80\x80",
          64, "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x94\x94\xED\x9F\xBF\xEE\x80\x80\xF4\x8F\xBF\xBF\xDF\xBF"
              "\xE0\xA0\x80\xF0\x90\x80\x80"); /* U+0080 is C1 */
    /* The buffer: cap - 1 bytes, never half a character, nothing after the first that doesn't
     * fit. */
    CLEAN("ab\xE2\x82\xAC" "c", 4, "ab");
    CLEAN("ab\xE2\x82\xAC" "c", 5, "ab");
    CLEAN("ab\xE2\x82\xAC" "c", 6, "ab\xE2\x82\xAC");
    CLEAN("ab\xE2\x82\xAC" "c", 7, "ab\xE2\x82\xAC" "c");
    CLEAN("ab", 1, "");
    CLEAN("\xF0\x9F\x94\x94", 4, "");
    CLEAN("\xF0\x9F\x94\x94", 5, "\xF0\x9F\x94\x94");
#undef CLEAN
    char untouched[2] = {'q', 'q'};
    plink_clean_text("ab", 2, untouched, 0); /* no room at all: nothing written */
    TEST_ASSERT_EQUAL_HEX8('q', untouched[0]);
    plink_clean_text(NULL, 0, untouched, sizeof(untouched));
    TEST_ASSERT_EQUAL_STRING("", untouched);
}

/* ---- golden bytes, message by message ----------------------------------------------------- */

static void expect_golden(const char *name, const uint8_t *got, size_t got_len)
{
    const codec_t *c = codec(name);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(c->golden_len, got_len, name);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(c->golden, got, c->golden_len, name);
}

/* LINK-017: the pairing messages are laid out as docs/features/11 "Messages" says. */
static void test_golden_pairing(void)
{
    size_t n = 0;
    plink_pair_req_t req;
    memset(&req, 0, sizeof(req));
    req.attempt = 0x11223344u;
    req.proto = 1;
    req.caps = 0xA55A;
    req.kind = PLINK_KIND_PANEL;
    req.cap = (plink_caps_t){4, 128, 64, 16};
    fill_seq(req.pk, sizeof(req.pk), 0x00);
    strcpy(req.name, "Hall");
    strcpy(req.fw, "1.2.3");
    TEST_ASSERT_TRUE(plink_enc_pair_req(&req, OUT, sizeof(OUT), &n));
    expect_golden("PAIR_REQ", OUT, n);
    plink_pair_req_t req2;
    TEST_ASSERT_TRUE(plink_dec_pair_req(G_PAIR_REQ, sizeof(G_PAIR_REQ), &req2));
    TEST_ASSERT_EQUAL_HEX32(0x11223344u, req2.attempt);
    TEST_ASSERT_EQUAL_UINT8(1, req2.proto);
    TEST_ASSERT_EQUAL_HEX16(0xA55A, req2.caps);
    TEST_ASSERT_EQUAL_UINT8(PLINK_KIND_PANEL, req2.kind);
    TEST_ASSERT_EQUAL_UINT8(4, req2.cap.partitions);
    TEST_ASSERT_EQUAL_UINT16(128, req2.cap.zones);
    TEST_ASSERT_EQUAL_UINT8(64, req2.cap.users);
    TEST_ASSERT_EQUAL_UINT8(16, req2.cap.devices);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(req.pk, req2.pk, PLINK_X25519_LEN);
    TEST_ASSERT_EQUAL_STRING("Hall", req2.name);
    TEST_ASSERT_EQUAL_STRING("1.2.3", req2.fw);

    plink_pair_commit_t com;
    memset(&com, 0, sizeof(com));
    com.attempt = 0x01020304u;
    com.proto = 1;
    com.caps = 3;
    fill_seq(com.pk, sizeof(com.pk), 0x20);
    fill_seq(com.commit, sizeof(com.commit), 0x40);
    strcpy(com.name, "Harbor House");
    strcpy(com.panel_id, "a1b2c3");
    TEST_ASSERT_TRUE(plink_enc_pair_commit(&com, OUT, sizeof(OUT), &n));
    expect_golden("PAIR_COMMIT", OUT, n);
    plink_pair_commit_t com2;
    TEST_ASSERT_TRUE(plink_dec_pair_commit(G_PAIR_COMMIT, sizeof(G_PAIR_COMMIT), &com2));
    TEST_ASSERT_EQUAL_HEX32(0x01020304u, com2.attempt);
    TEST_ASSERT_EQUAL_HEX16(3, com2.caps);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(com.pk, com2.pk, PLINK_X25519_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(com.commit, com2.commit, PLINK_SHA256_LEN);
    TEST_ASSERT_EQUAL_STRING("Harbor House", com2.name);
    TEST_ASSERT_EQUAL_STRING("a1b2c3", com2.panel_id);

    plink_pair_nonce_t nonce = {.attempt = 0xDEADBEEFu};
    fill_seq(nonce.n, sizeof(nonce.n), 0x60);
    TEST_ASSERT_TRUE(plink_enc_pair_nonce(&nonce, OUT, sizeof(OUT), &n));
    expect_golden("PAIR_NONCE", OUT, n); /* PAIR_REVEAL has the same layout */
    plink_pair_nonce_t nonce2;
    TEST_ASSERT_TRUE(plink_dec_pair_nonce(G_PAIR_NONCE, sizeof(G_PAIR_NONCE), &nonce2));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, nonce2.attempt);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(nonce.n, nonce2.n, PLINK_NONCE_LEN);

    plink_pair_confirm_t conf = {.attempt = 7};
    fill_seq(conf.mac, sizeof(conf.mac), 0xA0);
    TEST_ASSERT_TRUE(plink_enc_pair_confirm(&conf, OUT, sizeof(OUT), &n));
    expect_golden("PAIR_CONFIRM", OUT, n);
    plink_pair_confirm_t conf2;
    TEST_ASSERT_TRUE(plink_dec_pair_confirm(G_PAIR_CONFIRM, sizeof(G_PAIR_CONFIRM), &conf2));
    TEST_ASSERT_EQUAL_UINT32(7, conf2.attempt);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(conf.mac, conf2.mac, PLINK_MAC_LEN);

    plink_pair_done_t done = {.attempt = 7, .device = 3, .home = 2};
    fill_seq(done.mac, sizeof(done.mac), 0xB0);
    TEST_ASSERT_TRUE(plink_enc_pair_done(&done, OUT, sizeof(OUT), &n));
    expect_golden("PAIR_DONE", OUT, n);
    plink_pair_done_t done2;
    TEST_ASSERT_TRUE(plink_dec_pair_done(G_PAIR_DONE, sizeof(G_PAIR_DONE), &done2));
    TEST_ASSERT_EQUAL_UINT32(7, done2.attempt);
    TEST_ASSERT_EQUAL_UINT8(3, done2.device);
    TEST_ASSERT_EQUAL_UINT8(2, done2.home);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(done.mac, done2.mac, PLINK_MAC_LEN);

    const plink_pair_fail_t fail = {.attempt = 0xABCD, .reason = PLINK_FAIL_OVERLAP};
    TEST_ASSERT_TRUE(plink_enc_pair_fail(&fail, OUT, sizeof(OUT), &n));
    expect_golden("PAIR_FAIL", OUT, n);
    plink_pair_fail_t fail2;
    TEST_ASSERT_TRUE(plink_dec_pair_fail(G_PAIR_FAIL, sizeof(G_PAIR_FAIL), &fail2));
    TEST_ASSERT_EQUAL_HEX32(0xABCD, fail2.attempt);
    TEST_ASSERT_EQUAL_UINT8(PLINK_FAIL_OVERLAP, fail2.reason);
}

/* LINK-017: HELLO, WELCOME and REKEY as in docs/features/11 "Messages"; the MAC is the payload's
 * last 16 bytes. */
static void test_golden_handshake(void)
{
    size_t n = 0;
    plink_hello_t h;
    memset(&h, 0, sizeof(h));
    h.proto = 1;
    h.caps = 0x0102;
    h.cap = (plink_caps_t){2, 320, 32, 8};
    fill_seq(h.n, sizeof(h.n), 0x10);
    fill_seq(h.e, sizeof(h.e), 0x30);
    fill_seq(h.mac, sizeof(h.mac), 0xF0);
    TEST_ASSERT_TRUE(plink_enc_hello(&h, true, OUT, sizeof(OUT), &n));
    expect_golden("HELLO", OUT, n);
    plink_hello_t h2;
    TEST_ASSERT_TRUE(plink_dec_hello(G_HELLO, sizeof(G_HELLO), true, &h2));
    TEST_ASSERT_EQUAL_UINT8(1, h2.proto);
    TEST_ASSERT_EQUAL_HEX16(0x0102, h2.caps);
    TEST_ASSERT_EQUAL_UINT8(2, h2.cap.partitions);
    TEST_ASSERT_EQUAL_UINT16(320, h2.cap.zones);
    TEST_ASSERT_EQUAL_UINT8(32, h2.cap.users);
    TEST_ASSERT_EQUAL_UINT8(8, h2.cap.devices);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(h.n, h2.n, PLINK_NONCE_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(h.e, h2.e, PLINK_X25519_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(h.mac, h2.mac, PLINK_MAC_LEN);

    h.caps = 0;
    TEST_ASSERT_TRUE(plink_enc_hello(&h, false, OUT, sizeof(OUT), &n));
    expect_golden("WELCOME", OUT, n); /* no capacities section */
    TEST_ASSERT_TRUE(plink_dec_hello(G_WELCOME, sizeof(G_WELCOME), false, &h2));
    TEST_ASSERT_EQUAL_UINT8(1, h2.proto);
    TEST_ASSERT_EQUAL_UINT16(0, h2.cap.zones);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(h.n, h2.n, PLINK_NONCE_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(h.e, h2.e, PLINK_X25519_LEN);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(h.mac, h2.mac, PLINK_MAC_LEN);
    /* Read as the other one, each is refused or read differently: the type tells them apart. */
    TEST_ASSERT_FALSE(plink_dec_hello(G_WELCOME, sizeof(G_WELCOME), true, &h2));

    const plink_rekey_t rk = {.sid = 0x89ABCDEFu};
    TEST_ASSERT_TRUE(plink_enc_rekey(&rk, OUT, sizeof(OUT), &n));
    expect_golden("REKEY", OUT, n);
    plink_rekey_t rk2;
    TEST_ASSERT_TRUE(plink_dec_rekey(G_REKEY, sizeof(G_REKEY), &rk2));
    TEST_ASSERT_EQUAL_HEX32(0x89ABCDEFu, rk2.sid);
}

/* The STATUS of G_STATUS. */
static void golden_status(plink_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->epoch_ms = 0x0102030405060708ll;
    st->partitions = 0x05;
    st->home = 1;
    st->sys_troubles = 9;
    st->sys_restored = 2;
    st->dev_troubles = 4;
    st->default_codes = 3;
    st->flags = PLINK_SF_PROGRAM | PLINK_SF_EXIT_SOUNDS;
    st->recovery_left_ms = 120000;
    st->lockout_left_ms = 30000;
    st->report = PLINK_REPORT_ONLINE;
    st->zones_used = 16;
    st->last_event_seq = 1234;
    st->cfg_crc = 0xCAFEBABEu;
    st->cfg_len = 0x1234;
    st->zones_crc = 0x12345678u;
    st->mono_ms = 0x112233445566ull;
    st->count = 2;
    plink_part_t *p = &st->part[0];
    p->number = 1;
    p->state = ALARM_STATE_ENTRY_DELAY;
    p->level = ALARM_LEVEL_AWAY;
    p->arm_flags = ALARM_ARM_SILENT_EXIT;
    p->armed_by = 5;
    p->arm_src_kind = ALARM_SRC_KEYPAD;
    p->arm_src_id = 3;
    p->ready_levels = 0x0E;
    p->flags = PLINK_PF_READY | PLINK_PF_WALK_TEST | PLINK_PF_TRIGGERED | PLINK_PF_TROUBLE_UNACK;
    p->sounder = ALARM_SOUND_ENTRY;
    p->bell = ALARM_PAT_STEADY;
    p->delay_left_ms = 31000;
    p->memory = 0x8002;
    p->faults = 2;
    p->bypassed = 1;
    p->alarms = 3;
    p->troubles = 4;
    p->walk_left_ms = 60000;
    p->walk_age_ms = 1000;
    p->walk_id = 0x00ABCDEFu;
    p->walk_seq = 0x11;
    p->walk_tested = 5;
    p->walk_total = 9;
    p->walk_flags = ALARM_WALK_F_LIFE24;
    p->walk_started_by = 2;
    p->walk_src_kind = ALARM_SRC_KEYPAD;
    p->walk_src_id = 4;
    p = &st->part[1];
    p->number = 3;
    p->ready_levels = 0x0E;
    p->flags = PLINK_PF_READY;
}

static void expect_part(const plink_part_t *e, const plink_part_t *g)
{
    TEST_ASSERT_EQUAL_UINT8(e->number, g->number);
    TEST_ASSERT_EQUAL_UINT8(e->state, g->state);
    TEST_ASSERT_EQUAL_UINT8(e->level, g->level);
    TEST_ASSERT_EQUAL_UINT8(e->arm_flags, g->arm_flags);
    TEST_ASSERT_EQUAL_UINT8(e->armed_by, g->armed_by);
    TEST_ASSERT_EQUAL_UINT8(e->arm_src_kind, g->arm_src_kind);
    TEST_ASSERT_EQUAL_UINT16(e->arm_src_id, g->arm_src_id);
    TEST_ASSERT_EQUAL_UINT8(e->ready_levels, g->ready_levels);
    TEST_ASSERT_EQUAL_UINT8(e->flags, g->flags);
    TEST_ASSERT_EQUAL_UINT8(e->sounder, g->sounder);
    TEST_ASSERT_EQUAL_UINT8(e->bell, g->bell);
    TEST_ASSERT_EQUAL_UINT32(e->delay_left_ms, g->delay_left_ms);
    TEST_ASSERT_EQUAL_UINT16(e->memory, g->memory);
    TEST_ASSERT_EQUAL_UINT16(e->faults, g->faults);
    TEST_ASSERT_EQUAL_UINT16(e->bypassed, g->bypassed);
    TEST_ASSERT_EQUAL_UINT16(e->alarms, g->alarms);
    TEST_ASSERT_EQUAL_UINT16(e->troubles, g->troubles);
    TEST_ASSERT_EQUAL_UINT32(e->walk_left_ms, g->walk_left_ms);
    TEST_ASSERT_EQUAL_UINT32(e->walk_age_ms, g->walk_age_ms);
    TEST_ASSERT_EQUAL_UINT32(e->walk_id, g->walk_id);
    TEST_ASSERT_EQUAL_UINT32(e->walk_seq, g->walk_seq);
    TEST_ASSERT_EQUAL_UINT16(e->walk_tested, g->walk_tested);
    TEST_ASSERT_EQUAL_UINT16(e->walk_total, g->walk_total);
    TEST_ASSERT_EQUAL_UINT8(e->walk_flags, g->walk_flags);
    TEST_ASSERT_EQUAL_UINT8(e->walk_started_by, g->walk_started_by);
    TEST_ASSERT_EQUAL_UINT8(e->walk_ended_by, g->walk_ended_by);
    TEST_ASSERT_EQUAL_UINT8(e->walk_end_how, g->walk_end_how);
    TEST_ASSERT_EQUAL_UINT8(e->walk_src_kind, g->walk_src_kind);
    TEST_ASSERT_EQUAL_UINT16(e->walk_src_id, g->walk_src_id);
}

static void expect_system(const plink_status_t *e, const plink_status_t *g)
{
    TEST_ASSERT_EQUAL_INT64(e->epoch_ms, g->epoch_ms);
    TEST_ASSERT_EQUAL_HEX8(e->partitions, g->partitions);
    TEST_ASSERT_EQUAL_UINT8(e->home, g->home);
    TEST_ASSERT_EQUAL_HEX32(e->sys_troubles, g->sys_troubles);
    TEST_ASSERT_EQUAL_HEX32(e->sys_restored, g->sys_restored);
    TEST_ASSERT_EQUAL_HEX32(e->dev_troubles, g->dev_troubles);
    TEST_ASSERT_EQUAL_UINT8(e->default_codes, g->default_codes);
    TEST_ASSERT_EQUAL_HEX8(e->flags, g->flags);
    TEST_ASSERT_EQUAL_UINT32(e->recovery_left_ms, g->recovery_left_ms);
    TEST_ASSERT_EQUAL_UINT32(e->lockout_left_ms, g->lockout_left_ms);
    TEST_ASSERT_EQUAL_UINT8(e->report, g->report);
    TEST_ASSERT_EQUAL_UINT16(e->zones_used, g->zones_used);
    TEST_ASSERT_EQUAL_UINT32(e->last_event_seq, g->last_event_seq);
    TEST_ASSERT_EQUAL_HEX32(e->cfg_crc, g->cfg_crc);
    TEST_ASSERT_EQUAL_UINT32(e->cfg_len, g->cfg_len);
    TEST_ASSERT_EQUAL_HEX32(e->zones_crc, g->zones_crc);
    TEST_ASSERT_EQUAL_UINT64(e->mono_ms, g->mono_ms);
}

static void expect_status(const plink_status_t *e, const plink_status_t *g)
{
    expect_system(e, g);
    TEST_ASSERT_EQUAL_UINT8(e->count, g->count);
    for (size_t i = 0; i < e->count; i++) {
        expect_part(&e->part[i], &g->part[i]);
    }
}

/* LINK-017: STATUS: the system section, then one record per enabled partition, each its number
 * and its length (docs/features/11 "STATUS"). */
static void test_golden_status(void)
{
    static plink_status_t st, st2;
    golden_status(&st);
    size_t n = 0;
    uint8_t written = 0;
    TEST_ASSERT_TRUE(plink_enc_status(&st, 0, OUT, sizeof(OUT), &n, &written));
    TEST_ASSERT_EQUAL_UINT8(2, written);
    expect_golden("STATUS", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_status(G_STATUS, sizeof(G_STATUS), &st2));
    expect_status(&st, &st2);
}

/* KPD-006: the appended selection preserves home and legacy STATUS bytes. */
static void test_status_temporary_selection_fixture(void)
{
    plink_status_t st;
    TEST_ASSERT_TRUE(plink_dec_status(STATUS_GOTO_V1, sizeof STATUS_GOTO_V1, &st));
    TEST_ASSERT_EQUAL_UINT8(1, st.home);
    TEST_ASSERT_EQUAL_UINT8(3, st.selected);
    size_t n = 0;
    uint8_t written = 0;
    TEST_ASSERT_TRUE(plink_enc_status(&st, 0, OUT, sizeof OUT, &n, &written));
    TEST_ASSERT_EQUAL_UINT(sizeof STATUS_GOTO_V1, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(STATUS_GOTO_V1, OUT, n);
    TEST_ASSERT_TRUE(plink_dec_status(G_STATUS, sizeof G_STATUS, &st));
    TEST_ASSERT_EQUAL_UINT8(0, st.selected);
    TEST_ASSERT_FALSE(st.switch_allowed);
    TEST_ASSERT_TRUE(plink_dec_status(STATUS_POLICY_V1, sizeof STATUS_POLICY_V1, &st));
    TEST_ASSERT_EQUAL_UINT8(3, st.selected);
    TEST_ASSERT_TRUE(st.switch_allowed);
    TEST_ASSERT_TRUE(st.return_never);
    TEST_ASSERT_EQUAL_UINT16(30, st.return_s);
    TEST_ASSERT_TRUE(plink_enc_status(&st, 0, OUT, sizeof OUT, &n, &written));
    TEST_ASSERT_EQUAL_UINT(sizeof STATUS_POLICY_V1, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(STATUS_POLICY_V1, OUT, n);
}

/* LINK-017, LINK-005: ZONES, ZONES_REQ, CFG_REQ, CFG_PAGE, EV_REQ and EVENTS as in
 * docs/features/11 "Messages", "Zone record", "Event record". */
static void test_golden_bulk(void)
{
    size_t n = 0;
    const alarm_zone_status_t z[2] = {
        {.flags = ALARM_ZS_FAULT | ALARM_ZS_NOT_READY, .trouble = 1, .walk = 5,
         .walk_first_s = 300, .walk_trips = 2},
        {.flags = ALARM_ZS_BYPASSED},
    };
    uint16_t zw = 0;
    TEST_ASSERT_TRUE(plink_enc_zones(0x0BADF00Du, 5, z, 2, OUT, sizeof(OUT), &n, &zw));
    TEST_ASSERT_EQUAL_UINT16(2, zw);
    expect_golden("ZONES", OUT, n);
    plink_zones_t zm;
    TEST_ASSERT_TRUE(plink_dec_zones(G_ZONES, sizeof(G_ZONES), &zm));
    TEST_ASSERT_EQUAL_HEX32(0x0BADF00Du, zm.zones_crc);
    TEST_ASSERT_EQUAL_UINT16(5, zm.first);
    TEST_ASSERT_EQUAL_UINT16(2, zm.count);
    TEST_ASSERT_EQUAL_UINT8(PLINK_ZONE_REC_LEN, zm.rec_len);
    TEST_ASSERT_EQUAL_PTR(G_ZONES + 9, zm.recs); /* records stay in the payload */

    const plink_zones_req_t zr = {.first = 1, .count = 128};
    TEST_ASSERT_TRUE(plink_enc_zones_req(&zr, OUT, sizeof(OUT), &n));
    expect_golden("ZONES_REQ", OUT, n);
    plink_zones_req_t zr2;
    TEST_ASSERT_TRUE(plink_dec_zones_req(G_ZONES_REQ, sizeof(G_ZONES_REQ), &zr2));
    TEST_ASSERT_EQUAL_UINT16(1, zr2.first);
    TEST_ASSERT_EQUAL_UINT16(128, zr2.count);

    const plink_cfg_req_t cr = {.cfg_crc = 0xCAFEBABEu, .offset = 1024, .max = 1024};
    TEST_ASSERT_TRUE(plink_enc_cfg_req(&cr, OUT, sizeof(OUT), &n));
    expect_golden("CFG_REQ", OUT, n);
    plink_cfg_req_t cr2;
    TEST_ASSERT_TRUE(plink_dec_cfg_req(G_CFG_REQ, sizeof(G_CFG_REQ), &cr2));
    TEST_ASSERT_EQUAL_HEX32(0xCAFEBABEu, cr2.cfg_crc);
    TEST_ASSERT_EQUAL_UINT32(1024, cr2.offset);
    TEST_ASSERT_EQUAL_UINT16(1024, cr2.max);

    static const uint8_t blob[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0xDE, 0xAD, 0xBE, 0xEF};
    const plink_cfg_page_t pg = {
        .cfg_crc = 0xCAFEBABEu, .total = 16, .offset = 12, .data = blob + 12, .len = 4};
    TEST_ASSERT_TRUE(plink_enc_cfg_page(&pg, OUT, sizeof(OUT), &n));
    expect_golden("CFG_PAGE", OUT, n);
    TEST_ASSERT_EQUAL_size_t(PLINK_CFG_PAGE_HDR + 4u, n);
    plink_cfg_page_t pg2;
    TEST_ASSERT_TRUE(plink_dec_cfg_page(G_CFG_PAGE, sizeof(G_CFG_PAGE), &pg2));
    TEST_ASSERT_EQUAL_HEX32(0xCAFEBABEu, pg2.cfg_crc);
    TEST_ASSERT_EQUAL_UINT32(16, pg2.total);
    TEST_ASSERT_EQUAL_UINT32(12, pg2.offset);
    TEST_ASSERT_EQUAL_UINT16(4, pg2.len);
    TEST_ASSERT_EQUAL_PTR(G_CFG_PAGE + PLINK_CFG_PAGE_HDR, pg2.data);

    const plink_ev_req_t er = {.after_seq = 1234, .max = 50};
    TEST_ASSERT_TRUE(plink_enc_ev_req(&er, OUT, sizeof(OUT), &n));
    expect_golden("EV_REQ", OUT, n);
    plink_ev_req_t er2;
    TEST_ASSERT_TRUE(plink_dec_ev_req(G_EV_REQ, sizeof(G_EV_REQ), &er2));
    TEST_ASSERT_EQUAL_UINT32(1234, er2.after_seq);
    TEST_ASSERT_EQUAL_UINT8(50, er2.max);

    alarm_event_t ev[2];
    memset(ev, 0, sizeof(ev));
    ev[0] = (alarm_event_t){.seq = 1233, .mono_ms = 123456, .epoch_ms = 0x1122334455667788ll,
                            .type = ALARM_EV_ARMED, .partition = 1, .user = 5,
                            .source = {ALARM_SRC_KEYPAD, 3}, .arg = ALARM_LEVEL_AWAY,
                            .arg2 = ALARM_ARM_SILENT_EXIT, .flags = ALARM_EVF_REPORT};
    ev[1] = (alarm_event_t){.seq = 1234, .mono_ms = 123556, .epoch_ms = -1,
                            .type = ALARM_EV_ENTRY_DELAY, .partition = 1, .zone = 7,
                            .source = {ALARM_SRC_ZONE, 7}};
    size_t ew = 0;
    TEST_ASSERT_TRUE(plink_enc_events(ev, 2, 0, OUT, sizeof(OUT), &n, &ew));
    TEST_ASSERT_EQUAL_size_t(2, ew);
    expect_golden("EVENTS", OUT, n);
    alarm_event_t got[4];
    size_t count = 0;
    uint8_t flags = 0xFF;
    TEST_ASSERT_TRUE(plink_dec_events(G_EVENTS, sizeof(G_EVENTS), got, 4, &count, &flags));
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_HEX8(0, flags);
    for (size_t i = 0; i < 2u; i++) {
        TEST_ASSERT_EQUAL_UINT32(ev[i].seq, got[i].seq);
        TEST_ASSERT_EQUAL_UINT64(ev[i].mono_ms, got[i].mono_ms);
        TEST_ASSERT_EQUAL_INT64(ev[i].epoch_ms, got[i].epoch_ms);
        TEST_ASSERT_EQUAL_INT(ev[i].type, got[i].type);
        TEST_ASSERT_EQUAL_UINT8(ev[i].partition, got[i].partition);
        TEST_ASSERT_EQUAL_UINT16(ev[i].zone, got[i].zone);
        TEST_ASSERT_EQUAL_UINT8(ev[i].user, got[i].user);
        TEST_ASSERT_EQUAL_UINT8(ev[i].source.kind, got[i].source.kind);
        TEST_ASSERT_EQUAL_UINT16(ev[i].source.id, got[i].source.id);
        TEST_ASSERT_EQUAL_UINT8(ev[i].arg, got[i].arg);
        TEST_ASSERT_EQUAL_UINT8(ev[i].arg2, got[i].arg2);
        TEST_ASSERT_EQUAL_UINT8(ev[i].flags, got[i].flags);
    }
}

/* LINK-017, LINK-018: CMD and RESULT as in docs/features/11 "Messages", "Commands": the codes
 * travel zero-padded with their real length in front. */
static void test_golden_commands(void)
{
    size_t n = 0;
    plink_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.req = 0x00C0FFEEu;
    c.op = PLINK_OP_COMMAND;
    c.cmd.type = ALARM_CMD_ARM;
    c.cmd.partition = 2;
    c.cmd.level = ALARM_LEVEL_STAY;
    c.cmd.arm_flags = ALARM_ARM_NO_ENTRY_DELAY;
    strcpy(c.cmd.code, "1234");
    TEST_ASSERT_TRUE(plink_enc_cmd(&c, OUT, sizeof(OUT), &n));
    expect_golden("CMD", OUT, n);
    plink_cmd_t c2;
    TEST_ASSERT_TRUE(plink_dec_cmd(G_CMD, sizeof(G_CMD), &c2));
    TEST_ASSERT_EQUAL_HEX32(0x00C0FFEEu, c2.req);
    TEST_ASSERT_EQUAL_UINT8(PLINK_OP_COMMAND, c2.op);
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_ARM, c2.cmd.type);
    TEST_ASSERT_EQUAL_UINT8(2, c2.cmd.partition);
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_STAY, c2.cmd.level);
    TEST_ASSERT_EQUAL_HEX8(ALARM_ARM_NO_ENTRY_DELAY, c2.cmd.arm_flags);
    TEST_ASSERT_EQUAL_UINT16(0, c2.cmd.zone);
    TEST_ASSERT_EQUAL_INT(ALARM_PANIC_POLICE, c2.cmd.panic);
    TEST_ASSERT_EQUAL_STRING("1234", c2.cmd.code);
    TEST_ASSERT_EQUAL_STRING("", c2.new_code);

    memset(&c, 0, sizeof(c));
    c.req = 42;
    c.op = PLINK_OP_CHANGE_CODE;
    c.cmd.partition = 1;
    strcpy(c.cmd.code, "123456");
    strcpy(c.new_code, "9876543210");
    TEST_ASSERT_TRUE(plink_enc_cmd(&c, OUT, sizeof(OUT), &n));
    expect_golden("CMD change", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_cmd(G_CMD_CHANGE, sizeof(G_CMD_CHANGE), &c2));
    TEST_ASSERT_EQUAL_UINT8(PLINK_OP_CHANGE_CODE, c2.op);
    TEST_ASSERT_EQUAL_UINT8(1, c2.cmd.partition);
    TEST_ASSERT_EQUAL_STRING("123456", c2.cmd.code);
    TEST_ASSERT_EQUAL_STRING("9876543210", c2.new_code);

    /* Every other op section field, through a walk test and a panic. */
    memset(&c, 0, sizeof(c));
    c.op = PLINK_OP_COMMAND;
    c.cmd.type = ALARM_CMD_WALK_TEST;
    c.cmd.partition = 4;
    c.cmd.zone = 0x0123;
    c.cmd.panic = ALARM_PANIC_MEDICAL;
    c.cmd.walk_op = ALARM_WALK_EXTEND;
    c.cmd.walk_flags = ALARM_WALK_F_LIFE24;
    TEST_ASSERT_TRUE(plink_enc_cmd(&c, OUT, sizeof(OUT), &n));
    static const uint8_t op_section[] = {0x09, 0x09, 0x04, 0x00, 0x00,
                                         0x23, 0x01, 0x03, 0x03, 0x01};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(op_section, OUT + 5, sizeof(op_section));
    TEST_ASSERT_TRUE(plink_dec_cmd(OUT, n, &c2));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_WALK_TEST, c2.cmd.type);
    TEST_ASSERT_EQUAL_UINT16(0x0123, c2.cmd.zone);
    TEST_ASSERT_EQUAL_INT(ALARM_PANIC_MEDICAL, c2.cmd.panic);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_EXTEND, c2.cmd.walk_op);
    TEST_ASSERT_EQUAL_UINT8(ALARM_WALK_F_LIFE24, c2.cmd.walk_flags);

    const plink_result_t res = {.req = 0x00C0FFEEu, .result = ALARM_ERR_NOT_READY, .user = 5,
                                .not_ready_zone = 258, .may_disarm = 1};
    TEST_ASSERT_TRUE(plink_enc_result(&res, OUT, sizeof(OUT), &n));
    expect_golden("RESULT", OUT, n);
    plink_result_t res2;
    TEST_ASSERT_TRUE(plink_dec_result(G_RESULT, sizeof(G_RESULT), &res2));
    TEST_ASSERT_EQUAL_HEX32(0x00C0FFEEu, res2.req);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ERR_NOT_READY, res2.result);
    TEST_ASSERT_EQUAL_UINT8(5, res2.user);
    TEST_ASSERT_EQUAL_UINT16(258, res2.not_ready_zone);
    TEST_ASSERT_EQUAL_UINT8(1, res2.may_disarm);
}

/* LINK-017: SOUND, HEARTBEAT, TIME and BYE as in docs/features/11 "Messages". */
static void test_golden_others(void)
{
    size_t n = 0;
    const plink_sound_t snd = {.partition = 2, .sound = ALARM_SOUND_CHIME, .zone = 10};
    TEST_ASSERT_TRUE(plink_enc_sound(&snd, OUT, sizeof(OUT), &n));
    expect_golden("SOUND", OUT, n);
    plink_sound_t snd2;
    TEST_ASSERT_TRUE(plink_dec_sound(G_SOUND, sizeof(G_SOUND), &snd2));
    TEST_ASSERT_EQUAL_UINT8(2, snd2.partition);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SOUND_CHIME, snd2.sound);
    TEST_ASSERT_EQUAL_UINT16(10, snd2.zone);

    plink_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));
    hb.flags = PLINK_HB_MAINS_LOST | PLINK_HB_BATTERY_THERE;
    hb.rssi = -61;
    hb.uptime_s = 86400;
    hb.status_age_s = PLINK_STATUS_AGE_NONE;
    strcpy(hb.fw, "0.9.1");
    TEST_ASSERT_TRUE(plink_enc_heartbeat(&hb, OUT, sizeof(OUT), &n));
    expect_golden("HEARTBEAT", OUT, n);
    plink_heartbeat_t hb2;
    TEST_ASSERT_TRUE(plink_dec_heartbeat(G_HEARTBEAT, sizeof(G_HEARTBEAT), &hb2));
    TEST_ASSERT_EQUAL_HEX8(0x05, hb2.flags);
    TEST_ASSERT_EQUAL_INT8(-61, hb2.rssi);
    TEST_ASSERT_EQUAL_UINT32(86400, hb2.uptime_s);
    TEST_ASSERT_EQUAL_HEX16(PLINK_STATUS_AGE_NONE, hb2.status_age_s);
    TEST_ASSERT_EQUAL_STRING("0.9.1", hb2.fw);
    const int8_t rssi[] = {-128, -1, 0, 127};
    for (size_t i = 0; i < sizeof(rssi); i++) {
        hb.rssi = rssi[i];
        TEST_ASSERT_TRUE(plink_enc_heartbeat(&hb, OUT, sizeof(OUT), &n));
        TEST_ASSERT_TRUE(plink_dec_heartbeat(OUT, n, &hb2));
        TEST_ASSERT_EQUAL_INT8(rssi[i], hb2.rssi);
    }

    plink_time_t t;
    memset(&t, 0, sizeof(t));
    t.epoch_ms = 1760000000000ll;
    strcpy(t.tz, "EST5EDT,M3.2.0,M11.1.0");
    TEST_ASSERT_TRUE(plink_enc_time(&t, OUT, sizeof(OUT), &n));
    expect_golden("TIME", OUT, n);
    plink_time_t t2;
    TEST_ASSERT_TRUE(plink_dec_time(G_TIME, sizeof(G_TIME), &t2));
    TEST_ASSERT_EQUAL_INT64(1760000000000ll, t2.epoch_ms);
    TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", t2.tz);

    const plink_bye_t bye = {.reason = 1};
    TEST_ASSERT_TRUE(plink_enc_bye(&bye, OUT, sizeof(OUT), &n));
    expect_golden("BYE", OUT, n);
    plink_bye_t bye2;
    TEST_ASSERT_TRUE(plink_dec_bye(G_BYE, sizeof(G_BYE), &bye2));
    TEST_ASSERT_EQUAL_UINT8(1, bye2.reason);
}

/* LINK-017: decoding every golden payload and encoding it again gives the same bytes, and an
 * encoder refuses a buffer one byte too small. */
static void test_golden_round_trip(void)
{
    for (size_t i = 0; i < CODEC_COUNT; i++) {
        const codec_t *c = &CODECS[i];
        if (c->enc == NULL) {
            continue;
        }
        expect_canonical(c, c->golden, c->golden_len, c->golden, c->golden_len);
        if (c->dec == d_events) {
            continue; /* a smaller EVENTS keeps fewer events (test_events_paging) */
        }
        size_t n = 99;
        TEST_ASSERT_FALSE_MESSAGE(c->enc(&M1, OUT, c->golden_len - 1u, &n), c->name);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(0, n, c->name);
    }
}

/* ---- random round trips ------------------------------------------------------------------- */

static void rnd_part(plink_part_t *p, uint8_t number)
{
    p->number = number;
    p->state = rnd8();
    p->level = rnd8();
    p->arm_flags = rnd8();
    p->armed_by = rnd8();
    p->arm_src_kind = rnd8();
    p->arm_src_id = rnd16();
    p->ready_levels = rnd8();
    p->flags = rnd8();
    p->sounder = rnd8();
    p->bell = rnd8();
    p->delay_left_ms = rnd32();
    p->memory = rnd16();
    p->faults = rnd16();
    p->bypassed = rnd16();
    p->alarms = rnd16();
    p->troubles = rnd16();
    p->walk_left_ms = rnd32();
    p->walk_age_ms = rnd32();
    p->walk_id = rnd32();
    p->walk_seq = rnd32();
    p->walk_tested = rnd16();
    p->walk_total = rnd16();
    p->walk_flags = rnd8();
    p->walk_started_by = rnd8();
    p->walk_ended_by = rnd8();
    p->walk_end_how = rnd8();
    p->walk_src_kind = rnd8();
    p->walk_src_id = rnd16();
}

/* LINK-017: random values of every field come back as they went, through STATUS, CMD, EVENTS,
 * the pairing and handshake messages. */
static void test_random_round_trips(void)
{
    RNG = 0x5EEDu;
    static plink_status_t st, st2;
    for (int iter = 0; iter < 2000; iter++) {
        memset(&st, 0, sizeof(st));
        st.epoch_ms = (int64_t)rnd();
        st.partitions = rnd8();
        st.home = rnd8();
        st.sys_troubles = rnd32();
        st.sys_restored = rnd32();
        st.dev_troubles = rnd32();
        st.default_codes = rnd8();
        st.flags = rnd8();
        st.recovery_left_ms = rnd32();
        st.lockout_left_ms = rnd32();
        st.report = rnd8();
        st.zones_used = rnd16();
        st.last_event_seq = rnd32();
        st.cfg_crc = rnd32();
        st.cfg_len = rnd32();
        st.zones_crc = rnd32();
        st.mono_ms = ((uint64_t)rnd32() << 32) | rnd32();
        /* Some of the partitions 1..8, in a random order. */
        uint8_t numbers[PLINK_PARTS_MAX];
        for (uint8_t i = 0; i < PLINK_PARTS_MAX; i++) {
            numbers[i] = (uint8_t)(i + 1u);
        }
        for (size_t i = PLINK_PARTS_MAX - 1u; i > 0u; i--) {
            const size_t j = rnd_below(i + 1u);
            const uint8_t t = numbers[i];
            numbers[i] = numbers[j];
            numbers[j] = t;
        }
        st.count = (uint8_t)rnd_below(PLINK_PARTS_MAX + 1u);
        for (uint8_t i = 0; i < st.count; i++) {
            rnd_part(&st.part[i], numbers[i]);
        }
        size_t n = 0;
        uint8_t written = 0;
        TEST_ASSERT_TRUE(plink_enc_status(&st, 0, OUT, sizeof(OUT), &n, &written));
        TEST_ASSERT_EQUAL_UINT8(st.count, written);
        TEST_ASSERT_EQUAL_size_t(60u + 54u * st.count, n);
        TEST_ASSERT_TRUE(plink_dec_status(OUT, n, &st2));
        expect_status(&st, &st2);
    }

    for (int iter = 0; iter < 2000; iter++) {
        plink_cmd_t c, c2;
        memset(&c, 0, sizeof(c));
        c.req = rnd32();
        c.op = rnd8();
        c.cmd.type = (alarm_cmd_type_t)rnd_below(ALARM_CMD_CODE_CHECK + 1u);
        c.cmd.partition = rnd8();
        c.cmd.level = (alarm_arm_level_t)rnd_below(ALARM_LEVEL_NIGHT + 1u);
        c.cmd.arm_flags = rnd8();
        c.cmd.zone = rnd16();
        c.cmd.panic = (alarm_panic_t)rnd_below(ALARM_PANIC_MEDICAL + 1u);
        c.cmd.walk_op = rnd8();
        c.cmd.walk_flags = rnd8();
        rnd_code(c.cmd.code);
        rnd_code(c.new_code);
        size_t n = 0;
        TEST_ASSERT_TRUE(plink_enc_cmd(&c, OUT, sizeof(OUT), &n));
        TEST_ASSERT_EQUAL_size_t(sizeof(G_CMD), n); /* one size whatever the codes */
        TEST_ASSERT_TRUE(plink_dec_cmd(OUT, n, &c2));
        TEST_ASSERT_EQUAL_UINT32(c.req, c2.req);
        TEST_ASSERT_EQUAL_UINT8(c.op, c2.op);
        TEST_ASSERT_EQUAL_INT(c.cmd.type, c2.cmd.type);
        TEST_ASSERT_EQUAL_UINT8(c.cmd.partition, c2.cmd.partition);
        TEST_ASSERT_EQUAL_INT(c.cmd.level, c2.cmd.level);
        TEST_ASSERT_EQUAL_UINT8(c.cmd.arm_flags, c2.cmd.arm_flags);
        TEST_ASSERT_EQUAL_UINT16(c.cmd.zone, c2.cmd.zone);
        TEST_ASSERT_EQUAL_INT(c.cmd.panic, c2.cmd.panic);
        TEST_ASSERT_EQUAL_UINT8(c.cmd.walk_op, c2.cmd.walk_op);
        TEST_ASSERT_EQUAL_UINT8(c.cmd.walk_flags, c2.cmd.walk_flags);
        TEST_ASSERT_EQUAL_STRING(c.cmd.code, c2.cmd.code);
        TEST_ASSERT_EQUAL_STRING(c.new_code, c2.new_code);
    }

    static alarm_event_t ev[36], got[36];
    for (int iter = 0; iter < 200; iter++) {
        const size_t count = rnd_below(37);
        for (size_t i = 0; i < count; i++) {
            memset(&ev[i], 0, sizeof(ev[i]));
            ev[i].seq = rnd32();
            ev[i].mono_ms = rnd();
            ev[i].epoch_ms = (int64_t)rnd();
            ev[i].type = (alarm_event_type_t)rnd_below(ALARM_EV_COUNT);
            ev[i].partition = rnd8();
            ev[i].zone = rnd16();
            ev[i].user = rnd8();
            ev[i].source.kind = rnd8();
            ev[i].source.id = rnd16();
            ev[i].arg = rnd8();
            ev[i].arg2 = rnd8();
            ev[i].flags = rnd8();
        }
        size_t n = 0, written = 0, back = 0;
        uint8_t flags = 0;
        TEST_ASSERT_TRUE(plink_enc_events(ev, count, 0, OUT, PLINK_PAYLOAD_MAX, &n, &written));
        TEST_ASSERT_EQUAL_size_t(count, written); /* 36 fit a UDP frame */
        TEST_ASSERT_TRUE(plink_dec_events(OUT, n, got, 36, &back, &flags));
        TEST_ASSERT_EQUAL_size_t(count, back);
        TEST_ASSERT_EQUAL_HEX8(0, flags);
        for (size_t i = 0; i < count; i++) {
            TEST_ASSERT_EQUAL_UINT32(ev[i].seq, got[i].seq);
            TEST_ASSERT_EQUAL_UINT64(ev[i].mono_ms, got[i].mono_ms);
            TEST_ASSERT_EQUAL_INT64(ev[i].epoch_ms, got[i].epoch_ms);
            TEST_ASSERT_EQUAL_INT(ev[i].type, got[i].type);
            TEST_ASSERT_EQUAL_UINT8(ev[i].partition, got[i].partition);
            TEST_ASSERT_EQUAL_UINT16(ev[i].zone, got[i].zone);
            TEST_ASSERT_EQUAL_UINT8(ev[i].user, got[i].user);
            TEST_ASSERT_EQUAL_UINT8(ev[i].source.kind, got[i].source.kind);
            TEST_ASSERT_EQUAL_UINT16(ev[i].source.id, got[i].source.id);
            TEST_ASSERT_EQUAL_UINT8(ev[i].arg, got[i].arg);
            TEST_ASSERT_EQUAL_UINT8(ev[i].arg2, got[i].arg2);
            TEST_ASSERT_EQUAL_UINT8(ev[i].flags, got[i].flags);
        }
    }

    for (int iter = 0; iter < 2000; iter++) {
        plink_pair_req_t req, req2;
        memset(&req, 0, sizeof(req));
        req.attempt = rnd32();
        req.proto = rnd8();
        req.caps = rnd16();
        req.kind = rnd8();
        req.cap = (plink_caps_t){rnd8(), rnd16(), rnd8(), rnd8()};
        rnd_bytes(req.pk, sizeof(req.pk));
        rnd_text(req.name, PLINK_NAME_MAX);
        rnd_text(req.fw, PLINK_FW_MAX);
        size_t n = 0;
        TEST_ASSERT_TRUE(plink_enc_pair_req(&req, OUT, sizeof(OUT), &n));
        TEST_ASSERT_TRUE(plink_dec_pair_req(OUT, n, &req2));
        TEST_ASSERT_EQUAL_UINT32(req.attempt, req2.attempt);
        TEST_ASSERT_EQUAL_UINT8(req.proto, req2.proto);
        TEST_ASSERT_EQUAL_UINT16(req.caps, req2.caps);
        TEST_ASSERT_EQUAL_UINT8(req.kind, req2.kind);
        TEST_ASSERT_EQUAL_UINT8(req.cap.partitions, req2.cap.partitions);
        TEST_ASSERT_EQUAL_UINT16(req.cap.zones, req2.cap.zones);
        TEST_ASSERT_EQUAL_UINT8(req.cap.users, req2.cap.users);
        TEST_ASSERT_EQUAL_UINT8(req.cap.devices, req2.cap.devices);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(req.pk, req2.pk, PLINK_X25519_LEN);
        TEST_ASSERT_EQUAL_STRING(req.name, req2.name);
        TEST_ASSERT_EQUAL_STRING(req.fw, req2.fw);

        plink_hello_t h, h2;
        memset(&h, 0, sizeof(h));
        h.proto = rnd8();
        h.caps = rnd16();
        h.cap = (plink_caps_t){rnd8(), rnd16(), rnd8(), rnd8()};
        rnd_bytes(h.n, sizeof(h.n));
        rnd_bytes(h.e, sizeof(h.e));
        rnd_bytes(h.mac, sizeof(h.mac));
        TEST_ASSERT_TRUE(plink_enc_hello(&h, true, OUT, sizeof(OUT), &n));
        TEST_ASSERT_TRUE(plink_dec_hello(OUT, n, true, &h2));
        TEST_ASSERT_EQUAL_UINT8(h.proto, h2.proto);
        TEST_ASSERT_EQUAL_UINT16(h.caps, h2.caps);
        TEST_ASSERT_EQUAL_UINT16(h.cap.zones, h2.cap.zones);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(h.n, h2.n, PLINK_NONCE_LEN);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(h.e, h2.e, PLINK_X25519_LEN);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(h.mac, h2.mac, PLINK_MAC_LEN);
    }
}

/* ---- version skew (LINK-017) -------------------------------------------------------------- */

/* LINK-017: a newer firmware's payload - longer, with fields appended to the end, to a section, to
 * a record, or before the MAC - reads as the same values here. */
static void test_appended_bytes_skipped(void)
{
    static const uint8_t extra[16] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,
                                      0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0};
    uint8_t buf[PLINK_FRAME_MAX];
    size_t n;

    /* Bytes after the whole payload; before the MAC for the messages that end in one. */
    for (size_t i = 0; i < CODEC_COUNT; i++) {
        const codec_t *c = &CODECS[i];
        if (c->enc == NULL || c->dec == d_status) {
            continue; /* STATUS: records run to the end (below); ZONES below */
        }
        if (c->golden == G_HEARTBEAT || c->golden == G_DEV_ACK) {
            continue; /* bytes after them are their optional section (test_heartbeat_section) */
        }
        const bool mac_last = c->dec == d_pair_confirm || c->dec == d_pair_done ||
                              c->dec == d_hello || c->dec == d_welcome;
        const size_t at = mac_last ? c->golden_len - PLINK_MAC_LEN : c->golden_len;
        for (size_t k = 1; k <= sizeof(extra); k += 5) {
            n = splice(buf, c->golden, c->golden_len, at, extra, k);
            expect_canonical(c, buf, n, c->golden, c->golden_len);
        }
    }

    /* The capacities section (PAIR_REQ at 8, HELLO at 3) grows by 3 bytes. */
    n = splice(buf, G_PAIR_REQ, sizeof(G_PAIR_REQ), 14, extra, 3);
    buf[8] = 8;
    expect_canonical(codec("PAIR_REQ"), buf, n, G_PAIR_REQ, sizeof(G_PAIR_REQ));
    n = splice(buf, G_HELLO, sizeof(G_HELLO), 9, extra, 3);
    buf[3] = 8;
    expect_canonical(codec("HELLO"), buf, n, G_HELLO, sizeof(G_HELLO));

    /* STATUS: the system section, then each partition record, grow; a record of a number this
     * firmware doesn't hold (0) is skipped, and so is a second record of the same partition. */
    n = splice(buf, G_STATUS, sizeof(G_STATUS), 60, extra, 5);
    buf[0] = 59 + 5;
    uint8_t buf2[PLINK_FRAME_MAX];
    size_t n2 = splice(buf2, buf, n, 60 + 5 + 54, extra, 7); /* the first record's end */
    buf2[60 + 5 + 1] = 52 + 7;
    n = splice(buf, buf2, n2, n2, extra, 16); /* the second record's end: 16 more */
    buf[60 + 5 + 54 + 7 + 1] = 52 + 16;
    static const uint8_t unknown[] = {0x00, 0x03, 0x11, 0x22, 0x33, /* partition 0 */
                                      0x01, 0x01, 0x07};            /* partition 1 again */
    n = splice(buf2, buf, n, n, unknown, sizeof(unknown));
    expect_canonical(codec("STATUS"), buf2, n, G_STATUS, sizeof(G_STATUS));

    /* CMD's op section grows by 2. */
    n = splice(buf, G_CMD, sizeof(G_CMD), 15, extra, 2);
    buf[5] = 9 + 2;
    expect_canonical(codec("CMD"), buf, n, G_CMD, sizeof(G_CMD));

    /* EVENTS records of 40 bytes: 8 more each. */
    n = splice(buf2, G_EVENTS, sizeof(G_EVENTS), 3 + 32, extra, 8);
    n = splice(buf, buf2, n, n, extra + 8, 8);
    buf[2] = 40;
    expect_canonical(codec("EVENTS"), buf, n, G_EVENTS, sizeof(G_EVENTS));

    /* ZONES records of 9 bytes and bytes after them: rec_len and the records as sent. */
    n = splice(buf2, G_ZONES, sizeof(G_ZONES), 9 + 7, extra, 2);
    n = splice(buf, buf2, n, n, extra, 2 + 4);
    buf[8] = 9;
    plink_zones_t z;
    TEST_ASSERT_TRUE(plink_dec_zones(buf, n, &z));
    TEST_ASSERT_EQUAL_UINT16(5, z.first);
    TEST_ASSERT_EQUAL_UINT16(2, z.count);
    TEST_ASSERT_EQUAL_UINT8(9, z.rec_len);
    TEST_ASSERT_EQUAL_PTR(buf + 9, z.recs);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(G_ZONES + 9, z.recs, 7);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(G_ZONES + 16, z.recs + 9, 7);
}

/* LINK-017: an older firmware's shorter section or record reads the fields it lacks as 0. */
static void test_short_sections_read_zero(void)
{
    uint8_t buf[PLINK_FRAME_MAX];
    size_t n;

    /* PAIR_REQ with only the partitions in its capacities section. */
    n = cut(buf, G_PAIR_REQ, sizeof(G_PAIR_REQ), 10, 4);
    buf[8] = 1;
    plink_pair_req_t req;
    TEST_ASSERT_TRUE(plink_dec_pair_req(buf, n, &req));
    TEST_ASSERT_EQUAL_UINT8(4, req.cap.partitions);
    TEST_ASSERT_EQUAL_UINT16(0, req.cap.zones);
    TEST_ASSERT_EQUAL_UINT8(0, req.cap.users);
    TEST_ASSERT_EQUAL_UINT8(0, req.cap.devices);
    TEST_ASSERT_EQUAL_STRING("Hall", req.name);
    TEST_ASSERT_EQUAL_STRING("1.2.3", req.fw);
    /* ... and an empty one. */
    n = cut(buf, G_PAIR_REQ, sizeof(G_PAIR_REQ), 9, 5);
    buf[8] = 0;
    TEST_ASSERT_TRUE(plink_dec_pair_req(buf, n, &req));
    TEST_ASSERT_EQUAL_UINT8(0, req.cap.partitions);
    TEST_ASSERT_EQUAL_HEX8(0x00, req.pk[0]);
    TEST_ASSERT_EQUAL_HEX8(0x1F, req.pk[31]);
    TEST_ASSERT_EQUAL_STRING("Hall", req.name);

    /* STATUS with only epoch_ms in its system section, a first record with only its first four
     * fields and an empty second one. */
    n = cut(buf, G_STATUS, sizeof(G_STATUS), 9, 51);
    buf[0] = 8;
    uint8_t buf2[PLINK_FRAME_MAX];
    size_t n2 = cut(buf2, buf, n, 9 + 2 + 4, 48);
    buf2[10] = 4;
    n = cut(buf, buf2, n2, n2 - 52, 52);
    buf[n - 1] = 0;
    static plink_status_t st, want;
    TEST_ASSERT_TRUE(plink_dec_status(buf, n, &st));
    memset(&want, 0, sizeof(want));
    want.epoch_ms = 0x0102030405060708ll;
    want.count = 2;
    want.part[0] = (plink_part_t){.number = 1, .state = 3, .level = 1, .arm_flags = 2,
                                  .armed_by = 5};
    want.part[1] = (plink_part_t){.number = 3};
    expect_status(&want, &st);

    /* CMD with only the type and the partition in its op section. */
    n = cut(buf, G_CMD, sizeof(G_CMD), 8, 7);
    buf[5] = 2;
    plink_cmd_t c;
    TEST_ASSERT_TRUE(plink_dec_cmd(buf, n, &c));
    TEST_ASSERT_EQUAL_INT(ALARM_CMD_ARM, c.cmd.type);
    TEST_ASSERT_EQUAL_UINT8(2, c.cmd.partition);
    TEST_ASSERT_EQUAL_INT(ALARM_LEVEL_NONE, c.cmd.level);
    TEST_ASSERT_EQUAL_HEX8(0, c.cmd.arm_flags);
    TEST_ASSERT_EQUAL_STRING("1234", c.cmd.code);

    /* EVENTS records of 12 bytes: seq and mono_ms only. */
    static const uint8_t short_ev[] = {
        0x00, 0x02, 0x0C,
        0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    alarm_event_t ev[2];
    size_t count = 0;
    uint8_t flags = 0;
    TEST_ASSERT_TRUE(plink_dec_events(short_ev, sizeof(short_ev), ev, 2, &count, &flags));
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_UINT32(2, ev[1].seq);
    TEST_ASSERT_EQUAL_UINT64(0x20, ev[1].mono_ms);
    TEST_ASSERT_EQUAL_INT64(0, ev[1].epoch_ms);
    TEST_ASSERT_EQUAL_INT(0, ev[1].type);
    TEST_ASSERT_EQUAL_UINT16(0, ev[1].source.id);
}

/* LINK-017: a payload shorter than v1's fixed part is refused, whatever byte it stops at; a
 * section or a string that claims more than the payload holds too. */
static void test_truncated_refused(void)
{
    for (size_t i = 0; i < CODEC_COUNT; i++) {
        const codec_t *c = &CODECS[i];
        for (size_t len = 0; len < c->golden_len; len++) {
            const bool ok = c->dec(c->golden, len, &M1);
            /* STATUS ends after any whole record: the system section alone, or with record 1;
             * HEARTBEAT and DEV_ACK without their optional section are v1's whole payloads. */
            const bool whole = (c->dec == d_status && (len == 60u || len == 60u + 54u)) ||
                               (c->golden == G_HEARTBEAT_PREFS && len == sizeof(G_HEARTBEAT)) ||
                               (c->golden == G_DEV_ACK_REC && len == sizeof(G_DEV_ACK));
            TEST_ASSERT_EQUAL_MESSAGE(whole, ok, c->name);
        }
        TEST_ASSERT_FALSE_MESSAGE(c->dec(NULL, 0, &M1), c->name);
    }
    plink_status_t *st = &M1.status;
    TEST_ASSERT_TRUE(plink_dec_status(G_STATUS, 60 + 54, st));
    TEST_ASSERT_EQUAL_UINT8(1, st->count);
    TEST_ASSERT_EQUAL_UINT8(1, st->part[0].number);

    uint8_t buf[PLINK_FRAME_MAX];
    memcpy(buf, G_PAIR_REQ, sizeof(G_PAIR_REQ));
    buf[8] = 0xFF; /* the capacities section */
    TEST_ASSERT_FALSE(plink_dec_pair_req(buf, sizeof(G_PAIR_REQ), &M1.pair_req));
    memcpy(buf, G_PAIR_REQ, sizeof(G_PAIR_REQ));
    buf[sizeof(G_PAIR_REQ) - 6] = 6; /* fw "1.2.3" says 6 bytes */
    TEST_ASSERT_FALSE(plink_dec_pair_req(buf, sizeof(G_PAIR_REQ), &M1.pair_req));
    memcpy(buf, G_STATUS, sizeof(G_STATUS));
    buf[60 + 54 + 1] = 53; /* the last record one byte longer than the payload */
    TEST_ASSERT_FALSE(plink_dec_status(buf, sizeof(G_STATUS), &M1.status));
    memcpy(buf, G_ZONES, sizeof(G_ZONES));
    buf[6] = 3; /* three records, two sent */
    TEST_ASSERT_FALSE(plink_dec_zones(buf, sizeof(G_ZONES), &M1.zones));
    memcpy(buf, G_EVENTS, sizeof(G_EVENTS));
    buf[2] = 33; /* records one byte longer each */
    TEST_ASSERT_FALSE(plink_dec_events(buf, sizeof(G_EVENTS), M1.events.ev, EVENTS_MAX,
                                       &M1.events.n, &M1.events.flags));
    memcpy(buf, G_CFG_PAGE, sizeof(G_CFG_PAGE));
    buf[12] = 5; /* five bytes, four sent */
    TEST_ASSERT_FALSE(plink_dec_cfg_page(buf, sizeof(G_CFG_PAGE), &M1.cfg_page));
    /* A refused decode leaves nothing behind. */
    plink_cmd_t *c = &M1.cmd;
    memcpy(buf, G_CMD, sizeof(G_CMD));
    TEST_ASSERT_FALSE(plink_dec_cmd(buf, sizeof(G_CMD) - 1u, c));
    TEST_ASSERT_EQUAL_STRING("", c->cmd.code);
    TEST_ASSERT_EQUAL_UINT32(0, c->req);
}

/* ---- names, codes and the digest ---------------------------------------------------------- */

/* LINK-004 (docs/features/11 "Messages"): every name from the network - the keypad's and the
 * master's in pairing, a firmware version, a time zone - comes out cleaned and fits its buffer. */
static void test_names_cleaned(void)
{
    uint8_t buf[PLINK_FRAME_MAX];
    /* "Kit" RLO "chen" ESC NUL DEL CSI, an invalid byte, an overlong '/', a surrogate, é and a
     * bell. */
    static const uint8_t name[] = {0x4B, 0x69, 0x74, 0xE2, 0x80, 0xAE, 0x63, 0x68, 0x65, 0x6E,
                                   0x1B, 0x00, 0x7F, 0xC2, 0x9B, 0xFF, 0xC0, 0xAF, 0xED, 0xA0,
                                   0x80, 0xC3, 0xA9, 0xF0, 0x9F, 0x94, 0x94};
    size_t n = cut(buf, G_PAIR_REQ, sizeof(G_PAIR_REQ), 46, 5); /* without "Hall" */
    uint8_t buf2[PLINK_FRAME_MAX];
    uint8_t len_byte = (uint8_t)sizeof(name);
    n = splice(buf2, buf, n, 46, &len_byte, 1);
    n = splice(buf, buf2, n, 47, name, sizeof(name));
    plink_pair_req_t req;
    TEST_ASSERT_TRUE(plink_dec_pair_req(buf, n, &req));
    TEST_ASSERT_EQUAL_STRING("Kitchen\xC3\xA9\xF0\x9F\x94\x94", req.name);
    TEST_ASSERT_EQUAL_STRING("1.2.3", req.fw);

    /* A newer master's 60-byte name: cut to the 47 bytes a name holds, on a whole character. */
    uint8_t long_name[61];
    long_name[0] = 60;
    memset(long_name + 1, 'a', 46);
    long_name[47] = 0xC3; /* é straddles byte 47 */
    long_name[48] = 0xA9;
    memset(long_name + 49, 'b', 12);
    n = cut(buf, G_PAIR_COMMIT, sizeof(G_PAIR_COMMIT), 71, 13); /* without "Harbor House" */
    n = splice(buf2, buf, n, 71, long_name, sizeof(long_name));
    plink_pair_commit_t com;
    TEST_ASSERT_TRUE(plink_dec_pair_commit(buf2, n, &com));
    char expect[47];
    memset(expect, 'a', 46);
    expect[46] = '\0';
    TEST_ASSERT_EQUAL_STRING(expect, com.name);
    TEST_ASSERT_EQUAL_STRING("a1b2c3", com.panel_id);
    /* A panel id longer than six characters: its first six. */
    static const uint8_t id8[] = {0x08, 0x61, 0x31, 0x62, 0x32, 0x63, 0x33, 0x64, 0x34};
    n = cut(buf, G_PAIR_COMMIT, sizeof(G_PAIR_COMMIT), sizeof(G_PAIR_COMMIT) - 7u, 7);
    n = splice(buf2, buf, n, n, id8, sizeof(id8));
    TEST_ASSERT_TRUE(plink_dec_pair_commit(buf2, n, &com));
    TEST_ASSERT_EQUAL_STRING("a1b2c3", com.panel_id);

    /* A firmware version and a time zone with control characters. */
    static const uint8_t hb_fw[] = {0x06, 0x31, 0x0A, 0x2E, 0x32, 0x1B, 0x33};
    n = cut(buf, G_HEARTBEAT, sizeof(G_HEARTBEAT), 8, 6);
    n = splice(buf2, buf, n, 8, hb_fw, sizeof(hb_fw));
    plink_heartbeat_t hb;
    TEST_ASSERT_TRUE(plink_dec_heartbeat(buf2, n, &hb));
    TEST_ASSERT_EQUAL_STRING("1.23", hb.fw);
    static const uint8_t tz[] = {0x05, 0x55, 0xE2, 0x81, 0xA6, 0x43};
    n = cut(buf, G_TIME, sizeof(G_TIME), 8, 23);
    n = splice(buf2, buf, n, 8, tz, sizeof(tz));
    plink_time_t t;
    TEST_ASSERT_TRUE(plink_dec_time(buf2, n, &t));
    TEST_ASSERT_EQUAL_STRING("UC", t.tz);
}

/* LINK-018 (docs/features/11 "Commands"): codes are digits only - the encoder and the decoder
 * refuse others - and travel padded to one length whatever their own. */
static void test_cmd_codes(void)
{
    plink_cmd_t c;
    size_t n = 0;
    memset(&c, 0, sizeof(c));
    c.op = PLINK_OP_AUTH;
    c.cmd.partition = 1;
    static const char *const bad[] = {"12a4", "12 4", "-123", "1234\n", "\xD9\xA1\xD9\xA2"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        strcpy(c.cmd.code, bad[i]);
        TEST_ASSERT_FALSE(plink_enc_cmd(&c, OUT, sizeof(OUT), &n));
        TEST_ASSERT_EQUAL_size_t(0, n);
        c.cmd.code[0] = '\0';
        strcpy(c.new_code, bad[i]);
        TEST_ASSERT_FALSE(plink_enc_cmd(&c, OUT, sizeof(OUT), &n));
        c.new_code[0] = '\0';
    }
    memset(c.cmd.code, '1', sizeof(c.cmd.code)); /* eleven digits, no end */
    TEST_ASSERT_FALSE(plink_enc_cmd(&c, OUT, sizeof(OUT), &n));
    TEST_ASSERT_FALSE(plink_enc_cmd(NULL, OUT, sizeof(OUT), &n));

    /* Every code length gives the same payload size. */
    static const char *const good[] = {"", "1", "1234", "000000", "9999999999"};
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        strcpy(c.cmd.code, good[i]);
        strcpy(c.new_code, good[sizeof(good) / sizeof(good[0]) - 1u - i]);
        TEST_ASSERT_TRUE(plink_enc_cmd(&c, OUT, sizeof(OUT), &n));
        TEST_ASSERT_EQUAL_size_t(sizeof(G_CMD), n);
        plink_cmd_t c2;
        TEST_ASSERT_TRUE(plink_dec_cmd(OUT, n, &c2));
        TEST_ASSERT_EQUAL_STRING(c.cmd.code, c2.cmd.code);
        TEST_ASSERT_EQUAL_STRING(c.new_code, c2.new_code);
    }

    /* The decoder refuses a code that isn't digits, or longer than ALARM_CODE_MAX_LEN. */
    uint8_t buf[sizeof(G_CMD)];
    plink_cmd_t c3;
    memcpy(buf, G_CMD, sizeof(G_CMD));
    buf[17] = 'x'; /* "12x4" */
    TEST_ASSERT_FALSE(plink_dec_cmd(buf, sizeof(buf), &c3));
    TEST_ASSERT_EQUAL_STRING("", c3.cmd.code); /* nothing of it kept */
    memcpy(buf, G_CMD, sizeof(G_CMD));
    buf[17] = 0x00; /* a NUL inside the code's length */
    TEST_ASSERT_FALSE(plink_dec_cmd(buf, sizeof(buf), &c3));
    memcpy(buf, G_CMD, sizeof(G_CMD));
    buf[15] = 11;
    TEST_ASSERT_FALSE(plink_dec_cmd(buf, sizeof(buf), &c3));
    buf[15] = 0xFF;
    TEST_ASSERT_FALSE(plink_dec_cmd(buf, sizeof(buf), &c3));
    memcpy(buf, G_CMD, sizeof(G_CMD));
    buf[26] = 1; /* the new code: one byte, a 0x00 */
    TEST_ASSERT_FALSE(plink_dec_cmd(buf, sizeof(buf), &c3));
    buf[27] = '5';
    TEST_ASSERT_TRUE(plink_dec_cmd(buf, sizeof(buf), &c3));
    TEST_ASSERT_EQUAL_STRING("5", c3.new_code);
    /* The padding is not read: only the length says where a code ends. */
    memcpy(buf, G_CMD, sizeof(G_CMD));
    buf[20] = '7';
    buf[25] = 0xEE;
    TEST_ASSERT_TRUE(plink_dec_cmd(buf, sizeof(buf), &c3));
    TEST_ASSERT_EQUAL_STRING("1234", c3.cmd.code);
    /* An op this firmware doesn't know is decoded: the master answers it UNAVAILABLE. */
    memcpy(buf, G_CMD, sizeof(G_CMD));
    buf[4] = 0x7F;
    TEST_ASSERT_TRUE(plink_dec_cmd(buf, sizeof(buf), &c3));
    TEST_ASSERT_EQUAL_UINT8(0x7F, c3.op);
}

/* LINK-018: the result cache's digest covers op and the op section, never the codes nor the
 * request id (known answers from Python 'cryptography', vectors_plink_msg.py). */
static void test_cmd_digest(void)
{
    plink_cmd_t c;
    uint8_t d[PLINK_SHA256_LEN], d2[PLINK_SHA256_LEN];
    TEST_ASSERT_TRUE(plink_dec_cmd(G_CMD, sizeof(G_CMD), &c));
    plink_cmd_digest(&c, d);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DIGEST_ARM, d, PLINK_SHA256_LEN);
    TEST_ASSERT_TRUE(plink_dec_cmd(G_CMD_CHANGE, sizeof(G_CMD_CHANGE), &c));
    plink_cmd_digest(&c, d);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DIGEST_CHANGE, d, PLINK_SHA256_LEN);

    /* Other codes and another request id: the same digest. */
    TEST_ASSERT_TRUE(plink_dec_cmd(G_CMD, sizeof(G_CMD), &c));
    strcpy(c.cmd.code, "999999");
    strcpy(c.new_code, "4321");
    c.req = 77;
    plink_cmd_digest(&c, d);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DIGEST_ARM, d, PLINK_SHA256_LEN);

    /* Any field of the command: another digest. */
    for (int field = 0; field < 9; field++) {
        TEST_ASSERT_TRUE(plink_dec_cmd(G_CMD, sizeof(G_CMD), &c));
        switch (field) {
        case 0:
            c.op = PLINK_OP_AUTH;
            break;
        case 1:
            c.cmd.type = ALARM_CMD_DISARM;
            break;
        case 2:
            c.cmd.partition = 1;
            break;
        case 3:
            c.cmd.level = ALARM_LEVEL_AWAY;
            break;
        case 4:
            c.cmd.arm_flags = 0;
            break;
        case 5:
            c.cmd.zone = 1;
            break;
        case 6:
            c.cmd.panic = ALARM_PANIC_FIRE;
            break;
        case 7:
            c.cmd.walk_op = 1;
            break;
        default:
            c.cmd.walk_flags = 1;
            break;
        }
        plink_cmd_digest(&c, d2);
        TEST_ASSERT_FALSE(memcmp(DIGEST_ARM, d2, PLINK_SHA256_LEN) == 0);
    }
}

/* ---- paging to the transport's frame (LINK-005) ------------------------------------------- */

/* LINK-005: a transport whose frame is too small for every partition record gets several
 * STATUS frames, each with the system section and as many records as fit. */
static void test_status_split(void)
{
    static plink_status_t st, got;
    golden_status(&st);
    st.count = PLINK_PARTS_MAX;
    for (uint8_t i = 0; i < PLINK_PARTS_MAX; i++) {
        rnd_part(&st.part[i], (uint8_t)(PLINK_PARTS_MAX - i)); /* 8, 7, ... 1 */
    }
    /* ESP-NOW's 250 bytes hold the system section and three records. */
    const size_t espnow = PLINK_FRAME_ESPNOW - PLINK_SEALED_OVERHEAD;
    uint8_t first = 0;
    int frames = 0;
    while (first < st.count) {
        size_t n = 0;
        uint8_t written = 0;
        TEST_ASSERT_TRUE(plink_enc_status(&st, first, OUT, espnow, &n, &written));
        TEST_ASSERT_TRUE(n <= espnow);
        const unsigned left = (unsigned)st.count - (unsigned)first;
        TEST_ASSERT_EQUAL_UINT8(left < 3u ? left : 3u, written);
        TEST_ASSERT_TRUE(plink_dec_status(OUT, n, &got));
        expect_system(&st, &got);
        TEST_ASSERT_EQUAL_UINT8(written, got.count);
        for (uint8_t i = 0; i < written; i++) {
            expect_part(&st.part[first + i], &got.part[i]);
        }
        first = (uint8_t)(first + written);
        frames++;
    }
    TEST_ASSERT_EQUAL_INT(3, frames);

    /* One record per frame at the smallest cap that holds one; none fits below it. */
    size_t n = 0;
    uint8_t written = 9;
    TEST_ASSERT_TRUE(plink_enc_status(&st, 5, OUT, 60 + 54, &n, &written));
    TEST_ASSERT_EQUAL_UINT8(1, written);
    TEST_ASSERT_EQUAL_size_t(60 + 54, n);
    TEST_ASSERT_FALSE(plink_enc_status(&st, 5, OUT, 60 + 53, &n, &written));
    TEST_ASSERT_EQUAL_UINT8(0, written);
    TEST_ASSERT_EQUAL_size_t(0, n);
    /* Nothing left to send: the system section alone; past the records, refused. */
    TEST_ASSERT_TRUE(plink_enc_status(&st, st.count, OUT, 60, &n, &written));
    TEST_ASSERT_EQUAL_UINT8(0, written);
    TEST_ASSERT_EQUAL_size_t(60, n);
    TEST_ASSERT_FALSE(plink_enc_status(&st, st.count, OUT, 59, &n, &written));
    TEST_ASSERT_FALSE(plink_enc_status(&st, (uint8_t)(st.count + 1u), OUT, sizeof(OUT), &n,
                                       &written));
    st.count = PLINK_PARTS_MAX + 1u;
    TEST_ASSERT_FALSE(plink_enc_status(&st, 0, OUT, sizeof(OUT), &n, &written));
    /* A UDP frame holds every record at once. */
    st.count = PLINK_PARTS_MAX;
    TEST_ASSERT_TRUE(plink_enc_status(&st, 0, OUT, PLINK_PAYLOAD_MAX, &n, &written));
    TEST_ASSERT_EQUAL_UINT8(PLINK_PARTS_MAX, written);
}

/* LINK-005: EVENTS keeps the newest events that fit the frame and says older ones were left
 * out; a decoder with less room keeps the newest too. */
static void test_events_paging(void)
{
    static alarm_event_t ev[50], got[50];
    memset(ev, 0, sizeof(ev));
    for (size_t i = 0; i < 50u; i++) {
        ev[i].seq = (uint32_t)(1000u + i);
        ev[i].type = ALARM_EV_ALARM;
    }
    const size_t espnow = PLINK_FRAME_ESPNOW - PLINK_SEALED_OVERHEAD;
    size_t n = 0, written = 0, count = 0;
    uint8_t flags = 0;
    TEST_ASSERT_TRUE(plink_enc_events(ev, 50, 0, OUT, espnow, &n, &written));
    TEST_ASSERT_EQUAL_size_t((espnow - 3u) / PLINK_EVENT_REC_LEN, written); /* 6 */
    TEST_ASSERT_TRUE(n <= espnow);
    TEST_ASSERT_TRUE(plink_dec_events(OUT, n, got, 50, &count, &flags));
    TEST_ASSERT_EQUAL_size_t(written, count);
    TEST_ASSERT_EQUAL_HEX8(PLINK_EVF_GAP, flags);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT32(1000u + 50u - count + i, got[i].seq); /* the newest, in order */
    }

    /* A UDP frame: 36 of them. */
    TEST_ASSERT_TRUE(plink_enc_events(ev, 50, 0, OUT, PLINK_PAYLOAD_MAX, &n, &written));
    TEST_ASSERT_EQUAL_size_t(36, written);
    TEST_ASSERT_EQUAL_size_t(3u + 36u * PLINK_EVENT_REC_LEN, n);
    /* All of them fit: no gap, unless the master's log already left some out. */
    TEST_ASSERT_TRUE(plink_enc_events(ev, 20, 0, OUT, PLINK_PAYLOAD_MAX, &n, &written));
    TEST_ASSERT_EQUAL_size_t(20, written);
    TEST_ASSERT_EQUAL_HEX8(0, OUT[0]);
    TEST_ASSERT_TRUE(plink_enc_events(ev, 20, PLINK_EVF_GAP, OUT, PLINK_PAYLOAD_MAX, &n, &written));
    TEST_ASSERT_EQUAL_HEX8(PLINK_EVF_GAP, OUT[0]);
    /* Nothing to send: an empty answer; no room for one event: refused. */
    TEST_ASSERT_TRUE(plink_enc_events(NULL, 0, 0, OUT, 3, &n, &written));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_size_t(0, written);
    TEST_ASSERT_FALSE(plink_enc_events(ev, 1, 0, OUT, 3u + PLINK_EVENT_REC_LEN - 1u, &n, &written));
    TEST_ASSERT_EQUAL_size_t(0, written);
    TEST_ASSERT_FALSE(plink_enc_events(ev, 1, 0, OUT, 2, &n, &written));

    /* A decoder that keeps one: the newest, and the gap flag. */
    TEST_ASSERT_TRUE(plink_dec_events(G_EVENTS, sizeof(G_EVENTS), got, 1, &count, &flags));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_UINT32(1234, got[0].seq);
    TEST_ASSERT_EQUAL_HEX8(PLINK_EVF_GAP, flags);
    TEST_ASSERT_TRUE(plink_dec_events(G_EVENTS, sizeof(G_EVENTS), NULL, 0, &count, &flags));
    TEST_ASSERT_EQUAL_size_t(0, count);
}

/* LINK-005: ZONES carries as many records as fit the frame, and only zones that exist; a
 * configuration page stays inside its blob. */
static void test_zones_and_pages(void)
{
    static alarm_zone_status_t z[128];
    memset(z, 0, sizeof(z));
    for (size_t i = 0; i < 128u; i++) {
        z[i].flags = (uint16_t)i;
    }
    size_t n = 0;
    uint16_t written = 0;
    const size_t espnow = PLINK_FRAME_ESPNOW - PLINK_SEALED_OVERHEAD;
    TEST_ASSERT_TRUE(plink_enc_zones(1, 1, z, 128, OUT, espnow, &n, &written));
    TEST_ASSERT_EQUAL_UINT16((espnow - 9u) / PLINK_ZONE_REC_LEN, written); /* 30 */
    plink_zones_t zm;
    TEST_ASSERT_TRUE(plink_dec_zones(OUT, n, &zm));
    TEST_ASSERT_EQUAL_UINT16(written, zm.count);
    TEST_ASSERT_EQUAL_HEX8(29, zm.recs[29u * PLINK_ZONE_REC_LEN]); /* zone 30's flags */
    TEST_ASSERT_TRUE(plink_enc_zones(1, 1, z, 128, OUT, PLINK_PAYLOAD_MAX, &n, &written));
    TEST_ASSERT_EQUAL_UINT16(128, written);
    TEST_ASSERT_FALSE(plink_enc_zones(1, 1, z, 128, OUT, 9u + PLINK_ZONE_REC_LEN - 1u, &n,
                                      &written));
    TEST_ASSERT_EQUAL_UINT16(0, written);
    TEST_ASSERT_TRUE(plink_enc_zones(1, 1, z, 0, OUT, 9, &n, &written)); /* none: the header */
    TEST_ASSERT_EQUAL_size_t(9, n);
    TEST_ASSERT_FALSE(plink_enc_zones(1, 0, z, 1, OUT, sizeof(OUT), &n, &written)); /* zone 0 */
    TEST_ASSERT_FALSE(plink_enc_zones(1, 0xFFFF, z, 2, OUT, sizeof(OUT), &n, &written));
    TEST_ASSERT_TRUE(plink_enc_zones(1, 0xFFFF, z, 1, OUT, sizeof(OUT), &n, &written));
    uint8_t buf[sizeof(G_ZONES)];
    memcpy(buf, G_ZONES, sizeof(G_ZONES));
    buf[4] = 0; /* first: zone 0 */
    buf[5] = 0;
    TEST_ASSERT_FALSE(plink_dec_zones(buf, sizeof(buf), &zm));
    buf[4] = 0xFF; /* first 65535 with two records: past the last zone number */
    buf[5] = 0xFF;
    TEST_ASSERT_FALSE(plink_dec_zones(buf, sizeof(buf), &zm));

    /* A page past the blob's end: the encoder and the decoder refuse it. */
    static const uint8_t blob[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    plink_cfg_page_t pg = {.cfg_crc = 1, .total = 8, .offset = 6, .data = blob + 6, .len = 3};
    TEST_ASSERT_FALSE(plink_enc_cfg_page(&pg, OUT, sizeof(OUT), &n));
    pg.len = 2;
    TEST_ASSERT_TRUE(plink_enc_cfg_page(&pg, OUT, sizeof(OUT), &n));
    OUT[8] = 7; /* offset 7 + 2 > 8 */
    TEST_ASSERT_FALSE(plink_dec_cfg_page(OUT, n, &pg));
    OUT[8] = 0xFF; /* an offset near 2^32 doesn't wrap */
    OUT[9] = 0xFF;
    OUT[10] = 0xFF;
    OUT[11] = 0xFF;
    TEST_ASSERT_FALSE(plink_dec_cfg_page(OUT, n, &pg));
    /* The biggest page a UDP frame holds. */
    static uint8_t big[PLINK_PAYLOAD_MAX];
    pg = (plink_cfg_page_t){.cfg_crc = 1, .total = sizeof(big), .offset = 0, .data = big,
                            .len = (uint16_t)(PLINK_PAYLOAD_MAX - PLINK_CFG_PAGE_HDR)};
    TEST_ASSERT_TRUE(plink_enc_cfg_page(&pg, OUT, PLINK_PAYLOAD_MAX, &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_PAYLOAD_MAX, n);
    pg.len++;
    TEST_ASSERT_FALSE(plink_enc_cfg_page(&pg, OUT, PLINK_PAYLOAD_MAX, &n));
}

/* ---- random bytes into every decoder ------------------------------------------------------ */

static bool text_clean(const char *s, size_t cap)
{
    size_t len = 0;
    while (len < cap && s[len] != '\0') {
        len++;
    }
    if (len >= cap) {
        return false;
    }
    char again[256];
    plink_clean_text(s, len, again, sizeof(again));
    return strcmp(again, s) == 0;
}

/* q (n bytes) lies inside the payload p (len bytes). */
static bool inside(const uint8_t *q, size_t n, const uint8_t *p, size_t len)
{
    return n == 0u || (q != NULL && q >= p && q + n <= p + len);
}

/* A decoded fields body (n bytes, count its first byte): empty, or count TLVs whole within it -
 * what proto_prefs_decode reads without refusing it. */
static bool body_framed(const uint8_t *f, size_t n, uint8_t count)
{
    if (n == 0u) {
        return f == NULL && count == 0u;
    }
    if (f[0] != count) {
        return false;
    }
    size_t at = 1;
    for (unsigned i = 0; i < count; i++) {
        if (n - at < 2u || n - at - 2u < f[at + 1u]) {
            return false;
        }
        at += 2u + f[at + 1u];
    }
    proto_prefs_decoded_t d;
    return proto_prefs_decode(f, n, PROTO_PREFS_KNOWN, &d);
}

/* What the new decoders promise about what they accepted. */
static void check_decoded_new(const codec_t *c, const any_msg_t *m, const uint8_t *p, size_t len)
{
    if (c->dec == d_fw_info) {
        const plink_fw_info_t *f = &m->fw_info;
        TEST_ASSERT_TRUE(text_clean(f->device, sizeof(f->device)));
        TEST_ASSERT_TRUE(text_clean(f->board_rev, sizeof(f->board_rev)));
        TEST_ASSERT_TRUE(text_clean(f->chip_rev, sizeof(f->chip_rev)));
        TEST_ASSERT_TRUE(text_clean(f->project, sizeof(f->project)));
        TEST_ASSERT_TRUE(text_clean(f->version, sizeof(f->version)));
        TEST_ASSERT_TRUE(text_clean(f->cp_version, sizeof(f->cp_version)));
    } else if (c->dec == d_fw_order) {
        TEST_ASSERT_NOT_EQUAL_UINT32(0u, m->fw_order.order);
        TEST_ASSERT_TRUE(text_clean(m->fw_order.version, sizeof(m->fw_order.version)));
        TEST_ASSERT_TRUE(text_clean(m->fw_order.server, sizeof(m->fw_order.server)));
        TEST_ASSERT_TRUE(text_clean(m->fw_order.channel, sizeof(m->fw_order.channel)));
    } else if (c->dec == d_fw_state) {
        TEST_ASSERT_TRUE(text_clean(m->fw_state.target, sizeof(m->fw_state.target)));
        TEST_ASSERT_TRUE(text_clean(m->fw_state.last_version, sizeof(m->fw_state.last_version)));
        TEST_ASSERT_TRUE(text_clean(m->fw_state.offered, sizeof(m->fw_state.offered)));
    } else if (c->dec == d_fw_page) {
        TEST_ASSERT_TRUE(inside(m->fw_page.data, m->fw_page.len, p, len));
        TEST_ASSERT_TRUE((uint64_t)m->fw_page.offset + m->fw_page.len <= m->fw_page.total);
    } else if (c->dec == d_dev_list_page) {
        TEST_ASSERT_TRUE(inside(m->dev_list_page.data, m->dev_list_page.len, p, len));
    } else if (c->dec == d_dev_state) {
        TEST_ASSERT_TRUE(inside(m->dev_state.items, m->dev_state.items_len, p, len));
        size_t pos = 0, item_len = 0, count = 0, sum = 0;
        const uint8_t *item = NULL;
        plink_dev_rec_t rec;
        while (plink_dev_state_next(&m->dev_state, &pos, NULL, &rec, &item, &item_len)) {
            TEST_ASSERT_TRUE(inside(item, item_len, p, len));
            TEST_ASSERT_TRUE(text_clean(rec.word, sizeof(rec.word)));
            sum += item_len;
            count++;
        }
        TEST_ASSERT_EQUAL_size_t(m->dev_state.count, count);
        TEST_ASSERT_EQUAL_size_t(m->dev_state.items_len, sum);
    } else if (c->dec == d_dev_act) {
        TEST_ASSERT_TRUE(text_clean(m->dev_act.ref, sizeof(m->dev_act.ref)));
        TEST_ASSERT_TRUE(strspn(m->dev_act.code, "0123456789") == strlen(m->dev_act.code));
    } else if (c->dec == d_dev_ack) {
        TEST_ASSERT_TRUE(m->dev_ack.has_rec || m->dev_ack.rec_raw == NULL);
        TEST_ASSERT_TRUE(inside(m->dev_ack.rec_raw, m->dev_ack.rec_raw_len, p, len));
        TEST_ASSERT_TRUE(text_clean(m->dev_ack.rec.word, sizeof(m->dev_ack.rec.word)));
    } else if (c->dec == d_prefs) {
        const plink_prefs_t *f = &m->prefs;
        TEST_ASSERT_TRUE(text_clean(f->panel_id, sizeof(f->panel_id)));
        TEST_ASSERT_TRUE(inside(f->refused, 2u * (size_t)f->refused_count, p, len));
        TEST_ASSERT_TRUE(inside(f->fields, f->fields_len, p, len));
        TEST_ASSERT_TRUE(body_framed(f->fields, f->fields_len, f->field_count));
    } else if (c->dec == d_prefs_set) {
        const plink_prefs_set_t *f = &m->prefs_set;
        TEST_ASSERT_NOT_EQUAL_UINT32(0u, f->req);
        TEST_ASSERT_TRUE(inside(f->fields, f->fields_len, p, len));
        TEST_ASSERT_TRUE(body_framed(f->fields, f->fields_len, f->field_count));
    } else if (c->dec == d_dev_rec) {
        TEST_ASSERT_TRUE(text_clean(m->dev_rec.word, sizeof(m->dev_rec.word)));
    } else if (c->dec == d_dev_cat) {
        TEST_ASSERT_TRUE(text_clean(m->dev_cat.ref, sizeof(m->dev_cat.ref)));
        TEST_ASSERT_TRUE(text_clean(m->dev_cat.name, sizeof(m->dev_cat.name)));
        TEST_ASSERT_TRUE(text_clean(m->dev_cat.area, sizeof(m->dev_cat.area)));
    }
}

/* What a decoder promises about what it accepted. */
static void check_decoded(const codec_t *c, const any_msg_t *m, const uint8_t *p, size_t len)
{
    check_decoded_new(c, m, p, len);
    if (c->dec == d_pair_req) {
        TEST_ASSERT_TRUE(text_clean(m->pair_req.name, sizeof(m->pair_req.name)));
        TEST_ASSERT_TRUE(text_clean(m->pair_req.fw, sizeof(m->pair_req.fw)));
    } else if (c->dec == d_pair_commit) {
        TEST_ASSERT_TRUE(text_clean(m->pair_commit.name, sizeof(m->pair_commit.name)));
        TEST_ASSERT_TRUE(text_clean(m->pair_commit.panel_id, sizeof(m->pair_commit.panel_id)));
    } else if (c->dec == d_heartbeat) {
        TEST_ASSERT_TRUE(text_clean(m->heartbeat.fw, sizeof(m->heartbeat.fw)));
        TEST_ASSERT_TRUE(m->heartbeat.has_prefs || m->heartbeat.prefs_crc == 0u);
    } else if (c->dec == d_time) {
        TEST_ASSERT_TRUE(text_clean(m->time.tz, sizeof(m->time.tz)));
    } else if (c->dec == d_status) {
        TEST_ASSERT_TRUE(m->status.count <= PLINK_PARTS_MAX);
        unsigned seen = 0;
        for (size_t i = 0; i < m->status.count; i++) {
            const uint8_t num = m->status.part[i].number;
            TEST_ASSERT_TRUE(num >= 1u && num <= PLINK_PARTS_MAX);
            TEST_ASSERT_EQUAL_HEX(0, seen & (1u << (num - 1u)));
            seen |= 1u << (num - 1u);
        }
    } else if (c->dec == d_zones) {
        const size_t bytes = (size_t)m->zones.count * m->zones.rec_len;
        TEST_ASSERT_TRUE(m->zones.count == 0u || m->zones.first != 0u);
        TEST_ASSERT_TRUE(m->zones.recs >= p && m->zones.recs + bytes <= p + len);
    } else if (c->dec == d_cfg_page) {
        TEST_ASSERT_TRUE(m->cfg_page.data >= p && m->cfg_page.data + m->cfg_page.len <= p + len);
        TEST_ASSERT_TRUE((uint64_t)m->cfg_page.offset + m->cfg_page.len <= m->cfg_page.total);
    } else if (c->dec == d_events) {
        TEST_ASSERT_TRUE(m->events.n <= EVENTS_MAX);
    } else if (c->dec == d_cmd) {
        TEST_ASSERT_TRUE(strspn(m->cmd.cmd.code, "0123456789") == strlen(m->cmd.cmd.code));
        TEST_ASSERT_TRUE(strspn(m->cmd.new_code, "0123456789") == strlen(m->cmd.new_code));
    }
}

/* One payload into every decoder; whatever one accepts must keep its promises and encode to a
 * canonical form that decodes and encodes to itself. */
static size_t feed_all(const uint8_t *p, size_t len)
{
    size_t accepted = 0;
    for (size_t i = 0; i < CODEC_COUNT; i++) {
        const codec_t *c = &CODECS[i];
        if (!c->dec(p, len, &M1)) {
            continue;
        }
        accepted++;
        check_decoded(c, &M1, p, len);
        if (c->enc == NULL) {
            continue;
        }
        size_t n1 = 0, n2 = 0;
        TEST_ASSERT_TRUE_MESSAGE(c->enc(&M1, OUT, sizeof(OUT), &n1), c->name);
        TEST_ASSERT_TRUE_MESSAGE(c->dec(OUT, n1, &M2), c->name);
        TEST_ASSERT_TRUE_MESSAGE(c->enc(&M2, OUT2, sizeof(OUT2), &n2), c->name);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(n1, n2, c->name);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(OUT, OUT2, n1, c->name);
    }
    return accepted;
}

/* A byte a mutation likes: lengths, edges and the values of a field. */
static uint8_t interesting(void)
{
    static const uint8_t v[] = {0x00, 0x01, 0x02, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x10, 0x20,
                                0x30, 0x33, 0x34, 0x7F, 0x80, 0xC0, 0xE2, 0xED, 0xF4, 0xFE, 0xFF};
    return v[rnd_below(sizeof(v))];
}

/* LINK-017 (docs/features/11 "Test and acceptance": random bytes into every parser): 200000
 * random and mutated payloads go into every decoder; none crashes or reads outside its payload
 * (each payload ends at the end of its buffer, so ASan sees a read past it), and what one
 * accepts keeps the decoder's promises. */
static void test_fuzz_decoders(void)
{
    static uint8_t arena[PLINK_FRAME_MAX];
    uint8_t work[PLINK_FRAME_MAX + 64];
    RNG = 0xF022u;
    size_t accepted = 0;
    for (uint32_t iter = 0; iter < 200000u; iter++) {
        size_t len;
        if (iter % 4u == 0u) {
            /* Random bytes, most of them short. */
            len = rnd_below(iter % 8u == 0u ? PLINK_PAYLOAD_MAX + 1u : 80u);
            rnd_bytes(work, len);
        } else {
            /* A golden payload with a few mutations. */
            const codec_t *c = &CODECS[rnd_below(CODEC_COUNT)];
            memcpy(work, c->golden, c->golden_len);
            len = c->golden_len;
            const size_t mutations = 1u + rnd_below(4);
            for (size_t k = 0; k < mutations; k++) {
                const size_t at = rnd_below(len + 1u);
                switch (rnd_below(6)) {
                case 0: /* flip a bit */
                    if (len > 0u) {
                        const size_t i = rnd_below(len);
                        work[i] = (uint8_t)(work[i] ^ (1u << rnd_below(8)));
                    }
                    break;
                case 1: /* an interesting byte */
                    if (len > 0u) {
                        work[rnd_below(len)] = interesting();
                    }
                    break;
                case 2: /* cut */
                    len = at;
                    break;
                case 3: /* random bytes after */
                {
                    const size_t add = rnd_below(64);
                    if (len + add <= PLINK_PAYLOAD_MAX) {
                        rnd_bytes(work + len, add);
                        len += add;
                    }
                    break;
                }
                case 4: /* insert a byte */
                    if (len < PLINK_PAYLOAD_MAX) {
                        memmove(work + at + 1, work + at, len - at);
                        work[at] = interesting();
                        len++;
                    }
                    break;
                default: /* delete a byte */
                    if (at < len) {
                        memmove(work + at, work + at + 1, len - at - 1u);
                        len--;
                    }
                    break;
                }
            }
        }
        uint8_t *p = arena + sizeof(arena) - len;
        memcpy(p, work, len);
        accepted += feed_all(p, len);
    }
    /* The mutations kept many payloads valid, so the checks above ran on real decodes. */
    TEST_ASSERT_TRUE(accepted > 200000u);
}

/* ---- updates, Smart Home and settings (LINK-043) ------------------------------------------- */

/* n characters 'a'.. into s, then its NUL. */
static void fill_text(char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s[i] = (char)('a' + i % 26u);
    }
    s[n] = '\0';
}

/* The FW_INFO of G_FW_INFO. */
static void golden_fw_info(plink_fw_info_t *m)
{
    memset(m, 0, sizeof(*m));
    m->can = PLINK_FWI_FETCH | PLINK_FWI_RELAY | PLINK_FWI_KEY_ACCEPT;
    strcpy(m->device, "crowpanel-7");
    strcpy(m->board_rev, "1.3");
    strcpy(m->chip_rev, "1.0");
    m->chip_rev_full = 100;
    strcpy(m->project, "roboalarms");
    strcpy(m->version, "1.4.0");
    m->slot = 0x00480000u;
    fill_seq(m->own_key8, PLINK_KEY8_LEN, 0xA0);
    strcpy(m->cp_version, "2.12.3");
}

static void golden_fw_order(plink_fw_order_t *m)
{
    memset(m, 0, sizeof(*m));
    m->order = 0x01020304u;
    m->op = PLINK_FW_OP_RELAY;
    m->kind = PLINK_FW_KIND_APP;
    strcpy(m->version, "1.4.0");
    m->size = 3600000u;
    fill_seq(m->sha256, PLINK_SHA256_LEN, 0x40);
    fill_seq(m->key, PLINK_FW_KEY_LEN, 0x80);
    strcpy(m->server, "https://fw.example.com");
    strcpy(m->channel, "stable");
    m->page = 1024;
    m->count = 16;
    m->gap_ms = 50;
}

static void golden_fw_state(plink_fw_state_t *m)
{
    memset(m, 0, sizeof(*m));
    m->order = 0x01020304u;
    m->op = PLINK_FW_OP_RELAY;
    m->state = PLINK_FWS_RECEIVING;
    m->flags = PLINK_FWF_PAUSED;
    m->why = PLINK_FW_WHY_NO_STATUS;
    m->bytes = 1048576u;
    m->size = 3600000u;
    fill_seq(m->id8, PLINK_ID8_LEN, 0x40);
    strcpy(m->target, "1.4.0");
    m->last = PLINK_FW_LAST_ROLLED_BACK;
    strcpy(m->last_version, "1.3.9");
    m->failed_mask = 0x05;
    m->asked = 1;
    m->probe_why = PLINK_FW_WHY_OTHER_VERSION;
    m->age_s = 3600;
    strcpy(m->offered, "1.5.0");
    fill_seq(m->offered_key8, PLINK_KEY8_LEN, 0xA0);
    m->key_trusted = 1;
}

/* LINK-043 (docs/features/11 "Updates through the link", "Messages"): FW_INFO, FW_ORDER,
 * FW_STATE, FW_REQ and FW_PAGE as the table lays them out. */
static void test_golden_updates(void)
{
    size_t n = 0;
    plink_fw_info_t fi, fi2;
    golden_fw_info(&fi);
    TEST_ASSERT_TRUE(plink_enc_fw_info(&fi, OUT, sizeof(OUT), &n));
    expect_golden("FW_INFO", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_fw_info(G_FW_INFO, sizeof(G_FW_INFO), &fi2));
    TEST_ASSERT_EQUAL_MEMORY(&fi, &fi2, sizeof(fi));

    plink_fw_order_t fo, fo2;
    golden_fw_order(&fo);
    TEST_ASSERT_TRUE(plink_enc_fw_order(&fo, OUT, sizeof(OUT), &n));
    expect_golden("FW_ORDER", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_fw_order(G_FW_ORDER, sizeof(G_FW_ORDER), &fo2));
    TEST_ASSERT_EQUAL_MEMORY(&fo, &fo2, sizeof(fo));

    plink_fw_state_t fs, fs2;
    golden_fw_state(&fs);
    TEST_ASSERT_TRUE(plink_enc_fw_state(&fs, OUT, sizeof(OUT), &n));
    expect_golden("FW_STATE", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_fw_state(G_FW_STATE, sizeof(G_FW_STATE), &fs2));
    TEST_ASSERT_EQUAL_MEMORY(&fs, &fs2, sizeof(fs));

    plink_fw_req_t fr, fr2;
    memset(&fr, 0, sizeof(fr));
    fill_seq(fr.id8, PLINK_ID8_LEN, 0x40);
    fr.offset = 65536u;
    fr.page = 1024;
    fr.count = 16;
    TEST_ASSERT_TRUE(plink_enc_fw_req(&fr, OUT, sizeof(OUT), &n));
    expect_golden("FW_REQ", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_fw_req(G_FW_REQ, sizeof(G_FW_REQ), &fr2));
    TEST_ASSERT_EQUAL_MEMORY(&fr, &fr2, sizeof(fr));

    static const uint8_t data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    plink_fw_page_t fp, fp2;
    memset(&fp, 0, sizeof(fp));
    fill_seq(fp.id8, PLINK_ID8_LEN, 0x40);
    fp.total = 3600000u;
    fp.offset = 65536u;
    fp.data = data;
    fp.len = 4;
    TEST_ASSERT_TRUE(plink_enc_fw_page(&fp, OUT, sizeof(OUT), &n));
    expect_golden("FW_PAGE", OUT, n);
    TEST_ASSERT_EQUAL_size_t(PLINK_FW_PAGE_HDR + 4u, n);
    TEST_ASSERT_TRUE(plink_dec_fw_page(G_FW_PAGE, sizeof(G_FW_PAGE), &fp2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(fp.id8, fp2.id8, PLINK_ID8_LEN);
    TEST_ASSERT_EQUAL_UINT32(3600000u, fp2.total);
    TEST_ASSERT_EQUAL_UINT32(65536u, fp2.offset);
    TEST_ASSERT_EQUAL_UINT16(4, fp2.len);
    TEST_ASSERT_EQUAL_PTR(G_FW_PAGE + PLINK_FW_PAGE_HDR, fp2.data);
}

/* LINK-043: a why is read by its decade; values past the known classes are a newer firmware's. */
static void test_fw_why_class(void)
{
    TEST_ASSERT_EQUAL_INT(PLINK_FWC_OK, plink_fw_why_class(PLINK_FW_WHY_OK));
    for (unsigned w = 1; w < 256u; w++) {
        const plink_fw_why_class_t c = plink_fw_why_class((uint8_t)w);
        const plink_fw_why_class_t want =
            w < 10u ? PLINK_FWC_NOT_REACHED
            : w < 20u ? PLINK_FWC_SERVER
            : w < 30u ? PLINK_FWC_TRANSFER
            : w < 50u ? PLINK_FWC_REFUSED
            : w < 60u ? PLINK_FWC_WAITING
            : w < 70u ? PLINK_FWC_STOPPED
                      : PLINK_FWC_UNKNOWN;
        TEST_ASSERT_EQUAL_INT(want, c);
    }
    TEST_ASSERT_EQUAL_INT(PLINK_FWC_NOT_REACHED, plink_fw_why_class(PLINK_FW_WHY_NOT_A_SERVER));
    TEST_ASSERT_EQUAL_INT(PLINK_FWC_SERVER, plink_fw_why_class(PLINK_FW_WHY_UP_TO_DATE));
    TEST_ASSERT_EQUAL_INT(PLINK_FWC_TRANSFER, plink_fw_why_class(PLINK_FW_WHY_FLASH));
    TEST_ASSERT_EQUAL_INT(PLINK_FWC_REFUSED, plink_fw_why_class(PLINK_FW_WHY_WRONG_VERSION));
    TEST_ASSERT_EQUAL_INT(PLINK_FWC_WAITING, plink_fw_why_class(PLINK_FW_WHY_BUSY));
    TEST_ASSERT_EQUAL_INT(PLINK_FWC_STOPPED, plink_fw_why_class(PLINK_FW_WHY_CANCELLED));
}

/* The two records of G_DEV_STATE and the lock's of G_DEV_ACK_REC. */
static void rec_light(plink_dev_rec_t *r)
{
    memset(r, 0, sizeof(*r));
    r->flags = PLINK_DEVF_ONLINE;
    r->vflags = 0x09; /* DEV_VAL_LEVEL | DEV_VAL_LEVEL_SET */
    r->level = 75;
    r->caps = 0x88; /* DEV_CAP_DIM | DEV_CAP_KNOWN */
    strcpy(r->word, "on");
}

static void rec_heat(plink_dev_rec_t *r)
{
    memset(r, 0, sizeof(*r));
    r->flags = PLINK_DEVF_ONLINE | PLINK_DEVF_PENDING;
    r->action = 10; /* ACT_SET_TEMP */
    r->vflags = 0x06; /* DEV_VAL_SETPOINT | DEV_VAL_TEMP */
    r->modes = PLINK_DEV_MODE_OFF | PLINK_DEV_MODE_HEAT | PLINK_DEV_MODE_AUTO;
    r->hvac_action = 2; /* DEV_HVAC_HEATING */
    r->setpoint = 2150;
    r->temp = -550;
    r->since = 1760000000u;
    r->caps = 0x80;
    strcpy(r->word, "heat");
}

static void rec_lock(plink_dev_rec_t *r)
{
    memset(r, 0, sizeof(*r));
    r->flags = PLINK_DEVF_ONLINE | PLINK_DEVF_PENDING;
    r->action = 5; /* ACT_UNLOCK */
    strcpy(r->word, "locked");
}

static void golden_dev_act(plink_dev_act_t *a)
{
    memset(a, 0, sizeof(*a));
    a->req = 0x0BEEF001u;
    a->action = 5; /* ACT_UNLOCK */
    strcpy(a->ref, "lock.front_door");
    strcpy(a->code, "1234");
}

/* LINK-043, LINK-047 (docs/features/11 "Smart Home and Home Assistant on keypads"): DEV_LIST_REQ
 * and DEV_LIST_PAGE (CFG_REQ's and CFG_PAGE's layouts), DEV_STATE with its records, DEV_STATE_REQ,
 * DEV_ACT, DEV_ACK with and without its record, and a catalog record, as the tables lay them
 * out. */
static void test_golden_home(void)
{
    size_t n = 0;
    const plink_dev_list_req_t lr = {.cfg_crc = 0x1BADB002u, .offset = 0, .max = 1024};
    TEST_ASSERT_TRUE(plink_enc_dev_list_req(&lr, OUT, sizeof(OUT), &n));
    expect_golden("DEV_LIST_REQ", OUT, n);
    plink_dev_list_req_t lr2;
    TEST_ASSERT_TRUE(plink_dec_dev_list_req(G_DEV_LIST_REQ, sizeof(G_DEV_LIST_REQ), &lr2));
    TEST_ASSERT_EQUAL_HEX32(0x1BADB002u, lr2.cfg_crc);
    TEST_ASSERT_EQUAL_UINT16(1024, lr2.max);
    static const uint8_t blob[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0xCA, 0xFE, 0xF0, 0x0D};
    const plink_dev_list_page_t lp = {
        .cfg_crc = 0x1BADB002u, .total = 16, .offset = 12, .data = blob + 12, .len = 4};
    TEST_ASSERT_TRUE(plink_enc_dev_list_page(&lp, OUT, sizeof(OUT), &n));
    expect_golden("DEV_LIST_PAGE", OUT, n);
    plink_dev_list_page_t lp2;
    TEST_ASSERT_TRUE(plink_dec_dev_list_page(G_DEV_LIST_PAGE, sizeof(G_DEV_LIST_PAGE), &lp2));
    TEST_ASSERT_EQUAL_PTR(G_DEV_LIST_PAGE + PLINK_CFG_PAGE_HDR, lp2.data);

    /* Records: encoded once each, DEV_STATE copies them. */
    plink_dev_rec_t light, heat, got;
    rec_light(&light);
    rec_heat(&heat);
    TEST_ASSERT_TRUE(plink_enc_dev_rec(&heat, OUT, sizeof(OUT), &n));
    expect_golden("dev record", OUT, n);
    size_t used = 0;
    TEST_ASSERT_TRUE(plink_dec_dev_rec(G_DEV_REC, sizeof(G_DEV_REC), &got, &used));
    TEST_ASSERT_EQUAL_size_t(sizeof(G_DEV_REC), used);
    TEST_ASSERT_EQUAL_MEMORY(&heat, &got, sizeof(got));
    uint8_t items[2u * PLINK_DEV_ITEM_MAX];
    size_t a = 0, b = 0;
    TEST_ASSERT_TRUE(plink_enc_dev_item(0, &light, items, sizeof(items), &a));
    TEST_ASSERT_TRUE(plink_enc_dev_item(5, &heat, items + a, sizeof(items) - a, &b));
    const plink_dev_state_t ds = {
        .list_crc = 0x1BADB002u, .states_crc = 0x5EED5EEDu, .count = 2, .items = items,
        .items_len = a + b};
    TEST_ASSERT_TRUE(plink_enc_dev_state(&ds, OUT, sizeof(OUT), &n));
    expect_golden("DEV_STATE", OUT, n);
    plink_dev_state_t ds2;
    TEST_ASSERT_TRUE(plink_dec_dev_state(G_DEV_STATE, sizeof(G_DEV_STATE), &ds2));
    TEST_ASSERT_EQUAL_HEX32(0x1BADB002u, ds2.list_crc);
    TEST_ASSERT_EQUAL_HEX32(0x5EED5EEDu, ds2.states_crc);
    TEST_ASSERT_EQUAL_UINT8(2, ds2.count);
    TEST_ASSERT_EQUAL_PTR(G_DEV_STATE + PLINK_DEV_STATE_HEAD, ds2.items);
    TEST_ASSERT_EQUAL_size_t(sizeof(G_DEV_STATE) - PLINK_DEV_STATE_HEAD, ds2.items_len);
    size_t pos = 0, item_len = 0;
    uint8_t index = 0xFF;
    const uint8_t *item = NULL;
    TEST_ASSERT_TRUE(plink_dev_state_next(&ds2, &pos, &index, &got, &item, &item_len));
    TEST_ASSERT_EQUAL_UINT8(0, index);
    TEST_ASSERT_EQUAL_MEMORY(&light, &got, sizeof(got));
    TEST_ASSERT_EQUAL_PTR(G_DEV_STATE + 10, item);
    TEST_ASSERT_EQUAL_size_t(24, item_len);
    TEST_ASSERT_TRUE(plink_dev_state_next(&ds2, &pos, &index, &got, &item, &item_len));
    TEST_ASSERT_EQUAL_UINT8(5, index);
    TEST_ASSERT_EQUAL_MEMORY(&heat, &got, sizeof(got));
    TEST_ASSERT_EQUAL_PTR(G_DEV_STATE + 34, item);
    TEST_ASSERT_EQUAL_size_t(1u + sizeof(G_DEV_REC), item_len);
    TEST_ASSERT_FALSE(plink_dev_state_next(&ds2, &pos, &index, &got, &item, &item_len));
    /* The summary: no record at all. */
    const plink_dev_state_t sum = {.list_crc = 1, .states_crc = 2, .count = 0};
    TEST_ASSERT_TRUE(plink_enc_dev_state(&sum, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_DEV_STATE_HEAD, n);
    TEST_ASSERT_TRUE(plink_dec_dev_state(OUT, n, &ds2));
    TEST_ASSERT_EQUAL_UINT8(0, ds2.count);
    pos = 0;
    TEST_ASSERT_FALSE(plink_dev_state_next(&ds2, &pos, NULL, NULL, NULL, NULL));

    const plink_dev_state_req_t sr = {.list_crc = 0x1BADB002u, .first = 0, .count = 0};
    TEST_ASSERT_TRUE(plink_enc_dev_state_req(&sr, OUT, sizeof(OUT), &n));
    expect_golden("DEV_STATE_REQ", OUT, n);
    plink_dev_state_req_t sr2;
    TEST_ASSERT_TRUE(plink_dec_dev_state_req(G_DEV_STATE_REQ, sizeof(G_DEV_STATE_REQ), &sr2));
    TEST_ASSERT_EQUAL_HEX32(0x1BADB002u, sr2.list_crc);

    plink_dev_act_t act, act2;
    golden_dev_act(&act);
    TEST_ASSERT_TRUE(plink_enc_dev_act(&act, OUT, sizeof(OUT), &n));
    expect_golden("DEV_ACT", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_dev_act(G_DEV_ACT, sizeof(G_DEV_ACT), &act2));
    TEST_ASSERT_EQUAL_MEMORY(&act, &act2, sizeof(act));
    memset(&act, 0, sizeof(act));
    act.req = 0x0BEEF003u;
    act.action = 11; /* ACT_SET_MODE */
    act.mode = PLINK_DEV_MODE_HEAT;
    strcpy(act.ref, "climate.hall");
    TEST_ASSERT_TRUE(plink_enc_dev_act(&act, OUT, sizeof(OUT), &n));
    expect_golden("DEV_ACT mode", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_dev_act(G_DEV_ACT_MODE, sizeof(G_DEV_ACT_MODE), &act2));
    TEST_ASSERT_EQUAL_MEMORY(&act, &act2, sizeof(act));

    plink_dev_ack_t ack, ack2;
    memset(&ack, 0, sizeof(ack));
    ack.req = 0x0BEEF001u;
    ack.result = PLINK_DEV_CODE_REFUSED;
    ack.why = 3;
    TEST_ASSERT_TRUE(plink_enc_dev_ack(&ack, OUT, sizeof(OUT), &n));
    expect_golden("DEV_ACK", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_dev_ack(G_DEV_ACK, sizeof(G_DEV_ACK), &ack2));
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_CODE_REFUSED, ack2.result);
    TEST_ASSERT_EQUAL_UINT8(3, ack2.why);
    TEST_ASSERT_FALSE(ack2.has_rec);
    TEST_ASSERT_NULL(ack2.rec_raw);
    memset(&ack, 0, sizeof(ack));
    ack.req = 0x0BEEF002u;
    ack.has_rec = true;
    ack.list_crc = 0x1BADB002u;
    ack.index = 7;
    rec_lock(&ack.rec);
    TEST_ASSERT_TRUE(plink_enc_dev_ack(&ack, OUT, sizeof(OUT), &n));
    expect_golden("DEV_ACK rec", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_dev_ack(G_DEV_ACK_REC, sizeof(G_DEV_ACK_REC), &ack2));
    TEST_ASSERT_TRUE(ack2.has_rec);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_OK, ack2.result);
    TEST_ASSERT_EQUAL_HEX32(0x1BADB002u, ack2.list_crc);
    TEST_ASSERT_EQUAL_UINT8(7, ack2.index);
    TEST_ASSERT_EQUAL_MEMORY(&ack.rec, &ack2.rec, sizeof(ack.rec));
    TEST_ASSERT_EQUAL_PTR(G_DEV_ACK_REC + 12, ack2.rec_raw);
    TEST_ASSERT_EQUAL_size_t(27, ack2.rec_raw_len);

    plink_dev_cat_t cat, cat2;
    memset(&cat, 0, sizeof(cat));
    cat.kind = 3;   /* DEV_LOCK */
    cat.source = 1; /* DSRC_HA */
    strcpy(cat.ref, "lock.front_door");
    strcpy(cat.name, "Front door");
    strcpy(cat.area, "Hall");
    TEST_ASSERT_TRUE(plink_enc_dev_cat(&cat, OUT, sizeof(OUT), &n));
    expect_golden("catalog record", OUT, n);
    TEST_ASSERT_TRUE(plink_dec_dev_cat(G_DEV_CAT, sizeof(G_DEV_CAT), &cat2, &used));
    TEST_ASSERT_EQUAL_size_t(sizeof(G_DEV_CAT), used);
    TEST_ASSERT_EQUAL_MEMORY(&cat, &cat2, sizeof(cat));
}

/* A fields body with every v1 settings field, 1..19: the count, then the TLVs (proto_prefs.h's
 * sizes: 3, 4 and 9 four bytes, 6 and 7 two, the rest one), each value its id: 69 bytes. */
static size_t v1_fields(uint8_t *out)
{
    size_t n = 0;
    out[n++] = 19u;
    for (uint8_t id = 1; id <= 19u; id++) {
        const uint8_t size =
            (uint8_t)(id == 3u || id == 4u || id == 9u ? 4u : id == 6u || id == 7u ? 2u : 1u);
        out[n++] = id;
        out[n++] = size;
        for (uint8_t i = 0; i < size; i++) {
            out[n++] = (uint8_t)(i == 0u ? id : 0u);
        }
    }
    return n;
}

/* LINK-050, LINK-051: PREFS, PREFS_REQ, PREFS_SET and HEARTBEAT's section as the tables lay them
 * out; a fields body (count first) is opaque bytes here, kept where it lies in the payload. */
static void test_golden_prefs(void)
{
    size_t n = 0;
    static const uint8_t refused[] = {11, PLINK_PREFS_IN_USE};
    static const uint8_t fields[] = {0x03, 0x01, 0x01, 0x28, 0x06, 0x02, 0x28, 0x05,
                                     0x03, 0x04, 0xC0, 0x27, 0x09, 0x00};
    plink_prefs_t p;
    memset(&p, 0, sizeof(p));
    p.req = 0x777;
    strcpy(p.panel_id, "a1b2c3");
    p.voice = PLINK_VOICE_READY;
    p.accepts = 0x0007FFFFu;
    p.refused_count = 1;
    p.refused = refused;
    p.field_count = 99; /* not read: the count is the body's */
    p.fields = fields;
    p.fields_len = sizeof(fields);
    TEST_ASSERT_TRUE(plink_enc_prefs(&p, OUT, sizeof(OUT), &n));
    expect_golden("PREFS", OUT, n);
    plink_prefs_t p2;
    TEST_ASSERT_TRUE(plink_dec_prefs(G_PREFS, sizeof(G_PREFS), &p2));
    TEST_ASSERT_EQUAL_HEX32(0x777, p2.req);
    TEST_ASSERT_EQUAL_STRING("a1b2c3", p2.panel_id);
    TEST_ASSERT_EQUAL_UINT8(PLINK_VOICE_READY, p2.voice);
    TEST_ASSERT_EQUAL_HEX32(0x0007FFFFu, p2.accepts);
    TEST_ASSERT_EQUAL_UINT8(1, p2.refused_count);
    TEST_ASSERT_EQUAL_PTR(G_PREFS + 20, p2.refused);
    TEST_ASSERT_EQUAL_UINT8(3, p2.field_count);
    TEST_ASSERT_EQUAL_PTR(G_PREFS + 23, p2.fields); /* the body, its count first */
    TEST_ASSERT_EQUAL_size_t(sizeof(fields), p2.fields_len);
    /* Unsolicited, nothing refused, no field (proto_prefs_encode's body {0}): every section still
     * there. */
    static const uint8_t none[] = {0x00};
    memset(&p, 0, sizeof(p));
    p.fields = none;
    p.fields_len = sizeof(none);
    TEST_ASSERT_TRUE(plink_enc_prefs(&p, OUT, sizeof(OUT), &n));
    static const uint8_t empty[] = {0x04, 0, 0, 0, 0, 0x06, 0, 0, 0, 0, 0, 0, 0x01, 0, 0x01, 0};
    TEST_ASSERT_EQUAL_size_t(sizeof(empty), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(empty, OUT, sizeof(empty));
    TEST_ASSERT_TRUE(plink_dec_prefs(OUT, n, &p2));
    TEST_ASSERT_EQUAL_UINT8(0, p2.field_count);
    TEST_ASSERT_EQUAL_PTR(OUT + 15, p2.fields);
    TEST_ASSERT_EQUAL_size_t(1, p2.fields_len);
    /* An empty body: an empty section, read back as one. */
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_TRUE(plink_enc_prefs(&p, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(sizeof(empty) - 1u, n);
    TEST_ASSERT_EQUAL_HEX8(0x00, OUT[n - 1u]);
    TEST_ASSERT_TRUE(plink_dec_prefs(OUT, n, &p2));
    TEST_ASSERT_EQUAL_UINT8(0, p2.field_count);
    TEST_ASSERT_NULL(p2.fields);
    TEST_ASSERT_EQUAL_size_t(0, p2.fields_len);

    const plink_prefs_req_t pr = {.have_crc = 0xFEEDF00Du};
    TEST_ASSERT_TRUE(plink_enc_prefs_req(&pr, OUT, sizeof(OUT), &n));
    expect_golden("PREFS_REQ", OUT, n);
    plink_prefs_req_t pr2;
    TEST_ASSERT_TRUE(plink_dec_prefs_req(G_PREFS_REQ, sizeof(G_PREFS_REQ), &pr2));
    TEST_ASSERT_EQUAL_HEX32(0xFEEDF00Du, pr2.have_crc);

    static const uint8_t change[] = {0x02, 0x01, 0x01, 0x28, 0x0B, 0x01, 0x02};
    const plink_prefs_set_t ps = {.req = 0x777, .origin = PLINK_ORIGIN_HA, .fields = change,
                                  .fields_len = sizeof(change)};
    TEST_ASSERT_TRUE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));
    expect_golden("PREFS_SET", OUT, n);
    plink_prefs_set_t ps2;
    TEST_ASSERT_TRUE(plink_dec_prefs_set(G_PREFS_SET, sizeof(G_PREFS_SET), &ps2));
    TEST_ASSERT_EQUAL_HEX32(0x777, ps2.req);
    TEST_ASSERT_EQUAL_UINT8(PLINK_ORIGIN_HA, ps2.origin);
    TEST_ASSERT_EQUAL_UINT8(2, ps2.field_count);
    TEST_ASSERT_EQUAL_PTR(G_PREFS_SET + 6, ps2.fields); /* the body, its count first */
    TEST_ASSERT_EQUAL_size_t(sizeof(change), ps2.fields_len);
}

/* proto_prefs_values_crc of panel_prefs_defaults() as fields: test_prefs.c's GOLDEN_CRC, the
 * CRC-32 of its fields body worked out outside this code. */
#define DEFAULTS_CRC 0x31DE243Eu

/* panel_prefs_defaults() as fields (test_prefs.c's defaults()). */
static proto_prefs_values_t prefs_defaults(void)
{
    static const uint32_t V[PROTO_PREFS_F_LAST] = {
        80u, 25u, 60000u, 0u, 0u, 1320u, 420u, 20u, 60000u, 0u, 0u, 0u, 70u, 70u, 70u, 1u, 1u,
        1u, 1u,
    };
    proto_prefs_values_t v;
    memset(&v, 0, sizeof(v));
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, proto_prefs_put(&v, id, V[id - 1u]));
    }
    return v;
}

/* Encodes body (n bytes) in a PREFS (into OUT) and a PREFS_SET (into OUT2) and decodes both into
 * *got and *got_set: each carries it whole, at its place in the payload, ready for
 * proto_prefs_decode and proto_prefs_crc as received. */
static void prefs_body_through(const uint8_t *body, size_t n, plink_prefs_t *got,
                               plink_prefs_set_t *got_set)
{
    size_t len = 0;
    plink_prefs_t p;
    memset(&p, 0, sizeof(p));
    p.req = 5;
    strcpy(p.panel_id, "a1b2c3");
    p.fields = body;
    p.fields_len = n;
    TEST_ASSERT_TRUE(plink_enc_prefs(&p, OUT, sizeof(OUT), &len));
    TEST_ASSERT_EQUAL_HEX8(n, OUT[len - n - 1u]); /* the section's length byte, then the body */
    TEST_ASSERT_TRUE(plink_dec_prefs(OUT, len, got));
    TEST_ASSERT_EQUAL_PTR(OUT + len - n, got->fields);
    TEST_ASSERT_EQUAL_size_t(n, got->fields_len);
    TEST_ASSERT_EQUAL_UINT8(body[0], got->field_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(body, got->fields, n);

    plink_prefs_set_t s;
    memset(&s, 0, sizeof(s));
    s.req = 6;
    s.origin = PLINK_ORIGIN_HA;
    s.fields = body;
    s.fields_len = n;
    TEST_ASSERT_TRUE(plink_enc_prefs_set(&s, OUT2, sizeof(OUT2), &len));
    TEST_ASSERT_EQUAL_size_t(4u + 1u + 1u + n, len);
    TEST_ASSERT_TRUE(plink_dec_prefs_set(OUT2, len, got_set));
    TEST_ASSERT_EQUAL_PTR(OUT2 + 6, got_set->fields);
    TEST_ASSERT_EQUAL_size_t(n, got_set->fields_len);
    TEST_ASSERT_EQUAL_UINT8(body[0], got_set->field_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(body, got_set->fields, n);
}

/* LINK-050, LINK-051: a fields body goes from proto_prefs_encode through plink_enc_prefs and
 * plink_dec_prefs (and PREFS_SET's codecs) to proto_prefs_decode unchanged, so the CRC the master
 * takes over the body it received (proto_prefs_crc) is the keypad's own proto_prefs_values_crc -
 * the defaults' known answer included - and a newer keypad's body, with bytes appended after its
 * TLVs, keeps its CRC on both sides while this firmware still reads its fields. */
static void test_prefs_body_crc(void)
{
    uint8_t body[PROTO_PREFS_ENC_MAX + 8u];
    proto_prefs_decoded_t d;
    plink_prefs_t got;
    plink_prefs_set_t got_set;
    const proto_prefs_values_t v = prefs_defaults();
    size_t n = proto_prefs_encode(&v, PROTO_PREFS_KNOWN, body, sizeof(body));
    TEST_ASSERT_EQUAL_size_t(PROTO_PREFS_ENC_MAX, n);
    prefs_body_through(body, n, &got, &got_set);
    TEST_ASSERT_EQUAL_HEX32(DEFAULTS_CRC, proto_prefs_values_crc(&v));
    TEST_ASSERT_EQUAL_HEX32(DEFAULTS_CRC, proto_prefs_crc(got.fields, got.fields_len));
    TEST_ASSERT_EQUAL_HEX32(DEFAULTS_CRC, proto_prefs_crc(got_set.fields, got_set.fields_len));
    TEST_ASSERT_TRUE(proto_prefs_decode(got.fields, got.fields_len, PROTO_PREFS_KNOWN, &d));
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_KNOWN, d.values.have);
    TEST_ASSERT_EQUAL_HEX32(0u, d.refused);
    TEST_ASSERT_EQUAL_HEX32(DEFAULTS_CRC, proto_prefs_values_crc(&d.values));

    /* Random subsets and values; nothing at all (the body {0}); a newer keypad's appended bytes. */
    RNG = 0x9EF5u;
    for (int iter = 0; iter < 2000; iter++) {
        proto_prefs_values_t r;
        memset(&r, 0, sizeof(r));
        const proto_prefs_mask_t have = iter == 0 ? 0u : rnd32() & PROTO_PREFS_KNOWN;
        for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
            const proto_prefs_field_t *f = proto_prefs_field(id);
            if ((have & PROTO_PREFS_BIT(id)) != 0u) {
                const uint32_t x = f->min + (uint32_t)rnd_below((size_t)(f->max - f->min) + 1u);
                TEST_ASSERT_EQUAL_INT(PROTO_PREFS_WHY_NONE, proto_prefs_put(&r, id, x));
            }
        }
        n = proto_prefs_encode(&r, PROTO_PREFS_KNOWN, body, sizeof(body));
        TEST_ASSERT_TRUE(n >= 1u);
        const size_t extra = (size_t)(iter % 3) * 4u; /* 0, 4 or 8 bytes a later version appends */
        rnd_bytes(body + n, extra);
        const uint32_t own = proto_prefs_crc(body, n + extra); /* the keypad's prefs_crc */
        if (extra == 0u) {
            TEST_ASSERT_EQUAL_HEX32(proto_prefs_values_crc(&r), own);
        }
        prefs_body_through(body, n + extra, &got, &got_set);
        TEST_ASSERT_EQUAL_HEX32(own, proto_prefs_crc(got.fields, got.fields_len));
        TEST_ASSERT_EQUAL_HEX32(own, proto_prefs_crc(got_set.fields, got_set.fields_len));
        TEST_ASSERT_TRUE(proto_prefs_decode(got.fields, got.fields_len, PROTO_PREFS_KNOWN, &d));
        TEST_ASSERT_EQUAL_HEX32(have, d.values.have);
        TEST_ASSERT_EQUAL_HEX32(proto_prefs_values_crc(&r), proto_prefs_values_crc(&d.values));
        TEST_ASSERT_TRUE(
            proto_prefs_decode(got_set.fields, got_set.fields_len, PROTO_PREFS_KNOWN, &d));
        TEST_ASSERT_EQUAL_HEX32(have, d.values.have);
    }
}

/* HEARTBEAT as v1's decoder read it before the section existed: its fields, nothing after them
 * looked at. A master of that firmware runs it. */
static bool v1_dec_heartbeat(const uint8_t *p, size_t len, plink_heartbeat_t *m)
{
    memset(m, 0, sizeof(*m));
    plink_r_t r;
    plink_r_init(&r, p, len);
    m->flags = plink_r_u8(&r);
    const uint8_t rssi = plink_r_u8(&r);
    m->rssi = rssi < 0x80u ? (int8_t)rssi : (int8_t)((int)rssi - 256);
    m->uptime_s = plink_r_u32(&r);
    m->status_age_s = plink_r_u16(&r);
    plink_r_str(&r, m->fw, sizeof(m->fw));
    return r.ok;
}

static void expect_hb_fields(const plink_heartbeat_t *hb)
{
    TEST_ASSERT_EQUAL_HEX8(0x05, hb->flags);
    TEST_ASSERT_EQUAL_INT8(-61, hb->rssi);
    TEST_ASSERT_EQUAL_UINT32(86400, hb->uptime_s);
    TEST_ASSERT_EQUAL_HEX16(PLINK_STATUS_AGE_NONE, hb->status_age_s);
    TEST_ASSERT_EQUAL_STRING("0.9.1", hb->fw);
}

/* LINK-050, LINK-017: HEARTBEAT's appended section (prefs_crc) goes only when asked for, and is
 * read only when bytes follow fw: v1's heartbeat without it decodes as it always did, the new
 * one with it is read whole by today's decoder, and v1's decoder reads its fields and ignores the
 * section. A newer keypad's longer section is read as far as this firmware knows; a section cut
 * short is refused; an empty section, or one too short for prefs_crc, carries no CRC (and a later
 * section may follow it). */
static void test_heartbeat_section(void)
{
    plink_heartbeat_t hb, got;
    memset(&hb, 0, sizeof(hb));
    hb.flags = PLINK_HB_MAINS_LOST | PLINK_HB_BATTERY_THERE;
    hb.rssi = -61;
    hb.uptime_s = 86400;
    hb.status_age_s = PLINK_STATUS_AGE_NONE;
    strcpy(hb.fw, "0.9.1");
    hb.prefs_crc = 0x0C0FFEE0u; /* not asked for: not sent */
    size_t n = 0;
    TEST_ASSERT_TRUE(plink_enc_heartbeat(&hb, OUT, sizeof(OUT), &n));
    expect_golden("HEARTBEAT", OUT, n);
    hb.has_prefs = true;
    TEST_ASSERT_TRUE(plink_enc_heartbeat(&hb, OUT, sizeof(OUT), &n));
    expect_golden("HEARTBEAT prefs", OUT, n);

    TEST_ASSERT_TRUE(plink_dec_heartbeat(G_HEARTBEAT, sizeof(G_HEARTBEAT), &got));
    expect_hb_fields(&got);
    TEST_ASSERT_FALSE(got.has_prefs);
    TEST_ASSERT_EQUAL_HEX32(0, got.prefs_crc);
    TEST_ASSERT_TRUE(plink_dec_heartbeat(G_HEARTBEAT_PREFS, sizeof(G_HEARTBEAT_PREFS), &got));
    expect_hb_fields(&got);
    TEST_ASSERT_TRUE(got.has_prefs);
    TEST_ASSERT_EQUAL_HEX32(0x0C0FFEE0u, got.prefs_crc);
    TEST_ASSERT_TRUE(v1_dec_heartbeat(G_HEARTBEAT, sizeof(G_HEARTBEAT), &got));
    expect_hb_fields(&got);
    TEST_ASSERT_TRUE(v1_dec_heartbeat(G_HEARTBEAT_PREFS, sizeof(G_HEARTBEAT_PREFS), &got));
    expect_hb_fields(&got);

    /* A newer keypad's section, three bytes longer, and bytes after it. */
    uint8_t buf[64], buf2[64];
    static const uint8_t more[] = {0x11, 0x22, 0x33};
    n = splice(buf, G_HEARTBEAT_PREFS, sizeof(G_HEARTBEAT_PREFS), sizeof(G_HEARTBEAT_PREFS), more,
               sizeof(more));
    buf[sizeof(G_HEARTBEAT)] = 4u + 3u;
    n = splice(buf2, buf, n, n, more, sizeof(more));
    TEST_ASSERT_TRUE(plink_dec_heartbeat(buf2, n, &got));
    TEST_ASSERT_TRUE(got.has_prefs);
    TEST_ASSERT_EQUAL_HEX32(0x0C0FFEE0u, got.prefs_crc);
    /* The section cut short: refused (a section's length never lies). */
    TEST_ASSERT_FALSE(plink_dec_heartbeat(G_HEARTBEAT_PREFS, sizeof(G_HEARTBEAT_PREFS) - 1u, &got));
    TEST_ASSERT_EQUAL_STRING("", got.fw);
    /* An empty section, alone and before a later one; a section of 1-3 bytes: no prefs_crc. */
    static const uint8_t empty_then[] = {0x00, 0x02, 0x11, 0x22};
    for (size_t k = 1; k <= sizeof(empty_then); k += 3u) {
        n = splice(buf, G_HEARTBEAT, sizeof(G_HEARTBEAT), sizeof(G_HEARTBEAT), empty_then, k);
        TEST_ASSERT_TRUE(plink_dec_heartbeat(buf, n, &got));
        expect_hb_fields(&got);
        TEST_ASSERT_FALSE(got.has_prefs);
        TEST_ASSERT_EQUAL_HEX32(0, got.prefs_crc);
        expect_canonical(codec("HEARTBEAT"), buf, n, G_HEARTBEAT, sizeof(G_HEARTBEAT));
    }
    for (uint8_t len = 1; len < 4u; len++) {
        memcpy(buf, G_HEARTBEAT_PREFS, sizeof(G_HEARTBEAT_PREFS));
        buf[sizeof(G_HEARTBEAT)] = len;
        n = sizeof(G_HEARTBEAT) + 1u + len;
        TEST_ASSERT_TRUE(plink_dec_heartbeat(buf, n, &got));
        expect_hb_fields(&got);
        TEST_ASSERT_FALSE(got.has_prefs);
        TEST_ASSERT_EQUAL_HEX32(0, got.prefs_crc);
    }
}

/* LINK-047, LINK-017: a DEV_ACK's record section is optional; when its section holds anything,
 * it holds list_crc, the index and a whole record. An empty one means no record, so a later
 * version can append a section after it to an answer without one (NEED_CODE, BUSY...). */
static void test_dev_ack_record(void)
{
    plink_dev_ack_t ack;
    uint8_t buf[64];
    /* The section with only list_crc and the index: no record, refused. */
    static const uint8_t no_rec[] = {0x01, 0xF0, 0xEE, 0x0B, 0x00, 0x00,
                                     0x05, 0x02, 0xB0, 0xAD, 0x1B, 0x07};
    TEST_ASSERT_FALSE(plink_dec_dev_ack(no_rec, sizeof(no_rec), &ack));
    TEST_ASSERT_EQUAL_UINT32(0, ack.req);
    /* The record cut by the optional section's end: refused. */
    memcpy(buf, G_DEV_ACK_REC, sizeof(G_DEV_ACK_REC));
    buf[6] = (uint8_t)(buf[6] - 1u);
    TEST_ASSERT_FALSE(plink_dec_dev_ack(buf, sizeof(G_DEV_ACK_REC) - 1u, &ack));
    TEST_ASSERT_FALSE(plink_dec_dev_ack(buf, sizeof(G_DEV_ACK_REC), &ack));
    /* An empty optional section, alone and before a later section: no record. */
    static const uint8_t empty_then[] = {0x00, 0x03, 0xA1, 0xA2, 0xA3};
    for (size_t k = 1; k <= sizeof(empty_then); k += 4u) {
        const size_t n = splice(buf, G_DEV_ACK, sizeof(G_DEV_ACK), sizeof(G_DEV_ACK), empty_then, k);
        TEST_ASSERT_TRUE(plink_dec_dev_ack(buf, n, &ack));
        TEST_ASSERT_EQUAL_HEX32(0x0BEEF001u, ack.req);
        TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_CODE_REFUSED, ack.result);
        TEST_ASSERT_EQUAL_UINT8(3, ack.why);
        TEST_ASSERT_FALSE(ack.has_rec);
        TEST_ASSERT_NULL(ack.rec_raw);
        TEST_ASSERT_EQUAL_size_t(0, ack.rec_raw_len);
        expect_canonical(codec("DEV_ACK"), buf, n, G_DEV_ACK, sizeof(G_DEV_ACK));
    }
    /* After a record, a later section is skipped too. */
    const size_t n = splice(buf, G_DEV_ACK_REC, sizeof(G_DEV_ACK_REC), sizeof(G_DEV_ACK_REC),
                            empty_then + 1, 4);
    expect_canonical(codec("DEV_ACK rec"), buf, n, G_DEV_ACK_REC, sizeof(G_DEV_ACK_REC));
}

/* LINK-047 (docs/features/11 "Device actions"): every DEV_ACT has PLINK_DEV_ACT_LEN bytes,
 * whatever the ref and the code; a ref that is too long or that cleaning would change, and a code
 * that isn't digits, are refused by both codecs; the padding is never read. */
static void test_dev_act_one_size(void)
{
    plink_dev_act_t a, b;
    size_t n = 0;
    for (size_t r = 0; r <= PLINK_REF_MAX; r++) {
        for (size_t c = 0; c <= ALARM_CODE_MAX_LEN; c += 5u) {
            golden_dev_act(&a);
            fill_text(a.ref, r);
            memset(a.code, 0, sizeof(a.code));
            for (size_t i = 0; i < c; i++) {
                a.code[i] = (char)('0' + (r + i) % 10u);
            }
            TEST_ASSERT_TRUE(plink_enc_dev_act(&a, OUT, sizeof(OUT), &n));
            TEST_ASSERT_EQUAL_size_t(PLINK_DEV_ACT_LEN, n);
            TEST_ASSERT_TRUE(plink_dec_dev_act(OUT, n, &b));
            TEST_ASSERT_EQUAL_STRING(a.ref, b.ref);
            TEST_ASSERT_EQUAL_STRING(a.code, b.code);
        }
    }
    /* A ref of 96 bytes (no end within the array), a control byte, a bidi override, a byte that
     * isn't UTF-8; a code with a letter: refused, nothing written. */
    golden_dev_act(&a);
    memset(a.ref, 'r', sizeof(a.ref));
    TEST_ASSERT_FALSE(plink_enc_dev_act(&a, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    static const char *const bad_refs[] = {"lock.a\x01" "b", "lock.\xE2\x80\xAE" "x",
                                           "lock.\xC0\xAF", "lock.\x7F"};
    for (size_t i = 0; i < sizeof(bad_refs) / sizeof(bad_refs[0]); i++) {
        golden_dev_act(&a);
        strcpy(a.ref, bad_refs[i]);
        TEST_ASSERT_FALSE(plink_enc_dev_act(&a, OUT, sizeof(OUT), &n));
    }
    golden_dev_act(&a);
    strcpy(a.code, "12a4");
    TEST_ASSERT_FALSE(plink_enc_dev_act(&a, OUT, sizeof(OUT), &n));
    TEST_ASSERT_FALSE(plink_enc_dev_act(NULL, OUT, sizeof(OUT), &n));

    uint8_t buf[sizeof(G_DEV_ACT)];
    memcpy(buf, G_DEV_ACT, sizeof(buf));
    buf[12] = 96; /* a ref longer than 95 */
    TEST_ASSERT_FALSE(plink_dec_dev_act(buf, sizeof(buf), &b));
    TEST_ASSERT_EQUAL_STRING("", b.code); /* nothing of it kept */
    memcpy(buf, G_DEV_ACT, sizeof(buf));
    buf[17] = 0x00; /* a NUL inside the ref's length */
    TEST_ASSERT_FALSE(plink_dec_dev_act(buf, sizeof(buf), &b));
    buf[17] = 0x1B; /* ESC */
    TEST_ASSERT_FALSE(plink_dec_dev_act(buf, sizeof(buf), &b));
    memcpy(buf, G_DEV_ACT, sizeof(buf));
    buf[110] = 'x'; /* the code "1x34" */
    TEST_ASSERT_FALSE(plink_dec_dev_act(buf, sizeof(buf), &b));
    memcpy(buf, G_DEV_ACT, sizeof(buf));
    buf[50] = 0xEE; /* in the ref's padding */
    buf[115] = 0xEE; /* in the code's */
    TEST_ASSERT_TRUE(plink_dec_dev_act(buf, sizeof(buf), &b));
    TEST_ASSERT_EQUAL_STRING("lock.front_door", b.ref);
    TEST_ASSERT_EQUAL_STRING("1234", b.code);
}

/* LINK-047: the answer cache's digest covers the act section and the padded ref, never the code
 * nor the request id (the known answer from Python 'cryptography', vectors_plink_msg.py). */
static void test_dev_act_digest(void)
{
    plink_dev_act_t a;
    uint8_t d[PLINK_SHA256_LEN], d2[PLINK_SHA256_LEN];
    golden_dev_act(&a);
    plink_dev_act_digest(&a, d);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DIGEST_DEV_ACT, d, PLINK_SHA256_LEN);
    a.req = 99;
    strcpy(a.code, "999999");
    plink_dev_act_digest(&a, d2);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DIGEST_DEV_ACT, d2, PLINK_SHA256_LEN);
    for (int field = 0; field < 5; field++) {
        golden_dev_act(&a);
        switch (field) {
        case 0:
            a.action = 4; /* ACT_LOCK */
            break;
        case 1:
            a.value = -1;
            break;
        case 2:
            a.mode = PLINK_DEV_MODE_COOL;
            break;
        case 3:
            a.flags = 1;
            break;
        default:
            strcpy(a.ref, "lock.back_door");
            break;
        }
        plink_dev_act_digest(&a, d2);
        TEST_ASSERT_FALSE(memcmp(DIGEST_DEV_ACT, d2, PLINK_SHA256_LEN) == 0);
    }
}

/* LINK-043: every new payload but FW_PAGE and DEV_LIST_PAGE fits a 250-byte frame
 * (PLINK_PAYLOAD_SMALL) at its longest, each exactly its PLINK_*_MAX: every string at its
 * longest, every refusal and every v1 settings field; DEV_STATE with four records at their
 * longest (25 in a UDP frame). */
static void test_longest_payloads(void)
{
    size_t n = 0;
    plink_fw_info_t fi;
    memset(&fi, 0xFF, sizeof(fi));
    fill_text(fi.device, PLINK_FW_DEVICE_MAX);
    fill_text(fi.board_rev, PLINK_FW_BOARD_MAX);
    fill_text(fi.chip_rev, PLINK_FW_CHIP_MAX);
    fill_text(fi.project, PLINK_FW_PROJECT_MAX);
    fill_text(fi.version, PLINK_FW_VERSION_MAX);
    fill_text(fi.cp_version, PLINK_FW_CP_MAX);
    TEST_ASSERT_TRUE(plink_enc_fw_info(&fi, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_FW_INFO_MAX, n);

    plink_fw_order_t fo;
    golden_fw_order(&fo);
    fill_text(fo.version, PLINK_FW_VERSION_MAX);
    fill_text(fo.server, PLINK_FW_SERVER_MAX);
    fill_text(fo.channel, PLINK_FW_CHANNEL_MAX);
    TEST_ASSERT_TRUE(plink_enc_fw_order(&fo, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_FW_ORDER_MAX, n);

    plink_fw_state_t fs;
    golden_fw_state(&fs);
    fill_text(fs.target, PLINK_FW_VERSION_MAX);
    fill_text(fs.last_version, PLINK_FW_VERSION_MAX);
    fill_text(fs.offered, PLINK_FW_VERSION_MAX);
    TEST_ASSERT_TRUE(plink_enc_fw_state(&fs, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_FW_STATE_MAX, n);

    const plink_fw_req_t fr = {.offset = 1};
    TEST_ASSERT_TRUE(plink_enc_fw_req(&fr, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_FW_REQ_LEN, n);

    plink_dev_rec_t rec;
    rec_heat(&rec);
    fill_text(rec.word, PLINK_DEV_WORD_MAX);
    TEST_ASSERT_TRUE(plink_enc_dev_rec(&rec, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_DEV_REC_MAX, n);
    static uint8_t items[26u * PLINK_DEV_ITEM_MAX];
    for (uint8_t i = 0; i < 26u; i++) {
        TEST_ASSERT_TRUE(plink_enc_dev_item(i, &rec, items + (size_t)i * PLINK_DEV_ITEM_MAX,
                                            PLINK_DEV_ITEM_MAX, &n));
        TEST_ASSERT_EQUAL_size_t(PLINK_DEV_ITEM_MAX, n);
    }
    plink_dev_state_t ds = {.list_crc = 1, .states_crc = 2, .count = 4, .items = items,
                            .items_len = 4u * PLINK_DEV_ITEM_MAX};
    TEST_ASSERT_TRUE(plink_enc_dev_state(&ds, OUT, PLINK_PAYLOAD_SMALL, &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_DEV_STATE_HEAD + 4u * PLINK_DEV_ITEM_MAX, n);
    ds.count = 25;
    ds.items_len = 25u * PLINK_DEV_ITEM_MAX;
    TEST_ASSERT_TRUE(plink_enc_dev_state(&ds, OUT, PLINK_PAYLOAD_MAX, &n));
    ds.count = 26;
    ds.items_len = 26u * PLINK_DEV_ITEM_MAX;
    TEST_ASSERT_FALSE(plink_enc_dev_state(&ds, OUT, PLINK_PAYLOAD_MAX, &n));

    const plink_dev_state_req_t sr = {.count = 1};
    TEST_ASSERT_TRUE(plink_enc_dev_state_req(&sr, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_DEV_STATE_REQ_LEN, n);

    plink_dev_act_t act;
    golden_dev_act(&act);
    fill_text(act.ref, PLINK_REF_MAX);
    strcpy(act.code, "0123456789");
    TEST_ASSERT_TRUE(plink_enc_dev_act(&act, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_DEV_ACT_LEN, n);

    plink_dev_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.has_rec = true;
    ack.rec = rec;
    TEST_ASSERT_TRUE(plink_enc_dev_ack(&ack, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_DEV_ACK_MAX, n);

    plink_dev_cat_t cat;
    memset(&cat, 0, sizeof(cat));
    fill_text(cat.ref, PLINK_REF_MAX);
    fill_text(cat.name, PLINK_NAME_MAX);
    fill_text(cat.area, PLINK_AREA_MAX);
    TEST_ASSERT_TRUE(plink_enc_dev_cat(&cat, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_DEV_CAT_MAX, n);

    uint8_t fields[80], refused[38];
    const size_t flen = v1_fields(fields);
    TEST_ASSERT_EQUAL_size_t(PROTO_PREFS_ENC_MAX, flen);
    proto_prefs_decoded_t d;
    TEST_ASSERT_TRUE(proto_prefs_decode(fields, flen, PROTO_PREFS_KNOWN, &d));
    TEST_ASSERT_EQUAL_HEX32(PROTO_PREFS_KNOWN, d.values.have | d.refused); /* each one known */
    for (uint8_t i = 0; i < 19u; i++) {
        refused[2u * i] = (uint8_t)(i + 1u);
        refused[2u * i + 1u] = PLINK_PREFS_BUSY;
    }
    plink_prefs_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.panel_id, "a1b2c3");
    p.refused_count = 19;
    p.refused = refused;
    p.fields = fields;
    p.fields_len = flen;
    TEST_ASSERT_TRUE(plink_enc_prefs(&p, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_PREFS_MAX, n);
    const plink_prefs_set_t ps = {.req = 1, .origin = PLINK_ORIGIN_CONSOLE, .fields = fields,
                                  .fields_len = flen};
    TEST_ASSERT_TRUE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_PREFS_SET_MAX, n);
    const plink_prefs_req_t pr = {.have_crc = 1};
    TEST_ASSERT_TRUE(plink_enc_prefs_req(&pr, OUT, sizeof(OUT), &n));
    TEST_ASSERT_TRUE(n <= PLINK_PAYLOAD_SMALL);
    const plink_dev_list_req_t lr = {.cfg_crc = 1};
    TEST_ASSERT_TRUE(plink_enc_dev_list_req(&lr, OUT, sizeof(OUT), &n));
    TEST_ASSERT_TRUE(n <= PLINK_PAYLOAD_SMALL);

    plink_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));
    fill_text(hb.fw, PLINK_FW_MAX);
    hb.has_prefs = true;
    TEST_ASSERT_TRUE(plink_enc_heartbeat(&hb, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(45, n);

    const size_t longest[] = {PLINK_FW_INFO_MAX, PLINK_FW_ORDER_MAX, PLINK_FW_STATE_MAX,
                              PLINK_FW_REQ_LEN, PLINK_DEV_STATE_HEAD + 4u * PLINK_DEV_ITEM_MAX,
                              PLINK_DEV_STATE_REQ_LEN, PLINK_DEV_ACT_LEN, PLINK_DEV_ACK_MAX,
                              PLINK_PREFS_MAX, PLINK_PREFS_SET_MAX, 45u};
    for (size_t i = 0; i < sizeof(longest) / sizeof(longest[0]); i++) {
        TEST_ASSERT_TRUE(longest[i] <= PLINK_PAYLOAD_SMALL);
    }
    TEST_ASSERT_EQUAL_size_t(225, PLINK_PAYLOAD_SMALL);

    /* The pages fill the frame they go in. */
    static uint8_t big[PLINK_PAYLOAD_SMALL];
    const plink_fw_page_t fp = {.total = sizeof(big), .data = big,
                                .len = (uint16_t)(PLINK_PAYLOAD_SMALL - PLINK_FW_PAGE_HDR)};
    TEST_ASSERT_TRUE(plink_enc_fw_page(&fp, OUT, PLINK_PAYLOAD_SMALL, &n));
    TEST_ASSERT_EQUAL_size_t(PLINK_PAYLOAD_SMALL, n);
}

/* k bytes of EXTRA appended to the section whose length byte is at g[at]; the sections around it
 * (their length bytes at outer[0..n_outer-1]) grow with it. Returns the new length. */
static const uint8_t EXTRA[16] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,
                                  0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0};

static size_t grow(uint8_t *out, const uint8_t *g, size_t g_len, size_t at, size_t k,
                   const size_t *outer, size_t n_outer)
{
    const size_t n = splice(out, g, g_len, at + 1u + g[at], EXTRA, k);
    out[at] = (uint8_t)(g[at] + k);
    for (size_t i = 0; i < n_outer; i++) {
        out[outer[i]] = (uint8_t)(g[outer[i]] + k);
    }
    return n;
}

/* LINK-017: every section of the new messages takes fields a newer firmware appends - each read
 * as far as this one knows, the rest skipped - and so do the device records inside DEV_STATE and
 * DEV_ACK and HEARTBEAT's section. A fields body and DEV_STATE's records keep what was appended,
 * as sent (their CRCs cover it). */
static void test_new_sections_grow(void)
{
    static const struct {
        const char *codec;
        size_t at[4];
        size_t count;
        size_t outer; /* 0: none; else the outer section's length byte + 1 */
    } S[] = {
        {"FW_INFO", {0}, 1, 0},
        {"FW_ORDER", {0, 81, 112}, 3, 0},
        {"FW_STATE", {0, 31, 41}, 3, 0},
        {"FW_REQ", {0}, 1, 0},
        {"DEV_STATE", {0}, 1, 0},
        {"DEV_ACT", {4}, 1, 0},
        {"DEV_ACT mode", {4}, 1, 0},
        {"DEV_ACK rec", {6}, 1, 0},
        {"DEV_ACK rec", {12}, 1, 7},
        {"PREFS", {0, 5, 18}, 3, 0},
        {"HEARTBEAT prefs", {14}, 1, 0},
        {"dev record", {0}, 1, 0},
        {"catalog record", {0}, 1, 0},
    };
    uint8_t buf[PLINK_FRAME_MAX];
    for (size_t i = 0; i < sizeof(S) / sizeof(S[0]); i++) {
        const codec_t *c = codec(S[i].codec);
        for (size_t j = 0; j < S[i].count; j++) {
            for (size_t k = 1; k <= sizeof(EXTRA); k += 7u) {
                const size_t outer = S[i].outer - 1u;
                const size_t n = grow(buf, c->golden, c->golden_len, S[i].at[j], k, &outer,
                                      S[i].outer != 0u ? 1u : 0u);
                expect_canonical(c, buf, n, c->golden, c->golden_len);
            }
        }
    }
    /* DEV_STATE's records grown: kept as sent (the keypad's CRC), read as the golden's. */
    plink_dev_rec_t want[2], got;
    rec_light(&want[0]);
    rec_heat(&want[1]);
    uint8_t buf2[PLINK_FRAME_MAX];
    size_t n = grow(buf2, G_DEV_STATE, sizeof(G_DEV_STATE), 11, 5, NULL, 0);
    n = grow(buf, buf2, n, 11u + 1u + 22u + 5u + 1u, 9, NULL, 0);
    plink_dev_state_t ds;
    TEST_ASSERT_TRUE(plink_dec_dev_state(buf, n, &ds));
    TEST_ASSERT_EQUAL_size_t(n - PLINK_DEV_STATE_HEAD, ds.items_len);
    size_t pos = 0, item_len = 0;
    uint8_t index = 0;
    const uint8_t *item = NULL;
    for (size_t i = 0; i < 2u; i++) {
        TEST_ASSERT_TRUE(plink_dev_state_next(&ds, &pos, &index, &got, &item, &item_len));
        TEST_ASSERT_EQUAL_MEMORY(&want[i], &got, sizeof(got));
        TEST_ASSERT_EQUAL_size_t(i == 0u ? 24u + 5u : 26u + 9u, item_len);
    }
    TEST_ASSERT_FALSE(plink_dev_state_next(&ds, &pos, &index, &got, &item, &item_len));
    /* ... and bytes after the count records are skipped. */
    n = splice(buf2, G_DEV_STATE, sizeof(G_DEV_STATE), sizeof(G_DEV_STATE), EXTRA, 7);
    expect_canonical(codec("DEV_STATE"), buf2, n, G_DEV_STATE, sizeof(G_DEV_STATE));

    /* The fields sections grown: the body kept whole, appended bytes and all (prefs_crc covers
     * them), its fields still read; encoded again exactly as it came. */
    static const struct {
        const char *codec;
        size_t at; /* the fields section's length byte */
    } F[] = {{"PREFS", 22}, {"PREFS_SET", 5}};
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
        const codec_t *c = codec(F[i].codec);
        for (size_t k = 1; k <= sizeof(EXTRA); k += 7u) {
            n = grow(buf, c->golden, c->golden_len, F[i].at, k, NULL, 0);
            expect_canonical(c, buf, n, buf, n);
            const plink_prefs_t *p = &M1.prefs;
            const plink_prefs_set_t *ps = &M1.prefs_set;
            const uint8_t *body = i == 0u ? p->fields : ps->fields;
            const size_t body_len = i == 0u ? p->fields_len : ps->fields_len;
            TEST_ASSERT_EQUAL_PTR(buf + F[i].at + 1u, body);
            TEST_ASSERT_EQUAL_size_t(c->golden[F[i].at] + k, body_len);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(EXTRA, body + c->golden[F[i].at], k);
            proto_prefs_decoded_t d;
            TEST_ASSERT_TRUE(proto_prefs_decode(body, body_len, PROTO_PREFS_KNOWN, &d));
            TEST_ASSERT_EQUAL_HEX32(i == 0u ? 0x25u : 0x401u, d.values.have); /* 1, 3, 6; 1, 11 */
        }
    }
}

/* LINK-017: an older firmware's shorter sections read the fields they lack as 0 (FW_ORDER's
 * relay section empty, FW_STATE's probe section with only asked; a record with only its flags;
 * PREFS's info section with only the panel id). */
static void test_new_short_sections(void)
{
    uint8_t buf[PLINK_FRAME_MAX];
    size_t n = cut(buf, G_FW_ORDER, sizeof(G_FW_ORDER), 113, 5);
    buf[112] = 0;
    plink_fw_order_t fo;
    TEST_ASSERT_TRUE(plink_dec_fw_order(buf, n, &fo));
    TEST_ASSERT_EQUAL_STRING("stable", fo.channel);
    TEST_ASSERT_EQUAL_UINT16(0, fo.page);
    TEST_ASSERT_EQUAL_UINT8(0, fo.count);
    TEST_ASSERT_EQUAL_UINT16(0, fo.gap_ms);

    n = cut(buf, G_FW_STATE, sizeof(G_FW_STATE), 43, 20);
    buf[41] = 1;
    plink_fw_state_t fs;
    TEST_ASSERT_TRUE(plink_dec_fw_state(buf, n, &fs));
    TEST_ASSERT_EQUAL_UINT8(1, fs.asked);
    TEST_ASSERT_EQUAL_UINT8(0, fs.probe_why);
    TEST_ASSERT_EQUAL_STRING("", fs.offered);
    TEST_ASSERT_EQUAL_STRING("1.3.9", fs.last_version);

    static const uint8_t flags_only[] = {0x01, PLINK_DEVF_ONLINE};
    plink_dev_rec_t rec;
    size_t used = 0;
    TEST_ASSERT_TRUE(plink_dec_dev_rec(flags_only, sizeof(flags_only), &rec, &used));
    TEST_ASSERT_EQUAL_size_t(2, used);
    TEST_ASSERT_EQUAL_HEX8(PLINK_DEVF_ONLINE, rec.flags);
    TEST_ASSERT_EQUAL_INT32(0, rec.temp);
    TEST_ASSERT_EQUAL_STRING("", rec.word);

    n = cut(buf, G_PREFS, sizeof(G_PREFS), 13, 5);
    buf[5] = 7;
    plink_prefs_t p;
    TEST_ASSERT_TRUE(plink_dec_prefs(buf, n, &p));
    TEST_ASSERT_EQUAL_STRING("a1b2c3", p.panel_id);
    TEST_ASSERT_EQUAL_UINT8(PLINK_VOICE_UNKNOWN, p.voice);
    TEST_ASSERT_EQUAL_HEX32(0, p.accepts);
    TEST_ASSERT_EQUAL_UINT8(3, p.field_count);
}

/* LINK-017, LINK-043: what the new decoders refuse beyond a payload cut short (test_truncated_
 * refused): an order or a request of 0, TLVs or refusals that run past their section, a page
 * outside its image, fewer records than DEV_STATE's count or one running past the payload; and
 * what the encoders refuse to send. A catalog record whose ref isn't clean is refused but still
 * says how long it is, so a list can step over it. */
static void test_new_refusals(void)
{
    uint8_t buf[PLINK_FRAME_MAX];
    size_t n = 0;
    plink_fw_order_t fo;
    golden_fw_order(&fo);
    fo.order = 0;
    TEST_ASSERT_FALSE(plink_enc_fw_order(&fo, OUT, sizeof(OUT), &n));
    memcpy(buf, G_FW_ORDER, sizeof(G_FW_ORDER));
    memset(buf + 1, 0, 4);
    TEST_ASSERT_FALSE(plink_dec_fw_order(buf, sizeof(G_FW_ORDER), &fo));

    plink_prefs_set_t ps;
    TEST_ASSERT_TRUE(plink_dec_prefs_set(G_PREFS_SET, sizeof(G_PREFS_SET), &ps));
    ps.req = 0;
    TEST_ASSERT_FALSE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));
    memcpy(buf, G_PREFS_SET, sizeof(G_PREFS_SET));
    memset(buf, 0, 4);
    TEST_ASSERT_FALSE(plink_dec_prefs_set(buf, sizeof(G_PREFS_SET), &ps));
    memcpy(buf, G_PREFS_SET, sizeof(G_PREFS_SET));
    buf[6] = 3; /* three TLVs, two there */
    TEST_ASSERT_FALSE(plink_dec_prefs_set(buf, sizeof(G_PREFS_SET), &ps));
    memcpy(buf, G_PREFS_SET, sizeof(G_PREFS_SET));
    buf[11] = 2; /* the last TLV runs past the section */
    TEST_ASSERT_FALSE(plink_dec_prefs_set(buf, sizeof(G_PREFS_SET), &ps));
    /* The encoder: a body framed as the decoder reads it - its count's TLVs whole within it, then
     * whatever follows them (a later version's, kept) - and no longer than a section. */
    static const uint8_t two[] = {0x02, 0x01, 0x01, 0x28, 0x0B, 0x01, 0x02, 0xEE};
    static const uint8_t three[] = {0x03, 0x01, 0x01, 0x28, 0x0B, 0x01, 0x02};
    ps = (plink_prefs_set_t){.req = 1, .fields = two, .fields_len = 7};
    TEST_ASSERT_TRUE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));
    ps.fields_len = 8;
    TEST_ASSERT_TRUE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(6u + 8u, n);
    for (size_t len = 1; len < 7u; len++) { /* a TLV cut */
        ps.fields_len = len;
        TEST_ASSERT_FALSE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));
        TEST_ASSERT_EQUAL_size_t(0, n);
    }
    ps.fields = three;
    ps.fields_len = sizeof(three);
    TEST_ASSERT_FALSE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));
    ps.fields = NULL;
    ps.fields_len = 1;
    TEST_ASSERT_FALSE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));
    ps.fields_len = 0; /* an empty body: an empty section */
    TEST_ASSERT_TRUE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));
    TEST_ASSERT_EQUAL_size_t(6u, n);
    static uint8_t big[256];
    ps.fields = big; /* count 0, then 255 or 256 bytes after it */
    ps.fields_len = 255;
    TEST_ASSERT_TRUE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));
    ps.fields_len = 256;
    TEST_ASSERT_FALSE(plink_enc_prefs_set(&ps, OUT, sizeof(OUT), &n));

    plink_prefs_t p;
    memcpy(buf, G_PREFS, sizeof(G_PREFS));
    buf[19] = 2; /* two refusals, one there */
    TEST_ASSERT_FALSE(plink_dec_prefs(buf, sizeof(G_PREFS), &p));
    memcpy(buf, G_PREFS, sizeof(G_PREFS));
    buf[23] = 4; /* four fields, three there */
    TEST_ASSERT_FALSE(plink_dec_prefs(buf, sizeof(G_PREFS), &p));
    memset(&p, 0, sizeof(p));
    p.refused_count = 1;
    TEST_ASSERT_FALSE(plink_enc_prefs(&p, OUT, sizeof(OUT), &n));
    memset(&p, 0, sizeof(p));
    p.fields = three;
    p.fields_len = sizeof(three);
    TEST_ASSERT_FALSE(plink_enc_prefs(&p, OUT, sizeof(OUT), &n));

    plink_fw_page_t fp;
    memcpy(buf, G_FW_PAGE, sizeof(G_FW_PAGE));
    buf[12] = 0x7E; /* offset 3600000 - 2 + 4 > total */
    buf[13] = 0xEE;
    buf[14] = 0x36;
    buf[15] = 0x00;
    TEST_ASSERT_FALSE(plink_dec_fw_page(buf, sizeof(G_FW_PAGE), &fp));
    memset(buf + 12, 0xFF, 4); /* an offset near 2^32 doesn't wrap */
    TEST_ASSERT_FALSE(plink_dec_fw_page(buf, sizeof(G_FW_PAGE), &fp));
    TEST_ASSERT_TRUE(plink_dec_fw_page(G_FW_PAGE, sizeof(G_FW_PAGE), &fp));
    fp.offset = fp.total - 3u;
    TEST_ASSERT_FALSE(plink_enc_fw_page(&fp, OUT, sizeof(OUT), &n));

    plink_dev_state_t ds;
    memcpy(buf, G_DEV_STATE, sizeof(G_DEV_STATE));
    buf[9] = 3; /* three records, two there */
    TEST_ASSERT_FALSE(plink_dec_dev_state(buf, sizeof(G_DEV_STATE), &ds));
    memcpy(buf, G_DEV_STATE, sizeof(G_DEV_STATE));
    buf[35] = 25; /* the last record one byte longer than the payload */
    TEST_ASSERT_FALSE(plink_dec_dev_state(buf, sizeof(G_DEV_STATE), &ds));
    /* The encoder: exactly count whole records. */
    TEST_ASSERT_TRUE(plink_dec_dev_state(G_DEV_STATE, sizeof(G_DEV_STATE), &ds));
    ds.count = 1;
    TEST_ASSERT_FALSE(plink_enc_dev_state(&ds, OUT, sizeof(OUT), &n));
    ds.count = 3;
    TEST_ASSERT_FALSE(plink_enc_dev_state(&ds, OUT, sizeof(OUT), &n));
    ds.count = 2;
    ds.items_len--;
    TEST_ASSERT_FALSE(plink_enc_dev_state(&ds, OUT, sizeof(OUT), &n));
    ds.items = NULL;
    TEST_ASSERT_FALSE(plink_enc_dev_state(&ds, OUT, sizeof(OUT), &n));

    plink_dev_rec_t rec;
    size_t used = 99;
    TEST_ASSERT_FALSE(plink_dec_dev_rec(G_DEV_REC, sizeof(G_DEV_REC) - 1u, &rec, &used));
    TEST_ASSERT_EQUAL_size_t(0, used);

    plink_dev_cat_t cat;
    memcpy(buf, G_DEV_CAT, sizeof(G_DEV_CAT));
    buf[8] = 0x01; /* a control byte in the ref */
    TEST_ASSERT_FALSE(plink_dec_dev_cat(buf, sizeof(G_DEV_CAT), &cat, &used));
    TEST_ASSERT_EQUAL_size_t(sizeof(G_DEV_CAT), used);
    TEST_ASSERT_EQUAL_STRING("", cat.ref);
    TEST_ASSERT_EQUAL_STRING("", cat.name);
    memcpy(buf, G_DEV_CAT, sizeof(G_DEV_CAT));
    buf[3] = 40; /* the ref runs past its section */
    TEST_ASSERT_FALSE(plink_dec_dev_cat(buf, sizeof(G_DEV_CAT), &cat, &used));
    TEST_ASSERT_EQUAL_size_t(sizeof(G_DEV_CAT), used);
    TEST_ASSERT_FALSE(plink_dec_dev_cat(G_DEV_CAT, sizeof(G_DEV_CAT) - 1u, &cat, &used));
    TEST_ASSERT_EQUAL_size_t(0, used);
    TEST_ASSERT_TRUE(plink_dec_dev_cat(G_DEV_CAT, sizeof(G_DEV_CAT), &cat, NULL));
    strcpy(cat.ref, "lock.\x1B");
    TEST_ASSERT_FALSE(plink_enc_dev_cat(&cat, OUT, sizeof(OUT), &n));
    memset(cat.ref, 'r', sizeof(cat.ref));
    TEST_ASSERT_FALSE(plink_enc_dev_cat(&cat, OUT, sizeof(OUT), &n));
}

/* LINK-017: random values of every field come back as they went, through the update, device and
 * settings messages. */
static void test_new_random_round_trips(void)
{
    RNG = 0xF00Du;
    for (int iter = 0; iter < 1000; iter++) {
        plink_fw_order_t fo, fo2;
        memset(&fo, 0, sizeof(fo));
        fo.order = rnd32() | 1u;
        fo.op = rnd8();
        fo.kind = rnd8();
        rnd_text(fo.version, PLINK_FW_VERSION_MAX);
        fo.size = rnd32();
        rnd_bytes(fo.sha256, sizeof(fo.sha256));
        rnd_bytes(fo.key, sizeof(fo.key));
        rnd_text(fo.server, PLINK_FW_SERVER_MAX);
        rnd_text(fo.channel, PLINK_FW_CHANNEL_MAX);
        fo.page = rnd16();
        fo.count = rnd8();
        fo.gap_ms = rnd16();
        size_t n = 0;
        TEST_ASSERT_TRUE(plink_enc_fw_order(&fo, OUT, sizeof(OUT), &n));
        TEST_ASSERT_TRUE(n <= PLINK_FW_ORDER_MAX);
        TEST_ASSERT_TRUE(plink_dec_fw_order(OUT, n, &fo2));
        TEST_ASSERT_EQUAL_MEMORY(&fo, &fo2, sizeof(fo));

        plink_fw_state_t fs, fs2;
        memset(&fs, 0, sizeof(fs));
        fs.order = rnd32();
        fs.op = rnd8();
        fs.state = rnd8();
        fs.flags = rnd8();
        fs.why = rnd8();
        fs.bytes = rnd32();
        fs.size = rnd32();
        rnd_bytes(fs.id8, sizeof(fs.id8));
        rnd_text(fs.target, PLINK_FW_VERSION_MAX);
        fs.probation = rnd8();
        fs.last = rnd8();
        rnd_text(fs.last_version, PLINK_FW_VERSION_MAX);
        fs.failed_mask = rnd8();
        fs.asked = rnd8();
        fs.probe_why = rnd8();
        fs.age_s = rnd32();
        rnd_text(fs.offered, PLINK_FW_VERSION_MAX);
        rnd_bytes(fs.offered_key8, sizeof(fs.offered_key8));
        fs.key_trusted = rnd8();
        TEST_ASSERT_TRUE(plink_enc_fw_state(&fs, OUT, sizeof(OUT), &n));
        TEST_ASSERT_TRUE(plink_dec_fw_state(OUT, n, &fs2));
        TEST_ASSERT_EQUAL_MEMORY(&fs, &fs2, sizeof(fs));

        plink_dev_act_t a, a2;
        memset(&a, 0, sizeof(a));
        a.req = rnd32();
        a.action = rnd8();
        a.value = (int32_t)(rnd32() & 0x7FFFFFFFu) - (int32_t)(rnd32() & 0x7FFFFFFFu);
        a.mode = rnd8();
        a.flags = rnd8();
        rnd_text(a.ref, PLINK_REF_MAX);
        rnd_code(a.code);
        TEST_ASSERT_TRUE(plink_enc_dev_act(&a, OUT, sizeof(OUT), &n));
        TEST_ASSERT_EQUAL_size_t(PLINK_DEV_ACT_LEN, n);
        TEST_ASSERT_TRUE(plink_dec_dev_act(OUT, n, &a2));
        TEST_ASSERT_EQUAL_MEMORY(&a, &a2, sizeof(a));

        plink_dev_ack_t k, k2;
        memset(&k, 0, sizeof(k));
        k.req = rnd32();
        k.result = rnd8();
        k.why = rnd8();
        k.has_rec = (rnd8() & 1u) != 0u;
        if (k.has_rec) {
            k.list_crc = rnd32();
            k.index = rnd8();
            k.rec.flags = rnd8();
            k.rec.action = rnd8();
            k.rec.vflags = rnd8();
            k.rec.level = rnd8();
            k.rec.modes = rnd8();
            k.rec.hvac_action = rnd8();
            k.rec.setpoint = (int32_t)rnd16() - 30000;
            k.rec.temp = (int32_t)rnd32();
            k.rec.since = rnd32();
            k.rec.caps = rnd8();
            rnd_text(k.rec.word, PLINK_DEV_WORD_MAX);
        }
        TEST_ASSERT_TRUE(plink_enc_dev_ack(&k, OUT, sizeof(OUT), &n));
        TEST_ASSERT_TRUE(plink_dec_dev_ack(OUT, n, &k2));
        TEST_ASSERT_EQUAL_UINT32(k.req, k2.req);
        TEST_ASSERT_EQUAL_UINT8(k.result, k2.result);
        TEST_ASSERT_EQUAL_UINT8(k.why, k2.why);
        TEST_ASSERT_EQUAL(k.has_rec, k2.has_rec);
        TEST_ASSERT_EQUAL_HEX32(k.list_crc, k2.list_crc);
        TEST_ASSERT_EQUAL_UINT8(k.index, k2.index);
        TEST_ASSERT_EQUAL_MEMORY(&k.rec, &k2.rec, sizeof(k.rec));

        /* A DEV_STATE of random records, each read back with its index and bytes. */
        static uint8_t items[25u * PLINK_DEV_ITEM_MAX];
        static plink_dev_rec_t recs[25];
        const uint8_t count = (uint8_t)rnd_below(26);
        size_t len = 0;
        for (uint8_t i = 0; i < count; i++) {
            plink_dev_rec_t *r = &recs[i];
            memset(r, 0, sizeof(*r));
            r->flags = rnd8();
            r->setpoint = (int32_t)rnd32();
            r->since = rnd32();
            rnd_text(r->word, PLINK_DEV_WORD_MAX);
            size_t one = 0;
            TEST_ASSERT_TRUE(plink_enc_dev_item((uint8_t)(i * 2u), r, items + len,
                                                sizeof(items) - len, &one));
            len += one;
        }
        const plink_dev_state_t ds = {.list_crc = rnd32(), .states_crc = rnd32(), .count = count,
                                      .items = items, .items_len = len};
        TEST_ASSERT_TRUE(plink_enc_dev_state(&ds, OUT, PLINK_PAYLOAD_MAX, &n));
        plink_dev_state_t ds2;
        TEST_ASSERT_TRUE(plink_dec_dev_state(OUT, n, &ds2));
        TEST_ASSERT_EQUAL_HEX32(ds.list_crc, ds2.list_crc);
        TEST_ASSERT_EQUAL_HEX32(ds.states_crc, ds2.states_crc);
        TEST_ASSERT_EQUAL_UINT8(count, ds2.count);
        if (len > 0u) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(items, ds2.items, len);
        }
        size_t pos = 0;
        uint8_t index = 0;
        plink_dev_rec_t got;
        for (uint8_t i = 0; i < count; i++) {
            TEST_ASSERT_TRUE(plink_dev_state_next(&ds2, &pos, &index, &got, NULL, NULL));
            TEST_ASSERT_EQUAL_UINT8(i * 2u, index);
            TEST_ASSERT_EQUAL_MEMORY(&recs[i], &got, sizeof(got));
        }
        TEST_ASSERT_FALSE(plink_dev_state_next(&ds2, &pos, &index, &got, NULL, NULL));

        /* PREFS and PREFS_SET with random refusals and a random fields body: random TLVs, now and
         * then bytes after them, now and then no body at all. */
        uint8_t refused[2u * 40u], fields[200];
        const uint8_t nref = (uint8_t)rnd_below(41);
        rnd_bytes(refused, 2u * nref);
        uint8_t nf = 0;
        size_t flen = 1;
        while (flen < 150u && rnd_below(8) != 0u) {
            const uint8_t size = (uint8_t)rnd_below(6);
            fields[flen++] = rnd8();
            fields[flen++] = size;
            rnd_bytes(fields + flen, size);
            flen += size;
            nf++;
        }
        fields[0] = nf;
        if (rnd_below(4) == 0u) {
            const size_t after = 1u + rnd_below(20);
            rnd_bytes(fields + flen, after);
            flen += after;
        } else if (nf == 0u && rnd_below(2) == 0u) {
            flen = 0;
        }
        plink_prefs_t p, p2;
        memset(&p, 0, sizeof(p));
        p.req = rnd32();
        rnd_text(p.panel_id, PLINK_PANEL_ID_LEN);
        p.voice = rnd8();
        p.accepts = rnd32();
        p.refused_count = nref;
        p.refused = nref > 0u ? refused : NULL;
        p.fields = flen > 0u ? fields : NULL;
        p.fields_len = flen;
        TEST_ASSERT_TRUE(plink_enc_prefs(&p, OUT, sizeof(OUT), &n));
        TEST_ASSERT_TRUE(plink_dec_prefs(OUT, n, &p2));
        TEST_ASSERT_EQUAL_UINT32(p.req, p2.req);
        TEST_ASSERT_EQUAL_STRING(p.panel_id, p2.panel_id);
        TEST_ASSERT_EQUAL_UINT8(p.voice, p2.voice);
        TEST_ASSERT_EQUAL_HEX32(p.accepts, p2.accepts);
        TEST_ASSERT_EQUAL_UINT8(nref, p2.refused_count);
        if (nref > 0u) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(refused, p2.refused, 2u * nref);
        }
        TEST_ASSERT_EQUAL_UINT8(flen > 0u ? nf : 0u, p2.field_count);
        TEST_ASSERT_EQUAL_size_t(flen, p2.fields_len);
        if (flen > 0u) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(fields, p2.fields, flen);
        } else {
            TEST_ASSERT_NULL(p2.fields);
        }
        const uint32_t sreq = rnd32() | 1u;
        const uint8_t origin = rnd8();
        const plink_prefs_set_t s = {.req = sreq, .origin = origin, .fields = p.fields,
                                     .fields_len = flen};
        plink_prefs_set_t s2;
        TEST_ASSERT_TRUE(plink_enc_prefs_set(&s, OUT, sizeof(OUT), &n));
        TEST_ASSERT_TRUE(plink_dec_prefs_set(OUT, n, &s2));
        TEST_ASSERT_EQUAL_UINT32(s.req, s2.req);
        TEST_ASSERT_EQUAL_UINT8(s.origin, s2.origin);
        TEST_ASSERT_EQUAL_UINT8(flen > 0u ? nf : 0u, s2.field_count);
        TEST_ASSERT_EQUAL_size_t(flen, s2.fields_len);
        if (flen > 0u) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(fields, s2.fields, flen);
        }
    }
}

/* ---- the frozen v1 fixture's values ---------------------------------------------------------
 * What each payload of plink_wire_fixture_v1.h says, field by field from the documents' tables
 * (vectors_plink_msg.py's arguments), wire numbers as literals; a pointer is the offset of what it
 * names in the payload p. Frozen with the fixture: never edit; a decoder that reads a v1 layout
 * differently fails here even when its encoder writes the same bytes back. */

static void v1_pair_req(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_pair_req_t *m = &e->pair_req;
    m->attempt = 0x11223344u;
    m->proto = 1;
    m->caps = 0xA55Au;
    m->kind = 1; /* a panel */
    m->cap.partitions = 4;
    m->cap.zones = 128;
    m->cap.users = 64;
    m->cap.devices = 16;
    fill_seq(m->pk, PLINK_X25519_LEN, 0x00);
    strcpy(m->name, "Hall");
    strcpy(m->fw, "1.2.3");
}

static void v1_pair_commit(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_pair_commit_t *m = &e->pair_commit;
    m->attempt = 0x01020304u;
    m->proto = 1;
    m->caps = 0x0003u;
    fill_seq(m->pk, PLINK_X25519_LEN, 0x20);
    fill_seq(m->commit, PLINK_SHA256_LEN, 0x40);
    strcpy(m->name, "Harbor House");
    strcpy(m->panel_id, "a1b2c3");
}

static void v1_pair_nonce(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->pair_nonce.attempt = 0xDEADBEEFu;
    fill_seq(e->pair_nonce.n, PLINK_NONCE_LEN, 0x60);
}

static void v1_pair_reveal(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->pair_nonce.attempt = 0xDEADBEEFu;
    fill_seq(e->pair_nonce.n, PLINK_NONCE_LEN, 0x70);
}

static void v1_pair_confirm(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->pair_confirm.attempt = 7;
    fill_seq(e->pair_confirm.mac, PLINK_MAC_LEN, 0xA0);
}

static void v1_pair_done(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->pair_done.attempt = 7;
    e->pair_done.device = 3;
    e->pair_done.home = 2;
    fill_seq(e->pair_done.mac, PLINK_MAC_LEN, 0xB0);
}

static void v1_pair_fail(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->pair_fail.attempt = 0xABCDu;
    e->pair_fail.reason = 7; /* overlap */
}

static void v1_hello(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_hello_t *m = &e->hello;
    m->proto = 1;
    m->caps = 0x0102u;
    m->cap.partitions = 2;
    m->cap.zones = 320;
    m->cap.users = 32;
    m->cap.devices = 8;
    fill_seq(m->n, PLINK_NONCE_LEN, 0x10);
    fill_seq(m->e, PLINK_X25519_LEN, 0x30);
    fill_seq(m->mac, PLINK_MAC_LEN, 0xF0);
}

static void v1_welcome(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_hello_t *m = &e->hello;
    m->proto = 1;
    fill_seq(m->n, PLINK_NONCE_LEN, 0x10);
    fill_seq(m->e, PLINK_X25519_LEN, 0x30);
    fill_seq(m->mac, PLINK_MAC_LEN, 0xF0);
}

static void v1_rekey(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->rekey.sid = 0x89ABCDEFu;
}

static void v1_status(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_status_t *m = &e->status;
    m->epoch_ms = 0x0102030405060708ll;
    m->partitions = 0x05;
    m->home = 1;
    m->sys_troubles = 9;
    m->sys_restored = 2;
    m->dev_troubles = 4;
    m->default_codes = 3;
    m->flags = 0x03; /* program, exit sounds */
    m->recovery_left_ms = 120000;
    m->lockout_left_ms = 30000;
    m->report = 2; /* online */
    m->zones_used = 16;
    m->last_event_seq = 1234;
    m->cfg_crc = 0xCAFEBABEu;
    m->cfg_len = 0x1234;
    m->zones_crc = 0x12345678u;
    m->mono_ms = 0x112233445566ull;
    m->count = 2;
    plink_part_t *a = &m->part[0];
    a->number = 1;
    a->state = 3; /* entry delay */
    a->level = 1; /* away */
    a->arm_flags = 0x02;
    a->armed_by = 5;
    a->arm_src_kind = 1; /* keypad 3 */
    a->arm_src_id = 3;
    a->ready_levels = 0x0E;
    a->flags = 0x2D; /* ready, walk test, triggered, trouble_unack */
    a->sounder = 4;
    a->bell = 1;
    a->delay_left_ms = 31000;
    a->memory = 0x8002;
    a->faults = 2;
    a->bypassed = 1;
    a->alarms = 3;
    a->troubles = 4;
    a->walk_left_ms = 60000;
    a->walk_age_ms = 1000;
    a->walk_id = 0x00ABCDEFu;
    a->walk_seq = 0x11;
    a->walk_tested = 5;
    a->walk_total = 9;
    a->walk_flags = 1;
    a->walk_started_by = 2;
    a->walk_src_kind = 1; /* keypad 4 */
    a->walk_src_id = 4;
    plink_part_t *b = &m->part[1];
    b->number = 3;
    b->ready_levels = 0x0E;
    b->flags = 0x01; /* ready */
}

static void v1_zones(const uint8_t *p, any_msg_t *e)
{
    e->zones.zones_crc = 0x0BADF00Du;
    e->zones.first = 5;
    e->zones.count = 2;
    e->zones.rec_len = 7;
    e->zones.recs = p + 9;
}

static void v1_zones_req(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->zones_req.first = 1;
    e->zones_req.count = 128;
}

static void v1_cfg_req(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->cfg_req.cfg_crc = 0xCAFEBABEu;
    e->cfg_req.offset = 1024;
    e->cfg_req.max = 1024;
}

static void v1_cfg_page(const uint8_t *p, any_msg_t *e)
{
    e->cfg_page.cfg_crc = 0xCAFEBABEu;
    e->cfg_page.total = 16;
    e->cfg_page.offset = 12;
    e->cfg_page.data = p + 14;
    e->cfg_page.len = 4;
}

static void v1_ev_req(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->ev_req.after_seq = 1234;
    e->ev_req.max = 50;
}

static void v1_events(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    events_msg_t *m = &e->events;
    m->n = 2;
    m->flags = 0;
    alarm_event_t *a = &m->ev[0]; /* armed away by user 5 at keypad 3 */
    a->seq = 1233;
    a->mono_ms = 123456;
    a->epoch_ms = 0x1122334455667788ll;
    a->type = 1;
    a->partition = 1;
    a->user = 5;
    a->source.kind = 1;
    a->source.id = 3;
    a->arg = 1;
    a->arg2 = 2;
    a->flags = 1;
    alarm_event_t *b = &m->ev[1]; /* entry delay from zone 7 */
    b->seq = 1234;
    b->mono_ms = 123556;
    b->epoch_ms = -1;
    b->type = 6;
    b->partition = 1;
    b->zone = 7;
    b->source.kind = 3;
    b->source.id = 7;
}

static void v1_cmd(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_cmd_t *m = &e->cmd;
    m->req = 0x00C0FFEEu;
    m->op = 1;
    m->cmd.type = 0;  /* arm */
    m->cmd.partition = 2;
    m->cmd.level = 2; /* stay */
    m->cmd.arm_flags = 0x01;
    strcpy(m->cmd.code, "1234");
}

static void v1_cmd_change(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_cmd_t *m = &e->cmd;
    m->req = 42;
    m->op = 3;
    m->cmd.partition = 1;
    strcpy(m->cmd.code, "123456");
    strcpy(m->new_code, "9876543210");
}

static void v1_result(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->result.req = 0x00C0FFEEu;
    e->result.result = 6; /* not ready */
    e->result.user = 5;
    e->result.not_ready_zone = 258;
    e->result.may_disarm = 1;
}

static void v1_sound(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->sound.partition = 2;
    e->sound.sound = 12; /* chime */
    e->sound.zone = 10;
}

static void v1_heartbeat(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_heartbeat_t *m = &e->heartbeat;
    m->flags = 0x05;
    m->rssi = -61;
    m->uptime_s = 86400;
    m->status_age_s = 0xFFFF;
    strcpy(m->fw, "0.9.1");
}

static void v1_heartbeat_prefs(const uint8_t *p, any_msg_t *e)
{
    v1_heartbeat(p, e);
    e->heartbeat.has_prefs = true;
    e->heartbeat.prefs_crc = 0x0C0FFEE0u;
}

static void v1_time(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->time.epoch_ms = 1760000000000ll;
    strcpy(e->time.tz, "EST5EDT,M3.2.0,M11.1.0");
}

static void v1_bye(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->bye.reason = 1;
}

static void v1_fw_info(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_fw_info_t *m = &e->fw_info;
    m->can = 0x07; /* fetch, relay, key accept */
    strcpy(m->device, "crowpanel-7");
    strcpy(m->board_rev, "1.3");
    strcpy(m->chip_rev, "1.0");
    m->chip_rev_full = 100;
    strcpy(m->project, "roboalarms");
    strcpy(m->version, "1.4.0");
    m->slot = 0x00480000u;
    fill_seq(m->own_key8, PLINK_KEY8_LEN, 0xA0);
    strcpy(m->cp_version, "2.12.3");
}

static void v1_fw_order(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_fw_order_t *m = &e->fw_order;
    m->order = 0x01020304u;
    m->op = 4;   /* relay */
    m->kind = 0; /* the P4 application */
    strcpy(m->version, "1.4.0");
    m->size = 3600000u;
    fill_seq(m->sha256, PLINK_SHA256_LEN, 0x40);
    fill_seq(m->key, PLINK_FW_KEY_LEN, 0x80);
    strcpy(m->server, "https://fw.example.com");
    strcpy(m->channel, "stable");
    m->page = 1024;
    m->count = 16;
    m->gap_ms = 50;
}

static void v1_fw_state(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    plink_fw_state_t *m = &e->fw_state;
    m->order = 0x01020304u;
    m->op = 4;    /* relay */
    m->state = 3; /* receiving */
    m->flags = 1; /* paused */
    m->why = 54;  /* no status */
    m->bytes = 1048576u;
    m->size = 3600000u;
    fill_seq(m->id8, PLINK_ID8_LEN, 0x40);
    strcpy(m->target, "1.4.0");
    m->last = 2; /* rolled back */
    strcpy(m->last_version, "1.3.9");
    m->failed_mask = 0x05;
    m->asked = 1;
    m->probe_why = 13; /* another version */
    m->age_s = 3600;
    strcpy(m->offered, "1.5.0");
    fill_seq(m->offered_key8, PLINK_KEY8_LEN, 0xA0);
    m->key_trusted = 1;
}

static void v1_fw_req(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    fill_seq(e->fw_req.id8, PLINK_ID8_LEN, 0x40);
    e->fw_req.offset = 65536u;
    e->fw_req.page = 1024;
    e->fw_req.count = 16;
}

static void v1_fw_page(const uint8_t *p, any_msg_t *e)
{
    fill_seq(e->fw_page.id8, PLINK_ID8_LEN, 0x40);
    e->fw_page.total = 3600000u;
    e->fw_page.offset = 65536u;
    e->fw_page.data = p + 18;
    e->fw_page.len = 4;
}

static void v1_dev_list_req(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->dev_list_req.cfg_crc = 0x1BADB002u;
    e->dev_list_req.offset = 0;
    e->dev_list_req.max = 1024;
}

static void v1_dev_list_page(const uint8_t *p, any_msg_t *e)
{
    e->dev_list_page.cfg_crc = 0x1BADB002u;
    e->dev_list_page.total = 16;
    e->dev_list_page.offset = 12;
    e->dev_list_page.data = p + 14;
    e->dev_list_page.len = 4;
}

static void v1_dev_state(const uint8_t *p, any_msg_t *e)
{
    e->dev_state.list_crc = 0x1BADB002u;
    e->dev_state.states_crc = 0x5EED5EEDu;
    e->dev_state.count = 2;
    e->dev_state.items = p + 10;
    e->dev_state.items_len = 50;
}

static void v1_dev_state_req(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->dev_state_req.list_crc = 0x1BADB002u;
}

static void v1_dev_act(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->dev_act.req = 0x0BEEF001u;
    e->dev_act.action = 5; /* unlock */
    strcpy(e->dev_act.ref, "lock.front_door");
    strcpy(e->dev_act.code, "1234");
}

static void v1_dev_act_mode(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->dev_act.req = 0x0BEEF003u;
    e->dev_act.action = 11; /* set the mode */
    e->dev_act.mode = 0x02; /* heat */
    strcpy(e->dev_act.ref, "climate.hall");
}

static void v1_dev_ack(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->dev_ack.req = 0x0BEEF001u;
    e->dev_ack.result = 2; /* the code was refused */
    e->dev_ack.why = 3;
}

/* The three records of the fixture: a dimmed light, a thermostat, a lock. */
static void v1_rec(plink_dev_rec_t *r, int which)
{
    memset(r, 0, sizeof(*r));
    if (which == 0) {
        r->flags = 0x01; /* online */
        r->vflags = 0x09;
        r->level = 75;
        r->caps = 0x88;
        strcpy(r->word, "on");
    } else if (which == 1) {
        r->flags = 0x03; /* online, pending */
        r->action = 10;
        r->vflags = 0x06;
        r->modes = 0x0B;
        r->hvac_action = 2;
        r->setpoint = 2150;
        r->temp = -550;
        r->since = 1760000000u;
        r->caps = 0x80;
        strcpy(r->word, "heat");
    } else {
        r->flags = 0x03; /* online, pending */
        r->action = 5;
        strcpy(r->word, "locked");
    }
}

static void v1_dev_ack_rec(const uint8_t *p, any_msg_t *e)
{
    e->dev_ack.req = 0x0BEEF002u;
    e->dev_ack.has_rec = true;
    e->dev_ack.list_crc = 0x1BADB002u;
    e->dev_ack.index = 7;
    v1_rec(&e->dev_ack.rec, 2);
    e->dev_ack.rec_raw = p + 12;
    e->dev_ack.rec_raw_len = 27;
}

static void v1_prefs(const uint8_t *p, any_msg_t *e)
{
    e->prefs.req = 0x777;
    strcpy(e->prefs.panel_id, "a1b2c3");
    e->prefs.voice = 1;
    e->prefs.accepts = 0x0007FFFFu;
    e->prefs.refused_count = 1;
    e->prefs.refused = p + 20;
    e->prefs.field_count = 3;
    e->prefs.fields = p + 23; /* the body, its count first */
    e->prefs.fields_len = 14;
}

static void v1_prefs_req(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->prefs_req.have_crc = 0xFEEDF00Du;
}

static void v1_prefs_set(const uint8_t *p, any_msg_t *e)
{
    e->prefs_set.req = 0x777;
    e->prefs_set.origin = 1; /* Home Assistant */
    e->prefs_set.field_count = 2;
    e->prefs_set.fields = p + 6; /* the body, its count first */
    e->prefs_set.fields_len = 7;
}

static void v1_dev_rec(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    v1_rec(&e->dev_rec, 1);
}

static void v1_dev_cat(const uint8_t *p, any_msg_t *e)
{
    (void)p;
    e->dev_cat.kind = 3;   /* a lock */
    e->dev_cat.source = 1; /* Home Assistant */
    strcpy(e->dev_cat.ref, "lock.front_door");
    strcpy(e->dev_cat.name, "Front door");
    strcpy(e->dev_cat.area, "Hall");
}

typedef void (*v1_fn)(const uint8_t *p, any_msg_t *e);

/* Each fixture row's values, and how many bytes of its union member they fill. */
static const struct {
    const char *codec;
    v1_fn want;
    size_t size;
} V1_VALUES[] = {
    {"PAIR_REQ", v1_pair_req, sizeof(plink_pair_req_t)},
    {"PAIR_COMMIT", v1_pair_commit, sizeof(plink_pair_commit_t)},
    {"PAIR_NONCE", v1_pair_nonce, sizeof(plink_pair_nonce_t)},
    {"PAIR_REVEAL", v1_pair_reveal, sizeof(plink_pair_nonce_t)},
    {"PAIR_CONFIRM", v1_pair_confirm, sizeof(plink_pair_confirm_t)},
    {"PAIR_DONE", v1_pair_done, sizeof(plink_pair_done_t)},
    {"PAIR_FAIL", v1_pair_fail, sizeof(plink_pair_fail_t)},
    {"HELLO", v1_hello, sizeof(plink_hello_t)},
    {"WELCOME", v1_welcome, sizeof(plink_hello_t)},
    {"REKEY", v1_rekey, sizeof(plink_rekey_t)},
    {"STATUS", v1_status, sizeof(plink_status_t)},
    {"ZONES", v1_zones, sizeof(plink_zones_t)},
    {"ZONES_REQ", v1_zones_req, sizeof(plink_zones_req_t)},
    {"CFG_REQ", v1_cfg_req, sizeof(plink_cfg_req_t)},
    {"CFG_PAGE", v1_cfg_page, sizeof(plink_cfg_page_t)},
    {"EV_REQ", v1_ev_req, sizeof(plink_ev_req_t)},
    {"EVENTS", v1_events, sizeof(events_msg_t)},
    {"CMD", v1_cmd, sizeof(plink_cmd_t)},
    {"CMD change", v1_cmd_change, sizeof(plink_cmd_t)},
    {"RESULT", v1_result, sizeof(plink_result_t)},
    {"SOUND", v1_sound, sizeof(plink_sound_t)},
    {"HEARTBEAT", v1_heartbeat, sizeof(plink_heartbeat_t)},
    {"HEARTBEAT prefs", v1_heartbeat_prefs, sizeof(plink_heartbeat_t)},
    {"TIME", v1_time, sizeof(plink_time_t)},
    {"BYE", v1_bye, sizeof(plink_bye_t)},
    {"FW_INFO", v1_fw_info, sizeof(plink_fw_info_t)},
    {"FW_ORDER", v1_fw_order, sizeof(plink_fw_order_t)},
    {"FW_STATE", v1_fw_state, sizeof(plink_fw_state_t)},
    {"FW_REQ", v1_fw_req, sizeof(plink_fw_req_t)},
    {"FW_PAGE", v1_fw_page, sizeof(plink_fw_page_t)},
    {"DEV_LIST_REQ", v1_dev_list_req, sizeof(plink_dev_list_req_t)},
    {"DEV_LIST_PAGE", v1_dev_list_page, sizeof(plink_dev_list_page_t)},
    {"DEV_STATE", v1_dev_state, sizeof(plink_dev_state_t)},
    {"DEV_STATE_REQ", v1_dev_state_req, sizeof(plink_dev_state_req_t)},
    {"DEV_ACT", v1_dev_act, sizeof(plink_dev_act_t)},
    {"DEV_ACT mode", v1_dev_act_mode, sizeof(plink_dev_act_t)},
    {"DEV_ACK", v1_dev_ack, sizeof(plink_dev_ack_t)},
    {"DEV_ACK rec", v1_dev_ack_rec, sizeof(plink_dev_ack_t)},
    {"PREFS", v1_prefs, sizeof(plink_prefs_t)},
    {"PREFS_REQ", v1_prefs_req, sizeof(plink_prefs_req_t)},
    {"PREFS_SET", v1_prefs_set, sizeof(plink_prefs_set_t)},
    {"dev record", v1_dev_rec, sizeof(plink_dev_rec_t)},
    {"catalog record", v1_dev_cat, sizeof(plink_dev_cat_t)},
};

static any_msg_t V1_GOT, V1_WANT;

/* The fixture's row i read by today's decoder, as the values frozen with it. ZONES' records and
 * DEV_STATE's are read one by one as well. */
static void expect_v1_values(size_t i)
{
    const char *name = PLINK_WIRE_V1[i].codec;
    const uint8_t *p = PLINK_WIRE_V1[i].p;
    const size_t len = PLINK_WIRE_V1[i].len;
    size_t row = 0;
    while (row < sizeof(V1_VALUES) / sizeof(V1_VALUES[0]) &&
           strcmp(V1_VALUES[row].codec, name) != 0) {
        row++;
    }
    TEST_ASSERT_TRUE_MESSAGE(row < sizeof(V1_VALUES) / sizeof(V1_VALUES[0]), name);
    memset(&V1_GOT, 0, sizeof(V1_GOT));
    memset(&V1_WANT, 0, sizeof(V1_WANT));
    TEST_ASSERT_TRUE_MESSAGE(codec(name)->dec(p, len, &V1_GOT), name);
    V1_VALUES[row].want(p, &V1_WANT);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&V1_WANT, &V1_GOT, V1_VALUES[row].size, name);
    if (strcmp(name, "ZONES") == 0) {
        static const struct {
            uint16_t flags;
            uint8_t trouble, walk;
            uint16_t walk_first_s;
            uint8_t walk_trips;
        } Z[2] = {{0x0201, 1, 5, 300, 2}, {0x0002, 0, 0, 0, 0}};
        for (uint16_t r = 0; r < 2u; r++) {
            alarm_zone_status_t z;
            plink_zones_rec(&V1_GOT.zones, r, &z);
            TEST_ASSERT_EQUAL_HEX16(Z[r].flags, z.flags);
            TEST_ASSERT_EQUAL_UINT8(Z[r].trouble, z.trouble);
            TEST_ASSERT_EQUAL_UINT8(Z[r].walk, z.walk);
            TEST_ASSERT_EQUAL_UINT16(Z[r].walk_first_s, z.walk_first_s);
            TEST_ASSERT_EQUAL_UINT8(Z[r].walk_trips, z.walk_trips);
        }
    } else if (strcmp(name, "DEV_STATE") == 0) {
        static const struct {
            uint8_t index;
            int which;
            size_t at, len;
        } R[2] = {{0, 0, 10, 24}, {5, 1, 34, 26}};
        size_t pos = 0, item_len = 0;
        const uint8_t *item = NULL;
        for (size_t r = 0; r < 2u; r++) {
            uint8_t index = 0xFF;
            plink_dev_rec_t got, want;
            memset(&got, 0, sizeof(got));
            TEST_ASSERT_TRUE(plink_dev_state_next(&V1_GOT.dev_state, &pos, &index, &got, &item,
                                                  &item_len));
            v1_rec(&want, R[r].which);
            TEST_ASSERT_EQUAL_UINT8(R[r].index, index);
            TEST_ASSERT_EQUAL_MEMORY(&want, &got, sizeof(got));
            TEST_ASSERT_EQUAL_PTR(p + R[r].at, item);
            TEST_ASSERT_EQUAL_size_t(R[r].len, item_len);
        }
        TEST_ASSERT_FALSE(plink_dev_state_next(&V1_GOT.dev_state, &pos, NULL, NULL, NULL, NULL));
    }
}

/* LINK-017, LINK-043 (docs/features/11 "Frozen values"): every v1 number the fixture froze from
 * the tables - capability bits, STATUS and record flags, op, kind, state, last, why, result,
 * voice and origin values - is plink_msg.h's, and plink_msg.h has no other. */
#define NUM(sym) {#sym, (uint32_t)(sym)}
static const struct {
    const char *name;
    uint32_t value;
} NUMBERS[] = {
    NUM(PLINK_PROTO),
    NUM(PLINK_CAP_FW), NUM(PLINK_CAP_HOME), NUM(PLINK_CAP_PREFS),
    NUM(PLINK_SF_PROGRAM), NUM(PLINK_SF_EXIT_SOUNDS), NUM(PLINK_SF_UPDATING), NUM(PLINK_SF_HOLD),
    NUM(PLINK_SF_FIRE), NUM(PLINK_SF_CO),
    NUM(PLINK_PF_READY), NUM(PLINK_PF_CHIME), NUM(PLINK_PF_WALK_TEST), NUM(PLINK_PF_TRIGGERED),
    NUM(PLINK_PF_EXIT_ERROR), NUM(PLINK_PF_TROUBLE_UNACK), NUM(PLINK_PF_QUICK_EXIT),
    NUM(PLINK_REPORT_NONE), NUM(PLINK_REPORT_OFFLINE), NUM(PLINK_REPORT_ONLINE),
    NUM(PLINK_HB_MAINS_LOST), NUM(PLINK_HB_BATTERY_LOW), NUM(PLINK_HB_BATTERY_THERE),
    NUM(PLINK_STATUS_AGE_NONE), NUM(PLINK_EVF_GAP),
    NUM(PLINK_OP_COMMAND), NUM(PLINK_OP_AUTH), NUM(PLINK_OP_CHANGE_CODE),
    NUM(PLINK_FWI_FETCH), NUM(PLINK_FWI_RELAY), NUM(PLINK_FWI_KEY_ACCEPT),
    NUM(PLINK_FWI_CFG_UNREADABLE),
    NUM(PLINK_FW_OP_INFO), NUM(PLINK_FW_OP_PROBE), NUM(PLINK_FW_OP_FETCH), NUM(PLINK_FW_OP_RELAY),
    NUM(PLINK_FW_OP_INSTALL), NUM(PLINK_FW_OP_CANCEL),
    NUM(PLINK_FW_KIND_APP), NUM(PLINK_FW_KIND_C6), NUM(PLINK_FW_KIND_LANG),
    NUM(PLINK_FW_KIND_ASSETS),
    NUM(PLINK_FWS_IDLE), NUM(PLINK_FWS_PROBING), NUM(PLINK_FWS_FETCHING), NUM(PLINK_FWS_RECEIVING),
    NUM(PLINK_FWS_KEY_WAIT), NUM(PLINK_FWS_VERIFYING), NUM(PLINK_FWS_READY),
    NUM(PLINK_FWS_INSTALLING), NUM(PLINK_FWS_FAILED), NUM(PLINK_FWS_STOPPED),
    NUM(PLINK_FWF_PAUSED),
    NUM(PLINK_FW_LAST_NONE), NUM(PLINK_FW_LAST_UPDATED), NUM(PLINK_FW_LAST_ROLLED_BACK),
    NUM(PLINK_FW_LAST_STOPPED), NUM(PLINK_FW_LAST_FAILED),
    NUM(PLINK_FW_WHY_OK),
    NUM(PLINK_FW_WHY_NO_NETWORK), NUM(PLINK_FW_WHY_NO_LOOKUP), NUM(PLINK_FW_WHY_NO_CONNECT),
    NUM(PLINK_FW_WHY_SERVER_ERROR), NUM(PLINK_FW_WHY_NOT_A_SERVER),
    NUM(PLINK_FW_WHY_UNKNOWN_DEVICE), NUM(PLINK_FW_WHY_HELD_BACK),
    NUM(PLINK_FW_WHY_STEPPING_STONE), NUM(PLINK_FW_WHY_OTHER_VERSION),
    NUM(PLINK_FW_WHY_UP_TO_DATE), NUM(PLINK_FW_WHY_UNKNOWN_VERSION), NUM(PLINK_FW_WHY_NOT_OFFERED),
    NUM(PLINK_FW_WHY_BROKE_OFF), NUM(PLINK_FW_WHY_TOO_SLOW), NUM(PLINK_FW_WHY_BAD_SIZE),
    NUM(PLINK_FW_WHY_BAD_SHA), NUM(PLINK_FW_WHY_STALLED), NUM(PLINK_FW_WHY_TIMEOUT),
    NUM(PLINK_FW_WHY_NO_MEMORY), NUM(PLINK_FW_WHY_FLASH),
    NUM(PLINK_FW_WHY_KEY), NUM(PLINK_FW_WHY_CHIP), NUM(PLINK_FW_WHY_PROJECT),
    NUM(PLINK_FW_WHY_NOT_NEWER), NUM(PLINK_FW_WHY_SIGNATURE), NUM(PLINK_FW_WHY_TOO_BIG),
    NUM(PLINK_FW_WHY_UNKNOWN_KIND), NUM(PLINK_FW_WHY_UNKNOWN_OP), NUM(PLINK_FW_WHY_BUDGET),
    NUM(PLINK_FW_WHY_BAD_ORDER), NUM(PLINK_FW_WHY_WRONG_VERSION),
    NUM(PLINK_FW_WHY_MASTER_BUSY), NUM(PLINK_FW_WHY_NOT_QUIET), NUM(PLINK_FW_WHY_PANIC_PENDING),
    NUM(PLINK_FW_WHY_KEPT_TONE), NUM(PLINK_FW_WHY_NO_STATUS), NUM(PLINK_FW_WHY_BUSY),
    NUM(PLINK_FW_WHY_STOPPED), NUM(PLINK_FW_WHY_CANCELLED),
    NUM(PLINK_DEVF_ONLINE), NUM(PLINK_DEVF_PENDING), NUM(PLINK_DEVF_FAILED), NUM(PLINK_DEVF_UNSURE),
    NUM(PLINK_DEV_MODE_OFF), NUM(PLINK_DEV_MODE_HEAT), NUM(PLINK_DEV_MODE_COOL),
    NUM(PLINK_DEV_MODE_AUTO),
    NUM(PLINK_DEV_OK), NUM(PLINK_DEV_NEED_CODE), NUM(PLINK_DEV_CODE_REFUSED), NUM(PLINK_DEV_BUSY),
    NUM(PLINK_DEV_UNREACHABLE), NUM(PLINK_DEV_REFUSED), NUM(PLINK_DEV_UNAVAILABLE),
    NUM(PLINK_DEV_CAT_COMPAT),
    NUM(PLINK_PREFS_NOT_ALLOWED), NUM(PLINK_PREFS_RANGE), NUM(PLINK_PREFS_IN_USE),
    NUM(PLINK_PREFS_HELD), NUM(PLINK_PREFS_VOICE_NOT_READY), NUM(PLINK_PREFS_BAD),
    NUM(PLINK_PREFS_BUSY),
    NUM(PLINK_VOICE_UNKNOWN), NUM(PLINK_VOICE_READY), NUM(PLINK_VOICE_LOADING), NUM(PLINK_VOICE_NONE),
    NUM(PLINK_ORIGIN_HA), NUM(PLINK_ORIGIN_CONSOLE),
};
#undef NUM

static void test_wire_numbers_v1(void)
{
    const size_t frozen = sizeof(PLINK_WIRE_V1_NUMBERS) / sizeof(PLINK_WIRE_V1_NUMBERS[0]);
    const size_t here = sizeof(NUMBERS) / sizeof(NUMBERS[0]);
    for (size_t i = 0; i < frozen; i++) {
        bool found = false;
        for (size_t j = 0; j < here; j++) {
            if (strcmp(NUMBERS[j].name, PLINK_WIRE_V1_NUMBERS[i].name) == 0) {
                TEST_ASSERT_FALSE_MESSAGE(found, NUMBERS[j].name); /* named once */
                TEST_ASSERT_EQUAL_HEX32_MESSAGE(PLINK_WIRE_V1_NUMBERS[i].value, NUMBERS[j].value,
                                                NUMBERS[j].name);
                found = true;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, PLINK_WIRE_V1_NUMBERS[i].name);
    }
    TEST_ASSERT_EQUAL_size_t(frozen, here);
    /* The masks built from them. */
    TEST_ASSERT_EQUAL_HEX32(0x0007u, PLINK_CAPS_KNOWN & ~PLINK_CAP_WIFI);
    TEST_ASSERT_EQUAL_HEX32(0x0008u, PLINK_CAP_WIFI);
}

/* The type of each codec row the frozen fixture names (its first word); NOT_PAIRED has no
 * payload and BEACON is reserved. */
static const struct {
    uint8_t type;
    const char *name;
} TYPE_NAMES[] = {
    {PLINK_T_PAIR_REQ, "PAIR_REQ"},       {PLINK_T_PAIR_COMMIT, "PAIR_COMMIT"},
    {PLINK_T_PAIR_NONCE, "PAIR_NONCE"},   {PLINK_T_PAIR_REVEAL, "PAIR_REVEAL"},
    {PLINK_T_PAIR_CONFIRM, "PAIR_CONFIRM"}, {PLINK_T_PAIR_DONE, "PAIR_DONE"},
    {PLINK_T_PAIR_FAIL, "PAIR_FAIL"},     {PLINK_T_HELLO, "HELLO"},
    {PLINK_T_WELCOME, "WELCOME"},         {PLINK_T_REKEY, "REKEY"},
    {PLINK_T_STATUS, "STATUS"},           {PLINK_T_ZONES, "ZONES"},
    {PLINK_T_ZONES_REQ, "ZONES_REQ"},     {PLINK_T_CFG_REQ, "CFG_REQ"},
    {PLINK_T_CFG_PAGE, "CFG_PAGE"},       {PLINK_T_EV_REQ, "EV_REQ"},
    {PLINK_T_EVENTS, "EVENTS"},           {PLINK_T_CMD, "CMD"},
    {PLINK_T_RESULT, "RESULT"},           {PLINK_T_SOUND, "SOUND"},
    {PLINK_T_HEARTBEAT, "HEARTBEAT"},     {PLINK_T_TIME, "TIME"},
    {PLINK_T_BYE, "BYE"},                 {PLINK_T_FW_INFO, "FW_INFO"},
    {PLINK_T_FW_ORDER, "FW_ORDER"},       {PLINK_T_FW_STATE, "FW_STATE"},
    {PLINK_T_FW_REQ, "FW_REQ"},           {PLINK_T_FW_PAGE, "FW_PAGE"},
    {PLINK_T_DEV_LIST_REQ, "DEV_LIST_REQ"}, {PLINK_T_DEV_LIST_PAGE, "DEV_LIST_PAGE"},
    {PLINK_T_DEV_STATE, "DEV_STATE"},     {PLINK_T_DEV_STATE_REQ, "DEV_STATE_REQ"},
    {PLINK_T_DEV_ACT, "DEV_ACT"},         {PLINK_T_DEV_ACK, "DEV_ACK"},
    {PLINK_T_PREFS, "PREFS"},             {PLINK_T_PREFS_REQ, "PREFS_REQ"},
    {PLINK_T_PREFS_SET, "PREFS_SET"},
};

#define FIXTURE_COUNT (sizeof(PLINK_WIRE_V1) / sizeof(PLINK_WIRE_V1[0]))

/* LINK-017, LINK-043: the frozen v1 payloads (plink_wire_fixture_v1.h, made independently by
 * vectors_plink_msg.py from the tables) are read by today's decoders as the values frozen with
 * them (pointers at their offsets, HEARTBEAT's and DEV_ACK's sections there or not) and encoded
 * again to exactly their bytes, so no v1 layout has moved, not even in the encoder and the decoder
 * alike; and every type this firmware knows that carries a payload is in it (a type added after
 * v1 goes in a fixture of its own). */
static void test_wire_fixture_v1(void)
{
    TEST_ASSERT_EQUAL_size_t(sizeof(V1_VALUES) / sizeof(V1_VALUES[0]), FIXTURE_COUNT);
    for (size_t i = 0; i < FIXTURE_COUNT; i++) {
        const codec_t *c = codec(PLINK_WIRE_V1[i].codec);
        const uint8_t *p = PLINK_WIRE_V1[i].p;
        const size_t len = PLINK_WIRE_V1[i].len;
        expect_v1_values(i);
        if (c->enc != NULL) {
            expect_canonical(c, p, len, p, len);
            continue;
        }
        /* ZONES: its records through plink_zones_rec, encoded again. */
        plink_zones_t z;
        TEST_ASSERT_TRUE(plink_dec_zones(p, len, &z));
        alarm_zone_status_t recs[8];
        TEST_ASSERT_TRUE(z.count <= 8u);
        for (uint16_t r = 0; r < z.count; r++) {
            plink_zones_rec(&z, r, &recs[r]);
        }
        size_t n = 0;
        uint16_t written = 0;
        TEST_ASSERT_TRUE(plink_enc_zones(z.zones_crc, z.first, recs, z.count, OUT, sizeof(OUT), &n,
                                         &written));
        TEST_ASSERT_EQUAL_size_t(len, n);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(p, OUT, len);
    }
    for (unsigned t = 0; t < 256u; t++) {
        if (!plink_type_known((uint8_t)t) || t == PLINK_T_NOT_PAIRED || t == PLINK_T_BEACON) {
            continue;
        }
        if (t == PLINK_T_WIFI_REQ || t == PLINK_T_WIFI) {
            /* added after v1: frozen in plink_wifi_fixture_v1.h (test_wifi_fixture_v1) */
            TEST_ASSERT_TRUE(t == PLINK_WIFI_V1_TYPE_REQ || t == PLINK_WIFI_V1_TYPE);
            continue;
        }
        const char *name = NULL;
        for (size_t i = 0; i < sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]); i++) {
            if (TYPE_NAMES[i].type == t) {
                name = TYPE_NAMES[i].name;
            }
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(name, "a known type without a name here");
        bool found = false;
        for (size_t i = 0; i < FIXTURE_COUNT; i++) {
            found = found || strcmp(PLINK_WIRE_V1[i].codec, name) == 0;
        }
        TEST_ASSERT_TRUE_MESSAGE(found, name);
    }
}

/* LINK-053, LINK-017: the frozen WIFI_REQ and WIFI payloads (plink_wifi_fixture_v1.h, built from
 * the published layout) keep their numbers, read as the values frozen with them, and encode again
 * to exactly their bytes. */
static void test_wifi_fixture_v1(void)
{
    TEST_ASSERT_EQUAL_HEX8(PLINK_WIFI_V1_TYPE_REQ, PLINK_T_WIFI_REQ);
    TEST_ASSERT_EQUAL_HEX8(PLINK_WIFI_V1_TYPE, PLINK_T_WIFI);
    TEST_ASSERT_EQUAL_HEX16(PLINK_WIFI_V1_CAP, PLINK_CAP_WIFI);
    TEST_ASSERT_EQUAL_size_t(PLINK_WIFI_REQ_LEN, sizeof PLINK_WIFI_REQ_V1);
    TEST_ASSERT_EQUAL_size_t(PLINK_WIFI_LEN, sizeof PLINK_WIFI_V1);
    uint32_t id = 0;
    for (unsigned i = 0; i < sizeof PLINK_WIFI_REQ_V1; i++) {
        id |= (uint32_t)PLINK_WIFI_REQ_V1[i] << (8u * i);
    }
    TEST_ASSERT_EQUAL_HEX32(PLINK_WIFI_V1_REQ_ID, id);
    plink_wifi_t w;
    TEST_ASSERT_TRUE(plink_dec_wifi(PLINK_WIFI_V1, sizeof PLINK_WIFI_V1, id, &w));
    TEST_ASSERT_EQUAL_STRING(PLINK_WIFI_V1_SSID, w.ssid);
    TEST_ASSERT_EQUAL_STRING(PLINK_WIFI_V1_PASSWORD, w.password);
    uint8_t again[PLINK_WIFI_LEN];
    TEST_ASSERT_TRUE(plink_enc_wifi(&w, id, again, sizeof again));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(PLINK_WIFI_V1, again, sizeof again);
}

/* LINK-053: maximum lengths, malformed and stale replies, unavailable/open network. */
static void test_wifi_credentials_codec(void)
{
    plink_wifi_t in = {0}, out;
    uint8_t wire[103];
    memset(in.ssid, 's', 32);
    memset(in.password, 'p', 64);
    TEST_ASSERT_TRUE(plink_enc_wifi(&in, 0x04030201u, wire, sizeof wire));
    TEST_ASSERT_EQUAL_UINT8(1, wire[0]);
    TEST_ASSERT_EQUAL_UINT8(4, wire[3]);
    TEST_ASSERT_TRUE(plink_dec_wifi(wire, 102, 0x04030201u, &out));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
    TEST_ASSERT_FALSE(plink_dec_wifi(wire, 101, 0x04030201u, &out));
    TEST_ASSERT_FALSE(plink_dec_wifi(wire, 102, 12, &out));
    TEST_ASSERT_EQUAL_STRING("", out.password);
    wire[36] = 's';
    TEST_ASSERT_FALSE(plink_dec_wifi(wire, 102, 0x04030201u, &out));
    in.password[0] = 0;
    TEST_ASSERT_TRUE(plink_enc_wifi(&in, 1, wire, sizeof wire));
    TEST_ASSERT_TRUE(plink_dec_wifi(wire, 103, 1, &out));
    TEST_ASSERT_EQUAL_STRING("", out.password);
    in.ssid[0] = 0;
    strcpy(in.password, "must-not-leak");
    TEST_ASSERT_TRUE(plink_enc_wifi(&in, 1, wire, sizeof wire));
    TEST_ASSERT_TRUE(plink_dec_wifi(wire, 102, 1, &out));
    TEST_ASSERT_EQUAL_STRING("", out.password);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_wifi_credentials_codec);
    RUN_TEST(test_wifi_fixture_v1);
    RUN_TEST(test_wire_numbers);
    RUN_TEST(test_wire_sections);
    RUN_TEST(test_wire_strings);
    RUN_TEST(test_clean_text);
    RUN_TEST(test_golden_pairing);
    RUN_TEST(test_golden_handshake);
    RUN_TEST(test_golden_status);
    RUN_TEST(test_status_temporary_selection_fixture);
    RUN_TEST(test_golden_bulk);
    RUN_TEST(test_golden_commands);
    RUN_TEST(test_golden_others);
    RUN_TEST(test_golden_round_trip);
    RUN_TEST(test_random_round_trips);
    RUN_TEST(test_appended_bytes_skipped);
    RUN_TEST(test_short_sections_read_zero);
    RUN_TEST(test_truncated_refused);
    RUN_TEST(test_names_cleaned);
    RUN_TEST(test_cmd_codes);
    RUN_TEST(test_cmd_digest);
    RUN_TEST(test_status_split);
    RUN_TEST(test_events_paging);
    RUN_TEST(test_zones_and_pages);
    RUN_TEST(test_golden_updates);
    RUN_TEST(test_fw_why_class);
    RUN_TEST(test_golden_home);
    RUN_TEST(test_golden_prefs);
    RUN_TEST(test_prefs_body_crc);
    RUN_TEST(test_heartbeat_section);
    RUN_TEST(test_dev_ack_record);
    RUN_TEST(test_dev_act_one_size);
    RUN_TEST(test_dev_act_digest);
    RUN_TEST(test_longest_payloads);
    RUN_TEST(test_new_sections_grow);
    RUN_TEST(test_new_short_sections);
    RUN_TEST(test_new_refusals);
    RUN_TEST(test_new_random_round_trips);
    RUN_TEST(test_wire_fixture_v1);
    RUN_TEST(test_wire_numbers_v1);
    RUN_TEST(test_fuzz_decoders);
    return UNITY_END();
}
