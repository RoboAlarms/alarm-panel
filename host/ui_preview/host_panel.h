/*
 * host_panel.h - the alarm engine running in-process for the UI preview, exposed to the UI
 * as a ui_backend_t (same contract as the panel's alarm_service), with the real event log on a
 * RAM "flash" and a simulated Wi-Fi radio.
 */
#pragma once

#include "alarm/alarm_engine.h"
#include "alarm_proto/proto_prefs.h"
#include "host_features.h"
#include "alarm/alarm_snapshot.h"
#include "ui_app.h"
#include "voice_render.h"
#include "power_monitor.h"
#include "sounder/sounder_mix.h"
#include "automation/auto_run.h"
#include "automation/auto_store.h"
#include "automation/automation.h"

void host_panel_init(int64_t epoch_ms_at_start);
void host_panel_backend(ui_backend_t *out);

/* Simulated time (ms) and inputs; each call refreshes the snapshot when something changed. */
void host_panel_advance(alarm_ms_t ms);
void host_panel_zone(uint16_t zone, alarm_zone_event_t ev); /* the zone's sensor does ev */
/* A wired terminal's loop: open (1) or closed (0); the wired source sees it on its next poll. */
void host_panel_wire(uint8_t gpio, bool open);
void host_panel_trouble(alarm_sys_trouble_t trouble, bool active);
void host_panel_set_power(ui_battery_state_t state, int percent, bool low);
void host_panel_set_charging(bool charging);
void host_panel_set_net(ui_net_state_t state, int rssi);

/* A command from outside this keypad (Home Assistant, a phone): arms while the UI looks on. */
alarm_result_t host_panel_remote_command(const alarm_cmd_t *cmd);
/* The same from the browser Panel Manager (its own source, docs/features/31 and 40 WALK-023). */
alarm_result_t host_panel_manager_command(const alarm_cmd_t *cmd);
/* Installer mode entered (true) or left at another keypad, device 2 (B008). */
alarm_result_t host_panel_keypad2_program(bool on);

/* The backlight the touch UI last asked for, in percent; -1 before it asked (UI-043). */
int host_panel_backlight(void);

/* The keypad's preferences as the touch UI last saved them (backend.save_options): false before
 * it saved any. The preview's stand-in for the panel's NVS "ui" store (BL-132). */
bool host_panel_saved_options(ui_options_t *out);
/* How many times the touch UI saved them since the program started. */
unsigned host_panel_options_saves(void);

/* A change of the Screen and sound settings from elsewhere (docs/features/07 UI-046): Home
 * Assistant or the master's link, as the panel's panel_prefs_remote_submit / _cancel hand it to
 * the screen (the same mailbox, ui_prefs.h, on the simulated clock). patch->have names the fields.
 * Returns the ticket, 0 while another is under way (BUSY). done runs inside lv_timer_handler (the
 * screen's task) at most once per ticket: why[id - 1] a PROTO_PREFS_WHY_*, 0 = applied; a patch
 * not taken within 3 s is answered BUSY when the screen next looks. cancel: true when withdrawn
 * before the screen took it (no done will come). */
typedef void (*host_panel_prefs_done_fn)(void *ctx, uint32_t ticket,
                                         const uint8_t why[PROTO_PREFS_F_LAST]);
uint32_t host_panel_prefs_submit(const proto_prefs_values_t *patch, host_panel_prefs_done_fn done,
                                 void *ctx);
bool host_panel_prefs_cancel(uint32_t ticket);

/* Forget the saved network (the setup starts from scratch). */
void host_panel_forget_wifi(void);

/* The simulated house network. Its router is 192.168.1.1 on 255.255.255.0 and it leases
 * 192.168.1.42 to the panel; a fixed address on another network can't reach the router, the
 * same as at home. Each fault below can be switched on to see what the connection check
 * (docs/features/09 NET-012) says about it, without breaking anything real. */
typedef enum {
    HOST_NET_WIFI_DOWN = 0, /* the access point stops answering */
    HOST_NET_NO_LEASE,      /* the router hands out no address (DHCP) */
    HOST_NET_ROUTER_DOWN,   /* the router ignores pings */
    HOST_NET_DNS_DOWN,      /* name lookups fail */
    HOST_NET_INTERNET_DOWN, /* nothing out on the internet answers */
    HOST_NET_TIME_DOWN,     /* the time server doesn't answer */
    HOST_NET_SERVICE_DOWN,  /* the broker doesn't answer */
    HOST_NET_FAULT_COUNT,
} host_net_fault_t;

