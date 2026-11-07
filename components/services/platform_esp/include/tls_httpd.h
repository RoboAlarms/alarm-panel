/*
 * tls_httpd.h - an HTTPS server with the panel's TLS gate in front of the handshake (tls_gate.h,
 * SEC-012). The session handling is esp_https_server's, but esp_https_server runs its handshake
 * before any hook of ours, so a flood of handshakes from the home network cost internal RAM the
 * Wi-Fi co-processor needed (docs/bugs/B033). Here a refused connection is closed before mbedTLS
 * allocates anything. Used by the Panel Manager and the developer tools.
 */
#ifndef TLS_HTTPD_H
#define TLS_HTTPD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"
#include "esp_tls.h"

typedef struct {
    const char *who;               /* names the server in the gate's log line */
    uint16_t port;
    const uint8_t *cert;           /* the server's certificate (DER or PEM with its NUL) */
    size_t cert_len;
    const uint8_t *key;
    size_t key_len;
    const uint8_t *client_ca;      /* NULL: no client certificate asked for */
    size_t client_ca_len;
    uint32_t handshake_timeout_ms;
    bool session_tickets;          /* a client that reconnects resumes without a full handshake */
    /* After a handshake, before httpd's own open_fn (the config's, if any). */
    void (*on_session)(httpd_handle_t server, int fd, esp_tls_t *tls);
} tls_httpd_opts_t;

/* Starts httpd with config (copied; its open_fn runs after the handshake). The buffers are copied. */
esp_err_t tls_httpd_start(httpd_handle_t *out, httpd_config_t *config, const tls_httpd_opts_t *opts);
esp_err_t tls_httpd_stop(httpd_handle_t server);

#endif
