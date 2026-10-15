/*
 * zw_cc_answer.c - §5 "what the panel answers" (see zw_cc_answer.h). A build failure (the
 * caller's 'out'/'cap' too small) falls back to ZW_CC_ANSWER_NOT_HANDLED: that is a caller sizing
 * bug, not a protocol fact, but the enum has no separate code for it - size buffers generously.
 */
#include "zwave_proto/zw_cc_answer.h"

#include "zwave_proto/zw_cc.h"

/* §5 "Version Command Class Get": the version the panel implements for each CC it answers for
 * (it answers every command of that version, no more); everything else, including Security 0
 * (98) - "only with S0", not modelled by this identity struct - is 0. */
static uint8_t answer_version_for_cc(uint8_t cc)
{
    switch (cc) {
    case (uint8_t)ZW_CC_ZWAVE_PLUS_INFO:
        return 2u;
    case (uint8_t)ZW_CC_SECURITY_2:
        return 1u;
    case (uint8_t)ZW_CC_SUPERVISION:
        return 1u;
    case (uint8_t)ZW_CC_CRC16_ENCAPSULATION:
        return 1u;
    case (uint8_t)ZW_CC_MULTI_COMMAND:
        return 1u;
    case (uint8_t)ZW_CC_VERSION:
        return 2u;
    case (uint8_t)ZW_CC_MANUFACTURER_SPECIFIC:
        return 1u;
    case (uint8_t)ZW_CC_ASSOCIATION:
        return 2u;
    case (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION:
        return 1u;
    case (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION:
        return 2u;
    case (uint8_t)ZW_CC_DEVICE_RESET_LOCALLY:
        return 1u;
    case (uint8_t)ZW_CC_TIME:
        return 1u; /* phase 2 */
    default:
        return 0u;
    }
}

zw_cc_answer_result_t zw_cc_panel_answer(const uint8_t *command, size_t command_len,
                                          const zw_cc_panel_identity_t *identity, uint8_t *out,
                                          size_t cap, size_t *out_len)
{
    if (command == NULL || command_len < 2u || identity == NULL) {
        return ZW_CC_ANSWER_NOT_HANDLED;
    }
    const uint8_t cc = command[0];
    const uint8_t cmd = command[1];

    if (cc == (uint8_t)ZW_CC_VERSION && cmd == ZW_CMD_VERSION_GET) {
        if (!zw_cc_version_get_parse(command, command_len)) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_version_report_t rep = {
            .library_type = ZW_VERSION_LIBRARY_STATIC_CONTROLLER,
            .protocol_major = identity->protocol_major,
            .protocol_minor = identity->protocol_minor,
            .firmware0_major = identity->firmware_major,
            .firmware0_minor = identity->firmware_minor,
            .has_v2 = true,
            .hardware_version = 0x01u,
            .other_targets = NULL,
            .other_target_count = 0u,
        };
        const size_t n = zw_cc_version_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_VERSION && cmd == ZW_CMD_VERSION_COMMAND_CLASS_GET) {
        zw_cc_version_command_class_get_t req;
        if (!zw_cc_version_command_class_get_parse(command, command_len, &req)) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_version_command_class_report_t rep = {
            .cc = req.cc,
            .version = answer_version_for_cc(req.cc),
        };
        const size_t n = zw_cc_version_command_class_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_ASSOCIATION && cmd == ZW_CMD_ASSOCIATION_SPECIFIC_GROUP_GET) {
        if (!zw_cc_association_specific_group_get_parse(command, command_len)) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_association_specific_group_report_t rep = { .group = 0u };
        const size_t n = zw_cc_association_specific_group_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION && cmd == ZW_CMD_AGI_COMMAND_LIST_GET) {
        zw_cc_agi_command_list_get_t req;
        if (!zw_cc_agi_command_list_get_parse(command, command_len, &req) || req.group != 1u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_agi_command_list_report_t rep = {
            .group = 1u,
            .cc_command_pairs = NULL,
            .cc_command_pairs_len = 0u,
        };
        const size_t n = zw_cc_agi_command_list_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_MANUFACTURER_SPECIFIC && cmd == ZW_CMD_MANUFACTURER_SPECIFIC_GET) {
        if (!zw_cc_manufacturer_specific_get_parse(command, command_len)) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_manufacturer_specific_report_t rep = {
            .manufacturer_id = identity->manufacturer_id,
            .product_type = identity->product_type,
            .product_id = identity->product_id,
        };
        const size_t n = zw_cc_manufacturer_specific_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_ZWAVE_PLUS_INFO && cmd == ZW_CMD_ZWAVE_PLUS_INFO_GET) {
        if (!zw_cc_zwave_plus_info_get_parse(command, command_len)) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_zwave_plus_info_report_t rep = {
            .version = 2u,
            .role_type = identity->zwave_plus_role_type,
            .node_type = identity->zwave_plus_node_type,
            .installer_icon = identity->installer_icon,
            .user_icon = identity->user_icon,
        };
        const size_t n = zw_cc_zwave_plus_info_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_ASSOCIATION && cmd == ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_GET) {
        if (!zw_cc_association_supported_groupings_get_parse(command, command_len)) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_association_supported_groupings_report_t rep = { .group_count = 1u };
        const size_t n = zw_cc_association_supported_groupings_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_ASSOCIATION && cmd == ZW_CMD_ASSOCIATION_GET) {
        zw_cc_association_get_t req;
        if (!zw_cc_association_get_parse(command, command_len, &req) || req.group != 1u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_association_report_t rep = {
            .group = 1u, .max_nodes = 1u, .reports_to_follow = 0u, .node_ids = NULL, .node_id_count = 0u,
        };
        const size_t n = zw_cc_association_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_ASSOCIATION && (cmd == ZW_CMD_ASSOCIATION_SET || cmd == ZW_CMD_ASSOCIATION_REMOVE)) {
        return ZW_CC_ANSWER_NO_REPLY; /* accepted and ignored (Supervision: FF) */
    }

    /* Multi Channel Association Get / Supported Groupings Get: "as Association" (§5). Set/Remove
     * are not spelled out in §5's table; treated the same as Association's by extension of "as
     * Association" - flag this in review if that reading is wrong. */
    if (cc == (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION && cmd == ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_GET) {
        if (!zw_cc_mc_association_supported_groupings_get_parse(command, command_len)) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_mc_association_supported_groupings_report_t rep = { .group_count = 1u };
        const size_t n = zw_cc_mc_association_supported_groupings_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }
    if (cc == (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION && cmd == ZW_CMD_ASSOCIATION_GET) {
        zw_cc_mc_association_get_t req;
        if (!zw_cc_mc_association_get_parse(command, command_len, &req) || req.group != 1u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_mc_association_report_t rep = {
            .group = 1u, .max_nodes = 1u, .reports_to_follow = 0u, .destinations = NULL, .destinations_len = 0u,
        };
        const size_t n = zw_cc_mc_association_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }
    if (cc == (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION &&
        (cmd == ZW_CMD_ASSOCIATION_SET || cmd == ZW_CMD_ASSOCIATION_REMOVE)) {
        return ZW_CC_ANSWER_NO_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION && cmd == ZW_CMD_AGI_NAME_GET) {
        zw_cc_agi_name_get_t req;
        if (!zw_cc_agi_name_get_parse(command, command_len, &req) || req.group != 1u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        static const char lifeline[] = "Lifeline";
        const zw_cc_agi_name_report_t rep = { .group = 1u, .name = lifeline, .name_len = sizeof(lifeline) - 1u };
        const size_t n = zw_cc_agi_name_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION && cmd == ZW_CMD_AGI_INFO_GET) {
        zw_cc_agi_info_get_t req;
        if (!zw_cc_agi_info_get_parse(command, command_len, &req)) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        /* the panel has exactly one group: 1, profile 0001 "General: Lifeline". */
        uint8_t entry[7];
        entry[0] = 1u;
        entry[1] = 0u;
        entry[2] = (uint8_t)(ZW_AGI_PROFILE_GENERAL_LIFELINE >> 8);
        entry[3] = (uint8_t)ZW_AGI_PROFILE_GENERAL_LIFELINE;
        entry[4] = 0u;
        entry[5] = 0u;
        entry[6] = 0u;
        const zw_cc_agi_info_report_t rep = {
            .list_mode = req.list_mode, .dynamic = false, .count = 1u, .groups = entry, .groups_len = sizeof(entry),
        };
        const size_t n = zw_cc_agi_info_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_TIME && cmd == ZW_CMD_TIME_GET) {
        if (!zw_cc_time_get_parse(command, command_len)) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_time_report_t rep = {
            .clock_not_set = !identity->clock_trusted,
            .hour = identity->time_hour,
            .minute = identity->time_minute,
            .second = identity->time_second,
        };
        const size_t n = zw_cc_time_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    if (cc == (uint8_t)ZW_CC_TIME && cmd == ZW_CMD_DATE_GET) {
        if (!zw_cc_date_get_parse(command, command_len)) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        const zw_cc_date_report_t rep = {
            .year = identity->date_year, .month = identity->date_month, .day = identity->date_day,
        };
        const size_t n = zw_cc_date_report_build(out, cap, &rep);
        if (n == 0u) {
            return ZW_CC_ANSWER_NOT_HANDLED;
        }
        if (out_len != NULL) {
            *out_len = n;
        }
        return ZW_CC_ANSWER_REPLY;
    }

    /* §5 "anything else": not handled here (a wrapping Supervision should say 00, no support). */
    return ZW_CC_ANSWER_NOT_HANDLED;
}
