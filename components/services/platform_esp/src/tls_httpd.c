/*
 * tls_httpd.c - an HTTPS server behind the panel's TLS gate (tls_httpd.h, SEC-012). The session
 * handling follows esp_https_server (ESP-IDF 6.1, components/esp_https_server/src/https_server.c)
 * without its events; what is new is the gate in tls_open(), before esp_tls allocates anything.
 */
#include "tls_httpd.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "tls_gate.h"

static const char *TAG = "tls_httpd";

typedef struct {
    esp_tls_cfg_server_t *cfg;
    const char *who;
    httpd_open_func_t open_fn; /* the caller's, after the handshake */
    void (*on_session)(httpd_handle_t server, int fd, esp_tls_t *tls);
} tls_server_t;

static void tls_close(void *ctx)
{
    esp_tls_server_session_delete(ctx);
}

static int tls_pending(httpd_handle_t server, int fd)
{
    esp_tls_t *tls = httpd_sess_get_transport_ctx(server, fd);
    return tls != NULL ? esp_tls_get_bytes_avail(tls) : -1;
}

static int tls_recv(httpd_handle_t server, int fd, char *buf, size_t len, int flags)
{
    (void)flags;
    esp_tls_t *tls = httpd_sess_get_transport_ctx(server, fd);
    return tls != NULL ? esp_tls_conn_read(tls, buf, len) : -1;
}

static int tls_send(httpd_handle_t server, int fd, const char *buf, size_t len, int flags)
{
    (void)flags;
    esp_tls_t *tls = httpd_sess_get_transport_ctx(server, fd);
    return tls != NULL ? esp_tls_conn_write(tls, buf, len) : -1;
}

static esp_err_t tls_open(httpd_handle_t server, int fd)
{
    tls_server_t *srv = httpd_get_global_transport_ctx(server);
    if (!tls_gate_fd(fd, srv->who)) {
        return ESP_FAIL; /* httpd closes the socket: nothing was allocated for it */
    }
    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (esp_tls_server_session_create(srv->cfg, fd, tls) != 0) {
        tls_gate_fd_failed(fd);
        esp_tls_server_session_delete(tls);
        return ESP_FAIL;
    }
    httpd_sess_set_transport_ctx(server, fd, tls, tls_close);
    httpd_sess_set_send_override(server, fd, tls_send);
    httpd_sess_set_recv_override(server, fd, tls_recv);
    httpd_sess_set_pending_override(server, fd, tls_pending);
    if (srv->on_session != NULL) {
        srv->on_session(server, fd, tls);
    }
    if (srv->open_fn != NULL) {
        (void)srv->open_fn(server, fd);
    }
    return ESP_OK;
}

static void free_server(void *ctx)
{
    tls_server_t *srv = ctx;
    esp_tls_cfg_server_session_tickets_free(srv->cfg);
    free((void *)srv->cfg->servercert_buf);
    free((void *)srv->cfg->serverkey_buf);
    free((void *)srv->cfg->cacert_buf);
    free(srv->cfg);
    free(srv);
}

static unsigned char *copy(const uint8_t *p, size_t n)
{
    unsigned char *c = n != 0u ? malloc(n) : NULL;
    if (c != NULL) {
        memcpy(c, p, n);
    }
    return c;
}

esp_err_t tls_httpd_start(httpd_handle_t *out, httpd_config_t *config, const tls_httpd_opts_t *opts)
{
    tls_server_t *srv = calloc(1, sizeof(*srv));
    esp_tls_cfg_server_t *cfg = calloc(1, sizeof(*cfg));
    if (srv == NULL || cfg == NULL) {
        free(srv);
        free(cfg);
        return ESP_ERR_NO_MEM;
    }
    srv->cfg = cfg;
    cfg->servercert_buf = copy(opts->cert, opts->cert_len);
    cfg->servercert_bytes = (unsigned int)opts->cert_len;
    cfg->serverkey_buf = copy(opts->key, opts->key_len);
    cfg->serverkey_bytes = (unsigned int)opts->key_len;
    if (opts->client_ca != NULL) {
        cfg->cacert_buf = copy(opts->client_ca, opts->client_ca_len);
        cfg->cacert_bytes = (unsigned int)opts->client_ca_len;
    }
    if (cfg->servercert_buf == NULL || cfg->serverkey_buf == NULL ||
        (opts->client_ca != NULL && cfg->cacert_buf == NULL)) {
        free_server(srv);
        return ESP_ERR_NO_MEM;
    }
    cfg->tls_handshake_timeout_ms = opts->handshake_timeout_ms;
#if CONFIG_ESP_TLS_SERVER_SESSION_TICKETS
    if (opts->session_tickets && esp_tls_cfg_server_session_tickets_init(cfg) != ESP_OK) {
        ESP_LOGW(TAG, "%s: no session tickets, every connection is a full handshake", opts->who);
    }
#endif
    srv->who = opts->who;
    srv->on_session = opts->on_session;
    srv->open_fn = config->open_fn;
    config->open_fn = tls_open;
    config->global_transport_ctx = srv;
    config->global_transport_ctx_free_fn = free_server;
    config->server_port = opts->port;
    /* httpd logs every connection it closes for the gate: a flood would fill the log and hold the
     * server on the UART. tls_gate_fd() says it once every ten seconds instead. */
    esp_log_level_set("httpd", ESP_LOG_NONE);
    const esp_err_t e = httpd_start(out, config);
    if (e != ESP_OK) {
        free_server(srv);
    }
    return e;
}

esp_err_t tls_httpd_stop(httpd_handle_t server)
{
    return httpd_stop(server);
}
