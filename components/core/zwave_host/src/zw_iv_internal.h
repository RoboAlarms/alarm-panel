/*
 * zw_iv_internal.h - between the interview's engine (zw_iv.c: slots, tries, timing, sleep) and
 * its steps (zw_iv_steps.c: network.md §5's table, §6, push or pull). Private to zwave_host.
 */
#ifndef ZW_IV_INTERNAL_H
#define ZW_IV_INTERNAL_H

#include "zwave_host/zw_iv.h"

/* What the step at a slot's cursor asks next. */
typedef enum {
    IV_ASK_NODE = 0,  /* a question to the node: `cmd` at endpoint `ep` */
    IV_ASK_PROTOCOL,  /* GetNodeProtocolInfo, to the module */
    IV_ASK_NODE_INFO, /* RequestNodeInfo (port.node_info): ApplicationUpdate 84 answers */
    IV_STEP_DONE,     /* nothing (more) to ask in this step */
    IV_NOT_USABLE     /* another controller: the interview ends */
} iv_ask_kind_t;

typedef struct {
    iv_ask_kind_t kind;
    uint8_t cmd[ZW_IV_CMD_MAX];
    uint8_t len;
    uint8_t ep;
    uint8_t cls;    /* zw_sec_class_t */
    bool answer;    /* a report answers it; false: its delivery does */
    bool supervise; /* a Set to a node with Supervision: wrapped, the Report answers it */
    bool optional;  /* silence after the tries is an answer too (zw_iv_on_silence) */
    uint8_t tries;  /* its tries (0: ZW_IV_TRIES) */
    bool nok_aborts; /* its last try not delivered (ZW_S2_FAILED): the attempt fails, even when
                      * optional (Z-Wave JS: a transmission error aborts the interview attempt) */
} iv_ask_t;

/* What an answer means for the step. */
typedef enum {
    IV_NEXT = 0, /* taken: the cursor moved on */
    IV_CONTINUE, /* taken, the cursor moved on, but its tries go on with the next question (a Set
                  * and its read-back are one exchange) */
    IV_RETRY,    /* not what was wanted (a read-back without the panel): a failed try, the cursor
                  * set to what goes again */
    IV_MORE      /* a part of an answer in parts (reports to follow) without what was wanted: the
                  * rest is awaited ZW_IV_REPORT_MS more, no try spent, the cursor unchanged */
} iv_verdict_t;

/* The step's next question at the slot's cursor. Parts that do not apply are passed over (the
 * cursor moves past them); asking again without an answer gives the same question. */
void zw_iv_plan(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec, iv_ask_t *ask);

/* The answer to the question out: a node's command already matched to it (zw_iv_matches), or
 * NULL for a Set that was delivered (or whose Supervision Report said done). Records what it
 * says and moves the cursor. */
iv_verdict_t zw_iv_on_answer(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec,
                             const zw_s2_rx_t *rx);

/* An optional question that got no answer: its default, and the cursor moves on (or stays, to
 * ask again). Returns how long to wait before the next question, in ms (0: at once). */
uint32_t zw_iv_on_silence(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec);

/* GetNodeProtocolInfo's response: the record's field 02. False when it did not parse. */
bool zw_iv_on_protocol(zw_iv_node_t *s, zw_node_record_t *rec, const uint8_t *p, size_t len);

/* The node's information (ApplicationUpdate 84): the record's CC list (field 03). */
void zw_iv_on_node_info(zw_iv_node_t *s, zw_node_record_t *rec, const uint8_t *ccs, size_t len);

/* Whether a node's command (from the slot's node) answers the slot's question. */
bool zw_iv_matches(const zw_iv_node_t *s, const zw_s2_rx_t *rx);

/* A controller, by its basic device class (serial-api.md 6.10). */
bool zw_iv_is_controller(const zw_node_record_t *rec);

#endif /* ZW_IV_INTERNAL_H */
