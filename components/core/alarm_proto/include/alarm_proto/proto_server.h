/*
 * proto_server.h - the server link's messages, the remote ceiling and the replay queue (the panel's
 * side of alarm-server's docs/features/02-Panel-and-App-API, panel-link.md; BL-225).
 *
 * Portable C11 like the rest of alarm_proto: no heap, no globals, no RTOS. Topics and JSON keys are
 * an external contract shared with alarm-server: never rename, only add. Tests reference API-NNN.
 */
#ifndef PROTO_SERVER_H
#define PROTO_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_engine.h"
#include "alarm/alarm_snapshot.h"
#include "alarm_proto/proto.h"
#include "alarm_proto/proto_json.h"
#include "event_log/elog.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_SRV_API          1
#define PROTO_SRV_ID_LEN       17u  /* 16 hex digits + NUL */
#define PROTO_SRV_HOST_LEN     96u  /* the server's address, bytes incl. NUL */
#define PROTO_SRV_KEY_LEN      32u  /* Ed25519 public key, X25519 public key */
#define PROTO_SRV_SIG_LEN      64u  /* Ed25519 signature */
#define PROTO_SRV_SIGN_MAX     160u /* the bytes signed for a sign-in */
#define PROTO_SRV_PASSWORD_LEN 160u /* counter.signature.public key, bytes incl. NUL */
#define PROTO_SRV_B64_KEY_LEN  44u  /* a 32-byte key as unpadded base64url, incl. NUL */
#define PROTO_SRV_CMD_ID_LEN   41u
#define PROTO_SRV_CODE_LEN     8u   /* a link code, incl. NUL */
#define PROTO_SRV_SEALED_LEN   128u /* a sealed code, base64url, incl. NUL */
#define PROTO_SRV_USERS_BYTES  ((ALARM_MAX_USERS + 7u) / 8u)

/* ---- identity and sign-in (API-002) --------------------------------------------------------- */

/* First 16 hex digits of SHA-256(public key). */
void proto_srv_id(const uint8_t pub[PROTO_SRV_KEY_LEN], char out[PROTO_SRV_ID_LEN]);

/* "ra/<id><suffix>", e.g. suffix "/up/hello". false when it doesn't fit. */
bool proto_srv_topic(const char *id, const char *suffix, char *out, size_t cap);

/* The server's address as it is signed: lowercase, no port. false when it doesn't fit. */
bool proto_srv_host_norm(const char *host, char *out, size_t cap);

/* The bytes a sign-in signs: "roboalarms-mqtt-v1" 0 host 0 id 0 counter-in-decimal; host goes
 * through proto_srv_host_norm. Returns their length, 0 when they don't fit. */
size_t proto_srv_sign_bytes(const char *host, const char *id, uint64_t counter, uint8_t *out,
                            size_t cap);

/* Unpadded base64url into out, NUL-terminated. Returns the length, 0 when it doesn't fit (or the
 * input is empty). */
size_t proto_srv_b64url(const uint8_t *in, size_t len, char *out, size_t cap);
/* The reverse: returns the bytes written, 0 on another character, a bad length or no room. */
size_t proto_srv_unb64url(const char *in, uint8_t *out, size_t cap);

/* "<counter>.<signature>.<public key>", the last two unpadded base64url. */
bool proto_srv_password(uint64_t counter, const uint8_t sig[PROTO_SRV_SIG_LEN],
                        const uint8_t pub[PROTO_SRV_KEY_LEN], char *out, size_t cap);

/* ---- the remote ceiling (D6, API-005) ------------------------------------------------------- */

typedef struct {
    uint32_t rev;                               /* goes up with each change */
    uint8_t remote_users[PROTO_SRV_USERS_BYTES]; /* bit (user - 1): may act through the server */
    int64_t until_ms[ALARM_MAX_USERS];          /* end of that person's access, 0 = none */
    bool remote_disarm;
    bool remote_setup;
    bool remote_people;
    bool ask_code;
} proto_srv_ceiling_t;

