/* CP-003: versioned P4/C6 peer-data wire. Bytes, never native packed structs.
 * Payloads are authenticated by panel_link; the bridge does not interpret them. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ENB_MSG_ID   0x52414E31u
#define ENB_VERSION  1u
#define ENB_DATA_MAX 1470u
#define ENB_HEADER   14u
#define ENB_MAX      (ENB_HEADER + ENB_DATA_MAX)
enum {
    ENB_INIT = 1,
    ENB_ADD = 2,
    ENB_DEL = 3,
    ENB_SEND = 4,
    ENB_DONE = 5,
    ENB_RECV = 6,
    ENB_STATE = 7,
    ENB_INFO = 8
};
/* version, op, request LE16, MAC[6], channel, status/RSSI, length LE16, data.
 * SEND uses byte 11 for priority: 0 bulk, 1 control. */
static inline uint16_t enb_u16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}
static inline void enb_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline bool enb_valid(const uint8_t *p, size_t n)
{
    if (p == NULL || n < ENB_HEADER || p[0] != ENB_VERSION || p[1] < ENB_INIT || p[1] > ENB_INFO ||
        p[10] > 13u)
        return false;
    const size_t len = enb_u16(p + 12);
    if (len > ENB_DATA_MAX || n != ENB_HEADER + len)
        return false;
    if (p[1] == ENB_INFO) {
        if (!len || len >= 24u)
            return false;
        for (size_t i = ENB_HEADER; i < n; ++i) {
            const uint8_t c = p[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  c == '.' || c == '-' || c == '+'))
                return false;
        }
        return true;
    }
    return p[1] == ENB_SEND || p[1] == ENB_RECV ? len != 0u : len == 0u;
}