void host_panel_net_fault(host_net_fault_t fault, bool on);
bool host_panel_net_faulty(host_net_fault_t fault);
void host_panel_net_clear_faults(void);

/* The event log's latched loss (LOG-010): events never saved, alerts never kept. 0, 0 = none. */
void host_panel_log_loss(uint32_t records, uint32_t reports);
/* The simulated firmware update state (Settings > About). */
void host_panel_fw_preset(const ui_fw_status_t *st);
/* An update's stage logged now (ALARM_EV_FIRMWARE_UPDATE, alarm_fw_stage_t), as the firmware logs
 * it: the panel's own (keypad 0), or a keypad's through the panel link (keypad 1..8, its stages
 * ALARM_FW_KEYPAD_*; why: ALARM_FW_KEYPAD_FAILED's reason, the link's why table). */
void host_panel_fw_event(uint8_t stage, uint8_t keypad, uint8_t why);
/* BL-186: the update server in use (a fictional one: example.net), and whether RoboAlarms' key is
 * trusted. A check of example.net answers OK with an untrusted key "Example Builds" after a moment,
 * example.org NOT_A_SERVER, example.com NO_FIRMWARE, anything else UNREACHABLE. */
void host_panel_fw_server_preset(const char *domain, bool official, bool robo_trusted);
/* A pretend panel with language packs installed (host_panel.c, "languages"); off: the built-in
 * catalogs answer as if installed. Either way the language generation changes. */
void host_panel_lang_world(bool on);
/* With the language world on: a ready el-GR with a Greek body font (I18N-049). */
void host_panel_lang_greek(bool on);
/* The letters pack (docs/features/39 OPT-010): on by default, the layout's committed parts; off,
 * a panel without it. Either way the asset generation changes. */
void host_panel_letters(bool on);
/* Renames a zone in the running world (UTF-8). */
void host_panel_zone_name(uint16_t zone, const char *name);

/* The simulated Home Assistant connection, set up directly (tls: ui_mqtt_tls_t). */
void host_panel_mqtt_preset(const char *host, const char *user, const char *password,
                            uint8_t tls, bool enabled, uint32_t sent);

/* The pretend Home Assistant answers an open pairing window: both screens show 588460
 * (golden vector 1) and the page waits for Allow (docs/features/23 HAI-003). */
void host_panel_ha_pair_request(void);

/* Once paired it shares three demo sensors (binary_sensor.porch_door, .garage_motion,
 * .attic_window); this pushes one of their states, like the integration would (M13). */
void host_panel_ha_entity(const char *entity_id, const char *state, bool available);

/* The pretend ESP32-H2 radio and its sensors (docs/features/20). The module is plugged in from the
 * start; pairing joins a Frient contact and a motion sensor into the learn window, exactly as a
 * real sensor joins when its button is held, and they can then be bound to zones in the zone
 * editor like any other source. The rest drive the contact. */
void host_panel_zigbee_pair(void);
bool host_panel_zigbee_paired(void);
void host_panel_zigbee_radio(bool present); /* the module pulled out of its socket */
void host_panel_zigbee_jam(bool on);        /* the channel drowned out (ZB-006) */
void host_panel_zigbee_open(bool open);
void host_panel_zigbee_tamper(bool on);
void host_panel_zigbee_battery(bool low);
void host_panel_zigbee_leave(void); /* the sensor leaves the network for good */

/* The panel's real Zigbee (host_zigbee_real.h; BL-143 ZB-M6, docs/features/20 testing.md §10):
 * zbz_panel, its zone source and the fake radio's seven virtual devices behind Installer > Zigbee
 * and the zones, instead of the demo model and the pretend radio above. Chosen before
 * host_panel_init() and kept by it: the simulator runs it, the preview's walk only in its own
 * segment (the demo's shots stay as they are). With it on, the calls above drive its door
 * contact; pair presses the next virtual device's pairing button (Add a device must be open:
 * two press theirs by themselves when it opens). */