typedef enum {
    PROTO_SRV_ACT_ORDINARY = 0, /* arming, bypass, chime, the walk test: anyone with remote access */
    PROTO_SRV_ACT_DISARM,       /* disarm and silence: remote_disarm */
    PROTO_SRV_ACT_SETUP,        /* Setup mode: remote_setup */
    PROTO_SRV_ACT_PEOPLE,       /* people and codes: remote_people */
} proto_srv_act_t;

typedef enum {
    PROTO_SRV_OK = 0,
    PROTO_SRV_NOT_ALLOWED, /* "not_allowed": this user has no remote access */
    PROTO_SRV_EXPIRED,     /* "expired": past the end time, or the panel has no clock to tell */
    PROTO_SRV_REMOTE_OFF,  /* "remote_off": the ceiling forbids this for everyone */
} proto_srv_refusal_t;

/* Defaults: nobody has remote access, the three switches on, no code asked. */
void proto_srv_ceiling_defaults(proto_srv_ceiling_t *c);
bool proto_srv_user_remote(const proto_srv_ceiling_t *c, uint8_t user);
void proto_srv_user_set_remote(proto_srv_ceiling_t *c, uint8_t user, bool on, int64_t until_ms);
/* now_ms: the panel's clock, 0 = unknown (an end time then counts as passed). */
proto_srv_refusal_t proto_srv_check(const proto_srv_ceiling_t *c, uint8_t user, proto_srv_act_t act,
                                    int64_t now_ms);
const char *proto_srv_refusal_name(proto_srv_refusal_t r);

/* ---- messages the panel sends --------------------------------------------------------------- */

typedef struct {
    const char *id;
    const char *model;
    const char *fw;
    const char *const *caps;
    size_t caps_n;
    const char *pub;      /* base64url */
    const char *seal_pub; /* base64url */
    const char *seal_sig; /* base64url: the link key's signature of proto_srv_seal_signed(seal_pub) */
    bool linked;
    uint32_t boot;
    uint64_t uptime_ms;
    int64_t now_ms;       /* 0 = unknown: null */
    const char *clock;
    const char *tz;       /* NULL or "": null */
    uint32_t last_seq;
    uint32_t oldest_seq;
    const char *path;
} proto_srv_hello_t;

void proto_srv_hello(pj_writer_t *w, const proto_srv_hello_t *h);
/* What the panel signs with its link key (the one "pub" is) so the server and the app know the seal
 * key is this panel's (panel-link.md "Sealed codes", F3): "rs1-seal|" and the seal_pub string as
 * sent. Its length, or 0 when it doesn't fit out. */
size_t proto_srv_seal_signed(const char *seal_pub, char *out, size_t cap);
void proto_srv_online(pj_writer_t *w, bool online);

/* What the clock's source is called on the wire, from elog_entry_t.clock. */
#define PROTO_SRV_CLOCK_NONE   0u
#define PROTO_SRV_CLOCK_RTC    1u
#define PROTO_SRV_CLOCK_NTP    2u
#define PROTO_SRV_CLOCK_MANUAL 3u
#define PROTO_SRV_CLOCK_SERVER 4u
const char *proto_srv_clock_name(uint8_t clock);

/* Who a command or a tunnelled request was for, as the server named them ("actor"): the events it
 * caused carry it back, so the server credits the person and not the panel user it ran as (a
 * technician working as the installer user; panel-link.md "event"). */
typedef struct {
    char account[40];
    char name[32];
    char via[12];
} proto_srv_actor_t;

/* One log record: the Home Assistant link's event fields plus at_ms, boot, uptime_ms, clock and the
 * words the panel itself would show (title and body). words NULL leaves them out; actor NULL (or
 * nameless) too. */
void proto_srv_event(pj_writer_t *w, const alarm_config_t *cfg, const elog_entry_t *e, bool replayed,
                     const char *title, const char *body, const proto_srv_actor_t *actor);

