/*
 * link_udp.c - see link_udp.h. The only file of host/link_sim that includes the OS's network
 * headers: winsock2.h comes before windows.h, and the POSIX names are asked for explicitly because
 * the host builds compile as plain C11 (CMAKE_C_EXTENSIONS OFF).
 */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* WSAPoll */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

#include "link_udp.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
typedef SOCKET sock_t;
typedef int sock_len_t;
#define SOCK_BAD INVALID_SOCKET
/* Not every MinGW has mswsock.h's name for it: an ICMP "port unreachable" must not make the next
 * receive fail (WSAECONNRESET) after a keypad or master went away. */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
typedef int sock_t;
typedef socklen_t sock_len_t;
#define SOCK_BAD (-1)
#endif

/* Datagrams are received here first, so one longer than the caller's buffer is seen and dropped
 * instead of read cut short (POSIX recvfrom doesn't say it cut one). */
#define LINK_UDP_RX_MAX 2048u

static sock_t sock_of(const link_udp_t *u)
{
    return (sock_t)u->fd;
}

static int os_error(void)
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

static void set_error(link_udp_t *u, const char *what)
{
    snprintf(u->error, sizeof(u->error), "%s", what);
}

static void set_os_error(link_udp_t *u, const char *call)
{
    const int err = os_error();
#if defined(_WIN32)
    snprintf(u->error, sizeof(u->error), "%s failed (Winsock error %d)", call, err);
#else
    snprintf(u->error, sizeof(u->error), "%s failed: %s", call, strerror(err));
#endif
}

static void sock_close(sock_t s)
{
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

static void to_sockaddr(const uint8_t ip[4], uint16_t port, struct sockaddr_in *a)
{
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    memcpy(&a->sin_addr, ip, 4);
    /* Network order by hand: glibc's htons trips -Wconversion on some versions. */
    uint8_t *p = (uint8_t *)&a->sin_port;
    p[0] = (uint8_t)(port >> 8);
    p[1] = (uint8_t)(port & 0xFFu);
}

static void from_sockaddr(const struct sockaddr_in *a, uint8_t ip[4], uint16_t *port)
{
    memcpy(ip, &a->sin_addr, 4);
    const uint8_t *p = (const uint8_t *)&a->sin_port;
    *port = (uint16_t)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

bool link_udp_startup(void)
{
#if defined(_WIN32)
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void link_udp_cleanup(void)
{
#if defined(_WIN32)
    WSACleanup();
#endif
}

bool link_udp_is_loopback(const uint8_t ip[4])
{
    return ip[0] == 127u;
}

bool link_udp_open(link_udp_t *u, const uint8_t ip[4], uint16_t port, bool broadcast,
                   bool loopback_only)
{
    memset(u, 0, sizeof(*u));
    u->loopback_only = loopback_only;
    if (loopback_only && !link_udp_is_loopback(ip)) {
        set_error(u, "this socket may only use 127.0.0.0/8 (the crypto port is not secure)");
        return false;
    }
    const sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCK_BAD) {
        set_os_error(u, "socket");
        return false;
    }
    int on = 1;
    /* No SO_REUSEADDR: UDP has no TIME_WAIT, so a restarted master gets its port back anyway, and
     * a shared port would split a keypad's frames between two masters. On Windows, keep others
     * from taking the port over. */
#if defined(_WIN32)
    (void)setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, (sock_len_t)sizeof(on));
#endif
    if (broadcast &&
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, (sock_len_t)sizeof(on)) != 0) {
        set_os_error(u, "setsockopt(SO_BROADCAST)");
        sock_close(s);
        return false;
    }
    /* Room for a burst: a configuration pull, statuses and events from a master with keypads. */
    int rcvbuf = 256 * 1024;
    (void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, (sock_len_t)sizeof(rcvbuf));
#if defined(_WIN32)
    BOOL report = FALSE;
    DWORD bytes = 0;
    (void)WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), NULL, 0, &bytes, NULL, NULL);
#else
    const int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0) {
        (void)fcntl(s, F_SETFL, fl | O_NONBLOCK);
    }
#endif
    struct sockaddr_in a;
    to_sockaddr(ip, port, &a);
    if (bind(s, (const struct sockaddr *)&a, (sock_len_t)sizeof(a)) != 0) {
        set_os_error(u, "bind");
        sock_close(s);
        return false;
    }
    sock_len_t alen = (sock_len_t)sizeof(a);
    memset(&a, 0, sizeof(a));
    if (getsockname(s, (struct sockaddr *)&a, &alen) != 0) {
        set_os_error(u, "getsockname");
        sock_close(s);
        return false;
    }
    uint8_t bound_ip[4];
    from_sockaddr(&a, bound_ip, &u->port);
    memcpy(u->ip, ip, 4);
    u->fd = (uintptr_t)s;
    u->open = true;
    return true;
}

void link_udp_close(link_udp_t *u)
{
    if (u->open) {
        sock_close(sock_of(u));
    }
    u->open = false;
}

