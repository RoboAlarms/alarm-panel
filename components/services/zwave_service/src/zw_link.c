/*
 * zw_link.c - the zw_link task: the ZAC93's UART (docs/features/36 serial-api.md §1 "The wire").
 *
 * It owns the UART. Every byte the module sends goes through a zw_rx_t: a good frame is ACKed at
 * once - before the zwave task has seen it, so the module's ACK timeout is always met - and handed
 * on; a damaged one is NAKed; ACK, NAK and CAN bytes and the bytes outside frames (the
 * bootloader's text) are handed on in their order. A good frame the zwave task has no room for is
 * NAKed instead of ACKed: the module sends it again (a frame ACKed and then dropped would be lost
 * for good). What the zwave task sends is written here too, so nothing interleaves on the wire.
 *
 * It waits on a queue set: the UART driver's events and a doorbell the zwave task rings after
 * queueing bytes; with a partial frame collected it also wakes for the 150 ms byte timeout.
 * Its news and the bytes to write go through message buffers in PSRAM (tasks only, never an
 * ISR); its stack is internal (4 KB). The UART's interrupt is not placed in IRAM: bytes arriving
 * while a flash write has the cache off can be lost - that frame is NAKed or times out, and the
 * module sends it again.
 *
 * Development builds also record everything that crosses the wire in the frame trace
 * (zw_trace.c, for the developer link): frames each way, ACK/NAK/CAN each way, the text runs - and
 * a damaged frame, or one the byte timeout cut off, as it came.
 */
#include "zwave_internal.h"

#if CONFIG_ALARM_ZSRC_ZWAVE

#include <stdatomic.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_service.h"
#include "zwave_proto/zw_redact.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"

#if CONFIG_ALARM_ZWAVE_UART_NUM >= SOC_UART_HP_NUM
#error "CONFIG_ALARM_ZWAVE_UART_NUM names a UART this chip does not have"
#endif

static const char *TAG = "zw_link";

#define LINK_STACK      4096
#define LINK_PRIORITY   (CONFIG_ALARM_TASK_PRIORITY + 1)
#define UART_RX_BUF     1024
#define UART_TX_BUF     1024
#define UART_EVENTS     20
#define TO_ZWAVE_BYTES  4096u /* the module's news for the zwave task */
#define TO_UART_BYTES   1024u /* the zwave task's writes */
#define WRITE_WAIT_MS   50u
#define TEXT_RUN        64u

static struct {
    int uart;
    TaskHandle_t task;
    TaskHandle_t zwave;
    QueueHandle_t events;
    QueueSetHandle_t set;
    SemaphoreHandle_t doorbell;
    MessageBufferHandle_t to_zwave;
    MessageBufferHandle_t to_uart;
    StaticMessageBuffer_t to_zwave_mb;
    StaticMessageBuffer_t to_uart_mb;
    zw_rx_t rx;
    atomic_bool trace;
    zw_link_stats_t stats;
} L;

/* The message buffers' storage: tasks only, so PSRAM (alarm_mem.h). */
static ALARM_SLOW_BSS uint8_t s_to_zwave[TO_ZWAVE_BYTES + 1u];
static ALARM_SLOW_BSS uint8_t s_to_uart[TO_UART_BYTES + 1u];

#if CONFIG_ALARM_BUILD_DEV
/* The frame being collected, for the trace: a damaged or cut-off one goes in as it came (the
 * parser keeps only good frames readable). The zw_link task's alone, so PSRAM. */
static ALARM_SLOW_BSS uint8_t s_partial[ZW_RX_BUF_SIZE];
static size_t s_partial_n;

/* A byte of the frame being collected; `starts`: the first (its SOF). */
static void partial_add(bool starts, uint8_t b)
{
    if (starts) {
        s_partial_n = 0u;
    }
    if (s_partial_n < sizeof s_partial) {
        s_partial[s_partial_n++] = b;
    }
}

