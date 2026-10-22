/* web_setup_usb.c - setting up a panel without a screen over USB (docs/features/42; the Flasher,
 * docs/features/34 FLS-035; the owner 2026-10-21): the installer who just installed the firmware
 * from a laptop gives it the installer's access and the home Wi-Fi there, with no phone. The same
 * steps as the phone's (web_setup.c's actions, as the installer on the setup Wi-Fi), one line each:
 *
 *   provision accounts "<installer name>" "<password>"   -> PROVISION ok key <recovery key>
 *   provision network "<ssid>" "<password>"               -> PROVISION ok   (the key counts as kept)
 *   provision status                                      -> PROVISION status <stage> <wifi> <ip>
 *   provision finish                                      -> PROVISION ok <ip>   (once it joined)
 *   provision cancel                                      -> PROVISION ok
 *
 * Every answer is one line starting "PROVISION "; a refusal is "PROVISION error <word>" with the
 * Panel Manager's error word. Arguments are split as the console splits them (quotes, \" escapes).
 * Only on a headless board, and accounts only while it is unclaimed - the same rule as the setup
 * Wi-Fi: whoever holds the board claims it. Passwords are never printed or logged.
 *
 * Development builds take it as the console's `web provision ...`; a release build, which has no
 * service console, reads these lines from the USB port itself while setup isn't finished
 * (web_setup_usb_start). */
#include "web_setup.h"

#include <stdio.h>
#include <string.h>

#include "alarm/alarm_snapshot.h"
#include "alarm_service.h"
#include "cJSON.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "sdkconfig.h"
#include "web_mem.h"

static const char *TAG = "setup_usb";

static bool locked_now(void)
{
    alarm_snapshot_t *snap = web_alloc(sizeof(*snap));
    if (!snap)
        return true;
    alarm_service_get_snapshot(snap);
    const bool locked = alarm_snapshot_locked(snap);
    free(snap);
    return locked;
}

/* One action as the installer on the setup Wi-Fi; out (may be NULL) the answer, freed by the caller. */
static bool run(const char *op, const char *k1, const char *v1, const char *k2, const char *v2,
                cJSON **out, const char **why)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        *why = "setup_try_again";
        return false;
    }
    cJSON_AddStringToObject(o, "op", op);
    if (k1)
        cJSON_AddStringToObject(o, k1, v1);
    if (k2)
        cJSON_AddStringToObject(o, k2, v2);
    const bool claim = !strcmp(op, "claim") || !strcmp(op, "accounts");
    const char *status = "";
    cJSON *r = web_setup_action(o, claim ? 0u : ALARM_USER_INSTALLER,
                                claim ? ALARM_AUTH_NONE : ALARM_AUTH_INSTALLER, true, locked_now(),
                                &status, why);
    for (cJSON *c = o->child; c; c = c->next)
        if (cJSON_IsString(c) && c->valuestring)
            mbedtls_platform_zeroize(c->valuestring, strlen(c->valuestring));
    cJSON_Delete(o);
    if (out)
        *out = r;
    else
        cJSON_Delete(r);
    return r != NULL;
}

