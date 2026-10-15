/*
 * zw_cc_network.h - the interview/network command classes (command-classes.md §4.7-§4.13): Wake
 * Up, Association, Multi Channel Association, Association Group Information, Version,
 * Manufacturer Specific, Z-Wave Plus Info.
 */
#ifndef ZW_CC_NETWORK_H
#define ZW_CC_NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_cc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- §4.7 Wake Up (84): a plain 1-byte node ID (not `nid` - this predates 16-bit node IDs). --- */
#define ZW_CMD_WAKE_UP_INTERVAL_SET               0x04u
#define ZW_CMD_WAKE_UP_INTERVAL_GET               0x05u
#define ZW_CMD_WAKE_UP_INTERVAL_REPORT            0x06u
#define ZW_CMD_WAKE_UP_NOTIFICATION               0x07u
#define ZW_CMD_WAKE_UP_NO_MORE_INFORMATION        0x08u
#define ZW_CMD_WAKE_UP_INTERVAL_CAPABILITIES_GET  0x09u
#define ZW_CMD_WAKE_UP_INTERVAL_CAPABILITIES_REPORT 0x0Au

typedef struct {
    uint32_t interval_seconds; /* 3 bytes on the wire; 0 = only woken by hand */
    uint8_t node_id_to_notify;
} zw_cc_wake_up_interval_set_t;
size_t zw_cc_wake_up_interval_set_build(uint8_t *out, size_t cap, const zw_cc_wake_up_interval_set_t *in);
bool zw_cc_wake_up_interval_set_parse(const uint8_t *p, size_t len, zw_cc_wake_up_interval_set_t *out);

size_t zw_cc_wake_up_interval_get_build(uint8_t *out, size_t cap);
bool zw_cc_wake_up_interval_get_parse(const uint8_t *p, size_t len);

typedef struct {
    uint32_t interval_seconds;
    uint8_t node_id;
} zw_cc_wake_up_interval_report_t;
size_t zw_cc_wake_up_interval_report_build(uint8_t *out, size_t cap,
                                            const zw_cc_wake_up_interval_report_t *in);
bool zw_cc_wake_up_interval_report_parse(const uint8_t *p, size_t len,
                                          zw_cc_wake_up_interval_report_t *out);

size_t zw_cc_wake_up_notification_build(uint8_t *out, size_t cap);
bool zw_cc_wake_up_notification_parse(const uint8_t *p, size_t len);
size_t zw_cc_wake_up_no_more_information_build(uint8_t *out, size_t cap);
bool zw_cc_wake_up_no_more_information_parse(const uint8_t *p, size_t len);
size_t zw_cc_wake_up_interval_capabilities_get_build(uint8_t *out, size_t cap);
bool zw_cc_wake_up_interval_capabilities_get_parse(const uint8_t *p, size_t len);

typedef struct {
    uint32_t min_seconds, max_seconds, default_seconds, step_seconds;
    bool has_v3;
    bool wake_up_on_demand;
} zw_cc_wake_up_interval_capabilities_report_t;
size_t zw_cc_wake_up_interval_capabilities_report_build(
    uint8_t *out, size_t cap, const zw_cc_wake_up_interval_capabilities_report_t *in);
bool zw_cc_wake_up_interval_capabilities_report_parse(
    const uint8_t *p, size_t len, zw_cc_wake_up_interval_capabilities_report_t *out);

/* ---- §4.8 Association (85) --------------------------------------------------------------------- */
#define ZW_CMD_ASSOCIATION_SET                     0x01u
#define ZW_CMD_ASSOCIATION_GET                     0x02u
#define ZW_CMD_ASSOCIATION_REPORT                  0x03u
#define ZW_CMD_ASSOCIATION_REMOVE                  0x04u
#define ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_GET    0x05u
#define ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_REPORT 0x06u
#define ZW_CMD_ASSOCIATION_SPECIFIC_GROUP_GET      0x0Bu
#define ZW_CMD_ASSOCIATION_SPECIFIC_GROUP_REPORT   0x0Cu

