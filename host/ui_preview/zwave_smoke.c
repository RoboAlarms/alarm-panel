/*
 * zwave_smoke.c - a scripted proof of docs/features/36's simulator world, without a window: the
 * same host_panel_zwave_* calls the "Z-Wave" button group in host/ui_sim/sim_main.c makes, run
 * head to tail on the simulator's own clock, checking the panel's real state (not just the log
 * strip's prose) - the real zone and trouble outcomes a bound Z-Wave input produces, and what the
 * panel's own Z-Wave (zwave_zones' zwz_panel, as the firmware runs it) logs into the engine and
 * keeps across a restart of the panel.
 *
 * ALARM_SIM_ZWAVE only. Built by `pwsh tools/ui_sim.ps1 -ZWave -BuildOnly`; run by hand:
 * build/ui_sim-zwave/zwave_smoke.exe (or wherever that build lands). Not part of the host test
 * suite (host/CMakeLists.txt only discovers components/<tier>/<name>/test); CI builds it in
 * host/ui_preview and runs it (ZW-021, host-tests.yml). This is a
 * simulator-side check, like host/ui_preview's own scripted walk-through.
 */
#include <stdio.h>
#include <string.h>

#include "host_panel.h"

#if !ALARM_SIM_ZWAVE

int main(void)
{
    printf("ALARM_SIM_ZWAVE is off: nothing to prove.\n");
    return 0;
}

#else

#define MIN_MS (60u * 1000u)
#define ZONE   9u /* sim_main.c's ZW_SIM_DOOR_ZONE: zones 1-8 are the demo zones */

static int g_fail;
static uint32_t g_seq_seen;

/* Prints every status change as it happens (not just the outcome), so a failure shows the whole
 * story instead of only the last line. */
static void drain_log(void)
{
    const uint32_t seq = host_panel_zwave_seq();
    if (seq != g_seq_seen) {
        g_seq_seen = seq;
        printf("     .. %s\n", host_panel_zwave_status());
    }
}

static void expect(bool ok, const char *what)
{
    drain_log();
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        g_fail++;
    }
}

static void run_for(uint32_t ms)
{
    host_panel_advance((alarm_ms_t)ms);
    drain_log();
}

/* Runs the world forward, `step_ms` at a time, until `cond` is true or `timeout_ms` passes. */
static bool run_until_step(bool (*cond)(void), uint32_t timeout_ms, uint32_t step_ms)
{
    uint32_t t = 0;
    while (t < timeout_ms) {
        if (cond()) {
            return true;
        }
        run_for(step_ms);
        t += step_ms;
    }
    return cond();
}

static bool run_until(bool (*cond)(void), uint32_t timeout_ms)
{
    return run_until_step(cond, timeout_ms, 1000u);
}

/* The same, in coarser steps, for the multi-hour waits (network.md/zones.md's graces): the
 * outcome does not need millisecond precision here - the library's own tests already have that -
 * only proving the simulator's wiring reaches it at all. */
static bool run_until_slow(bool (*cond)(void), uint32_t timeout_ms)
{
    return run_until_step(cond, timeout_ms, 5000u);
}

static bool sent_ok(void)
{
    return strstr(host_panel_zwave_status(), "not delivered") == NULL;
}

static bool pin_pending(void) { return host_panel_zwave_pin_pending(); }
static bool door_ready(void) { return host_panel_zwave_door_ready(); }
static bool motion_ready(void) { return host_panel_zwave_motion_ready(); }
static bool door_unreachable(void) { return host_panel_zwave_door_unreachable(); }
static bool door_reachable_again(void) { return !host_panel_zwave_door_unreachable(); }
static bool link_down(void) { return !host_panel_zwave_link_up(); }
static bool link_back_up(void) { return host_panel_zwave_link_up(); }

static bool zone_flag(uint16_t f) { return (host_panel_zone_flags(ZONE) & f) != 0u; }
static bool zone_no_flag(uint16_t f) { return (host_panel_zone_flags(ZONE) & f) == 0u; }
static bool zone_faulted(void) { return zone_flag(ALARM_ZS_FAULT); }
static bool zone_restored(void) { return zone_no_flag(ALARM_ZS_FAULT); }
static bool zone_tampered(void) { return zone_flag(ALARM_ZS_TAMPER); }
static bool zone_tamper_restored(void) { return zone_no_flag(ALARM_ZS_TAMPER); }
static bool zone_low_battery(void) { return zone_flag(ALARM_ZS_LOW_BATTERY); }
static bool zone_battery_ok(void) { return zone_no_flag(ALARM_ZS_LOW_BATTERY); }
static bool zone_checked(void) { return zone_flag(ALARM_ZS_SUPERVISION); }
static bool zone_check_cleared(void) { return zone_no_flag(ALARM_ZS_SUPERVISION); }
static bool radio_trouble_on(void) { return host_panel_sensor_radio_trouble(); }
static bool radio_trouble_off(void) { return !host_panel_sensor_radio_trouble(); }

