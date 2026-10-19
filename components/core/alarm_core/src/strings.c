/*
 * strings.c - stable identifiers for enums (external contract: never rename).
 */
#include "alarm/alarm_strings.h"

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static const char *pick(const char *const *table, size_t n, unsigned v)
{
    return (v < n && table[v] != NULL) ? table[v] : "unknown";
}

const char *alarm_state_name(alarm_arm_state_t state)
{
    static const char *const t[] = {"disarmed", "exit_delay", "armed", "entry_delay"};
    return pick(t, COUNT_OF(t), (unsigned)state);
}

const char *alarm_level_name(alarm_arm_level_t level)
{
    static const char *const t[] = {"none", "away", "stay", "night"};
    return pick(t, COUNT_OF(t), (unsigned)level);
}

const char *alarm_zone_type_name(alarm_zone_type_t type)
{
    switch (type) {
    case ALARM_ZT_NOT_USED: return "not_used";
    case ALARM_ZT_ENTRY_EXIT_1: return "entry_exit_1";
    case ALARM_ZT_ENTRY_EXIT_2: return "entry_exit_2";
    case ALARM_ZT_PERIMETER: return "perimeter";
    case ALARM_ZT_INTERIOR_FOLLOWER: return "interior_follower";
    case ALARM_ZT_DAY_NIGHT: return "trouble_day_alarm_night";
    case ALARM_ZT_24H_SILENT: return "24h_silent";
    case ALARM_ZT_24H_AUDIBLE: return "24h_audible";
    case ALARM_ZT_24H_AUX: return "24h_aux";
    case ALARM_ZT_FIRE: return "fire";
    case ALARM_ZT_INTERIOR_DELAY: return "interior_delay";
    case ALARM_ZT_MONITOR: return "monitor";
    case ALARM_ZT_CO: return "carbon_monoxide";
    case ALARM_ZT_FIRE_VERIFIED: return "fire_verified";
    case ALARM_ZT_ARM_STAY: return "arm_stay";
    case ALARM_ZT_ARM_AWAY: return "arm_away";
    case ALARM_ZT_DISARM: return "disarm";
    case ALARM_ZT_NO_RESPONSE: return "no_response";
    case ALARM_ZT_SILENT_BURGLARY: return "silent_burglary";
    case ALARM_ZT_MOMENTARY_EXIT: return "momentary_exit";
    case ALARM_ZT_KEYSWITCH: return "keyswitch";
    default: return "unknown";
    }
}

