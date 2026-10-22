/* auto_smoke - the PC simulator runs the automations core (docs/features/41 U12, BL-168): the
 * real engine -> auto_watch -> auto_run -> the simulator's Smart Home devices, with Harbor
 * House's seeded automations. Windowless; exits non-zero when a check fails.
 *
 * AUT-004: what starts an automation (a door zone, the "Only between" window; never a silent
 * panic). AUT-006: each action sent once, answered, and a device that can't be reached ends
 * unreachable after the hold. AUT-007: the live kind is checked before a send. AUT-002: every
 * action sent passes hc_auto_allowed - the port refuses none of the seeded ones. AUT-015: an
 * unlock isn't sent while "Allow less secure automations" is off, and is once it is back on.
 * AUT-010: the panel's edits are refused while it is armed. U13: ui_backend_t's automation hooks
 * over the simulator's auto_store (AUT-001, AUT-002, AUT-006, AUT-008, AUT-010, AUT-015, AUT-016).
 * U15, through the touch UI: a change elsewhere redraws the list, a stale page's save is refused
 * CHANGED (AUT-008), a change while armed is refused (AUT-010), a refused save beeps and says why
 * (AUT-002). U16: the editors driven through the UI - one added through its steps and stored at
 * the last pick, Cancel storing nothing, an unlock after a door not stored without its confirm,
 * the option off leaving no Unlock (AUT-001, AUT-002, AUT-005, AUT-015, AUT-016).
 * Phase C (BL-170 C4): a time-of-day Night arm warns warn_min minutes before and arms on the time
 * (AUT-011, AUT-012); a put-off for an hour with any code (AUT-013, D26), "Not tonight" refused
 * with a code that may not disarm (AUT-013), a duress code still puts off (DESIGN.md section 8), a
 * wrong code counts toward the keypad's lockout (CORE-USR-034); "Bypass open zones" with the
 * installer option off arms without bypassing and fails on the open zone (AUT-014, D29).
 * C5, at the wall (test_ui_warning): the banner and Home's card from warn_min minutes before,
 * the tiles left as they are (AUT-012, D19); Put off, the choice and the code; the hold note in
 * the card's place and the banner gone, the warning back 5 minutes before the new time (AUT-013,
 * D20, D26, D27); Not tonight refused for an arm-only and a guest code, held until 6:00 AM with
 * the master's; the not-ready line (D28); wrong codes counting toward the lockout; arming by
 * hand ending the card, and an arming elsewhere closing the open pad.
 * BL-170 C6, the editors: Disarm offered after any trigger but an alarm, never after its own
 * armed trigger nor with the option off, its confirm with whole sentences (AUT-011, AUT-015,
 * AUT-016, D22, D23, D24); a stop after an armed trigger asks (U16 review a); the same action
 * after a kind change is stored (b); a stale save goes back to the list (c, AUT-008); a missing
 * device opens the picker (d); every editor entry point and delete (e); an Arm's page and its
 * outcomes (AUT-012, AUT-013, AUT-014, D30). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm_ui_gen.h"
#include "home_control/home_control.h"
#include "host_panel.h"
#include "lvgl.h"
#include "ui_app.h"
/* The UI's own header, for the editors' entry points (U16): the section's sheets are driven as a
 * finger would. Only function declarations depend on the feature switches. */
#define ALARM_UI_AUTOMATIONS 1
#include "../../components/ui/alarm_ui/src/ui_internal.h"

/* Monday 2026-01-12 under EST5EDT (no daylight time in January). */
#define AT_7PM_MS  1768262400000LL
#define AT_NOON_MS 1768237200000LL

static int s_checks, s_failed;
static ui_backend_t B;
static unsigned s_refused; /* the port's AUT-002 refusals, over every scenario */

static void check(bool ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL: %s\n", what);
    }
}

static ui_dev_t *dev(const char *ref)
{
    return (ui_dev_t *)host_panel_device(ref); /* the test plays the house: it may change them */
}

static void start(int64_t epoch_ms)
{
    if (s_checks > 0) {
        host_panel_auto_counts_t c;
        host_panel_auto_counts(&c);
        s_refused += c.refused;
    }
    host_panel_init(epoch_ms);
    host_panel_backend(&B);
    host_panel_advance(1000);
}

static host_panel_auto_counts_t counts(void)
{
    host_panel_auto_counts_t c;
    host_panel_auto_counts(&c);
    return c;
}

static uint8_t result(uint16_t uid)
{
    auto_status_t st;
    host_panel_auto_status(uid, &st);
    return st.result;
}

static alarm_result_t remote(alarm_cmd_type_t type, alarm_arm_level_t level)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.partition = 1;
    cmd.level = level;
    snprintf(cmd.code, sizeof(cmd.code), "123456"); /* the factory master code */
    return host_panel_remote_command(&cmd);
}

/* AUT-002: every seeded automation passes the validator against the simulator's panel. */
static void test_seeded(void)
{
    start(AT_7PM_MS);
    auto_rule_t r[AUTO_MAX];
    const size_t n = host_panel_auto_rules(r, AUTO_MAX);
    check(n == 4u, "AUT-001: four Harbor House automations (no leak rule: no water main valve)");
    for (size_t i = 0; i < n; i++) {
        check(r[i].uid == i + 1u && r[i].slot == i + 1u && r[i].rec == AUTO_REC_OK, "seeded ids");
        check(host_panel_auto_check(&r[i]) == AUTO_OK, "AUT-002: a seeded automation checks");
    }
    check(n == 4u && r[3].off, "the garage rule is turned off, as the mockup shows");
}

/* AUT-004, AUT-006: Front Door opens at 7 PM -> Porch Light on 300 ms later, off 10 minutes on. */
static void test_porch_evening(void)
{
    start(AT_7PM_MS);
    check(B.dev_act(B.ctx, "light.porch", ACT_OFF, -1, NULL, NULL) == UI_DEV_OK, "porch off first");
    host_panel_zone(1, ALARM_ZEV_FAULT);
    check(dev("light.porch")->status == UI_DEV_PENDING, "AUT-006: Porch Light under way at once");
    host_panel_advance(299);
    check(dev("light.porch")->state == UI_DEV_ST_OFF, "AUT-006: not on before the answer");
    host_panel_advance(1);
    check(dev("light.porch")->state == UI_DEV_ST_ON, "AUT-006: Porch Light on 300 ms later");
    check(dev("light.porch")->status == UI_DEV_IDLE, "AUT-006: answered");
    check(result(1) == AUTO_RES_DONE, "AUT-006: the door rule's status is done");
    host_panel_zone(1, ALARM_ZEV_RESTORE);
    host_panel_advance(9u * 60u * 1000u);
    check(dev("light.porch")->state == UI_DEV_ST_ON, "still on after 9 minutes");
    host_panel_advance(60u * 1000u + 400u);
    check(dev("light.porch")->state == UI_DEV_ST_OFF, "AUT-006: off 10 minutes later");
    check(counts().sent == 2u, "AUT-006: on, then off: two sends");
}

/* AUT-004: outside "Only between 6:00 PM and 6:00 AM" the same door does nothing. */
static void test_porch_noon(void)
{
    start(AT_NOON_MS);
    check(B.dev_act(B.ctx, "light.porch", ACT_OFF, -1, NULL, NULL) == UI_DEV_OK, "porch off first");
    host_panel_zone(1, ALARM_ZEV_FAULT);
    host_panel_advance(2000);
    check(dev("light.porch")->state == UI_DEV_ST_OFF, "AUT-004: at noon the window holds it");
    check(dev("light.porch")->status == UI_DEV_IDLE, "AUT-004: nothing under way at noon");
    check(counts().sent == 0u, "AUT-004: nothing sent at noon");
}

/* AUT-004, AUT-006: armed Away -> Front Door locked and Good Night run, once per arming. */
static void test_away(void)
{
    start(AT_7PM_MS);
    alarm_result_t cr = ALARM_OK;
    check(B.dev_act(B.ctx, "lock.front_door", ACT_UNLOCK, -1, "123456", &cr) == UI_DEV_OK,
          "unlock the front door first");
    const int64_t night_before = dev("scene.good_night")->since_ms;
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_AWAY) == ALARM_OK, "arming Away");
    host_panel_advance(5000);
    check(counts().sent == 0u, "AUT-004: nothing during the exit time");
    host_panel_advance(180000);
    check(dev("lock.front_door")->state == UI_DEV_ST_LOCKED, "AUT-006: Front Door locked");
    check(dev("scene.good_night")->since_ms != night_before, "AUT-006: Good Night ran");
    check(counts().sent == 2u, "AUT-006: two sends, once");
    check(result(2) == AUTO_RES_DONE, "the Away rule's status is done");
    host_panel_advance(120000);
    check(counts().sent == 2u, "AUT-006: still once while it stays armed");

    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarming");
    host_panel_advance(1000);
    check(B.dev_act(B.ctx, "lock.front_door", ACT_UNLOCK, -1, "123456", &cr) == UI_DEV_OK,
          "unlock again");
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_AWAY) == ALARM_OK, "arming Away again");
    host_panel_advance(185000);
    check(counts().sent == 4u, "AUT-006: once more for the second arming");
    check(dev("lock.front_door")->state == UI_DEV_ST_LOCKED, "locked again");

    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarming again");
    host_panel_advance(1000);
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_PANIC;
    cmd.partition = 1;
    cmd.panic = ALARM_PANIC_SILENT;
    snprintf(cmd.code, sizeof(cmd.code), "123456");
    check(host_panel_remote_command(&cmd) == ALARM_OK, "a silent panic");
    host_panel_advance(5000);
    check(counts().sent == 4u, "AUT-004: a silent panic starts nothing");
}

/* AUT-004: a fire alarm -> Hallway's mode off (the one climate action an automation may do). */
static void test_fire(void)
{
    start(AT_7PM_MS);
    check(dev("climate.hallway")->state == UI_DEV_ST_HEAT, "Hallway heats first");
    host_panel_zone(6, ALARM_ZEV_FAULT);
    host_panel_advance(1000);
    check(dev("climate.hallway")->state == UI_DEV_ST_OFF, "AUT-004: a fire alarm turns Hallway off");
    check(result(3) == AUTO_RES_DONE, "the fire rule's status is done");
}

/* AUT-006: a device its source can't reach is held, then the rule ends unreachable. */
static void test_offline(void)
{
    start(AT_7PM_MS);
    dev("light.porch")->status = UI_DEV_OFFLINE;
    host_panel_zone(1, ALARM_ZEV_FAULT);
    host_panel_advance(20000);
    check(result(1) == AUTO_RES_NONE, "AUT-006: held while the source tries again");
    host_panel_advance(15000);
    check(result(1) == AUTO_RES_UNREACHABLE, "AUT-006: unreachable after the 30 s hold");
    check(counts().sent == 0u, "AUT-006: nothing sent to an offline device");
}

/* AUT-007 / AUT-002 layer 4: the device became another kind -> nothing sent, can't run. */
static void test_kind_changed(void)
{
    start(AT_7PM_MS);
    dev("light.porch")->kind = DEV_SWITCH;
    host_panel_zone(1, ALARM_ZEV_FAULT);
    host_panel_advance(2000);
    check(counts().sent == 0u, "AUT-002: nothing sent to a device of another kind");
    check(result(1) == AUTO_RES_CANT_RUN, "AUT-007: the door rule can't run");
}

/* AUT-015 (D25): "When the panel is disarmed: Unlock Back Door", saved by the master. With the
 * installer option off it is kept but never sent (less_secure_off); back on, it is sent. */
static void test_unlock_and_the_option(void)
{
    start(AT_7PM_MS);
    host_panel_automations(false);
    alarm_result_t cr = ALARM_OK;
    check(B.dev_act(B.ctx, "lock.back_door", ACT_LOCK, -1, NULL, &cr) == UI_DEV_OK,
          "lock the back door first");
    host_panel_advance(1000);
    auto_rule_t r;
    memset(&r, 0, sizeof(r));
    r.n = 1;
    r.made_by = 2; /* the factory master's slot */
    r.when.kind = AUTO_TRIG_DISARMED;
    r.then[0].kind = AUTO_ACT_DEVICE;
    r.then[0].source = DSRC_HA;
    r.then[0].dev_kind = DEV_LOCK;
    r.then[0].dev_action = ACT_UNLOCK;
    r.then[0].value = -1;
    snprintf(r.then[0].ref, sizeof(r.then[0].ref), "lock.back_door");
    snprintf(r.then[0].name, sizeof(r.then[0].name), "Back Door");

    alarm_config_t cfg;
    check(B.get_config(B.ctx, &cfg), "read the configuration");
    cfg.opt.auto_less_secure = false;
    check(B.set_config(B.ctx, &cfg) == ALARM_OK, "AUT-015: the installer turns the option off");
    check(host_panel_auto_add_unchecked(&r), "add the unlock rule");
    check(host_panel_auto_check(&r) == AUTO_ERR_LESS_SECURE_OFF,
          "AUT-015: a save of it would be refused while the option is off");
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_STAY) == ALARM_OK, "arming Stay");
    host_panel_advance(1000);
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarming");
    host_panel_advance(2000);
    check(dev("lock.back_door")->state == UI_DEV_ST_LOCKED, "AUT-015: not unlocked while off");
    check(counts().sent == 0u && counts().refused == 0u, "AUT-015: nothing sent, nothing refused");
    check(result(1) == AUTO_RES_LESS_SECURE_OFF, "AUT-015: the status says the option is off");
    auto_rule_t kept[AUTO_MAX];
    check(host_panel_auto_rules(kept, AUTO_MAX) == 1u && kept[0].rec == AUTO_REC_OK &&
              kept[0].then[0].dev_action == ACT_UNLOCK,
          "AUT-015: the rule is kept as it is");

    cfg.opt.auto_less_secure = true;
    check(B.set_config(B.ctx, &cfg) == ALARM_OK, "AUT-015: the option back on");
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_STAY) == ALARM_OK, "arming Stay again");
    host_panel_advance(1000);
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarming again");
    host_panel_advance(2000);
    check(counts().sent == 1u, "AUT-015: the unlock is sent once the option is on");
    check(dev("lock.back_door")->state == UI_DEV_ST_UNLOCKED, "AUT-015: Back Door unlocked");
    check(result(1) == AUTO_RES_DONE, "the unlock rule's status is done");
}

/* AUT-010, AUT-015: the simulated panel's edits answer as automation_service's do - nobody
 * changes an automation while armed (LOCKED: save, turn off and delete alike), a stale page is
 * told CHANGED, a save is checked against the panel's own option (an unlock refused while it is
 * off) and makes the saver the maker. */
static void test_edits(void)
{
    start(AT_7PM_MS);
    host_panel_automations(false);
    auto_rule_t r;
    memset(&r, 0, sizeof(r));
    r.n = 1;
    r.when.kind = AUTO_TRIG_DISARMED;
    r.then[0].kind = AUTO_ACT_DEVICE;
    r.then[0].source = DSRC_HA;
    r.then[0].dev_kind = DEV_LOCK;
    r.then[0].dev_action = ACT_LOCK;
    r.then[0].value = -1;
    snprintf(r.then[0].ref, sizeof(r.then[0].ref), "lock.back_door");
    snprintf(r.then[0].name, sizeof(r.then[0].name), "Back Door");
    auto_check_t why = AUTO_OK;
    uint16_t uid = 0;
    const uint16_t gen = host_panel_auto_gen();
    check(host_panel_auto_save(&r, 2, (uint16_t)(gen + 1u), &why, &uid) == AUTO_SAVE_CHANGED,
          "a stale page is told it changed");
    check(host_panel_auto_save(&r, 2, gen, &why, &uid) == AUTO_SAVE_OK && uid != 0u,
          "AUT-010: saved while disarmed");
    auto_rule_t got[AUTO_MAX];
    check(host_panel_auto_rules(got, AUTO_MAX) == 1u && got[0].made_by == 2u,
          "the saver is the maker");

    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_STAY) == ALARM_OK, "arming Stay");
    host_panel_advance(1000);
    r.uid = uid;
    check(host_panel_auto_save(&r, 2, host_panel_auto_gen(), &why, NULL) == AUTO_SAVE_LOCKED,
          "AUT-010: no save while armed");
    check(host_panel_auto_set_off(uid, true, host_panel_auto_gen()) == AUTO_SAVE_LOCKED,
          "AUT-010: no turning off while armed");
    check(host_panel_auto_delete(uid, host_panel_auto_gen()) == AUTO_SAVE_LOCKED,
          "AUT-010: no delete while armed");
    check(host_panel_auto_rules(got, AUTO_MAX) == 1u && !got[0].off, "AUT-010: nothing changed");
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarming");
    host_panel_advance(1000);
    check(host_panel_auto_set_off(uid, true, host_panel_auto_gen()) == AUTO_SAVE_OK,
          "AUT-010: turned off once disarmed");

    /* AUT-015: the installer option off - an unlock is refused at save */
    alarm_config_t cfg;
    check(B.get_config(B.ctx, &cfg), "read the configuration");
    cfg.opt.auto_less_secure = false;
    check(B.set_config(B.ctx, &cfg) == ALARM_OK, "the installer turns the option off");
    r.uid = 0;
    r.then[0].dev_action = ACT_UNLOCK;
    check(host_panel_auto_save(&r, 2, host_panel_auto_gen(), &why, NULL) == AUTO_SAVE_INVALID &&
              why == AUTO_ERR_LESS_SECURE_OFF,
          "AUT-015: no unlock saved while the option is off");
    check(host_panel_auto_delete(uid, host_panel_auto_gen()) == AUTO_SAVE_OK &&
              host_panel_auto_rules(got, AUTO_MAX) == 0u,
          "deleted once disarmed");
    cfg.opt.auto_less_secure = true;
    check(B.set_config(B.ctx, &cfg) == ALARM_OK, "the option back on");
}