#define ZW_ASSOCIATION_LIFELINE_GROUP 1u

typedef struct {
    uint8_t group;
    const uint8_t *node_ids; /* plain 1-byte node IDs */
    size_t node_id_count;
} zw_cc_association_set_t;
size_t zw_cc_association_set_build(uint8_t *out, size_t cap, const zw_cc_association_set_t *in);
bool zw_cc_association_set_parse(const uint8_t *p, size_t len, zw_cc_association_set_t *out);

typedef struct {
    uint8_t group;
} zw_cc_association_get_t;
size_t zw_cc_association_get_build(uint8_t *out, size_t cap, const zw_cc_association_get_t *in);
bool zw_cc_association_get_parse(const uint8_t *p, size_t len, zw_cc_association_get_t *out);

typedef struct {
    uint8_t group;
    uint8_t max_nodes;
    uint8_t reports_to_follow;
    const uint8_t *node_ids;
    size_t node_id_count;
} zw_cc_association_report_t;
size_t zw_cc_association_report_build(uint8_t *out, size_t cap, const zw_cc_association_report_t *in);
bool zw_cc_association_report_parse(const uint8_t *p, size_t len, zw_cc_association_report_t *out);

typedef struct {
    uint8_t group;
    const uint8_t *node_ids; /* empty = remove all */
    size_t node_id_count;
} zw_cc_association_remove_t;
size_t zw_cc_association_remove_build(uint8_t *out, size_t cap, const zw_cc_association_remove_t *in);
bool zw_cc_association_remove_parse(const uint8_t *p, size_t len, zw_cc_association_remove_t *out);

size_t zw_cc_association_supported_groupings_get_build(uint8_t *out, size_t cap);
bool zw_cc_association_supported_groupings_get_parse(const uint8_t *p, size_t len);

typedef struct {
    uint8_t group_count;
} zw_cc_association_supported_groupings_report_t;
size_t zw_cc_association_supported_groupings_report_build(
    uint8_t *out, size_t cap, const zw_cc_association_supported_groupings_report_t *in);
bool zw_cc_association_supported_groupings_report_parse(
    const uint8_t *p, size_t len, zw_cc_association_supported_groupings_report_t *out);

size_t zw_cc_association_specific_group_get_build(uint8_t *out, size_t cap);
bool zw_cc_association_specific_group_get_parse(const uint8_t *p, size_t len);

typedef struct {
    uint8_t group;
} zw_cc_association_specific_group_report_t;
size_t zw_cc_association_specific_group_report_build(
    uint8_t *out, size_t cap, const zw_cc_association_specific_group_report_t *in);
bool zw_cc_association_specific_group_report_parse(
    const uint8_t *p, size_t len, zw_cc_association_specific_group_report_t *out);

/* ---- §4.9 Multi Channel Association (8E): Association's commands/numbers, endpoint-aware
 * destinations. Destinations: plain node IDs, then an optional 00 marker followed by
 * (node, endpoint) pairs; an endpoint byte's bit 7 set = a mask of endpoints 1..7 (bits 6..0),
 * else one endpoint 0..127 (bits 6..0). ------------------------------------------------------- */
typedef struct {
    uint16_t node_id;
    bool is_endpoint;    /* false: 'node_id' is a plain destination, before any 00 marker */
    bool is_mask;         /* true: 'endpoint_mask' is a mask of endpoints 1..7 */
    uint8_t endpoint;      /* valid when is_endpoint && !is_mask */
    uint8_t endpoint_mask; /* valid when is_endpoint && is_mask: bit n = endpoint n+1 */
} zw_cc_mc_destination_t;
/* Walks a raw destination list one entry at a time; init with the 'destinations'/'destinations_len'
 * from one of this section's structs. Returns false at the end. */
