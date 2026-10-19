#pragma once
/*
 * ui_backend.h - what the touch UI needs from the panel, as data and function pointers
 * (ADR-012; docs/features/33 step 5).
 *
 * The contract between the UI (components/ui/alarm_ui) and whatever runs it: the panel's
 * services on ESP-IDF (components/services/ui_port_esp), a panel link to a master for a remote
 * keypad (docs/features/11), or the simulated world of the PC preview and simulator
 * (host/ui_preview/host_panel.c). The browser Panel Manager (components/services/panel_web)
 * uses it too. Portable: no LVGL, no ESP-IDF. A NULL entry hides what depends on it.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_engine.h"
#include "alarm/alarm_snapshot.h"
#include "automation/auto_run.h"
#include "automation/auto_store.h"
#include "automation/automation.h"
#include "event_log/elog.h"
#include "home_control/device_source.h"
#include "i18n/i18n.h"
#include "notify/notify_gate.h"
#include "voice_pack/voice_pack.h"
#include "zone_hub/zone_hub.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    UI_NET_STARTING = 0,
    UI_NET_NO_RADIO,     /* Wi-Fi module not answering */
    UI_NET_UNCONFIGURED, /* no network saved */
    UI_NET_CONNECTING,
    UI_NET_CONNECTED,
    UI_NET_DISCONNECTED,
} ui_net_state_t;

typedef enum { UI_BATTERY_UNKNOWN, UI_BATTERY_ABSENT, UI_BATTERY_PRESENT } ui_battery_state_t;
typedef struct {
    ui_battery_state_t state;
    int percent; /* -1 unavailable */
    bool low;
    bool charging;
} ui_power_info_t;

typedef enum {
    UI_BEEP_KEY = 0, /* touch feedback */
    UI_BEEP_ERROR,   /* input rejected before it reached the engine */
    UI_BEEP_TEST,    /* volume preview */
} ui_beep_t;

typedef enum {
    UI_WIFI_FAIL_NONE = 0,
    UI_WIFI_FAIL_PASSWORD,  /* authentication failed: most likely the password */
    UI_WIFI_FAIL_NOT_FOUND, /* the network didn't answer */
    UI_WIFI_FAIL_OTHER,
} ui_wifi_fail_t;

typedef enum { UI_LANG_READY, UI_LANG_OTHER, UI_LANG_REFUSED, UI_LANG_DAMAGED } ui_lang_state_t;
/* A language installed on the panel (docs/features/37 language-packs.md); English is not one. */
typedef struct {
    char locale[16];   /* BCP 47 */
    char name[32];     /* in itself: "Espanol" with its tilde */
    char version[16];  /* the firmware version it was made for */
    bool draft;        /* not yet reviewed (development builds only) */
    uint8_t state;     /* READY: made from this firmware's English, `missing` phrases show in English;
                          OTHER: made for other firmware - some may; REFUSED: can't be used here (a
                          draft on a release firmware); DAMAGED: its signature or format failed when
                          loaded */
    uint32_t missing;
} ui_lang_t;

typedef struct {
    ui_net_state_t state;
    char ssid[33];
    char ip[16];
    char mac[18];
    int8_t rssi;
    ui_wifi_fail_t fail;
    bool time_synced;
    char radio_fw[24]; /* Wi-Fi co-processor firmware, "" = unknown */
    /* The address in use right now ("" until there is one), whatever set it. */
    char mask[16];
    char gateway[16];
    char dns1[16];
    char dns2[16];
    bool static_ip; /* the address came from the settings below, not from the router */
} ui_wifi_info_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} ui_wifi_ap_t;

/* How the panel gets its address (Settings > Wi-Fi > the cog). All fields are dotted IPv4
 * ("192.168.1.42"); empty means "not set". With static off the panel asks the router (DHCP)
 * and only dns1/dns2 are used, and only when they are set. */
typedef struct {
    bool use_static;
    char ip[16];
    char mask[16];
    char gateway[16];
    char dns1[16];
    char dns2[16];
} ui_ip_config_t;

/* Checking the connection (Settings > Wi-Fi > the cog > Check the connection), one step at a
 * time in this order. Every step says what it found, in words. */
typedef enum {
    UI_CHECK_WIFI = 0, /* joined the access point */
    UI_CHECK_ADDRESS,  /* the panel has an address (from the router or from the settings) */
    UI_CHECK_ROUTER,   /* the router answers */
    UI_CHECK_DNS,      /* names can be looked up */
    UI_CHECK_INTERNET, /* something out on the internet answers */
    UI_CHECK_TIME,     /* the time server answers */
    UI_CHECK_SERVICE,  /* the connected service (MQTT broker), only when one is set up */
    UI_CHECK_COUNT,
} ui_check_step_t;

typedef enum {
    UI_CHECK_WAITING = 0, /* its turn hasn't come */
    UI_CHECK_RUNNING,
    UI_CHECK_PASS,
    UI_CHECK_FAIL,
    UI_CHECK_WARN,    /* worked, but worth knowing */
    UI_CHECK_SKIPPED, /* nothing to check, or an earlier step failed */
} ui_check_state_t;

typedef struct {
    uint8_t state;     /* ui_check_state_t */
    uint16_t ms;       /* how long it took, 0 = not measured */
    const char *words; /* what it found, or where it went wrong: a pattern with {0} and {1}
                          (ui_check_words.h, a string literal); NULL: nothing yet */
    char value[2][48]; /* what goes in {0} and {1}; ui_check_line() puts them together */
} ui_check_result_t;

typedef struct {
    bool running;
    bool done; /* a run has finished: the results below are its */
    ui_check_result_t steps[UI_CHECK_COUNT];
} ui_netcheck_t;

/* The battery-backed clock module (docs/features/35). Most panels have none, and then the UI says
 * nothing about it. */
typedef enum {
    UI_CLOCK_MODULE_NONE = 0, /* none fitted */
    UI_CLOCK_MODULE_OK,       /* keeping the time */
    UI_CLOCK_MODULE_LOST,     /* fitted, but without the time until it is set */
    UI_CLOCK_MODULE_FAILED,   /* stopped answering */
    UI_CLOCK_MODULE_OTHER,    /* another chip answers at its address */
} ui_clock_module_t;

/* What set the panel's clock last. */
typedef enum {
    UI_CLOCK_NOT_SET = 0,
    UI_CLOCK_FROM_MODULE,
    UI_CLOCK_FROM_INTERNET,
    UI_CLOCK_BY_HAND,
} ui_clock_source_t;

typedef struct {
    uint8_t module;     /* ui_clock_module_t */
    uint8_t source;     /* ui_clock_source_t */
    bool lost_at_start; /* the module had no time when the panel started: its coin cell? */
    bool temp_known;
    int16_t temp_c4;    /* the module's temperature, quarter degrees C */
} ui_clock_info_t;

/* A module that answers can keep a time set by hand. */
static inline bool ui_clock_settable(const ui_clock_info_t *c)
{
    return c->module == UI_CLOCK_MODULE_OK || c->module == UI_CLOCK_MODULE_LOST;
}

typedef struct {
    char firmware[32];
    char built[32];
    char idf[32];
    char chip[48];
    uint32_t uptime_s;
    uint32_t heap_internal_kb;
    uint32_t heap_psram_kb;
    uint32_t log_count;
    uint32_t log_capacity;
    char panel_id[16]; /* this panel's ID (About; no longer a sign-in name) */
} ui_about_t;

/* Firmware updates (Settings > About). */
typedef enum {
    UI_FW_IDLE = 0,
    UI_FW_CHECKING,
    UI_FW_DOWNLOADING,
    UI_FW_WAITING,    /* verified: restarts as soon as everything is disarmed and quiet */
    UI_FW_RESTARTING,
    UI_FW_FAILED,     /* see reason */
} ui_fw_phase_t;

/* What an update run is doing (docs/features/13 OTA-026; panel_ota_stage_t). A plain check has
 * none: phase UI_FW_CHECKING alone. */
typedef enum {
    UI_FW_STAGE_NONE = 0,
    UI_FW_STAGE_DOWNLOADING,  /* bytes of size (size 0: connecting) */
    UI_FW_STAGE_VERIFYING,
    UI_FW_STAGE_WAITING,      /* reason: what holds it ("The system is armed") */
    UI_FW_STAGE_INSTALLING,   /* the restart is announced */
    UI_FW_STAGE_STOPPING,
    UI_FW_STAGE_ROLLING_BACK, /* the self-test failed: restarting with `previous` */
    UI_FW_STAGE_WIFI_MODULE,  /* no steps, no Stop */
    UI_FW_STAGE_FILE,         /* a developer's file: no steps, no Stop */
} ui_fw_stage_t;

typedef enum {
    UI_FW_LAST_NONE = 0,
    UI_FW_LAST_UPDATED,
    UI_FW_LAST_ROLLED_BACK,
    UI_FW_LAST_STOPPED,
    UI_FW_LAST_FAILED,
} ui_fw_last_result_t;

typedef struct {
    ui_fw_phase_t phase;
    char running[32];
    char latest[32];      /* newest release from the last check, "" = not checked */
    bool available;       /* latest is newer than running */
    bool has_source;      /* an update server is set */
    int percent;          /* UI_FW_DOWNLOADING: 0..100, -1 = unknown */
    char reason[64];      /* UI_FW_WAITING: what holds the restart back; UI_FW_FAILED: why */
    bool confirming;      /* first start after an update: the self-test still runs */
    char rolled_back[32]; /* an update to this version failed its self-test, "" = none */
    uint32_t checked;     /* Unix time the last check finished, either way; 0 = never checked */

    /* ---- the run (OTA-026..OTA-031, BL-188) ---- */
    uint8_t stage;        /* ui_fw_stage_t */
    bool release;         /* the run installs a release from the update server (Stop is offered);
                             false for the Wi-Fi module or a developer's file */
    char target[32];      /* the version being installed */
    char previous[32];    /* what a failed self-test returns to */
    uint32_t bytes, size; /* UI_FW_STAGE_DOWNLOADING: written so far, of size (0 = not known) */
    bool failed_install;  /* UI_FW_FAILED: an install failed (false: a check did) */
    struct {
        uint8_t result;   /* ui_fw_last_result_t */
        char version[32]; /* the version the run was for */
        char from[32];    /* the version before it */
        uint32_t at;      /* Unix time it ended, 0 = unknown */
        char reason[64];  /* UI_FW_LAST_FAILED: why */
        char by[24];      /* UI_FW_LAST_STOPPED: who stopped it ("Alex", "The installer"), "" = the
                             system */
    } last;

    /* ---- the update server (OTA-017..OTA-025, BL-186) ---- */
    char installed_channel[8]; /* the channel the running version is on now, "" = not known */
    bool withdrawn;       /* its publisher withdrew the running version */
    bool critical;        /* the offered version is an important fix */
} ui_fw_status_t;

/* The update server (Installer > Updates, BL-186). */
typedef struct {
    char domain[96];      /* "roboalarms.com" (never the full address) */
    bool official;        /* this build's own server */
    char key_name[48];    /* "RoboAlarms release", "" = not known yet */
    char fingerprint[20]; /* "3F9A 1C2B 77D0 E415": the key's digest, first 16 hex digits */
    char channel[8];      /* "stable", "beta", "alpha" or "dev" */
} ui_fw_server_t;

typedef enum {
    UI_FW_SRV_NONE = 0,
    UI_FW_SRV_CHECKING,
    UI_FW_SRV_OK,           /* it answered with firmware for this panel: see key, trusted */
    UI_FW_SRV_UNREACHABLE,
    UI_FW_SRV_NOT_A_SERVER,
    UI_FW_SRV_NO_FIRMWARE,
    UI_FW_SRV_BAD_DOMAIN,
} ui_fw_srv_result_t;

typedef struct {
    uint8_t result;       /* ui_fw_srv_result_t */
    char domain[96];      /* as normalised */
    char key_name[48];    /* UI_FW_SRV_OK */
    char fingerprint[20];
    uint8_t key_sha256[32]; /* the whole digest: what fw_server_commit is given back */
    bool trusted;         /* the panel trusts the key already: the usual question, no warning */
} ui_fw_srv_check_t;

/* What fw_server_commit did. */
typedef enum {
    UI_FW_COMMIT_OK = 0,
    UI_FW_COMMIT_FACTORY_CODE, /* the installer code is still the factory one (OTA-021) */
    UI_FW_COMMIT_RUNNING,      /* an update runs (OTA-030) */
    UI_FW_COMMIT_STALE,        /* no finished check to keep (it was forgotten or replaced) */
} ui_fw_commit_t;

/* What fw_stop did. */
typedef enum {
    UI_FW_STOP_OK = 0,      /* stopping (or already stopping) */
    UI_FW_STOP_NOTHING,     /* nothing runs any more */
    UI_FW_STOP_NOT_STOPPABLE,
} ui_fw_stop_t;

/* The panel's connection to an MQTT broker (docs/features/10), its own integration. */
typedef enum {
    UI_MQTT_OFF = 0,     /* turned off */
    UI_MQTT_NO_NETWORK,  /* waiting for Wi-Fi */
    UI_MQTT_CONNECTING,
    UI_MQTT_CONNECTED,
    UI_MQTT_FAILED,      /* see ui_mqtt_status_t.fail; tried again by itself */
} ui_mqtt_state_t;

typedef enum {
    UI_MQTT_FAIL_NONE = 0,
    UI_MQTT_FAIL_UNREACHABLE, /* address not found, or nothing answers on the port */
    UI_MQTT_FAIL_AUTH,        /* the broker refused the user name or password */
    UI_MQTT_FAIL_TLS,         /* the broker's certificate isn't trusted, or TLS failed */
    UI_MQTT_FAIL_OTHER,
} ui_mqtt_fail_t;

typedef enum {
    UI_MQTT_TLS_OFF = 0, /* not encrypted: home network only */
    UI_MQTT_TLS_PUBLIC,  /* encrypted, certificate from a public authority (Let's Encrypt...) */
    UI_MQTT_TLS_OWN_CA,  /* encrypted, own certificate authority (loaded on the service console) */
} ui_mqtt_tls_t;

typedef struct {
    bool enabled;
    char host[64];
    uint16_t port;         /* 0 = the usual one for the security choice (1883 / 8883) */
    char user[64];
    char password[64];     /* never read back: "" from mqtt_get */
    bool password_set;     /* mqtt_get: a password is stored */
    bool password_change;  /* mqtt_set: store 'password' (empty clears it) */
    uint8_t tls;           /* ui_mqtt_tls_t */
    bool ca_loaded;        /* mqtt_get: a certificate for UI_MQTT_TLS_OWN_CA is stored */
    bool discovery;        /* the panel announces itself on the broker (MQTT discovery) */
    bool commands;         /* the broker may arm and disarm the panel (off = read-only) */
    bool allow_panic;      /* panics over MQTT too (police, fire, medical, silent): off by
                            * default, the installer's to change (docs/features/14) */
    char prefix[32];       /* topic prefix, default "alarmpanel" */
    char panel_id[17];     /* default: from the MAC */
} ui_mqtt_settings_t;

typedef struct {
    ui_mqtt_state_t state;
    ui_mqtt_fail_t fail;
    bool waiting;          /* events waiting to be delivered */
    uint32_t sent;         /* events delivered since the panel started */
    char broker[80];       /* "host:port" in use */
} ui_mqtt_status_t;

/* The local Home Assistant link (docs/features/23): discovered by mDNS, paired on this screen. */
typedef enum {
    UI_HALINK_OFF = 0,      /* switched off on the panel */
    UI_HALINK_NO_NETWORK,   /* enabled, waiting for Wi-Fi */
    UI_HALINK_READY,        /* advertising, nothing paired yet */
    UI_HALINK_PAIRING,      /* the 2-minute window is open */
    UI_HALINK_PAIRING_CODE, /* both screens show the code: halink_pair_decide */
    UI_HALINK_PAIRED_IDLE,  /* paired, Home Assistant not connected right now */
    UI_HALINK_CONNECTED,
} ui_halink_state_t;

typedef struct {
    ui_halink_state_t state;
    bool enabled;
    bool paired;
    char client[48];   /* the paired (or pairing) client, e.g. "aioroboalarms 0.1.0" */
    char code[7];      /* the 6-digit code while UI_HALINK_PAIRING_CODE, else "" */
    uint32_t window_s; /* seconds left of the pairing window, 0 when closed */
    char panel_id[17];
    uint16_t port;
} ui_halink_status_t;