void host_panel_zigbee_use_real(bool on);
bool host_panel_zigbee_is_real(void);
const char *host_panel_zigbee_press(void); /* the device that pressed its button, NULL none */
/* A virtual device's sensor (host_zbr_dev_t: 0 door, 1 motion, 2 smoke, 3 water). */
bool host_panel_zigbee_trip(uint8_t which, bool on);
void host_panel_zigbee_restart(void); /* the panel restarts: zbz_panel from what it stored */

/* The pretend CC1101 receiver and one 433 MHz contact (docs/features/25): device A2B3C sends
 * code 5 when it opens, E when it closes, A when its cover comes off, 3 when its battery is
 * flat - learned in the zone editor by triggering it, like a real one. */
#define HOST_RF_DEMO_DEVICE 0xA2B3Cu
/* The battery-backed clock module (docs/features/35): UI_CLOCK_MODULE_NONE (the default) and the
 * others, and whether it had lost the time at start-up. */
void host_panel_clock_module(ui_clock_module_t state, bool lost_at_start);
void host_panel_rf_receiver(bool present); /* the module answers on its bus, or doesn't */
void host_panel_rf_open(bool open);
void host_panel_rf_tamper(void);
void host_panel_rf_battery(void);
void host_panel_rf_jam(bool on); /* a carrier sits on the band (RF-005) */
#define HOST_RF_NEIGHBOUR_DEVICE 0x0BEEFu
void host_panel_rf_neighbour(void); /* a neighbour's remote, pressed once (RF-010) */
void host_panel_rf_glitch(void);    /* one corrupted frame of the demo contact (RF-009) */

/* The Z-Wave world (docs/features/36 Z6, Z7): the panel's own Z-Wave as the firmware runs it - the
 * zwz_panel_t of components/core/zwave_zones, which zwave_service owns on a panel - against the
 * fake ZAC93 and four simulated devices, off by default - on with `pwsh tools/ui_sim.ps1 -ZWave`
 * or `pwsh tools/ui_preview.ps1 -ZWave` (-DALARM_SIM_ZWAVE=ON), whose walk starts it only after
 * its other shots (host_panel_zwave_later), so those stay the same. What it stores is kept in
 * memory (the simulator's NVS) and what it logs goes into the engine (the Activity list). */
#ifndef ALARM_SIM_ZWAVE
#define ALARM_SIM_ZWAVE 0
#endif

#if ALARM_SIM_ZWAVE
/* The preview (host/ui_preview): the world waits for host_panel_zwave_begin(), so the walk's
 * first shots are those of a panel without Z-Wave (no Z-Wave zone, source or pages). Call before
 * host_panel_init(); the simulator and zwave_smoke start with the world running. */
void host_panel_zwave_later(void);
void host_panel_zwave_begin(void);

/* "Add a device": the request the pages make, then the simulated device's own button. The Z-Wave
 * pages ask for it through ui_backend_t's zw_request themselves; this is the simulator's button,
 * which answers the security question as an installer would (allowing what the device asks for)
 * and leaves the PIN to host_panel_zwave_pin(). */
void host_panel_zwave_add_secure(void);   /* a sleeping, S2 Authenticated door sensor */
void host_panel_zwave_add_insecure(void); /* a mains, unencrypted motion sensor */
void host_panel_zwave_pin(void); /* the installer reads the joining device's label, types it back */

/* Z7b: two more devices, in range but not yet joined - for exercising the add pages themselves
 * (screens.md §4) end to end. Press only once the pages' own "Add a device" is listening. */
void host_panel_zwave_press_spare_secure(void);   /* another sleeping S2 sensor */
void host_panel_zwave_press_spare_insecure(void); /* a ZSE43 tilt sensor: battery, no Security 2 */
uint16_t host_panel_zwave_spare_secure_pin(void); /* its label's PIN, to type back */
void host_panel_zwave_wake_spare_insecure(void);  /* the ZSE43's button: it wakes up, checks in */
void host_panel_zwave_press_door_remove(void);    /* its button, for an exclusion */
/* ZW-045: the module offers this region too, from its next start-up (a region on another band,
 * for the region page's "devices stop working"). */
void host_panel_zwave_offer_region(uint8_t code);
/* ZW-028/029 (docs/features/36): a keypad woken by the panel's call, on batteries, S2 with its
 * PIN (its sounds and lights can show the panel's state); and a device that joins with an empty
 * node information, then hears nothing (reset, or gone). Press once the pages listen. */
