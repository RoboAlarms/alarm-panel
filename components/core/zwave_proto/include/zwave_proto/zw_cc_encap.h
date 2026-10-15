/*
 * zw_cc_encap.h - §1 encapsulation (command-classes.md): Multi Channel, CRC-16, Supervision and
 * Multi Command wrap a command for sending and unwrap one received layer at a time. Security
 * 0/2 (9F/98) and Transport Service (55) are only *recognised* here, never decrypted or
 * reassembled - security is components/core/zwave_host's job (a later milestone); Transport
 * Service is unsupported in v1 (§1: "not supported... drop and count").
 *
 * Receiving: call zw_cc_unwrap() on the command as received; while it returns true and
 * out.kind != ZW_CC_UNWRAP_NONE, feed out.inner/out.inner_len back in for the next layer. A
 * false return means a layer was recognised but malformed (a CRC mismatch, a length that does
 * not fit) - refuse the whole command. ZW_CC_UNWRAP_NONE with the same inner/inner_len as what
 * was given means there is nothing left to unwrap: that is the command itself.
 *
 * Sending: the panel builds from the inside out (§1) - command, then zw_cc_wrap_multi_channel()
 * only when addressing an endpoint other than 0, then zw_cc_wrap_supervision() for SET-type
 * commands to a node that supports it. CRC-16 and Multi Command are never used when the panel
 * itself sends (§1), but their wrap functions are here too, for a simulated device or a test.
 */
#ifndef ZW_CC_ENCAP_H
#define ZW_CC_ENCAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_cc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* §4.16 CRC-16 Encapsulation (56): `56 01 | command | CRC (2)`, CRC-16-CCITT (zw_crc16.h),
 * init ZW_CRC16_INIT, over `56 01 | command`. */
#define ZW_CMD_CRC16_ENCAPSULATE 0x01u
/* §4.17 Multi Command (8F): `8F 01 | count | count x (length . command)`. */
#define ZW_CMD_MULTI_COMMAND_ENCAPSULATE 0x01u

typedef enum {
    ZW_CC_UNWRAP_NONE = 0,          /* not wrapped: 'inner'/'inner_len' echo the input */
    ZW_CC_UNWRAP_SECURITY_2,        /* 9F: decrypting it is zwave_host's job */
    ZW_CC_UNWRAP_SECURITY_0,        /* 98: decrypting it is zwave_host's job */
    ZW_CC_UNWRAP_TRANSPORT_SERVICE, /* 55: not supported in v1, drop and count */
    ZW_CC_UNWRAP_SUPERVISION,
    ZW_CC_UNWRAP_MULTI_CHANNEL,
    ZW_CC_UNWRAP_CRC16,
    ZW_CC_UNWRAP_MULTI_COMMAND,     /* several inner commands: see zw_cc_multi_command_iter_t */
} zw_cc_unwrap_kind_t;

typedef struct {
    zw_cc_unwrap_kind_t kind;
    /* Supervision (kind == ZW_CC_UNWRAP_SUPERVISION) */
    bool supervision_status_updates_wanted;
    uint8_t supervision_session_id;
    /* Multi Channel (kind == ZW_CC_UNWRAP_MULTI_CHANNEL) */
    uint8_t source_endpoint;
    uint8_t destination_endpoint;
    bool destination_is_mask;
    /* the inner command (every kind except ZW_CC_UNWRAP_MULTI_COMMAND, where this is instead the
     * raw `count | count x (length . command)` payload for zw_cc_multi_command_iter_init()) */
    const uint8_t *inner;
    size_t inner_len;
} zw_cc_unwrap_t;

/* Looks at the outermost layer of 'command' and unwraps it if §1 says it is an encapsulation
 * layer. False on a malformed layer (refuse the command); true with kind ZW_CC_UNWRAP_NONE means
 * 'command' was not wrapped at all. */
bool zw_cc_unwrap(const uint8_t *command, size_t len, zw_cc_unwrap_t *out);

/* Walks a Multi Command payload (a ZW_CC_UNWRAP_MULTI_COMMAND unwrap's inner/inner_len, or a
 * buffer built with zw_cc_wrap_multi_command()) one inner command at a time. */
typedef struct {
    const uint8_t *pos;
    const uint8_t *end;
    uint8_t remaining;
} zw_cc_multi_command_iter_t;
bool zw_cc_multi_command_iter_init(zw_cc_multi_command_iter_t *it, const uint8_t *payload, size_t len);
bool zw_cc_multi_command_iter_next(zw_cc_multi_command_iter_t *it, const uint8_t **command,
                                    size_t *command_len);

/* ---- wrapping (sending): each returns the outer command's total length, 0 on failure --------- */
size_t zw_cc_wrap_multi_channel(uint8_t *out, size_t cap, uint8_t source_endpoint, uint8_t destination,
                                 bool destination_is_mask, const uint8_t *inner, size_t inner_len);
size_t zw_cc_wrap_supervision(uint8_t *out, size_t cap, uint8_t session_id, bool status_updates_wanted,
                               const uint8_t *inner, size_t inner_len);
size_t zw_cc_wrap_crc16(uint8_t *out, size_t cap, const uint8_t *inner, size_t inner_len);
/* 'commands'/'command_lens' are parallel arrays of 'count' entries. */
size_t zw_cc_wrap_multi_command(uint8_t *out, size_t cap, const uint8_t *const *commands,
                                 const size_t *command_lens, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_ENCAP_H */
