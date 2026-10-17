/*
 * ota_console.c - "ota" and "c6" service console commands, and the serial image upload
 * (OTA-013) used by tools/ota_serial.py.
 *
 * Upload protocol (one line each way, stop-and-wait, so a busy panel can't lose data):
 *   host: ota recv <size> <sha256 hex> [force]
 *   panel: OTA READY <max bytes per line>
 *   host: <seq> <crc32 hex8> <base64>        (seq from 0; the CRC covers the decoded bytes)
 *   panel: OTA OK <seq>  |  OTA RETRY <expected seq>
 *   host: END (or ABORT)
 *   panel: OTA DONE <version>  |  OTA FAIL <reason>
 * Anything else the panel prints (log lines) is to be ignored by the host.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_proto/proto_fw.h"
#include "alarm_proto/proto_fwsrv.h"
#include "alarm_service.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "panel_ota_internal.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#define HAVE_UART_UPLOAD 1
#else
#define HAVE_UART_UPLOAD 0
#endif

static const char *TAG = "ota_con";

#define CHUNK_MAX     1024u /* decoded bytes per line */
#define UPLOAD_LINE   (16u + 1u + 8u + 1u + (CHUNK_MAX + 2u) / 3u * 4u + 8u)
#define LINE_TIMEOUT  30000u
#define UART_RX_BUF   8192

static const alarm_source_t CONSOLE = {.kind = ALARM_SRC_KEYPAD, .id = ALARM_KEYPAD_CONSOLE};

static const char *const PHASE[] = {"idle", "checking", "receiving", "waiting to restart",
                                    "restarting", "failed"};
/* panel_ota_stage_t, panel_ota_kind_t and panel_ota_last_result_t, in their order. */
static const char *const STAGE[] = {"",         "downloading", "verifying",    "waiting",
                                    "installing", "stopping",  "rolling back", "Wi-Fi module",
                                    "developer file"};
static const char *const KIND[] = {"", "release", "developer file", "Wi-Fi module",
                                   "from the alarm panel"};
static const char *const LAST[] = {"", "updated", "rolled back", "stopped", "failed"};

static const char *name_of(const char *const *names, size_t count, unsigned i)
{
    return i < count ? names[i] : "?";
}
#define NAME(table, i) name_of(table, sizeof(table) / sizeof(table[0]), (unsigned)(i))

static void print_status(void)
{
    panel_ota_status_t st;
    panel_ota_get_status(&st);
    printf("Firmware:      %s%s\n", st.running, st.probation ? " (self-test running)" : "");
    printf("Update server: %s%s, channel %s\n", st.server[0] ? st.server : "(none)",
           st.official ? " (this build's)" : "", st.channel);
    if (st.installed_channel[0] || st.withdrawn) {
        printf("Running on:    %s%s\n", st.installed_channel[0] ? st.installed_channel : "-",
               st.withdrawn ? " (withdrawn by its publisher)" : "");
    }
    if (st.latest[0]) {
        printf("Latest:        %s%s%s\n", st.latest, st.available ? " (update available)" : "",
               st.critical ? ", an important fix" : "");
    }
    printf("State:         %s", PHASE[st.phase]);
    if (st.phase == PANEL_OTA_RECEIVING && st.percent >= 0) {
        printf(" %d%%", st.percent);
    }
    if (st.phase == PANEL_OTA_WAITING && st.waiting_for[0]) {
        printf(" (%s)", st.waiting_for);
    }
    if (st.phase == PANEL_OTA_FAILED) {
        printf(": %s", st.error);
    }
    printf("\n");
    if (st.ready[0]) {
        printf("Ready:         %s (verified, not started)\n", st.ready);
    }
    if (st.rolled_back[0]) {
        printf("Rolled back:   the update to %s failed its self-test\n", st.rolled_back);
    }
    if (st.stage != PANEL_OTA_STAGE_NONE) { /* OTA-026 */
        printf("Stage:         %s (%s) %s%s%s\n", NAME(STAGE, st.stage), NAME(KIND, st.kind),
               st.previous[0] ? st.previous : "?", st.target[0] ? " -> " : "", st.target);
        if (st.stage == PANEL_OTA_STAGE_DOWNLOADING) {
            printf("Downloaded:    %" PRIu32 " of %" PRIu32 " bytes\n", st.bytes, st.size);
        }
        if (st.reason[0]) {
            printf("Held by:       %s\n", st.reason);
        }
    } else if (st.previous[0]) {
        printf("Self-test:     a failure returns to %s\n", st.previous);
    }
    if (st.last.result != PANEL_OTA_LAST_NONE) { /* OTA-029 */
        printf("Last:          %s %s (from %s, at %" PRIu32 ", by %u/%u user %u)%s%s\n",
               NAME(LAST, st.last.result), st.last.version, st.last.from[0] ? st.last.from : "?",
               st.last.at, (unsigned)st.last.by_src.kind, (unsigned)st.last.by_src.id,
               (unsigned)st.last.by_user, st.last.reason[0] ? ": " : "", st.last.reason);
        if (st.last.failed != 0u) {
            printf("Self-test:     missed 0x%02" PRIx32 "%s\n", st.last.failed,
                   (st.last.failed & PANEL_OTA_OK_KEYPADS) != 0u
                       ? " (no keypad answered during its self-test)"
                       : "");
        }
    }
    /* A keypad of the panel link (docs/features/11): its cap and its master's orders. */
    char cap[32];
    char master[32];
    const proto_fw_cap_t kind = panel_ota_get_version_cap(cap, master);
    if (kind != PROTO_FW_CAP_NONE) {
        printf("Updates from:  %s%s%s\n", master[0] ? master : "the alarm panel",
               kind == PROTO_FW_CAP_AT ? ", at most " : " (its version not heard yet)",
               kind == PROTO_FW_CAP_AT ? cap : "");
    }
    panel_ota_order_status_t o;
    panel_ota_order_get(&o);
    if (o.active || o.why != 0u) {
        printf("Ordered:       %s %s%s%s, why %u\n", o.relay ? "relay" : "fetch",
               o.version[0] ? o.version : "-", o.ready ? ", ready" : "",
               o.install ? ", install" : "", (unsigned)o.why);
    }
    if (o.probed || o.probing) {
        printf("Probe:         %s, why %u%s%s%s\n", o.probing ? "asking" : "answered",
               (unsigned)o.probe_why, o.offered[0] ? ", offered " : "", o.offered,
               o.offered[0] && !o.key_trusted ? " (a key this panel doesn't trust)" : "");
    }
}