void host_panel_zwave_press_keypad(void);
uint16_t host_panel_zwave_keypad_pin(void);
void host_panel_zwave_press_stray(void);
void host_panel_zwave_keypad_silent(bool silent);   /* its power gone: it hears nothing */
void host_panel_zwave_keypad_lose_replies(unsigned n); /* its next n answers lost */
void host_panel_zwave_fw_offline(bool offline);
void host_panel_zwave_status_force(unsigned what); /* BL-152: 1 jammed, 2 not answering, 0 as is */
void host_panel_zwave_wake_door(void);            /* its button: it wakes up */
void host_panel_zwave_press_light(void);          /* ZW-034: the light switch's pairing button */
void host_panel_zwave_press_relay(void);          /* ZW-035: the relay kit's pairing button */
void host_panel_zwave_light_toggle(void);         /* the light switch pressed by hand */
void host_panel_zwave_tilt(bool open);            /* the garage door moves (its tilt sensor) */
uint8_t host_panel_zwave_relay_level(uint8_t ep); /* the relay kit's relay, as it is */
uint32_t host_panel_zwave_relay_sets(void);       /* Sets the relay kit took */
uint16_t host_panel_zwave_light_pin(void);        /* their labels' PINs */
void host_panel_zwave_press_dimmer(void);         /* ZW-034: a dimmer's pairing button */
uint16_t host_panel_zwave_dimmer_pin(void);
uint8_t host_panel_zwave_dimmer_level(void);      /* its level as it is (0..0x63) */
void host_panel_zwave_smart_power(void);          /* ZW-024: the SmartStart sensor powered */
void host_panel_zwave_smart_key(char *out, size_t cap); /* its 40-digit key, as printed */
bool host_panel_zwave_smart_joined(void);
void host_panel_zwave_press_smart_remove(void);   /* its button, for an exclusion */
void host_panel_zwave_noise(int dbm);             /* §9: every channel's background noise */
void host_panel_zwave_keypad_jam_report(void);    /* §9: the keypad reports "RF jamming detected" */
void host_panel_zwave_break_light(void);          /* §10: the light switch breaks */
void host_panel_zwave_press_spare_light(void);    /* its replacement's pairing button */
uint16_t host_panel_zwave_spare_light_pin(void);
uint32_t host_panel_zwave_fake_overflow(void); /* 0: the simulated radio had room */
uint16_t host_panel_zwave_relay_pin(void);    /* the update service can't be reached */
void host_panel_zwave_radio_keeps_failed(bool keeps); /* the radio never counts one as failed */
uint16_t host_panel_zwave_stray_node(void);
uint16_t host_panel_zwave_keypad_node(void);
bool host_panel_zwave_in_radio(uint16_t node);      /* in the radio's own node list */
bool host_panel_zwave_in_panel(uint16_t node);      /* in the panel's node cache */
unsigned host_panel_zwave_keypad_indicators(uint8_t *last_id); /* Indicator Sets it got */
/* Its buttons: `code` then an Entry Control event (ZW_ENTRY_EVENT_ARM_AWAY, _ARM_HOME,
 * _DISARM_ALL; an emergency with code NULL); the engine's answer once it ran. */
void host_panel_zwave_keypad_keys(const char *code, uint8_t event);
alarm_result_t host_panel_zwave_keypad_last(void);

void host_panel_zwave_door(bool open);
void host_panel_zwave_motion(bool open);
void host_panel_zwave_tamper(bool on);     /* the door sensor's cover (Home Security) */
void host_panel_zwave_battery(bool low);  /* the door sensor's battery */
void host_panel_zwave_sleep_miss(bool on);/* the door sensor stops waking itself up to check in */
void host_panel_zwave_radio(bool present);/* pull the ZAC93 out of its socket, or plug it back */
/* TODO(Z-Wave phase 2): a "channel jammed" fault - network.md has no jam detector yet (Z6
 * README, "Skip jamming"). */
void host_panel_zwave_restart(void);      /* the panel restarts: a fresh zwz_panel from what it
                                           * stored (keys, gen, nodes, cfg, chk), as
                                           * zwave_service_start builds it (network.md §2) */

uint32_t host_panel_zwave_seq(void);      /* bumps whenever status changes: for the log strip */
const char *host_panel_zwave_status(void);