/* Push notifications straight to a phone (docs/features/26-28): ntfy and Pushover, each its own
 * entry under Integrations > Notifications. Neither needs a server of the owner's own. */
typedef enum {
    UI_NOTIFY_NTFY = 0,
    UI_NOTIFY_PUSHOVER,
    UI_NOTIFY_PROVIDERS,
} ui_notify_provider_t;

typedef enum {
    UI_NOTIFY_OFF = 0,    /* turned off */
    UI_NOTIFY_NO_NETWORK, /* on, waiting for Wi-Fi */
    UI_NOTIFY_READY,      /* on, nothing sent yet */
    UI_NOTIFY_SENDING,
    UI_NOTIFY_OK,     /* the last one got through */
    UI_NOTIFY_FAILED, /* see ui_notify_status_t.fail; tried again by itself */
    UI_NOTIFY_PAUSED, /* too many today: alarms only, or nothing (NOTE-031) */
} ui_notify_state_t;

typedef enum {
    UI_NOTIFY_FAIL_NONE = 0,
    UI_NOTIFY_FAIL_UNREACHABLE, /* the address didn't answer */
    UI_NOTIFY_FAIL_AUTH,        /* the token, or the user key, was refused */
    UI_NOTIFY_FAIL_TLS,         /* the certificate isn't trusted */
    UI_NOTIFY_FAIL_QUOTA,       /* the service says the allowance is used up */
    UI_NOTIFY_FAIL_OTHER,
} ui_notify_fail_t;

typedef struct {
    ui_notify_state_t state;
    ui_notify_fail_t fail;
    bool waiting;      /* something is queued for when it can be sent */
    uint32_t sent;     /* delivered since the panel started */
    uint32_t today;    /* toward the daily limit */
    uint32_t limit;    /* the daily limit, 0 when the service doesn't say */
    bool ever_sent;    /* something has got through at least once */
    uint32_t ago_s;    /* seconds since the last one got through */
    char detail[80];   /* what the service said, when it refused */
    int32_t quota_left; /* messages left this month, -1 = the service has not said (PUSH-011) */
} ui_notify_status_t;

/* ntfy: a topic on ntfy.sh, or on a server of your own. The topic is the only credential
 * anyone needs to read the alerts, so it is generated, not chosen, and shown only here. */
typedef struct {
    bool enabled;
    bool self_hosted;  /* false: the hosted ntfy.sh */
    char server[96];   /* "https://ntfy.example.com", only when self_hosted */
    char topic[64];
    char token[64];    /* never read back: "" from ntfy_get */
    bool token_set;    /* ntfy_get: a token is stored */
    bool token_change; /* ntfy_set: store 'token' (empty clears it) */
    bool ca_loaded;    /* ntfy_get: an own certificate authority is stored */
    uint8_t cats;      /* which kinds of event: NOTIFY_CAT_BIT(...) */
    bool silent_too;   /* send duress and a silent panic as well (default off) */
} ui_ntfy_settings_t;

/* Pushover: the owner's own user key and application token - Pushover asks every installation
 * to register its own, so each gets its own monthly allowance. Its emergency priority repeats
 * on the phone until someone answers, which is why alarms use it. */
typedef struct {
    bool enabled;
    char user_key[48];
    bool user_key_set;
    bool user_key_change;
    char app_token[48];
    bool app_token_set;
    bool app_token_change;
    uint8_t cats;
    bool silent_too;
    bool emergency;   /* alarms repeat until acknowledged (default on) */
    char sound[16];   /* the alarm sound, "" = whatever the account uses */
} ui_pushover_settings_t;

/* A zone source the installer can pick (docs/features/22). The UI has its own words per kind. */
typedef struct {
    uint8_t kind;  /* zsrc_kind_t */
    uint32_t caps; /* ZSRC_CAP_* */
    bool usable;   /* zones can be added with it now (ZSRC-005) */
} ui_zone_source_t;

/* ---- Z-Wave (docs/features/36 screens.md §12) ------------------------------------------------
 * What the Z-Wave pages show, as states and numbers: the words are the UI's (ui_zwave.c), except
 * the quirks table's (a model's name, how to wake it) and the zone source's input words, which
 * the source picker shows too. */

#define UI_ZW_DEVICES_MAX 64u /* devices in a network (the node cache's) */
#define UI_ZW_ROUTES_LIST 6u  /* devices a route rebuild names (ui_zw_progress_t.routes_list) */
#define UI_ZW_ZONES_MAX   4u  /* zones one device's inputs feed, as its row lists them */
#define UI_ZW_INPUTS_MAX  8u  /* inputs one device offers on its page */
#define UI_ZW_OUTPUTS_MAX 3u  /* outputs one device switches (a multi-relay: 3) */
#define UI_ZW_REGIONS_MAX 12u /* regions a radio offers */
#define UI_ZW_MODEL_LEN   40u
#define UI_ZW_WORDS_LEN   64u
#define UI_ZW_DSK_LEN     48u /* "34028-23669-...-12345": 8 groups of 5 digits */
#define UI_ZW_BATTERY_UNKNOWN 0xFFu
#define UI_ZW_NEVER       UINT32_MAX /* heard_s: not heard since the panel started */

/* The classes of security a device asks for or gets (the Z-Wave KEX bits). */
#define UI_ZW_KEY_S2_UNAUTH 0x01u
#define UI_ZW_KEY_S2_AUTH   0x02u
#define UI_ZW_KEY_S2_ACCESS 0x04u
#define UI_ZW_KEY_S0        0x80u

/* The radio, as the installer menu's row and the Z-Wave page's first row say it (§2, §3). */
typedef enum {
    UI_ZW_RADIO_NOT_CONNECTED = 0, /* no place chosen for it (the Flasher or the console) */
    UI_ZW_RADIO_STARTING,          /* its start-up runs, or runs again */
    UI_ZW_RADIO_WORKING,
    UI_ZW_RADIO_NOT_ANSWERING,     /* silent: the panel keeps trying */
    UI_ZW_RADIO_UNUSABLE,          /* it answers, but isn't a Z-Wave controller it can use */
    UI_ZW_RADIO_FOREIGN,           /* it belongs to another system: reset it to use it here */
    UI_ZW_RADIO_REPLACED,          /* replaced or reset: `replaced` devices to add again */
    UI_ZW_RADIO_UPDATING,          /* its own firmware is being updated (network.md §14) */
} ui_zw_radio_t;

typedef struct {
    uint8_t radio;                  /* ui_zw_radio_t */
    bool update_advised;            /* its Z-Wave software is older than the panel wants */
    bool region_known;
    uint8_t region;                 /* its region code (docs/features/36 serial-api.md §6.7) */
    uint8_t region_count;
    uint8_t regions[UI_ZW_REGIONS_MAX]; /* the regions it offers */
    uint16_t devices;               /* in the network */
    uint16_t not_heard;             /* of them: unreachable now */
    /* network.md §14: the radio's own software check (ui_zw_fw_check_t), the version offered,
     * why a check or download failed (UI_ZW_REQ_FW_CHECK / _INSTALL with node 0) */
    uint8_t radio_fw_check;
    char radio_fw_version[24];
    char radio_fw_words[96];
    uint16_t replaced;              /* UI_ZW_RADIO_REPLACED: the devices to add again */
    uint32_t check_in_s;            /* the check-in battery devices get when added */
    uint8_t firmware_major, firmware_minor; /* the radio's own software */
    uint8_t sdk_major, sdk_minor, sdk_patch; /* its Z-Wave version */
    uint32_t home_id;               /* for the Panel Manager's radio page only (§11) */
    bool noise_known;               /* phase 2: background noise, the quietest channel */
    int8_t noise_dbm;
    uint16_t joining;               /* phase 2: SmartStart entries waiting to join (§9) */
    uint16_t routes_done, routes_total; /* phase 2: rebuilding routes ("3 of 7 done"); 0 when not */
    bool jammed;                    /* ZW-024 (network.md §9): something drowns out its signals */
    /* ZW-045 (network.md §12): what choosing regions[i] would do to the devices
     * (ui_zw_region_change_t), and how many of them would stop working */
    uint16_t devices_lr;            /* of `devices`: Long Range ones */
    uint8_t region_change[UI_ZW_REGIONS_MAX];
    uint16_t region_stop[UI_ZW_REGIONS_MAX];
    bool storage_full;              /* ZW-039: the panel's storage is down to what a new device
                                     * needs (zwz_panel_nvs_need): Add and Replace answer FULL,
                                     * SmartStart devices wait until there is room */
} ui_zw_status_t;

/* What a region change does to the devices (ui_zw_status_t.region_change; zwave_host's
 * zw_region_change_t, network.md §12, ZW-045). */
typedef enum {
    UI_ZW_REGION_SAME_BAND = 0, /* every device keeps working; the radio restarts */
    UI_ZW_REGION_DROPS_LR,      /* the Long Range devices stop (a Long Range region left for its
                                 * classic one) */
    UI_ZW_REGION_BAND_CHANGE,   /* another band: every device stops until it is added again */
} ui_zw_region_change_t;

typedef enum {
    UI_ZW_SEC_UNKNOWN = 0, /* not asked yet */
    UI_ZW_SEC_NONE,        /* not encrypted */
    UI_ZW_SEC_S0,          /* older security */
    UI_ZW_SEC_S2_UNAUTH,   /* secure, no PIN */
    UI_ZW_SEC_S2_AUTH,
    UI_ZW_SEC_S2_ACCESS,
    UI_ZW_SEC_UNCONFIRMED, /* no key granted, and its setup never read what it supports since:
                            * whether it runs unencrypted is not confirmed */
    UI_ZW_SEC_NONE_CAPABLE, /* not encrypted, though it lists Security 2 (its key exchange failed
                             * or was declined) */
} ui_zw_security_t;

typedef enum {
    UI_ZW_DEV_READY = 0,    /* set up: its inputs are offered */
    UI_ZW_DEV_SETTING_UP,   /* its interview runs */
    UI_ZW_DEV_WAKE_NEEDED,  /* ... and waits for it to wake up: "Wake it to finish setting it up" */
    UI_ZW_DEV_NO_REPORTS,   /* it reports nothing to the panel (no lifeline): no inputs */
    UI_ZW_DEV_NOT_USABLE,   /* another controller: remove it */
    UI_ZW_DEV_SETUP_STOPPED, /* its setup gave up after its attempts (not asleep): "Try again" */
    UI_ZW_DEV_JOINING,       /* it is being added: its security question or PIN waits */
} ui_zw_dev_state_t;

/* What kind of device it is, for its name when it has no model or zone (never a brand). */
typedef enum {
    UI_ZW_KIND_DEVICE = 0, /* not known */
    UI_ZW_KIND_SENSOR,
    UI_ZW_KIND_KEYPAD,     /* Entry Control */
    UI_ZW_KIND_SWITCH,     /* on/off (Binary Switch) */
    UI_ZW_KIND_DIMMER,     /* Multilevel Switch */
    UI_ZW_KIND_RELAY,      /* several on/off outputs (Multi Channel with Binary Switch) */
    UI_ZW_KIND_SIREN,
    UI_ZW_KIND_REPEATER,   /* a range extender: routes only */
} ui_zw_kind_t;

/* Where a device's power comes from, as far as the panel knows (never guessed from its radio:
 * a battery keypad can listen for the panel's call too). */
typedef enum {
    UI_ZW_POWER_UNKNOWN = 0, /* not read yet */
    UI_ZW_POWER_BATTERY,     /* it has the Battery command class */
    UI_ZW_POWER_MAINS,       /* always listening and no Battery command class in its full list */
} ui_zw_power_t;

typedef enum {
    UI_ZW_SIGNAL_UNKNOWN = 0, /* phase 2 measures it */
    UI_ZW_SIGNAL_GOOD,
    UI_ZW_SIGNAL_FAIR,
    UI_ZW_SIGNAL_WEAK,
} ui_zw_signal_t;

/* A device, as the device list shows it (§3). */
typedef enum {
    UI_ZW_ROUTE_WAITING = 0, /* a sleeper that waits for its wake-up */
    UI_ZW_ROUTE_FAILED,      /* its neighbour update failed every try */
    UI_ZW_ROUTE_SKIPPED,     /* dead, Long Range, or the job was stopped */
    UI_ZW_ROUTE_NO_ROUTE     /* done, but its return route to the panel could not be assigned */
} ui_zw_route_item_t;

typedef struct {
    uint16_t node;         /* its node ID ("device 7", on its page only) */
    uint16_t generation;   /* with the node ID: this device, even once the ID is given out again */
    char model[UI_ZW_MODEL_LEN]; /* the quirks table's name for it; "" when it has no row */
    bool has_identity;     /* manufacturer, product type and product ID are known */
    uint16_t manufacturer, product_type, product_id;
    uint8_t security;      /* ui_zw_security_t */
    uint8_t state;         /* ui_zw_dev_state_t */
    bool listening;        /* answers the panel at any time (always listening, or woken by the
                            * panel's call: `flirs`): no check-ins. Not its power source. */
    bool flirs;            /* woken by the panel's call (FLiRS): usually battery powered */
    uint8_t power;         /* ui_zw_power_t */
    uint8_t setup_done;    /* its setup's steps done (READY: all) */
    uint8_t setup_total;
    uint8_t kind;          /* ui_zw_kind_t */
    bool unreachable;      /* missed its check-ins, or didn't answer its pings */
    uint8_t battery_pct;   /* 0..100, UI_ZW_BATTERY_UNKNOWN */
    bool battery_low;
    uint32_t heard_s;      /* seconds since it was heard from; UI_ZW_NEVER */
    uint8_t signal;        /* ui_zw_signal_t */
    uint8_t device_class;  /* alarm_device_class_t of its first input: its icon */
    uint8_t zone_count;    /* zones its inputs feed (more than UI_ZW_ZONES_MAX: the first ones) */
    uint16_t zones[UI_ZW_ZONES_MAX];
} ui_zw_device_t;

/* One input a device offers (zones.md §2): what a zone binds. */
typedef struct {
    zsrc_address_t addr;
    uint8_t endpoint;
    uint8_t slot;          /* zsrc_zw_slot_t */
    uint8_t device_class;  /* alarm_device_class_t a zone of it starts with */
    char words[48];        /* the source's: "door contact", "input 2 · water" */
    char state[24];        /* the source's, right now: "Open", "Dry", "21.5 °C"; "" not known */
    uint16_t zone;         /* the zone it feeds, 0 none */
} ui_zw_input_t;

/* One output a device switches (ZW-034), and a relay that opens a garage door (ZW-035). */
typedef struct {
    uint8_t endpoint;      /* 0: the device itself */
    uint8_t kind;          /* ui_zw_out_kind_t */
    int8_t level;          /* -1 unknown, 0..100 % */
    bool busy;             /* a command under way */
    uint8_t last;          /* ui_zw_out_result_t of its last command (NONE: none said) */
    bool garage;           /* it is a garage door's opener */
    uint16_t garage_zone;  /* ... whose tilt sensor is this zone */
    uint16_t garage_travel_s;
    char garage_state[12]; /* where its sensor says it is: "open", "closed", "unknown" */
    uint8_t garage_goal;   /* a press under way: 1 to open, 2 to close; 0 none */
} ui_zw_output_t;