/* ---- serial upload --------------------------------------------------------------------------- */

#if HAVE_UART_UPLOAD
static const uart_port_t PORT = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;

/* The console's receive buffer (256 bytes) is smaller than one upload line. */
static void grow_rx_buffer(void)
{
    static bool done;
    if (done) {
        return;
    }
    uart_vfs_dev_use_nonblocking(PORT); /* output keeps working while the driver is replaced */
    uart_driver_delete(PORT);
    if (uart_driver_install(PORT, UART_RX_BUF, 0, 0, NULL, 0) == ESP_OK) {
        done = true;
    } else {
        uart_driver_install(PORT, 256, 0, 0, NULL, 0);
    }
    uart_vfs_dev_use_driver(PORT);
}

static uint8_t s_rx[256];
static size_t s_rx_len;
static size_t s_rx_pos;

/* Next received byte, or -1 when nothing arrives within wait. */
static int next_byte(TickType_t wait)
{
    if (s_rx_pos >= s_rx_len) {
        size_t avail = 0;
        uart_get_buffered_data_len(PORT, &avail);
        const size_t want = avail == 0u ? 1u : (avail < sizeof(s_rx) ? avail : sizeof(s_rx));
        const int got = uart_read_bytes(PORT, s_rx, (uint32_t)want, wait);
        if (got <= 0) {
            return -1;
        }
        s_rx_len = (size_t)got;
        s_rx_pos = 0;
    }
    return s_rx[s_rx_pos++];
}