/* For proving the flows without a window (zwave_smoke.c): precise state the log strip only
 * describes in prose, read from what the pages see. 0/false before the fact. */
uint16_t host_panel_zwave_door_node(void);
uint16_t host_panel_zwave_motion_node(void);
bool host_panel_zwave_door_ready(void);       /* its interview finished (its page: set up) */
bool host_panel_zwave_motion_ready(void);
bool host_panel_zwave_door_unreachable(void); /* missed its check-ins (its page says so) */
bool host_panel_zwave_link_up(void);          /* the ZAC93 is answering */
bool host_panel_zwave_pin_pending(void);      /* waiting for host_panel_zwave_pin() */
bool host_panel_zwave_door_known(void);       /* still in the panel's node cache */

/* "Bind the Z-Wave door to zone N": the door's contact input, through the hub's own candidate
 * list and binding call, as the source picker does. False: not joined yet, or no candidate. */
bool host_panel_zwave_bind_door(uint16_t zone);

/* The zone outcomes a binding produces, for zwave_smoke.c: the real alarm engine's zone status
 * (ALARM_ZS_FAULT/TAMPER/LOW_BATTERY/SUPERVISION - the last is CHECK) and the source's own
 * sensor_radio trouble (ALARM_TRBL_SENSOR_RADIO), exactly what the zone and Troubles pages show. */
uint16_t host_panel_zone_flags(uint16_t zone);
bool host_panel_sensor_radio_trouble(void);
#endif

/* The keypad sounder (docs/features/08). The panel wires exactly these four things
 * (firmware/crowpanel_p4/main/app_main.c and components/ui/alarm_ui/port/ui_port_esp.c): the
 * continuous pattern from every status snapshot, one-shot sounds from the engine's on_sound
 * port, the touch UI's key clicks and error beeps, and the volume from Settings > Display and
 * sound. Leave it unset - as the scripted preview does - and the panel is silent; the
 * interactive simulator points it at the PC's sound card. */
typedef struct {
    void *ctx;
    void (*continuous)(void *ctx, alarm_sound_t sound); /* partition 1's status "sounder" */
    void (*one_shot)(void *ctx, alarm_sound_t sound);   /* ack, error, chime, walk test */
    void (*key_click)(void *ctx);
    void (*volume)(void *ctx, uint8_t percent);       /* Beeps (AUD-032) */
    void (*chime_volume)(void *ctx, uint8_t percent); /* the door chime alone; NULL: none */
    void (*voice_volume)(void *ctx, uint8_t percent); /* speech alone; NULL: none */
    /* The voice (docs/features/08 voice-first-build.md D11, D15; all NULL: the preview's own
     * virtual sound card, which renders WAV files and a trace instead - voice_out below).
     * tone: an engine one-shot with the policy's ticket (snd_play_tracked), used instead of
     * one_shot; ticket: the next tracked tone's end; speak: start utterance `id` over the items
     * (the pack stays valid); speech_cancel; speech_done: an utterance's end. */
    bool (*tone)(void *ctx, alarm_sound_t sound, uint32_t ticket);
    bool (*ticket)(void *ctx, uint32_t *ticket, bool *played);
    bool (*speak)(void *ctx, const vp_pack_t *pack, const vr_item_t *items, size_t n, uint32_t id);
    void (*speech_cancel)(void *ctx);
    bool (*speech_done)(void *ctx, uint32_t *id, bool *completed);
    /* speech is queued or playing (ann_due): the card's mixer holds the exit beeps for it
     * (snd_set_speech_due, AUD-034), as the panel's sounder task does; NULL: nothing to tell */
    void (*speech_due)(void *ctx, bool due);
    /* the power policy's result (power_saving_update): the card's mixer drops key clicks and
     * the chime (snd_set_power_saving); NULL: nothing to tell */
    void (*power_saving)(void *ctx, bool saving);
} host_panel_sound_t;

void host_panel_sound(const host_panel_sound_t *snd); /* NULL: silent again */

/* The pretend notification services (docs/features/26-28). provider: ui_notify_provider_t.
 * preset() switches one on with credentials that pass, optionally as if it had already sent
 * something; fault() makes it refuse, so the pages' failure wording can be seen. */
void host_panel_notify_preset(uint8_t provider, bool enabled, bool sent_once);
void host_panel_notify_fault(uint8_t provider, bool on);

