/*
 * web_signin_gate.h - how often a password (or a phone's signature) may be tried, per source
 * address (F8, docs/features/31). One gate for everyone let someone on the home network keep the
 * owner out of the Panel Manager for 15 minutes with a wrong guess now and then. Each source now
 * has its own spacing and its own count (five free tries, then a minute's wait doubling to 15),
 * and all sources together share a count with a lower ceiling (a wait of at most a minute), so
 * guessing from many addresses is slowed without one address locking out the rest.
 * Portable: time comes in as an argument (host test: test_web_signin_gate.c). HTTP worker only.
 */
#ifndef WEB_SIGNIN_GATE_H
#define WEB_SIGNIN_GATE_H

#include <stdbool.h>
#include <stdint.h>

#define SIGNIN_SOURCES        8u
#define SIGNIN_SPACING_US     2000000LL           /* between two tries from one source */
#define SIGNIN_FREE_TRIES     5u                  /* wrong ones before a source waits */
#define SIGNIN_HOLD_US        (60LL * 1000000)    /* its first wait, doubling */
#define SIGNIN_HOLD_MAX       (15LL * 60 * 1000000)
#define SIGNIN_FORGET_US      (60LL * 60 * 1000000) /* an hour without a wrong one forgets them */
#define SIGNIN_SHARED_FREE    20u                 /* wrong ones from every source together */
#define SIGNIN_SHARED_HOLD_US (5LL * 1000000)     /* their first wait, doubling */
#define SIGNIN_SHARED_MAX     (60LL * 1000000)

typedef struct {
    uint8_t addr[16];
    bool used;
    int64_t seen_us, next_try_us;
    unsigned fails;
    int64_t last_fail_us, hold_until_us;
} signin_source_t;

typedef struct {
    signin_source_t src[SIGNIN_SOURCES];
    unsigned shared_fails;
    int64_t shared_last_fail_us, shared_hold_until_us;
} signin_gate_t;

typedef enum {
    SIGNIN_OK = 0,
    SIGNIN_TOO_SOON, /* this source tried less than SIGNIN_SPACING_US ago */
    SIGNIN_HELD,     /* this source, or every source together, guessed wrong too often */
} signin_verdict_t;

/* May addr try now? spaced: keep SIGNIN_SPACING_US between this source's tries (a sign-in, a
 * guarded change); a try counts toward the spacing whether or not it was right. */
signin_verdict_t signin_gate_try(signin_gate_t *g, const uint8_t addr[16], int64_t now_us,
                                 bool spaced);
/* addr's try was wrong. */
void signin_gate_failed(signin_gate_t *g, const uint8_t addr[16], int64_t now_us);

#endif