static const char *field(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

int web_setup_provision(int argc, char **argv)
{
    const char *why = "bad_request";
    const char *op = argc >= 1 ? argv[0] : "";
    cJSON *out = NULL;
    /* Only while setup isn't finished: afterwards the USB port changes nothing (the Panel
     * Manager, signed in, does). */
    cJSON *now = web_setup_status(true, 0u);
    const bool done = !strcmp(field(now, "stage"), "ready") || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(now, "setup"));
    cJSON_Delete(now);
    if (done && strcmp(op, "status") != 0) {
        printf("PROVISION error setup_stage\n");
        return 1;
    }
    if (!strcmp(op, "accounts") && argc == 3) {
        /* claim, the installer's access (the household's comes later, from People), the key */
        cJSON *c = NULL;
        if (run("claim", NULL, NULL, NULL, NULL, &c, &why)) {
            cJSON *o = cJSON_CreateObject();
            if (o) {
                cJSON_AddStringToObject(o, "op", "accounts");
                cJSON_AddStringToObject(o, "claim", field(c, "claim"));
                cJSON_AddStringToObject(o, "installer", argv[1]);
                cJSON_AddStringToObject(o, "installer_password", argv[2]);
                const char *status = "";
                cJSON *r = web_setup_action(o, 0u, ALARM_AUTH_NONE, true, locked_now(), &status, &why);
                cJSON *pw = cJSON_GetObjectItemCaseSensitive(o, "installer_password");
                if (pw && pw->valuestring)
                    mbedtls_platform_zeroize(pw->valuestring, strlen(pw->valuestring));
                cJSON_Delete(o);
                if (r) {
                    cJSON_Delete(r);
                    if (run("key", NULL, NULL, NULL, NULL, &out, &why)) {
                        printf("PROVISION ok key %s\n", field(out, "key"));
                        cJSON *k = cJSON_GetObjectItemCaseSensitive(out, "key");
                        if (k && k->valuestring)
                            mbedtls_platform_zeroize(k->valuestring, strlen(k->valuestring));
                        cJSON_Delete(out);
                        cJSON_Delete(c);
                        ESP_LOGI(TAG, "installer access created over USB");
                        return 0;
                    }
                }
            }
        }
        cJSON_Delete(c);
    } else if (!strcmp(op, "network") && argc == 3) {
        cJSON *st = web_setup_status(true, ALARM_USER_INSTALLER);
        const bool owed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "key_pending"));
        cJSON_Delete(st);
        if ((!owed || run("key-saved", NULL, NULL, NULL, NULL, NULL, &why)) &&
            run("network", "ssid", argv[1], "password", argv[2], NULL, &why)) {
            printf("PROVISION ok\n");
            return 0;
        }
    } else if (!strcmp(op, "finish") && argc == 1) {
        if (run("confirm", NULL, NULL, NULL, NULL, &out, &why)) {
            printf("PROVISION ok %s\n", field(out, "ip"));
            cJSON_Delete(out);
            return 0;
        }
    } else if (!strcmp(op, "cancel") && argc == 1) {
        if (run("cancel", NULL, NULL, NULL, NULL, NULL, &why)) {
            printf("PROVISION ok\n");
            return 0;
        }
    } else if (!strcmp(op, "status") && argc == 1) {
        cJSON *st = web_setup_status(true, ALARM_USER_INSTALLER);
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(st, "wifi");
        printf("PROVISION status %s %s %s\n", field(st, "stage"),
               w ? field(w, "state") : "none", w && field(w, "ip")[0] ? field(w, "ip") : "-");
        cJSON_Delete(st);
        return 0;
    }
    printf("PROVISION error %s\n", why);
    return 1;
}

#if !CONFIG_ALARM_SERVICE_CONSOLE && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

/* A release build: the USB port's lines, read here while setup isn't finished. Its stack is
 * internal RAM: the steps write flash (the accounts, the network). */
static void usb_task(void *arg)
{
    (void)arg;
    static char line[256];
    size_t n = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(1000)) != 1) {
            continue;
        }
        if (ch != '\n' && ch != '\r') {
            if (n + 1 < sizeof(line))
                line[n++] = (char)ch;
            continue;
        }
        line[n] = '\0';
        n = 0;
        if (strncmp(line, "provision ", 10) != 0)
            continue;
        char *argv[6];
        const size_t argc = esp_console_split_argv(line + 10, argv, 6);
        (void)web_setup_provision((int)argc, argv);
        mbedtls_platform_zeroize(line, sizeof(line));
    }
}

void web_setup_usb_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK)
        return;
    usb_serial_jtag_vfs_use_driver(); /* the log keeps going out through the driver */
    if (xTaskCreate(usb_task, "setup_usb", 8192, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "setup over USB unavailable");
}
#else
void web_setup_usb_start(void) {}
#endif