/* The events each recent command or request caused, by their sequence numbers, with its actor: the
 * engine numbers its events, so the ones from first_seq to last_seq with the server's source are
 * that run's. The oldest run is forgotten first. */
#define PROTO_SRV_ACTOR_RUNS 8u
typedef struct {
    bool used;
    uint32_t first_seq, last_seq;
    proto_srv_actor_t actor;
} proto_srv_actor_run_t;
typedef struct {
    proto_srv_actor_run_t run[PROTO_SRV_ACTOR_RUNS];
    uint8_t next;
} proto_srv_actors_t;
/* A run that caused the events after before_seq up to after_seq (the engine's newest before and
 * after it). Nothing is kept for a run that caused none, or with no actor. */
void proto_srv_actors_note(proto_srv_actors_t *t, uint32_t before_seq, uint32_t after_seq,
                           const proto_srv_actor_t *actor);
/* The actor of the run that caused ev, or NULL: ev must come from the server (a remote source with
 * one of the server link's ids) and fall in a run's range. */
const proto_srv_actor_t *proto_srv_actors_find(const proto_srv_actors_t *t, const alarm_event_t *ev);

typedef struct {
    const char *name;
    const char *tz;
    const char *region;
    const char *fw;
    const char *power;  /* "mains" | "battery" */
    int battery_pct;    /* < 0 = unknown */
    bool setup_mode;
} proto_srv_panel_t;

void proto_srv_snapshot(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                        const proto_when_t *when, const proto_srv_panel_t *panel);

/* browser: bit (user - 1) has a browser sign-in (NULL = none). */
void proto_srv_people(pj_writer_t *w, const alarm_config_t *cfg, const proto_srv_ceiling_t *c,
                      const uint8_t *browser);
/* installer_setup: the Owner's "Let the Admin code open Setup mode" (CORE-PROG-014,
 * alarm_config_installer_sets_up): the key "installer_setup", added 2026-11-08 (older panels don't
 * send it: the server reads absent as true). */
void proto_srv_ceiling(pj_writer_t *w, const proto_srv_ceiling_t *c, bool installer_setup);

typedef struct {
    const char *power;
    int battery_pct; /* < 0 = unknown */
    int rssi;
    uint64_t uptime_s;
    uint32_t heap_kb;
    const char *clock;
    const char *path;
} proto_srv_health_t;
void proto_srv_health(pj_writer_t *w, const proto_srv_health_t *h);

void proto_srv_config(pj_writer_t *w, const char *domain, uint32_t rev);

void proto_srv_link_start(pj_writer_t *w);
void proto_srv_link_cancel(pj_writer_t *w);
void proto_srv_link_done(pj_writer_t *w, const char *claim, bool ok, uint8_t user);
void proto_srv_unlink(pj_writer_t *w);

/* {"t":"res","id":…,"status":…,"body":{"error":…}}; error NULL leaves the body out. */
void proto_srv_res(pj_writer_t *w, const char *id, int status, const char *error);

/* ---- messages the panel receives ------------------------------------------------------------ */

typedef enum {
    PROTO_SRV_IN_UNKNOWN = 0, /* ignored */
    PROTO_SRV_IN_BAD,         /* not valid JSON, or a known type missing a field */
    PROTO_SRV_IN_TIME,
    PROTO_SRV_IN_ACK,
    PROTO_SRV_IN_RESYNC,
    PROTO_SRV_IN_UNSUPPORTED,
    PROTO_SRV_IN_LINK_CODE,
    PROTO_SRV_IN_LINK_CLAIM,
    PROTO_SRV_IN_LINKED,
    PROTO_SRV_IN_UNLINK,
    PROTO_SRV_IN_COMMAND,
    PROTO_SRV_IN_REQ,
    PROTO_SRV_IN_PEOPLE_LINK,
    PROTO_SRV_IN_CONTACTS,
} proto_srv_in_type_t;