/* The panel's event log, as the Activity list reads it: the event is there (network.md §15). */
static bool logged(alarm_event_type_t type)
{
    static alarm_event_t ev[64];
    ui_backend_t be;
    host_panel_backend(&be);
    const size_t n = be.recent_events(be.ctx, ev, sizeof ev / sizeof ev[0]);
    for (size_t i = 0; i < n; i++) {
        if (ev[i].type == type) {
            return true;
        }
    }
    return false;
}

/* The door sensor's page as the Z-Wave pages read it (ui_backend_t zw_device): its check-in now,
 * or the one it gets when it next wakes up. */
#define CHECK_IN_12H (12u * 3600u)
static uint16_t g_door_generation;

static uint32_t door_check_in(bool next)
{
    ui_backend_t be;
    ui_zw_device_page_t page;
    host_panel_backend(&be);
    if (!be.zw_device(be.ctx, host_panel_zwave_door_node(), g_door_generation, &page)) {
        return 0u;
    }
    return next ? page.check_in_next_s : page.check_in_s;
}

static bool door_took_check_in(void)
{
    return door_check_in(false) == CHECK_IN_12H && door_check_in(true) == 0u;
}

/* The pages' radio row: "Working" (its network checked, not only the link up). */
static bool radio_working(void)
{
    ui_backend_t be;
    ui_zw_status_t st;
    host_panel_backend(&be);
    return be.zw_status(be.ctx, &st) && st.radio == UI_ZW_RADIO_WORKING;
}