/* A rule for the hook checks: "When the panel is disarmed: <action> <ref>". */
static auto_rule_t hook_rule(uint8_t kind, uint8_t action, const char *ref)
{
    auto_rule_t r;
    memset(&r, 0, sizeof(r));
    r.n = 1;
    r.when.kind = AUTO_TRIG_DISARMED;
    r.then[0].kind = AUTO_ACT_DEVICE;
    r.then[0].source = DSRC_HA;
    r.then[0].dev_kind = kind;
    r.then[0].dev_action = action;
    r.then[0].value = -1;
    snprintf(r.then[0].ref, sizeof(r.then[0].ref), "%s", ref);
    snprintf(r.then[0].name, sizeof(r.then[0].name), "%s", ref);
    return r;
}

static size_t hook_list(auto_rule_t *out)
{
    return B.auto_list(B.ctx, out, AUTO_MAX);
}

/* U13: ui_backend_t's automation hooks over the simulator's auto_store, answering as
 * automation_service does - what Settings > Automations will call. */
static void test_hooks(void)
{
    start(AT_7PM_MS);
    check(B.auto_list != NULL && B.auto_gen != NULL && B.auto_read_only != NULL &&
              B.auto_less_secure != NULL && B.auto_save != NULL && B.auto_delete_slot != NULL &&
              B.auto_set_off != NULL && B.auto_status != NULL && B.auto_try != NULL,
          "the simulator fills every automation hook");
    auto_rule_t got[AUTO_MAX];
    check(hook_list(got) == 4u && got[0].uid == 1u && got[3].slot == 4u,
          "AUT-008: the seeded store lists uids and slots 1..4 in slot order");
    check(!B.auto_read_only(B.ctx), "the store loaded writable");

    /* AUT-008: a save moves gen by one and lists in the first empty slot */
    auto_rule_t r = hook_rule(DEV_LOCK, ACT_LOCK, "lock.back_door");
    auto_check_t why = AUTO_OK;
    uint16_t uid = 0;
    const uint16_t gen = B.auto_gen(B.ctx);
    check(B.auto_save(B.ctx, &r, 2, gen, &why, &uid) == AUTO_SAVE_OK && uid == 5u,
          "AUT-008: a new automation is saved with the next uid");
    check(B.auto_gen(B.ctx) == (uint16_t)(gen + 1u), "AUT-008: one save, gen + 1");
    check(hook_list(got) == 5u && got[4].uid == 5u && got[4].slot == 5u && got[4].made_by == 2u,
          "AUT-008: listed after the others, the saver its maker (D23)");
    check(B.auto_save(B.ctx, &r, 2, gen, &why, NULL) == AUTO_SAVE_CHANGED,
          "AUT-008: a page read before that save is told CHANGED");
    check(hook_list(got) == 5u, "AUT-008: the stale save stored nothing");

    /* AUT-002: a script is never run by an automation, whatever the option */
    host_panel_devices_extra(true);
    check(host_panel_device("script.movie_night") != NULL, "the extra devices add a script");
    hc_auto_offer_t offer[HC_AUTO_OFFER_MAX];
    check(hc_auto_offer(DEV_SCRIPT, DSRC_HA, true, offer) == 0u, "AUT-002: a script offers nothing");
    r = hook_rule(DEV_SCRIPT, ACT_RUN, "script.movie_night");
    r.then[0].confirmed = 1;
    check(B.auto_save(B.ctx, &r, 2, B.auto_gen(B.ctx), &why, NULL) == AUTO_SAVE_INVALID &&
              why == AUTO_ERR_NOT_ALLOWED,
          "AUT-002: a save holding a script is refused NOT_ALLOWED");
    host_panel_devices_extra(false);
    check(host_panel_device("script.movie_night") == NULL, "the extra devices go again");

    /* AUT-015: an unlock with the installer option off; AUT-016: after a door, only confirmed */
    host_panel_auto_less_secure(false);
    check(!B.auto_less_secure(B.ctx), "AUT-015: the hook reads the option off");
    r = hook_rule(DEV_LOCK, ACT_UNLOCK, "lock.back_door");
    check(B.auto_save(B.ctx, &r, 2, B.auto_gen(B.ctx), &why, NULL) == AUTO_SAVE_INVALID &&
              why == AUTO_ERR_LESS_SECURE_OFF,
          "AUT-015: no unlock saved while the option is off");
    host_panel_auto_less_secure(true);
    check(B.auto_less_secure(B.ctx), "AUT-015: the hook reads the option on, live");
    r.when.kind = AUTO_TRIG_ZONE_OPENED;
    r.when.zone = 1;
    check(B.auto_save(B.ctx, &r, 2, B.auto_gen(B.ctx), &why, NULL) == AUTO_SAVE_INVALID &&
              why == AUTO_ERR_NEEDS_CONFIRM,
          "AUT-016: an unlock after a door opens needs its confirm");
    r.then[0].confirmed = 1;
    check(B.auto_save(B.ctx, &r, 2, B.auto_gen(B.ctx), &why, &uid) == AUTO_SAVE_OK,
          "AUT-016: confirmed, it is saved");

    /* AUT-006: Try it now runs the actions; the status says tried */
    check(B.auto_try(B.ctx, 999) == AUTO_TRY_UNKNOWN, "AUT-006: an unknown uid isn't tried");
    check(B.dev_act(B.ctx, "light.porch", ACT_OFF, -1, NULL, NULL) == UI_DEV_OK, "porch off first");
    check(B.auto_try(B.ctx, 1) == AUTO_TRY_OK, "AUT-006: Try it now is taken");
    host_panel_advance(1000);
    auto_status_t st;
    check(B.auto_status(B.ctx, 1, &st) && st.result == AUTO_RES_DONE && st.tried == 1u,
          "AUT-006: done, and marked tried");
    check(dev("light.porch")->state == UI_DEV_ST_ON, "AUT-006: Porch Light turned on");
    check(!B.auto_status(B.ctx, 999, &st), "an unknown uid has no status");
    auto_status_t forced = {.result = AUTO_RES_UNREACHABLE, .action_index = 0};
    host_panel_auto_force_status(2, &forced);
    check(B.auto_status(B.ctx, 2, &st) && st.result == AUTO_RES_UNREACHABLE,
          "a forced status is what the hook answers");
    host_panel_auto_force_status(2, NULL);

    /* AUT-008: a refused write changes nothing */
    host_panel_auto_store_fail(true);
    r = hook_rule(DEV_LOCK, ACT_LOCK, "lock.front_door");
    check(B.auto_save(B.ctx, &r, 2, B.auto_gen(B.ctx), &why, NULL) == AUTO_SAVE_FAILED &&
              hook_list(got) == 6u,
          "AUT-008: a refused write answers FAILED, nothing stored");
    host_panel_auto_store_fail(false);

    /* AUT-001: sixteen at most */
    size_t n = hook_list(got);
    bool ok = true;
    while (n < AUTO_MAX && ok) {
        ok = B.auto_save(B.ctx, &r, 2, B.auto_gen(B.ctx), &why, NULL) == AUTO_SAVE_OK;
        n = hook_list(got);
    }
    check(ok && n == AUTO_MAX, "AUT-001: filled to sixteen");
    check(B.auto_save(B.ctx, &r, 2, B.auto_gen(B.ctx), &why, NULL) == AUTO_SAVE_FULL,
          "AUT-001: a 17th is refused FULL");

    /* AUT-008: a store that loaded read-only refuses every edit */
    host_panel_auto_read_only(true);
    check(B.auto_read_only(B.ctx), "the hook says read-only");
    check(B.auto_delete_slot(B.ctx, 16, B.auto_gen(B.ctx)) == AUTO_SAVE_READ_ONLY &&
              B.auto_set_off(B.ctx, 1, true, B.auto_gen(B.ctx)) == AUTO_SAVE_READ_ONLY,
          "AUT-008: read-only refuses delete and turn off");
    (void)hook_list(got);
    check(B.auto_save(B.ctx, &got[0], 2, B.auto_gen(B.ctx), &why, NULL) == AUTO_SAVE_READ_ONLY,
          "AUT-008: read-only refuses a save");
    host_panel_auto_read_only(false);
    check(B.auto_delete_slot(B.ctx, 16, B.auto_gen(B.ctx)) == AUTO_SAVE_OK && hook_list(got) == 15u,
          "AUT-008: deleted by slot once writable");

    /* AUT-008: a NEWER record is listed, can't be changed, can be deleted */
    auto_rule_t newer = hook_rule(DEV_LOCK, ACT_LOCK, "lock.front_door");
    newer.rec = AUTO_REC_NEWER;
    check(host_panel_auto_inject(&newer) && hook_list(got) == AUTO_MAX &&
              got[AUTO_MAX - 1u].rec == AUTO_REC_NEWER,
          "AUT-008: a NEWER record is listed");
    const uint16_t newer_uid = got[AUTO_MAX - 1u].uid;
    check(B.auto_set_off(B.ctx, newer_uid, true, B.auto_gen(B.ctx)) == AUTO_SAVE_NOT_EDITABLE,
          "AUT-008: a NEWER record can't be turned off");
    check(B.auto_delete_slot(B.ctx, got[AUTO_MAX - 1u].slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK,
          "AUT-008: a NEWER record can be deleted");

    /* AUT-010: nobody changes an automation while the panel is armed */
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_STAY) == ALARM_OK, "arming Stay");
    host_panel_advance(1000);
    check(B.auto_save(B.ctx, &r, 2, B.auto_gen(B.ctx), &why, NULL) == AUTO_SAVE_LOCKED &&
              B.auto_set_off(B.ctx, 1, true, B.auto_gen(B.ctx)) == AUTO_SAVE_LOCKED &&
              B.auto_delete_slot(B.ctx, 1, B.auto_gen(B.ctx)) == AUTO_SAVE_LOCKED,
          "AUT-010: armed, every edit is LOCKED");
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarming");
    host_panel_advance(1000);

    /* ui_dev_t.source: the device's source, for an action to name */
    ui_dev_t devs[32];
    const size_t nd = B.dev_list(B.ctx, devs, 32);
    bool all_ha = nd > 0u;
    for (size_t i = 0; i < nd; i++) {
        all_ha = all_ha && devs[i].source == DSRC_HA;
    }
    check(all_ha, "every listed device names its source");
}

/* host_panel_automations(false): the old behaviour for walks. */
static void test_off_switch(void)
{
    start(AT_7PM_MS);
    host_panel_automations(false);
    check(B.dev_act(B.ctx, "light.porch", ACT_OFF, -1, NULL, NULL) == UI_DEV_OK, "porch off first");
    host_panel_zone(1, ALARM_ZEV_FAULT);
    host_panel_advance(2000);
    check(dev("light.porch")->state == UI_DEV_ST_OFF && counts().sent == 0u,
          "without automations the door does nothing");
}


/* ---- through the touch UI (docs/features/41 U15): what the hooks alone can't show ---------- */

#ifndef ALARM_SCREEN_W
#define ALARM_SCREEN_W 1024
#define ALARM_SCREEN_H 600
#endif

static uint16_t s_fb[ALARM_SCREEN_W * ALARM_SCREEN_H];
static uint32_t s_ms;
static int32_t s_px, s_py;
static bool s_pressed;
static unsigned s_error_beeps;

static uint32_t tick_cb(void)
{
    return s_ms;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(disp);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_px;
    data->point.y = s_py;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void beep_cb(void *ctx, ui_beep_t b)
{
    (void)ctx;
    if (b == UI_BEEP_ERROR) {
        s_error_beeps++;
    }
}

static void ui_run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10u) {
        s_ms += 10u;
        host_panel_advance(10u);
        lv_timer_handler();
    }
}

static bool shown(lv_obj_t *o)
{
    for (; o != NULL; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
    }
    return true;
}

static lv_obj_t *named(lv_obj_t *root, const char *name)
{
    const char *n = lv_obj_get_name(root);
    if (n != NULL && strcmp(n, name) == 0 && shown(root)) {
        return root;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *o = named(lv_obj_get_child(root, (int32_t)i), name);
        if (o != NULL) {
            return o;
        }
    }
    return NULL;
}

static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if (root == NULL) {
        return NULL;
    }
    if (lv_obj_check_type(root, &lv_label_class) && shown(root) &&
        strcmp(lv_label_get_text(root), text) == 0) {
        return root;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *o = label(lv_obj_get_child(root, (int32_t)i), text);
        if (o != NULL) {
            return o;
        }
    }
    return NULL;
}

static unsigned labels(lv_obj_t *root, const char *text)
{
    unsigned n = 0;
    if (root == NULL) {
        return 0;
    }
    if (lv_obj_check_type(root, &lv_label_class) && shown(root) &&
        strcmp(lv_label_get_text(root), text) == 0) {
        n++;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        n += labels(lv_obj_get_child(root, (int32_t)i), text);
    }
    return n;
}

static void press(lv_obj_t *o)
{
    if (o == NULL) {
        return;
    }
    lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
    ui_run(60);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    s_px = (a.x1 + a.x2) / 2;
    s_py = (a.y1 + a.y2) / 2;
    s_pressed = true;
    ui_run(60);
    s_pressed = false;
    ui_run(200);
}

/* A row of the page, by its title: its clickable row. */
static void press_text(const char *text)
{
    lv_obj_t *o = label(named(lv_screen_active(), "page_body"), text);
    while (o != NULL && !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) {
        o = lv_obj_get_parent(o);
    }
    press(o);
}

static void type_code(const char *code)
{
    lv_obj_t *keys = lv_obj_find_by_name(named(lv_screen_active(), "settings_pad"), "pad_keys");
    for (const char *k = code; keys != NULL && *k != '\0'; k++) {
        const int idx = (*k == '0') ? 10 : (*k == 'k') ? 11 : (*k - '1');
        press(lv_obj_get_child(keys, idx));
    }
}

static const char *page_note(void)
{
    return lv_subject_get_string(&subject_page_note);
}

static bool toasted(const char *text)
{
    return lv_subject_get_int(&subject_toast_visible) == 1 &&
           strcmp(lv_subject_get_string(&subject_toast), text) == 0;
}

/* Settings > Automations, opened directly (its line stays hidden until BL-131 finding 7), with
 * the master code. */
static void open_automations(void)
{
    press(named(lv_screen_active(), "bar_settings"));
    lv_obj_send_event(lv_obj_find_by_name(lv_screen_active(), "set_automations"), LV_EVENT_CLICKED, NULL);
    ui_run(300);
    if (lv_subject_get_int(&subject_settings_pad) == 1) {
        type_code("123456k");
        ui_run(1100);
    }
}

/* The switch of the open automation's page, flipped as a finger would. */
static void flip_use(void)
{
    lv_obj_t *sw = lv_obj_find_by_name(named(lv_screen_active(), "page_body"), "toggle");
    lv_obj_set_state(sw, LV_STATE_CHECKED, !lv_obj_has_state(sw, LV_STATE_CHECKED));
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}

