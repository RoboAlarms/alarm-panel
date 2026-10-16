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
 * Requirements: LINK-004, LINK-005, LINK-017, LINK-018
 */
#include <string.h>

#include "panel_link/plink_frame.h"
#include "panel_link/plink_msg.h"
#include "panel_link/plink_wire.h"
#include "unity.h"

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
    ROW("TIME", G_TIME, d_time, e_time),
    ROW("BYE", G_BYE, d_bye, e_bye),
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
            /* STATUS ends after any whole record: the system section alone, or with record 1. */
            const bool whole = c->dec == d_status && (len == 60u || len == 60u + 54u);
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

/* What a decoder promises about what it accepted. */
static void check_decoded(const codec_t *c, const any_msg_t *m, const uint8_t *p, size_t len)
{
    if (c->dec == d_pair_req) {
        TEST_ASSERT_TRUE(text_clean(m->pair_req.name, sizeof(m->pair_req.name)));
        TEST_ASSERT_TRUE(text_clean(m->pair_req.fw, sizeof(m->pair_req.fw)));
    } else if (c->dec == d_pair_commit) {
        TEST_ASSERT_TRUE(text_clean(m->pair_commit.name, sizeof(m->pair_commit.name)));
        TEST_ASSERT_TRUE(text_clean(m->pair_commit.panel_id, sizeof(m->pair_commit.panel_id)));
    } else if (c->dec == d_heartbeat) {
        TEST_ASSERT_TRUE(text_clean(m->heartbeat.fw, sizeof(m->heartbeat.fw)));
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_wire_numbers);
    RUN_TEST(test_wire_sections);
    RUN_TEST(test_wire_strings);
    RUN_TEST(test_clean_text);
    RUN_TEST(test_golden_pairing);
    RUN_TEST(test_golden_handshake);
    RUN_TEST(test_golden_status);
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
    RUN_TEST(test_fuzz_decoders);
    return UNITY_END();
}
