/*
 * zwave_internal.h - what zwave_service's files share: the link (zw_link.c), its frame trace
 * (zw_trace.c), the crypto port (zw_psa.c), and the service's state and requests for the console
 * (zwave_console.c) and the developer link (zwave_dev.c).
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_ALARM_ZSRC_ZWAVE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zone_hub/zsrc_zwave.h"
#include "zwave_host/zw_crypto.h"
#include "zwave_proto/zw_frame.h"
#include "zwave_service.h"
#include "zwave_zones/zwz_panel.h"
#include "zwave_zones/zw_feedback.h"

/* ---- the link (zw_link.c): the UART, framing, ACK/NAK (serial-api.md §1) ---------------------- */

/* What it hands the zwave task: an ACK/NAK/CAN byte, a whole frame (SOF..CHK, already ACKed),
 * or bytes its parser discarded while idle (the bootloader's text, line noise). */
#define ZW_LINK_CONTROL 'C'
#define ZW_LINK_FRAME   'F'
#define ZW_LINK_TEXT    'T'
#define ZW_LINK_NEWS_MAX (ZW_RX_BUF_SIZE + 1u)

typedef struct {
    uint32_t rx_bytes;
    uint32_t frames;       /* good frames, ACKed and handed on */
    uint32_t naks;         /* damaged frames NAKed */
    uint32_t full;         /* good frames NAKed because the zwave task was behind (resent) */
    uint32_t lost;         /* ACK/NAK/CAN bytes or text dropped for the same reason */
    uint32_t timeouts;     /* partial frames dropped by the byte timeout */
    uint32_t discarded;    /* bytes outside frames */
    uint32_t overflows;    /* the UART's own buffers overflowed */
    uint32_t tx_frames;    /* writes of the zwave task's */
    uint32_t tx_bytes;
    uint32_t tx_lost;      /* writes that could not be queued */
} zw_link_stats_t;

/* Opens the UART on its pins and starts the zw_link task; its news wakes `zwave`. */
esp_err_t zw_link_start(int uart, int tx_gpio, int rx_gpio, TaskHandle_t zwave);
/* Bytes to the module (a frame, or the start-up's NAK); the zw_link task writes them. */
bool zw_link_write(const uint8_t *bytes, size_t len);
/* The next piece of news: its kind and bytes (at most ZW_LINK_NEWS_MAX - 1); 0 when none. */
size_t zw_link_read(uint8_t *kind, uint8_t *bytes, size_t cap);
/* Frames in hex on the log, both ways (`zwave trace`, development builds). */
void zw_link_trace(bool on);
bool zw_link_tracing(void);
void zw_link_stats(zw_link_stats_t *out);
/* The link task's stack: bytes never used and its size (both 0 before it starts). */
void zw_link_stack(uint32_t *free_bytes, uint32_t *size);
/* The firmware worker's stack, the same (0 before its first job). */
void zw_fwsvc_stack(uint32_t *free_bytes, uint32_t *size);

/* ---- the frame trace (zw_trace.c, development builds): what the link sees and sends ------------
 * A ring in PSRAM that zw_link writes and the developer link reads (GET /dev/v1/zwave/trace,
 * docs/features/30): wire bytes only - S2 payloads stay encrypted, the keys never pass the link.
 * The words are the developer link's `dir`. */
#define ZW_TRACE_RX   0u /* a frame from the module, SOF..CHK (damaged or cut off: as it came) */
#define ZW_TRACE_TX   1u /* a frame to the module */
#define ZW_TRACE_CRX  2u /* an ACK, NAK or CAN from the module */
#define ZW_TRACE_CTX  3u /* one the panel sent */
#define ZW_TRACE_TEXT 4u /* bytes outside frames (the bootloader's text, noise) */

#if CONFIG_ALARM_BUILD_DEV
/* Its lock; before the zw_link task starts. */
void zw_trace_start(void);
/* One entry, stamped now (ms since boot): the zw_link task's alone. It waits for the lock a few
 * milliseconds at most; an entry that could not wait is a gap readers count as lost. */
void zw_trace_put(uint8_t dir, const uint8_t *bytes, size_t len);
#else
static inline void zw_trace_start(void)
{
}
static inline void zw_trace_put(uint8_t dir, const uint8_t *bytes, size_t len)
{
    (void)dir;
    (void)bytes;
    (void)len;
}
#endif

/* ---- the crypto port on PSA (zw_psa.c): AES-128 ECB, X25519, the random source ----------------- */

/* False when PSA could not be started. The port is the zwave task's alone. */
bool zw_psa_port(zw_crypto_port_t *out);

/* The port's self-test at start (zw_crypto_port_check, ZW-008): run only when a place is chosen;
 * failed, the radio stays off. */
typedef enum {
    ZWSVC_CRYPTO_NOT_RUN = 0,
    ZWSVC_CRYPTO_PASSED,
    ZWSVC_CRYPTO_FAILED,
} zwsvc_crypto_t;

/* ---- the service, for the console and the developer link (zwave_service.c) -------------------- */