static void test_ui(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(ALARM_SCREEN_W, ALARM_SCREEN_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, pointer_read);

    start(AT_7PM_MS);
    ui_backend_t be = B;
    be.beep = beep_cb;
    ui_app_start(&be, NULL);
    ui_run(400);

    /* AUT-010, D3: the section asks for the master or installer code, then lists them */
    open_automations();
    check(strcmp(page_note(), "4 of 16 automations") == 0, "AUT-010: the master code opens the list");

    /* AUT-008: a change made elsewhere (the Panel Manager) is drawn at the next tick */
    check(host_panel_auto_set_off(3, true, host_panel_auto_gen()) == AUTO_SAVE_OK, "turned off elsewhere");
    check(labels(named(lv_screen_active(), "page_body"), "Off") == 1u, "only the garage rule says Off");
    ui_run(1100);
    check(labels(named(lv_screen_active(), "page_body"), "Off") == 2u && strcmp(page_note(), "4 of 16 automations") == 0,
          "AUT-008: the list was drawn again after a change elsewhere: the fire rule says Off");

    /* AUT-008: a stale page's save is refused CHANGED, beeps and says so; the page is drawn
     * again from the store, so the next save goes through */
    press_text("When Front Door opens");
    ui_run(1100);
    check(lv_subject_get_int(&subject_page_back) == 1, "the automation's page opened");
    check(host_panel_auto_set_off(3, false, host_panel_auto_gen()) == AUTO_SAVE_OK, "changed elsewhere");
    unsigned beeps = s_error_beeps;
    flip_use(); /* before the tick has seen the change */
    ui_run(100);
    auto_rule_t got[AUTO_MAX];
    check(s_error_beeps == beeps + 1u && toasted("Changed somewhere else meanwhile: here it is now"),
          "AUT-008: a stale page's save beeps and says it changed");
    check(host_panel_auto_rules(got, AUTO_MAX) == 4u && !got[0].off, "AUT-008: the stale save changed nothing");
    check(lv_obj_has_state(lv_obj_find_by_name(named(lv_screen_active(), "page_body"), "toggle"), LV_STATE_CHECKED),
          "AUT-008: the switch shows what is stored again");
    flip_use();
    ui_run(1100);
    check(host_panel_auto_rules(got, AUTO_MAX) == 4u && got[0].off && s_error_beeps == beeps + 1u,
          "AUT-008: once drawn again, the page's save goes through");
    check(label(named(lv_screen_active(), "page_body"), "Off \xC2\xB7 it won't run") != NULL,
          "AUT-010: turned off, the page says it won't run");
    flip_use();
    ui_run(1100);
    check(host_panel_auto_rules(got, AUTO_MAX) == 4u && !got[0].off, "turned on again");

    /* AUT-002: a save the panel refuses beeps and says why; nothing is stored */
    auto_rule_t r = hook_rule(DEV_SCRIPT, ACT_RUN, "script.water_garden");
    beeps = s_error_beeps;
    check(!ui_auto_save(&r, NULL), "AUT-002: a script isn't saved");
    check(s_error_beeps == beeps + 1u && toasted("An automation can't do that"),
          "AUT-002: the refused save beeps and says why");
    check(host_panel_auto_rules(got, AUTO_MAX) == 4u, "AUT-002: nothing was stored");
    ui_run(1100);

    /* AUT-010: armed elsewhere while the page is open - the save the page makes before it
     * hears of it is refused, beeps and says so; then Settings closes (UI-022) */
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_STAY) == ALARM_OK, "armed Stay elsewhere");
    host_panel_advance(1000); /* the engine only: the screen hasn't caught up yet */
    beeps = s_error_beeps;
    flip_use();
    check(s_error_beeps == beeps + 1u && toasted("Automations can be changed once the system is disarmed"),
          "AUT-010: a change while armed beeps and says why");
    check(host_panel_auto_rules(got, AUTO_MAX) == 4u && !got[0].off, "AUT-010: nothing changed while armed");
    ui_run(1100);
    check(lv_subject_get_int(&subject_view) != 5, "UI-022: Settings closed once armed");
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarmed");
    ui_run(1100);
}

/* ---- U16: the editors, driven through the UI (ui_auto_edit.c; DESIGN.md section 9) ---------
 * The section is opened directly (its Settings line stays hidden, R4); each sheet is answered as
 * a finger would, by its rows' titles, in test_ui's LVGL session. */

static int32_t sheet_kind(void)
{
    return lv_subject_get_int(&subject_sheet);
}

static const char *sheet_title(void)
{
    return lv_subject_get_string(&subject_sheet_title);
}

static const char *sheet_note(void)
{
    return lv_subject_get_string(&subject_sheet_note);
}

/* The pick sheet's row with this title, or NULL. */
static lv_obj_t *row(const char *title)
{
    lv_obj_t *list = sheet_kind() == SHEET_PICK ? ui_find(lv_screen_active(), "pick_list") : NULL;
    for (uint32_t i = 0; list != NULL && i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *r = lv_obj_get_child(list, (int32_t)i);
        lv_obj_t *t = ui_find(r, "title_label");
        if (t != NULL && strcmp(lv_label_get_text(t), title) == 0) {
            return r;
        }
    }
    return NULL;
}

static bool tap(const char *title)
{
    lv_obj_t *r = row(title);
    if (r != NULL) {
        lv_obj_send_event(r, LV_EVENT_CLICKED, NULL);
        ui_run(50);
    }
    return r != NULL;
}

static size_t stored(auto_rule_t *out)
{
    return B.auto_list(B.ctx, out, AUTO_MAX);
}

/* AUT-001: a new automation through its steps, stored once at the last pick. */
static void test_ui_add(void)
{
    auto_rule_t got[AUTO_MAX];
    const size_t before = stored(got);
    ui_auto_add_start();
    ui_run(50);
    check(sheet_kind() == SHEET_PICK && strcmp(sheet_title(), "Add an automation") == 0,
          "AUT-001: Add opens the ideas");
    check(row("Turn on a light when a door opens") != NULL && row("Lock up when you arm Away") != NULL &&
              row("Close the water when a leak is found") != NULL,
          "AUT-001: the ideas whose parts the house has");
    check(tap("Start from scratch") && strcmp(sheet_title(), "When should it run?") == 0,
          "AUT-001: from scratch asks when");
    check(tap("When a door or window opens") && strcmp(sheet_title(), "Which sensor?") == 0,
          "AUT-004: opens asks which sensor");
    check(row("Front Door") != NULL && row("Kitchen Smoke") == NULL && row("Hallway Motion") != NULL,
          "AUT-004: only zones that may start one are listed");
    check(stored(got) == before, "AUT-001: nothing stored before the last pick");
    check(tap("Front Door") && strcmp(sheet_title(), "What should it do?") == 0,
          "AUT-001: then what it should do");
    check(row("Movie Night") == NULL, "AUT-002: a script is never offered");
    check(tap("Patio Lights") && strcmp(sheet_title(), "What should Patio Lights do?") == 0 &&
              row("Turn on") != NULL && row("Toggle") != NULL && row("Unlock") == NULL,
          "AUT-002: a light offers its own actions");
    check(stored(got) == before, "AUT-001: still nothing stored");
    check(tap("Turn on") && sheet_kind() == SHEET_NONE, "AUT-001: the last pick closes the sheets");
    const size_t n = stored(got);
    check(n == before + 1u && got[n - 1u].when.kind == AUTO_TRIG_ZONE_OPENED &&
              got[n - 1u].when.zone == 1u && got[n - 1u].n == 1u &&
              strcmp(got[n - 1u].then[0].ref, "light.patio") == 0 &&
              got[n - 1u].then[0].dev_action == ACT_ON,
          "AUT-001: stored as picked");
}

/* AUT-001: Cancel at any step leaves nothing. */
static void test_ui_cancel(void)
{
    auto_rule_t got[AUTO_MAX];
    const size_t before = stored(got);
    const uint16_t gen = B.auto_gen(B.ctx);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is disarmed") &&
              strcmp(sheet_title(), "What should it do?") == 0,
          "AUT-001: one partition: no partition sheet");
    ui_sheet_cancel();
    ui_run(50);
    check(sheet_kind() == SHEET_NONE && stored(got) == before && B.auto_gen(B.ctx) == gen,
          "AUT-001: Cancel on the last step stores nothing");
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("At a time of day") && sheet_kind() == SHEET_TIME,
          "AUT-005: a time of day opens the time sheet");
    lv_obj_t *sheet = ui_find(lv_screen_active(), "time_sheet");
    static const char *const DAYS[7] = {"day_1", "day_2", "day_3", "day_4", "day_5", "day_6", "day_7"};
    for (int d = 0; d < 7; d++) {
        lv_obj_t *t = ui_find(sheet, DAYS[d]);
        if (t != NULL && lv_obj_has_state(t, LV_STATE_CHECKED)) {
            lv_obj_remove_state(t, LV_STATE_CHECKED);
            lv_obj_send_event(t, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }
    ui_run(50);
    lv_obj_t *ok = ui_find(sheet, "time_ok");
    lv_obj_t *line = ui_find(sheet, "time_result");
    check(ok != NULL && lv_obj_has_state(ok, LV_STATE_DISABLED) && line != NULL &&
              strcmp(lv_label_get_text(line), "Pick at least one day") == 0,
          "AUT-005: no day picked: OK is dimmed and the line says why");
    ui_sheet_done();
    ui_run(50);
    check(sheet_kind() == SHEET_TIME, "AUT-005: OK does nothing without a day");
    ui_sheet_cancel();
    ui_run(50);
    check(sheet_kind() == SHEET_NONE && stored(got) == before && B.auto_gen(B.ctx) == gen,
          "AUT-001: Cancel on the time sheet stores nothing");
}

/* AUT-016: an unlock after a door asks first; refused, nothing is stored. */
static void test_ui_unlock_confirm(void)
{
    auto_rule_t got[AUTO_MAX];
    const size_t before = stored(got);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When a door or window opens") && tap("Front Door") &&
              tap("Back Door") && strcmp(sheet_title(), "What should Back Door do?") == 0 &&
              row("Lock") != NULL && row("Unlock") != NULL,
          "AUT-015: with the option on, a lock may lock or unlock");
    check(tap("Unlock") && sheet_kind() == SHEET_CONFIRM &&
              strcmp(sheet_title(), "Unlock Back Door when Front Door opens?") == 0 &&
              strcmp(sheet_note(),
                     "Anyone who opens Front Door will unlock Back Door, even when the alarm is on.") == 0,
          "AUT-016: an unlock after a door asks the plain confirm with its sentence");
    ui_sheet_cancel();
    ui_run(50);
    check(sheet_kind() == SHEET_NONE && stored(got) == before,
          "AUT-016: an unlock after a door without the confirm isn't stored");

    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is disarmed") && tap("Back Door") &&
              tap("Unlock") && sheet_kind() == SHEET_NONE,
          "AUT-016: after a disarm an unlock asks nothing (D24)");
    size_t n = stored(got);
    check(n == before + 1u && got[n - 1u].then[0].dev_action == ACT_UNLOCK &&
              got[n - 1u].then[0].confirmed == 0u,
          "AUT-016: stored without a confirm");

    const size_t mid = n;
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is armed") && tap("Away") &&
              tap("Back Door") && tap("Unlock") && sheet_kind() == SHEET_CONFIRM &&
              strcmp(sheet_title(), "Unlock Back Door when the panel is armed?") == 0,
          "AUT-016: an unlock after an armed trigger asks too (decided 2026-10-05)");
    ui_sheet_done();
    ui_run(50);
    check(sheet_kind() == SHEET_NONE, "AUT-016: Use it closes the confirm");
    n = stored(got);
    check(n == mid + 1u && got[n - 1u].when.kind == AUTO_TRIG_ARMED &&
              got[n - 1u].when.levels == ALARM_LEVEL_BIT(ALARM_LEVEL_AWAY) &&
              got[n - 1u].then[0].confirmed == 1u,
          "AUT-016: stored with confirmed");

    /* D8: a scene asks once when picked */
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is disarmed") && tap("Good Night") &&
              sheet_kind() == SHEET_CONFIRM &&
              strcmp(sheet_title(), "Use Good Night in an automation?") == 0,
          "AUT-002: a scene goes straight to its confirm (D8)");
    ui_sheet_cancel();
    ui_run(50);
}

/* AUT-015: with the option off, no Unlock, Open or Stop; the then-sheet says why. */
static void test_ui_option_off(void)
{
    auto_rule_t got[AUTO_MAX];
    host_panel_auto_less_secure(false);
    const size_t before = stored(got);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is disarmed") &&
              strcmp(sheet_note(), "Your installer has turned off automations that unlock, open or disarm.") == 0,
          "AUT-015: the option off: the then-sheet's note");
    check(row("Back Door") != NULL && row("Garage Door") != NULL,
          "AUT-015: the devices stay; only their actions change");
    check(tap("Back Door") && sheet_kind() == SHEET_NONE && row("Unlock") == NULL,
          "AUT-015: a lock can only lock: its sheet is skipped, no Unlock");
    const size_t n = stored(got);
    check(n == before + 1u && got[n - 1u].then[0].dev_action == ACT_LOCK,
          "AUT-015: stored as Lock");
    host_panel_auto_less_secure(true);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is disarmed") && sheet_note()[0] == '\0' &&
              tap("Garage Door") && row("Close") != NULL && row("Open") != NULL && row("Stop") != NULL,
          "AUT-015: the option on: a garage door may close, open or stop, and no note");
    ui_sheet_cancel();
    ui_run(50);
}

/* U16: the editors, in the session test_ui left disarmed, the section opened again with the
 * master code (D3). */
static void test_ui_editors(void)
{
    open_automations();
    check(strstr(page_note(), " of 16 automations") != NULL, "AUT-010: the section is open again");
    test_ui_add();
    test_ui_cancel();
    test_ui_unlock_confirm();
    test_ui_option_off();
}

/* ---- phase C: arming by automation (BL-170 C4) ---- */

#define MIN_MS (60u * 1000u)

/* "At 7:10 PM every day: Arm Night, partition 1, 5 minutes' warning", saved by the system
 * master (who it then arms as, D23). */
static uint16_t save_night_arm(uint8_t bypass_open)
{
    auto_rule_t r;
    memset(&r, 0, sizeof(r));
    r.when.kind = AUTO_TRIG_TIME;
    r.when.minute = 19u * 60u + 10u;
    r.n = 1;
    r.then[0].kind = AUTO_ACT_ARM;
    r.then[0].partition = 1;
    r.then[0].arm_level = ALARM_LEVEL_NIGHT;
    r.then[0].warn_min = 5;
    r.then[0].bypass_open = bypass_open;
    r.then[0].value = -1;
    auto_check_t why = AUTO_OK;
    uint16_t uid = 0;
    const auto_save_t res = B.auto_save(B.ctx, &r, ALARM_USER_MASTER, B.auto_gen(B.ctx), &why, &uid);
    check(res == AUTO_SAVE_OK && uid != 0u, "AUT-011: a time-of-day Night arm is saved");
    if (res != AUTO_SAVE_OK) {
        printf("  (save %d, why %d)\n", (int)res, (int)why);
    }
    return uid;
}

static bool warning_of(uint16_t uid, auto_warning_t *out)
{
    auto_warning_t w[AUTO_MAX];
    const size_t n = B.auto_warnings(B.ctx, w, AUTO_MAX);
    for (size_t i = 0; i < n; i++) {
        if (w[i].uid == uid) {
            *out = w[i];
            return true;
        }
    }
    memset(out, 0, sizeof(*out));
    return false;
}

static alarm_arm_state_t part1_state(void)
{
    alarm_snapshot_t snap;
    B.get_snapshot(B.ctx, &snap);
    return snap.part[0].state;
}

/* AUT-011, AUT-012: the warning starts warn_min minutes before the time, and it arms on it. */
static void test_arm_on_time(void)
{
    start(AT_7PM_MS); /* 7:00:01 PM */
    check(B.auto_warnings != NULL && B.auto_put_off != NULL,
          "the simulator fills the phase C hooks");
    const uint16_t uid = save_night_arm(0);
    auto_warning_t w;
    host_panel_advance(4u * MIN_MS + 49000u); /* 7:04:50 */
    check(!warning_of(uid, &w), "AUT-012: no warning before 7:05");
    host_panel_advance(20000u); /* 7:05:10 */
    check(warning_of(uid, &w) && w.state == AUTO_WARN_ON && w.partition == 1u &&
              w.level == ALARM_LEVEL_NIGHT && w.warn_min == 5u && w.put_off_by == 0u,
          "AUT-012, D19: the warning is ON from 7:05, Night on partition 1");
    check(w.arm_at_epoch_ms >= AT_7PM_MS + 10 * (int64_t)MIN_MS &&
              w.arm_at_epoch_ms <= AT_7PM_MS + 10 * (int64_t)MIN_MS + 2000,
          "AUT-012: it says it arms at 7:10");
    check(part1_state() == ALARM_STATE_DISARMED, "AUT-012: nothing armed during the warning");
    host_panel_advance(4u * MIN_MS + 40000u); /* 7:09:50 */
    check(part1_state() == ALARM_STATE_DISARMED, "AUT-012: still disarmed at 7:09:50");
    host_panel_advance(20000u); /* 7:10:10 */
    check(part1_state() != ALARM_STATE_DISARMED, "AUT-011: armed on the time, as the master");
    check(result(uid) == AUTO_RES_DONE, "AUT-011: the status is done");
    check(!warning_of(uid, &w), "AUT-012: the warning is gone once it armed");
}

