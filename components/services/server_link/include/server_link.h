/*
 * server_link.h - the panel's link to an alarm-server: MQTT 5 over a secure WebSocket, outbound
 * only, signed in with the panel's own Ed25519 key (alarm-server's docs/features/02-Panel-and-App-API,
 * panel-link.md; BL-225).
 *
 * One task owns the client. The messages, the ceiling's rules and the replay queue live in
 * alarm_proto (proto_server.h, host-tested); this component is the transport, the key, NVS and the
 * engine calls. Never linked and not linking, it holds no connection at all, and nothing about how
 * the alarm works depends on it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_proto/proto_phone.h"
#include "alarm_proto/proto_server.h"
#include "alarm_service.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVER_LINK_DEFAULT_HOST "cloud.roboalarms.com"

typedef enum {
    SERVER_LINK_IDLE = 0,    /* not linked, not linking */
    SERVER_LINK_NO_NETWORK,  /* wanted, waiting for the network */
    SERVER_LINK_CONNECTING,
    SERVER_LINK_FAILED,      /* the last try failed (fail says how); trying again */
    SERVER_LINK_WAIT_CODE,   /* linking: connected, the server is making a code */
    SERVER_LINK_SHOW_CODE,   /* linking: show code and url */
    SERVER_LINK_CLAIMED,     /* linking: someone claimed it: server_link_decide() */
    SERVER_LINK_CONNECTED,   /* linked and up */
} server_link_state_t;

typedef enum {
    SERVER_LINK_FAIL_NONE = 0,
    SERVER_LINK_FAIL_UNREACHABLE,
    SERVER_LINK_FAIL_REFUSED,  /* the server refused the sign-in */
    SERVER_LINK_FAIL_TLS,      /* the certificate isn't trusted */
    SERVER_LINK_FAIL_OLD,      /* the server says this panel needs a software update */
    SERVER_LINK_FAIL_OTHER,
} server_link_fail_t;

typedef struct {
    server_link_state_t state;
    server_link_fail_t fail;
    bool linked;
    char host[PROTO_SRV_HOST_LEN];
    uint16_t port;              /* 0 = 443 */
    bool ca_loaded;
    char panel_id[PROTO_SRV_ID_LEN];
    char location_id[48];
    /* While linking. */
    char code[PROTO_SRV_CODE_LEN];
    char url[144];
    uint32_t code_left_s;
    char account_name[48];
    char account_email[64];
    char location_name[48];
    /* While linked. */
    uint32_t sent;              /* events since the panel started */
    bool waiting;               /* events not yet stored on the server */
    int64_t clock_offset_ms;    /* server minus panel at the last exchange */
    uint64_t last_heard_ms;     /* alarm_service_now() of the last message from the server */
} server_link_status_t;

/* Register before alarm_service_start(). */
const alarm_listener_t *server_link_listener(void);
/* The hardware model reported in hello (a literal or the board's description: kept, not copied). */
void server_link_set_model(const char *model);
/* Loads NVS, starts the task. Call after net_wifi_start(); returns at once. */
esp_err_t server_link_start(void);
void server_link_get_status(server_link_status_t *out);

/* The server's address, default SERVER_LINK_DEFAULT_HOST. port 0 = 443. Only while not linked. */
esp_err_t server_link_set_host(const char *host, uint16_t port);
/* A certificate authority to trust instead of the public bundle (PEM); NULL clears it. */
esp_err_t server_link_set_ca(const char *pem);

/* Start linking: user is whoever's Owner or Admin code the caller checked. */
esp_err_t server_link_begin(uint8_t user);
void server_link_cancel(void);
/* Allow or Don't allow, in SERVER_LINK_CLAIMED. */
void server_link_decide(bool allow);
/* The caller checked an Owner or Admin code. Wipes the link record first (one NVS write), then
 * tells the server when connected; works with no server at all. ESP_OK only when written down. */
esp_err_t server_link_unlink(void);

/* The remote ceiling (D6), as seen and changed at the panel. */
void server_link_get_ceiling(proto_srv_ceiling_t *out);
esp_err_t server_link_set_ceiling(const proto_srv_ceiling_t *in);