/* A device's page (§6) and the add flow's "Use it for" (§4). */
typedef struct {
    ui_zw_device_t dev;
    uint32_t check_in_s;       /* its wake-up interval now; 0: it never checks in */
    bool check_in_known;       /* check_in_s is its answer (false: its setup hasn't read it) */
    uint32_t check_in_next_s;  /* chosen, applied when it next wakes up; 0: nothing waiting */
    uint32_t check_in_min_s, check_in_max_s; /* its limits; 0 when not known */
    bool can_identify;         /* phase 2: it can blink its light (Indicator v3) */
    char wake_words[UI_ZW_WORDS_LEN]; /* how to wake it (the quirks table's); "" not known */
    char add_words[UI_ZW_WORDS_LEN];  /* how to put it into pairing mode; "" not known */
    char note[UI_ZW_WORDS_LEN];       /* the quirks table's note; "" none */
    uint8_t input_count;
    ui_zw_input_t inputs[UI_ZW_INPUTS_MAX];
    /* A keypad's own sounds and lights showing the panel's state (panel to device only; it gives
     * the keypad no control of the panel). fit UI_ZW_FB_NONE: not a keypad this panel drives. */
    struct {
        uint8_t fit;       /* ui_zw_feedback_fit_t */
        uint8_t partition; /* the partition it shows, 0: off */
        bool live;         /* assigned and being sent now */
    } feedback;
    /* A keypad's own buttons arming and disarming the panel (device to panel), with the users'
     * codes checked by the panel; separate from `feedback`. fit as for feedback (UI_ZW_FB_*,
     * NONE: not a keypad with secure Entry Control). */
    struct {
        uint8_t fit;
        uint8_t partition;  /* the partition its buttons act on, 0: off */
        uint8_t panic_mask; /* its emergency buttons allowed: 1 << alarm_panic_t */
        bool live;          /* assigned, and still the device and class it was assigned at */
    } control;
    /* What it switches (ZW-034, ZW-035): its outputs - a switch, a dimmer, a multi-relay's
     * relays - and a relay that opens a garage door. */
    uint8_t output_count;
    ui_zw_output_t outputs[UI_ZW_OUTPUTS_MAX];
    /* Its software (ZW-033): what it runs, and an update the installer asked about. */
    struct {
        bool known;          /* its setup read its versions */
        uint8_t major, minor;
        bool has_hardware;
        uint8_t hardware;    /* its hardware version, as it reports it */
        bool updatable;      /* it takes software updates (Firmware Update Meta Data) and its
                              * setup is done; its identity is known */
        uint8_t check;       /* ui_zw_fw_check_t: the last check's answer, for this device */
        char version[24];    /* UI_ZW_FW_AVAILABLE: the version offered */
        bool beta;
        char words[96];      /* UI_ZW_FW_CHECK_FAILED / FILE_FAILED: why, in words; AVAILABLE: its
                              * change notes, cut ("" none) */
    } fw;
    bool identify;           /* ZW-024: it can blink its own light when asked (UI_ZW_REQ_IDENTIFY) */
    uint32_t jam_report_s;   /* seconds since it last reported radio jamming (network.md §9);
                              * UI_ZW_NEVER: not since the panel started */
} ui_zw_device_page_t;

typedef enum {
    UI_ZW_OUT_SWITCH = 1, /* on/off at the device */
    UI_ZW_OUT_DIMMER,     /* a level */
    UI_ZW_OUT_RELAY,      /* on/off at an end point (a multi-relay's) */
} ui_zw_out_kind_t;

/* How an output's (or a garage's) last command ended. */
typedef enum {
    UI_ZW_OUT_NONE = 0,
    UI_ZW_OUT_DONE,
    UI_ZW_OUT_ALREADY,     /* the garage was there already: nothing pressed */
    UI_ZW_OUT_NO_ANSWER,   /* it didn't confirm */
    UI_ZW_OUT_SEND_FAILED, /* the radio couldn't reach it */
    UI_ZW_OUT_NO_POSITION, /* the garage's sensor doesn't say where the door is: nothing pressed */
    UI_ZW_OUT_NO_MOVE,     /* the door didn't get there in its travel time */
    UI_ZW_OUT_STUCK,       /* the relay didn't let go after its press */
    UI_ZW_OUT_FAILED,      /* anything else (it was removed meanwhile) */
    UI_ZW_OUT_STILL_MOVING, /* the garage door is on its way: a press would reverse it */
} ui_zw_out_result_t;

/* A device's software check (ZW-033): only when the installer asks. */
typedef enum {
    UI_ZW_FW_NONE = 0,       /* not checked since the panel started */
    UI_ZW_FW_CHECKING,       /* asking the update service */
    UI_ZW_FW_UP_TO_DATE,     /* the service has nothing newer for it */
    UI_ZW_FW_UNKNOWN,        /* the service doesn't know this device */
    UI_ZW_FW_AVAILABLE,      /* `version` can be installed */
    UI_ZW_FW_CHECK_FAILED,   /* the service couldn't be asked: `words` */
    UI_ZW_FW_DOWNLOADING,    /* the installer said install: its file is fetched and checked */
    UI_ZW_FW_FILE_FAILED,    /* its file didn't arrive, or isn't the one promised: `words` */
} ui_zw_fw_check_t;

typedef enum {
    UI_ZW_FB_NONE = 0,    /* not a keypad with a known feedback profile */
    UI_ZW_FB_AVAILABLE,   /* it can show the panel's state */
    UI_ZW_FB_NOT_READY,   /* its setup must finish first */
    UI_ZW_FB_NOT_SECURE,  /* it needs Security 2 with its PIN (added again) */
    UI_ZW_FB_UNSUPPORTED, /* its setup found no secure indicator of the needed version */
} ui_zw_feedback_fit_t;

typedef enum {
    UI_ZW_REQ_ADD = 1,        /* start adding: the radio listens for 60 s */
    UI_ZW_REQ_REMOVE,         /* start removing (also resets a device of another system) */
    UI_ZW_REQ_KEEP_LISTENING, /* adding or removing: 60 s again */
    UI_ZW_REQ_STOP,           /* stop listening; while it is being set up: "finish later" */
    UI_ZW_REQ_ALLOW,          /* the security question: grant `keys` (UI_ZW_KEY_*) */
    UI_ZW_REQ_PIN,            /* the PIN on its label: `pin`, 5 digits */
    UI_ZW_REQ_INSECURE,       /* "Add it without encryption": the key exchange is cancelled */
    UI_ZW_REQ_REMOVE_FAILED,  /* "Remove it: it's broken or gone": `node` */
    UI_ZW_REQ_REPLACE,        /* phase 2: "Replace it": `node` */
    UI_ZW_REQ_PING,           /* "Check it now": `node` (a listening device) */
    UI_ZW_REQ_IDENTIFY,       /* phase 2: blink its light: `node` */
    UI_ZW_REQ_ROUTES,         /* phase 2: rebuild routes: `node`, 0 every device */
    UI_ZW_REQ_CHECK_IN,       /* `seconds` for `node`'s check-in; node 0: new devices' */
    UI_ZW_REQ_REGION,         /* `region` (one of the radio's) */
    UI_ZW_REQ_RESET,          /* reset the network (the UI asked twice) */
    UI_ZW_REQ_RESTART,        /* restart the radio */
    UI_ZW_REQ_DISMISS,        /* the ended operation was seen: zw_progress returns to idle */
    UI_ZW_REQ_SETUP,          /* "Try its setup again": continue `node`'s interview */
    UI_ZW_REQ_FEEDBACK,       /* a keypad's sounds and lights for `partition` (0: off): `node` */
    UI_ZW_REQ_CONTROL,        /* a keypad's buttons arm/disarm `partition` (0: off), with its
                               * emergency buttons `panic_mask`: `node` */
    UI_ZW_REQ_FW_CHECK,       /* ask the update service about `node`'s software (ZW-033) */
    UI_ZW_REQ_FW_INSTALL,     /* install the version its check offered: `node`; STOP stops it */
    UI_ZW_REQ_OUTPUT,         /* switch `node`'s output at `endpoint` to `level` (0..100) */
    UI_ZW_REQ_GARAGE,         /* `node`'s relay at `endpoint` opens a garage door whose tilt
                               * sensor is `zone` (0: no longer), travel `seconds` (0 default) */
    UI_ZW_REQ_GARAGE_MOVE,    /* that garage door: `level` 100 open, 0 closed */
} ui_zw_req_kind_t;

typedef struct {
    uint8_t kind;          /* ui_zw_req_kind_t */
    uint8_t user;          /* who asks (the event log) */
    uint16_t node;         /* the device: node ID and generation (0: none) */
    uint16_t generation;
    uint8_t keys;          /* UI_ZW_REQ_ALLOW */
    uint8_t region;        /* UI_ZW_REQ_REGION */
    uint32_t pin;          /* UI_ZW_REQ_PIN: 0..UI_ZW_PIN_MAX (5 digits, leading zeros kept) */
    uint32_t seconds;      /* UI_ZW_REQ_CHECK_IN */
    uint8_t partition;     /* UI_ZW_REQ_FEEDBACK, UI_ZW_REQ_CONTROL */
    uint8_t panic_mask;    /* UI_ZW_REQ_CONTROL: 1 << alarm_panic_t (fire, police, medical) */
    uint8_t endpoint;      /* UI_ZW_REQ_OUTPUT, _GARAGE, _GARAGE_MOVE */
    int16_t level;         /* UI_ZW_REQ_OUTPUT, _GARAGE_MOVE */
    uint16_t zone;         /* UI_ZW_REQ_GARAGE */
} ui_zw_request_t;

/* The PIN is the DSK's first group: a 16-bit number written as 5 digits (00000..65535). */
#define UI_ZW_PIN_MAX 65535u

typedef enum {
    UI_ZW_REQ_OK = 0,      /* taken: zw_progress follows it */
    UI_ZW_REQ_NOT_ALLOWED, /* installer mode with every partition disarmed is needed */
    UI_ZW_REQ_BUSY,        /* another network operation runs */
    UI_ZW_REQ_NO_RADIO,    /* the radio isn't working */
    UI_ZW_REQ_INVALID,     /* not now, not that device, or a value out of range */
    UI_ZW_REQ_UNSUPPORTED, /* this panel can't do it (yet) */
    UI_ZW_REQ_LOW_BATTERY, /* UI_ZW_REQ_FW_INSTALL: its battery is too low for an update */
    UI_ZW_REQ_FULL,        /* ADD, REPLACE: the panel's storage keeps the room the alarm needs
                            * (ZW-039; docs/features/20 devices.md "The NVS budget"); ADD,
                            * REPLACE, REMOVE, REMOVE_FAILED: the last write of the Z-Wave node
                            * cache failed (ZW-014); ADD: the inclusion counter is used up */
} ui_zw_req_result_t;

/* The network operation zw_progress reports (one at a time). */
typedef enum {
    UI_ZW_OP_NONE = 0,
    UI_ZW_OP_ADD,
    UI_ZW_OP_REMOVE,
    UI_ZW_OP_REMOVE_FAILED,
    UI_ZW_OP_REPLACE,       /* phase 2 */
    UI_ZW_OP_PING,
    UI_ZW_OP_IDENTIFY,      /* phase 2 */
    UI_ZW_OP_ROUTES,        /* phase 2 */
    UI_ZW_OP_CHECK_IN,
    UI_ZW_OP_REGION,
    UI_ZW_OP_RESET,
    UI_ZW_OP_RESTART,
    UI_ZW_OP_FIRMWARE,      /* a device's software update: `fw_done` of `fw_total` */
    UI_ZW_OP_RADIO_UPDATE,  /* phase 3: the radio's own firmware: `fw_done` of `fw_total` blocks
                             * (network.md §14); arming waits (CORE-ARM-013) */
} ui_zw_op_t;

typedef enum {
    UI_ZW_STEP_IDLE = 0,
    UI_ZW_STEP_STARTING,   /* the radio is told to listen */
    UI_ZW_STEP_LISTENING,  /* "put the device in pairing mode": `seconds_left` */
    UI_ZW_STEP_CONNECTING, /* a device answered: it joins, or leaves */
    UI_ZW_STEP_ASK_ALLOW,  /* the security question: `requested`, `csa`, `seconds_left` */
    UI_ZW_STEP_ASK_PIN,    /* its PIN: `dsk`, `seconds_left` */
    UI_ZW_STEP_SECURING,   /* the key exchange runs */
    UI_ZW_STEP_READING,    /* "reading what it can do": `setup_done` of `setup_total`, `asleep` */
    UI_ZW_STEP_WORKING,    /* region, reset, restart, a ping, a removal of a failed device */
    UI_ZW_STEP_DONE,       /* it ended: `result` */
} ui_zw_step_t;

typedef enum {
    UI_ZW_RES_NONE = 0,
    UI_ZW_RES_ADDED,           /* in the network, set up or finishing later: `security` */
    UI_ZW_RES_ADDED_INSECURE,  /* in the network, its key exchange failed: `s2_failure` */
    UI_ZW_RES_DOWNGRADED,      /* in the network with weaker security than it asked for */
    UI_ZW_RES_NO_REPORTS,      /* in the network, but it can't report anything: no zone */
    UI_ZW_RES_NOTHING_JOINED,  /* the window ran out */
    UI_ZW_RES_JOIN_FAILED,     /* the device did not finish joining */
    UI_ZW_RES_ALREADY_ADDED,   /* `node` is paired already */
    UI_ZW_RES_CONTROLLER,      /* another controller, not a sensor: not added */
    UI_ZW_RES_STOPPED,         /* the installer stopped it */
    UI_ZW_RES_REMOVED,         /* `node` left: `model` and `zones` as they were */
    UI_ZW_RES_REMOVED_FOREIGN, /* a device of another system was reset: it can be added now */
    UI_ZW_RES_NOTHING_REMOVED, /* the window ran out */
    UI_ZW_RES_STILL_ANSWERS,   /* removing a failed device: it answers - remove it the normal way */
    UI_ZW_RES_ANSWERED,        /* a ping */
    UI_ZW_RES_NO_ANSWER,
    UI_ZW_RES_DONE,            /* region, reset, restart, check-in: done */
    UI_ZW_RES_FAILED,          /* it could not be done (the radio went away, it refused) */
    UI_ZW_RES_SETUP_PAUSED,    /* in the network, "Stop and finish later": its setup goes on and
                                * it is not usable until it ends */
    UI_ZW_RES_TIMED_OUT,       /* removing: it started but did not finish in time */
    UI_ZW_RES_REFUSED,         /* removing a failed device: the radio refused, or does not count
                                * it as failed - nothing removed */
    UI_ZW_RES_FW_UPDATED,      /* it took its new software (restarting: `fw_wait_s`) */
    UI_ZW_RES_FW_FAILED,       /* it didn't: `fw_why` - it keeps its old software, as a rule */
    UI_ZW_RES_RADIO_RESTARTED, /* the radio restarted while it listened: its window closed */
} ui_zw_result_t;

/* Why a software update ended without it (UI_ZW_RES_FW_FAILED). */
typedef enum {
    UI_ZW_FWF_NONE = 0,
    UI_ZW_FWF_NOT_UPGRADABLE, /* the device says its software can't be changed */
    UI_ZW_FWF_WRONG_DEVICE,   /* it isn't the device the file is for */
    UI_ZW_FWF_LOW_BATTERY,    /* it refused: its battery is low */
    UI_ZW_FWF_REFUSED,        /* it refused the file (hardware, identity, or anything else) */
    UI_ZW_FWF_BAD_TRANSFER,   /* it found the file damaged on arrival */
    UI_ZW_FWF_DEVICE_FAILED,  /* it could not store or start it */
    UI_ZW_FWF_NO_ANSWER,      /* it stopped answering */
    UI_ZW_FWF_NOT_WOKEN,      /* a sleeping device that wasn't woken in time */
    UI_ZW_FWF_SEND_FAILED,    /* the radio could not reach it */
    UI_ZW_FWF_STOPPED,        /* the installer stopped it */
    UI_ZW_FWF_NO_RADIO,       /* the radio went away */
} ui_zw_fw_failure_t;

/* Why a key exchange failed (UI_ZW_RES_ADDED_INSECURE). */
typedef enum {
    UI_ZW_S2_OK = 0,
    UI_ZW_S2_WRONG_PIN,
    UI_ZW_S2_TIMEOUT,        /* it, or the installer, took too long */
    UI_ZW_S2_CANCELLED,      /* the installer chose "without encryption" */
    UI_ZW_S2_DEVICE_STOPPED, /* the device stopped it */
    UI_ZW_S2_FAILED,         /* anything else (a message came back changed, the radio failed) */
    UI_ZW_S2_NO_INFO,        /* it didn't say what it supports and didn't answer the key
                              * exchange: whether it can be encrypted is not known */
} ui_zw_s2_failure_t;