/* The newest 64 log entries hold an engine event of this type. */
/* The newest put-off event's choice (ALARM_PUT_OFF_*), 0 if none is in the last 64. */
static unsigned put_off_logged(void)
{
    elog_entry_t e[64];
    const size_t n = B.log_read(B.ctx, UINT32_MAX, NULL, NULL, e, 64u, 0u, NULL);
    unsigned choice = 0u;
    uint32_t seq = 0u;
    for (size_t i = 0; i < n; i++) {
        if (e[i].kind == ELOG_KIND_EVENT && e[i].ev.type == ALARM_EV_ARMING_PUT_OFF &&
            (choice == 0u || e[i].ev.seq > seq)) {
            choice = e[i].ev.arg;
            seq = e[i].ev.seq;
        }
    }
    return choice;
}

static bool logged(alarm_event_type_t type)
{
    elog_entry_t e[64];
    const size_t n = B.log_read(B.ctx, UINT32_MAX, NULL, NULL, e, 64u, 0u, NULL);
    for (size_t i = 0; i < n; i++) {
        if (e[i].kind == ELOG_KIND_EVENT && e[i].ev.type == type) {
            return true;
        }
    }
    return false;
}

/* AUT-011, DESIGN.md "Arming" 5: installer mode at the arm time refuses the arming - reported as a
 * refusal, never "armed already" over a disarmed house (the C4 review). */
static void test_arm_in_installer_mode(void)
{
    start(AT_7PM_MS);
    const uint16_t uid = save_night_arm(0);
    host_panel_advance(5u * MIN_MS + 10000u); /* 7:05:11, the warning on */
    check(host_panel_keypad2_program(true) == ALARM_OK, "installer mode at the second keypad");
    host_panel_advance(5u * MIN_MS); /* 7:10:11 */
    check(part1_state() == ALARM_STATE_DISARMED, "AUT-011: not armed in installer mode");
    check(result(uid) == AUTO_RES_ARM_REFUSED, "AUT-011: refused, not \"armed already\"");
    check(host_panel_keypad2_program(false) == ALARM_OK, "installer mode ended");
}

/* AUT-013, D26: an hour with the master code; the warning comes back 5 minutes before 8:10. */
static void test_put_off_hour(void)
{
    start(AT_7PM_MS);
    const uint16_t uid = save_night_arm(0);
    alarm_result_t cr = ALARM_ERR_BAD_ARG;
    check(B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_HOUR, "123456", &cr) == AUTO_POFF_NO_WARNING,
          "AUT-013: nothing to put off before the warning");
    host_panel_advance(5u * MIN_MS + 30000u); /* 7:05:31 */
    check(B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_HOUR, "123456", &cr) == AUTO_POFF_OK &&
              cr == ALARM_OK,
          "AUT-013: put off an hour with the master code");
    auto_warning_t w;
    check(warning_of(uid, &w) && w.state == AUTO_WARN_HOUR && w.put_off_by == ALARM_USER_MASTER,
          "D26: held an hour, put off by the master");
    check(put_off_logged() == ALARM_PUT_OFF_HOUR, "AUT-013, D30: the put-off is logged as an event");
    check(B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_HOUR, "123456", &cr) == AUTO_POFF_NO_WARNING,
          "AUT-013: a hold isn't put off again (only a warning is)");
    host_panel_advance(5u * MIN_MS); /* 7:10:31 */
    check(part1_state() == ALARM_STATE_DISARMED, "AUT-013: not armed at 7:10");
    auto_status_t st;
    host_panel_auto_status(uid, &st);
    check(st.result == AUTO_RES_PUT_OFF && st.user == ALARM_USER_MASTER &&
              st.choice == AUTO_PUT_OFF_HOUR,
          "AUT-013: the status says put off an hour, by the master");
    host_panel_advance(55u * MIN_MS); /* 8:05:31 */
    check(warning_of(uid, &w) && w.state == AUTO_WARN_ON, "D26: the warning again at 8:05");
    host_panel_advance(5u * MIN_MS); /* 8:10:31 */
    check(part1_state() != ALARM_STATE_DISARMED, "D26: armed an hour later");
}

/* AUT-013 (decided 2026-10-05): "Not tonight" needs a code that may disarm the partition;
 * any code gets the hour, and a duress code works as any other (it reports duress silently). */
static void test_put_off_codes(void)
{
    start(AT_7PM_MS);
    check(B.user_set(B.ctx, "123456", 5, "5555", ALARM_AUTH_ARM_ONLY, 0x01u, "Sam") == ALARM_OK,
          "an arm-only user Sam");
    check(B.user_set(B.ctx, "123456", 6, "6666", ALARM_AUTH_DURESS, 0x01u, "Pat") == ALARM_OK,
          "a duress code");
    const uint16_t uid = save_night_arm(0);
    host_panel_advance(5u * MIN_MS + 30000u); /* 7:05:31 */
    alarm_result_t cr = ALARM_ERR_BAD_ARG;
    check(B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_NIGHT, "5555", &cr) == AUTO_POFF_NOT_ALLOWED &&
              cr == ALARM_OK,
          "AUT-013: Not tonight refused for a code that may not disarm");
    auto_warning_t w;
    check(warning_of(uid, &w) && w.state == AUTO_WARN_ON, "AUT-013: the warning goes on");
    check(B.auto_put_off(B.ctx, uid, 7u, "123456", &cr) == AUTO_POFF_BAD_ARG,
          "an unknown choice is refused before any code check");
    check(B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_HOUR, "6666", &cr) == AUTO_POFF_OK && cr == ALARM_OK,
          "section 8: a duress code puts off as any code (nothing at the wall gives it away)");
    check(warning_of(uid, &w) && w.state == AUTO_WARN_HOUR && w.put_off_by == 6u,
          "AUT-013: put off by the duress code's user");
    check(logged(ALARM_EV_DURESS), "section 8: the duress put-off reported duress silently");
    host_panel_advance(60u * MIN_MS); /* 8:05:31: warning again */
    check(B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_HOUR, "5555", &cr) == AUTO_POFF_OK,
          "section 8: anyone at the wall puts off for an hour, an arm-only code too");
    host_panel_advance(60u * MIN_MS); /* 9:05:31 */
    check(B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_NIGHT, "123456", &cr) == AUTO_POFF_OK,
          "D26: Not tonight with the master code");
    check(warning_of(uid, &w) && w.state == AUTO_WARN_NIGHT, "D26: held until 6:00 AM");
    check(put_off_logged() == ALARM_PUT_OFF_NIGHT, "AUT-013, D30: logged as Not tonight");
}

/* D26: an arm time in the day (12:10 PM) is held until midnight, and logged as Not today. */
static void test_put_off_today(void)
{
    start(AT_NOON_MS);
    auto_rule_t r;
    memset(&r, 0, sizeof(r));
    r.when.kind = AUTO_TRIG_TIME;
    r.when.minute = 12u * 60u + 10u;
    r.n = 1;
    r.then[0].kind = AUTO_ACT_ARM;
    r.then[0].partition = 1;
    r.then[0].arm_level = ALARM_LEVEL_AWAY;
    r.then[0].warn_min = 5;
    r.then[0].value = -1;
    auto_check_t why = AUTO_OK;
    uint16_t uid = 0;
    check(B.auto_save(B.ctx, &r, ALARM_USER_MASTER, B.auto_gen(B.ctx), &why, &uid) ==
                  AUTO_SAVE_OK && uid != 0u,
          "AUT-011: a 12:10 PM Away arm is saved");
    host_panel_advance(5u * MIN_MS + 30000u); /* 12:05:31 */
    alarm_result_t cr = ALARM_ERR_BAD_ARG;
    check(B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_NIGHT, "123456", &cr) == AUTO_POFF_OK &&
              cr == ALARM_OK,
          "D26: Not today with the master code");
    auto_warning_t w;
    check(warning_of(uid, &w) && w.state == AUTO_WARN_DAY, "D26: held until midnight");
    check(put_off_logged() == ALARM_PUT_OFF_DAY, "AUT-013, D30: logged as Not today");
    host_panel_advance(10u * MIN_MS); /* 12:15:31 */
    check(part1_state() == ALARM_STATE_DISARMED, "D26: not armed at 12:10");
}

/* CORE-USR-034: a wrong code at the put-off pad counts toward the keypad's lockout. */
static void test_put_off_lockout(void)
{
    start(AT_7PM_MS);
    const uint16_t uid = save_night_arm(0);
    host_panel_advance(5u * MIN_MS + 30000u);
    alarm_result_t cr = ALARM_OK;
    check(B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_HOUR, "999999", &cr) == UI_AUTO_POFF_CODE &&
              cr == ALARM_ERR_INVALID_CODE,
          "CORE-USR-034: a wrong code is refused and says so");
    int tries = 1;
    while (cr != ALARM_ERR_LOCKED_OUT && tries < 12) {
        (void)B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_HOUR, "999999", &cr);
        tries++;
    }
    check(cr == ALARM_ERR_LOCKED_OUT, "CORE-USR-034: wrong codes lock the keypad out");
    check(B.auto_put_off(B.ctx, uid, AUTO_PUT_OFF_HOUR, "123456", &cr) == UI_AUTO_POFF_CODE &&
              cr == ALARM_ERR_LOCKED_OUT,
          "CORE-USR-034: locked out, even the right code is refused");
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ALARM_CMD_ARM;
    cmd.partition = 1;
    cmd.level = ALARM_LEVEL_STAY;
    snprintf(cmd.code, sizeof(cmd.code), "123456");
    check(B.command(B.ctx, &cmd, NULL) == ALARM_ERR_LOCKED_OUT,
          "CORE-USR-034: the same lockout as the keypad's (an arm refused too)");
    auto_warning_t w;
    check(warning_of(uid, &w) && w.state == AUTO_WARN_ON, "the warning goes on, not put off");
}

/* AUT-014, D29: "Bypass open zones" stored, the installer option turned off: it arms without
 * bypassing and fails on the open zone, naming it - never "can't run". With it on, it bypasses. */
static void test_bypass_and_the_option(void)
{
    start(AT_7PM_MS);
    const uint16_t uid = save_night_arm(1);
    host_panel_auto_less_secure(false);
    host_panel_zone(3, ALARM_ZEV_FAULT); /* a window left open */
    host_panel_advance(5u * MIN_MS + 30000u);
    auto_warning_t w;
    check(warning_of(uid, &w) && w.state == AUTO_WARN_ON && w.bypass == 0u,
          "D28: the warning says open zones won't be bypassed while the option is off");
    host_panel_advance(5u * MIN_MS);
    auto_status_t st;
    host_panel_auto_status(uid, &st);
    check(part1_state() == ALARM_STATE_DISARMED, "AUT-014: not armed with a zone open");
    check(st.result == AUTO_RES_NOT_READY && st.zone == 3u,
          "AUT-014: NOT_READY, naming the open zone (not ARM_REFUSED)");

    start(AT_7PM_MS);
    const uint16_t uid2 = save_night_arm(1);
    host_panel_zone(3, ALARM_ZEV_FAULT);
    host_panel_advance(5u * MIN_MS + 30000u);
    check(warning_of(uid2, &w) && w.bypass == 1u, "D28: with the option on it will bypass");
    host_panel_advance(5u * MIN_MS);
    check(part1_state() != ALARM_STATE_DISARMED && result(uid2) == AUTO_RES_DONE,
          "D29: armed, the open zone bypassed");
}

/* ---- U16's review fixes and phase C's Disarm, through the editors (BL-170 C6) ------------- */

/* The newest stored automation (the last in the list), or NULL. */
static const auto_rule_t *newest(auto_rule_t got[AUTO_MAX])
{
    const size_t n = stored(got);
    return n > 0u ? &got[n - 1u] : NULL;
}

static bool sheet_is(const char *title, const char *note)
{
    return strcmp(sheet_title(), title) == 0 && (note == NULL || strcmp(sheet_note(), note) == 0);
}

/* The list again, drawn from the store (the editors' saves send the generation it was built
 * from, AUT-008). */
static void list_again(void)
{
    ui_run(1100);
}

/* AUT-016 (BL-170 C6, U16 review a): a stop after an armed trigger asks its whole sentence; it
 * was skipped, and the save then dead-ended NEEDS_CONFIRM. */
static void test_ui_armed_stop(void)
{
    auto_rule_t got[AUTO_MAX];
    const size_t before = stored(got);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is armed") && tap("Away") &&
              tap("Garage Door") && tap("Stop") && sheet_kind() == SHEET_CONFIRM &&
              sheet_is("Stop Garage Door when the panel is armed?",
                       "Every time the panel is armed Away, Garage Door will stop."),
          "AUT-016: armed Away, then Stop Garage Door asks the confirm with its whole sentence");
    ui_sheet_cancel();
    ui_run(50);
    check(stored(got) == before, "AUT-016: cancelled, nothing stored");
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is armed") && tap("Away") &&
              tap("Garage Door") && tap("Open") && sheet_kind() == SHEET_CONFIRM &&
              sheet_is("Open Garage Door when the panel is armed?",
                       "Every time the panel is armed Away, Garage Door will open."),
          "AUT-016: armed Away, then Open Garage Door: the owner's sentence");
    ui_sheet_done();
    ui_run(50);
    const auto_rule_t *r = newest(got);
    check(sheet_kind() == SHEET_NONE && stored(got) == before + 1u && r != NULL &&
              r->then[0].dev_action == ACT_OPEN && r->then[0].confirmed == 1u,
          "AUT-016: Use it stores it confirmed");
    if (r != NULL) {
        check(B.auto_delete_slot(B.ctx, r->slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "tidied away");
    }
    list_again();
}

/* AUT-011, AUT-015, AUT-016, D22, D23, D24: "Disarm the panel" in "What should it do?". */
static void test_ui_disarm(void)
{
    auto_rule_t got[AUTO_MAX];
    size_t before = stored(got);

    /* D23, D24: after a door, Disarm asks the plain confirm (one partition: "the panel") */
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When a door or window opens") && tap("Front Door") &&
              sheet_is("What should it do?", NULL) && row("Disarm the panel") != NULL,
          "D23: after a door opens, \"The alarm\" offers Disarm");
    check(tap("Disarm the panel") && sheet_kind() == SHEET_CONFIRM &&
              sheet_is("Disarm the panel when Front Door opens?", "Anyone who opens Front Door will disarm the panel."),
          "AUT-016, D24: Disarm after a door asks the plain confirm, whole sentences");
    ui_sheet_cancel();
    ui_run(50);
    check(sheet_kind() == SHEET_NONE && stored(got) == before, "AUT-016: cancelled, nothing stored");

    /* D22: never after an alarm */
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When an alarm sounds") && sheet_is("What should it do?", NULL) &&
              row("Disarm the panel") == NULL && row("Porch Light") != NULL,
          "D22, AUT-011: no Disarm after an alarm trigger");
    ui_sheet_cancel();
    ui_run(50);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When a fire or CO alarm sounds") && row("Disarm the panel") == NULL,
          "D22: no Disarm after a fire or CO alarm");
    ui_sheet_cancel();
    ui_run(50);

    /* AUT-016: never after an armed trigger of its own partition (it would undo the arming) */
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is armed") && tap("Night") &&
              sheet_is("What should it do?", NULL) && row("Disarm the panel") == NULL,
          "AUT-016: no Disarm after the panel's own armed trigger");
    ui_sheet_cancel();
    ui_run(50);

    /* AUT-015: the option off, no Disarm and the note says why */
    host_panel_auto_less_secure(false);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is disarmed") &&
              sheet_is("What should it do?", "Your installer has turned off automations that unlock, open or disarm.") &&
              row("Disarm the panel") == NULL,
          "AUT-015: the option off: no Disarm, the note says why");
    ui_sheet_cancel();
    ui_run(50);
    host_panel_auto_less_secure(true);
    list_again();

    /* D24: at a time of day Disarm asks nothing; stored for partition 1 as the master */
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("At a time of day") && sheet_kind() == SHEET_TIME, "the time sheet");
    ui_sheet_done(); /* 11:00 PM on weekdays */
    ui_run(50);
    check(sheet_is("What should it do?", NULL) && tap("Disarm the panel") && sheet_kind() == SHEET_NONE,
          "D24: a time-of-day Disarm asks no confirm");
    const auto_rule_t *r = newest(got);
    check(stored(got) == before + 1u && r != NULL && r->when.kind == AUTO_TRIG_TIME && r->n == 1u &&
              r->then[0].kind == AUTO_ACT_DISARM && r->then[0].partition == 1u && r->then[0].confirmed == 0u &&
              r->made_by == ALARM_USER_MASTER,
          "AUT-011: stored as picked: Disarm partition 1, made by the master");
    const uint8_t slot = r != NULL ? r->slot : 0u;

    /* Its page: "Disarm the panel", who set it up, the nudge, a second action but no Try it now */
    ui_page_open(PAGE_AUTOMATION, slot);
    ui_run(300);
    lv_obj_t *body = named(lv_screen_active(), "page_body");
    check(label(body, "Disarm the panel") != NULL && label(body, "Set up by Master") != NULL,
          "D30: the Then row says Disarm and who set it up");
    check(label(body, "Nothing arms it again. Add one that arms in the evening.") != NULL,
          "DESIGN Disarming 5: a time-of-day Disarm with nothing arming again says so");
    check(label(body, "Add a second action") != NULL && label(body, "Try it now") == NULL,
          "D23: a Disarm may take one device action; no Try it now");

    /* D23: then one device action after it; "The alarm" isn't offered again */
    ui_auto_add_action(slot);
    ui_run(50);
    check(sheet_is("What should it do?", NULL) && row("Disarm the panel") == NULL && tap("Back Door") &&
              tap("Unlock") && sheet_kind() == SHEET_NONE,
          "D23: Disarm, then Unlock Back Door (a time of day asks nothing)");
    r = newest(got);
    check(r != NULL && r->slot == slot && r->n == 2u && r->then[0].kind == AUTO_ACT_DISARM &&
              r->then[1].dev_action == ACT_UNLOCK,
          "D23: stored with its device action");

    /* AUT-016: the trigger changed to a door: each less secure action asks again, in order */
    ui_auto_edit_when(slot);
    ui_run(50);
    check(tap("When a door or window opens") && tap("Front Door") && sheet_kind() == SHEET_CONFIRM &&
              sheet_is("Disarm the panel when Front Door opens?", NULL),
          "AUT-016: a new trigger asks the Disarm's confirm again");
    ui_sheet_done();
    ui_run(50);
    check(sheet_kind() == SHEET_CONFIRM && sheet_is("Unlock Back Door when Front Door opens?", NULL),
          "AUT-016: then the unlock's");
    ui_sheet_done();
    ui_run(50);
    r = newest(got);
    check(sheet_kind() == SHEET_NONE && r != NULL && r->when.kind == AUTO_TRIG_ZONE_OPENED &&
              r->then[0].confirmed == 1u && r->then[1].confirmed == 1u,
          "AUT-016: stored with both confirms");

    /* D22: changing a Disarm's trigger, the alarm rows can't be picked */
    ui_auto_edit_when(slot);
    ui_run(50);
    lv_obj_t *alarm = row("When an alarm sounds");
    check(alarm != NULL && lv_obj_has_state(alarm, LV_STATE_DISABLED), "D22: no alarm trigger for a Disarm");
    ui_sheet_cancel();
    ui_run(50);
    ui_page_back();
    ui_run(300);
    check(B.auto_delete_slot(B.ctx, slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "tidied away");
    list_again();
}