const char *alarm_event_name(alarm_event_type_t type)
{
    static const char *const t[ALARM_EV_COUNT] = {
        [ALARM_EV_SYSTEM_START] = "system_start",
        [ALARM_EV_ARMED] = "armed",
        [ALARM_EV_ARM_FAILED] = "arm_failed",
        [ALARM_EV_DISARMED] = "disarmed",
        [ALARM_EV_AUTO_STAY] = "auto_stay",
        [ALARM_EV_EXIT_RESTART] = "exit_restart",
        [ALARM_EV_ENTRY_DELAY] = "entry_delay",
        [ALARM_EV_ALARM] = "alarm",
        [ALARM_EV_ALARM_RESTORE] = "alarm_restore",
        [ALARM_EV_EXIT_ERROR] = "exit_error",
        [ALARM_EV_RECENT_CLOSING] = "recent_closing",
        [ALARM_EV_ALARM_ABORTED] = "alarm_aborted",
        [ALARM_EV_ALARM_CANCELED] = "alarm_canceled",
        [ALARM_EV_ALARM_SILENCED] = "alarm_silenced",
        [ALARM_EV_BELL_TIMEOUT] = "bell_timeout",
        [ALARM_EV_SWINGER_SHUTDOWN] = "swinger_shutdown",
        [ALARM_EV_MEMORY_CLEARED] = "memory_cleared",
        [ALARM_EV_BYPASS] = "bypass",
        [ALARM_EV_UNBYPASS] = "unbypass",
        [ALARM_EV_TROUBLE] = "trouble",
        [ALARM_EV_TROUBLE_RESTORE] = "trouble_restore",
        [ALARM_EV_TROUBLES_ACKED] = "troubles_acked",
        [ALARM_EV_DURESS] = "duress",
        [ALARM_EV_INVALID_CODE] = "invalid_code",
        [ALARM_EV_LOCKOUT] = "lockout",
        [ALARM_EV_CHIME_ON] = "chime_on",
        [ALARM_EV_CHIME_OFF] = "chime_off",
        [ALARM_EV_WALK_TEST_START] = "walk_test_start",
        [ALARM_EV_WALK_TEST_END] = "walk_test_end",
        [ALARM_EV_WALK_TEST_ZONE] = "walk_test_zone",
        [ALARM_EV_MONITOR_FAULT] = "monitor_fault",
        [ALARM_EV_MONITOR_RESTORE] = "monitor_restore",
        [ALARM_EV_USER_CHANGED] = "user_changed",
        [ALARM_EV_USER_DELETED] = "user_deleted",
        [ALARM_EV_CONFIG_CHANGED] = "config_changed",
        [ALARM_EV_USER_UPDATED] = "user_updated",
        [ALARM_EV_PROGRAM_ENTER] = "program_enter",
        [ALARM_EV_PROGRAM_EXIT] = "program_exit",
        [ALARM_EV_FACTORY_RESET] = "factory_reset",
        [ALARM_EV_FIRMWARE_UPDATE] = "firmware_update",
        [ALARM_EV_ZWAVE_ADDED] = "zwave_device_added",
        [ALARM_EV_ZWAVE_ADDED_INSECURE] = "zwave_device_added_insecure",
        [ALARM_EV_ZWAVE_REMOVED] = "zwave_device_removed",
        [ALARM_EV_ZWAVE_LEFT] = "zwave_device_left",
        [ALARM_EV_ZWAVE_NETWORK_RESET] = "zwave_network_reset",
        [ALARM_EV_ZWAVE_RADIO_RESTARTED] = "zwave_radio_restarted",
        [ALARM_EV_ZWAVE_SOFTWARE] = "zwave_device_software",
        [ALARM_EV_ZWAVE_GARAGE] = "zwave_garage",
        [ALARM_EV_ZWAVE_JAMMING] = "zwave_jamming_reported",
        [ALARM_EV_BACKUP_MADE] = "backup_made",
        [ALARM_EV_BACKUP_RESTORED] = "backup_restored",
        [ALARM_EV_WALK_TEST_EXTEND] = "walk_test_extend",
        [ALARM_EV_WALK_TEST_WARNING] = "walk_test_warning",
        [ALARM_EV_CLOCK_SET] = "clock_set",
        [ALARM_EV_ZIGBEE_ADDED] = "zigbee_added",
        [ALARM_EV_ZIGBEE_REMOVED] = "zigbee_removed",
        [ALARM_EV_ZIGBEE_FORGOTTEN] = "zigbee_forgotten",
        [ALARM_EV_ZIGBEE_LEFT] = "zigbee_left",
        [ALARM_EV_ZIGBEE_REPLACED] = "zigbee_replaced",
        [ALARM_EV_ZIGBEE_JOIN_REFUSED] = "zigbee_join_refused",
        [ALARM_EV_ZIGBEE_CHANNEL] = "zigbee_channel",
        [ALARM_EV_ZIGBEE_NETWORK_RESET] = "zigbee_network_reset",
        [ALARM_EV_ZIGBEE_RADIO_RESTARTED] = "zigbee_radio_restarted",
        [ALARM_EV_AUTOMATION_FAILED] = "automation_failed",
        [ALARM_EV_ARMING_PUT_OFF] = "arming_put_off",
    };
    return pick(t, COUNT_OF(t), (unsigned)type);
}

const char *alarm_cause_name(alarm_cause_t cause)
{
    static const char *const t[ALARM_CAUSE_COUNT] = {
        [ALARM_CAUSE_NONE] = "none",
        [ALARM_CAUSE_BURGLARY] = "burglary",
        [ALARM_CAUSE_SILENT_BURGLARY] = "silent_burglary",
        [ALARM_CAUSE_EXIT_ERROR] = "exit_error",
        [ALARM_CAUSE_TAMPER] = "tamper",
        [ALARM_CAUSE_24H_SILENT] = "24h_silent",
        [ALARM_CAUSE_24H_AUDIBLE] = "24h_audible",
        [ALARM_CAUSE_24H_AUX] = "24h_aux",
        [ALARM_CAUSE_FIRE] = "fire",
        [ALARM_CAUSE_CO] = "carbon_monoxide",
        [ALARM_CAUSE_PANIC_POLICE] = "panic_police",
        [ALARM_CAUSE_PANIC_SILENT] = "panic_silent",
        [ALARM_CAUSE_PANIC_FIRE] = "panic_fire",
        [ALARM_CAUSE_PANIC_MEDICAL] = "panic_medical",
    };
    return pick(t, COUNT_OF(t), (unsigned)cause);
}

