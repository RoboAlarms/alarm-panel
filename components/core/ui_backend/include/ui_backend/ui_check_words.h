/*
 * ui_check_words.h - the connection check's words (docs/features/09, docs/features/37 G3): one
 * English copy for the check on the panel (net_check.c) and the PC preview's pretend network.
 * Each is a pattern with {0} and {1} for the values (a network's name, an address), marked for
 * the catalog: the console and the Panel Manager get it filled in in English, the touchscreen
 * looks the pattern up in its language and fills in the same values.
 */
#ifndef UI_BACKEND_UI_CHECK_WORDS_H
#define UI_BACKEND_UI_CHECK_WORDS_H

#include <stddef.h>

#include "i18n/i18n.h"

/* A step's line, its words filled with its values: the console and the Panel Manager ask for it
 * in English (words as stored); the screen passes its own language's words. No words: "". */
static inline size_t ui_check_line(char *out, size_t len, const char *words, const char *v0,
                                   const char *v1)
{
    if (words == NULL) {
        if (len > 0u) {
            out[0] = '\0';
        }
        return 0u;
    }
    return I18N_FORMAT(out, len, words, v0, v1);
}

/* TR: the connection check, a step's line under its name; {0} and {1} are a network's name, an
 * address, a server or a number, up to 47 bytes each. At most 71 bytes */
#define UI_CHECK_W_JOINED          I18N_NOOP_C("connection check", "Joined {0}")
#define UI_CHECK_W_MODULE_SILENT   I18N_NOOP_C("connection check", "The Wi-Fi module isn't answering")
#define UI_CHECK_W_NO_NETWORK      I18N_NOOP_C("connection check", "No network chosen yet")
#define UI_CHECK_W_PASSWORD        I18N_NOOP_C("connection check", "{0} refused the password")
#define UI_CHECK_W_NO_ANSWER       I18N_NOOP_C("connection check", "{0} didn't answer")
#define UI_CHECK_W_NOT_JOINED      I18N_NOOP_C("connection check", "Not joined to {0} yet")
#define UI_CHECK_W_FIXED_NOT_TAKEN I18N_NOOP_C("connection check", "The fixed address wasn't taken up")
#define UI_CHECK_W_NO_LEASE        I18N_NOOP_C("connection check", "The router gave out no address (DHCP)")
#define UI_CHECK_W_ADDRESS_FIXED   I18N_NOOP_C("connection check", "{0} · {1} · fixed")
#define UI_CHECK_W_ADDRESS_LEASED  I18N_NOOP_C("connection check", "{0} · {1} · from the router")
#define UI_CHECK_W_NO_ROUTER       I18N_NOOP_C("connection check", "No router address")
#define UI_CHECK_W_ANSWERED        I18N_NOOP_C("connection check", "{0} answered")
#define UI_CHECK_W_NO_PING         I18N_NOOP_C("connection check", "Couldn't send a ping from the panel")
#define UI_CHECK_W_WRONG_ROUTER    I18N_NOOP_C("connection check", "{0} didn't answer: is that the right router?")
#define UI_CHECK_W_RESOLVED        I18N_NOOP_C("connection check", "{0} is {1}")
#define UI_CHECK_W_DNS_SILENT      I18N_NOOP_C("connection check", "{0} didn't answer for {1}")
#define UI_CHECK_W_NO_DNS          I18N_NOOP_C("connection check", "No name server: couldn't look up {0}")
#define UI_CHECK_W_PORT_ANSWERED   I18N_NOOP_C("connection check", "{0} answered on port {1}")
#define UI_CHECK_W_NOTHING_OUT     I18N_NOOP_C("connection check", "Nothing answered out there (tried {0} and {1})")
#define UI_CHECK_W_NOT_FOUND       I18N_NOOP_C("connection check", "{0} couldn't be looked up")
#define UI_CHECK_W_NTP_SILENT      I18N_NOOP_C("connection check", "{0} didn't answer (port 123)")
#define UI_CHECK_W_CLOCK_OUT       I18N_NOOP_C("connection check", "{0} answered; the clock is {1} s out")
#define UI_CHECK_W_CLOCK_RIGHT     I18N_NOOP_C("connection check", "{0} answered; the clock is right")
#define UI_CHECK_W_NO_SERVICE      I18N_NOOP_C("connection check", "No service is set up")
#define UI_CHECK_W_NOTHING_AT      I18N_NOOP_C("connection check", "Nothing answered at {0}")
#define UI_CHECK_W_OFF_NETWORK     I18N_NOOP_C("connection check", "The panel isn't on the network")
#define UI_CHECK_W_NO_ADDRESS      I18N_NOOP_C("connection check", "The panel has no address yet")

#endif /* UI_BACKEND_UI_CHECK_WORDS_H */