/* AUT-011, AUT-016: with two partitions, which one; an armed trigger rules its own one out and
 * asks the confirm for the other (decided 2026-10-05). */
static void test_ui_disarm_two(void)
{
    alarm_config_t cfg;
    check(B.get_config(B.ctx, &cfg), "the panel's settings");
    const uint8_t had = cfg.users[ALARM_USER_MASTER - 1u].partitions;
    cfg.partitions[1].enabled = true;
    snprintf(cfg.partitions[1].name, ALARM_NAME_LEN, "Studio");
    cfg.users[ALARM_USER_MASTER - 1u].partitions = (uint8_t)(had | 0x03u); /* the master has both */
    check(B.set_config(B.ctx, &cfg) == ALARM_OK, "a second partition, Studio");
    list_again();
    auto_rule_t got[AUTO_MAX];
    const size_t before = stored(got);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When a door or window opens") && tap("Front Door") &&
              tap("Disarm the panel") && sheet_is("Which partition?", NULL) && row("Home") != NULL &&
              row("Studio") != NULL && row("Any of them") == NULL,
          "AUT-011: two partitions: which one, never any of them");
    check(tap("Home") && sheet_kind() == SHEET_CONFIRM &&
              sheet_is("Disarm Home when Front Door opens?", "Anyone who opens Front Door will disarm Home."),
          "AUT-016: the confirm names the partition (automation-disarm-confirm)");
    ui_sheet_cancel();
    ui_run(50);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is armed") && tap("Away") && tap("Home") &&
              tap("Disarm the panel") && sheet_is("Which partition?", NULL) && row("Home") == NULL &&
              row("Studio") != NULL,
          "AUT-016: after Home is armed, Disarm offers only Studio");
    check(tap("Studio") && sheet_kind() == SHEET_CONFIRM &&
              sheet_is("Disarm Studio when Home is armed?", "Every time Home is armed Away, it will disarm Studio."),
          "decided 2026-10-05: a disarm after an armed trigger asks the plain confirm");
    ui_sheet_done();
    ui_run(50);
    const auto_rule_t *r = newest(got);
    check(stored(got) == before + 1u && r != NULL && r->then[0].kind == AUTO_ACT_DISARM && r->then[0].partition == 2u &&
              r->then[0].confirmed == 1u,
          "AUT-011: stored: Disarm Studio, confirmed");
    if (r != NULL) {
        check(B.auto_delete_slot(B.ctx, r->slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "tidied away");
    }
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is armed") && tap("Away") && tap("Any of them") &&
              row("Disarm the panel") == NULL,
          "AUT-016: after any partition is armed, no Disarm at all");
    ui_sheet_cancel();
    ui_run(50);
    cfg.partitions[1].enabled = false;
    cfg.users[ALARM_USER_MASTER - 1u].partitions = had;
    check(B.set_config(B.ctx, &cfg) == ALARM_OK, "one partition again");
    list_again();
}

/* U16 review b, d, e and AUT-008: the editors that change one automation. */
static void test_ui_change(void)
{
    auto_rule_t got[AUTO_MAX];
    /* "When the panel is disarmed: Turn off Patio Lights" */
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is disarmed") && tap("Patio Lights") && tap("Turn off") &&
              sheet_kind() == SHEET_NONE,
          "a light's Turn off, stored");
    const auto_rule_t *r = newest(got);
    const uint8_t slot = r != NULL ? r->slot : 0u;

    /* b: the light became a switch: the same Turn off again is stored with the new kind */
    dev("light.patio")->kind = DEV_SWITCH;
    ui_auto_edit_action(slot, 0);
    ui_run(50);
    check(sheet_is("What should Patio Lights do?", NULL) && tap("Turn off") && sheet_kind() == SHEET_NONE,
          "AUT-002: the same action picked again after the device changed kind");
    r = newest(got);
    check(r != NULL && r->slot == slot && r->then[0].dev_kind == DEV_SWITCH,
          "AUT-002 (U16 review b): stored with the device's new kind, so it can run");
    dev("light.patio")->kind = DEV_LIGHT;
    ui_auto_edit_action(slot, 0);
    ui_run(50);
    check(tap("Turn on") && sheet_kind() == SHEET_NONE, "Turn on again, as a light");

    /* e: "Turn it off after" and "Only between" */
    ui_auto_edit_off_after(slot, 0);
    ui_run(50);
    check(sheet_is("Turn it off after", NULL) && tap("5 minutes") && sheet_kind() == SHEET_NONE,
          "AUT-006: Turn it off after 5 minutes");
    r = newest(got);
    check(r != NULL && r->then[0].off_after_min == 5u, "AUT-006: stored");
    ui_auto_edit_window(slot);
    ui_run(50);
    check(sheet_kind() == SHEET_WINDOW, "AUT-004: Only between opens its sheet");
    ui_sheet_done(); /* 6:00 PM to 6:00 AM */
    ui_run(50);
    r = newest(got);
    check(r != NULL && r->when.win_from == 18u * 60u + 1u && r->when.win_to == 6u * 60u + 1u,
          "AUT-004: the window stored");

    /* e, AUT-016: an action changed to Unlock after a door asks again */
    ui_auto_edit_when(slot);
    ui_run(50);
    check(tap("When a door or window opens") && tap("Front Door") && sheet_kind() == SHEET_NONE,
          "a light after a door asks nothing");
    ui_auto_edit_action(slot, 0);
    ui_run(50);
    check(tap("Choose another device") && tap("Back Door") && tap("Unlock") && sheet_kind() == SHEET_CONFIRM &&
              sheet_is("Unlock Back Door when Front Door opens?", NULL),
          "AUT-016: a new action asks the confirm");
    ui_sheet_done();
    ui_run(50);
    ui_auto_edit_action(slot, 0);
    ui_run(50);
    check(tap("Lock") && sheet_kind() == SHEET_NONE, "Lock asks nothing");
    ui_auto_edit_action(slot, 0);
    ui_run(50);
    check(tap("Unlock") && sheet_kind() == SHEET_CONFIRM, "AUT-016: back to Unlock, the confirm is asked again");
    ui_sheet_cancel();
    ui_run(50);
    r = newest(got);
    check(r != NULL && r->then[0].dev_action == ACT_LOCK, "AUT-016: cancelled, the Lock stays");

    /* d: a device that left the panel opens the device picker (automation-missing) */
    auto_rule_t gone = *r;
    snprintf(gone.then[0].ref, sizeof(gone.then[0].ref), "lock.gone");
    snprintf(gone.then[0].name, sizeof(gone.then[0].name), "Old Lock");
    auto_check_t why = AUTO_OK;
    uint16_t uid = 0;
    check(host_panel_auto_save(&gone, ALARM_USER_MASTER, host_panel_auto_gen(), &why, &uid) == AUTO_SAVE_OK,
          "its lock left the panel");
    list_again();
    ui_auto_edit_action(slot, 0);
    ui_run(50);
    check(sheet_is("What should it do?", NULL) && row("Back Door") != NULL,
          "U16 review d: a missing device's action opens the device picker");
    ui_sheet_cancel();
    ui_run(50);

    /* c, AUT-008: a save from a page the store changed under is refused, back to the list */
    ui_page_open(PAGE_AUTOMATION, slot);
    ui_run(300);
    check(host_panel_auto_set_off(1, true, host_panel_auto_gen()) == AUTO_SAVE_OK, "changed elsewhere");
    ui_auto_edit_when(slot); /* before the page's tick has seen it */
    ui_run(50);
    check(tap("When the panel is disarmed") && sheet_kind() == SHEET_NONE &&
              toasted("Changed somewhere else meanwhile: here it is now"),
          "AUT-008: the stale save is refused CHANGED and says so");
    ui_run(300);
    check(strstr(page_note(), " of 16 automations") != NULL,
          "AUT-008 (U16 review c): the editor went back to the list");
    r = newest(got);
    check(r != NULL && r->when.kind == AUTO_TRIG_ZONE_OPENED, "AUT-008: nothing changed");
    check(host_panel_auto_set_off(1, false, host_panel_auto_gen()) == AUTO_SAVE_OK, "back on");
    list_again();

    /* e: delete */
    const size_t before = stored(got);
    ui_auto_delete_ask(slot);
    ui_run(50);
    check(sheet_kind() == SHEET_CONFIRM && sheet_is("Delete this automation?", "It stops at once. This can't be undone."),
          "AUT-010: delete asks first");
    ui_sheet_done();
    ui_run(50);
    check(stored(got) == before - 1u, "AUT-010: deleted");
}

/* BL-170, D30, AUT-012, AUT-014: an arming automation's page (its sheet: test_ui_arm_sheet):
 * "Arm Night", who set it up and its warning, no second action, no Try it now; its
 * switch says each outcome. */
static void test_ui_arm_page(void)
{
    const uint16_t uid = save_night_arm(0u);
    list_again();
    auto_rule_t got[AUTO_MAX];
    const auto_rule_t *r = newest(got);
    const uint8_t slot = r != NULL ? r->slot : 0u;
    ui_page_open(PAGE_AUTOMATION, slot);
    ui_run(300);
    lv_obj_t *body = named(lv_screen_active(), "page_body");
    check(label(body, "Arm Night") != NULL && label(body, "Set up by Master \xC2\xB7 warns 5 minutes before") != NULL,
          "D30: Arm Night, set up by its maker, its warning");
    check(label(body, "Add a second action") == NULL && label(body, "Try it now") == NULL,
          "AUT-011: an Arm is the only action; no Try it now");
    static const struct {
        uint8_t result, alarm_res, choice, night;
        uint16_t zone;
        const char *words;
        const char *what;
    } CASES[] = {
        {AUTO_RES_ALREADY, 0, 0, 0, 0, "On \xC2\xB7 already armed at 7:00 PM", "AUT-014: already armed"},
        {AUTO_RES_NOT_READY, 0, 0, 0, 1, "On \xC2\xB7 didn't arm at 7:00 PM: Front Door was open", "AUT-014: not ready"},
        {AUTO_RES_NO_WARNING, 0, 0, 0, 0, "On \xC2\xB7 didn't arm at 7:00 PM: no time to warn first", "AUT-012: no warning"},
        {AUTO_RES_ARM_REFUSED, ALARM_ERR_NOT_AUTHORIZED, 0, 0, 0, "Can't run: Master can no longer arm this partition",
         "AUT-011: the maker can't arm"},
        {AUTO_RES_PUT_OFF, 0, AUTO_PUT_OFF_NIGHT, 1, 0,
         "On \xC2\xB7 not tonight, put off by Master \xC2\xB7 arms again tomorrow at 7:10 PM", "AUT-013: not tonight"},
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        auto_status_t st;
        memset(&st, 0, sizeof(st));
        st.result = CASES[i].result;
        st.alarm_res = CASES[i].alarm_res;
        st.choice = CASES[i].choice;
        st.night = CASES[i].night;
        st.zone = CASES[i].zone;
        st.user = ALARM_USER_MASTER;
        st.at_epoch_ms = AT_7PM_MS;
        host_panel_auto_force_status(uid, &st);
        ui_run(1100);
        check(label(named(lv_screen_active(), "page_body"), CASES[i].words) != NULL, CASES[i].what);
    }
    host_panel_auto_force_status(uid, NULL);
    ui_page_back();
    ui_run(300);
    check(B.auto_delete_slot(B.ctx, slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "tidied away");
    list_again();
}

/* ---- C6t2: "Arm the panel", driven as a finger would (ui_auto_edit.c arm_sheet) ------------ */

static lv_obj_t *form_body(void)
{
    return sheet_kind() == SHEET_FORM ? named(lv_screen_active(), "form_body") : NULL;
}

/* A key or a part of the arm sheet, by its name. */
static lv_obj_t *form_part(const char *name)
{
    lv_obj_t *body = form_body();
    return body != NULL ? lv_obj_find_by_name(body, name) : NULL;
}

static bool level_is(const char *key)
{
    static const char *const KEYS[3] = {"level_away", "level_stay", "level_night"};
    bool ok = true;
    for (size_t i = 0; i < 3u; i++) {
        lv_obj_t *k = form_part(KEYS[i]);
        ok = ok && k != NULL && lv_obj_has_state(k, LV_STATE_CHECKED) == (strcmp(KEYS[i], key) == 0);
    }
    return ok;
}

static const char *result_words(void)
{
    lv_obj_t *l = form_part("result_label");
    return l != NULL ? lv_label_get_text(l) : "";
}

static bool result_is(const char *text)
{
    const bool ok = strcmp(result_words(), text) == 0;
    if (!ok) {
        printf("  (result: \"%s\")\n", result_words());
    }
    return ok;
}

/* The words of the warning's value ("5 minutes"). */
static const char *warn_words(void)
{
    lv_obj_t *l = form_part("value_label");
    return l != NULL ? lv_label_get_text(l) : "";
}

static void form_press(const char *name)
{
    press(form_part(name));
}

/* The bypass switch, as a finger would. */
static void bypass_press(void)
{
    lv_obj_t *t = label(form_body(), "Bypass open zones");
    while (t != NULL && lv_obj_find_by_name(t, "toggle") == NULL) {
        t = lv_obj_get_parent(t);
    }
    press(t != NULL ? lv_obj_find_by_name(t, "toggle") : NULL);
}

static void form_done(void)
{
    press(named(lv_screen_active(), "form_ok"));
}

/* The page's row for this title, and whether it can be tapped. */
static lv_obj_t *page_row(const char *title)
{
    lv_obj_t *o = label(named(lv_screen_active(), "page_body"), title);
    while (o != NULL && lv_obj_get_parent(o) != named(lv_screen_active(), "page_body")) {
        o = lv_obj_get_parent(o);
    }
    return o;
}

/* The arm sheet's partition row (its clickable row). */
static lv_obj_t *partition_row(void)
{
    lv_obj_t *p = label(form_body(), "Partition");
    while (p != NULL && !lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) {
        p = lv_obj_get_parent(p);
    }
    return p;
}

static bool second_partition(bool on, uint8_t master_parts)
{
    alarm_config_t cfg;
    if (!B.get_config(B.ctx, &cfg)) {
        return false;
    }
    cfg.partitions[1].enabled = on;
    snprintf(cfg.partitions[1].name, ALARM_NAME_LEN, "Studio");
    cfg.users[ALARM_USER_MASTER - 1u].partitions = master_parts;
    const bool ok = B.set_config(B.ctx, &cfg) == ALARM_OK;
    list_again();
    return ok;
}

/* AUT-011, D4, D22, D23: "What should it do?" (drawings automation-then-arm, -alarm,
 * -option-off): "The alarm" first, Arm after any trigger and whatever the option. */
static void test_ui_then_arm(void)
{
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When a door or window opens") && tap("Front Door") &&
              sheet_is("What should it do?", NULL),
          "the action sheet after a door");
    lv_obj_t *list = ui_find(lv_screen_active(), "pick_list");
    lv_obj_t *head = label(list, "The alarm");
    lv_obj_t *arm = row("Arm the panel");
    check(head != NULL && lv_obj_get_parent(head) == lv_obj_get_child(list, 0) && arm == lv_obj_get_child(list, 1) &&
              row("Disarm the panel") == lv_obj_get_child(list, 2) && row("Door chime") == lv_obj_get_child(list, 3) &&
              label(list, "Tell someone") != NULL && row("Send a notification") == lv_obj_get_child(list, 5) &&
              label(list, "Devices") != NULL && lv_obj_get_parent(label(list, "Devices")) == lv_obj_get_child(list, 6),
          "D23, BL-224: \"The alarm\" first with Arm, Disarm and the chime, then \"Tell someone\", then \"Devices\"");
    check(arm != NULL && label(arm, "After a warning") != NULL, "AUT-012: Arm says it warns first");
    ui_sheet_cancel();
    ui_run(50);

    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When an alarm sounds") && row("Arm the panel") != NULL &&
              row("Disarm the panel") == NULL,
          "D22, D23: after an alarm, Arm but never Disarm");
    ui_sheet_cancel();
    ui_run(50);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When a fire or CO alarm sounds") && row("Arm the panel") != NULL &&
              row("Disarm the panel") == NULL,
          "D22, D23: after a fire or CO alarm, Arm but never Disarm");
    ui_sheet_cancel();
    ui_run(50);

    host_panel_auto_less_secure(false);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is disarmed") && row("Arm the panel") != NULL &&
              row("Disarm the panel") == NULL,
          "D4, AUT-015: the option off: Arm stays, Disarm goes");
    ui_sheet_cancel();
    ui_run(50);
    host_panel_auto_less_secure(true);
    list_again();
}