typedef enum {
    ZWSVC_REQ_UI = 1,  /* a ui_zw_request_t */
    ZWSVC_REQ_INTERVIEW, /* interview `node` again */
    ZWSVC_REQ_PULSE,   /* pulse the module's reset line */
    ZWSVC_REQ_FEEDBACK, /* explicit output-only keypad assignment */
    ZWSVC_REQ_CONTROL,  /* explicit keypad control assignment (its buttons arm/disarm) */
    ZWSVC_REQ_FW,       /* a device's software: ui.kind UI_ZW_REQ_FW_CHECK / _INSTALL (ZW-033) */
    ZWSVC_REQ_HA,       /* Home Assistant's command for an output: ha_key, ha_level (ZW-036) */
    ZWSVC_REQ_POWER,    /* the radio's power (network.md §12, ZW-025): read, or power_set */
    ZWSVC_REQ_RADIO_FILE, /* the radio's own firmware from the installer's .gbl (§14) */
} zwsvc_req_kind_t;

typedef struct {
    uint8_t kind;      /* zwsvc_req_kind_t */
    bool allowed;      /* installer mode (the UI) or every partition disarmed (the console, the
                        * developer link) */
    ui_zw_request_t ui;
    uint8_t feedback_slot, feedback_partition; /* slot 0-based (ZWSVC_SLOT_ANY: the node's own
                                                * or the first free one); partition 0 removes;
                                                * CONTROL uses them too */
    uint8_t control_panic; /* CONTROL: its emergency buttons, 1 << alarm_panic_t */
    bool console;          /* FW: the console's (disarmed is enough), not the pages' */
    char ha_key[PROTO_ZW_KEY_LEN]; /* HA */
    int ha_level;
    uint64_t ha_received;
    bool power_set;        /* POWER: change it (else read it) */
    bool power_lr;         /* POWER: the Long Range maximum */
    int16_t power_dbm10;   /* POWER: the new power, tenths of a dBm */
} zwsvc_req_t;

/* ZWSVC_REQ_FEEDBACK from the device page: the node's own slot, else the first free one. */
#define ZWSVC_SLOT_ANY 0xFFu

typedef struct {
    uint32_t stores, store_failures; /* NVS writes */
    uint32_t events, events_failed;  /* events logged */
    uint32_t requests, request_timeouts;
    /* Each task's stack high-water mark: bytes it never used, and its size (0: not started).
     * The "zwave" task, the "zw_link" task, the "zwave-fw" worker (made at its first job). */
    uint32_t stack_free, stack_size;
    uint32_t link_stack_free, link_stack_size;
    uint32_t fw_stack_free, fw_stack_size;
} zwsvc_stats_t;

/* The service runs (its source registered) / its radio runs (a place chosen). */
bool zwsvc_started(void);
bool zwsvc_running(void);
zwsvc_crypto_t zwsvc_crypto_check(void);
const zwave_service_config_t *zwsvc_config(void);
const zwave_service_rules_t *zwsvc_rules(void);
void zwsvc_stats(zwsvc_stats_t *out);
/* The network and the zone source: read them with zone_service_lock() held. */
zwz_panel_t *zwsvc_panel(void);
uint64_t zwsvc_now_ms(void);
bool zwsvc_installer_and_disarmed(void);
/* SmartStart's list with the caller's own `allowed` (the pages: installer mode; the console:
 * every partition disarmed). */
ui_zw_prov_result_t zwsvc_prov_register(const char *code, const char *name, bool allowed);
ui_zw_prov_result_t zwsvc_prov_change(const uint8_t dsk[16], uint8_t op, uint8_t arg,
                                      bool allowed);

/* ---- a device's software update (zw_fwsvc.c, ZW-033) ------------------------------------------ */

/* ZWSVC_REQ_FW, in the zwave task: starts the check or the download (the worker does the HTTPS). */
ui_zw_req_result_t zwsvc_fw_request(const zwsvc_req_t *req, bool need_installer);
/* STOP while a file downloads: the job back to its offer (the zone lock taken here). */
bool zwsvc_fw_cancel(void);
/* network.md §14: the radio's own firmware from the installer's .gbl (ZWSVC_REQ_RADIO_FILE, the
 * zwave task), and its check's state for the pages (takes the zone lock). */
ui_zw_req_result_t zwsvc_fw_radio_file(uint8_t *image, size_t len, bool allowed, uint8_t user);
void zwsvc_fw_radio_status(ui_zw_status_t *st);
/* In the zwave task, outside the zone lock: the worker's answer taken, the image let go. */
void zwsvc_fw_poll(void);
/* The device page's software check; the zone lock held. */
void zwsvc_fw_page(uint16_t node, uint16_t generation, ui_zw_device_page_t *out);
/* The job in words, for the console. */
void zwsvc_fw_describe(char *out, size_t len);
/* Read under zone_service_lock; output assignments only, never PIN/control rights. */
const zwf_t *zwsvc_feedback(void);
/* Read under zone_service_lock: which keypads may arm/disarm (never a PIN). */
const zwkc_t *zwsvc_keyctl(void);
zsrc_zw_t *zwsvc_source(void);
/* Runs a request on the zwave task and waits for its answer. */
ui_zw_req_result_t zwsvc_submit(const zwsvc_req_t *req);
/* NVS zwave: a string or a small number, or its erasure (value NULL / < 0). */
esp_err_t zwsvc_store_str(const char *key, const char *value);
esp_err_t zwsvc_store_u8(const char *key, int value);
/* `zwave place custom` stored (ZW-046, ADR-048): the place erased first, the reset pin of a named
 * place cleared, the pins next, place "custom" last. reset -1: none. */
esp_err_t zwsvc_store_custom(int16_t tx, int16_t rx, int16_t reset);
/* After the place left custom: its pins and the stored reset pin forgotten (ZW-046). */
esp_err_t zwsvc_forget_custom(void);

#endif /* CONFIG_ALARM_ZSRC_ZWAVE */