void host_panel_storage_preset(storage_state_t state, uint64_t capacity, uint64_t free_bytes);
void host_panel_storage_fail(bool fail);
unsigned host_panel_storage_formats(void);

/* Panel Manager sign-ins (docs/features/31): was this user's password set by somebody else? */
bool host_panel_web_login_temporary(uint8_t user);

/* Smart Home (docs/features/24, BL-045): the fictional house's devices. on false: a panel with no
 * device at all (the rail has no Smart Home). garage_closing: the garage door was told to close
 * elsewhere and is on its way (HCTL-006). host_panel_device: one device as the world has it now. */
void host_panel_devices(bool on);
void host_panel_devices_garage_closing(void);
const ui_dev_t *host_panel_device(const char *ref);
/* The Smart Home layout as stored (dev_layout_set, HCTL-008): its bytes into out (max), the length. */
size_t host_panel_dev_layout(uint8_t *out, size_t max);
/* The automation editors' shots and tests (docs/features/41): on adds a script (never offered to
 * an automation, AUT-002) and a lock and a garage door when the house lacks them, at the end of
 * the list; off takes them away again. Off by default, so Smart Home's shots don't change. */
void host_panel_devices_extra(bool on);
/* A thermostat that holds a range (Home Assistant's heat_cool: no single set point), in the Kitchen,
 * off Favorites: on adds it, off takes it away. Off by default, so Smart Home's shots don't change. */
void host_panel_devices_range_thermostat(bool on);
/* What a device can't do, as its source says (BL-171), in the Living room, off Favorites: a fan
 * without speeds (Ceiling Fan), blinds that can't stop, opening (Blinds), and a thermostat that holds
 * a range (Upstairs; the range thermostat above goes first). off takes them away. Off by default, so
 * Smart Home's shots don't change. */
void host_panel_devices_limits(bool on);
/* A house of many rooms (UI-020, BL-171): a light, off, in each of four more rooms, off
 * Favorites, so the area tabs reach the layout's widest. off takes them away. Off by default, so
 * Smart Home's shots don't change. */
void host_panel_devices_rooms(bool on);
/* A device's last action failed (HCTL-006): its status and action, as the source reports them.
 * failed: refused, it didn't happen; unconfirmed: no answer came or the link dropped with it in
 * flight, so whether it happened isn't known (HCTL-012, ui_dev_t.unsure). settle: done, idle again.
 * rename: the name its source gives it (a long one for the note's shortening, BL-171). */
void host_panel_device_failed(const char *ref, uint8_t action);
void host_panel_device_unconfirmed(const char *ref, uint8_t action);
void host_panel_device_settle(const char *ref);
/* An action under way (HCTL-006): its status pending and its action, as the source reports them;
 * the state stays the one reported until settle (a mode asked for isn't the mode, BL-171). */
void host_panel_device_pending(const char *ref, uint8_t action);
/* The device's source can't reach it (HCTL-006): offline since since_ms, its state and values kept
 * as last reported; host_panel_device_settle brings it back (BL-181). */
void host_panel_device_offline(const char *ref, int64_t since_ms);
/* No device can be reached (HCTL-006, LINK-046: Home Assistant's link dropped; on a link keypad,
 * its alarm panel's device data stopped): on, every device offline since since_ms; off, each back
 * as it was (BL-216). */
void host_panel_devices_unreachable(bool on, int64_t since_ms);
/* How many actions the screen has asked of any device so far (ui_backend_t dev_act), refused ones
 * included: a check that a tap sent nothing (BL-181). */
unsigned host_panel_device_acts(void);
void host_panel_device_rename(const char *ref, const char *name);
/* A room renamed at its source: every device in area `from` is now in `to` (a long room name for
 * the open tab beside the note, BL-171). */
void host_panel_devices_area_rename(const char *from, const char *to);
/* A device's state as its source reports it (a ui_dev_state_t: a cover opening or closing, set
 * elsewhere, BL-179); only the state, its status and level as they are. */
void host_panel_device_state(const char *ref, uint8_t state);
/* A thermostat's source says what it is doing (hvac_action, a dev_hvac_t; DEV_HVAC_UNKNOWN for
 * nothing) and the room's temperature (hundredths; UI_DEV_TEMP_UNKNOWN for none) (BL-171). */