/* A tunnelled Panel Manager request (panel_web_tunnel()'s signature; the app wires it, so this
 * component doesn't depend on panel_web). Without one every request answers 404. */
typedef int (*server_link_req_fn)(uint8_t user, bool fresh, uint16_t source_id, const char *method,
                                  const char *path, const char *body, size_t body_len, char **out,
                                  size_t *out_len);
void server_link_set_req_handler(server_link_req_fn fn);

/* ---- direct mode (alarm-server's direct-mode.md) --------------------------------------------------
 * Phones paired at the panel. cert_sha256: of the Panel Manager's certificate, hosts: the addresses to
 * try, comma separated. uri is what the QR code holds. Only the caller checks the person's code. */
esp_err_t server_link_phone_pair_begin(uint8_t user, const uint8_t cert_sha256[32], const char *hosts,
                                       char *uri, size_t cap);
void server_link_phone_pair_cancel(void);
typedef enum {
    SERVER_LINK_PHONE_REFUSED = 0, /* a wrong or spent token, or the window closed */
    SERVER_LINK_PHONE_ASKING,      /* Allow is on the panel now */
    SERVER_LINK_PHONE_BUSY,        /* it is already asking about another */
} server_link_phone_ask_t;
/* From the Panel Manager: phone carries pub, name and any push fields; user and used are set here. */
server_link_phone_ask_t server_link_phone_pair_request(const uint8_t token[PROTO_PHONE_TOKEN_LEN],
                                                       const proto_phone_t *phone);
bool server_link_phone_pair_asking(char *name, size_t cap, uint8_t *user);
esp_err_t server_link_phone_pair_decide(bool allow);
/* For this key: 0 still asking, 1 allowed, -1 not allowed. */
int server_link_phone_pair_result(const uint8_t pub[PROTO_PHONE_KEY_LEN]);
/* action NULL or "": a session's challenge. It works once, for 60 seconds. */
bool server_link_phone_challenge(const char *phone_id, const char *action,
                                 char out[PROTO_SRV_B64_KEY_LEN]);
/* The signature on that challenge (action "": a session's), true with the phone's user. */
bool server_link_phone_verify(const char *phone_id, const char *challenge, const char *action,
                              const uint8_t sig[PROTO_PHONE_SIG_LEN], uint8_t *user);
size_t server_link_phone_list(proto_phone_t *out, size_t max);
uint32_t server_link_phone_revision(void);
esp_err_t server_link_phone_remove(const char *phone_id);
/* Whether phone_id is still paired, acting as user: a session it opened ends when it isn't. */
bool server_link_phone_paired(const char *phone_id, uint8_t user);
/* Direct push: its handle and push key, or none ("" and NULL) when it takes alerts from the server. */
esp_err_t server_link_phone_set_push(const char *phone_id, const char *handle, const char *kid,
                                     const uint8_t key[PROTO_PHONE_KEY_LEN], uint8_t levels);

/* The sender id the relay knows this panel by, which a phone needs to make it a handle. false the
 * first time: registering has started, ask again in a moment. */
bool server_link_push_sender(char *out, size_t cap);

/* The panel's id, available from server_link_start() on. */
void server_link_panel_id(char out[PROTO_SRV_ID_LEN]);
/* The push relay, https://push.roboalarms.com by default. */
esp_err_t server_link_set_relay(const char *base);
void server_link_relay_base(char *out, size_t cap, bool *own_ca);
const char *server_link_ca(void);
/* The Panel Manager's certificate and addresses, for a QR code (the app wires these). */
typedef bool (*server_link_cert_fn)(uint8_t sha256[32]);
void server_link_set_cert_source(server_link_cert_fn fn);
/* Through that source: false when there is none, or the Panel Manager has no certificate yet. */
bool server_link_cert_sha256(uint8_t sha256[32]);

/* Console command "server". */
esp_err_t server_link_console_register(void);

#ifdef __cplusplus
}
#endif
