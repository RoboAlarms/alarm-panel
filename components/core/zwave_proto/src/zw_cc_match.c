/*
 * zw_cc_match.c - §6 "which answer belongs to which question" (see zw_cc_match.h). Security 2/0
 * rows and No Operation (whose answer is the SendData ACK, not a command) are not handled here -
 * see zw_cc_match.h's header comment.
 */
#include "zwave_proto/zw_cc_match.h"

#include "zwave_proto/zw_cc.h"

bool zw_cc_answers(const uint8_t *sent, size_t sent_len, const uint8_t *got, size_t got_len)
{
    if (sent == NULL || got == NULL || sent_len < 2u || got_len < 2u) {
        return false;
    }
    const uint8_t scc = sent[0];
    const uint8_t scmd = sent[1];
    const uint8_t gcc = got[0];
    const uint8_t gcmd = got[1];

    /* Basic Get -> Basic Report */
    if (scc == (uint8_t)ZW_CC_BASIC && scmd == ZW_CMD_BASIC_GET) {
        return gcc == (uint8_t)ZW_CC_BASIC && gcmd == ZW_CMD_BASIC_REPORT;
    }

    /* Binary Sensor Get(type t) -> Report[type t, or a v1 report with no type: accept it] */
    if (scc == (uint8_t)ZW_CC_BINARY_SENSOR && scmd == ZW_CMD_BINARY_SENSOR_GET) {
        if (gcc != (uint8_t)ZW_CC_BINARY_SENSOR || gcmd != ZW_CMD_BINARY_SENSOR_REPORT) {
            return false;
        }
        zw_cc_binary_sensor_get_t sg;
        zw_cc_binary_sensor_report_t gr;
        if (!zw_cc_binary_sensor_get_parse(sent, sent_len, &sg) ||
            !zw_cc_binary_sensor_report_parse(got, got_len, &gr)) {
            return false;
        }
        return !sg.has_type || !gr.has_type || sg.sensor_type == gr.sensor_type;
    }
    if (scc == (uint8_t)ZW_CC_BINARY_SENSOR && scmd == ZW_CMD_BINARY_SENSOR_SUPPORTED_GET) {
        return gcc == (uint8_t)ZW_CC_BINARY_SENSOR && gcmd == ZW_CMD_BINARY_SENSOR_SUPPORTED_REPORT;
    }

    /* Alarm Sensor Supported Get -> Supported Report */
    if (scc == (uint8_t)ZW_CC_ALARM_SENSOR && scmd == ZW_CMD_ALARM_SENSOR_SUPPORTED_GET) {
        return gcc == (uint8_t)ZW_CC_ALARM_SENSOR && gcmd == ZW_CMD_ALARM_SENSOR_SUPPORTED_REPORT;
    }

    /* Multilevel Sensor Get(type t) -> Report[type t] */
    if (scc == (uint8_t)ZW_CC_MULTILEVEL_SENSOR && scmd == ZW_CMD_MULTILEVEL_SENSOR_GET) {
        if (gcc != (uint8_t)ZW_CC_MULTILEVEL_SENSOR || gcmd != ZW_CMD_MULTILEVEL_SENSOR_REPORT) {
            return false;
        }
        zw_cc_multilevel_sensor_get_t sg;
        zw_cc_multilevel_sensor_report_t gr;
        if (!zw_cc_multilevel_sensor_get_parse(sent, sent_len, &sg) ||
            !zw_cc_multilevel_sensor_report_parse(got, got_len, &gr)) {
            return false;
        }
        return !sg.has_type || sg.sensor_type == gr.sensor_type;
    }
    if (scc == (uint8_t)ZW_CC_MULTILEVEL_SENSOR && scmd == ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SENSOR_GET) {
        return gcc == (uint8_t)ZW_CC_MULTILEVEL_SENSOR &&
               gcmd == ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SENSOR_REPORT;
    }
    if (scc == (uint8_t)ZW_CC_MULTILEVEL_SENSOR && scmd == ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SCALE_GET) {
        if (gcc != (uint8_t)ZW_CC_MULTILEVEL_SENSOR || gcmd != ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SCALE_REPORT) {
            return false;
        }
        zw_cc_multilevel_sensor_supported_scale_get_t sg;
        zw_cc_multilevel_sensor_supported_scale_report_t gr;
        if (!zw_cc_multilevel_sensor_supported_scale_get_parse(sent, sent_len, &sg) ||
            !zw_cc_multilevel_sensor_supported_scale_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.sensor_type == gr.sensor_type;
    }

    /* Notification Get(type t) -> Report[type t, or a v1-only report] */
    if (scc == (uint8_t)ZW_CC_NOTIFICATION && scmd == ZW_CMD_NOTIFICATION_GET) {
        if (gcc != (uint8_t)ZW_CC_NOTIFICATION || gcmd != ZW_CMD_NOTIFICATION_REPORT) {
            return false;
        }
        zw_cc_notification_get_t sg;
        zw_cc_notification_report_t gr;
        if (!zw_cc_notification_get_parse(sent, sent_len, &sg) ||
            !zw_cc_notification_report_parse(got, got_len, &gr)) {
            return false;
        }
        return gr.is_v1 || sg.notification_type == gr.notification_type;
    }
    if (scc == (uint8_t)ZW_CC_NOTIFICATION && scmd == ZW_CMD_NOTIFICATION_SUPPORTED_GET) {
        return gcc == (uint8_t)ZW_CC_NOTIFICATION && gcmd == ZW_CMD_NOTIFICATION_SUPPORTED_REPORT;
    }
    if (scc == (uint8_t)ZW_CC_NOTIFICATION && scmd == ZW_CMD_NOTIFICATION_EVENT_SUPPORTED_GET) {
        if (gcc != (uint8_t)ZW_CC_NOTIFICATION || gcmd != ZW_CMD_NOTIFICATION_EVENT_SUPPORTED_REPORT) {
            return false;
        }
        zw_cc_notification_event_supported_get_t sg;
        zw_cc_notification_event_supported_report_t gr;
        if (!zw_cc_notification_event_supported_get_parse(sent, sent_len, &sg) ||
            !zw_cc_notification_event_supported_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.notification_type == gr.notification_type;
    }

    /* Battery Get -> Battery Report */
    if (scc == (uint8_t)ZW_CC_BATTERY && scmd == ZW_CMD_BATTERY_GET) {
        return gcc == (uint8_t)ZW_CC_BATTERY && gcmd == ZW_CMD_BATTERY_REPORT;
    }

    /* Wake Up Interval Get -> Interval Report; Capabilities Get -> Capabilities Report */
    if (scc == (uint8_t)ZW_CC_WAKE_UP && scmd == ZW_CMD_WAKE_UP_INTERVAL_GET) {
        return gcc == (uint8_t)ZW_CC_WAKE_UP && gcmd == ZW_CMD_WAKE_UP_INTERVAL_REPORT;
    }
    if (scc == (uint8_t)ZW_CC_WAKE_UP && scmd == ZW_CMD_WAKE_UP_INTERVAL_CAPABILITIES_GET) {
        return gcc == (uint8_t)ZW_CC_WAKE_UP && gcmd == ZW_CMD_WAKE_UP_INTERVAL_CAPABILITIES_REPORT;
    }

    /* Association Get(g) -> Report[g]; Supported Groupings Get -> Supported Groupings Report */
    if (scc == (uint8_t)ZW_CC_ASSOCIATION && scmd == ZW_CMD_ASSOCIATION_GET) {
        if (gcc != (uint8_t)ZW_CC_ASSOCIATION || gcmd != ZW_CMD_ASSOCIATION_REPORT) {
            return false;
        }
        zw_cc_association_get_t sg;
        zw_cc_association_report_t gr;
        if (!zw_cc_association_get_parse(sent, sent_len, &sg) ||
            !zw_cc_association_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.group == gr.group;
    }
    if (scc == (uint8_t)ZW_CC_ASSOCIATION && scmd == ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_GET) {
        return gcc == (uint8_t)ZW_CC_ASSOCIATION && gcmd == ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_REPORT;
    }

    /* Multi Channel Association Get(g) -> its Report[g] (same command IDs, its own CC) */
    if (scc == (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION && scmd == ZW_CMD_ASSOCIATION_GET) {
        if (gcc != (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION || gcmd != ZW_CMD_ASSOCIATION_REPORT) {
            return false;
        }
        zw_cc_mc_association_get_t sg;
        zw_cc_mc_association_report_t gr;
        if (!zw_cc_mc_association_get_parse(sent, sent_len, &sg) ||
            !zw_cc_mc_association_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.group == gr.group;
    }
    if (scc == (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION && scmd == ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_GET) {
        return gcc == (uint8_t)ZW_CC_MULTI_CHANNEL_ASSOCIATION &&
               gcmd == ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_REPORT;
    }

    /* AGI Name / Info / Command List Get(g) -> the matching Report[g] */
    if (scc == (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION && scmd == ZW_CMD_AGI_NAME_GET) {
        if (gcc != (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION || gcmd != ZW_CMD_AGI_NAME_REPORT) {
            return false;
        }
        zw_cc_agi_name_get_t sg;
        zw_cc_agi_name_report_t gr;
        if (!zw_cc_agi_name_get_parse(sent, sent_len, &sg) || !zw_cc_agi_name_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.group == gr.group;
    }
    if (scc == (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION && scmd == ZW_CMD_AGI_INFO_GET) {
        if (gcc != (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION || gcmd != ZW_CMD_AGI_INFO_REPORT) {
            return false;
        }
        zw_cc_agi_info_get_t sg;
        zw_cc_agi_info_report_t gr;
        if (!zw_cc_agi_info_get_parse(sent, sent_len, &sg) || !zw_cc_agi_info_report_parse(got, got_len, &gr)) {
            return false;
        }
        if (sg.list_mode) {
            return true; /* asked for everything: any list-mode report answers it */
        }
        zw_cc_agi_group_info_t first;
        if (!zw_cc_agi_group_info_get(gr.groups, gr.groups_len, 0u, &first)) {
            return false;
        }
        return first.group == sg.group;
    }
    if (scc == (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION && scmd == ZW_CMD_AGI_COMMAND_LIST_GET) {
        if (gcc != (uint8_t)ZW_CC_ASSOCIATION_GROUP_INFORMATION || gcmd != ZW_CMD_AGI_COMMAND_LIST_REPORT) {
            return false;
        }
        zw_cc_agi_command_list_get_t sg;
        zw_cc_agi_command_list_report_t gr;
        if (!zw_cc_agi_command_list_get_parse(sent, sent_len, &sg) ||
            !zw_cc_agi_command_list_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.group == gr.group;
    }

    /* Version Get / Command Class Get(cc) / Capabilities Get -> the matching Report[cc] */
    if (scc == (uint8_t)ZW_CC_VERSION && scmd == ZW_CMD_VERSION_GET) {
        return gcc == (uint8_t)ZW_CC_VERSION && gcmd == ZW_CMD_VERSION_REPORT;
    }
    if (scc == (uint8_t)ZW_CC_VERSION && scmd == ZW_CMD_VERSION_COMMAND_CLASS_GET) {
        if (gcc != (uint8_t)ZW_CC_VERSION || gcmd != ZW_CMD_VERSION_COMMAND_CLASS_REPORT) {
            return false;
        }
        zw_cc_version_command_class_get_t sg;
        zw_cc_version_command_class_report_t gr;
        if (!zw_cc_version_command_class_get_parse(sent, sent_len, &sg) ||
            !zw_cc_version_command_class_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.cc == gr.cc;
    }
    if (scc == (uint8_t)ZW_CC_VERSION && scmd == ZW_CMD_VERSION_CAPABILITIES_GET) {
        return gcc == (uint8_t)ZW_CC_VERSION && gcmd == ZW_CMD_VERSION_CAPABILITIES_REPORT;
    }

    /* Manufacturer Specific Get / Z-Wave Plus Info Get -> their Report */
    if (scc == (uint8_t)ZW_CC_MANUFACTURER_SPECIFIC && scmd == ZW_CMD_MANUFACTURER_SPECIFIC_GET) {
        return gcc == (uint8_t)ZW_CC_MANUFACTURER_SPECIFIC && gcmd == ZW_CMD_MANUFACTURER_SPECIFIC_REPORT;
    }
    if (scc == (uint8_t)ZW_CC_ZWAVE_PLUS_INFO && scmd == ZW_CMD_ZWAVE_PLUS_INFO_GET) {
        return gcc == (uint8_t)ZW_CC_ZWAVE_PLUS_INFO && gcmd == ZW_CMD_ZWAVE_PLUS_INFO_REPORT;
    }

    /* Multi Channel End Point Get / Capability Get(e) -> the matching Report[e] */
    if (scc == (uint8_t)ZW_CC_MULTI_CHANNEL && scmd == ZW_CMD_MULTI_CHANNEL_END_POINT_GET) {
        return gcc == (uint8_t)ZW_CC_MULTI_CHANNEL && gcmd == ZW_CMD_MULTI_CHANNEL_END_POINT_REPORT;
    }
    if (scc == (uint8_t)ZW_CC_MULTI_CHANNEL && scmd == ZW_CMD_MULTI_CHANNEL_CAPABILITY_GET) {
        if (gcc != (uint8_t)ZW_CC_MULTI_CHANNEL || gcmd != ZW_CMD_MULTI_CHANNEL_CAPABILITY_REPORT) {
            return false;
        }
        zw_cc_multi_channel_capability_get_t sg;
        zw_cc_multi_channel_capability_report_t gr;
        if (!zw_cc_multi_channel_capability_get_parse(sent, sent_len, &sg) ||
            !zw_cc_multi_channel_capability_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.endpoint == gr.endpoint;
    }

    /* Configuration Get(p) -> Report[p] */
    if (scc == (uint8_t)ZW_CC_CONFIGURATION && scmd == ZW_CMD_CONFIGURATION_GET) {
        if (gcc != (uint8_t)ZW_CC_CONFIGURATION || gcmd != ZW_CMD_CONFIGURATION_REPORT) {
            return false;
        }
        zw_cc_configuration_get_t sg;
        zw_cc_configuration_report_t gr;
        if (!zw_cc_configuration_get_parse(sent, sent_len, &sg) ||
            !zw_cc_configuration_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.parameter == gr.parameter;
    }

    /* Indicator Supported Get(i) -> Supported Report[i] */
    if (scc == (uint8_t)ZW_CC_INDICATOR && scmd == ZW_CMD_INDICATOR_SUPPORTED_GET) {
        if (gcc != (uint8_t)ZW_CC_INDICATOR || gcmd != ZW_CMD_INDICATOR_SUPPORTED_REPORT) {
            return false;
        }
        zw_cc_indicator_supported_get_t sg;
        zw_cc_indicator_supported_report_t gr;
        if (!zw_cc_indicator_supported_get_parse(sent, sent_len, &sg) ||
            !zw_cc_indicator_supported_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.indicator_id == gr.indicator_id;
    }

    /* Supervision Get(session s) -> Report[session s] */
    if (scc == (uint8_t)ZW_CC_SUPERVISION && scmd == ZW_CMD_SUPERVISION_GET) {
        if (gcc != (uint8_t)ZW_CC_SUPERVISION || gcmd != ZW_CMD_SUPERVISION_REPORT) {
            return false;
        }
        zw_cc_supervision_get_t sg;
        zw_cc_supervision_report_t gr;
        if (!zw_cc_supervision_get_parse(sent, sent_len, &sg) ||
            !zw_cc_supervision_report_parse(got, got_len, &gr)) {
            return false;
        }
        return sg.session_id == gr.session_id;
    }

    if (scc == (uint8_t)ZW_CC_ENTRY_CONTROL) {
        zw_cc_entry_keys_t keys;
        zw_cc_entry_events_t events;
        zw_cc_entry_configuration_t config;
        if (zw_cc_entry_key_supported_get_parse(sent, sent_len)) {
            return zw_cc_entry_keys_parse(got, got_len, &keys);
        }
        if (zw_cc_entry_event_supported_get_parse(sent, sent_len)) {
            return zw_cc_entry_events_parse(got, got_len, &events);
        }
        if (zw_cc_entry_configuration_get_parse(sent, sent_len)) {
            return zw_cc_entry_configuration_report_parse(got, got_len, &config);
        }
    }

    /* Security 2/0 rows and No Operation: out of scope here (see zw_cc_match.h). */
    return false;
}