typedef struct {
    const uint8_t *pos;
    const uint8_t *end;
    bool past_marker;
} zw_cc_mc_dest_iter_t;
void zw_cc_mc_dest_iter_init(zw_cc_mc_dest_iter_t *it, const uint8_t *destinations, size_t len);
bool zw_cc_mc_dest_iter_next(zw_cc_mc_dest_iter_t *it, zw_cc_mc_destination_t *out);

typedef struct {
    uint8_t group;
    const uint8_t *destinations;
    size_t destinations_len;
} zw_cc_mc_association_set_t;
size_t zw_cc_mc_association_set_build(uint8_t *out, size_t cap, const zw_cc_mc_association_set_t *in);
bool zw_cc_mc_association_set_parse(const uint8_t *p, size_t len, zw_cc_mc_association_set_t *out);

typedef struct {
    uint8_t group;
} zw_cc_mc_association_get_t;
size_t zw_cc_mc_association_get_build(uint8_t *out, size_t cap, const zw_cc_mc_association_get_t *in);
bool zw_cc_mc_association_get_parse(const uint8_t *p, size_t len, zw_cc_mc_association_get_t *out);

typedef struct {
    uint8_t group;
    uint8_t max_nodes;
    uint8_t reports_to_follow;
    const uint8_t *destinations;
    size_t destinations_len;
} zw_cc_mc_association_report_t;
size_t zw_cc_mc_association_report_build(uint8_t *out, size_t cap,
                                          const zw_cc_mc_association_report_t *in);
bool zw_cc_mc_association_report_parse(const uint8_t *p, size_t len,
                                        zw_cc_mc_association_report_t *out);

typedef struct {
    uint8_t group;
    const uint8_t *destinations;
    size_t destinations_len;
} zw_cc_mc_association_remove_t;
size_t zw_cc_mc_association_remove_build(uint8_t *out, size_t cap,
                                          const zw_cc_mc_association_remove_t *in);
bool zw_cc_mc_association_remove_parse(const uint8_t *p, size_t len,
                                        zw_cc_mc_association_remove_t *out);

size_t zw_cc_mc_association_supported_groupings_get_build(uint8_t *out, size_t cap);
bool zw_cc_mc_association_supported_groupings_get_parse(const uint8_t *p, size_t len);

typedef struct {
    uint8_t group_count;
} zw_cc_mc_association_supported_groupings_report_t;
size_t zw_cc_mc_association_supported_groupings_report_build(
    uint8_t *out, size_t cap, const zw_cc_mc_association_supported_groupings_report_t *in);
bool zw_cc_mc_association_supported_groupings_report_parse(
    const uint8_t *p, size_t len, zw_cc_mc_association_supported_groupings_report_t *out);

/* ---- §4.10 Association Group Information (59) -------------------------------------------------- */
#define ZW_CMD_AGI_NAME_GET          0x01u
#define ZW_CMD_AGI_NAME_REPORT       0x02u
#define ZW_CMD_AGI_INFO_GET          0x03u
#define ZW_CMD_AGI_INFO_REPORT       0x04u
#define ZW_CMD_AGI_COMMAND_LIST_GET  0x05u
#define ZW_CMD_AGI_COMMAND_LIST_REPORT 0x06u

#define ZW_AGI_PROFILE_GENERAL_LIFELINE 0x0001u

typedef struct {
    uint8_t group;
} zw_cc_agi_name_get_t;
size_t zw_cc_agi_name_get_build(uint8_t *out, size_t cap, const zw_cc_agi_name_get_t *in);
bool zw_cc_agi_name_get_parse(const uint8_t *p, size_t len, zw_cc_agi_name_get_t *out);

typedef struct {
    uint8_t group;
    const char *name; /* UTF-8, a view; not NUL-terminated */
    size_t name_len;
} zw_cc_agi_name_report_t;
size_t zw_cc_agi_name_report_build(uint8_t *out, size_t cap, const zw_cc_agi_name_report_t *in);
bool zw_cc_agi_name_report_parse(const uint8_t *p, size_t len, zw_cc_agi_name_report_t *out);