typedef struct {
    uint8_t op;            /* ui_zw_op_t */
    uint8_t step;          /* ui_zw_step_t */
    uint8_t result;        /* UI_ZW_STEP_DONE: ui_zw_result_t */
    uint32_t seconds_left; /* the countdown of LISTENING, ASK_ALLOW, ASK_PIN; 0 none */
    uint8_t requested;     /* ASK_ALLOW: the classes it asks for (UI_ZW_KEY_*) */
    bool csa;              /* ASK_ALLOW: it asks to be trusted without a PIN of its own */
    char dsk[UI_ZW_DSK_LEN]; /* ASK_PIN and after: its key as printed; the PIN is the first group */
    uint16_t node;         /* the device it concerns; 0 before one answered */
    uint16_t generation;
    uint8_t security;      /* ui_zw_security_t it got */
    uint8_t s2_failure;    /* ui_zw_s2_failure_t */
    uint8_t setup_done, setup_total; /* READING: its interview's steps */
    bool asleep;           /* READING: a sleeping device fell asleep before it finished */
    bool stalled;          /* READING: its setup gave up after its attempts (it didn't answer;
                            * not asleep): the device page's "Try again" continues it */
    char model[UI_ZW_MODEL_LEN]; /* the device's model, when known (removed: as it was) */
    uint8_t zone_count;    /* UI_ZW_RES_REMOVED: the zones that now need a sensor */
    uint16_t zones[UI_ZW_ZONES_MAX];
    uint16_t fw_done, fw_total; /* UI_ZW_OP_FIRMWARE: its file's parts sent (0 of 0: not yet) */
    bool fw_wake;          /* UI_ZW_OP_FIRMWARE: waiting for the sleeping device to wake up */
    uint8_t fw_why;        /* UI_ZW_RES_FW_FAILED: ui_zw_fw_failure_t */
    uint16_t fw_wait_s;    /* UI_ZW_RES_FW_UPDATED: the device restarts for this long */
    uint8_t fw_error;      /* UI_ZW_OP_RADIO_UPDATE failed: the radio's bootloader's error code
                            * (0x18...), 0 none */
    bool prov_forgot;      /* UI_ZW_RES_REMOVED: its SmartStart code was forgotten with it (else
                            * it would join again when reset): UI_ZW_PROV_KEEP keeps it */
    /* UI_ZW_OP_ROUTES (network.md §11): devices ended of all, how many failed, were skipped
     * (didn't answer, or the job was stopped), and wait for their wake-up; `node` is the one
     * being repaired (0: none, waiting); a single device's job names it in `generation` too */
    uint16_t routes_done, routes_total, routes_failed, routes_skipped, routes_waiting;
    bool routes_stopped;
    /* BL-152 (ZW-024): the devices worth naming, waiting first, then failed, skipped, and those
     * whose return route could not be assigned (ui_zw_route_item_t); the first
     * UI_ZW_ROUTES_LIST of them. Kept with the result until dismissed. */
    uint8_t routes_list_n;
    struct {
        uint16_t node;
        uint8_t what; /* ui_zw_route_item_t */
    } routes_list[UI_ZW_ROUTES_LIST];
    bool replacing;        /* UI_ZW_OP_ADD from UI_ZW_REQ_REPLACE (network.md §10): a new device
                            * takes `node`'s place and generation - its zones follow it; ended
                            * without it: STILL_ANSWERS (the old one answered), REFUSED */
} ui_zw_progress_t;

/* ---- SmartStart: the provisioning list (network.md §4.2, screens.md §9) ---- */
#define UI_ZW_PROV_MAX  32u
#define UI_ZW_PROV_NAME 24u

typedef enum {
    UI_ZW_PROV_WAITING = 0, /* "Waiting": it joins when powered */
    UI_ZW_PROV_JOINED,      /* in the network (node, generation) */
    UI_ZW_PROV_STOPPED,     /* 5 joins failed: "Could not secure it 5 times: check its code" */
    UI_ZW_PROV_OFF,         /* turned off by the installer */
} ui_zw_prov_state_t;

typedef struct {
    uint8_t dsk[16];
    char name[UI_ZW_PROV_NAME];
    uint8_t state;           /* ui_zw_prov_state_t */
    uint8_t failures;
    bool long_range;
    uint8_t requested;       /* UI_ZW_KEY_* its code asked for (0: a typed key - not known) */
    uint8_t grant;           /* UI_ZW_KEY_* it may have */
    uint16_t node, generation;
    uint8_t generic;         /* from its code: what it is (0: not known) */
    uint16_t manufacturer, product_type, product_id;
    char model[UI_ZW_MODEL_LEN]; /* the quirks table's name for that product; "" when unknown (BL-152) */
} ui_zw_prov_t;

/* What registering a code did. */
typedef enum {
    UI_ZW_PROV_ADDED = 0,
    UI_ZW_PROV_UPDATED,      /* it was registered already: its details refreshed, turned on */
    UI_ZW_PROV_JOINED_ALREADY, /* registered, and that device is in the network already */
    UI_ZW_PROV_FULL,
    UI_ZW_PROV_BAD_CODE,     /* not a Z-Wave code or key (a typo, another kind of QR code) */
    UI_ZW_PROV_CHECKSUM,     /* a Z-Wave code, damaged or mistyped */
    UI_ZW_PROV_S2_ONLY,      /* its code can't join by itself: add it with its PIN */
    UI_ZW_PROV_NOT_ALLOWED,  /* installer mode is needed */
    UI_ZW_PROV_NOT_FOUND,    /* zw_prov_change: no such entry (or nothing to keep) */
} ui_zw_prov_result_t;

/* zw_prov_change's operations. */
typedef enum {
    UI_ZW_PROV_ON = 1,       /* turn it on (its failures cleared) */
    UI_ZW_PROV_OFF_OP,       /* turn it off */
    UI_ZW_PROV_FORGET,       /* remove it from the list */
    UI_ZW_PROV_GRANT,        /* the keys it may have: `arg`, a non-empty subset of what it asked */
    UI_ZW_PROV_KEEP,         /* the last removal forgot its code: put it back (any dsk) */
} ui_zw_prov_op_t;

/* ---- Zigbee (docs/features/20 devices.md §9; BL-143, the pages BL-144 and BL-145) ------------
 * What the Zigbee pages show, as states and numbers: the words are the UI's, except a model's
 * name and how to wake or pair it (zigbee_zones' model table) and the zone source's input words.
 * A device is named by its registry id (`id`, 1..65535: never 0, never given to another device
 * while the panel keeps the first one's record), which fits a page's int32_t argument - never by
 * its 64-bit address. The tier B parts (sirens, keypads, key fobs: BL-147, BL-148) are declared
 * now so the pages are drawn once. */

#define UI_ZB_DEVICES_MAX 64u /* devices the registry keeps (zbz_registry.h ZBZ_DEV_MAX) */
#define UI_ZB_ZONES_MAX   4u  /* zones one device's inputs feed, as its row lists them */
#define UI_ZB_INPUTS_MAX  8u  /* inputs one device offers on its page */
#define UI_ZB_JOINED_MAX  4u  /* devices one add shows as they arrive */
#define UI_ZB_CODES_MAX   16u /* install codes waiting (zbz_codes.h ZBZ_CODES_MAX) */
#define UI_ZB_CHANNELS    4u  /* the channels the panel offers: 11, 15, 20, 25 */
#define UI_ZB_NAME_LEN    24u /* a name the installer gives a device */
#define UI_ZB_MODEL_LEN   40u
#define UI_ZB_WORDS_LEN   64u
#define UI_ZB_CODE_TEXT   128u /* an install code as typed, or a scanned QR code's text */
#define UI_ZB_BATTERY_UNKNOWN 0xFFu
#define UI_ZB_NEVER       UINT32_MAX /* heard_s: not heard since the panel started */

/* The radio, as the installer menu's row and the Zigbee page's first row say it (pages 1, 2). */
typedef enum {
    UI_ZB_RADIO_NOT_CONNECTED = 0, /* nothing answered in the socket ("Not connected", gray) */
    UI_ZB_RADIO_STARTING,          /* its start-up runs, or runs again */
    UI_ZB_RADIO_WORKING,           /* "Channel 15 · 5 devices"; with `jammed`: "Interference" */
    UI_ZB_RADIO_NOT_ANSWERING,     /* it answered once and has gone silent: the panel retries */
    UI_ZB_RADIO_PAUSED,            /* the restart guard stopped it after restarts: `paused_why`,
                                    * "Try again" (UI_ZB_REQ_RETRY) */
    UI_ZB_RADIO_SOCKET_ZWAVE,      /* the socket is given to the Z-Wave radio */
    UI_ZB_RADIO_UNUSABLE,          /* it answers, but its firmware is not one the panel can use */
} ui_zb_radio_t;

/* Why the restart guard paused the radio (UI_ZB_RADIO_PAUSED). */
typedef enum {
    UI_ZB_PAUSE_NONE = 0,
    UI_ZB_PAUSE_FIRMWARE,  /* its firmware didn't match what the stack expects */
    UI_ZB_PAUSE_RESET,     /* it didn't come back from a reset */
    UI_ZB_PAUSE_NO_ANSWER, /* it stopped answering while the stack ran */
} ui_zb_pause_t;

typedef struct {
    uint8_t radio;              /* ui_zb_radio_t */
    uint8_t paused_why;         /* ui_zb_pause_t */
    bool jammed;                /* ZB-006: the channel is drowned out ("Interference", amber) */
    uint16_t devices;           /* devices in the registry (not endpoints) */
    uint16_t not_heard;         /* of them: unreachable now */
    uint16_t codes_waiting;     /* install codes given and not used yet (page 10) */
    uint16_t learn_s;           /* the join window's seconds left; 0 closed */
    /* the network (page 7, the Panel Manager's radio card) */
    bool network_known;
    uint8_t channel;            /* 11..26 */
    uint16_t pan_id;
    uint64_t ext_pan_id;
    uint64_t panel_ieee;        /* the panel's own Zigbee address */
    char radio_version[32];     /* the radio's own firmware, as it reports it; "" not known */
    bool noise_known;           /* the latest energy scan of the network's channel */
    int8_t noise_dbm;
    /* the channel survey (page 8): the channels 11, 15, 20, 25 in that order */
    uint8_t survey_known;       /* bit i: survey_dbm[i] measured since the panel started */
    int8_t survey_dbm[UI_ZB_CHANNELS];
    uint8_t quietest;           /* the quietest of them by that survey; 0 not known */
    uint8_t restarts;           /* restarts the guard counted (0..2) */
    bool storage_full;          /* the panel's storage is down to its reserve: new devices and
                                 * install codes are refused FULL (devices.md §4) */
} ui_zb_status_t;

typedef enum {
    UI_ZB_DEV_READY = 0,        /* set up: its inputs are offered */
    UI_ZB_DEV_SETTING_UP,       /* its setup runs ("still setting up") */
    UI_ZB_DEV_WAKE_NEEDED,      /* ... and waits for it to wake up ("Press its button to wake it",
                                 * or the model's own words) */
    UI_ZB_DEV_SETUP_STOPPED,    /* its setup gave up after its attempts: "Set it up again" */
    UI_ZB_DEV_NOT_USABLE,       /* nothing the panel uses (no usable inputs, or a protocol the
                                 * panel doesn't speak: its note says which) */
    UI_ZB_DEV_JOINING,          /* it is joining now */
} ui_zb_dev_state_t;

/* What it is, for its name when it has no zone and no model (never a brand). */
typedef enum {
    UI_ZB_KIND_DEVICE = 0,      /* not known yet */
    UI_ZB_KIND_SENSOR,          /* device_class says which (contact, motion, smoke...) */
    UI_ZB_KIND_SIREN,           /* IAS WD (ZB-023) */
    UI_ZB_KIND_KEYPAD,          /* IAS ACE with a keypad (ZB-024) */
    UI_ZB_KIND_KEYFOB,          /* IAS ACE without one (ZB-024) */
    UI_ZB_KIND_REPEATER,        /* routes only */
    UI_ZB_KIND_OTHER,           /* a light, plug, lock... the panel doesn't use yet (BL-149) */
} ui_zb_kind_t;

typedef enum {
    UI_ZB_POWER_UNKNOWN = 0,
    UI_ZB_POWER_BATTERY,
    UI_ZB_POWER_MAINS,
} ui_zb_power_t;

/* Signal from its link quality (devices.md §7 gives the buckets). */
typedef enum {
    UI_ZB_SIGNAL_UNKNOWN = 0,
    UI_ZB_SIGNAL_GOOD,
    UI_ZB_SIGNAL_FAIR,
    UI_ZB_SIGNAL_WEAK,
} ui_zb_signal_t;

/* How it joined (zsrc_zb_join_t's values). */
typedef enum {
    UI_ZB_JOIN_UNKNOWN = 0,
    UI_ZB_JOIN_INSTALL_CODE,
    UI_ZB_JOIN_STANDARD_KEY,
} ui_zb_join_t;

/* A device, as the device list shows it (page 2): one row per device, not per endpoint. */
typedef struct {
    uint16_t id;                /* its registry id: the page's argument */
    char name[UI_ZB_NAME_LEN];  /* the installer's; "" none (then its zone's name, its model) */
    char model[UI_ZB_MODEL_LEN]; /* the model table's name for it, else the model it reports */
    uint8_t state;              /* ui_zb_dev_state_t */
    uint8_t kind;               /* ui_zb_kind_t */
    uint8_t power;              /* ui_zb_power_t */
    bool router;                /* it extends the network */
    uint8_t battery_pct;        /* 0..100, UI_ZB_BATTERY_UNKNOWN */
    bool battery_low;
    uint32_t heard_s;           /* seconds since it was heard from; UI_ZB_NEVER */
    uint8_t signal;             /* ui_zb_signal_t */
    bool unreachable;           /* silent past its supervision, or gone */
    uint8_t device_class;       /* alarm_device_class_t of its first input: its icon */
    uint8_t join;               /* ui_zb_join_t */
    uint8_t setup_done, setup_total; /* its setup's steps (READY: all) */
    uint8_t zone_count;         /* zones its inputs feed (more than UI_ZB_ZONES_MAX: the first) */
    uint16_t zones[UI_ZB_ZONES_MAX];
} ui_zb_device_t;

/* One input a device offers: what a zone binds (the zone source's address and words). */
typedef struct {
    zsrc_address_t addr;
    uint8_t endpoint;
    uint8_t device_class;       /* alarm_device_class_t a zone of it starts with */
    char words[48];             /* the source's: "door contact", "endpoint 2 · water" */
    char state[24];             /* the source's, right now: "Open", "21.5 °C"; "" not known */
    uint16_t zone;              /* the zone it feeds, 0 none */
} ui_zb_input_t;

/* Whether a device can take a tier B role, and why not. */
typedef enum {
    UI_ZB_FIT_NONE = 0,         /* not that kind of device */
    UI_ZB_FIT_AVAILABLE,        /* it can */
    UI_ZB_FIT_NOT_READY,        /* its setup must finish first */
    UI_ZB_FIT_NO_DEVICE_SLOT,   /* a keypad: every device number a Zigbee keypad may have is taken
                                 * (by the other Zigbee keypads, the panel-link keypads or the
                                 * installer's device table; alarm-devices.md §4) */
} ui_zb_fit_t;

/* A siren or strobe as the panel's own (page 14, ZB-023). The same struct is the request. */
typedef struct {
    uint8_t fit;                /* ui_zb_fit_t */
    uint8_t partitions;         /* bit 0 = partition 1; 0: not used by the panel */
    bool burglary;              /* sounds for burglary and audible panic */
    bool fire;                  /* sounds for fire (its fire sound) and CO (its emergency sound) */
    bool strobe;                /* its strobe with it */
    bool squawk;                /* a chirp when the partition arms and disarms */
    uint8_t volume;             /* 0 low .. 3 very high */
    uint8_t device;             /* always 0: a siren takes no device number - its troubles, its
                                 * silence included, are the bell trouble (alarm-devices.md §4) */
    bool live;                  /* assigned and still this device */
    uint8_t last_test;          /* ui_zb_result_t of its last "Test it", NONE none */
    uint16_t supervision_min;   /* not heard this long (minutes): the bell trouble; 0 the default
                                 * (90 min mains, 3 h battery); 60 and up; never off (zbz_wd.h) */
} ui_zb_siren_t;

/* A keypad arming and disarming the panel (page 15, ZB-024): codes are always the panel's to
 * check, through the alarm task; five wrong ones lock it out like any keypad. */
typedef struct {
    uint8_t fit;                /* ui_zb_fit_t */
    uint8_t partition;          /* the partition it arms and disarms, 0: off */
    uint8_t panic_mask;         /* its emergency buttons allowed: 1 << alarm_panic_t */
    bool show_state;            /* the panel's state is sent to it (Panel Status Changed) */
    uint8_t device;             /* the device number the panel gave it (its source: lockout and
                                 * log), 0 none yet; never typed (alarm-devices.md §4) */
    bool live;
} ui_zb_keypad_t;