int main(void)
{
    host_panel_init(1789857660000LL);
    expect(run_until(link_back_up, 5000u), "the ZAC93 comes up (start-up handshake)");

    /* Add a secure S2 door sensor: it joins, asks for its PIN, then interviews. */
    host_panel_zwave_add_secure();
    expect(run_until(pin_pending, 30000u), "the door sensor asks for its PIN");
    host_panel_zwave_pin();
    expect(run_until(door_ready, 5u * MIN_MS), "the door sensor's interview finishes");
    const uint16_t door = host_panel_zwave_door_node();
    expect(door != 0u, "the door sensor has a node number");
    expect(logged(ALARM_EV_ZWAVE_ADDED), "the Activity says a Z-Wave device was added");

    /* Add an insecure motion sensor: no PIN, its own node number. */
    host_panel_zwave_add_insecure();
    expect(run_until(motion_ready, 5u * MIN_MS), "the motion sensor joins and interviews, no PIN");
    const uint16_t motion = host_panel_zwave_motion_node();
    expect(motion != 0u && motion != door, "the motion sensor has its own node number");
    expect(logged(ALARM_EV_ZWAVE_ADDED_INSECURE),
           "the Activity says one was added without encryption");

    /* Bind the door's contact input to a zone (Z7's source picker, stood in for): the bridge and
     * zsrc_zwave must have offered it as a candidate. */
    expect(host_panel_zwave_bind_door(ZONE), "the door's contact input binds to zone 9");
    /* zones.md §3 "Fresh state": binding a watched input asks the device afresh (still awake from
     * its own interview); let that one-time refresh's own Notification/Battery Gets and answers
     * settle before sending reports of our own, so they are not overwritten by the refresh's. */
    run_for(5000u);

    /* Open/close reach the bound zone for real; motion's report merely goes out (unbound). */
    host_panel_zwave_door(true);
    expect(run_until(zone_faulted, 10000u), "zone 9 faults when the door opens");
    host_panel_zwave_door(false);
    expect(run_until(zone_restored, 10000u), "zone 9 restores when the door closes");
    host_panel_zwave_motion(true);
    expect(sent_ok(), "motion report sent (not bound to a zone)");

    /* Tamper and its restore. */
    host_panel_zwave_tamper(true);
    expect(run_until(zone_tampered, 10000u), "zone 9 tampers on the door sensor's cover");
    host_panel_zwave_tamper(false);
    expect(run_until(zone_tamper_restored, 10000u), "zone 9's tamper restores");

    /* Low battery and its restore. */
    host_panel_zwave_battery(true);
    expect(run_until(zone_low_battery, 10000u), "zone 9 shows low battery");
    host_panel_zwave_battery(false);
    expect(run_until(zone_battery_ok, 10000u), "zone 9's low battery restores");

    /* Fall asleep and miss check-ins: the door's negotiated Wake Up interval is the installer's
     * check-in (ZW_NET_CHECK_IN_S, 1 h, none configured) clamped into the device's own limits;
     * zw_health says UNREACHABLE T + 10 min after that (network.md), and the bound zone shows
     * CHECK its own grace later again (zones.md ZSRC_ZW_GRACE_S, the same 1 h + 10 min from the
     * device's last frame) - to the millisecond in the library's own tests; generous budgets
     * here, this only proves the simulator's wiring reaches both. */
    host_panel_zwave_sleep_miss(true);
    expect(run_until_slow(door_unreachable, 100u * MIN_MS), "missing check-ins goes UNREACHABLE");
    expect(run_until_slow(zone_checked, 150u * MIN_MS), "zone 9 shows CHECK after its own grace");
    host_panel_zwave_sleep_miss(false);
    expect(run_until_slow(door_reachable_again, 100u * MIN_MS),
           "it wakes by itself and is heard from again");
    expect(run_until(zone_check_cleared, 30000u), "zone 9's CHECK clears too");

    /* Pull the ZAC93, then plug it back in: the link goes down (at the next command - the
     * motion sensor's listening-device ping, at most 15 min after its last frame), one
     * sensor_radio trouble after the source's own grace (zones.md ZSRC_ZW_GRACE_S, the same
     * ~70 min as a zone's CHECK - not a zone CHECK itself, ZSRC-015), and it recovers once the
     * radio is back and a device is heard from again. */
    host_panel_zwave_radio(false);
    expect(run_until_slow(link_down, 25u * MIN_MS), "pulling the radio takes the link down");
    expect(run_until_slow(radio_trouble_on, 90u * MIN_MS), "exactly one sensor_radio trouble appears");
    host_panel_zwave_radio(true);
    expect(run_until(link_back_up, 10u * MIN_MS), "plugging it back in brings the link up");
    host_panel_zwave_door(true); /* proof of life: heard again, besides the automatic re-ask */
    run_for(3000u);
    host_panel_zwave_door(false);
    expect(run_until(radio_trouble_off, 30000u), "the sensor_radio trouble clears");

    /* A panel restart: the stored node cache and keys survive it (network.md "Persistence") -
     * the radio and the sensors themselves were never reset, only the panel's own zw_net_t. */
    host_panel_zwave_restart();
    expect(run_until(link_back_up, 5000u), "the panel restarts and the link comes back");
    expect(host_panel_zwave_door_known(), "the door sensor is still in the stored cache");

    /* A device's own check-in, chosen on its page in installer mode (screens.md §6): kept in what
     * the panel stores, so a restart before the device wakes does not lose it; the device takes it
     * at its next wake-up (network.md §7). */
    expect(run_until(radio_working, 5000u), "the pages see the radio working again");
    {
        ui_backend_t be;
        host_panel_backend(&be);
        uint8_t user = 0u;
        expect(be.program_enter(be.ctx, "69420", &user) == ALARM_OK, "installer mode opens");
        ui_zw_device_t list[8];
        const size_t n = be.zw_devices(be.ctx, list, 8u);
        for (size_t i = 0; i < n; i++) {
            if (list[i].node == door) {
                g_door_generation = list[i].generation;
            }
        }
        ui_zw_request_t req;
        memset(&req, 0, sizeof req);
        req.kind = UI_ZW_REQ_CHECK_IN;
        req.user = user;
        req.node = door;
        req.generation = g_door_generation;
        req.seconds = CHECK_IN_12H;
        expect(be.zw_request(be.ctx, &req) == UI_ZW_REQ_OK,
               "the door sensor's page takes a check-in of its own");
    }
    expect(door_check_in(true) == CHECK_IN_12H, "its page says it changes when it next wakes up");
    host_panel_zwave_restart();
    expect(run_until(radio_working, 5000u), "the panel restarts before the sensor wakes");
    expect(door_check_in(true) == CHECK_IN_12H, "the stored check-in is still to come after it");
    expect(run_until_slow(door_took_check_in, 90u * MIN_MS),
           "the door sensor takes it at its next wake-up");

    printf("\n%d check(s) failed\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

#endif /* ALARM_SIM_ZWAVE */