typedef struct {
    bool refresh_cache; /* bit 7 */
    bool list_mode;      /* bit 6 */
    uint8_t group;         /* 0 in list mode */
} zw_cc_agi_info_get_t;
size_t zw_cc_agi_info_get_build(uint8_t *out, size_t cap, const zw_cc_agi_info_get_t *in);
bool zw_cc_agi_info_get_parse(const uint8_t *p, size_t len, zw_cc_agi_info_get_t *out);

/* One group entry inside an Info Report, 7 bytes: group . mode (00) . profile (2) . reserved .
 * event code (2). Access entries with zw_cc_agi_group_info_get() rather than an iterator, since
 * each is a fixed size. */
typedef struct {
    uint8_t group;
    uint16_t profile;
    uint16_t event_code;
} zw_cc_agi_group_info_t;
bool zw_cc_agi_group_info_get(const uint8_t *groups, size_t groups_len, size_t index,
                               zw_cc_agi_group_info_t *out);

typedef struct {
    bool list_mode; /* bit 7 */
    bool dynamic;    /* bit 6 */
    uint8_t count;    /* bits 5..0 */
    const uint8_t *groups; /* count * 7 bytes, a view; read with zw_cc_agi_group_info_get() */
    size_t groups_len;
} zw_cc_agi_info_report_t;
size_t zw_cc_agi_info_report_build(uint8_t *out, size_t cap, const zw_cc_agi_info_report_t *in);
bool zw_cc_agi_info_report_parse(const uint8_t *p, size_t len, zw_cc_agi_info_report_t *out);

typedef struct {
    uint8_t flags;
    uint8_t group;
} zw_cc_agi_command_list_get_t;
size_t zw_cc_agi_command_list_get_build(uint8_t *out, size_t cap, const zw_cc_agi_command_list_get_t *in);
bool zw_cc_agi_command_list_get_parse(const uint8_t *p, size_t len, zw_cc_agi_command_list_get_t *out);

typedef struct {
    uint8_t group;
    const uint8_t *cc_command_pairs; /* a view: length bytes of CC/command pairs */
    size_t cc_command_pairs_len;
} zw_cc_agi_command_list_report_t;
size_t zw_cc_agi_command_list_report_build(uint8_t *out, size_t cap,
                                            const zw_cc_agi_command_list_report_t *in);
bool zw_cc_agi_command_list_report_parse(const uint8_t *p, size_t len,
                                          zw_cc_agi_command_list_report_t *out);

/* ---- §4.11 Version (86) -------------------------------------------------------------------------- */
#define ZW_CMD_VERSION_GET                  0x11u
#define ZW_CMD_VERSION_REPORT               0x12u
#define ZW_CMD_VERSION_COMMAND_CLASS_GET    0x13u
#define ZW_CMD_VERSION_COMMAND_CLASS_REPORT 0x14u
#define ZW_CMD_VERSION_CAPABILITIES_GET     0x15u
#define ZW_CMD_VERSION_CAPABILITIES_REPORT  0x16u

#define ZW_VERSION_LIBRARY_STATIC_CONTROLLER 0x01u

size_t zw_cc_version_get_build(uint8_t *out, size_t cap);
bool zw_cc_version_get_parse(const uint8_t *p, size_t len);

/* v2 adds hardware_version and other_targets (each a major/minor pair); a v1 report has neither. */
typedef struct {
    uint8_t library_type;
    uint8_t protocol_major, protocol_minor;
    uint8_t firmware0_major, firmware0_minor;
    bool has_v2;
    uint8_t hardware_version;
    const uint8_t *other_targets; /* other_target_count * 2 bytes: major, minor pairs */
    uint8_t other_target_count;
} zw_cc_version_report_t;
size_t zw_cc_version_report_build(uint8_t *out, size_t cap, const zw_cc_version_report_t *in);
bool zw_cc_version_report_parse(const uint8_t *p, size_t len, zw_cc_version_report_t *out);

