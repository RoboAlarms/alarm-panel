/*
 * proto_json.h - small JSON writer and strict reader over caller-owned buffers (no heap).
 *
 * The writer builds one document into a fixed buffer. It remembers an overflow instead of
 * failing every call, so message builders stay linear: check pj_finish() once at the end.
 *
 * The reader parses the messages the panel accepts from the network: objects, arrays, strings
 * with every escape, integers, true / false / null. It is strict (RFC 8259 grammar, no
 * trailing commas, no comments) and bounded (PJ_MAX_DEPTH), so hostile input stays cheap.
 * Values the caller doesn't read are skipped automatically by the next pj_*_next() call.
 */
#ifndef PROTO_JSON_H
#define PROTO_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PJ_MAX_DEPTH 8

/* ---- writer ------------------------------------------------------------------------------ */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;      /* something didn't fit (or the nesting was wrong) */
    bool after_key;     /* a key was written, its value comes next */
    uint8_t depth;
    uint16_t has_items; /* bit per depth: this level already holds a value */
} pj_writer_t;

void pj_init(pj_writer_t *w, char *buf, size_t cap);

void pj_obj(pj_writer_t *w);
void pj_obj_end(pj_writer_t *w);
void pj_arr(pj_writer_t *w);
void pj_arr_end(pj_writer_t *w);
void pj_key(pj_writer_t *w, const char *key);

void pj_str(pj_writer_t *w, const char *s); /* NULL writes null */
void pj_int(pj_writer_t *w, int64_t v);
void pj_uint(pj_writer_t *w, uint64_t v);
void pj_bool(pj_writer_t *w, bool v);
void pj_null(pj_writer_t *w);

/* key + value */
void pj_kstr(pj_writer_t *w, const char *key, const char *s);
void pj_kint(pj_writer_t *w, const char *key, int64_t v);
void pj_kuint(pj_writer_t *w, const char *key, uint64_t v);
void pj_kbool(pj_writer_t *w, const char *key, bool v);
void pj_knull(pj_writer_t *w, const char *key);
void pj_kobj(pj_writer_t *w, const char *key);
void pj_karr(pj_writer_t *w, const char *key);
/* 1-based numbers where 0 means none: 0 writes null. */
void pj_knum(pj_writer_t *w, const char *key, uint32_t v);

/* NUL-terminates. Returns the document length, or 0 when it overflowed or isn't closed. */
size_t pj_finish(pj_writer_t *w);

/* ---- reader ------------------------------------------------------------------------------ */
typedef enum {
    PJ_END = 0, /* nothing (end of input or a syntax error) */
    PJ_OBJECT,
    PJ_ARRAY,
    PJ_STRING,
    PJ_NUMBER,
    PJ_BOOL,
    PJ_NULL,
} pj_type_t;

typedef struct {
    const char *p;
    const char *end;
    bool error;        /* syntax error or nesting too deep: stop reading */
    bool pending;      /* a member / element value is waiting to be read */
    uint8_t depth;
    uint16_t started;  /* bit per depth: the first member / element was seen */
    uint16_t is_array; /* bit per depth */
} pj_reader_t;

void pj_read_init(pj_reader_t *r, const char *json, size_t len);

/* Type of the next value (without consuming it). */
pj_type_t pj_peek(pj_reader_t *r);

/* Enter the next value as an object / array: false (and error) when it isn't one. */
bool pj_enter_obj(pj_reader_t *r);
bool pj_enter_arr(pj_reader_t *r);

/* Next member of the current object: copies its key (a key longer than key_cap - 1 bytes
 * comes back as "" so it never matches) and positions at its value. false at the end of the
 * object (which is then left) or on error. */
bool pj_next_member(pj_reader_t *r, char *key, size_t key_cap);

/* Next element of the current array. false at the end of the array (then left) or on error. */
bool pj_next_element(pj_reader_t *r);

/* Read the next value. false when it has another type (it is skipped) or doesn't fit: a string
 * longer than cap - 1 bytes or containing \u0000; a number with a fraction or exponent or
 * outside int64. Syntax errors set r->error. */
bool pj_get_str(pj_reader_t *r, char *out, size_t cap);
bool pj_get_int(pj_reader_t *r, int64_t *out);
bool pj_get_bool(pj_reader_t *r, bool *out);
bool pj_get_null(pj_reader_t *r);
void pj_skip(pj_reader_t *r);

/* After the top-level value: true when only whitespace is left and nothing went wrong. */
bool pj_finished(pj_reader_t *r);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_JSON_H */