int link_udp_recv(link_udp_t *u, uint8_t *buf, size_t cap, uint8_t from_ip[4],
                  uint16_t *from_port, uint32_t timeout_ms)
{
    if (!u->open) {
        set_error(u, "the socket is closed");
        return -1;
    }
    const sock_t s = sock_of(u);
    uint8_t rx[LINK_UDP_RX_MAX];
    for (;;) {
#if defined(_WIN32)
        WSAPOLLFD p;
        p.fd = s;
        p.events = POLLRDNORM;
        p.revents = 0;
        const int r = WSAPoll(&p, 1, (INT)timeout_ms);
#else
        struct pollfd p;
        p.fd = s;
        p.events = POLLIN;
        p.revents = 0;
        const int r = poll(&p, 1, (int)timeout_ms);
        if (r < 0 && errno == EINTR) {
            return 0;
        }
#endif
        if (r == 0) {
            return 0;
        }
        if (r < 0) {
            set_os_error(u, "poll");
            return -1;
        }
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        sock_len_t alen = (sock_len_t)sizeof(a);
#if defined(_WIN32)
        const int n = recvfrom(s, (char *)rx, (int)sizeof(rx), 0, (struct sockaddr *)&a, &alen);
        if (n < 0) {
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                return 0;
            }
            if (err == WSAECONNRESET || err == WSAEMSGSIZE) {
                timeout_ms = 0u; /* a stale ICMP or an oversized datagram: dropped, look again */
                continue;
            }
            set_os_error(u, "recvfrom");
            return -1;
        }
#else
        const ssize_t n = recvfrom(s, rx, sizeof(rx), 0, (struct sockaddr *)&a, &alen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return 0;
            }
            if (errno == ECONNREFUSED) {
                timeout_ms = 0u;
                continue;
            }
            set_os_error(u, "recvfrom");
            return -1;
        }
#endif
        uint8_t ip[4];
        uint16_t port;
        from_sockaddr(&a, ip, &port);
        if (a.sin_family != AF_INET || (size_t)n > cap ||
            (u->loopback_only && !link_udp_is_loopback(ip))) {
            timeout_ms = 0u; /* not for us: dropped, look again */
            continue;
        }
        memcpy(buf, rx, (size_t)n);
        memcpy(from_ip, ip, 4);
        *from_port = port;
        return (int)n;
    }
}

bool link_udp_send(link_udp_t *u, const uint8_t ip[4], uint16_t port, const uint8_t *buf,
                   size_t len)
{
    if (!u->open) {
        set_error(u, "the socket is closed");
        return false;
    }
    if (u->loopback_only && !link_udp_is_loopback(ip)) {
        set_error(u, "refused: this socket may only send to 127.0.0.0/8");
        return false;
    }
    if (len > LINK_UDP_RX_MAX) {
        set_error(u, "refused: datagram too long");
        return false;
    }
    struct sockaddr_in a;
    to_sockaddr(ip, port, &a);
#if defined(_WIN32)
    const int n = sendto(sock_of(u), (const char *)buf, (int)len, 0, (const struct sockaddr *)&a,
                         (sock_len_t)sizeof(a));
    if (n < 0 || (size_t)n != len) {
#else
    const ssize_t n = sendto(sock_of(u), buf, len, 0, (const struct sockaddr *)&a,
                             (sock_len_t)sizeof(a));
    if (n < 0 || (size_t)n != len) {
#endif
        set_os_error(u, "sendto");
        return false;
    }
    return true;
}

bool link_udp_parse_ip(const char *s, uint8_t ip[4])
{
    const char *p = s;
    for (unsigned i = 0; i < 4u; i++) {
        unsigned v = 0;
        unsigned digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4u) {
            v = v * 10u + (unsigned)(*p - '0');
            p++;
            digits++;
        }
        if (digits == 0u || digits > 3u || v > 255u) {
            return false;
        }
        ip[i] = (uint8_t)v;
        if (i < 3u) {
            if (*p != '.') {
                return false;
            }
            p++;
        }
    }
    return *p == '\0';
}

bool link_udp_parse_endpoint(const char *s, uint16_t def_port, uint8_t ip[4], uint16_t *port)
{
    char host[32];
    const char *colon = strchr(s, ':');
    const size_t hlen = colon != NULL ? (size_t)(colon - s) : strlen(s);
    if (hlen == 0u || hlen >= sizeof(host)) {
        return false;
    }
    memcpy(host, s, hlen);
    host[hlen] = '\0';
    if (!link_udp_parse_ip(host, ip)) {
        return false;
    }
    *port = def_port;
    if (colon != NULL) {
        unsigned v = 0;
        const char *p = colon + 1;
        if (*p == '\0') {
            return false;
        }
        for (; *p != '\0'; p++) {
            if (*p < '0' || *p > '9') {
                return false;
            }
            v = v * 10u + (unsigned)(*p - '0');
            if (v > 65535u) {
                return false;
            }
        }
        *port = (uint16_t)v;
    }
    return true;
}

uint64_t link_os_mono_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

int64_t link_os_epoch_ms(void)
{
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    return (int64_t)((t - 116444736000000000ull) / 10000u); /* 100 ns since 1601 */
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#endif
}

void link_os_sleep_ms(uint32_t ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}
