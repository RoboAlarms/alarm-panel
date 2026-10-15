/*
 * ui_why_words.h - the services' answers that carry a number (an HTTP status), one English copy
 * for the service that writes them and every place that shows them (docs/features/37 G3). Each is
 * a pattern with {0}, marked for the catalog: the service fills it in in English for the console,
 * the logs and MQTT; ui_why_line() says any answer - one of these, or one marked by itself - in
 * the screen's or a browser's language, the number kept.
 */
#ifndef UI_BACKEND_UI_WHY_WORDS_H
#define UI_BACKEND_UI_WHY_WORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "i18n/i18n.h"

/* TR: a notification service's answer (net_notify.c), in a toast or a status line; {0} is the
 * HTTP status the service answered (429). At most 60 bytes */
#define UI_WHY_PUSHOVER_ANSWERED I18N_NOOP_C("why", "Pushover answered {0}")
/* TR: see "Pushover answered {0}" */
#define UI_WHY_SERVICE_ANSWERED  I18N_NOOP_C("why", "The service answered {0}")
/* TR: a notification service's status line when the service didn't answer. At most 60 bytes */
#define UI_WHY_NO_ANSWER         I18N_NOOP_C("why", "No answer from the service")
/* TR: why a software update check failed (panel_ota.c); {0} is the HTTP status. At most 60
 * bytes */
#define UI_WHY_UPDATE_ANSWERED   I18N_NOOP_C("update", "The update server answered {0}")

/* The number where pattern has {0}, when text is pattern filled in with a whole number. */
static inline bool ui_why_number(const char *pattern, const char *text, char *num, size_t num_len)
{
    const char *hole = strstr(pattern, "{0}");
    if (hole == NULL) {
        return false;
    }
    const size_t pre = (size_t)(hole - pattern);
    const char *post = hole + 3;
    const size_t post_len = strlen(post);
    const size_t text_len = strlen(text);
    if (text_len <= pre + post_len || strncmp(text, pattern, pre) != 0 ||
        strcmp(text + text_len - post_len, post) != 0) {
        return false;
    }
    const size_t n = text_len - pre - post_len;
    if (n >= num_len) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char ch = text[pre + i];
        if (!((ch >= '0' && ch <= '9') || (i == 0u && ch == '-' && n > 1u))) {
            return false;
        }
    }
    memcpy(num, text + pre, n);
    num[n] = '\0';
    return true;
}

/* An answer why, marked in ctx ("why", "update"), in catalog c's language (NULL: English): a
 * marked answer by itself, one of the patterns above with its number, anything else as it came
 * (a service's own words). Returns strlen(out). */
static inline size_t ui_why_line(const i18n_catalog_t *c, const char *ctx, const char *why,
                                 char *out, size_t len)
{
    static const char *const NUMBERED[] = {UI_WHY_PUSHOVER_ANSWERED, UI_WHY_SERVICE_ANSWERED,
                                           UI_WHY_UPDATE_ANSWERED};
    static const char *const CONTEXTS[] = {"why", "why", "update"};
    if (why == NULL) {
        why = "";
    }
    const char *found = i18n_text(c, ctx, why);
    if (found == why) {
        for (size_t i = 0; i < sizeof(NUMBERED) / sizeof(NUMBERED[0]); i++) {
            char num[16];
            if (ctx != NULL && strcmp(CONTEXTS[i], ctx) == 0 &&
                ui_why_number(NUMBERED[i], why, num, sizeof(num))) {
                return I18N_FORMAT(out, len, i18n_text(c, ctx, NUMBERED[i]), num);
            }
        }
    }
    return i18n_utf8_copy(out, len, found);
}

#endif /* UI_BACKEND_UI_WHY_WORDS_H */
