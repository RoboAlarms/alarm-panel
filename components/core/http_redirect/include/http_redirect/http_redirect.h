#pragma once
/*
 * http_redirect.h - where a plain-HTTP request to the panel is sent: the same page over HTTPS
 * (docs/features/31 WEB-023).
 *
 * The browser's Host header names the address the person typed (an IP, a DNS name, a VPN
 * address), so the redirect keeps it - but only a Host that is plainly a host name, an IPv4
 * address or a bracketed IPv6 address, with an optional port that is dropped. Anything else
 * (control characters, spaces, '/', '@', '\', quotes, an empty or overlong value) is replaced by
 * the panel's own address, so the Location header can never be split or pointed somewhere
 * else. The path and query are kept when they are printable ASCII starting with '/', else "/".
 *
 * Portable C11: no heap, no globals.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_REDIRECT_HOST_MAX 253 /* a DNS name's longest */

/* The Host header's host part (no port), or false when it is not one to repeat. */
bool http_redirect_host_ok(const char *host_header, char *host, size_t host_cap);

/* The Location for a plain-HTTP request: "https://" host [":" https_port unless 443] uri.
 * host_header may be NULL (none sent); fallback_host is the panel's own address and is checked
 * the same way. false: neither host is usable, or out is too small even for "/". */
bool http_redirect_location(const char *host_header, const char *fallback_host, const char *uri,
                            uint16_t https_port, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
