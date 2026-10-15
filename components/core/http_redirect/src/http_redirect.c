/*
 * http_redirect.c - the HTTPS address for a plain-HTTP request (docs/features/31 WEB-023).
 * See http_redirect.h.
 */
#include "http_redirect/http_redirect.h"

#include <stdio.h>
#include <string.h>

static bool is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool is_hex(char c)
{
    return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || (c >= '0' && c <= '9');
}

/* ":1..65535" or nothing. */
static bool port_ok(const char *p, size_t len)
{
    if (len == 0u) {
        return true;
    }
    if (p[0] != ':' || len < 2u || len > 6u) {
        return false;
    }
    unsigned long v = 0;
    for (size_t i = 1; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        v = v * 10u + (unsigned long)(p[i] - '0');
    }
    return v >= 1u && v <= 65535u;
}

bool http_redirect_host_ok(const char *host_header, char *host, size_t host_cap)
{
    if (host_header == NULL) {
        return false;
    }
    /* Optional whitespace around a header value (RFC 9110 5.5) is not part of it. */
    while (*host_header == ' ' || *host_header == '\t') {
        host_header++;
    }
    size_t len = strlen(host_header);
    while (len > 0u && (host_header[len - 1u] == ' ' || host_header[len - 1u] == '\t')) {
        len--;
    }
    if (len == 0u) {
        return false;
    }
    size_t name_len;
    if (host_header[0] == '[') { /* an IPv6 literal: hex digits, ':' and '.' (an IPv4 tail) */
        const char *close = memchr(host_header, ']', len);
        if (close == NULL) {
            return false;
        }
        name_len = (size_t)(close - host_header) + 1u;
        if (name_len < 4u) { /* "[::]" is the shortest */
            return false;
        }
        for (size_t i = 1; i + 1u < name_len; i++) {
            const char c = host_header[i];
            if (!is_hex(c) && c != ':' && c != '.') {
                return false;
            }
        }
    } else { /* a DNS name or an IPv4 address */
        const char *colon = memchr(host_header, ':', len);
        name_len = colon != NULL ? (size_t)(colon - host_header) : len;
        if (name_len == 0u || !is_alnum(host_header[0]) || host_header[name_len - 1u] == '-') {
            return false;
        }
        for (size_t i = 0; i < name_len; i++) {
            const char c = host_header[i];
            if (!is_alnum(c) && c != '.' && c != '-') {
                return false;
            }
        }
    }
    if (name_len > HTTP_REDIRECT_HOST_MAX || !port_ok(host_header + name_len, len - name_len) ||
        name_len + 1u > host_cap) {
        return false;
    }
    memcpy(host, host_header, name_len);
    host[name_len] = '\0';
    return true;
}

/* A request target to repeat: origin-form ("/path?query"), printable ASCII, no spaces. */
static bool uri_ok(const char *uri)
{
    if (uri == NULL || uri[0] != '/' || uri[1] == '/') { /* "//x" would read as another host */
        return false;
    }
    for (const char *p = uri; *p != '\0'; p++) {
        if ((unsigned char)*p <= 0x20u || (unsigned char)*p >= 0x7Fu || *p == '\\') {
            return false;
        }
    }
    return true;
}

bool http_redirect_location(const char *host_header, const char *fallback_host, const char *uri,
                            uint16_t https_port, char *out, size_t out_cap)
{
    char host[HTTP_REDIRECT_HOST_MAX + 1];
    if (!http_redirect_host_ok(host_header, host, sizeof(host)) &&
        !http_redirect_host_ok(fallback_host, host, sizeof(host))) {
        return false;
    }
    char port[8] = "";
    if (https_port != 443u) {
        snprintf(port, sizeof(port), ":%u", (unsigned)https_port);
    }
    const char *path = uri_ok(uri) ? uri : "/";
    int n = snprintf(out, out_cap, "https://%s%s%s", host, port, path);
    if (n < 0 || (size_t)n >= out_cap) { /* a very long path: the start page instead */
        n = snprintf(out, out_cap, "https://%s%s/", host, port);
    }
    return n > 0 && (size_t)n < out_cap;
}