/* AUT-011, AUT-012, AUT-014, D29: Arm Night at a time with the default warning; then changed. */
static void test_ui_arm_sheet(void)
{
    auto_rule_t got[AUTO_MAX];
    const size_t before = stored(got);
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("At a time of day") && sheet_kind() == SHEET_TIME, "the time sheet");
    ui_sheet_done(); /* 11:00 PM on weekdays */
    ui_run(50);
    check(tap("Arm the panel") && sheet_kind() == SHEET_FORM && sheet_is("Arm the panel", NULL),
          "AUT-011: Arm the panel opens its sheet");
    lv_obj_t *body = form_body();
    check(level_is("level_night") && strcmp(warn_words(), "5 minutes") == 0,
          "AUT-012: Night chosen, 5 minutes' warning by default");
    check(label(body, "Bypass open zones") != NULL &&
              label(body, "Off: it won't arm while a door or window is open") != NULL,
          "D29: Bypass open zones offered, off by default, with its detail");
    check(result_is("Warns at 10:55 PM, arms Night at 11:00 PM."), "AUT-012: the result line for a time trigger");
    check(label(body, "Inside sensors will be on.") == NULL && label(body, "Partition") == NULL,
          "no Away line with Night; one partition, no partition row");
    check(label(body, "Fire, CO and 24-hour zones are never bypassed. It does nothing if the panel is already "
                      "armed. Anyone can put it off with their code.") != NULL,
          "AUT-014: the note says what it never bypasses or does");
    check(stored(got) == before, "AUT-001: nothing stored before Done");
    form_done();
    const auto_rule_t *r = newest(got);
    check(sheet_kind() == SHEET_NONE && stored(got) == before + 1u && r != NULL && r->when.kind == AUTO_TRIG_TIME &&
              r->when.minute == 1380u && r->n == 1u && r->then[0].kind == AUTO_ACT_ARM &&
              r->then[0].partition == 1u && r->then[0].arm_level == ALARM_LEVEL_NIGHT &&
              r->then[0].warn_min == 5u && r->then[0].bypass_open == 0u && r->made_by == ALARM_USER_MASTER,
          "AUT-011: stored as picked (the validator passed it): Arm Night, 5 minutes, no bypass, the master's");
    const uint8_t slot = r != NULL ? r->slot : 0u;

    /* Its page: the Arm row opens the sheet again; Away, 7 minutes, bypassing */
    ui_page_open(PAGE_AUTOMATION, slot);
    ui_run(300);
    check(label(named(lv_screen_active(), "page_body"), "Add a second action") == NULL,
          "AUT-011: no second action after an Arm");
    press_text("Arm Night");
    check(sheet_kind() == SHEET_FORM && level_is("level_night"), "the Arm row opens its sheet (U16 review d)");
    form_press("level_away");
    check(level_is("level_away") && label(form_body(), "Inside sensors will be on.") != NULL &&
              result_is("Warns at 10:55 PM, arms Away at 11:00 PM."),
          "AUT-011: Away: its amber line and its sentence");
    form_press("plus");
    form_press("plus");
    check(strcmp(warn_words(), "7 minutes") == 0 && result_is("Warns at 10:53 PM, arms Away at 11:00 PM."),
          "AUT-012: the warning steps up a minute at a time");
    bypass_press();
    check(label(form_body(), "Arms anyway; each one is watched again once it closes") != NULL &&
              result_is("Warns at 10:53 PM, arms Away at 11:00 PM, bypassing open zones."),
          "D29: the switch on: its detail and the sentence say so");
    form_done();
    r = newest(got);
    check(sheet_kind() == SHEET_NONE && r != NULL && r->slot == slot && r->then[0].arm_level == ALARM_LEVEL_AWAY &&
              r->then[0].warn_min == 7u && r->then[0].bypass_open == 1u,
          "AUT-014, D29: stored Away, 7 minutes, bypassing open zones");

    /* 1 to 15 minutes, no further */
    ui_auto_edit_action(slot, 0);
    ui_run(50);
    for (int i = 0; i < 10; i++) {
        form_press("plus");
    }
    check(strcmp(warn_words(), "15 minutes") == 0, "AUT-012: at most 15 minutes");
    for (int i = 0; i < 16; i++) {
        form_press("minus");
    }
    check(strcmp(warn_words(), "1 minute") == 0 && result_is("Warns at 10:59 PM, arms Away at 11:00 PM, bypassing open zones."),
          "AUT-012: at least 1 minute");
    ui_sheet_cancel();
    ui_run(50);
    r = newest(got);
    check(r != NULL && r->then[0].warn_min == 7u, "AUT-001: Cancel stores nothing");

    /* AUT-015, D31: the option off - no switch; an unchanged Done keeps the stored bypass; a change
     * is saved without it (the validator refuses a bypass while the option is off) */
    host_panel_auto_less_secure(false);
    list_again();
    const uint16_t gen = B.auto_gen(B.ctx);
    ui_auto_edit_action(slot, 0);
    ui_run(50);
    check(sheet_kind() == SHEET_FORM && label(form_body(), "Bypass open zones") == NULL &&
              result_is("Warns at 10:53 PM, arms Away at 11:00 PM."),
          "D31: the option off: no Bypass row, and the sentence doesn't bypass");
    form_done();
    r = newest(got);
    check(B.auto_gen(B.ctx) == gen && r != NULL && r->then[0].bypass_open == 1u,
          "D31: nothing changed: nothing stored, the stored bypass kept");
    ui_auto_edit_when(slot);
    ui_run(50);
    check(tap("When a door or window opens") && tap("Front Door"), "its trigger changed to a door");
    ui_run(50);
    r = newest(got);
    check(sheet_kind() == SHEET_NONE && r != NULL && r->when.kind == AUTO_TRIG_ZONE_OPENED &&
              r->then[0].bypass_open == 0u &&
              !toasted("Your installer has turned off automations that unlock, open or disarm"),
          "D31: a trigger change with the option off is saved too, without the bypass");
    ui_auto_edit_action(slot, 0);
    ui_run(50);
    form_press("level_night");
    form_done();
    r = newest(got);
    check(r != NULL && r->then[0].arm_level == ALARM_LEVEL_NIGHT && r->then[0].bypass_open == 0u &&
              !toasted("Your installer has turned off automations that unlock, open or disarm"),
          "D31: a change with the option off is saved, without the bypass it can't use");
    host_panel_auto_less_secure(true);
    ui_page_back();
    ui_run(300);
    check(B.auto_delete_slot(B.ctx, slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "tidied away");
    list_again();

    /* AUT-012: after a trigger other than a time, the warning's length */
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When a door or window opens") && tap("Front Door") &&
              tap("Arm the panel") && result_is("Warns for 5 minutes, then arms Night."),
          "AUT-012, D23: after a door: \"Warns for 5 minutes, then arms Night.\"");
    const size_t n = stored(got);
    ui_sheet_cancel();
    ui_run(50);
    check(sheet_kind() == SHEET_NONE && stored(got) == n, "AUT-001: Cancel on the arm sheet stores nothing");
}

/* AUT-011, AUT-014: with two partitions, the partition row, its pick sheet and back. */
static void test_ui_arm_two(void)
{
    alarm_config_t cfg;
    check(B.get_config(B.ctx, &cfg), "the panel's settings");
    const uint8_t had = cfg.users[ALARM_USER_MASTER - 1u].partitions;
    check(second_partition(true, (uint8_t)(had | 0x03u)), "a second partition, Studio");
    auto_rule_t got[AUTO_MAX];
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("At a time of day"), "the time sheet");
    ui_sheet_done();
    ui_run(50);
    check(tap("Arm the panel") && label(form_body(), "Partition") != NULL && label(form_body(), "Home") != NULL &&
              result_is("Warns at 10:55 PM, arms Home Night at 11:00 PM."),
          "AUT-011: with two partitions, the partition row, and the sentence names it");
    form_press("level_away");
    press(partition_row());
    check(sheet_kind() == SHEET_PICK && sheet_is("Which partition?", NULL) && row("Home") != NULL &&
              row("Studio") != NULL && row("Any of them") == NULL,
          "AUT-011: the partition row opens the pick sheet: one partition, never any");
    ui_sheet_cancel();
    ui_run(50);
    check(sheet_kind() == SHEET_FORM && level_is("level_away") && result_is("Warns at 10:55 PM, arms Home Away at 11:00 PM."),
          "Cancel on the partition goes back to the arm sheet, the draft as it was");
    press(partition_row());
    check(tap("Studio") && sheet_kind() == SHEET_FORM && level_is("level_away") &&
              label(form_body(), "Studio") != NULL && result_is("Warns at 10:55 PM, arms Studio Away at 11:00 PM."),
          "the pick goes back to the arm sheet with Studio, the draft kept");
    form_done();
    const auto_rule_t *r = newest(got);
    check(sheet_kind() == SHEET_NONE && r != NULL && r->then[0].kind == AUTO_ACT_ARM && r->then[0].partition == 2u &&
              r->then[0].arm_level == ALARM_LEVEL_AWAY,
          "AUT-011: stored: Arm Studio Away");
    if (r != NULL) {
        check(B.auto_delete_slot(B.ctx, r->slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "tidied away");
    }
    /* U16 review e: the armed confirm's title names the partition as its note does */
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When the panel is armed") && tap("Away") && tap("Home") &&
              tap("Garage Door") && tap("Stop") && sheet_kind() == SHEET_CONFIRM &&
              sheet_is("Stop Garage Door when Home is armed?", "Every time Home is armed Away, Garage Door will stop."),
          "AUT-016: a named armed trigger's confirm names it in its title too");
    ui_sheet_cancel();
    ui_run(50);
    check(second_partition(false, had), "one partition again");
}

/* The bedtime idea (drawing automation-add-arm): 10:30 PM every day, then Arm Night. */
static void test_ui_bedtime(void)
{
    auto_rule_t got[AUTO_MAX];
    const size_t before = stored(got);
    ui_auto_add_start();
    ui_run(50);
    lv_obj_t *idea = row("Arm at bedtime");
    check(idea != NULL && label(idea, "Every night at a time you choose, after a warning") != NULL,
          "D4: the bedtime idea is offered");
    check(tap("Arm at bedtime") && sheet_kind() == SHEET_TIME, "AUT-011: it asks the time first");
    ui_sheet_done();
    ui_run(50);
    check(sheet_kind() == SHEET_FORM && level_is("level_night") && result_is("Warns at 10:25 PM, arms Night at 10:30 PM."),
          "AUT-012: then the arm sheet: Night, 10:30 PM every day, 5 minutes");
    form_done();
    const auto_rule_t *r = newest(got);
    check(stored(got) == before + 1u && r != NULL && r->when.kind == AUTO_TRIG_TIME && r->when.minute == 1350u &&
              r->when.days == 0u && r->then[0].kind == AUTO_ACT_ARM && r->then[0].arm_level == ALARM_LEVEL_NIGHT &&
              r->then[0].warn_min == 5u,
          "AUT-011: stored: at 10:30 PM every day, Arm Night, 5 minutes");
    if (r != NULL) {
        check(B.auto_delete_slot(B.ctx, r->slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "tidied away");
    }
    list_again();
}

/* "At 10:00 AM: Disarm partition 1", the master's. */
static uint8_t save_disarm(void)
{
    auto_rule_t got[AUTO_MAX];
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("At a time of day"), "the time sheet");
    ui_sheet_done();
    ui_run(50);
    check(tap("Disarm the panel") && sheet_kind() == SHEET_NONE, "a time-of-day Disarm, stored");
    const auto_rule_t *r = newest(got);
    return r != NULL ? r->slot : 0u;
}

static uint16_t uid_of(uint8_t slot)
{
    auto_rule_t got[AUTO_MAX];
    const size_t n = stored(got);
    for (size_t i = 0; i < n; i++) {
        if (got[i].slot == slot) {
            return got[i].uid;
        }
    }
    return 0u;
}

