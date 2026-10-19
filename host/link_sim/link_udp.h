/*
 * link_udp.h - one IPv4 UDP socket for the panel link on a PC (host/link_sim; docs/features/11
 * "Transports", LINK-013), on Windows (Winsock) and POSIX, and the clocks the tools run on.
 *
 * This header names no socket type, so it can be included anywhere; only link_udp.c includes the
 * OS's network headers (winsock2.h before windows.h, never next to LVGL or the UI).
 *
 * Addresses are four bytes in network order (192.0.2.10 is {192, 0, 2, 10}), ports in host order.
 *
 * Loopback only: a socket opened with loopback_only binds only to 127.0.0.0/8, sends only there
 * and drops every datagram from elsewhere. A tool whose crypto port is the seeded test port
 * (plink_crypto_t.insecure, link_smoke) must open its sockets that way (docs/features/11
 * "Design").
 */
#ifndef LINK_UDP_H
#define LINK_UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool open;
    bool loopback_only;
    uintptr_t fd;    /* the SOCKET (Windows) or the descriptor (POSIX) */
    uint8_t ip[4];   /* bound to */
    uint16_t port;   /* the local port, read back after binding (port 0 picks one) */
    char error[96];  /* the last failure, for a message */
} link_udp_t;

/* Once per process before the first socket (WSAStartup on Windows); false: no sockets here. */
bool link_udp_startup(void);
void link_udp_cleanup(void);

/* Binds ip:port (port 0: the OS picks one, read back into u->port). broadcast lets it send to
 * 255.255.255.255. The port is never shared (no SO_REUSEADDR: UDP has no TIME_WAIT, so a
 * restarted master gets it back anyway, and two masters on one port would split a keypad's
 * frames). false: u->error says why. */
bool link_udp_open(link_udp_t *u, const uint8_t ip[4], uint16_t port, bool broadcast,
                   bool loopback_only);
void link_udp_close(link_udp_t *u);

/* Waits up to timeout_ms (0: just looks) for one datagram of at most cap bytes: its length, 0
 * when none came, -1 on an error (u->error). A longer datagram is dropped (the link's frames are
 * at most 1200 bytes); on a loopback-only socket so is one from elsewhere. */
int link_udp_recv(link_udp_t *u, uint8_t *buf, size_t cap, uint8_t from_ip[4],
                  uint16_t *from_port, uint32_t timeout_ms);

/* Sends one datagram; false when the OS refused it (UDP: nothing says it arrived) or a
 * loopback-only socket was asked to send elsewhere. */
bool link_udp_send(link_udp_t *u, const uint8_t ip[4], uint16_t port, const uint8_t *buf,
                   size_t len);

/* "192.0.2.10" -> {192, 0, 2, 10}; false when it isn't a dotted IPv4 address. */
bool link_udp_parse_ip(const char *s, uint8_t ip[4]);
/* "192.0.2.10" or "192.0.2.10:6056"; the port defaults to def_port. */
bool link_udp_parse_endpoint(const char *s, uint16_t def_port, uint8_t ip[4], uint16_t *port);
/* 127.0.0.0/8 */
bool link_udp_is_loopback(const uint8_t ip[4]);

/* A monotonic clock in ms (never goes back, starts anywhere). */
uint64_t link_os_mono_ms(void);
/* The wall clock in ms since the Unix epoch. */
int64_t link_os_epoch_ms(void);
/* Sleeps about ms. */
void link_os_sleep_ms(uint32_t ms);

#endif /* LINK_UDP_H */
