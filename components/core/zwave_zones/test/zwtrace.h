/*
 * zwtrace.h - a "zwtrace 1" recording of the panel's Z-Wave link, read into memory (docs/features/36
 * README "Test and acceptance"; the fixtures and how a bench recording becomes one:
 * test/traces/README.md).
 *
 * panelctl's `zwave trace` and `zwave check` write these files from the developer link's
 * GET /dev/v1/zwave/trace (docs/features/30 "Z-Wave"): a first line `zwtrace 1`; `#` comments
 * (the tool, the module's firmware, the date; `# panel restarted` where the panel rebooted while it
 * recorded); then one line per entry, `<seq> <t_ms> <dir> <hex>`, as the zw_link task saw it:
 *   rx    a frame from the module, SOF to checksum - a damaged one, or one the 150 ms byte timeout
 *         cut off, as it came (the panel NAKed or dropped it)
 *   tx    what the panel wrote: a frame, or a raw byte (the bootloader's "run" key)
 *   crx   an ACK, NAK or CAN from the module
 *   ctx   one the panel sent: the link's answer to the rx entry just before it, or the start-up's
 *         lone NAK
 *   text  bytes outside frames (bootloader text, noise)
 * and `lost <n>` where n entries were missed.
 *
 * Test support, not firmware: the whole file in a caller-owned zwt_trace_t (static: it is big), no
 * heap; zwt_load() reads a file with stdio.
 */
#ifndef ZWTRACE_H
#define ZWTRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZWT_ENTRIES_MAX   8192u
#define ZWT_BYTES_MAX     (256u * 1024u)
#define ZWT_ENTRY_MAX     1024u /* bytes in one entry (the panel's are at most 260) */
#define ZWT_ERROR_LEN     160u
#define ZWT_SCRUBBED_MARK "# scrubbed by zwtrace_scrub.py" /* tools/zwtrace_scrub.py's line */

typedef enum {
    ZWT_RX = 0,
    ZWT_TX,
    ZWT_CRX,
    ZWT_CTX,
    ZWT_TEXT,
    ZWT_LOST,   /* `lost <n>`: n entries missed here */
    ZWT_RESTART /* `# panel restarted`: the panel rebooted, its numbers and clock start again */
} zwt_kind_t;

typedef struct {
    uint8_t kind;  /* zwt_kind_t */
    uint32_t seq;  /* as recorded; 0 for LOST and RESTART */
    uint64_t t;    /* ms since the panel started, as recorded */
    uint32_t lost; /* LOST: how many */
    uint32_t line; /* its line in the file */
    uint32_t off;  /* its bytes: zwt_bytes() */
    uint16_t len;
} zwt_entry_t;

typedef struct {
    size_t n;
    zwt_entry_t e[ZWT_ENTRIES_MAX];
    size_t used;
    uint8_t bytes[ZWT_BYTES_MAX];
    uint32_t lines;      /* lines read */
    uint32_t lost;       /* entries missed, all `lost` lines together */
    uint32_t restarts;   /* `# panel restarted` markers */
    bool scrubbed;       /* it carries tools/zwtrace_scrub.py's mark */
    uint32_t error_line; /* the refused line (1: the header) */
    char error[ZWT_ERROR_LEN];
} zwt_trace_t;

typedef enum {
    ZWT_OK = 0,
    ZWT_ERR_HEADER, /* not `zwtrace 1` */
    ZWT_ERR_LINE,   /* a line that is neither a comment, `lost <n>` nor an entry */
    ZWT_ERR_FULL,   /* more than ZWT_ENTRIES_MAX entries or ZWT_BYTES_MAX bytes */
    ZWT_ERR_FILE    /* the file could not be read */
} zwt_result_t;

/* The recording in text (the whole file); on a refusal t->error says why, t->error_line where. */
zwt_result_t zwt_parse(zwt_trace_t *t, const char *text, size_t len);
/* The same from a file. */
zwt_result_t zwt_load(zwt_trace_t *t, const char *path);

const uint8_t *zwt_bytes(const zwt_trace_t *t, const zwt_entry_t *e);

/* A whole frame with a good checksum (SOF, LEN >= 3, LEN + 2 bytes). */
bool zwt_frame_ok(const uint8_t *b, size_t n);
/* b in lowercase hex into out (NUL-terminated, "..." at the end when it does not fit); the
 * length written. */
size_t zwt_hex(const uint8_t *b, size_t n, char *out, size_t cap);

/* What the module said about itself in a recording - its last good answers, what the pages learn
 * from them: GetSerialApiCapabilities (firmware), GetSerialApiInitData (chip), GetProtocolVersion
 * (SDK), SerialAPISetup GetRFRegion (region), GetControllerId (home ID, the panel's node ID). */
typedef struct {
    bool has_caps;
    uint8_t firmware_major, firmware_minor;
    uint16_t manufacturer, product_type, product_id;
    bool has_init;
    uint8_t chip_type;
    bool has_sdk;
    uint8_t sdk_major, sdk_minor, sdk_patch;
    bool has_region;
    uint8_t region;
    bool has_home;
    uint32_t home_id;
    uint16_t node_id;
    unsigned started; /* SerialAPIStarted frames */
} zwt_module_t;

void zwt_module(const zwt_trace_t *t, zwt_module_t *out);

#endif /* ZWTRACE_H */
