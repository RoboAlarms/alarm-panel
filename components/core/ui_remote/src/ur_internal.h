/*
 * ur_internal.h - ui_remote's record codec: a struct crosses as a record described by a table of
 * its fields (offset, size, kind), written in the table's order. Appending a field to a table is
 * the only change ever made to one (the frozen fixture checks it).
 */
#ifndef UR_INTERNAL_H
#define UR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "panel_link/plink_wire.h"
#include "ui_remote/ui_remote.h"

typedef enum {
    UR_F_INT = 0, /* an integer or enum of its size (1, 2, 4 or 8 bytes), two's complement */
    UR_F_BOOL,    /* a bool: one byte, 0 or 1 */
    UR_F_STR,     /* a char array: u16 length, the bytes (cleaned before use when read) */
    UR_F_SECRET,  /* a char array that holds a code or a password: as UR_F_STR, never cleaned */
    UR_F_REC,     /* a struct: a record of its own table */
} ur_kind_t;

typedef struct ur_table ur_table_t;
typedef struct {
    uint16_t off;
    uint16_t size;  /* of one element */
    uint8_t kind;
    uint16_t count; /* elements (an array), 1 otherwise */
    const ur_table_t *sub;
} ur_field_t;

struct ur_table {
    const ur_field_t *f;
    size_t n;
    size_t size;
};

#define UR_MEMBER_SIZE(T, m) sizeof(((T *)0)->m)
#define UR_INT(T, m)  {(uint16_t)offsetof(T, m), (uint16_t)UR_MEMBER_SIZE(T, m), UR_F_INT, 1u, NULL}
#define UR_BOOL(T, m) {(uint16_t)offsetof(T, m), 1u, UR_F_BOOL, 1u, NULL}
#define UR_STR(T, m)  {(uint16_t)offsetof(T, m), (uint16_t)UR_MEMBER_SIZE(T, m), UR_F_STR, 1u, NULL}
#define UR_SECRET(T, m) \
    {(uint16_t)offsetof(T, m), (uint16_t)UR_MEMBER_SIZE(T, m), UR_F_SECRET, 1u, NULL}
#define UR_REC(T, m, tab) \
    {(uint16_t)offsetof(T, m), (uint16_t)UR_MEMBER_SIZE(T, m), UR_F_REC, 1u, &(tab)}
/* An array of integers or of records. */
#define UR_INTS(T, m) \
    {(uint16_t)offsetof(T, m), (uint16_t)sizeof(((T *)0)->m[0]), UR_F_INT, \
     (uint16_t)(UR_MEMBER_SIZE(T, m) / sizeof(((T *)0)->m[0])), NULL}
#define UR_RECS(T, m, tab) \
    {(uint16_t)offsetof(T, m), (uint16_t)sizeof(((T *)0)->m[0]), UR_F_REC, \
     (uint16_t)(UR_MEMBER_SIZE(T, m) / sizeof(((T *)0)->m[0])), &(tab)}
#define UR_TABLE(T, fields) {fields, sizeof(fields) / sizeof((fields)[0]), sizeof(T)}

/* One record: obj by t into w. */
void ur_put(plink_w_t *w, const ur_table_t *t, const void *obj);
/* One record from r into obj (zeroed first; fields the record lacks stay zero). */
void ur_get(plink_r_t *r, const ur_table_t *t, void *obj);
/* A list: count u16, then each record. */
void ur_put_list(plink_w_t *w, const ur_table_t *t, const void *objs, size_t count);
/* At most max records into objs; returns how many. */
size_t ur_get_list(plink_r_t *r, const ur_table_t *t, void *objs, size_t max);
/* A string argument (u16 length and its bytes) into out, cleaned or not. */
void ur_put_str(plink_w_t *w, const char *s);
void ur_get_str(plink_r_t *r, char *out, size_t cap, bool clean);

/* The tables the calls use (ur_tables.c). */
extern const ur_table_t ur_t_binding;
extern const ur_table_t ur_t_info;
extern const ur_table_t ur_t_candidate;
extern const ur_table_t ur_t_source;

#endif /* UR_INTERNAL_H */
