#pragma once
/* The remote keypad's live screen, presses and keys over one WebSocket (docs/features/31,
 * "Remote keypad"). Private to panel_web. */
#include "dev_remote.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

/* Once, at start: the board's adapter (video, taps, keys, session banner, pause). */
esp_err_t web_screen_setup(const dev_remote_adapter_t *adapter);
/* Each time the HTTPS server starts: the /api/v1/screen route, before the wildcard API routes. */
void web_screen_register(httpd_handle_t server);
/* Just before the server is stopped (web access off, a mode change): nothing reaches it any
 * more, and the viewer goes. */
void web_screen_stop(void);
/* Just after httpd_ssl_stop() returned: a picture its worker never sent lets go of its buffer. */
void web_screen_stopped(void);
/* The server's close callback, for every socket (HTTP worker): a closing socket is forgotten
 * before its number can be given to another connection. */
void web_screen_closed(int fd);
/* The person at the panel paused the remote keypad with its red banner, or let it go on. The
 * pause lasts until they answer, whoever connects meanwhile, or until remote access goes off. */
void web_screen_pause(bool paused);

/* From panel_web.c: the signed-in session behind a WebSocket upgrade request, as for any GET
 * (cookie, same origin, web access on, no password change pending); NULL when there is none.
 * who: the name the panel's banner shows. HTTP worker. */
const void *panel_web_ws_session(httpd_req_t *r, uint8_t *user, char *who, size_t who_cap,
                                 char token[65]);
/* Is that session still signed in (not signed out, expired, replaced, its user changed)? It
 * counts as use (idle time). csrf, when not NULL, must be the session's. HTTP worker. */
bool panel_web_ws_alive(const void *session, const char token[65], const char *csrf);
