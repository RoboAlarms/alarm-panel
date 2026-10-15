/*
 * zw_cc_multichannel.h - Multi Channel (command-classes.md §4.15): a multi-sensor's endpoints.
 * Command Encapsulation is also §1's Multi Channel layer; see zw_cc_encap.h's
 * zw_cc_wrap_multi_channel()/zw_cc_unwrap() for wrapping/unwrapping a command through it.
 */
#ifndef ZW_CC_MULTICHANNEL_H
#define ZW_CC_MULTICHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_cc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_CMD_MULTI_CHANNEL_END_POINT_GET       0x07u
#define ZW_CMD_MULTI_CHANNEL_END_POINT_REPORT    0x08u
#define ZW_CMD_MULTI_CHANNEL_CAPABILITY_GET      0x09u
#define ZW_CMD_MULTI_CHANNEL_CAPABILITY_REPORT   0x0Au
#define ZW_CMD_MULTI_CHANNEL_COMMAND_ENCAPSULATION 0x0Du

size_t zw_cc_multi_channel_end_point_get_build(uint8_t *out, size_t cap);
bool zw_cc_multi_channel_end_point_get_parse(const uint8_t *p, size_t len);

/* individual_count: bits 6..0 of byte 1. aggregated_count is present only from v?+ (the command
 * being long enough to carry it). */
typedef struct {
    bool is_dynamic;
    bool identical_capabilities;
    uint8_t individual_count;
    bool has_aggregated_count;
    uint8_t aggregated_count;
} zw_cc_multi_channel_end_point_report_t;
size_t zw_cc_multi_channel_end_point_report_build(uint8_t *out, size_t cap,
                                                   const zw_cc_multi_channel_end_point_report_t *in);
bool zw_cc_multi_channel_end_point_report_parse(const uint8_t *p, size_t len,
                                                 zw_cc_multi_channel_end_point_report_t *out);

typedef struct {
    uint8_t endpoint;
} zw_cc_multi_channel_capability_get_t;
size_t zw_cc_multi_channel_capability_get_build(uint8_t *out, size_t cap,
                                                 const zw_cc_multi_channel_capability_get_t *in);
bool zw_cc_multi_channel_capability_get_parse(const uint8_t *p, size_t len,
                                               zw_cc_multi_channel_capability_get_t *out);

typedef struct {
    bool is_dynamic;
    uint8_t endpoint; /* bits 6..0 */
    uint8_t generic_class;
    uint8_t specific_class;
    const uint8_t *cc_list;
    size_t cc_list_len;
} zw_cc_multi_channel_capability_report_t;
size_t zw_cc_multi_channel_capability_report_build(uint8_t *out, size_t cap,
                                                    const zw_cc_multi_channel_capability_report_t *in);
bool zw_cc_multi_channel_capability_report_parse(const uint8_t *p, size_t len,
                                                  zw_cc_multi_channel_capability_report_t *out);

/* destination_is_mask: the destination byte's bit 7 (bit mask of endpoints 1..7, else one
 * endpoint 0..127 in 'destination'). This is also §1's Multi Channel encapsulation layer;
 * zw_cc_encap.h wraps/unwraps it with a friendlier signature. */
typedef struct {
    uint8_t source_endpoint;    /* bits 6..0 */
    bool destination_is_mask;
    uint8_t destination;         /* endpoint 0..127, or a mask of endpoints 1..7 when is_mask */
    const uint8_t *command;
    size_t command_len;
} zw_cc_multi_channel_command_encapsulation_t;
size_t zw_cc_multi_channel_command_encapsulation_build(
    uint8_t *out, size_t cap, const zw_cc_multi_channel_command_encapsulation_t *in);
bool zw_cc_multi_channel_command_encapsulation_parse(
    const uint8_t *p, size_t len, zw_cc_multi_channel_command_encapsulation_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_MULTICHANNEL_H */
