#pragma once
/* Small Panel Manager decisions kept free of ESP-IDF, so the PC tests them
 * (test/test_web_rules.c): attaching a JSON child, which events a viewer's Activity shows, and
 * sending one WebSocket message within its time. */
#include "alarm/alarm_config.h"
#include "cJSON.h"
#include "event_log/elog.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Puts child into parent: under key in an object, or appended to an array when key is NULL.
 * false - no parent, no child, or no memory for the key - and child is deleted, so a builder
 * never loses a tree it made (code review M01). A builder that sees false answers "Memory
 * busy" instead of a partial document. */
bool web_attach(cJSON *parent, const char *key, cJSON *child);

/* Does a viewer whose partitions are mine (viewer_partitions(): bit p-1 = partition p, 0xFF for
 * the installer and the system master) see this event in GET history / activity (docs/features/31
 * WEB-016, code review S06)? A partition's events: only that partition's viewers. The
 * system's own (partition 0: power, restarts, the clock, users): everyone, as the touchscreen's
 * Activity shows them to anyone at the panel and the Troubles list counts the system's for
 * everyone - unless it names a zone (not a device) whose partition isn't the viewer's. Silent
 * and duress events are a separate rule (notify_words()). */
bool web_history_visible(const alarm_config_t *cfg, const alarm_event_t *ev, uint8_t mine);

/* An elog_filter_fn over web_history_visible(); ctx is a web_history_view_t. Records that
 * aren't events pass, as without a filter (the page skips them itself). */
typedef struct {
    const alarm_config_t *cfg;
    uint8_t mine;
} web_history_view_t;
bool web_history_keep(void *ctx, const elog_entry_t *entry);

/* One message out over a connection that may take only part of it per write (code review
 * PERF11). */
typedef struct {
    /* Sends up to n bytes: how many went (> 0), 0 when none could go now (would block), < 0
     * when the connection is broken. One call blocks at most the transport's own timeout. */
    int (*write)(void *ctx, const uint8_t *p, size_t n);
    int64_t (*now_us)(void *ctx); /* a monotonic clock */
    bool (*stopping)(void *ctx);  /* the server is being stopped */
    void (*pause)(void *ctx);     /* between two tries that sent nothing */
    void *ctx;
    /* The most one write is handed (0: no limit). A TLS write keeps going until everything it
     * was handed has gone, so its time grows with what it holds; a small piece - one TLS
     * record - keeps one write short. */
    size_t piece;
} web_send_io_t;
/* true: all n bytes went. false: the connection broke, a stop was asked, or give_up_us (on
 * now_us's clock) passed. Both are checked before every write - the first, and each after a
 * part went - and a write is handed at most io->piece bytes, so a message takes at most
 * give_up_us plus one piece's write. On false the caller closes the connection: a WebSocket
 * frame sent in part can't be followed by anything. */
bool web_send_all(const web_send_io_t *io, const uint8_t *p, size_t n, int64_t give_up_us);