/* U16 review a-e on a Disarm: its trigger sheets, its refusal's words, its statuses, its row. */
static void test_ui_disarm_fixes(void)
{
    const uint8_t slot = save_disarm();
    const uint16_t uid = uid_of(slot);

    /* a, e: one partition: neither the alarm rows nor "armed" can be picked (D22, AUT-016) */
    ui_auto_edit_when(slot);
    ui_run(50);
    lv_obj_t *armed = row("When the panel is armed");
    lv_obj_t *fire = row("When a fire or CO alarm sounds");
    lv_obj_t *alarm = row("When an alarm sounds");
    check(armed != NULL && lv_obj_has_state(armed, LV_STATE_DISABLED), "AUT-016: no armed trigger for its Disarm");
    check(alarm != NULL && lv_obj_has_state(alarm, LV_STATE_DISABLED) && fire != NULL &&
              lv_obj_has_state(fire, LV_STATE_DISABLED),
          "D22: neither alarm row for a Disarm, the fire row too");
    ui_sheet_cancel();
    ui_run(50);

    /* a: two partitions: armed stays, but its own partition and "Any of them" are dimmed */
    alarm_config_t cfg;
    check(B.get_config(B.ctx, &cfg), "the panel's settings");
    const uint8_t had = cfg.users[ALARM_USER_MASTER - 1u].partitions;
    check(second_partition(true, (uint8_t)(had | 0x03u)), "a second partition, Studio");
    ui_auto_edit_when(slot);
    ui_run(50);
    armed = row("When the panel is armed");
    check(armed != NULL && !lv_obj_has_state(armed, LV_STATE_DISABLED) && tap("When the panel is armed") && tap("Away"),
          "AUT-016: with two partitions, armed may be picked");
    lv_obj_t *home = row("Home"), *studio = row("Studio"), *any = row("Any of them");
    check(home != NULL && lv_obj_has_state(home, LV_STATE_DISABLED) && any != NULL &&
              lv_obj_has_state(any, LV_STATE_DISABLED) && studio != NULL && !lv_obj_has_state(studio, LV_STATE_DISABLED),
          "AUT-016: its own partition and any of them dimmed, the other one not");
    ui_sheet_cancel();
    ui_run(50);

    /* b: a refused disarm, with two partitions: the maker still holds Home - the alarm's (D22) */
    auto_status_t st;
    memset(&st, 0, sizeof(st));
    st.result = AUTO_RES_ARM_REFUSED;
    st.alarm_res = ALARM_ERR_NOT_AUTHORIZED;
    st.at_epoch_ms = AT_7PM_MS;
    host_panel_auto_force_status(uid, &st);
    ui_page_open(PAGE_AUTOMATION, slot);
    ui_run(1100);
    lv_obj_t *body = named(lv_screen_active(), "page_body");
    check(label(body, "On \xC2\xB7 didn't disarm at 7:00 PM: Home's alarm was sounding") != NULL,
          "D22: a refused disarm the maker may do was the alarm's: Home's alarm was sounding");
    lv_obj_t *drow = page_row("Disarm Home");
    check(drow != NULL && lv_obj_has_flag(drow, LV_OBJ_FLAG_CLICKABLE), "two partitions: the Disarm row opens");
    ui_page_back();
    ui_run(300);
    check(second_partition(true, 0x02u), "the master's list holds only Studio");
    ui_page_open(PAGE_AUTOMATION, slot);
    ui_run(1100);
    check(label(named(lv_screen_active(), "page_body"), "On \xC2\xB7 didn't disarm at 7:00 PM: Home's alarm was sounding") != NULL,
          "AUT-011: a master reaches every partition whatever its list, as the engine: still the alarm's");
    ui_page_back();
    ui_run(300);
    check(B.user_set(B.ctx, "123456", 8, "8888", ALARM_AUTH_STANDARD, 0x02u, "Alex") == ALARM_OK,
          "a standard user Alex, Studio only");
    for (uint8_t maker_no = 8u;; maker_no = 0u) {
        auto_rule_t all[AUTO_MAX], dup;
        memset(&dup, 0, sizeof(dup));
        size_t n = stored(all);
        for (size_t i = 0; i < n; i++) {
            if (all[i].slot == slot) {
                dup = all[i];
            }
        }
        dup.made_by = maker_no;
        dup.uid = 0u;
        dup.slot = 0u;
        check(dup.n == 1u && host_panel_auto_inject(&dup), "a copy of the Disarm, made by another");
        list_again();
        n = stored(all);
        const auto_rule_t *other = n > 0u ? &all[n - 1u] : NULL;
        for (size_t i = 0; i < n; i++) {
            if (all[i].uid > (other != NULL ? other->uid : 0u)) {
                other = &all[i];
            }
        }
        if (other == NULL) {
            break;
        }
        const uint8_t other_slot = other->slot;
        host_panel_auto_force_status(other->uid, &st);
        ui_page_open(PAGE_AUTOMATION, other_slot);
        ui_run(1100);
        check(maker_no != 0u
                  ? label(named(lv_screen_active(), "page_body"), "Can't run: Alex can no longer disarm Home") != NULL
                  : label(named(lv_screen_active(), "page_body"), "On \xC2\xB7 didn't disarm at 7:00 PM: the panel said no") != NULL,
              maker_no != 0u ? "AUT-011: a standard maker who lost the partition: Can't run, as drawn"
                             : "nobody named: the panel said no, never the alarm's");
        host_panel_auto_force_status(other->uid, NULL);
        ui_page_back();
        ui_run(300);
        check(B.auto_delete_slot(B.ctx, other_slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "the copy tidied away");
        list_again();
        if (maker_no == 0u) {
            break;
        }
    }
    check(B.user_delete(B.ctx, "123456", 8) == ALARM_OK, "Alex tidied away");
    check(second_partition(false, had), "one partition again");
    ui_page_open(PAGE_AUTOMATION, slot);
    ui_run(1100);
    body = named(lv_screen_active(), "page_body");
    check(label(body, "On \xC2\xB7 didn't disarm at 7:00 PM: the alarm was sounding") != NULL,
          "D22: one partition: the alarm was sounding");
    /* d: one partition: the Disarm row leads nowhere */
    drow = page_row("Disarm the panel");
    check(drow != NULL && !lv_obj_has_flag(drow, LV_OBJ_FLAG_CLICKABLE) &&
              label(drow, "\xEE\x81\xAF") == NULL,
          "U16 review d: one partition: no chevron and no tap on the Disarm row");
    host_panel_auto_force_status(uid, NULL);

    /* e: the nudge counts only an Arm that is switched on */
    const uint16_t arm_uid = save_night_arm(0u);
    check(host_panel_auto_set_off(arm_uid, true, host_panel_auto_gen()) == AUTO_SAVE_OK, "the arm switched off");
    ui_run(1100);
    check(label(named(lv_screen_active(), "page_body"), "Nothing arms it again. Add one that arms in the evening.") != NULL,
          "DESIGN Disarming 5: a switched-off Arm doesn't count");
    check(host_panel_auto_set_off(arm_uid, false, host_panel_auto_gen()) == AUTO_SAVE_OK, "the arm switched on");
    ui_run(1100);
    check(label(named(lv_screen_active(), "page_body"), "Nothing arms it again. Add one that arms in the evening.") == NULL,
          "DESIGN Disarming 5: a switched-on Arm does");
    ui_page_back();
    ui_run(300);

    /* a: the refusal's own words (AUTO_ERR_UNDOES_ARMING), never the generic ones */
    auto_rule_t r;
    memset(&r, 0, sizeof(r));
    r.when.kind = AUTO_TRIG_ARMED;
    r.n = 1;
    r.then[0].kind = AUTO_ACT_DISARM;
    r.then[0].partition = 1;
    r.then[0].value = -1;
    uint16_t got_uid = 0;
    check(!ui_auto_save(&r, &got_uid) && toasted("It can't disarm what its own trigger just armed"),
          "AUT-016: a Disarm after its own armed trigger is refused in its own words");
    ui_run(1100);

    auto_rule_t got[AUTO_MAX];
    const size_t n = stored(got);
    for (size_t i = 0; i < n; i++) {
        if (got[i].slot == slot || got[i].uid == arm_uid) {
            B.auto_delete_slot(B.ctx, got[i].slot, B.auto_gen(B.ctx));
        }
    }
    list_again();
}

/* U16 review c: a put-off with nobody named never reads "put off by" and an empty name. */
static void test_ui_put_off_nobody(void)
{
    const uint16_t uid = save_night_arm(0u);
    list_again();
    auto_rule_t got[AUTO_MAX];
    const auto_rule_t *r = newest(got);
    const uint8_t slot = r != NULL ? r->slot : 0u;
    auto_status_t st;
    memset(&st, 0, sizeof(st));
    st.result = AUTO_RES_PUT_OFF;
    st.choice = AUTO_PUT_OFF_HOUR;
    st.until_epoch_ms = AT_7PM_MS + 3600000LL;
    st.at_epoch_ms = AT_7PM_MS;
    host_panel_auto_force_status(uid, &st);
    ui_run(1100);
    check(label(named(lv_screen_active(), "page_body"), "Put off until 8:00 PM") != NULL,
          "AUT-013, D20: the list's line without a name");
    ui_page_open(PAGE_AUTOMATION, slot);
    ui_run(1100);
    check(label(named(lv_screen_active(), "page_body"), "On \xC2\xB7 put off until 8:00 PM") != NULL,
          "AUT-013, D20: the page's switch without a name");
    st.choice = AUTO_PUT_OFF_NIGHT;
    st.night = 1;
    host_panel_auto_force_status(uid, &st);
    ui_run(1100);
    check(label(named(lv_screen_active(), "page_body"), "On \xC2\xB7 not tonight \xC2\xB7 arms again tomorrow at 7:10 PM") != NULL,
          "AUT-013: not tonight, without a name");
    host_panel_auto_force_status(uid, NULL);
    ui_page_back();
    ui_run(300);
    check(B.auto_delete_slot(B.ctx, slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "tidied away");
    list_again();
}

/* BL-170 C6: phase C's editors and U16's review fixes, in test_ui's LVGL session (the section
 * still open with the master code). */
static void test_ui_bl170(void)
{
    test_ui_armed_stop();
    test_ui_disarm();
    test_ui_disarm_two();
    test_ui_change();
    test_ui_arm_page();
    test_ui_then_arm();
    test_ui_arm_sheet();
    test_ui_arm_two();
    test_ui_bedtime();
    test_ui_disarm_fixes();
    test_ui_put_off_nobody();
}

/* ---- C5: the warning at the wall, driven as a finger would (ui_auto_warn.c) ---- */

static lv_obj_t *on_screen(const char *name)
{
    return named(lv_screen_active(), name);
}

/* Digits on the pad called name ("modal" over the screen, "pad" on Home), then check. */
static void type_on(const char *pad, const char *code)
{
    lv_obj_t *keys = lv_obj_find_by_name(on_screen(pad), "pad_keys");
    for (const char *k = code; keys != NULL && *k != '\0'; k++) {
        const int idx = (*k == '0') ? 10 : (*k == 'k') ? 11 : (*k - '1');
        press(lv_obj_get_child(keys, idx));
    }
}

/* The simulator moves on by ms, then the screen catches up at its 1 s tick. Someone stands at
 * the panel meanwhile: the screen saver stays away. */
static void later(uint32_t ms)
{
    host_panel_advance(ms);
    s_ms += ms;
    lv_display_trigger_activity(NULL);
    ui_run(1100);
}

static const char *subj(lv_subject_t *s)
{
    return lv_subject_get_string(s);
}

static bool banner_is(const char *text)
{
    return lv_subject_get_int(&subject_bar_banner) == 5 && on_screen("arm_banner") != NULL &&
           strcmp(subj(&subject_arm_banner), text) == 0;
}

static bool card_is(int32_t kind, const char *title, const char *detail)
{
    return lv_subject_get_int(&subject_arm_strip) == kind && on_screen("arm_strip") != NULL &&
           strcmp(subj(&subject_arm_strip_title), title) == 0 &&
           strcmp(subj(&subject_arm_strip_detail), detail) == 0;
}

/* D19: the three arming tiles are on Home, below the card, each tall enough for its words. */
static bool tiles_as_ever(void)
{
    lv_obj_t *arm = on_screen("panel_arm");
    lv_obj_t *card = on_screen("arm_strip");
    const char *names[3] = {"Away", "Stay", "Night"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *l = label(arm, names[i]);
        lv_obj_t *tile = l != NULL ? lv_obj_get_parent(lv_obj_get_parent(l)) : NULL;
        if (tile == NULL || !lv_obj_has_flag(tile, LV_OBJ_FLAG_CLICKABLE) ||
            lv_obj_get_height(tile) < 90) {
            return false;
        }
        if (card != NULL) {
            lv_area_t a, b;
            lv_obj_get_coords(card, &a);
            lv_obj_get_coords(tile, &b);
            if (b.y1 <= a.y2) {
                return false; /* the card is above the tiles, never over them */
            }
        }
    }
    return true;
}

static void put_off(uint8_t choice)
{
    press(on_screen("arm_strip_put_off"));
    press(on_screen(choice == AUTO_PUT_OFF_HOUR ? "poff_hour" : "poff_night"));
}

static void test_ui_warning(void)
{
    start(AT_7PM_MS); /* 7:00:01 PM */
    check(B.user_set(B.ctx, "123456", 4, "4444", ALARM_AUTH_STANDARD, 0x01u, "Pat") == ALARM_OK, "a user Pat");
    check(B.user_set(B.ctx, "123456", 5, "5555", ALARM_AUTH_ARM_ONLY, 0x01u, "Sam") == ALARM_OK, "an arm-only user Sam");
    check(B.user_set(B.ctx, "123456", 6, "6666", ALARM_AUTH_GUEST, 0x01u, "Alex") == ALARM_OK, "a guest Alex");
    const uint16_t uid = save_night_arm(0);
    ui_run(1100);
    press(on_screen("nav_home"));

    /* AUT-012: nothing before 7:05; from 7:05 the banner and the card, the tiles as they are */
    later(4u * MIN_MS + 40000u); /* 7:04:50 */
    check(lv_subject_get_int(&subject_bar_banner) != 5 && on_screen("arm_strip") == NULL,
          "AUT-012: no banner or card before the warning");
    later(20000u); /* 7:05:10 */
    check(banner_is("Arming Night at 7:10 PM"), "AUT-012: the steady amber banner from 7:05");
    check(card_is(1, "Arming Night at 7:10 PM", "Set by an automation") &&
              on_screen("arm_strip_put_off") != NULL && on_screen("arm_strip_note") == NULL,
          "AUT-012, D26: Home's card with one Put off button");
    check(tiles_as_ever(), "D19: the arming tiles stay, usable, below the card");
    press(on_screen("nav_zones"));
    check(banner_is("Arming Night at 7:10 PM"), "AUT-012: the banner on every screen");
    press(on_screen("arm_banner"));
    check(lv_subject_get_int(&subject_view) == 0, "AUT-012: a tap on the banner opens Home");

    /* D28: a window open - the amber line, gone once it closes */
    host_panel_zone(3, ALARM_ZEV_FAULT);
    later(0u);
    check(on_screen("arm_strip_note") != NULL &&
              strcmp(subj(&subject_arm_strip_note), "Not ready: it won't arm yet") == 0,
          "D28: Not ready: it won't arm yet");
    check(tiles_as_ever(), "D19: the tiles stay beside the not-ready line");
    host_panel_zone(3, ALARM_ZEV_RESTORE);
    later(0u);
    check(on_screen("arm_strip_note") == NULL, "D28: the line goes once the zone closes");

    /* D26: Put off opens the choice; Cancel leaves the warning as it was */
    press(on_screen("arm_strip_put_off"));
    check(sheet_kind() == SHEET_PUT_OFF && strcmp(sheet_title(), "Put off arming?") == 0 &&
              strcmp(sheet_note(), "Arming Night at 7:10 PM") == 0,
          "D26: Put off asks an hour or not tonight");
    check(strcmp(subj(&subject_poff_hour), "Arms Night at 8:10 PM") == 0 &&
              strcmp(subj(&subject_poff_night_title), "Not tonight") == 0 &&
              strcmp(subj(&subject_poff_night), "Arms again tomorrow at 7:10 PM") == 0,
          "AUT-013: the choices say when it arms");
    press(on_screen("poff_cancel"));
    check(sheet_kind() == 0 && card_is(1, "Arming Night at 7:10 PM", "Set by an automation"),
          "D26: Cancel leaves the warning");

    /* AUT-013: An hour later -> the pad -> Pat's code: the hold note, the banner gone */
    put_off(AUTO_PUT_OFF_HOUR);
    check(lv_subject_get_int(&subject_modal_pad) == 1 &&
              strcmp(subj(&subject_pad_title), "Put off an hour") == 0 &&
              strcmp(subj(&subject_pad_note), "Any code puts it off an hour. Arms Night at 8:10 PM.") == 0,
          "D26: the code pad says what the code does");
    unsigned beeps = s_error_beeps;
    type_on("modal", "9999k");
    check(lv_subject_get_int(&subject_modal_pad) == 1 && s_error_beeps == beeps + 1u &&
              strcmp(subj(&subject_pad_hint), ui_result_text(ALARM_ERR_INVALID_CODE)) == 0,
          "AUT-013: a wrong code beeps and says what every pad says");
    type_on("modal", "4444k");
    auto_warning_t w;
    check(lv_subject_get_int(&subject_modal_pad) == 0 && warning_of(uid, &w) &&
              w.state == AUTO_WARN_HOUR && w.put_off_by == 4u,
          "AUT-013: Pat's code put it off an hour");
    later(0u);
    check(card_is(2, "Arms Night at 8:10 PM", "Put off by Pat") && on_screen("arm_strip_put_off") == NULL,
          "D27: the quiet note in the card's place");
    check(lv_subject_get_int(&subject_bar_banner) != 5, "D20: the banner goes after a put-off");
    check(!toasted("Put off"), "no toast: the screen shows it");
    check(tiles_as_ever(), "D19: the tiles don't change beside the note");

    /* D20: the warning back 5 minutes before the new time, with its own Put off */
    later(58u * MIN_MS); /* 8:04:xx */
    check(card_is(2, "Arms Night at 8:10 PM", "Put off by Pat"), "D27: the note until 8:05");
    later(2u * MIN_MS); /* 8:05:xx */
    check(banner_is("Arming Night at 8:10 PM") && card_is(1, "Arming Night at 8:10 PM", "Set by an automation"),
          "D20: the warning and its Put off again 5 minutes before");

    /* D27 (decided 2026-10-05): Not tonight needs a code that may disarm Home */
    put_off(AUTO_PUT_OFF_NIGHT);
    check(lv_subject_get_int(&subject_modal_pad) == 1 && strcmp(subj(&subject_pad_title), "Not tonight") == 0 &&
              strcmp(subj(&subject_pad_note),
                     "Arms again tomorrow at 7:10 PM. Needs a code that may disarm Home.") == 0,
          "D26: Not tonight's pad says it needs a code that may disarm");
    beeps = s_error_beeps;
    type_on("modal", "5555k");
    check(lv_subject_get_int(&subject_modal_pad) == 1 && s_error_beeps == beeps + 1u &&
              strcmp(subj(&subject_pad_hint), "That code can only put it off an hour") == 0,
          "D27: an arm-only code can't put it off for the night");
    type_on("modal", "6666k");
    check(lv_subject_get_int(&subject_modal_pad) == 1 &&
              strcmp(subj(&subject_pad_hint), "That code can only put it off an hour") == 0,
          "D27: nor a guest code");
    check(warning_of(uid, &w) && w.state == AUTO_WARN_ON, "D27: nothing was put off");
    type_on("modal", "123456k");
    check(lv_subject_get_int(&subject_modal_pad) == 0 && warning_of(uid, &w) &&
              w.state == AUTO_WARN_NIGHT && w.until_minute == 360u,
          "AUT-013: the master code holds it until 6:00 AM");
    later(0u);
    check(lv_subject_get_int(&subject_arm_strip) == 2 &&
              strcmp(subj(&subject_arm_strip_title), "Won't arm tonight") == 0 &&
              strncmp(subj(&subject_arm_strip_detail), "Put off by ", 11) == 0,
          "D27: Won't arm tonight, by whom");
    later(6u * MIN_MS); /* 8:11 */
    check(part1_state() == ALARM_STATE_DISARMED && lv_subject_get_int(&subject_arm_strip) == 2,
          "AUT-013: not armed at 8:10; the note stays");

    /* D27: never while armed - armed by hand, the note goes; disarmed, it is back */
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_STAY) == ALARM_OK, "armed Stay");
    later(0u);
    check(lv_subject_get_int(&subject_arm_strip) == 0, "D27: no note while armed");
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarmed");
    later(0u);

    /* AUT-014: the pad open, the house armed elsewhere: the pad closes and the card goes */
    start(AT_7PM_MS);
    const uint16_t uid2 = save_night_arm(0);
    later(5u * MIN_MS + 10000u); /* 7:05:11 */
    put_off(AUTO_PUT_OFF_HOUR);
    check(lv_subject_get_int(&subject_modal_pad) == 1, "the put-off pad is open");
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_AWAY) == ALARM_OK, "armed Away elsewhere");
    later(0u);
    check(lv_subject_get_int(&subject_modal_pad) == 0 && lv_subject_get_int(&subject_arm_strip) == 0 &&
              lv_subject_get_int(&subject_bar_banner) != 5,
          "AUT-013: armed meanwhile, the pad, the card and the banner go");
    later(2u * MIN_MS); /* past the exit time */
    check(!warning_of(uid2, &w) && lv_subject_get_int(&subject_arm_strip) == 0,
          "AUT-014: once armed, the warning has ended");
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarmed");

    /* AUT-014, D19: arming by hand with the tiles during the warning ends the card */
    start(AT_7PM_MS);
    const uint16_t uid3 = save_night_arm(0);
    later(5u * MIN_MS + 10000u);
    check(card_is(1, "Arming Night at 7:10 PM", "Set by an automation"), "the card again");
    lv_obj_t *night = label(on_screen("panel_arm"), "Night");
    press(night != NULL ? lv_obj_get_parent(lv_obj_get_parent(night)) : NULL);
    type_on("pad", "123456k");
    later(0u);
    check(part1_state() != ALARM_STATE_DISARMED && lv_subject_get_int(&subject_arm_strip) == 0 &&
              lv_subject_get_int(&subject_bar_banner) != 5,
          "D19, AUT-014: armed Night by hand from the tiles; the card and the banner are gone");
    later(2u * MIN_MS);
    check(!warning_of(uid3, &w), "AUT-014: once armed, the warning has ended");
    check(remote(ALARM_CMD_DISARM, ALARM_LEVEL_NONE) == ALARM_OK, "disarmed");
    later(0u);
    check(lv_subject_get_int(&subject_arm_strip) == 0, "AUT-014: the warning doesn't come back");

    /* D28, D29: with "Bypass open zones", the line says so instead */
    start(AT_7PM_MS);
    (void)save_night_arm(1);
    host_panel_zone(3, ALARM_ZEV_FAULT);
    later(5u * MIN_MS + 10000u);
    check(card_is(1, "Arming Night at 7:10 PM", "Set by an automation") && on_screen("arm_strip_note") != NULL &&
              strcmp(subj(&subject_arm_strip_note), "Open zones will be bypassed") == 0,
          "D28: Open zones will be bypassed");
    host_panel_zone(3, ALARM_ZEV_RESTORE);

    /* CORE-USR-034: wrong codes at the put-off pad lock the keypad out like any pad's */
    start(AT_7PM_MS);
    const uint16_t uid4 = save_night_arm(0);
    later(5u * MIN_MS + 10000u);
    put_off(AUTO_PUT_OFF_HOUR);
    alarm_snapshot_t snap;
    int tries = 0;
    do {
        type_on("modal", "9999k");
        tries++;
        B.get_snapshot(B.ctx, &snap);
    } while (snap.local_lockout_until == 0u && tries < 12);
    ui_run(1100);
    check(snap.local_lockout_until != 0u && tries <= 6,
          "CORE-USR-034: wrong put-off codes lock the keypad out");
    check(strncmp(subj(&subject_pad_hint), "Keypad locked. Try again in ", 28) == 0,
          "CORE-USR-034: the pad says it is locked, as every pad does");
    check(warning_of(uid4, &w) && w.state == AUTO_WARN_ON, "the warning goes on");
    press(lv_obj_find_by_name(on_screen("modal"), "pad_close"));
}