/* What was collected so far: *len bytes. */
static const uint8_t *partial_bytes(size_t *len)
{
    *len = s_partial_n;
    return s_partial;
}

/* Forgotten: a good frame (the parser has it whole), or one traced already. */
static void partial_drop(void)
{
    s_partial_n = 0u;
}

/* What was collected, into the trace (a frame the byte timeout cut off), then forgotten. */
static void partial_out(void)
{
    if (s_partial_n > 0u) {
        zw_trace_put(ZW_TRACE_RX, s_partial, s_partial_n);
    }
    s_partial_n = 0u;
}
#else
static inline void partial_add(bool starts, uint8_t b)
{
    (void)starts;
    (void)b;
}
static inline const uint8_t *partial_bytes(size_t *len)
{
    *len = 0u;
    return NULL;
}
static inline void partial_drop(void)
{
}
static inline void partial_out(void)
{
}
#endif

static uint64_t now_ms(void)
{
    return alarm_service_now();
}

static bool is_control(uint8_t b)
{
    return b == ZW_ACK || b == ZW_NAK || b == ZW_CAN;
}

/* The panel's answer to a frame, ACK or NAK, at once; then the frame and the answer into the
 * trace, in that order - the trace never delays the answer. */
static void answer(uint8_t b, const uint8_t *frame, size_t len)
{
    (void)uart_write_bytes(L.uart, &b, 1u);
    zw_trace_put(ZW_TRACE_RX, frame, len);
    zw_trace_put(ZW_TRACE_CTX, &b, 1u);
}

/* News for the zwave task; false when it has no room. */
static bool post(uint8_t kind, const uint8_t *bytes, size_t len)
{
    uint8_t msg[ZW_LINK_NEWS_MAX];
    if (len + 1u > sizeof msg) {
        return false;
    }
    msg[0] = kind;
    memcpy(&msg[1], bytes, len);
    if (xMessageBufferSend(L.to_zwave, msg, len + 1u, 0) != len + 1u) {
        return false;
    }
    xTaskNotifyGive(L.zwave);
    return true;
}

/* Room for a frame of len bytes (with its kind and the buffer's own length word). */
static bool room_for(size_t len)
{
    return xMessageBufferSpacesAvailable(L.to_zwave) >= len + 1u + sizeof(size_t);
}

/* `zwave trace on`: the frame in hex on the log - a copy, with any clear PIN or code blanked
 * (zw_redact.h, ZW-030). Only the zw_link task calls it: one scratch buffer. */
static void trace(const char *dir, const uint8_t *bytes, size_t len)
{
    if (atomic_load(&L.trace)) {
        static uint8_t copy[ZW_RX_BUF_SIZE];
        const size_t n = len < sizeof copy ? len : sizeof copy;
        memcpy(copy, bytes, n);
        const bool blanked = zw_redact_frame(copy, n);
        ESP_LOGI(TAG, "%s %u bytes%s", dir, (unsigned)len, blanked ? " (credentials blanked)" : "");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, copy, n, ESP_LOG_INFO);
    }
}

static void text_out(const uint8_t *text, size_t n)
{
    if (n == 0u) {
        return;
    }
    zw_trace_put(ZW_TRACE_TEXT, text, n);
    if (!post(ZW_LINK_TEXT, text, n)) {
        L.stats.lost++;
    }
}