/* A key fob (page 16, ZB-024): it belongs to a user, and acts as that user. */
typedef struct {
    uint8_t fit;                /* ui_zb_fit_t */
    uint8_t user;               /* 1..ALARM_MAX_USERS whose fob it is; 0: off */
    uint8_t partition;          /* the partition it arms (and disarms, when it may) */
    bool may_disarm;            /* off by default: the installer turns it on per fob */
    bool panic;                 /* its panic button raises a panic; off by default */
    bool live;
} ui_zb_fob_t;

/* A device's page (page 5) and the add flow's "Use it for" (page 3). */
typedef struct {
    ui_zb_device_t dev;
    uint64_t ieee;              /* its address: the radio details and the Panel Manager only */
    uint16_t nwk;
    char manufacturer[UI_ZB_MODEL_LEN]; /* as it reports it (Basic) */
    char model_id[UI_ZB_MODEL_LEN];     /* as it reports it (Basic) */
    char sw_build[24];          /* its software version as it reports it; "" not known */
    uint8_t lqi;                /* its last frame's link quality; 0 not measured */
    int8_t rssi;                /* dBm; 0 not measured */
    uint32_t joined_at;         /* when it joined, Unix seconds; 0 not known */
    bool sleepy;                /* a sleepy end device: it answers only when it wakes */
    uint32_t check_in_s;        /* its Poll Control check-in; 0 none or not known */
    uint8_t setup_step;         /* zsrc_zb_step_t its setup is at or stopped at */
    bool can_identify;          /* it has the Identify cluster */
    char wake_words[UI_ZB_WORDS_LEN]; /* how to wake it (the model table's); "" not known */
    char add_words[UI_ZB_WORDS_LEN];  /* how to put it into pairing mode; "" not known */
    char note[UI_ZB_WORDS_LEN];       /* the quirks table's note; "" none */
    uint8_t input_count;
    ui_zb_input_t inputs[UI_ZB_INPUTS_MAX];
    ui_zb_siren_t siren;        /* fit NONE: not a siren */
    ui_zb_keypad_t keypad;      /* fit NONE: not a keypad */
    ui_zb_fob_t fob;            /* fit NONE: not a key fob */
} ui_zb_device_page_t;

typedef enum {
    UI_ZB_REQ_ADD = 1,          /* open the join window: 3 minutes (ZB-002) */
    UI_ZB_REQ_ADD_CODE,         /* `ieee` + `code`: give the code, then listen for that device */
    UI_ZB_REQ_KEEP_LISTENING,   /* adding: the full window again */
    UI_ZB_REQ_STOP,             /* stop listening (devices still setting up go on) */
    UI_ZB_REQ_REMOVE,           /* `id`: ask it to leave */
    UI_ZB_REQ_FORGET,           /* `id`: it is gone or broken - remove it without its answer */
    UI_ZB_REQ_REPLACE,          /* `id`: listen; the next device to join takes its zones */
    UI_ZB_REQ_PING,             /* `id`: "Check it now" */
    UI_ZB_REQ_IDENTIFY,         /* `id`: blink its own light */
    UI_ZB_REQ_SETUP,            /* `id`: "Set it up again" (read and configure it again) */
    UI_ZB_REQ_RENAME,           /* `id`: `name` ("" its default) */
    UI_ZB_REQ_CODE_ADD,         /* `ieee` + `code`: keep a code until its device joins */
    UI_ZB_REQ_CODE_FORGET,      /* `ieee`: forget a waiting code */
    UI_ZB_REQ_CHANNEL,          /* move the network to `channel` (11, 15, 20, 25) */
    UI_ZB_REQ_SURVEY,           /* measure the channels for the Channel page */
    UI_ZB_REQ_RESET,            /* reset the network: the UI asked twice, and `installer_code` */
    UI_ZB_REQ_RESTART,          /* restart the radio */
    UI_ZB_REQ_RETRY,            /* the radio is paused: try again */
    UI_ZB_REQ_DISMISS,          /* the ended operation was seen: zb_progress returns to idle */
    UI_ZB_REQ_SIREN,            /* `id`: `siren` (tier B, BL-147) */
    UI_ZB_REQ_SIREN_TEST,       /* `id`: a short sound */
    UI_ZB_REQ_KEYPAD,           /* `id`: `keypad` (tier B, BL-148) */
    UI_ZB_REQ_FOB,              /* `id`: `fob` (tier B, BL-148) */
} ui_zb_req_kind_t;

typedef struct {
    uint8_t kind;               /* ui_zb_req_kind_t */
    uint8_t user;               /* who asks (the event log) */
    uint8_t via;                /* where (the event log's source): UI_ZB_VIA_PANEL, the touchscreen
                                 * or the console; UI_ZB_VIA_WEB, the Panel Manager (set by the
                                 * backend, never by what the browser sends) */
    uint16_t id;                /* the device, 0: none */
    uint64_t ieee;              /* ADD_CODE, CODE_ADD, CODE_FORGET: the address on its label */
    char code[UI_ZB_CODE_TEXT]; /* ADD_CODE, CODE_ADD: the code as typed (hex; spaces and dashes
                                 * allowed) or a scanned QR code's text (then `ieee` may be 0) */
    char name[UI_ZB_NAME_LEN];  /* RENAME */
    uint8_t channel;            /* CHANNEL */
    ui_zb_siren_t siren;        /* SIREN: its settings (fit, device, live, last_test ignored) */
    ui_zb_keypad_t keypad;      /* KEYPAD (fit, device, live ignored: the panel gives the number) */
    ui_zb_fob_t fob;            /* FOB (fit, live ignored) */
    char installer_code[ALARM_CODE_MAX_LEN + 1]; /* RESET: the installer code typed again
                                 * (ZB-017) on the touchscreen or the console, checked by the
                                 * backend with the engine before anything else and wiped; ""
                                 * for every other kind. The Panel Manager never sends a code: it
                                 * re-asks the password (devices.md §14) */
} ui_zb_request_t;

#define UI_ZB_VIA_PANEL 0u
#define UI_ZB_VIA_WEB   1u

typedef enum {
    UI_ZB_REQ_OK = 0,           /* taken: zb_progress follows it (settings: done) */
    UI_ZB_REQ_NOT_ALLOWED,      /* installer mode with every partition disarmed is needed (PING,
                                 * IDENTIFY, SURVEY, DISMISS: installer mode only) */
    UI_ZB_REQ_BUSY,             /* another operation runs */
    UI_ZB_REQ_NO_RADIO,         /* the radio isn't working */
    UI_ZB_REQ_INVALID,          /* not now, not that device, or a value out of range */
    UI_ZB_REQ_UNSUPPORTED,      /* this panel (or this radio) can't do it */
    UI_ZB_REQ_BAD_CODE,         /* an install code of the wrong length, or not hex */
    UI_ZB_REQ_CHECKSUM,         /* an install code whose CRC doesn't match: a typo */
    UI_ZB_REQ_BAD_ADDRESS,      /* not 16 hex digits, or 0 */
    UI_ZB_REQ_FULL,             /* no room for another code or device (the registry, the codes, the
                                 * panel's storage: status.storage_full), or no device number left
                                 * for a keypad */
    UI_ZB_REQ_NOT_FOUND,        /* no such device or code */
    UI_ZB_REQ_IN_USE,           /* the role is another device's (a user's fob) */
    UI_ZB_REQ_WRONG_CODE,       /* RESET: not the installer's code, or none (it counts toward this
                                 * keypad's lockout like any wrong code) */
    UI_ZB_REQ_LOCKED_OUT,       /* RESET: too many wrong codes here; try again later */
} ui_zb_req_result_t;

/* The operation zb_progress reports (one at a time). */
typedef enum {
    UI_ZB_OP_NONE = 0,
    UI_ZB_OP_ADD,
    UI_ZB_OP_ADD_CODE,
    UI_ZB_OP_REMOVE,
    UI_ZB_OP_FORGET,
    UI_ZB_OP_REPLACE,
    UI_ZB_OP_PING,
    UI_ZB_OP_IDENTIFY,
    UI_ZB_OP_SETUP,
    UI_ZB_OP_CHANNEL,
    UI_ZB_OP_SURVEY,
    UI_ZB_OP_RESET,
    UI_ZB_OP_RESTART,
    UI_ZB_OP_SIREN_TEST,
} ui_zb_op_t;

typedef enum {
    UI_ZB_STEP_IDLE = 0,
    UI_ZB_STEP_STARTING,        /* the radio is told to listen */
    UI_ZB_STEP_LISTENING,       /* "Listening": `seconds_left`, once a second */
    UI_ZB_STEP_JOINING,         /* a device answered and joins */
    UI_ZB_STEP_READING,         /* "Reading what it can do" */
    UI_ZB_STEP_SETTING_UP,      /* "Setting it up" (enroll, bind, reporting) */
    UI_ZB_STEP_WORKING,         /* asked it to leave, a ping, a channel move, a reset... */
    UI_ZB_STEP_DONE,            /* it ended: `result` */
} ui_zb_step_t;

typedef enum {
    UI_ZB_RES_NONE = 0,
    UI_ZB_RES_ADDED,            /* in the network and set up: "Use it for" */
    UI_ZB_RES_NOTHING_JOINED,   /* the window ran out */
    UI_ZB_RES_JOIN_FAILED,      /* "Didn't join" (it started and didn't finish) */
    UI_ZB_RES_ALREADY_ADDED,    /* it was in the network already: `id` */
    UI_ZB_RES_NOT_A_SENSOR,     /* in the network, nothing the panel uses (no usable inputs) */
    UI_ZB_RES_SETUP_NOT_FINISHED, /* in the network, its setup didn't finish: "Set it up again" */
    UI_ZB_RES_STOPPED,          /* the installer stopped it */
    UI_ZB_RES_REMOVED,          /* "Removed": `zones` now need a sensor */
    UI_ZB_RES_NO_ANSWER,        /* it didn't answer (remove: "Forget it anyway?"; a ping) */
    UI_ZB_RES_FORGOTTEN,        /* removed without its answer: `zones` need a sensor */
    UI_ZB_RES_REPLACED,         /* `id` took the old device's `zones` */
    UI_ZB_RES_ANSWERED,         /* a ping */
    UI_ZB_RES_DONE,             /* identify sent, set up again, channel moved, reset, restart */
    UI_ZB_RES_FAILED,           /* it could not be done (the radio went away, it refused) */
    UI_ZB_RES_FULL,             /* a device joined with no room left: it was asked to leave */
} ui_zb_result_t;

/* A device that joined during this add, as its row shows it. */
typedef struct {
    uint16_t id;                /* 0: this row is empty */
    uint8_t step;               /* ui_zb_step_t it is at (JOINING .. DONE) */
    uint8_t result;             /* DONE: ui_zb_result_t */
    bool asleep;                /* it fell asleep before its setup finished: the wake box */
    char model[UI_ZB_MODEL_LEN];
} ui_zb_joined_t;

typedef struct {
    uint8_t op;                 /* ui_zb_op_t */
    uint8_t step;               /* ui_zb_step_t */
    uint8_t result;             /* UI_ZB_STEP_DONE: ui_zb_result_t */
    uint32_t seconds_left;      /* LISTENING's countdown; 0 none */
    uint16_t id;                /* the device it concerns (REMOVE, FORGET, PING...; REPLACE: the
                                 * new one); 0 none */
    uint16_t old_id;            /* REPLACE: the device replaced */
    char model[UI_ZB_MODEL_LEN]; /* that device's model (removed: as it was) */
    uint8_t joined_count;       /* ADD, ADD_CODE: devices that joined, newest last */
    ui_zb_joined_t joined[UI_ZB_JOINED_MAX];
    uint8_t zone_count;         /* REMOVED, FORGOTTEN, REPLACED: the zones concerned */
    uint16_t zones[UI_ZB_ZONES_MAX];
} ui_zb_progress_t;

/* An install code waiting for its device (page 10). The code itself is never shown again. */
typedef struct {
    uint64_t ieee;
    uint32_t added_at;          /* Unix seconds; 0 the clock wasn't set */
} ui_zb_code_t;

/* ---- Smart Home (docs/features/24, BL-045): the devices the panel controls ---- */

/* What a device is doing, in words the screen knows (ui_dev_model.h reads a source's words). */
typedef enum {
    UI_DEV_ST_UNKNOWN = 0,
    UI_DEV_ST_OFF, /* a light, switch or fan; a thermostat switched off */
    UI_DEV_ST_ON,
    UI_DEV_ST_LOCKED,
    UI_DEV_ST_UNLOCKED,
    UI_DEV_ST_JAMMED,
    UI_DEV_ST_LOCKING,
    UI_DEV_ST_UNLOCKING,
    UI_DEV_ST_CLOSED, /* a cover or a valve */
    UI_DEV_ST_OPEN,
    UI_DEV_ST_CLOSING,
    UI_DEV_ST_OPENING,
    UI_DEV_ST_HEAT, /* a thermostat's mode */
    UI_DEV_ST_COOL,
    UI_DEV_ST_AUTO,
} ui_dev_state_t;

/* Where its last action stands (HCTL-006, HCTL-012). */
typedef enum {
    UI_DEV_IDLE = 0,
    UI_DEV_PENDING, /* an action is under way: `action` says which */
    UI_DEV_FAILED,  /* the last action failed or timed out: `action` says which; a tap tries again */
    UI_DEV_OFFLINE, /* its source can't reach it: its words stay readable, its chip dims and its
                     * controls rest (HCTL-006, BL-181); `since_ms` says since when */
} ui_dev_status_t;

#define UI_DEV_CAP_LEVEL    0x01u /* a light's brightness or a cover's position can be set */
#define UI_DEV_CAP_ENTRANCE 0x02u /* a cover that is a way in (a garage door): green closed, amber open */
/* Negative on purpose: a ui_dev_t from a producer that doesn't know (the simulator, a test, an
 * older integration's device) keeps the Stop and speed keys, as the wire's "absent = everything". */
#define UI_DEV_CAP_NO_STOP  0x04u /* a cover that can't stop (its source says): no Stop key */
#define UI_DEV_CAP_NO_SPEED 0x08u /* a fan without speeds (its source says): on and off only, as a switch */
/* A thermostat's modes it reports it has (ui_dev_t.modes; 0: not known, the sheet shows no mode keys).
 * dev_act's ACT_SET_MODE takes one of these as a ui_dev_state_t in `value`: UI_DEV_ST_OFF, _HEAT,
 * _COOL or _AUTO (the port turns it into the source's word). */
#define UI_DEV_MODE_OFF  0x01u
#define UI_DEV_MODE_HEAT 0x02u
#define UI_DEV_MODE_COOL 0x04u
#define UI_DEV_MODE_AUTO 0x08u
#define UI_DEV_TEMP_UNKNOWN INT32_MIN

/* One device, as the Smart Home screen shows it. */
typedef struct {
    uint8_t kind;          /* dev_kind_t */
    uint8_t state;         /* ui_dev_state_t */
    uint8_t status;        /* ui_dev_status_t */
    uint8_t action;        /* dev_action_t pending or failed, ACT_NONE when not known */
    uint8_t caps;          /* UI_DEV_CAP_* */
    int8_t level;          /* brightness, fan speed or position 0..100, -1 not known */
    uint8_t modes;         /* a thermostat's UI_DEV_MODE_* it has, 0 not known */
    uint8_t source;        /* dsrc_kind_t it comes from (an automation's action names it,
                            * docs/features/41) */
    uint8_t hvac_action;   /* a thermostat's dev_hvac_t (what it is doing, its source's hvac_action);
                            * DEV_HVAC_UNKNOWN when not known or not a thermostat (BL-171) */
    uint8_t unsure;        /* UI_DEV_FAILED only: 1 when whether it happened isn't known - no answer
                            * in time, its connection dropped with it in flight, or its source
                            * sent it and couldn't confirm it (HCTL-012) */
    int32_t setpoint;      /* a thermostat's, hundredths of a degree; UI_DEV_TEMP_UNKNOWN */
    int32_t temperature;   /* what it measures now, hundredths of a degree; UI_DEV_TEMP_UNKNOWN */
    int64_t since_ms;      /* epoch ms: offline since, or a scene last ran; 0 not known */
    char ref[DEV_REF_LEN]; /* the registry's key, unique */
    char name[DEV_NAME_LEN];
    char area[DEV_AREA_LEN]; /* "" none */
} ui_dev_t;
#ifndef __cplusplus
/* 224 bytes: three arrays of UI_DEVL_MAX hold it (Smart Home, the automations' list and editor), in
 * PSRAM on the P4 (ui_slow_alloc / UI_SLOW_BSS); 6 bytes of padding are left (after `unsure`, after
 * `temperature`). On a board without PSRAM (the 4", on hold) the 1.5 KB growth is internal RAM. */