typedef struct {
    int64_t server_ms;
    uint64_t echo_uptime_ms;
    uint32_t ack_seq;
    char location_id[48];
} proto_srv_time_t;

typedef struct {
    char code[PROTO_SRV_CODE_LEN];
    char url[144];
    uint32_t expires_s;
} proto_srv_link_code_t;

typedef struct {
    char claim[48];
    char account_name[48];
    char account_email_masked[64];
    char location_name[48];
} proto_srv_link_claim_t;

typedef struct {
    char id[PROTO_SRV_CMD_ID_LEN];
    uint8_t as;
    char action[20];
    uint8_t partition;
    char level[8];
    uint16_t zone;
    bool no_entry_delay;
    bool silent_exit;
    bool bypass_open;
    char sealed_code[PROTO_SRV_SEALED_LEN];
    char actor_name[32];
    char actor_via[12];
    char actor_account[40];
} proto_srv_command_t;

typedef struct {
    char id[PROTO_SRV_CMD_ID_LEN];
    uint8_t as;
    char method[8];
    char path[64];
    bool fresh;
    const char *body; /* the raw JSON of "body" inside the message, NULL when none */
    size_t body_len;
    proto_srv_actor_t actor;
} proto_srv_req_t;

typedef struct {
    char id[PROTO_SRV_CMD_ID_LEN];
    uint8_t user;
    bool on;
    int64_t until_ms;
    uint8_t as;
    bool fresh;
} proto_srv_people_link_t;

typedef struct {
    proto_srv_in_type_t type;
    union {
        proto_srv_time_t time;
        uint32_t ack_seq;
        proto_srv_link_code_t link_code;
        proto_srv_link_claim_t link_claim;
        char location_id[48]; /* LINKED */
        proto_srv_command_t command;
        proto_srv_req_t req;
        proto_srv_people_link_t people_link;
    } u;
} proto_srv_in_t;

/* The strict reader over one message. The ack on down/ack has no "t": pass is_ack for that topic,
 * and is_time for down/time when the server leaves it out. */
void proto_srv_parse(const char *json, size_t len, bool is_ack, bool is_time, proto_srv_in_t *out);

/* ---- commands -------------------------------------------------------------------------------- */

/* The state a command runs in: the same three as alarm_service_command_as()'s alarm_svc_when_t. */
typedef enum {
    PROTO_SRV_WHEN_ANY = 0,
    PROTO_SRV_WHEN_DISARMED,
    PROTO_SRV_WHEN_NOT_DISARMED,
} proto_srv_when_t;

/* The action as the engine's command, with the same table as the Panel Manager's POST partition
 * (WEB-024). false on an unknown action or level, or a field out of range. */
bool proto_srv_command_map(const proto_srv_command_t *in, alarm_cmd_t *cmd, proto_srv_when_t *when,
                           proto_srv_act_t *act);

/* A sealed code's plaintext (panel-link.md "Sealed codes"): "rs1|<ms since 1970>|<action>|<partition>|
 * <code>". Bound to the command it was typed for and to the moment, so a copy the server kept can't
 * disarm again later or do something else. Only a disarm-class action (PROTO_SRV_ACT_DISARM) takes
 * one; the caller also refuses one it has opened before (within twice PROTO_SRV_SEALED_MS).
 * true with the code in code (NUL-terminated) when it is for in, and its time is within
 * PROTO_SRV_SEALED_MS of now_ms (which must be known, > 0). */
#define PROTO_SRV_SEALED_MS 120000

/* How a command runs: as its user with the action's state check (alarm_service_command_as), with the
 * person's own sealed code, or not at all because the ceiling asks for a code. A code is only ever
 * used for a disarm-class action: a code runs without the state check, so "clear" with one would
 * disarm. */