void host_panel_device_climate(const char *ref, uint8_t hvac_action, int32_t temperature);
/* A device's since_ms ("Ran at", "Offline since", HCTL-006): sets it, returns the one it had. */
int64_t host_panel_device_since(const char *ref, int64_t since_ms);
/* A thermostat's set point (hundredths; UI_DEV_TEMP_UNKNOWN while it holds a range, so its tile
 * hides its stepper, BL-184): sets it, returns the one it had. */
int32_t host_panel_device_setpoint(const char *ref, int32_t setpoint);

/* Automations (docs/features/41 U12, U13): the real core runs beside the engine, on the devices
 * above; the rules live in a real auto_store_t over a RAM key-value port, seeded at
 * host_panel_init through auto_store_save with Harbor House's automations (uids and slots 1..4).
 * ui_backend_t's auto_* hooks (host_panel_backend) are this store.
 * host_panel_automations(false): none, for walks that need a house nothing acts on by itself
 * (true seeds them again; the next host_panel_init does too).
 * host_panel_auto_status: an automation's last outcome (AUTO_RES_NONE for an unknown uid);
 * host_panel_auto_rules: the automations as the runner has them, up to max; counts: what the
 * device port sent and what it refused under what an automation may do (hc_auto_allowed with
 * the installer option, AUT-002, AUT-015).
 * host_panel_auto_add_unchecked: TEST-ONLY - a test's own rule after the seeded ones (the next
 * uid and slot; made_by as given), handed to the runner at once WITHOUT auto_rule_check or the
 * lock, so a test can hold a rule a save would now refuse (one saved before the installer turned
 * the option off). Screens never call it: they save through host_panel_auto_save. */
typedef struct {
    unsigned sent;
    unsigned refused;
} host_panel_auto_counts_t;

void host_panel_automations(bool on);
bool host_panel_automations_active(void);
void host_panel_auto_status(uint16_t uid, auto_status_t *out);
size_t host_panel_auto_rules(auto_rule_t *out, size_t max);
void host_panel_auto_counts(host_panel_auto_counts_t *out);
bool host_panel_auto_add_unchecked(const auto_rule_t *r);
/* The validator (AUT-002) against the simulated panel's configuration now. */
auto_check_t host_panel_auto_check(const auto_rule_t *r);
/* The panel's edits as automation_service answers them (AUT-010, AUT-015): AUTO_SAVE_LOCKED while
 * the simulated panel is locked (armed or in alarm, alarm_snapshot_locked on the latest
 * snapshot); a save is checked with host_panel_auto_check (INVALID, *why) and makes `user` the
 * maker; uid 0 adds (FULL past AUTO_MAX), another uid changes that OK rule (NOT_FOUND,
 * NOT_EDITABLE); expected_gen must be host_panel_auto_gen() (CHANGED). Each change moves gen. */
uint16_t host_panel_auto_gen(void);
auto_save_t host_panel_auto_save(const auto_rule_t *r, uint8_t user, uint16_t expected_gen,
                                 auto_check_t *why, uint16_t *uid_out);
auto_save_t host_panel_auto_delete(uint16_t uid, uint16_t expected_gen);
auto_save_t host_panel_auto_set_off(uint16_t uid, bool off, uint16_t expected_gen);
auto_save_t host_panel_auto_delete_slot(uint8_t slot, uint16_t expected_gen);

/* TEST-ONLY controls of the store (screens never call them):
 * - read_only: the store as if it loaded read-only (every edit READ_ONLY) until turned back off;
 * - store_fail: every write and erase of its key-value port refused (an edit answers FAILED);
 * - inject: the rule into the store as given - any rec (NEWER, BROKEN, BAD), its uid (0: the next
 *   one) and slot (0 or a used one: the first empty) - without the validator, a write or a new gen;
 *   false when the store is full;
 * - force_status: what the backend's auto_status answers for that uid from now on (NULL: the
 *   runner's own again);
 * - less_secure: the installer option "Allow less secure automations" (AUT-015), saved as the
 *   installer's page saves the configuration. */
void host_panel_auto_read_only(bool on);
void host_panel_auto_store_fail(bool on);
bool host_panel_auto_inject(const auto_rule_t *r);
void host_panel_auto_force_status(uint16_t uid, const auto_status_t *st);
void host_panel_auto_less_secure(bool on);
/* TEST-ONLY: the automation service answers "busy" (as while it saves) until set false again;
 * the runner stands still meanwhile. */
