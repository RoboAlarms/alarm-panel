/*
 * ui_calls.c - the screen's calls that wait for an answer from another panel (LINK-054).
 *
 * On a keypad a command, a code check, a code change, a device action or a partition switch is
 * answered by its master over the network: about 0.3 s, up to 3 s while packets are lost. The
 * screen never waits for that on its own task: each such call is queued here and handed, one at a
 * time, to the backend's run_later, which runs it on a task of its own; a timer on this task picks
 * up its end and runs the caller's continuation here. While a call a person made waits, a
 * transparent layer over the whole screen takes the taps, as the screen did when it waited itself:
 * nothing on the screen changes, and the clock, the status and the alarm's glow go on. A call a
 * timer makes with no one waiting for it (the partition's activity) takes no taps, and a person's
 * call goes before it in the queue.
 * Without run_later (the alarm panel's own engine, the PC builds) every call runs at once, its
 * continuation before the ui_call_* returns, as before.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define CALLS_MAX    4u  /* a person's call, the activity, a panic: never more at once */
#define CALL_POLL_MS 10u /* how soon a continuation runs after its answer */

typedef enum {
    OP_COMMAND,
    OP_AUTHENTICATE,
    OP_CHANGE_CODE,
    OP_PARTITION,
    OP_DEV_ACT,
} call_op_t;

typedef struct {
    ui_call_t c;
    ui_call_done_fn done;
    call_op_t op;
    uint32_t seq;
    bool used;
    bool person; /* someone waits for it: the taps wait too */
    bool handed; /* run_later has it (or it runs here) */
    atomic_bool finished;
} slot_t;

/* Read by the backend's task while it runs a call: PSRAM is fine, nothing here is an ISR's. */
static slot_t s_slots[CALLS_MAX] UI_SLOW_BSS;
static uint32_t s_seq;
static lv_timer_t *s_timer;
static lv_obj_t *s_shield;

/* On the backend's task (or this one, without run_later): only backend entries, no LVGL. */
static void call_work(void *arg)
{
    slot_t *s = arg;
    ui_call_t *c = &s->c;
    switch (s->op) {
    case OP_COMMAND: c->r = S.be.command(S.be.ctx, &c->cmd, &c->res); break;
    case OP_AUTHENTICATE:
        c->r = S.be.authenticate != NULL ? S.be.authenticate(S.be.ctx, c->code, &c->user)
                                         : ALARM_ERR_DISABLED;
        break;
    case OP_CHANGE_CODE:
        c->r = S.be.change_code(S.be.ctx, c->code, c->new_code, &c->user);
        break;
    case OP_PARTITION:
        c->r = S.be.partition_select != NULL
                   ? S.be.partition_select(S.be.ctx, c->partition, c->activity)
                   : ALARM_ERR_UNAVAILABLE;
        break;
    case OP_DEV_ACT:
        c->r = ALARM_OK;
        c->dev = S.be.dev_act != NULL ? S.be.dev_act(S.be.ctx, c->ref, c->action, c->value,
                                                     c->code[0] != '\0' ? c->code : NULL, &c->r)
                                      : UI_DEV_UNREACHABLE;
        break;
    }
    atomic_store_explicit(&s->finished, true, memory_order_release);
}

static slot_t *call_running(void)
{
    for (size_t i = 0; i < CALLS_MAX; i++) {
        if (s_slots[i].used && s_slots[i].handed) {
            return &s_slots[i];
        }
    }
    return NULL;
}

/* The next to hand over: a person's call first, then the oldest. */
static slot_t *call_next(void)
{
    slot_t *best = NULL;
    for (size_t i = 0; i < CALLS_MAX; i++) {
        slot_t *s = &s_slots[i];
        if (!s->used || s->handed) {
            continue;
        }
        if (best == NULL || (s->person && !best->person) ||
            (s->person == best->person && (int32_t)(s->seq - best->seq) < 0)) {
            best = s;
        }
    }
    return best;
}

bool ui_call_waiting(void)
{
    for (size_t i = 0; i < CALLS_MAX; i++) {
        if (s_slots[i].used && s_slots[i].person) {
            return true;
        }
    }
    return false;
}

static void shield_gone(lv_event_t *e)
{
    (void)e;
    s_shield = NULL;
}

/* The taps wait while a person's call does; the layer draws nothing. */
static void shield_update(void)
{
    const bool want = ui_call_waiting();
    if (want && s_shield == NULL) {
        s_shield = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_shield);
        lv_obj_set_size(s_shield, lv_pct(100), lv_pct(100));
        lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_shield, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_shield, shield_gone, LV_EVENT_DELETE, NULL);
    } else if (!want && s_shield != NULL) {
        lv_obj_delete(s_shield);
    }
}