/* The C5 review: the choice closes when the house is armed meanwhile; a busy service keeps the
 * pad open and says so; and after an hour's put-off moves a 5:30 PM arming to 6:30 PM the second
 * choice reads Not tonight, as the runner holds it (until 6:00 AM, D26). */
static void test_ui_put_off_more(void)
{
    start(AT_7PM_MS);
    (void)save_night_arm(0);
    ui_run(1100);
    press(on_screen("nav_home"));
    later(5u * MIN_MS + 10000u); /* 7:05:11 */
    press(on_screen("arm_strip_put_off"));
    check(sheet_kind() == SHEET_PUT_OFF, "D26: the choice is open");
    check(remote(ALARM_CMD_ARM, ALARM_LEVEL_AWAY) == ALARM_OK, "armed Away elsewhere");
    later(0u);
    check(sheet_kind() != SHEET_PUT_OFF && lv_subject_get_int(&subject_arm_strip) == 0,
          "AUT-013: armed meanwhile, the choice closes and the card goes");

    start(AT_7PM_MS);
    const uint16_t uid = save_night_arm(0);
    ui_run(1100);
    press(on_screen("nav_home"));
    later(5u * MIN_MS + 10000u);
    put_off(AUTO_PUT_OFF_HOUR);
    host_panel_auto_busy(true);
    type_on("modal", "123456k");
    check(lv_subject_get_int(&subject_modal_pad) == 1 &&
              strcmp(subj(&subject_pad_hint), "The panel is busy. Try again in a moment.") == 0,
          "C4: a busy service keeps the pad open and says so");
    host_panel_auto_busy(false);
    type_on("modal", "123456k");
    auto_warning_t w;
    check(lv_subject_get_int(&subject_modal_pad) == 0 && warning_of(uid, &w) &&
              w.state == AUTO_WARN_HOUR,
          "C4: the same code works once it isn't busy");

    start(AT_NOON_MS);
    auto_rule_t r;
    memset(&r, 0, sizeof(r));
    r.when.kind = AUTO_TRIG_TIME;
    r.when.minute = 17u * 60u + 30u;
    r.n = 1;
    r.then[0].kind = AUTO_ACT_ARM;
    r.then[0].partition = 1;
    r.then[0].arm_level = ALARM_LEVEL_NIGHT;
    r.then[0].warn_min = 5;
    r.then[0].value = -1;
    auto_check_t why = AUTO_OK;
    uint16_t uid2 = 0;
    check(B.auto_save(B.ctx, &r, ALARM_USER_MASTER, B.auto_gen(B.ctx), &why, &uid2) ==
                  AUTO_SAVE_OK && uid2 != 0u,
          "a 5:30 PM Night arm is saved");
    ui_run(1100);
    press(on_screen("nav_home"));
    later(5u * 60u * MIN_MS + 25u * MIN_MS + 10000u); /* 5:25:11 PM */
    press(on_screen("arm_strip_put_off"));
    check(strcmp(subj(&subject_poff_night_title), "Not today") == 0, "D26: Not today at 5:25 PM");
    press(on_screen("poff_hour"));
    type_on("modal", "123456k");
    later(60u * MIN_MS); /* 6:25:11 PM, the warning again for 6:30 */
    press(on_screen("arm_strip_put_off"));
    check(sheet_kind() == SHEET_PUT_OFF &&
              strcmp(subj(&subject_poff_night_title), "Not tonight") == 0,
          "D26: moved to 6:30 PM, the choice is Not tonight, as the runner holds it");
    press(on_screen("poff_night"));
    type_on("modal", "123456k");
    check(warning_of(uid2, &w) && w.state == AUTO_WARN_NIGHT,
          "D26: held until 6:00 AM, as the words said");
}

/* D30, AUT-009, AUT-014: once an automation has armed, Home's state card says so - "By
 * automation" and how many zones it bypassed, never the person who saved it. */
static void test_ui_armed_by_automation(void)
{
    start(AT_7PM_MS);
    (void)save_night_arm(1);
    host_panel_zone(3, ALARM_ZEV_FAULT); /* a window left open: bypassed */
    ui_run(1100);
    press(on_screen("nav_home"));
    later(10u * MIN_MS + 30000u); /* 7:10:31, armed, exit time running */
    later(2u * MIN_MS);
    const char *note = subj(&subject_hero_note);
    check(part1_state() == ALARM_STATE_ARMED, "D29: armed Night with the window bypassed");
    check(strncmp(note, "Since ", 6) == 0 &&
              strstr(note, " · By automation · 1 zone bypassed") != NULL,
          "D30, AUT-014: Home's card says By automation and 1 zone bypassed");
    check(strstr(note, "By Master") == NULL, "D30: never read as the master's arming");
    if (strstr(note, "By automation") == NULL) {
        printf("  (the card's foot: \"%s\")\n", note);
    }
}

/* ---- BL-224: automations without devices (docs/features/41 DESIGN.md section 15) ------------
 * Driven as a finger would, on Harbor House with an output "For automations" (Garden Lights): an
 * output's action and its "Turn it off after", a notification as the second action and its
 * removal, a trouble trigger, and the installer's sheets (Arm, never Disarm). Everything it adds
 * is deleted again. */

/* The form sheet's row holding this label, tapped. */
static void form_row_press(const char *text)
{
    lv_obj_t *o = label(form_body(), text);
    while (o != NULL && !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) {
        o = lv_obj_get_parent(o);
    }
    if (o != NULL) {
        lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
        ui_run(50);
    }
}

static void test_ui_bl224(void)
{
    auto_rule_t got[AUTO_MAX];
    host_panel_auto_outputs(true);
    list_again();
    const size_t before = stored(got);

    /* N3, N4: an output after a time, then its "Turn it off after" */
    ui_auto_add_start();
    ui_run(50);
    check(row("Switch an output when an alarm sounds") != NULL && row("Remind me to arm") != NULL,
          "BL-224 N2: the ideas that need no device, the output's too");
    check(tap("Start from scratch") && tap("At a time of day") && sheet_kind() == SHEET_TIME, "the time sheet");
    ui_sheet_done();
    ui_run(50);
    check(sheet_is("What should it do?", NULL) && row("Remind to arm") != NULL &&
              row("Tell me if it's still armed") != NULL && row("Door chime") != NULL &&
              row("Send a notification") != NULL && row("Garden Lights") != NULL,
          "BL-224 N3: after a time, The alarm, Tell someone and Outputs");
    check(tap("Garden Lights") && sheet_is("What should Garden Lights do?", NULL) && tap("Turn on") &&
              sheet_kind() == SHEET_NONE,
          "BL-224 N4: an output's Turn on, stored at the last pick");
    const auto_rule_t *r = newest(got);
    check(stored(got) == before + 1u && r != NULL && r->n == 1u && r->then[0].kind == AUTO_ACT_OUTPUT &&
              r->then[0].output == 1u && r->then[0].dev_action == ACT_ON && r->then[0].off_after_min == 0u,
          "BL-224: stored: Turn on output 1");
    const uint8_t slot = r != NULL ? r->slot : 0u;
    list_again();
    ui_auto_edit_off_after(slot, 0);
    ui_run(50);
    check(sheet_is("Turn it off after", NULL) && tap("30 minutes"), "BL-224: an output's Turn it off after");
    r = newest(got);
    check(r != NULL && r->then[0].off_after_min == 30u, "BL-224: stored: off after 30 minutes");

    /* a notification as its second action; the same output is never offered twice (AUT-007) */
    list_again();
    ui_auto_add_action(slot);
    ui_run(50);
    check(sheet_is("What should it do?", NULL) && row("Arm the panel") == NULL && row("Remind to arm") == NULL &&
              row("Garden Lights") == NULL && row("Door chime") != NULL,
          "BL-224: a second action: the chime and a notification, never Arm, a reminder or the same output");
    check(tap("Send a notification") && sheet_kind() == SHEET_FORM && label(form_body(), "Goes to") != NULL,
          "BL-224 N4: the notification's sheet");
    form_done();
    r = newest(got);
    check(r != NULL && r->n == 2u && r->then[1].kind == AUTO_ACT_NOTIFY && r->then[1].partition == 0u,
          "BL-224: stored: the notification beside the output");
    list_again();
    ui_auto_edit_action(slot, 1);
    ui_run(50);
    check(sheet_kind() == SHEET_FORM && label(form_body(), "Remove this action") != NULL,
          "BL-224: one of two: the notification's sheet offers to remove it");
    form_row_press("Remove this action");
    r = newest(got);
    check(r != NULL && r->n == 1u && r->then[0].kind == AUTO_ACT_OUTPUT, "BL-224: removed; the output stays");
    list_again();
    check(B.auto_delete_slot(B.ctx, slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "tidied away");
    list_again();

    /* N5: a trouble; no reminder and no Disarm after it */
    ui_auto_add_start();
    ui_run(50);
    check(tap("Start from scratch") && tap("When there's a trouble") && sheet_is("Which trouble?", NULL) &&
              tap("The power goes off"),
          "BL-224 N5: which trouble");
    check(sheet_is("What should it do?", NULL) && row("Remind to arm") == NULL && row("Disarm the panel") == NULL &&
              tap("Send a notification"),
          "BL-224: after a trouble, no reminder and no Disarm");
    form_done();
    r = newest(got);
    check(r != NULL && r->when.kind == AUTO_TRIG_TROUBLE && r->when.trouble == AUTO_TROUBLE_AC &&
              r->then[0].kind == AUTO_ACT_NOTIFY,
          "BL-224: stored: when the power goes off, a notification");
    if (r != NULL) {
        check(B.auto_delete_slot(B.ctx, r->slot, B.auto_gen(B.ctx)) == AUTO_SAVE_OK, "tidied away");
    }
    list_again();

    /* N1: the installer's code: the arming ideas and Arm, never Disarm (USR-001) */
    const uint8_t was = S.auth_user;
    S.auth_user = ALARM_USER_INSTALLER;
    ui_auto_add_start();
    ui_run(50);
    check(row("Arm at bedtime") != NULL && row("Arm Away on weekday mornings") != NULL &&
              row("Disarm at opening time") == NULL,
          "BL-224 N1: the installer's ideas: arming, never disarming");
    check(tap("Start from scratch") && tap("At a time of day"), "the time sheet");
    ui_sheet_done();
    ui_run(50);
    check(row("Arm the panel") != NULL && row("Disarm the panel") == NULL,
          "BL-224 N1: the installer may pick Arm, never Disarm");
    ui_sheet_cancel();
    ui_run(50);
    S.auth_user = was;
    host_panel_auto_outputs(false);
    list_again();
    check(stored(got) == before, "BL-224: nothing left behind");
}

int main(void)
{
    /* The panel's fixed time zone, as the PC simulator sets it (sim_main.c). */
#if defined(_WIN32)
    _putenv("TZ=EST5EDT,M3.2.0,M11.1.0");
    _tzset();
#else
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
#endif
    test_seeded();
    test_porch_evening();
    test_porch_noon();
    test_away();
    test_fire();
    test_offline();
    test_kind_changed();
    test_unlock_and_the_option();
    test_edits();
    test_off_switch();
    test_hooks();
    test_ui();
    test_ui_editors();
    test_ui_bl170();
    test_ui_bl224();
    test_arm_on_time();
    test_arm_in_installer_mode();
    test_put_off_hour();
    test_put_off_codes();
    test_put_off_today();
    test_put_off_lockout();
    test_bypass_and_the_option();
    test_ui_warning();
    test_ui_armed_by_automation();
    test_ui_put_off_more();
    s_refused += counts().refused;
    check(s_refused == 0u, "AUT-002: the port refused none of the automations' actions");
    printf("auto_smoke: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