_Static_assert(sizeof(ui_dev_t) == 224u, "ui_dev_t grew: three arrays of UI_DEVL_MAX hold it");
#endif

/* What became of an action asked for (dev_act). */
typedef enum {
    UI_DEV_OK = 0,       /* sent: the device's status follows it */
    UI_DEV_NEED_CODE,    /* the action needs a user code (HCTL-005): ask, then send it again with it */
    UI_DEV_CODE_REFUSED, /* the code wasn't accepted: code_result says why (lockout included) */
    UI_DEV_BUSY,         /* an action is already under way on it */
    UI_DEV_UNREACHABLE,  /* its source is offline */
    UI_DEV_REFUSED,      /* no such device, or the action doesn't fit it */
} ui_dev_result_t;

/* This keypad's preferences. The UI changes them in Settings and hands them to
 * backend.save_options; the platform stores them. */
typedef struct {
    uint8_t partition;        /* partition this keypad shows (default 1) */
    uint8_t clock_mode;       /* 0 the region's clock, 1 12 h, 2 24 h (i18n's I18N_CLOCK_*;
                                 default 0: 12 h with AM/PM in the US) */
    uint8_t brightness;       /* backlight when awake, 10..100 (default 80) */
    uint8_t saver_brightness; /* backlight of the screen saver, 0 = off (default 25) */
    uint32_t saver_after_ms;  /* idle time before the screen saver, 0 = never (default 60 s) */
    /* The screen goes dark (UI-043; ui_light.h): idle time before the backlight goes off,
     * 0 = never (default). Counted from the last touch, like the saver's: the first of the two
     * wins, so a shorter one goes straight to dark. */
    uint32_t off_after_ms;
    /* Night hours (UI-043): between night_start and night_end (minutes after midnight, local
     * time; wrapping past midnight) the screen is dimmer and goes dark after its own time. */
    bool night_on;              /* default false */
    uint16_t night_start;       /* 0..1439 (default 22:00) */
    uint16_t night_end;         /* 0..1439 (default 07:00); equal to night_start: never night */
    uint8_t night_brightness;   /* backlight when awake at night, 10..100 (default 20) */
    uint32_t night_off_after_ms; /* off_after_ms at night, 0 = never (default 60 s) */
    uint8_t volume;           /* Beeps volume: keypad sounds except alarms, the chime and the
                               * voice, 0..100 (default 70; docs/features/08 AUD-032) */
    uint8_t chime_volume;     /* Chime volume: the door chime alone, 0..100 (default 70) */
    uint8_t voice_volume;     /* Voice volume: announcements and Preview, 0..100 (default 70) */
    bool key_click;           /* touch feedback (default true) */
    bool run_setup;           /* start with the first-boot setup (default false) */
    char lang[16];            /* the language, a BCP 47 tag (default "en-US"; docs/features/37) */
    char region[4];           /* the region for formats, ISO 3166 ("DE"); "" = the language's */
    /* How the screen looks (docs/features/07 UI-010, BL-132; ui_light.h ui_light_theme): a
     * UI_APPEARANCE_* (default Dark, the look before it), and High contrast with either. */
    uint8_t appearance;
    bool high_contrast;       /* default false */
    /* Announcements (docs/features/08 voice-first-build.md D06; Settings > Screen and sound >
     * Announcements): the main switch is on as shipped (decided 2026-10-12) and a person switches
     * it on again only while the voice is ready (ui_voice_info_t.state UI_VOICE_READY); the kinds
     * keep their choice while it is off. Each panel's own, a link keypad's too (LINK-028). */
    bool voice_enabled;       /* default true */
    bool voice_status;        /* arming, armed, disarmed, entry, not ready (default true) */
    bool voice_chime;         /* the zone's spoken name after the chime (default true) */
    uint8_t partition_home; /* saved default, distinct from the current view */
    bool switch_allowed;
    bool return_never;
    uint16_t return_s; /* default 30, 1..3600 */
} ui_options_t;

/* A change of these preferences from elsewhere (docs/features/07 UI-046; docs/features/11
 * "Settings fields", "Applying a change from elsewhere"): Home Assistant through the master, or
 * the master's console over the panel link. Each field by its frozen settings id (ui_prefs.h
 * UI_PREFS_F_*, alarm_proto's PROTO_PREFS_F_* numbers), its value in the wire's units: a delay
 * in ms, a switch 0 or 1. ui_backend_t.prefs_remote_take hands one to the screen. */
#define UI_PREFS_F_LAST 19u /* the highest settings id this firmware knows */
typedef struct {
    uint32_t have;                   /* bit id - 1: value[id - 1] holds that field's new value */
    uint32_t value[UI_PREFS_F_LAST]; /* index id - 1 */
} ui_prefs_patch_t;

/* The voice (docs/features/08 voice-first-build.md D03, D06, D09): ui_backend_t.voice_info. */
typedef enum {
    UI_VOICE_OFF = 0,     /* no voice in this build: no Spoken name row, no Announcements page */
    UI_VOICE_NONE,        /* no voice pack installed */
    UI_VOICE_LOADING,     /* installed, being checked at start-up */
    UI_VOICE_READY,       /* installed, checked, decoder ready */
    UI_VOICE_UNAVAILABLE, /* installed but not usable */
} ui_voice_state_t;

typedef struct {
    uint8_t state;                   /* ui_voice_state_t */
    bool draft;                      /* the pack's takes are not yet accepted */
    char locale[VP_LOCALE_SIZE];     /* the voice's language, e.g. "en-US" ("" without one) */
    char voice_name[VP_VOICE_ID_SIZE]; /* the pack's voice id: data, never translated */
    uint32_t pack_revision;          /* 0 without a pack */
} ui_voice_info_t;

/* ui_backend_t.voice_preview's answer. */
typedef enum {
    UI_VOICE_PREVIEW_OK = 0,      /* queued: plays after anything more important */
    UI_VOICE_PREVIEW_UNAVAILABLE, /* the voice is not ready */
    UI_VOICE_PREVIEW_BAD_WORDS,   /* empty, over 8 entries, or a word the pack doesn't have */
    UI_VOICE_PREVIEW_TOO_LONG,    /* over 10 s with its joins (D02) */
    UI_VOICE_PREVIEW_QUIET,       /* volume 0, power saving or an alarm: nothing plays */
} ui_voice_preview_t;

/* What the spoken-name editor showed when its Done was tapped (D07): the configuration's
 * generation and the zone's stored binding revision (ui_backend_t.voice_seen) and the revision of
 * the pack whose words the editor displayed (its own lease). An approval is granted only against
 * this identity, never against whatever is current when it runs. */
typedef struct {
    uint32_t generation;
    uint32_t binding_revision;
    uint32_t pack_revision;
} ui_voice_seen_t;

/* ui_backend_t.voice_apply_approve's answer. */
typedef enum {
    UI_VOICE_APPROVE_OK = 0,
    UI_VOICE_APPROVE_STALE,       /* the zone's stored words or binding changed meanwhile */
    UI_VOICE_APPROVE_UNAVAILABLE, /* the voice is not ready, or the words can't be said */
    UI_VOICE_APPROVE_REFUSED,     /* not allowed now (not in installer mode, armed, busy) */
} ui_voice_approve_t;

/* ui_options_t.appearance: stored by the platform and sent by the Panel Manager as these numbers
 * ("appearance"): never renumber. Anything else reads as Dark. */
enum {
    UI_APPEARANCE_DARK = 0,  /* light words on a dark screen (the default) */
    UI_APPEARANCE_LIGHT = 1, /* dark words on a light screen */
    UI_APPEARANCE_AUTO = 2,  /* dark during the night hours, light by day (UI-043) */
};

/* ---- The panel link: master and keypads (docs/features/11, BL-048) --------------------------
 * A panel is the alarm panel itself (the master, every panel before BL-048) or a keypad for a
 * master elsewhere in the house. The pages read the link through these four entries
 * (ui_backend_t.link_*), polled at most once a second (UI-027); NULL entries mean a firmware
 * without the link: nothing about keypads is shown and the setup has no role step. */

/* ui_link_info_t.role */
enum {
    UI_LINK_ROLE_MASTER = 0, /* the alarm panel (the default) */
    UI_LINK_ROLE_KEYPAD = 1, /* a keypad: the master-only pages are hidden (the role gate) */
};

/* ui_link_info_t.state on a keypad (plink_link_state_t, docs/features/11 LINK-008). */
enum {
    UI_LINK_NOT_PAIRED = 0, /* no master: "Pair again" / the setup's pairing step */
    UI_LINK_CONNECTING,     /* after a start or a pairing, until the first full sync */
    UI_LINK_ONLINE,
    UI_LINK_LOST,           /* "No system communication": arming off, Panic still tries */
};

typedef struct {
    uint8_t role;        /* UI_LINK_ROLE_* */
    /* A keypad: */
    uint8_t state;       /* UI_LINK_* */
    bool removed;        /* NOT_PAIRED because the master removed it (the "removed" page) */
    bool maintenance;    /* LINK-025: 10 min without system communication - Wi-Fi, updates, Pair
                          * again and "Make it a stand-alone panel" open without a code */
    bool needs_update;   /* the master's configuration is newer than this keypad can read: Home
                          * says so and Software update opens without a code */
    bool panic_pending;  /* a panic is still being sent (LINK-023: up to 1 min) */
    uint8_t device;      /* its keypad number at the master, 0 none */
    uint8_t home;        /* the partition it shows and arms */
    uint32_t home_gen;   /* changes when the master changes the home partition */
    int64_t since_epoch_ms; /* LOST: the last time the master answered (0 not known) */
    char master_name[48];   /* the alarm panel's name, e.g. "Harbor House" */
    /* A master: */
    uint8_t keypads;     /* keypads paired with it */
    uint8_t keypads_down;/* of those, not responding */
    /* Appended (the screens, BL-048): a keypad's alarm panel's address on the network, as About
     * shows it ("192.0.2.42"); "" not known (About leaves the row out). */
    char master_addr[24];
    /* Appended (BL-048 review): a keypad that lost the link keeps sounding a fire or CO tone until
     * OFF is pressed here or the link returns (decided 2026-10-15): while true, Home offers
     * Silence, which sends ALARM_CMD_SILENCE through command() and stops the tone here even when
     * the alarm panel can't be reached. */
    bool local_alarm;
    /* The home partition's name as the alarm panel calls it, "" not known: the setup's last step
     * on a panel becoming a keypad (its own configuration is not the alarm panel's). */
    char home_name[ALARM_NAME_LEN];
    /* Appended (keypad updates, docs/features/11 "Updates through the link", proposal P2): a
     * keypad whose alarm panel sends it its updates (paired with a master whose session offers
     * the link's updates, PLINK_CAP_FW): it installs only at its master's order. While true,
     * and outside LINK-025's maintenance and the needs_update state (where its own Install
     * stays), Software update says "Updates come from <master_name>" with no Install, Check
     * again or Stop, About's Software update row says "From <master_name>", and the status bar's
     * update banner says it restarts with master_name. The keypad's own run (stage, bytes, size,
     * target) is read through fw_status as on any panel. */
    bool fw_follows;
    /* With fw_follows, while fw_status reports a download (UI_FW_STAGE_DOWNLOADING): true when
     * the image comes from the alarm panel over the link (a relay, "Receiving from <master>"),
     * false when the keypad downloads it from the update server itself ("Downloading"). */
    bool fw_from_master;
    bool switch_allowed;
    bool return_never;
    uint16_t return_s;
    uint8_t selected;
} ui_link_info_t;

/* Local enrollment progress; never includes network secrets. */
enum {
    UI_LINK_WIFI_IDLE = 0,
    UI_LINK_WIFI_REQUESTING,
    UI_LINK_WIFI_JOINING,
    UI_LINK_WIFI_CONNECTED,
    UI_LINK_WIFI_UNAVAILABLE,
};

/* ui_link_pair_t.state: either side's pairing (plink_pair_state_t). */
enum {
    UI_LINK_PAIR_IDLE = 0,
    UI_LINK_PAIR_OPEN,    /* master: waiting for a keypad (left_s); keypad: looking / asked */
    UI_LINK_PAIR_WORKING, /* the two are exchanging keys */
    UI_LINK_PAIR_COMPARE, /* code shown: the person compares and answers */
    UI_LINK_PAIR_WAITING, /* this side said yes: waiting for the other */
    UI_LINK_PAIR_DONE,
    UI_LINK_PAIR_FAILED,  /* fail says why */
};

/* ui_link_pair_t.fail (plink_fail_t's numbers). */
enum {
    UI_LINK_FAIL_NONE = 0,
    UI_LINK_FAIL_REFUSED = 1,
    UI_LINK_FAIL_TIMEOUT = 2,
    UI_LINK_FAIL_MISMATCH = 3,
    UI_LINK_FAIL_BUSY = 4,
    UI_LINK_FAIL_CLOSED = 5,
    UI_LINK_FAIL_FULL = 6,
    UI_LINK_FAIL_OVERLAP = 7,
    UI_LINK_FAIL_LIMIT = 8,
    UI_LINK_FAIL_CAPACITY = 9,
    UI_LINK_FAIL_CANCELLED = 10,
    UI_LINK_FAIL_INTERRUPTED = 11,
    UI_LINK_FAIL_STORE = 12,
    UI_LINK_FAIL_VERSION = 13,
    UI_LINK_FAIL_NOT_FOUND = 100, /* keypad: no alarm panel open for pairing was found */
    UI_LINK_FAIL_TWO_MASTERS = 101, /* keypad: two alarm panels are open for pairing */
};

typedef struct {
    uint8_t state;       /* UI_LINK_PAIR_* */
    uint8_t fail;        /* UI_LINK_FAIL_* when FAILED */
    uint8_t attempts;    /* used of 3 */
    uint32_t left_s;     /* master: the window's seconds left; keypad: looking for this long */
    char code[8];        /* "588460" while COMPARE or WAITING, else "" */
    char peer_name[48];  /* the keypad's or the alarm panel's name */
    char peer_addr[24];  /* its address on the network, e.g. "192.0.2.57" */
    uint8_t device;      /* DONE: the keypad's number */
    /* Enrollment Wi-Fi: 0 idle/manual, 1 requesting, 2 joining, 3 connected, 4 unavailable. */
    uint8_t wifi_setup;
    bool online; /* authenticated, fresh master status; required for radio-only Done */
} ui_link_pair_t;

/* ui_link_keypad_t.fw_state: a keypad's part in this panel's updates (docs/features/11 "Updates
 * through the link"; proposal P2), one status line of its row in the master's Software update. The
 * master gives its keypads a target version: the one it installs, or the one it runs when it runs
 * no update. */
enum {
    UI_LINK_FW_NONE = 0,    /* not known yet (not heard, or it doesn't take the link's updates):
                             * its row has no status line */
    UI_LINK_FW_UP_TO_DATE,  /* it runs the target: "Up to date" (green) */
    UI_LINK_FW_DOWNLOADING, /* it downloads the target from the update server itself, fw_percent
                             * done: "Downloading · 45 %" (blue) */
    UI_LINK_FW_RECEIVING,   /* it receives the target from this panel over the link, fw_percent
                             * done: "Receiving from this panel · 30 %" (blue) */
    UI_LINK_FW_READY,       /* it has the target, checked, and restarts together with this panel
                             * once everything is disarmed: "Ready · restarts with this panel" */
    UI_LINK_FW_RESTARTING,  /* it restarts with the update (up to 10 minutes, its supervision
                             * trouble held meanwhile): "Restarting" here, and "Updating" instead of
                             * "Not responding" in Installer > Keypads and devices */
    UI_LINK_FW_NOT_UPDATED, /* behind the target and not getting it now, whatever the reason (a
                             * failed download, a refusal, it went back): it tries again by itself.
                             * "Not updated yet · tries again by itself" (gray) */
    UI_LINK_FW_NEWER,       /* it runs a newer version than the target: left alone. "Newer than
                             * this panel" (gray) */
};

