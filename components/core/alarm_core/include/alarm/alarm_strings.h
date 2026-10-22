/*
 * alarm_strings.h - stable snake_case identifiers for enums.
 *
 * These names are part of the external contract (event log export, MQTT JSON,
 * panel link). Never rename an existing identifier; only add new ones.
 */
#ifndef ALARM_STRINGS_H
#define ALARM_STRINGS_H

#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *alarm_state_name(alarm_arm_state_t state);
const char *alarm_level_name(alarm_arm_level_t level);
const char *alarm_zone_type_name(alarm_zone_type_t type);
const char *alarm_event_name(alarm_event_type_t type);
const char *alarm_cause_name(alarm_cause_t cause);
const char *alarm_sound_name(alarm_sound_t sound);
const char *alarm_result_name(alarm_result_t result);
const char *alarm_authority_name(alarm_authority_t authority);
const char *alarm_sys_trouble_name(alarm_sys_trouble_t trouble);
const char *alarm_zone_trouble_name(alarm_zone_trouble_t trouble);
const char *alarm_device_class_name(alarm_device_class_t cls);
const char *alarm_source_name(alarm_source_kind_t kind);
const char *alarm_panic_name(alarm_panic_t panic);
const char *alarm_fw_stage_name(alarm_fw_stage_t stage);
/* ALARM_EV_AUTOMATION_FAILED: its reason (arg) and its action (arg2, ALARM_AUTO_ACT_*) as JSON
 * words (docs/features/41 AUT-009). */
const char *alarm_auto_fail_name(alarm_auto_fail_t reason);
const char *alarm_auto_action_name(uint8_t action);
/* ALARM_EV_ARMING_PUT_OFF's arg: "hour", "night", "day" (docs/features/41 AUT-013). */
const char *alarm_put_off_name(alarm_put_off_t choice);
/* ALARM_EV_AUTOMATION_NOTICE's arg: "opened", "closed", "armed", "disarmed", "alarm", "fire",
 * "time", "trouble", "remind", "still_armed" (docs/features/41 BL-224; CORE-PROG-013). */
const char *alarm_auto_notice_name(alarm_auto_notice_t notice);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_STRINGS_H */