const char *alarm_sound_name(alarm_sound_t sound)
{
    static const char *const t[] = {
        [ALARM_SOUND_NONE] = "none",
        [ALARM_SOUND_TROUBLE] = "trouble",
        [ALARM_SOUND_EXIT] = "exit",
        [ALARM_SOUND_EXIT_FAST] = "exit_fast",
        [ALARM_SOUND_ENTRY] = "entry",
        [ALARM_SOUND_ENTRY_MEMORY] = "entry_memory",
        [ALARM_SOUND_AUX] = "aux",
        [ALARM_SOUND_ALARM] = "alarm",
        [ALARM_SOUND_CO] = "carbon_monoxide",
        [ALARM_SOUND_FIRE] = "fire",
        [ALARM_SOUND_ACK] = "ack",
        [ALARM_SOUND_ERROR] = "error",
        [ALARM_SOUND_CHIME] = "chime",
        [ALARM_SOUND_WALK_TEST] = "walk_test",
        [ALARM_SOUND_WALK_TEST_WARNING] = "walk_test_warning",
    };
    return pick(t, COUNT_OF(t), (unsigned)sound);
}

const char *alarm_result_name(alarm_result_t result)
{
    static const char *const t[] = {
        [ALARM_OK] = "ok",
        [ALARM_ERR_BAD_ARG] = "bad_arg",
        [ALARM_ERR_INVALID_CODE] = "invalid_code",
        [ALARM_ERR_CODE_REQUIRED] = "code_required",
        [ALARM_ERR_LOCKED_OUT] = "locked_out",
        [ALARM_ERR_NOT_AUTHORIZED] = "not_authorized",
        [ALARM_ERR_NOT_READY] = "not_ready",
        [ALARM_ERR_NOT_BYPASSABLE] = "not_bypassable",
        [ALARM_ERR_BAD_STATE] = "bad_state",
        [ALARM_ERR_CODE_IN_USE] = "code_in_use",
        [ALARM_ERR_DISABLED] = "disabled",
        [ALARM_ERR_UNAVAILABLE] = "unavailable",
        [ALARM_ERR_HELD] = "held",
    };
    return pick(t, COUNT_OF(t), (unsigned)result);
}

const char *alarm_authority_name(alarm_authority_t authority)
{
    static const char *const t[] = {
        [ALARM_AUTH_NONE] = "none",
        [ALARM_AUTH_INSTALLER] = "installer",
        [ALARM_AUTH_MASTER] = "master",
        [ALARM_AUTH_PARTITION_MASTER] = "partition_master",
        [ALARM_AUTH_STANDARD] = "standard",
        [ALARM_AUTH_ARM_ONLY] = "arm_only",
        [ALARM_AUTH_GUEST] = "guest",
        [ALARM_AUTH_DURESS] = "duress",
    };
    return pick(t, COUNT_OF(t), (unsigned)authority);
}

const char *alarm_sys_trouble_name(alarm_sys_trouble_t trouble)
{
    static const char *const t[ALARM_TRBL_COUNT] = {
        [ALARM_TRBL_AC_LOSS] = "ac_loss",
        [ALARM_TRBL_LOW_BATTERY] = "low_battery",
        [ALARM_TRBL_COMM_FAIL] = "comm_fail",
        [ALARM_TRBL_NETWORK] = "network",
        [ALARM_TRBL_CLOCK_NOT_SET] = "clock_not_set",
        [ALARM_TRBL_COPROCESSOR] = "coprocessor",
        [ALARM_TRBL_SENSOR_RADIO] = "sensor_radio",
        [ALARM_TRBL_BELL] = "bell",
        [ALARM_TRBL_PANEL_TAMPER] = "panel_tamper",
        [ALARM_TRBL_STORAGE] = "storage",
    };
    return pick(t, COUNT_OF(t), (unsigned)trouble);
}

const char *alarm_zone_trouble_name(alarm_zone_trouble_t trouble)
{
    static const char *const t[ALARM_ZTRBL_COUNT] = {
        [ALARM_ZTRBL_LOOP] = "loop",
        [ALARM_ZTRBL_TAMPER] = "tamper",
        [ALARM_ZTRBL_LOW_BATTERY] = "low_battery",
        [ALARM_ZTRBL_SUPERVISION] = "supervision",
        [ALARM_ZTRBL_DAY_FAULT] = "day_fault",
    };
    return pick(t, COUNT_OF(t), (unsigned)trouble);
}