typedef enum {
    PROTO_SRV_RUN_AS_USER = 0,
    PROTO_SRV_RUN_WITH_CODE,
    PROTO_SRV_RUN_CODE_REQUIRED,
} proto_srv_run_t;
proto_srv_run_t proto_srv_command_run(proto_srv_act_t act, bool ask_code, bool sealed);

/* What a tunnelled request (req) needs from the ceiling besides remote access: a GET is ordinary. */
proto_srv_act_t proto_srv_req_act(const char *method, const char *path);
/* Why a tunnelled request is refused before the ceiling is asked, or NULL: a POST to partition
 * ("use_command": arming and disarming go as commands, with the ceiling's disarm and code rules),
 * and a change to people or Setup without the server's fresh step-up ("step_up"). */
const char *proto_srv_req_refusal(const char *method, const char *path, bool fresh);
bool proto_srv_sealed_code(const char *plain, int64_t now_ms, const proto_srv_command_t *in,
                           char *code, size_t cap);
/* The same for any action and target: a person's keypad slot is "set_code" with the user's number,
 * or "create" with 0 for a new person (panel-link.md "Sealed codes", MOB-044). */
bool proto_srv_sealed_for(const char *plain, int64_t now_ms, const char *action, unsigned target,
                          char *code, size_t cap);

/* POST people from the server with a code (panel-link.md "What the server sends"):
 * {action:"create", name, role, partitions:[1,...], sealed_code} or
 * {action:"set_code", number, sealed_code}. Any other action is the Panel Manager's own. */
typedef enum {
    PROTO_SRV_PEOPLE_OTHER = 0, /* not one of these two: the request goes on unchanged */
    PROTO_SRV_PEOPLE_CREATE,
    PROTO_SRV_PEOPLE_SET_CODE,
    PROTO_SRV_PEOPLE_BAD,       /* one of these two with a field missing or out of range */
} proto_srv_people_kind_t;
typedef struct {
    proto_srv_people_kind_t kind;
    uint8_t number;              /* SET_CODE: the user */
    char name[ALARM_NAME_LEN];   /* CREATE */
    alarm_authority_t authority; /* CREATE: from role (proto_srv_role_authority) */
    uint8_t partitions;          /* CREATE: a mask, bit 0 = partition 1 */
    char sealed_code[PROTO_SRV_SEALED_LEN];
} proto_srv_people_t;
void proto_srv_people_parse(const char *json, size_t len, proto_srv_people_t *out);
/* The server's role as the panel's authority (owner master, admin installer, area_manager
 * partition_master, guest, arm_only, duress; the panel's own names too); anything else standard. */
alarm_authority_t proto_srv_role_authority(const char *role);

/* The answer: result is alarm_result_name(), or the refusal's name when refusal != OK. ps NULL
 * leaves out state, level and delay_left_ms. */
typedef struct {
    const char *id;
    const char *action;
    const char *result;
    uint8_t user;
    uint16_t zone;
    const char *words;
    uint8_t partition;
    const char *state;
    const char *level;
    uint64_t delay_left_ms;
} proto_srv_result_t;
void proto_srv_result(pj_writer_t *w, const proto_srv_result_t *r);

/* Commands remembered for ten minutes (API-008): the same id gets the same answer. */
#define PROTO_SRV_SEEN_N    16u
#define PROTO_SRV_SEEN_MS   600000u
#define PROTO_SRV_SEEN_RES  448u
typedef struct {
    char id[PROTO_SRV_CMD_ID_LEN];
    alarm_ms_t at;
    uint16_t len;
    char res[PROTO_SRV_SEEN_RES];
} proto_srv_seen_entry_t;
typedef struct {
    proto_srv_seen_entry_t e[PROTO_SRV_SEEN_N];
    uint8_t next;
} proto_srv_seen_t;

const proto_srv_seen_entry_t *proto_srv_seen_find(const proto_srv_seen_t *s, const char *id,
                                                  alarm_ms_t now);