typedef struct {
    uint8_t cc;
} zw_cc_version_command_class_get_t;
size_t zw_cc_version_command_class_get_build(uint8_t *out, size_t cap,
                                              const zw_cc_version_command_class_get_t *in);
bool zw_cc_version_command_class_get_parse(const uint8_t *p, size_t len,
                                            zw_cc_version_command_class_get_t *out);

typedef struct {
    uint8_t cc;
    uint8_t version; /* 0 = not supported */
} zw_cc_version_command_class_report_t;
size_t zw_cc_version_command_class_report_build(uint8_t *out, size_t cap,
                                                 const zw_cc_version_command_class_report_t *in);
bool zw_cc_version_command_class_report_parse(const uint8_t *p, size_t len,
                                               zw_cc_version_command_class_report_t *out);

size_t zw_cc_version_capabilities_get_build(uint8_t *out, size_t cap);
bool zw_cc_version_capabilities_get_parse(const uint8_t *p, size_t len);

typedef struct {
    bool supports_version_command;
    bool supports_command_class_command;
    bool supports_zwave_software_get;
} zw_cc_version_capabilities_report_t;
size_t zw_cc_version_capabilities_report_build(uint8_t *out, size_t cap,
                                                const zw_cc_version_capabilities_report_t *in);
bool zw_cc_version_capabilities_report_parse(const uint8_t *p, size_t len,
                                              zw_cc_version_capabilities_report_t *out);

/* ---- §4.12 Manufacturer Specific (72) ------------------------------------------------------------ */
#define ZW_CMD_MANUFACTURER_SPECIFIC_GET    0x04u
#define ZW_CMD_MANUFACTURER_SPECIFIC_REPORT 0x05u

size_t zw_cc_manufacturer_specific_get_build(uint8_t *out, size_t cap);
bool zw_cc_manufacturer_specific_get_parse(const uint8_t *p, size_t len);

typedef struct {
    uint16_t manufacturer_id;
    uint16_t product_type;
    uint16_t product_id;
} zw_cc_manufacturer_specific_report_t;
size_t zw_cc_manufacturer_specific_report_build(uint8_t *out, size_t cap,
                                                 const zw_cc_manufacturer_specific_report_t *in);
bool zw_cc_manufacturer_specific_report_parse(const uint8_t *p, size_t len,
                                               zw_cc_manufacturer_specific_report_t *out);

/* ---- §4.13 Z-Wave Plus Info (5E) ------------------------------------------------------------------ */
#define ZW_CMD_ZWAVE_PLUS_INFO_GET    0x01u
#define ZW_CMD_ZWAVE_PLUS_INFO_REPORT 0x02u

#define ZW_ZWAVE_PLUS_ROLE_CENTRAL_STATIC_CONTROLLER 0x00u
#define ZW_ZWAVE_PLUS_ROLE_SLEEPING_REPORTING_END_NODE 0x06u
#define ZW_ZWAVE_PLUS_NODE_TYPE_NODE       0x00u
#define ZW_ZWAVE_PLUS_NODE_TYPE_IP_GATEWAY 0x02u

size_t zw_cc_zwave_plus_info_get_build(uint8_t *out, size_t cap);
bool zw_cc_zwave_plus_info_get_parse(const uint8_t *p, size_t len);

typedef struct {
    uint8_t version;
    uint8_t role_type;
    uint8_t node_type;
    uint16_t installer_icon;
    uint16_t user_icon;
} zw_cc_zwave_plus_info_report_t;
size_t zw_cc_zwave_plus_info_report_build(uint8_t *out, size_t cap,
                                           const zw_cc_zwave_plus_info_report_t *in);
bool zw_cc_zwave_plus_info_report_parse(const uint8_t *p, size_t len,
                                         zw_cc_zwave_plus_info_report_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_NETWORK_H */