/* One line without its end, or false after timeout_ms without a complete line. */
static bool read_line(char *out, size_t cap, uint32_t timeout_ms)
{
    size_t n = 0;
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        const int c = next_byte(pdMS_TO_TICKS(100));
        if (c == '\n') {
            out[n] = '\0';
            return true;
        }
        if (c >= 0) {
            if (c != '\r' && n + 1u < cap) {
                out[n++] = (char)c;
            }
            continue;
        }
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_sha(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64u) {
        return false;
    }
    for (size_t i = 0; i < 32u; i++) {
        const int hi = hex_nibble(hex[2u * i]);
        const int lo = hex_nibble(hex[2u * i + 1u]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static int receive(uint32_t size, const uint8_t sha[32], bool force)
{
    grow_rx_buffer();
    const esp_err_t err = panel_ota_begin(size, sha, force);
    if (err != ESP_OK) {
        printf("OTA FAIL %s\n", err == ESP_ERR_INVALID_STATE ? "busy or not confirmed yet"
                                                            : esp_err_to_name(err));
        return 1;
    }
    char *line = malloc(UPLOAD_LINE);
    uint8_t *data = malloc(CHUNK_MAX);
    if (line == NULL || data == NULL) {
        free(line);
        free(data);
        panel_ota_abort();
        printf("OTA FAIL out of memory\n");
        return 1;
    }
    const esp_log_level_t level = esp_log_get_default_level();
    esp_log_level_set("*", ESP_LOG_WARN); /* keep the link quiet (no per-tag levels in use) */
    uart_flush_input(PORT);
    s_rx_len = 0;
    s_rx_pos = 0;
    printf("OTA READY %u\n", (unsigned)CHUNK_MAX);
    fflush(stdout);

    uint32_t expected = 0;
    int rc = 1;
    char why[64] = "";
    char version[32] = "";
    for (;;) {
        if (!read_line(line, UPLOAD_LINE, LINE_TIMEOUT)) {
            panel_ota_abort();
            printf("OTA FAIL timeout\n");
            break;
        }
        if (strcmp(line, "END") == 0) {
            if (panel_ota_end(version, sizeof(version), why, sizeof(why)) == ESP_OK) {
                printf("OTA DONE %s\n", version);
                rc = 0;
            } else {
                printf("OTA FAIL %s\n", why);
            }
            break;
        }
        if (strcmp(line, "ABORT") == 0) {
            panel_ota_abort();
            printf("OTA FAIL aborted\n");
            break;
        }
        uint32_t seq = 0;
        const size_t n = proto_fw_line_decode(line, &seq, data, CHUNK_MAX);
        if (n == SIZE_MAX || seq > expected) {
            printf("OTA RETRY %" PRIu32 "\n", expected);
        } else if (seq < expected) {
            printf("OTA OK %" PRIu32 "\n", seq); /* our answer got lost: already written */
        } else if (panel_ota_write(data, n, why, sizeof(why)) != ESP_OK) {
            printf("OTA FAIL %s\n", why);
            break;
        } else {
            expected++;
            printf("OTA OK %" PRIu32 "\n", seq);
        }
        fflush(stdout);
    }
    fflush(stdout);
    esp_log_level_set("*", level);
    free(line);
    free(data);
    if (rc == 0) {
        ESP_LOGI(TAG, "uploaded %s: restarting when the system is idle", version);
        panel_ota_apply(&CONSOLE, 0);
    }
    return rc;
}
#endif

/* ---- commands -------------------------------------------------------------------------------- */

static const char *const SRV_RESULT[] = {"OK", "can't be reached", "not a firmware server",
                                          "no firmware for this panel", "not a domain"};

static void print_server(void)
{
    panel_ota_server_t srv;
    panel_ota_get_server(&srv);
    char fp[PROTO_FWSRV_FINGERPRINT_LEN] = "-";
    if (srv.has_key) {
        proto_fwsrv_fingerprint(srv.key_sha256, fp);
    }
    printf("Update server: %s%s\n", srv.domain[0] ? srv.domain : "(none)",
           srv.official ? " (this build's)" : " (not this build's)");
    printf("Signing key:   %s (%s)\n", srv.key_name[0] ? srv.key_name : "name not known", fp);
    panel_ota_srv_check_t c;
    panel_ota_server_check_get(&c);
    if (c.state == PANEL_OTA_SRV_CHECKING) {
        printf("Checking:      %s\n", c.domain);
    } else if (c.state == PANEL_OTA_SRV_DONE) {
        printf("Checked:       %s: %s\n", c.domain[0] ? c.domain : "-", NAME(SRV_RESULT, c.result));
        if (c.result == PANEL_OTA_SRV_OK) {
            proto_fwsrv_fingerprint(c.key_sha256, fp);
            printf("  its key:     %s (%s)%s\n", c.key_name[0] ? c.key_name : "no name", fp,
                   c.trusted ? ", trusted already" : ", NOT trusted yet");
            printf("  keep it:     ota server use\n");
        }
    }
}

/* ota server [check <domain|http(s) address> | use] (OTA-017, OTA-020) */
static int cmd_server(int argc, char **argv)
{
    if (argc < 3) {
        print_server();
        return 0;
    }
    if (strcmp(argv[2], "check") == 0 && argc > 3) {
        char domain[PROTO_FWSRV_DOMAIN_MAX];
        const bool is_domain = proto_fwsrv_domain(argv[3], domain, sizeof(domain));
        const esp_err_t err = is_domain ? panel_ota_server_check(argv[3])
                                        : panel_ota_server_check_dev(argv[3]);
        if (err == ESP_ERR_INVALID_ARG) {
            printf("Not a domain or an http(s) address\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("Not while an update runs or self-tests\n");
            return 1;
        }
        printf("Checking %s (see 'ota server')\n", argv[3]);
        return 0;
    }
    if (strcmp(argv[2], "use") == 0) {
        panel_ota_srv_check_t c;
        panel_ota_server_check_get(&c);
        if (c.state != PANEL_OTA_SRV_DONE || c.result != PANEL_OTA_SRV_OK) {
            printf("No finished check to keep: ota server check <domain>\n");
            return 1;
        }
        /* The console isn't held back by the factory installer code (OTA-021). */
        if (panel_ota_server_commit(c.domain, c.key_sha256, false, &CONSOLE, 0) != ESP_OK) {
            printf("Not kept: an update runs, or the check changed\n");
            return 1;
        }
        printf("Update server %s kept; checking it now\n", c.domain);
        return 0;
    }
    printf("usage: ota server [check <domain|http(s) address> | use]\n");
    return 1;
}

static int cmd_ota(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "server") == 0) {
        return cmd_server(argc, argv);
    }
    if (strcmp(sub, "channel") == 0) {
        if (argc < 3) {
            panel_ota_status_t st;
            panel_ota_get_status(&st);
            printf("Channel: %s\n", st.channel);
            return 0;
        }
        const esp_err_t err = panel_ota_set_channel(argv[2], &CONSOLE, 0);
        if (err == ESP_ERR_INVALID_ARG) {
            printf("usage: ota channel [stable|beta|alpha|dev]\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("Not while an update runs or self-tests\n");
            return 1;
        }
        printf("Channel %s; checking now\n", argv[2]);
        return 0;
    }
    if (strcmp(sub, "check") == 0) {
        panel_ota_check();
        printf("Checking (see 'ota')\n");
        return 0;
    }
    if (strcmp(sub, "install") == 0) {
        const bool force = argc > 2 && strcmp(argv[2], "force") == 0;
        const esp_err_t err = panel_ota_install(&CONSOLE, 0, force);
        if (err == ESP_ERR_NOT_ALLOWED) { /* never past its master (docs/features/11 LINK-029) */
            char words[96];
            panel_ota_cap_words(words, sizeof(words));
            printf("%s\n", words);
            return 1;
        }
        if (err != ESP_OK) {
            printf("An update is already running\n");
            return 1;
        }
        printf("Installing the latest release; the panel restarts when the system is idle\n");
        return 0;
    }
    if (strcmp(sub, "apply") == 0) {
        if (panel_ota_apply(&CONSOLE, 0) != ESP_OK) {
            printf("No verified image waiting\n");
            return 1;
        }
        printf("Restarting into the new image when the system is idle\n");
        return 0;
    }
    if (strcmp(sub, "cancel") == 0) {
        /* A release update is stopped (OTA-027); a developer's file waiting to restart is
         * cancelled as before. */
        const esp_err_t err = panel_ota_stop(&CONSOLE, 0);
        if (err == ESP_OK) {
            printf("Stopping the update\n");
            return 0;
        }
        panel_ota_cancel();
        printf("The waiting image stays unused\n");
        return 0;
    }
    if (strcmp(sub, "recv") == 0) {
#if HAVE_UART_UPLOAD
        uint8_t sha[32];
        char *end = NULL;
        const unsigned long size = argc > 3 ? strtoul(argv[2], &end, 10) : 0u;
        if (argc < 4 || end == NULL || *end != '\0' || size == 0u || size > UINT32_MAX ||
            !parse_sha(argv[3], sha)) {
            printf("usage: ota recv <size> <sha256 hex> [force]  (use tools/ota_serial.py)\n");
            return 1;
        }
        return receive((uint32_t)size, sha, argc > 4 && strcmp(argv[4], "force") == 0);
#else
        printf("Serial upload needs the UART console\n");
        return 1;
#endif
    }
    printf("usage: ota [status | server [check <domain>|use] | channel [name] | check |\n"
           "            install [force] | apply | cancel | recv <size> <sha256> [force]]\n");
    return 1;
}

static int cmd_c6(int argc, char **argv)
{
    panel_ota_status_t st;
    panel_ota_get_status(&st);
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        printf("Wi-Fi co-processor: %s\n", st.cp_running[0] ? st.cp_running : "not answering");
        printf("Image in cp_fw:     %s\n", st.cp_image[0] ? st.cp_image : "none");
        return 0;
    }
    if (strcmp(argv[1], "update") == 0) {
        char why[64];
        const bool force = argc > 2 && strcmp(argv[2], "force") == 0;
        if (panel_ota_cp_update(&CONSOLE, 0, force, why, sizeof(why)) != ESP_OK) {
            printf("%s\n", why);
            return 1;
        }
        printf("Updating the Wi-Fi co-processor when the system is idle; the panel restarts\n");
        return 0;
    }
    printf("usage: c6 [status | update [force]]\n");
    return 1;
}

esp_err_t panel_ota_console_register(void)
{
    static const esp_console_cmd_t cmds[] = {
        {.command = "ota",
         .help = "Firmware updates: status, update server, channel, check, install, serial upload",
         .hint = "[status|server|channel|check|install|apply|cancel|recv]",
         .func = cmd_ota},
        {.command = "c6",
         .help = "Wi-Fi co-processor firmware: version, update from the cp_fw partition",
         .hint = "[status|update [force]]",
         .func = cmd_c6},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s",
                            cmds[i].command);
    }
    return ESP_OK;
}