void proto_srv_seen_add(proto_srv_seen_t *s, const char *id, const char *res, size_t len,
                        alarm_ms_t now);

/* ---- the clock (API-018) --------------------------------------------------------------------- */

/* Half the round trip and the offset of the server from the panel (Cristian). now_ms 0 = no clock. */
typedef struct {
    uint64_t round_trip_ms;
    int64_t offset_ms;   /* server minus panel; 0 when the panel has no clock */
    int64_t server_now;  /* the server's time now */
    bool set_clock;      /* no clock, or more than 2 s off */
    bool trusted;        /* the round trip was short enough to measure anything */
} proto_srv_clock_t;
void proto_srv_clock_check(int64_t server_ms, uint64_t echo_uptime_ms, uint64_t uptime_ms,
                           int64_t now_ms, proto_srv_clock_t *out);

/* ---- the replay queue (D7, API-006) ---------------------------------------------------------- */

/* The event log is the queue. Events go out in log order, a few at a time; only the server's ack
 * (it stored them) moves 'acked', which the owner saves. */
#define PROTO_SRVQ_WINDOW  16u
#define PROTO_SRVQ_SCAN    256u

typedef struct {
    void *ctx;
    /* elog_read_after() with a filter for ELOG_KIND_EVENT. */
    size_t (*read_after)(void *ctx, uint32_t after, elog_entry_t *out, size_t max, uint32_t scan,
                         uint32_t *next_after);
    uint32_t (*newest_index)(void *ctx);
    /* QoS 1 publish; false when it can't go now (tried again on the next poll). */
    bool (*publish)(void *ctx, const elog_entry_t *e, bool replayed);
} proto_srvq_port_t;

typedef struct {
    proto_srvq_port_t port;
    uint32_t acked;       /* log index: everything up to it is stored on the server */
    uint32_t acked_seq;   /* its engine sequence number */
    uint32_t sent;        /* log index of the newest record published on this connection */
    uint32_t skip_upto;   /* records with this seq or lower are the server's already */
    uint32_t live_from;   /* records from this index on became due while connected */
    struct {
        uint32_t index;
        uint32_t seq;
    } window[PROTO_SRVQ_WINDOW];
    uint8_t window_n;
    bool enabled;         /* linked */
    bool running;         /* connected, and down/time has arrived */
    bool connected;
    bool acked_dirty;
    alarm_ms_t down_since;
    alarm_ms_t waiting_since; /* the oldest unacknowledged record's time, 0 = none */
} proto_srvq_t;

/* acked and acked_seq: the saved position. */
void proto_srvq_init(proto_srvq_t *q, const proto_srvq_port_t *port, uint32_t acked,
                     uint32_t acked_seq, bool enabled, alarm_ms_t now);
void proto_srvq_enable(proto_srvq_t *q, bool enabled, alarm_ms_t now);
/* A first link starts from now (NOTE-011): nothing before it is sent. */
void proto_srvq_start_from_now(proto_srvq_t *q, uint32_t newest_seq);
void proto_srvq_connected(proto_srvq_t *q, alarm_ms_t now);
void proto_srvq_disconnected(proto_srvq_t *q, alarm_ms_t now);
/* down/time: ack_seq 0 = the server has nothing and the panel keeps its position; lower than the
 * panel's = the server lost events and gets what the log still has. Starts the replay. */
void proto_srvq_time(proto_srvq_t *q, uint32_t ack_seq, alarm_ms_t now);
/* down/ack. */
void proto_srvq_ack(proto_srvq_t *q, uint32_t seq, alarm_ms_t now);
/* Publishes what it can. true: call again right away. */
bool proto_srvq_poll(proto_srvq_t *q, alarm_ms_t now);
/* comm_fail (MQTT-011's rule): linked, and the server unreachable or an event unacknowledged for
 * PROTO_COMM_FAIL_MS. */
bool proto_srvq_comm_fail(const proto_srvq_t *q, alarm_ms_t now);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_SERVER_H */