/* The module's bytes through the parser (serial-api.md §1 "Receiving"). */
static void feed(const uint8_t *bytes, size_t n)
{
    uint8_t text[TEXT_RUN];
    size_t tn = 0u;
    const uint64_t now = now_ms();
    L.stats.rx_bytes += (uint32_t)n;
    for (size_t i = 0; i < n; i++) {
        const uint32_t discarded = L.rx.discarded;
        const uint32_t timeouts = L.rx.timeouts;
        const bool was_busy = zw_rx_busy(&L.rx);
        const zw_rx_event_t ev = zw_rx_feed(&L.rx, bytes[i], now);
        const bool cut = L.rx.timeouts != timeouts;
        if (cut) {
            partial_out(); /* the frame before this byte waited too long for it: dropped */
        }
        if (L.rx.discarded != discarded) {
            text[tn++] = bytes[i];
            if (tn == sizeof text) {
                text_out(text, tn);
                tn = 0u;
            }
            continue;
        }
        text_out(text, tn);
        tn = 0u;
        switch (ev) {
        case ZW_RX_ACK:
        case ZW_RX_NAK:
        case ZW_RX_CAN:
            zw_trace_put(ZW_TRACE_CRX, &bytes[i], 1u);
            if (!post(ZW_LINK_CONTROL, &bytes[i], 1u)) {
                L.stats.lost++;
            }
            break;
        case ZW_RX_FRAME: {
            zw_frame_t f;
            if (!zw_rx_frame(&L.rx, &f)) {
                break;
            }
            partial_drop();
            if (!room_for(f.raw_len)) {
                answer(ZW_NAK, f.raw, f.raw_len); /* the zwave task is behind: sent again */
                L.stats.full++;
                break;
            }
            answer(ZW_ACK, f.raw, f.raw_len); /* at once (serial-api.md §1, step 4) */
            trace("<", f.raw, f.raw_len);
            if (post(ZW_LINK_FRAME, f.raw, f.raw_len)) {
                L.stats.frames++;
            } else {
                L.stats.lost++; /* cannot happen: the room was checked, and only this task posts */
            }
            break;
        }
        case ZW_RX_BAD_FRAME: {
            size_t pn = 0u;
            partial_add(false, bytes[i]);
            const uint8_t *damaged = partial_bytes(&pn);
            answer(ZW_NAK, damaged, pn); /* the trace gets the damaged frame as it came */
            partial_drop();
            L.stats.naks++;
            break;
        }
        default: /* a byte of the frame being collected: its SOF when the parser was idle */
            partial_add(!was_busy || cut, bytes[i]);
            break;
        }
    }
    text_out(text, tn);
    L.stats.discarded = L.rx.discarded;
    L.stats.timeouts = L.rx.timeouts;
}

static void read_all(void)
{
    uint8_t buf[128];
    size_t avail = 0u;
    while (uart_get_buffered_data_len(L.uart, &avail) == ESP_OK && avail > 0u) {
        const int n = uart_read_bytes(L.uart, buf, avail < sizeof buf ? avail : sizeof buf, 0);
        if (n <= 0) {
            break;
        }
        feed(buf, (size_t)n);
    }
}

static void drain_tx(void)
{
    uint8_t buf[ZW_RX_BUF_SIZE];
    size_t n;
    while ((n = xMessageBufferReceive(L.to_uart, buf, sizeof buf, 0)) > 0u) {
        trace(">", buf, n);
        (void)uart_write_bytes(L.uart, buf, n);
        /* a frame, or a lone control byte (the start-up's NAK) */
        zw_trace_put(n == 1u && is_control(buf[0]) ? ZW_TRACE_CTX : ZW_TRACE_TX, buf, n);
        L.stats.tx_frames++;
        L.stats.tx_bytes += (uint32_t)n;
    }
}

static void link_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* a partial frame waits no longer than the byte timeout (serial-api.md §1, step 3) */
        const TickType_t wait =
            zw_rx_busy(&L.rx) ? pdMS_TO_TICKS(ZW_BYTE_TIMEOUT_MS + 10u) : portMAX_DELAY;
        const QueueSetMemberHandle_t m = xQueueSelectFromSet(L.set, wait > 0 ? wait : 1);
        if (m == NULL) {
            if (zw_rx_expire(&L.rx, now_ms())) {
                partial_out(); /* cut off: its next byte never came */
            }
            L.stats.timeouts = L.rx.timeouts;
            continue;
        }
        if (m == (QueueSetMemberHandle_t)L.doorbell) {
            (void)xSemaphoreTake(L.doorbell, 0);
            drain_tx();
            continue;
        }
        uart_event_t ev;
        if (xQueueReceive(L.events, &ev, 0) != pdTRUE) {
            continue;
        }
        switch (ev.type) {
        case UART_DATA: read_all(); break;
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            /* bytes are gone: whatever frame they were in is NAKed or times out and comes again */
            L.stats.overflows++;
            (void)uart_flush_input(L.uart);
            break;
        default: break;
        }
    }
}

