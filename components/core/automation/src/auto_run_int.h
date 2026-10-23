/* The runner's insides shared by auto_run.c (matching, device sends) and auto_arm.c (arming and
 * disarming, phase C). Private to this component: nothing outside src/ includes it. */
#ifndef AUTOMATION_AUTO_RUN_INT_H
#define AUTOMATION_AUTO_RUN_INT_H

#include "automation/auto_run.h"

/* AUT-007: between two automatic arms or disarms of one partition. */
#define AUTO_PART_GAP_MS UINT64_C(10000)
/* D20: after an hour's put-off the warning starts again this long before the new time. */
#define AUTO_REWARN_MS (UINT64_C(5) * UINT64_C(60000))
#define AUTO_PUT_OFF_HOUR_MS UINT64_C(3600000)

/* auto_run.c */
int64_t auto_run_epoch_ms(const auto_run_t *r, uint64_t now);
/* The failed port, at most once per automation per hour unless the reason changes. */
void auto_run_report_failed(auto_run_t *r, auto_entry_t *e, uint8_t reason, uint8_t dev_action,
                            uint16_t zone, uint64_t now);
/* PL04: the send_begin port for automation uid (true when NULL); false: nothing may be sent for
 * it now (send_end has been called). True: call auto_run_send_end after the send. */
bool auto_run_send_begin(auto_run_t *r, uint16_t uid);
void auto_run_send_end(auto_run_t *r);

/* auto_arm.c */
bool auto_arm_is_arm_rule(const auto_rule_t *rule);
/* An Arm action's trigger (not a time of day): starts the warning unless one runs or it is put
 * off. arm_run_mark: the time trigger's arm-minute run to mark as seen, or AUTO_RUN_NONE. */
void auto_arm_start(auto_run_t *r, auto_entry_t *e, uint64_t now, int32_t arm_run_mark);
/* A Disarm action's trigger: disarms now, or once the partition gap allows. */
void auto_arm_disarm_start(auto_run_t *r, auto_entry_t *e, uint8_t idx, uint64_t now);
/* One reading's spans for a time-of-day Arm rule (the warning minute, the arm minute). */
void auto_arm_time(auto_run_t *r, auto_entry_t *e, const auto_span_t *s, size_t n, uint64_t now);
/* Ends the warning or hold (the warning port told) and drops a waiting disarm. */
void auto_arm_end(auto_run_t *r, auto_entry_t *e);
/* Holds that wait for a local time: ended by a valid reading at or past their end. */
void auto_arm_hold_check(auto_run_t *r, const auto_local_t *lt);
void auto_arm_tick(auto_run_t *r, auto_entry_t *e, uint64_t now);
/* An ARMED of that partition: its warnings end, noted "already armed". */
void auto_arm_partition_armed(auto_run_t *r, uint8_t partition, uint64_t now);
uint64_t auto_arm_deadline(const auto_run_t *r, const auto_entry_t *e, uint64_t d);
/* Is this ARMED or DISARMED the runner's own (AUT-007)? Updates its notes of what it did. */
bool auto_arm_own_occurrence(auto_run_t *r, const auto_occ_t *o);

/* auto_run.c, for auto_panel.c */
void auto_run_finish(auto_run_t *r, auto_entry_t *e, uint8_t idx, uint8_t res, uint8_t tried,
                     uint64_t now);
bool auto_run_cap_full(const auto_run_t *r, uint64_t now);
void auto_run_cap_note(auto_run_t *r, uint64_t now);

/* auto_panel.c (BL-224): outputs, the chime, notifications, reminders */
bool auto_panel_kind(uint8_t kind);
/* Action idx of a run; o the occurrence (NULL: a time, a close, Try it now). */
void auto_panel_run(auto_run_t *r, auto_entry_t *e, uint8_t idx, const auto_occ_t *o,
                    uint8_t tried, uint64_t now);
/* A time trigger's Remind or "still armed" crossed its minute in span s. */
void auto_panel_time(auto_run_t *r, auto_entry_t *e, const auto_span_t *s, uint64_t now);
/* An ARMED of that partition: its reminders' notes end. */
void auto_panel_partition_armed(auto_run_t *r, uint8_t partition);
/* Notes that wait for a local time: ended by a valid reading at or past their end. */
void auto_panel_hold_check(auto_run_t *r, const auto_local_t *lt);
void auto_panel_tick(auto_run_t *r, auto_entry_t *e, uint64_t now);
uint64_t auto_panel_deadline(const auto_entry_t *e, uint64_t d);
/* Ends the note and drops waiting output sends (changed, turned off, deleted); keep_timers: an
 * off timer already running still turns its output off (turned off). */
void auto_panel_end(auto_run_t *r, auto_entry_t *e, bool keep_timers);
bool auto_panel_wants_clock(const auto_run_t *r);

#endif /* AUTOMATION_AUTO_RUN_INT_H */