const char *alarm_device_class_name(alarm_device_class_t cls)
{
    static const char *const t[] = {
        [ALARM_DEVCLASS_GENERIC] = "generic",
        [ALARM_DEVCLASS_DOOR] = "door",
        [ALARM_DEVCLASS_WINDOW] = "window",
        [ALARM_DEVCLASS_GARAGE_DOOR] = "garage_door",
        [ALARM_DEVCLASS_MOTION] = "motion",
        [ALARM_DEVCLASS_GLASS_BREAK] = "glass_break",
        [ALARM_DEVCLASS_SMOKE] = "smoke",
        [ALARM_DEVCLASS_HEAT] = "heat",
        [ALARM_DEVCLASS_CO] = "carbon_monoxide",
        [ALARM_DEVCLASS_GAS] = "gas",
        [ALARM_DEVCLASS_WATER] = "water",
        [ALARM_DEVCLASS_TEMPERATURE] = "temperature",
        [ALARM_DEVCLASS_VIBRATION] = "vibration",
        [ALARM_DEVCLASS_PANIC_BUTTON] = "panic_button",
        [ALARM_DEVCLASS_KEYFOB] = "keyfob",
        [ALARM_DEVCLASS_KEYSWITCH] = "keyswitch",
    };
    return pick(t, COUNT_OF(t), (unsigned)cls);
}

const char *alarm_source_name(alarm_source_kind_t kind)
{
    static const char *const t[] = {
        [ALARM_SRC_SYSTEM] = "system",
        [ALARM_SRC_KEYPAD] = "keypad",
        [ALARM_SRC_REMOTE] = "remote",
        [ALARM_SRC_ZONE] = "zone",
        [ALARM_SRC_SCHEDULE] = "schedule",
        [ALARM_SRC_AUTOMATION] = "automation",
    };
    return pick(t, COUNT_OF(t), (unsigned)kind);
}

const char *alarm_panic_name(alarm_panic_t panic)
{
    static const char *const t[] = {
        [ALARM_PANIC_POLICE] = "police",
        [ALARM_PANIC_SILENT] = "silent",
        [ALARM_PANIC_FIRE] = "fire",
        [ALARM_PANIC_MEDICAL] = "medical",
    };
    return pick(t, COUNT_OF(t), (unsigned)panic);
}

const char *alarm_fw_stage_name(alarm_fw_stage_t stage)
{
    static const char *const t[] = {
        [ALARM_FW_INSTALLING] = "installing",
        [ALARM_FW_UPDATED] = "updated",
        [ALARM_FW_ROLLED_BACK] = "rolled_back",
        [ALARM_FW_CP_UPDATED] = "coprocessor_updated",
        [ALARM_FW_CP_FAILED] = "coprocessor_failed",
        [ALARM_FW_SERVER_CHANGED] = "server_changed",
        [ALARM_FW_STOPPED] = "stopped",
        [ALARM_FW_ROLLING_BACK] = "rolling_back",
        [ALARM_FW_RESTART_PUT_OFF] = "restart_put_off",
        [ALARM_FW_KEYPAD_INSTALLING] = "keypad_installing",
        [ALARM_FW_KEYPAD_UPDATED] = "keypad_updated",
        [ALARM_FW_KEYPAD_ROLLED_BACK] = "keypad_rolled_back",
        [ALARM_FW_KEYPAD_FAILED] = "keypad_failed",
    };
    return pick(t, COUNT_OF(t), (unsigned)stage);
}

const char *alarm_auto_fail_name(alarm_auto_fail_t reason)
{
    static const char *const t[] = {
        [ALARM_AUTO_FAIL_UNREACHABLE] = "unreachable",
        [ALARM_AUTO_FAIL_NO_ANSWER] = "no_answer",
        [ALARM_AUTO_FAIL_REFUSED] = "refused",
        [ALARM_AUTO_FAIL_MISSING] = "missing",
        [ALARM_AUTO_FAIL_BUSY] = "busy",
        [ALARM_AUTO_FAIL_NOT_ALLOWED] = "not_allowed",
        [ALARM_AUTO_FAIL_NO_WARNING] = "no_warning",
    };
    return pick(t, COUNT_OF(t), (unsigned)reason);
}

const char *alarm_put_off_name(alarm_put_off_t choice)
{
    static const char *const t[] = {
        [ALARM_PUT_OFF_HOUR] = "hour",
        [ALARM_PUT_OFF_NIGHT] = "night",
        [ALARM_PUT_OFF_DAY] = "day",
    };
    return pick(t, COUNT_OF(t), (unsigned)choice);
}

const char *alarm_auto_action_name(uint8_t action)
{
    switch (action) {
    case ALARM_AUTO_ACT_ARM: return "arm";
    case ALARM_AUTO_ACT_LOCK: return "lock";
    case ALARM_AUTO_ACT_CLOSE: return "close";
    default: return "unknown";
    }
}