/* One keypad at the master (Installer > Keypads and devices). */
typedef struct {
    uint8_t device;         /* 1..8 */
    char name[48];
    uint8_t home;           /* its home partition */
    uint16_t supervision_s; /* 0 = not supervised */
    bool connected;         /* heard within its supervision time */
    bool down;              /* not responding: its trouble is raised */
    bool waiting;           /* paired, never connected yet */
    int64_t heard_epoch_ms; /* last heard (0 not known) */
    char addr[24];
    char fw[32];            /* the version it runs, "" not known; its row in Software update says
                             * "Keypad 2 · version 0.6.1" */
    /* Appended (keypad updates, proposal P2): its part in this panel's updates. */
    uint8_t fw_state;       /* UI_LINK_FW_* */
    uint8_t fw_percent;     /* DOWNLOADING and RECEIVING: 0..100 done; the screen reads it at most
                             * once a second (UI-020) */
    bool switch_allowed;
    bool return_never;
    uint16_t return_s;
} ui_link_keypad_t;

typedef enum {
    /* the master, installer mode with every partition disarmed: */
    UI_LINK_REQ_PAIR_OPEN = 1,    /* open the 2-minute window */
    UI_LINK_REQ_PAIR_DECIDE,      /* `allow`: the installer compared the codes */
    UI_LINK_REQ_PAIR_STOP,        /* close the window / end the attempt */
    UI_LINK_REQ_KEYPAD_SET,       /* `device`: `name`, `home`, `supervision_s` */
    UI_LINK_REQ_KEYPAD_REMOVE,    /* `device` (LINK-014) */
    /* a panel becoming a keypad (the setup), or a keypad pairing again: */
    UI_LINK_REQ_KP_PAIR_START,    /* look for an alarm panel open for pairing and ask it */
    UI_LINK_REQ_KP_PAIR_CONFIRM,  /* `allow`: the codes match (true) or not */
    UI_LINK_REQ_KP_PAIR_RETRY,    /* try again after a failure */
    UI_LINK_REQ_KP_PAIR_STOP,     /* give up (the setup's Back) */
    UI_LINK_REQ_KP_DONE,          /* the pairing's Done: store the role and restart as a keypad */
    UI_LINK_REQ_PARTITION_POLICY, /* installer-only per-keypad switching policy */
    UI_LINK_REQ_KP_WIFI, /* allow: request automatic credentials; false cancels for manual Wi-Fi */
} ui_link_request_t;

typedef struct {
    uint8_t kind; /* ui_link_request_t */
    bool allow;
    uint8_t device;
    uint8_t home;
    uint16_t supervision_s;
    char name[48];
    bool switch_allowed;
    bool return_never;
    uint16_t return_s;
    /* Optional browser confirmation binding. Rechecked by the link task, not a cached UI. */
    char pair_code[8];
    uint8_t pair_attempts;
} ui_link_req_t;

/* What link_request answers. */
typedef enum {
    UI_LINK_REQ_OK = 0,
    UI_LINK_REQ_NOT_ALLOWED, /* the master: installer mode with every partition disarmed needed */
    UI_LINK_REQ_FULL,        /* PAIR_OPEN: keypad numbers 1-8 are all in use */
    UI_LINK_REQ_BUSY,        /* something else is under way */
    UI_LINK_REQ_FAILED,      /* it couldn't be done (storage) */
    UI_LINK_REQ_REFUSED,     /* not in this role, or a bad argument */
} ui_link_result_t;

/* What the UI needs from the platform. Commands block until the engine has answered (with
 * run_later, the UI calls them off its own task: LINK-054).
 * The M4 entries (settings, installer mode, users, event log, network) are optional: a NULL
 * entry hides what depends on it. */
