/* Alarm state/events -> device-owned Indicator sounds. No audio generation or PIN input.
 * Caller serializes all calls. One pending state per target coalesces obsolete updates. */
#ifndef ZW_FEEDBACK_H
#define ZW_FEEDBACK_H
#include "alarm/alarm_snapshot.h"
#include "zwave_host/zw_net.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ZWF_TARGETS 8u
#define ZWF_BLOB_SIZE (4u + ZWF_TARGETS * 10u + 2u)
#define ZWF_TAG 0x5C000000u
typedef struct {
    uint32_t home;
    uint16_t node, generation;
    uint8_t partition;
} zwf_binding_t;
typedef struct {
    uint8_t indicator;
    uint64_t end_ms;
} zwf_state_t;
typedef struct {
    uint8_t partitions;
    uint64_t taken_ms;
    uint32_t last_event_seq;
    zwf_state_t part[ALARM_MAX_PARTITIONS];
} zwf_view_t;
typedef struct {
    zwf_binding_t binding;
    zwf_state_t sent, flight;
    bool synced, live, inflight;
    uint8_t prompt, flight_prompt, failures;
    uint64_t prompt_until, retry_at, refresh_at;
    uint32_t prompt_seq, flight_seq, tag;
} zwf_target_t;
typedef struct {
    zwf_target_t target[ZWF_TARGETS];
    zwf_view_t view;
    uint32_t stamp, last_event;
    bool have_view;
} zwf_t;
void zwf_init(zwf_t *f);
void zwf_view(const alarm_snapshot_t *snapshot, zwf_view_t *out);
void zwf_snapshot(zwf_t *f, const zwf_view_t *view);
void zwf_event(zwf_t *f, const alarm_event_t *event);
/* Explicit output assignment; partition 0 removes a slot. Model, ready state, S2 and
 * FLiRS/listening capability are validated. This never grants incoming control rights. */
bool zwf_assign(zwf_t *f, size_t slot, const zw_net_t *net, uint16_t node, uint8_t partition);
/* Whether `node` can take feedback now, and if not why (the device page's words). OK exactly
 * when zwf_assign() would accept it on this network. */
typedef enum {
    ZWF_FIT_NONE = 0,    /* no feedback profile for this model */
    ZWF_FIT_OK,
    ZWF_FIT_NOT_READY,   /* its interview has not finished */
    ZWF_FIT_NOT_SECURE,  /* not S2 Authenticated or Access Control */
    ZWF_FIT_UNSUPPORTED  /* no secure Indicator v3, or not reachable on demand */
} zwf_fit_t;
zwf_fit_t zwf_fit(const zw_net_t *net, uint16_t node);
/* The slot assigned to `node` (any generation), or ZWF_TARGETS. */
size_t zwf_slot_of(const zwf_t *f, uint16_t node);
/* Where an assignment for `node` goes: its own slot, else the first free one; ZWF_TARGETS: full. */
size_t zwf_slot_for(const zwf_t *f, uint16_t node);
/* Optional send port for alternate transports/tests; NULL uses zw_net_send. Accepted sends
 * must eventually report through zwf_done, including cancellation on link down. */
typedef zw_s2_result_t (*zwf_send_fn)(void *ctx, uint16_t node, zw_sec_class_t cls,
                                    const uint8_t *cmd, size_t len, uint32_t tag);
void zwf_poll(zwf_t *f, zw_net_t *net, uint64_t now, zwf_send_fn send, void *ctx);
bool zwf_done(zwf_t *f, uint32_t tag, zw_s2_result_t result, uint64_t now);
size_t zwf_encode(const zwf_t *f, uint8_t *out, size_t cap);
bool zwf_decode(zwf_t *f, const uint8_t *blob, size_t len);
/* Exposed for golden-wire tests and transports; false when a countdown has expired. */
size_t zwf_command(const zwf_state_t *state, uint8_t prompt, uint64_t now, uint8_t *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
