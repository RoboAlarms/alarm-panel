# http_redirect

**Tier:** core (portable C11) · **Features:** [31 Browser Panel Manager](../../../docs/features/31-Browser-Panel-Manager/README.md) (WEB-023) ·
**Tests:** [`test/`](test/) (1 suite: `http_redirect`)

Where a plain-HTTP request to the panel is sent: the same page over HTTPS. `panel_web` runs a
port-80 listener beside its HTTPS server that answers every GET and HEAD with a `301` to this
address and serves nothing else.

The browser's `Host` header names the address the person typed (an IP, a DNS name, a VPN
address), so the redirect keeps it - but only when it is plainly a host name, an IPv4 address
or a bracketed IPv6 address, with an optional port (dropped). Anything else (control
characters, spaces, `/`, `@`, `\`, quotes, a leading dot or dash, a bad port, more than 253
characters) is replaced by the panel's own address, so the `Location` header can't be split
into another header or point somewhere else. The path and query are kept when they are
printable ASCII starting with a single `/`, else the start page; a path too long for the buffer
also becomes the start page.

## Public API
| Header | What it offers |
|---|---|
| `include/http_redirect/http_redirect.h` | `http_redirect_location(host_header, fallback_host, uri, https_port, out, cap)`, `http_redirect_host_ok(host_header, host, cap)`, `HTTP_REDIRECT_HOST_MAX` |

## Depends on
- Project components: none.
- ESP-IDF: none (portable).

## Options
None.

## Rules
- Plain C11: no malloc, no globals, no RTOS or ESP-IDF headers; the host build stays warning-free.
- Never repeat anything from the request that isn't checked here.

## Tests
`http_redirect` (`test_http_redirect.c`, tag `WEB-023`): the same page over HTTPS (names, IPv4,
IPv6, ports dropped, another HTTPS port kept), strange hosts replaced (header injection, `/`,
`@`, `\`, quotes, bad ports, unclosed IPv6, non-ASCII, too long), strange paths becoming the
start page (`//host`, spaces, CR/LF, absolute URLs), and the buffer bounds.