typedef struct {
    void *ctx;
    uint32_t (*snapshot_seq)(void *ctx);
    void (*get_snapshot)(void *ctx, alarm_snapshot_t *out);
    bool (*get_config)(void *ctx, alarm_config_t *out);
    size_t (*recent_events)(void *ctx, alarm_event_t *out, size_t max); /* newest first */
    alarm_ms_t (*now_ms)(void *ctx);                                    /* engine time */
    int64_t (*epoch_ms)(void *ctx);                                     /* 0 = clock not set */
    alarm_result_t (*command)(void *ctx, const alarm_cmd_t *cmd, alarm_cmd_result_t *res);
    /* Authenticates current_code and gives that user new_code. */
    alarm_result_t (*change_code)(void *ctx, const char *current_code, const char *new_code,
                                  uint8_t *user_out);
    ui_net_state_t (*net_state)(void *ctx, int *rssi_out);
    void (*beep)(void *ctx, ui_beep_t beep);       /* optional */
    void (*backlight)(void *ctx, uint8_t percent); /* optional */

    /* ---- codes, installer mode and users ---- */
    alarm_result_t (*authenticate)(void *ctx, const char *code, uint8_t *user_out);
    /* code NULL or "": power-up recovery (no code during the first minutes after start). */
    alarm_result_t (*program_enter)(void *ctx, const char *code, uint8_t *user_out);
    void (*program_exit)(void *ctx);
    void (*program_keepalive)(void *ctx);
    alarm_result_t (*program_set_code)(void *ctx, uint8_t user, const char *new_code);
    alarm_result_t (*set_config)(void *ctx, const alarm_config_t *cfg);
    alarm_result_t (*user_set)(void *ctx, const char *actor_code, uint8_t user,
                               const char *new_code, alarm_authority_t authority,
                               uint8_t partitions, const char *name);
    alarm_result_t (*user_update)(void *ctx, const char *actor_code, uint8_t user,
                                  alarm_authority_t authority, uint8_t partitions,
                                  const char *name);
    alarm_result_t (*user_delete)(void *ctx, const char *actor_code, uint8_t user);

    /* ---- event log (see event_log/elog.h) ---- */
    size_t (*log_read)(void *ctx, uint32_t before, elog_filter_fn filter, void *filter_ctx,
                       elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_before);
    uint32_t (*boot_id)(void *ctx); /* this boot's number in the log */
    /* What the log could not keep (LOG-010), latched until cleared: records = events never
     * written, reports = alerts never kept for replay. Both optional: NULL = not offered. */
    void (*log_loss)(void *ctx, uint32_t *records, uint32_t *reports);
    void (*log_clear_loss)(void *ctx); /* also drops the storage trouble it held */

    void (*power_info)(void *ctx, ui_power_info_t *out);

    /* ---- network and time ---- */
    void (*wifi_info)(void *ctx, ui_wifi_info_t *out);
    bool (*wifi_scan)(void *ctx); /* start; false when the radio isn't ready */
    size_t (*wifi_results)(void *ctx, ui_wifi_ap_t *out, size_t max, bool *done);
    void (*wifi_connect)(void *ctx, const char *ssid, const char *password); /* "" forgets */
    /* Address settings. Optional: without them Settings > Wi-Fi has no cog. */
    void (*ip_get)(void *ctx, ui_ip_config_t *out);
    /* Checks, stores and applies. false: rejected (why: a short reason, may be NULL). */
    bool (*ip_set)(void *ctx, const ui_ip_config_t *in, char *why, size_t why_len);
    /* Checking the connection. Optional: without them the check button is left out. */
    bool (*net_check_start)(void *ctx); /* false: no radio, or a check is already running */
    void (*net_check_status)(void *ctx, ui_netcheck_t *out);
    void (*get_timezone)(void *ctx, char *out, size_t len);
    void (*set_timezone)(void *ctx, const char *posix_tz);
    void (*get_ntp)(void *ctx, char *out, size_t len);
    void (*set_ntp)(void *ctx, const char *server);
    /* The battery-backed clock module (docs/features/35). Optional: a NULL clock_info shows nothing
     * about it, and the time can be set by hand only when clock_set exists and a module answers
     * (ui_clock_settable). clock_set takes UTC ms and the user setting it (for the log); false:
     * why says why, in words. */
    void (*clock_info)(void *ctx, ui_clock_info_t *out);
    bool (*clock_set)(void *ctx, int64_t epoch_ms, uint8_t user, char *why, size_t why_len);

    /* ---- zone sources (installer: Zones; docs/features/22). Optional: NULL keeps the zone editor
     * without source rows ---- */
    size_t (*zone_sources)(void *ctx, ui_zone_source_t *out, size_t max);
    /* Listen for sensors (open and close a contact) while a picker shows them. */
    void (*zone_learn)(void *ctx, uint8_t kind, bool on);
    size_t (*zone_candidates)(void *ctx, uint8_t kind, const char *query, zsrc_candidate_t *out,
                              size_t max);
    /* Replaces the zone's sensor; ZONE_HUB_ERR_BUSY while anything is armed. */
    zone_hub_result_t (*zone_bind)(void *ctx, uint16_t zone, const zone_hub_binding_t *b);
    zone_hub_result_t (*zone_unbind)(void *ctx, uint16_t zone);
    /* Where the zone's sensor is and how it is doing; false: the zone has no sensor. */
    bool (*zone_info)(void *ctx, uint16_t zone, zone_hub_info_t *out);

    /* ---- integrations (Settings > Integrations) ---- */
    void (*mqtt_get)(void *ctx, ui_mqtt_settings_t *out);
    /* Validates, stores and applies. false: rejected (why: a short reason, may be NULL). */
    bool (*mqtt_set)(void *ctx, const ui_mqtt_settings_t *in, char *why, size_t why_len);
    void (*mqtt_status)(void *ctx, ui_mqtt_status_t *out);

    /* The local Home Assistant link (optional as a set: NULL halink_status hides it). */
    void (*halink_status)(void *ctx, ui_halink_status_t *out);
    bool (*halink_enable)(void *ctx, bool on);
    bool (*halink_pair_begin)(void *ctx);        /* opens the 2-minute window */
    void (*halink_pair_decide)(void *ctx, bool allow);
    void (*halink_pair_cancel)(void *ctx);
    void (*halink_unpair)(void *ctx);

    /* Push notifications (optional as a set: a NULL notify_status hides the Notifications
     * group in Integrations, the way a NULL halink_status hides Home Assistant). */
    void (*notify_status)(void *ctx, uint8_t provider, ui_notify_status_t *out);
    /* Starts a test notification and returns at once - posting it can take ten seconds, and
     * this runs on the LVGL thread. false: there was no point starting one (why says so).
     * The answer arrives through notify_test_result(), polled by the page's tick. */
    bool (*notify_test)(void *ctx, uint8_t provider, char *why, size_t why_len);
    /* true once the started test has an answer; it is taken, so it is reported once. */
    bool (*notify_test_result)(void *ctx, bool *ok, char *why, size_t why_len);
    void (*ntfy_get)(void *ctx, ui_ntfy_settings_t *out);
    bool (*ntfy_set)(void *ctx, const ui_ntfy_settings_t *in, char *why, size_t why_len);
    /* A fresh unguessable topic (the old one stops working: every phone must subscribe again). */
    void (*ntfy_new_topic)(void *ctx, char *out, size_t len);
    void (*pushover_get)(void *ctx, ui_pushover_settings_t *out);
    bool (*pushover_set)(void *ctx, const ui_pushover_settings_t *in, char *why, size_t why_len);

    /* ---- languages (optional as a set: a NULL lang_catalog keeps the catalogs built into the
     * firmware, i18n_catalog() and i18n_catalog_at()) ----
     * The words of a language: its catalog, NULL for English - also for a language that is not
     * installed or not loaded yet, which the backend then loads and lang_generation() changes. */
    const i18n_catalog_t *(*lang_catalog)(void *ctx, const char *locale);
    /* The languages installed (English not among them): at most max, the count. */
    size_t (*lang_list)(void *ctx, ui_lang_t *out, size_t max);
    /* Changes with every install, removal and load: the screen looks its language up again. */
    uint32_t (*lang_generation)(void *ctx);
    /* A font the language brings for a text font (optional): role "caption", "body", "label",
     * "title" or "headline"; fills the bytes of an LVGL binary font (lv_font_conv --format bin)
     * that stay valid while the language is loaded. False when it brings none. RAM only. The
     * screen hangs it behind the built-in font as its fallback while the language is on. */
    bool (*lang_font)(void *ctx, const char *locale, const char *role, const void **data,
                      size_t *size);

    /* ---- asset packs (docs/features/39 asset-packs.md "The panel", OPT-006; each optional,
     * NULL: none, the kind's built-in fallback) ----
     * The letters pack's LVGL binary font (lv_font_conv --format bin) for a text role "caption",
     * "body", "label" or "title": bytes valid until restart. False when no letters pack is loaded
     * or it has no part for the role. RAM only. */
    bool (*asset_font)(void *ctx, const char *role, const void **data, size_t *size);
    /* The picture pack's part of this name ("photo": a baseline JPEG of at most 1024 x 600):
     * bytes valid until restart. False without one. RAM only. */
    bool (*asset_picture)(void *ctx, const char *name, const void **data, size_t *size);
    /* Changes whenever a loaded asset pack changes (one installed now): the screen asks again. */
    uint32_t (*asset_generation)(void *ctx);
    /* A letters pack is loaded and was made from these fonts: its `match` equals `fingerprint`,
     * which the screen passes (alarm_ui_letters_fingerprint, generated with its layout). A pack
     * that differs is still offered by asset_font (a letter a pixel off beats a box). */
    bool (*asset_letters_match)(void *ctx, uint32_t fingerprint);

    /* ---- this keypad and the panel ---- */
    void (*save_options)(void *ctx, const ui_options_t *opt);
    /* A change of the preferences from elsewhere (UI-046; ui_prefs.h). Optional as a pair: NULL =
     * nothing from elsewhere is applied. Polled on every tick of the screen's task (ui_settings.c
     * prefs_remote_tick). take: true when a patch was waiting, now the screen's, with its ticket;
     * a patch that waited longer than UI_PREFS_TAKE_MS is never handed out (the backend answers
     * it BUSY itself). The screen answers each ticket taken exactly once, with done, on the same
     * task, in the same tick or a later one (at most 2 s later: UI_PREFS_LOOK_MS): why[id - 1] a
     * UI_PREFS_WHY_* for each field refused, 0 for the rest. A field applied went through
     * save_options before done. */
    bool (*prefs_remote_take)(void *ctx, ui_prefs_patch_t *out, uint32_t *ticket);
    void (*prefs_remote_done)(void *ctx, uint32_t ticket, const uint8_t why[UI_PREFS_F_LAST]);
    void (*remote_get)(void *ctx, bool *web, bool *developer);
    bool (*remote_set)(void *ctx, bool web, bool developer);
    void (*remote_info)(void *ctx, char *out, size_t capacity);
    /* Panel Manager sign-in (optional: NULL hides it). A user signs in with their name, the
     * installer with about.panel_id; whoever manages the user sets the password, here or in
     * the Panel Manager. web_login_set hashes it (about a second); temporary: set for somebody
     * else, changed at their first sign-in; false: why says why, in words. */
    bool (*web_login_get)(void *ctx, uint8_t user);
    bool (*web_login_set)(void *ctx, uint8_t user, const char *password, bool temporary,
                          char *why, size_t why_len);
    void (*web_login_clear)(void *ctx, uint8_t user);
    /* The remote keypad (docs/features/31): the person at the panel pauses the remote session with
     * the red banner (true) and lets it go on (false). Turning remote access off is
     * remote_set(false, false). Optional: NULL when the firmware has no remote keypad. */
    void (*remote_pause)(void *ctx, bool paused);
    /* The three everyday volumes (AUD-032), each 0..100 and on its own: Beeps (set_volume), the
     * door chime and the voice. Alarms, fire and CO stay at full volume whatever they say. */
    void (*set_volume)(void *ctx, uint8_t percent);
    void (*set_chime_volume)(void *ctx, uint8_t percent);
    void (*set_voice_volume)(void *ctx, uint8_t percent);
    void (*about)(void *ctx, ui_about_t *out);
    void (*storage_sd)(void *ctx, storage_sd_t *out);
    void (*storage_flash)(void *ctx, storage_flash_t *out);
    bool (*storage_action)(void *ctx, storage_action_t action, uint32_t generation, uint8_t user);

    /* ---- firmware updates (Settings > About; optional) ---- */
    void (*fw_status)(void *ctx, ui_fw_status_t *out);
    void (*fw_check)(void *ctx);
    bool (*fw_install)(void *ctx, uint8_t user); /* false: an update is already running */
    /* BL-188: Stop the update (ui_fw_stop_t). */
    uint8_t (*fw_stop)(void *ctx, uint8_t user);
    /* BL-186, the installer's (Installer > Updates): the server, its key and the channel; a new
     * server's check (asynchronous: fw_server_check_get follows it), keeping what was checked
     * (ui_fw_commit_t) and the channel (false: an update runs). */
    void (*fw_server)(void *ctx, ui_fw_server_t *out);
    bool (*fw_server_check)(void *ctx, const char *typed); /* false: not a domain, or an update
                                                              runs (fw_server_check_get says) */
    void (*fw_server_check_get)(void *ctx, ui_fw_srv_check_t *out);
    void (*fw_server_check_forget)(void *ctx);
    /* domain and key_sha256: the server and key the installer was shown (ui_fw_srv_check_t), so
     * what is kept and trusted is what was seen; a later check by someone else, of another
     * server or another key, makes this UI_FW_COMMIT_STALE. */
    uint8_t (*fw_server_commit)(void *ctx, const char *domain, const uint8_t key_sha256[32],
                                uint8_t user);
    bool (*fw_set_channel)(void *ctx, const char *channel, uint8_t user);
    void (*setup_done)(void *ctx);
    void (*factory_reset)(void *ctx, bool erase_log); /* restarts the panel */

    /* ---- Z-Wave (Installer > Z-Wave; docs/features/36 screens.md). Optional as a set: a NULL
     * zw_status - or one that returns false, this panel has no Z-Wave service running - hides
     * every Z-Wave row and page ---- */
    bool (*zw_status)(void *ctx, ui_zw_status_t *out);
    size_t (*zw_devices)(void *ctx, ui_zw_device_t *out, size_t max);
    /* false: no such device (removed, or its node ID given to another since). */
    bool (*zw_device)(void *ctx, uint16_t node, uint16_t generation, ui_zw_device_page_t *out);
    /* Starts or answers a network operation, and returns at once: what follows is zw_progress's,
     * polled once a second. The backend checks installer mode and the partitions itself. */
    ui_zw_req_result_t (*zw_request)(void *ctx, const ui_zw_request_t *req);
    void (*zw_progress)(void *ctx, ui_zw_progress_t *out);
    /* SmartStart (optional as a set; NULL hides "Waiting to join"): the list, a code registered
     * (a QR code's digits or a 40-digit key; `name` may be empty), an entry changed (by its DSK).
     * The backend checks installer mode itself. */
    size_t (*zw_prov)(void *ctx, ui_zw_prov_t *out, size_t max);
    ui_zw_prov_result_t (*zw_prov_register)(void *ctx, const char *code, const char *name);
    ui_zw_prov_result_t (*zw_prov_change)(void *ctx, const uint8_t dsk[16], uint8_t op,
                                          uint8_t arg);

    /* ---- Smart Home (docs/features/24, BL-045; ui_devices.c). Optional as a set: a NULL dev_list
     * - or one that lists no device - hides the rail's Smart Home (HCTL-003) ---- */
    /* Every device, newest state, in the registry's order: at most max, the count. */
    size_t (*dev_list)(void *ctx, ui_dev_t *out, size_t max);
    /* Starts an action (value as dev_action_t says; ACT_SET_MODE: a UI_DEV_MODE_*'s ui_dev_state_t,
     * see there). The backend judges HCTL-005 itself: an
     * unlock or an opening without a code answers UI_DEV_NEED_CODE; with one, the engine checks
     * it like any keypad code (a wrong one counts toward the lockout); an arm-only or a guest
     * code is refused (ALARM_ERR_NOT_AUTHORIZED). Who did it isn't logged yet (HCTL-007). code
     * NULL or "" for none; code_result (may be NULL) gets the engine's answer when the code is
     * refused. */
    ui_dev_result_t (*dev_act)(void *ctx, const char *ref, uint8_t action, int32_t value,
                               const char *code, alarm_result_t *code_result);
    /* The screen's layout (ui_dev_model.h), one panel setting, the same for everyone (HCTL-008):
     * stored and given back byte for byte. get: its length, 0 when nothing is stored or it is
     * longer than max; set: false when it couldn't be stored. */
    size_t (*dev_layout_get)(void *ctx, uint8_t *out, size_t max);
    bool (*dev_layout_set)(void *ctx, const uint8_t *blob, size_t len);

    /* ---- Zigbee (Installer > Zigbee; docs/features/20 devices.md §9, BL-144/BL-145). Optional as
     * a set: a NULL zb_status - or one that returns false, this panel runs no Zigbee service -
     * hides every Zigbee row and page. Filled by zbz_panel's readers under the zone lock ---- */
    bool (*zb_status)(void *ctx, ui_zb_status_t *out);
    /* Every device in the registry's order (the UI sorts them: unreachable first, then by name):
     * at most max, the count. */
    size_t (*zb_devices)(void *ctx, ui_zb_device_t *out, size_t max);
    /* false: no such device (removed since). */
    bool (*zb_device)(void *ctx, uint16_t id, ui_zb_device_page_t *out);
    /* Starts or answers an operation, or changes a setting, and returns at once: what follows is
     * zb_progress's, polled once a second. The backend checks installer mode and the partitions
     * itself (every partition disarmed for every kind but PING, IDENTIFY, SURVEY and DISMISS),
     * and RESET's installer_code with the engine before anything else (WRONG_CODE, LOCKED_OUT);
     * the caller wipes its copy of the request after the call. */
    ui_zb_req_result_t (*zb_request)(void *ctx, const ui_zb_request_t *req);
    void (*zb_progress)(void *ctx, ui_zb_progress_t *out);
    /* The install codes waiting (page 10): at most max, the count. */
    size_t (*zb_codes)(void *ctx, ui_zb_code_t *out, size_t max);

    /* ---- Automations (docs/features/41; ui_automations.c, ui_auto_edit.c). Optional as a set: a
     * NULL auto_list hides Settings > Automations. The panel's automation_service_*, the
     * simulator's auto_store. Every edit answers as automation_service does: AUTO_SAVE_LOCKED
     * while armed or in alarm (AUT-010), CHANGED for a stale expected_gen (AUT-008),
     * READ_ONLY for a store that loaded read-only ---- */
    /* The rules in slot order, NEWER and BROKEN ones included: at most max, the count. */
    size_t (*auto_list)(void *ctx, auto_rule_t *out, size_t max);
    /* The store's generation (an edit sends back the one its page read) and whether it loaded
     * read-only. */
    uint16_t (*auto_gen)(void *ctx);
    bool (*auto_read_only)(void *ctx);
    /* The installer option "Allow less secure automations" (opt.auto_less_secure), read live
     * (AUT-015): the editors offer less secure actions only while it is on. On the panel it asks
     * the alarm task each time, so read it when a page or sheet is built, never from a tick. */
    bool (*auto_less_secure)(void *ctx);
    /* Adds (r->uid 0) or changes a rule. The backend builds the validator's view of the panel
     * itself, never the caller's (AUT-002, AUT-015, AUT-016): *why (may be NULL) gets its answer
     * with AUTO_SAVE_INVALID; *uid_out (may be NULL) the uid saved. `user` becomes the maker. */
    auto_save_t (*auto_save)(void *ctx, const auto_rule_t *r, uint8_t user, uint16_t expected_gen,
                             auto_check_t *why, uint16_t *uid_out);
    /* Deletes by slot (a NEWER, BROKEN or BAD record may have no uid to name it by). */
    auto_save_t (*auto_delete_slot)(void *ctx, uint8_t slot, uint16_t expected_gen);
    auto_save_t (*auto_set_off)(void *ctx, uint16_t uid, bool off, uint16_t expected_gen);
    /* The rule's last outcome since the panel started; false for an unknown uid. */
    bool (*auto_status)(void *ctx, uint16_t uid, auto_status_t *out);
    /* Try it now (AUT-006): its device actions run now; the outcome comes later in auto_status,
     * with tried set. UI_AUTO_TRY_BUSY when the runner didn't take the request. */
    auto_try_t (*auto_try)(void *ctx, uint16_t uid);

    /* ---- Arming by automation (docs/features/41 phase C, DESIGN.md section 3 "Arming" 2 and 4;
     * Home's warning card and put-off pad). Each NULL = not supported: the UI hides the card,
     * the line and the put-off. The panel's automation_service_*, the simulator's runner ---- */
    /* The arming warnings and holds now (auto_warning_t.state ON, HOUR, NIGHT or DAY; one per
     * automation): at most max, the count. A copy of a RAM table, no engine call: fine from a
     * tick. */
    size_t (*auto_warnings)(void *ctx, auto_warning_t *out, size_t max);
    /* Put off automation uid's warning (AUT-013, D26): choice an auto_put_off_choice_t, code the
     * digits a person typed (anyone at the wall, any user code: DESIGN.md section 8). The code
     * is checked first with ALARM_CMD_CODE_CHECK on the warning's partition from the keypad
     * source - a wrong one counts toward the keypad's lockout; a duress code reports duress
     * silently and the put-off still works, so nothing at the wall gives it away - and its
     * answer goes in *code_result (may be NULL; ALARM_OK when accepted). Then the put-off by
     * that code's user: AUTO_POFF_OK, or NO_WARNING (that automation isn't warning now: no code
     * is checked), NOT_ALLOWED (NIGHT with a code that may not disarm that partition: offer the
     * hour), NO_CLOCK (NIGHT while the clock isn't set), BAD_ARG; UI_AUTO_POFF_CODE when the
     * code was refused (*code_result says why: INVALID_CODE, LOCKED_OUT...), UI_AUTO_POFF_BUSY
     * when the runner didn't take the request. */
    auto_put_off_t (*auto_put_off)(void *ctx, uint16_t uid, uint8_t choice, const char *code,
                                   alarm_result_t *code_result);

    /* ---- the voice (docs/features/08 voice-first-build.md D03, D06, D09). voice_info, the
     * lease and voice_preview NULL = no voice in this build: the UI hides Voice volume, the
     * Announcements page and the zone's Spoken name row. voice_seen and voice_apply_approve NULL
     * = no spoken-name editor here (a link keypad: its alarm panel's installer approves the words,
     * docs/features/11 LINK-028). The panel's panel_voice_*, the simulator's and the preview's
     * own. ---- */
    void (*voice_info)(void *ctx, ui_voice_info_t *out);
    /* A lease on the active pack's index, for the word lists and Use zone name
     * (voice_pack/vp_words.h) and the words' display texts (vp_find): valid until released,
     * even if a new pack is installed meanwhile. NULL when not ready. Release it when the
     * screen that took it closes, from the LVGL task. */
    const vp_pack_t *(*voice_pack_acquire)(void *ctx);
    void (*voice_pack_release)(void *ctx, const vp_pack_t *pack);
    /* Preview (D03): plays the words once as the zone's name would sound; works with
     * Announcements off; changes nothing stored. A ui_voice_preview_t. */
    uint8_t (*voice_preview)(void *ctx, const uint16_t *ids, size_t n);
    /* The identity the editor's Done saw (ui_voice_seen_t): the configuration's generation and
     * the zone's stored binding revision; pack_revision is left 0 (the caller's lease says it).
     * False when the zone is out of range or the engine didn't answer. */
    bool (*voice_seen)(void *ctx, uint16_t zone, ui_voice_seen_t *out);
    /* Save and approve (D01, D07), the one way the touchscreen approves: cfg (NULL = no save,
     * the words are already stored) is applied as set_config applies it, whatever the approval's
     * fate, and *saved (optional) is its result (ALARM_OK without cfg). Approval is granted only
     * when the pack the caller leases now is seen->pack_revision and says ids[0..n) (each word
     * known and selectable, at most 10 s), the save succeeded and was the only change since
     * seen->generation (a save moves it by exactly one; without cfg it must not have moved, and
     * the binding must still be seen->binding_revision), and the stored words equal ids; with
     * cfg it is approved against the binding the save produced (the installer's own rename or
     * move in the same page). Else STALE, UNAVAILABLE or REFUSED. A ui_voice_approve_t. Only an
     * explicit Done in the editor leads here; a saved configuration never grants approval. */
    uint8_t (*voice_apply_approve)(void *ctx, const alarm_config_t *cfg, uint16_t zone,
                                   const ui_voice_seen_t *seen, const uint16_t *ids, size_t n,
                                   alarm_result_t *saved);
    /* Stops Preview only: every Preview queued or playing now ends at once; automatic speech and
     * tones go on. The editor or Announcements page that played one calls it as it closes.
     * Optional (NULL: nothing to stop). */
    void (*voice_preview_stop)(void *ctx);

    /* ---- the panel link (docs/features/11; the types above ui_backend_t). NULL: no link in
     * this firmware - no Keypads section, no role step, every page as before. ---- */
    /* The role and, on a keypad, the link's state; false: not known yet (show nothing). */
    bool (*link_info)(void *ctx, ui_link_info_t *out);
    /* The pairing under way on either side; false: none. */
    bool (*link_pair_status)(void *ctx, ui_link_pair_t *out);
    /* The master's keypads, by number; returns how many (0 on a keypad). */
    size_t (*link_keypads)(void *ctx, ui_link_keypad_t *out, size_t max);
    /* A request; a ui_link_result_t. The pairing requests return at once, link_pair_status
     * follows them. */
    uint8_t (*link_request)(void *ctx, const ui_link_req_t *req);
    /* Select a partition or report actual input; never authorizes an alarm action. */
    alarm_result_t (*partition_select)(void *ctx, uint8_t partition, bool activity);

    /* Optional (LINK-054): runs work(arg) once on a task of the backend's and returns at once;
     * false: not now (the UI runs it itself). The UI hands over its calls to command,
     * authenticate, change_code, dev_act and partition_select, one at a time, when they may wait
     * for another panel (a keypad's master answers them); work calls only those entries and
     * nothing of LVGL. NULL: every call runs on the UI's task, as an engine on this panel answers
     * at once. */
    bool (*run_later)(void *ctx, void (*work)(void *arg), void *arg);
} ui_backend_t;

/* auto_try's "the runner didn't take it" (automation_service.h's AUTOMATION_TRY_BUSY, asserted
 * equal in the panel's port). */
#define UI_AUTO_TRY_BUSY ((auto_try_t)100)
/* auto_put_off's "the runner didn't take it" (automation_service.h's AUTOMATION_POFF_BUSY,
 * asserted equal in the panel's port) and "the code was refused" (see *code_result). */
#define UI_AUTO_POFF_BUSY ((auto_put_off_t)100)
#define UI_AUTO_POFF_CODE ((auto_put_off_t)101)

/* The preferences a keypad starts with before anything is stored. */
static inline void ui_options_defaults(ui_options_t *opt)
{
    opt->partition = 1;
    opt->partition_home = 1;
    opt->switch_allowed = false;
    opt->return_never = false;
    opt->return_s = 30;
    opt->clock_mode = 0;
    opt->brightness = 80;
    opt->saver_brightness = 25; /* the screen saver shows a picture */
    opt->saver_after_ms = 60000u;
    opt->off_after_ms = 0u; /* the screen saver stays lit, as before UI-043 */
    opt->night_on = false;
    opt->night_start = 22u * 60u;
    opt->night_end = 7u * 60u;
    opt->night_brightness = 20;
    opt->night_off_after_ms = 60000u;
    opt->volume = 70;
    opt->chime_volume = 70;
    opt->voice_volume = 70;
    opt->key_click = true;
    opt->run_setup = false;
    opt->appearance = UI_APPEARANCE_DARK; /* a panel updated keeps today's look */
    opt->high_contrast = false;
    opt->voice_enabled = true; /* Announcements on, as on modern panels (D06, decided 2026-10-12) */
    opt->voice_status = true;
    opt->voice_chime = true;
    static const char english[] = "en-US";
    for (size_t i = 0; i < sizeof(opt->lang); i++) {
        opt->lang[i] = '\0';
    }
    for (size_t i = 0; i < sizeof(english); i++) {
        opt->lang[i] = english[i];
    }
    for (size_t i = 0; i < sizeof(opt->region); i++) {
        opt->region[i] = '\0';
    }
}

#ifdef __cplusplus
}
#endif
