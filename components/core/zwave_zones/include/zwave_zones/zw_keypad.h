/* Assigned, authenticated Z-Wave keypads -> the existing alarm command authority.
 * This portable adapter is not yet enabled by the firmware. The owner must supply a current
 * installer assignment and trusted network/generation facts from its node cache, then run
 * commands through the alarm task. No PIN database, logging or credential retention here. */
#ifndef ZW_KEYPAD_H
#define ZW_KEYPAD_H
#include "alarm/alarm_types.h"
#include "zwave_host/zw_s2.h"
#include "zwave_proto/zw_cc.h"
#ifdef __cplusplus
extern "C" {
#endif

#define ZWK_RECENT 16u
#define ZWK_RETRY_WINDOW_MS 60000u /* application retry policy; S2 handles cryptographic replay */
typedef struct {
    uint32_t home_id;
    uint16_t generation, node, device;
    uint8_t endpoint, partition, panic_mask;
    zw_sec_class_t security; /* exact highest granted class: authenticated S2 or access only */
} zwk_binding_t;
typedef struct {
    alarm_result_t result;
    alarm_cmd_result_t detail;
} zwk_reply_t;
typedef struct {
    void *ctx;
    /* true: a definitive engine result, successful or refused. false: nothing executed
     * (e.g. queue unavailable), so a retransmission may retry. Must copy/use the command
     * before returning and erase any platform-side PIN copy. Never retain this pointer. */
    bool (*command)(void *ctx, const alarm_cmd_t *cmd, const alarm_source_t *source, zwk_reply_t *reply);
} zwk_port_t;
typedef struct {
    bool used;
    uint8_t sequence;
    uint64_t handled_ms;
    zwk_reply_t reply;
} zwk_recent_t;
typedef struct {
    bool enabled;
    zwk_binding_t binding;
    zwk_port_t port;
    uint8_t next;
    zwk_recent_t recent[ZWK_RECENT]; /* results only; never PINs */
} zwk_t;
typedef enum {
    ZWK_HANDLED, ZWK_DUPLICATE, ZWK_IGNORED, ZWK_INVALID, ZWK_UNTRUSTED, ZWK_BUSY
} zwk_result_t;

/* Call on assignment/revocation/replacement. Invalid or NULL binding leaves it disabled. */
bool zwk_init(zwk_t *k, const zwk_binding_t *binding, const zwk_port_t *port);
/* Consume only authenticated, unwrapped single-device traffic from zw_net. The home and
 * generation arguments are current trusted node facts, never fields supplied by the peer.
 * S2 already rejects cryptographic replays. Last 16 command sequence numbers additionally
 * suppress application retries for 60 seconds, including interleaved commands and refused
 * PIN attempts. now_ms is the owner's monotonic uint64 millisecond clock.
 * HANDLED is not success: use reply.result for feedback. BUSY must not be acknowledged as
 * success. Do not call recursively; the network owner serializes calls. */
zwk_result_t zwk_receive(zwk_t *k, uint32_t home_id, uint16_t generation,
                         const zw_s2_rx_t *rx, uint64_t now_ms, zwk_reply_t *reply);
#ifdef __cplusplus
}
#endif
#endif