void host_panel_auto_busy(bool on);

/* The voice (docs/features/08 voice-first-build.md): the real en-US pack, packed at build time.
 * pretend: show the voice as this ui_voice_state_t (UI_VOICE_NONE, _LOADING, _UNAVAILABLE...)
 * from now on, whatever the pack is; -1 = the pack's own state again. The environment variable
 * ALARM_PREVIEW_VOICE=none|loading|unavailable sets it at start. leases: packs acquired and not
 * released (a screen that leaks one shows here). */
void host_panel_voice_pretend(int state);
unsigned host_panel_voice_leases(void);
/* TEST-ONLY: the pack's gap between a phrase's words from now on, in ms (0: the pack's own).
 * A gap no pack carries (a header allows 0-100) stands in for long words: no word of the tracked
 * voice is long enough for eight of them to pass 10 s (D02). Speech already queued keeps its own. */
void host_panel_voice_word_gap(uint16_t ms);
/* The simulator's audio hook (a later unit): Preview hands the checked words to it and answers
 * what it does (a ui_voice_preview_t: OK, or QUIET at volume 0, on battery, in an alarm). NULL:
 * the trace line only. */
typedef uint8_t (*host_voice_player_t)(void *ctx, const vp_pack_t *pack, const uint16_t *ids,
                                       size_t n);
void host_panel_voice_player(host_voice_player_t play, void *ctx);

/* The voice policy (components/core/annunciator) wired as the panel wires it (panel_app.c's
 * listener, panel_voice): engine events, sounds, the bell and every snapshot go to it, the zone
 * words come from the configuration, Preview goes through ann_preview. Utterances play through
 * host_panel_sound()'s speak hook, or - with none - the preview's virtual sound card: virtual time,
 * the panel's mixer (snd_render_pcm: tones, volume, entry attenuation) rendered in 10 ms blocks, and
 * a WAV per utterance in the directory host_panel_voice_out() names (16-bit stereo 32 kHz, called
 * <n>-<kind>.wav) and one trace line, "voice: <kind> <zone> <clip ids> <total ms>", on stdout. */
void host_panel_voice_out(const char *dir); /* NULL: no WAV files */
/* The battery model the power policy reads (power_saving_update, 15 s of stable evidence for entry
 * and for restoration; unknown readings keep the state). Mains is the start: nothing is saved.
 * The result reaches the policy (no speech, no Preview), the virtual card's mixer and the
 * player's (host_panel_sound_t.power_saving). */
void host_panel_power_sample(const power_sample_t *sample);
bool host_panel_power_saving(void);
/* TEST-ONLY: the virtual card's mixer as the latest utterance's first block found it (tones,
 * volume, power saving, pattern): a test renders the same words through a copy of it and compares
 * with the WAV. false: no utterance yet, or a player of its own. */
bool host_panel_voice_mixer_at_start(snd_mixer_t *out);
void host_panel_voice_prefs(bool enabled, bool status, bool chime);

/* What the virtual card or the player was given, oldest first (the preview's checks). */
typedef struct {
    bool speech;           /* false: a tone (sound) */
    uint8_t kind;          /* speech: ann_kind_t */
    alarm_sound_t sound;   /* tone: the engine's one-shot */
    uint16_t zone;
    uint8_t count;
    uint16_t clips[8];
    uint16_t gap_ms[8];
    uint32_t total_32k;    /* vr_total_samples_32k */
    uint32_t wav_samples;  /* stereo frames written to the WAV (0: none written) */
    uint32_t id;           /* speech: the policy's utterance id */
    bool completed;        /* speech: played to its last sample */
    bool cancelled;        /* speech: stopped before that (an alarm, a new identity, power saving) */
    uint32_t wav_number;   /* the <n> in the file name, 0: none */
    uint64_t at_ms;
} host_voice_log_t;
unsigned host_panel_voice_log(host_voice_log_t *out, unsigned max); /* returns the count */
void host_panel_voice_log_clear(void);

/* The remote keypad (docs/features/31): paused with the banner? Remote access (the web server) on? */
bool host_panel_remote_paused(void);
bool host_panel_remote_web(void);