static void call_pump(void);

/* Its answer is in: the slot is freed and the next handed over before the continuation runs. */
static void call_finish(slot_t *s)
{
    ui_call_t c = s->c;
    const ui_call_done_fn done = s->done;
    memset(&s->c, 0, sizeof(s->c)); /* the codes */
    s->used = false;
    s->handed = false;
    s->done = NULL;
    atomic_store_explicit(&s->finished, false, memory_order_relaxed);
    call_pump();
    shield_update();
    if (done != NULL) {
        done(&c);
    }
    memset(&c, 0, sizeof(c));
}

static void call_poll(lv_timer_t *t)
{
    slot_t *s = call_running();
    if (s != NULL && atomic_load_explicit(&s->finished, memory_order_acquire)) {
        call_finish(s);
    }
    if (call_running() == NULL && call_next() == NULL && t != NULL) {
        lv_timer_pause(t);
    }
}

static void call_pump(void)
{
    if (call_running() != NULL) {
        return;
    }
    slot_t *s = call_next();
    if (s == NULL) {
        return;
    }
    s->handed = true;
    if (S.be.run_later != NULL && S.be.run_later(S.be.ctx, call_work, s)) {
        if (s_timer == NULL) {
            s_timer = lv_timer_create(call_poll, CALL_POLL_MS, NULL);
        } else {
            lv_timer_resume(s_timer);
        }
        return;
    }
    call_work(s); /* here and now, as before */
    call_finish(s);
}

/* A free slot for op, or NULL (the caller says so or tries again later). */
static slot_t *call_new(call_op_t op, bool person, ui_call_done_fn done, uintptr_t tag)
{
    for (size_t i = 0; i < CALLS_MAX; i++) {
        slot_t *s = &s_slots[i];
        if (!s->used) {
            memset(&s->c, 0, sizeof(s->c));
            s->c.tag = tag;
            s->done = done;
            s->op = op;
            s->seq = ++s_seq;
            s->used = true;
            s->person = person;
            s->handed = false;
            atomic_store_explicit(&s->finished, false, memory_order_relaxed);
            return s;
        }
    }
    return NULL;
}

static bool call_start(slot_t *s)
{
    if (s == NULL) {
        return false;
    }
    call_pump();
    shield_update();
    return true;
}

static void code_copy(char *dst, size_t cap, const char *code)
{
    memset(dst, 0, cap);
    if (code != NULL) {
        snprintf(dst, cap, "%s", code);
    }
}

bool ui_call_command(const alarm_cmd_t *cmd, ui_call_done_fn done, uintptr_t tag)
{
    slot_t *s = call_new(OP_COMMAND, true, done, tag);
    if (s != NULL) {
        s->c.cmd = *cmd;
    }
    return call_start(s);
}

bool ui_call_authenticate(const char *code, ui_call_done_fn done, uintptr_t tag)
{
    slot_t *s = call_new(OP_AUTHENTICATE, true, done, tag);
    if (s != NULL) {
        code_copy(s->c.code, sizeof(s->c.code), code);
    }
    return call_start(s);
}

bool ui_call_change_code(const char *current, const char *new_code, ui_call_done_fn done,
                         uintptr_t tag)
{
    slot_t *s = call_new(OP_CHANGE_CODE, true, done, tag);
    if (s != NULL) {
        code_copy(s->c.code, sizeof(s->c.code), current);
        code_copy(s->c.new_code, sizeof(s->c.new_code), new_code);
    }
    return call_start(s);
}

bool ui_call_partition(uint8_t partition, bool activity, ui_call_done_fn done, uintptr_t tag)
{
    slot_t *s = call_new(OP_PARTITION, !activity, done, tag);
    if (s != NULL) {
        s->c.partition = partition;
        s->c.activity = activity;
    }
    return call_start(s);
}

bool ui_call_dev_act(const char *ref, uint8_t action, int32_t value, const char *code,
                     ui_call_done_fn done, uintptr_t tag)
{
    slot_t *s = call_new(OP_DEV_ACT, true, done, tag);
    if (s != NULL) {
        snprintf(s->c.ref, sizeof(s->c.ref), "%s", ref != NULL ? ref : "");
        s->c.action = action;
        s->c.value = value;
        code_copy(s->c.code, sizeof(s->c.code), code);
    }
    return call_start(s);
}

void ui_call_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    for (size_t i = 0; i < CALLS_MAX; i++) {
        atomic_init(&s_slots[i].finished, false);
    }
    s_seq = 0u;
    s_timer = NULL; /* the screens are made again: what was on them went with them */
    s_shield = NULL;
}