esp_err_t zw_link_start(int uart, int tx_gpio, int rx_gpio, TaskHandle_t zwave)
{
    memset(&L, 0, sizeof L);
    L.uart = uart;
    L.zwave = zwave;
    zw_rx_init(&L.rx);
    atomic_store(&L.trace, false);
    L.to_zwave = xMessageBufferCreateStatic(TO_ZWAVE_BYTES, s_to_zwave, &L.to_zwave_mb);
    L.to_uart = xMessageBufferCreateStatic(TO_UART_BYTES, s_to_uart, &L.to_uart_mb);
    L.doorbell = xSemaphoreCreateBinary();
    L.set = xQueueCreateSet(UART_EVENTS + 1);
    if (L.to_zwave == NULL || L.to_uart == NULL || L.doorbell == NULL || L.set == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const uart_config_t cfg = {
        .baud_rate = 115200, /* 8N1, no flow control (serial-api.md §1) */
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(uart, UART_RX_BUF, UART_TX_BUF, UART_EVENTS, &L.events, 0);
    if (err != ESP_OK) {
        return err;
    }
    /* members of the set before the pins give the UART anything to report */
    if (xQueueAddToSet(L.events, L.set) != pdPASS ||
        xQueueAddToSet(L.doorbell, L.set) != pdPASS) {
        (void)uart_driver_delete(uart);
        return ESP_FAIL;
    }
    err = uart_param_config(uart, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(uart, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        (void)uart_driver_delete(uart);
        return err;
    }
    zw_trace_start(); /* development builds: the frame trace's lock */
    if (xTaskCreatePinnedToCore(link_task, "zw_link", LINK_STACK, NULL, LINK_PRIORITY, &L.task,
                                tskNO_AFFINITY) != pdPASS) {
        (void)uart_driver_delete(uart);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool zw_link_write(const uint8_t *bytes, size_t len)
{
    if (L.to_uart == NULL || bytes == NULL || len == 0u || len > ZW_RX_BUF_SIZE) {
        return false;
    }
    if (xMessageBufferSend(L.to_uart, bytes, len, pdMS_TO_TICKS(WRITE_WAIT_MS)) != len) {
        L.stats.tx_lost++;
        return false;
    }
    (void)xSemaphoreGive(L.doorbell);
    return true;
}

size_t zw_link_read(uint8_t *kind, uint8_t *bytes, size_t cap)
{
    uint8_t msg[ZW_LINK_NEWS_MAX];
    if (L.to_zwave == NULL) {
        return 0u;
    }
    const size_t n = xMessageBufferReceive(L.to_zwave, msg, sizeof msg, 0);
    if (n < 2u || n - 1u > cap) {
        return 0u;
    }
    *kind = msg[0];
    memcpy(bytes, &msg[1], n - 1u);
    return n - 1u;
}

void zw_link_trace(bool on)
{
    atomic_store(&L.trace, on);
}

bool zw_link_tracing(void)
{
    return atomic_load(&L.trace);
}

void zw_link_stats(zw_link_stats_t *out)
{
    *out = L.stats;
}

void zw_link_stack(uint32_t *free_bytes, uint32_t *size)
{
    const TaskHandle_t t = L.task;
    *free_bytes = t != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(t) : 0u;
    *size = t != NULL ? (uint32_t)LINK_STACK : 0u;
}

#endif /* CONFIG_ALARM_ZSRC_ZWAVE */
