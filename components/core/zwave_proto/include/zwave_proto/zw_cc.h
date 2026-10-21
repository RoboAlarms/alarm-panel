/*
 * zw_cc.h - the Z-Wave command classes the panel speaks (docs/features/36,
 * command-classes.md), split across several headers this one pulls in:
 *   zw_cc_basic.h        No Operation, Basic, Binary Sensor, Multilevel Sensor, Battery (§4.1,
 *                         §4.2, §4.3, §4.4, §4.6)
 *   zw_cc_notification.h Notification (§4.5)
 *   zw_cc_network.h      Wake Up, Association, Multi Channel Association, Association Group
 *                         Information, Version, Manufacturer Specific, Z-Wave Plus Info (§4.7,
 *                         §4.8, §4.9, §4.10, §4.11, §4.12, §4.13)
 *   zw_cc_multichannel.h Multi Channel (§4.15)
 *   zw_cc_supervision.h  Supervision (§4.14)
 *   zw_cc_misc.h         Indicator, Device Reset Locally, Configuration, Central Scene, Alarm
 *                         Sensor (legacy), Hail (legacy), Time, Application Status (§4.18,
 *                         §4.19, §4.20, §4.21, §4.22, §4.23, §4.24, §4.25)
 *   zw_cc_encap.h         §1 encapsulation: Multi Channel, CRC-16, Supervision, Multi Command -
 *                         wrap for sending, unwrap one layer at a time for receiving
 *   zw_cc_match.h         §6: whether an incoming command answers a Get the panel sent
 *   zw_cc_answer.h        §5: what the panel answers, and with what
 *
 * How to use:
 *   - A codec works on the whole command as it appears in ApplicationCommandHandler's command
 *     field: `CC | command | parameters` (serial-api.md §6.11), CC and command bytes included -
 *     unlike zw_api's codecs, which strip FUNC before the payload they work on. `build()` writes
 *     both ID bytes; `parse()` checks them (wrong CC or command = not this message = false).
 *   - `zw_cc_<cc>_<cmd>_parse(p, len, out)` never reads past `len`; `zw_cc_<cc>_<cmd>_build(out,
 *     cap, in)` returns the command's total length or 0. Every parser accepts and ignores
 *     trailing bytes (a newer CC version appending fields) but is strict about minimum lengths
 *     and any internal length or count; version-dependent optional fields are read only when the
 *     command is long enough to carry them (documented per command).
 *   - Variable-length parts (node ID lists, CC lists, masks, names, encapsulated commands) are
 *     views into the buffer just parsed, or caller-owned pointers to splice into one being built.
 *     No copies, no heap.
 *   - Every command in command-classes.md §4.1-§4.24 has both directions: the panel builds
 *     Gets/Sets and parses Reports; a simulated device (a later milestone) parses Gets/Sets and
 *     builds Reports. Nothing here decides *when* to send anything - that is components/core/
 *     zwave_host, coded against these names.
 *
 * Portable C11, no heap.
 */
#ifndef ZW_CC_H
#define ZW_CC_H

#ifdef __cplusplus
extern "C" {
#endif

/* command-classes.md §2: every CC identifier this project uses (`>= F1` would take two bytes;
 * none of these do). Security 0/2 (§7-§8) are handled only by §1's unwrap/wrap recognising them -
 * their own commands are components/core/zwave_host's (a later milestone). */
typedef enum {
    ZW_CC_NO_OPERATION                       = 0x00u,
    ZW_CC_BASIC                               = 0x20u,
    ZW_CC_APPLICATION_STATUS                  = 0x22u,
    ZW_CC_BINARY_SWITCH                       = 0x25u,
    ZW_CC_MULTILEVEL_SWITCH                   = 0x26u,
    ZW_CC_BINARY_SENSOR                       = 0x30u,
    ZW_CC_MULTILEVEL_SENSOR                   = 0x31u,
    ZW_CC_CRC16_ENCAPSULATION                 = 0x56u,
    ZW_CC_ASSOCIATION_GROUP_INFORMATION       = 0x59u,
    ZW_CC_DEVICE_RESET_LOCALLY                = 0x5Au,
    ZW_CC_CENTRAL_SCENE                       = 0x5Bu,
    ZW_CC_ZWAVE_PLUS_INFO                     = 0x5Eu,
    ZW_CC_MULTI_CHANNEL                       = 0x60u,
    ZW_CC_SUPERVISION                         = 0x6Cu,
    ZW_CC_CONFIGURATION                       = 0x70u,
    ZW_CC_NOTIFICATION                        = 0x71u,
    ZW_CC_MANUFACTURER_SPECIFIC               = 0x72u,
    ZW_CC_BATTERY                             = 0x80u,
    ZW_CC_HAIL                                = 0x82u,
    ZW_CC_WAKE_UP                             = 0x84u,
    ZW_CC_ASSOCIATION                         = 0x85u,
    ZW_CC_VERSION                             = 0x86u,
    ZW_CC_INDICATOR                           = 0x87u,
    ZW_CC_TIME                                = 0x8Au,
    ZW_CC_MULTI_CHANNEL_ASSOCIATION           = 0x8Eu,
    ZW_CC_MULTI_COMMAND                       = 0x8Fu,
    ZW_CC_ENTRY_CONTROL                       = 0x6Fu,
    ZW_CC_SOUND_SWITCH                        = 0x79u,
    ZW_CC_ALARM_SENSOR                        = 0x9Cu,
    ZW_CC_SECURITY_0                          = 0x98u,
    ZW_CC_SECURITY_2                          = 0x9Fu,
    ZW_CC_TRANSPORT_SERVICE                   = 0x55u,
} zw_cc_t;

#ifdef __cplusplus
}
#endif

#include "zwave_proto/zw_cc_basic.h"
#include "zwave_proto/zw_cc_notification.h"
#include "zwave_proto/zw_cc_supervision.h"
#include "zwave_proto/zw_cc_multichannel.h"
#include "zwave_proto/zw_cc_network.h"
#include "zwave_proto/zw_cc_misc.h"
#include "zwave_proto/zw_cc_entry_control.h"
#include "zwave_proto/zw_cc_encap.h"
#include "zwave_proto/zw_cc_match.h"
#include "zwave_proto/zw_cc_answer.h"

#endif /* ZW_CC_H */
