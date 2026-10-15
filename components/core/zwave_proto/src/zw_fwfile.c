/*
 * zw_fwfile.c - a device firmware file into its image. See zw_fwfile.h.
 */
#include "zwave_proto/zw_fwfile.h"

#include <string.h>

static bool ends_with(const char *name, const char *ext)
{
    const size_t n = strlen(name);
    const size_t e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        char c = name[n - e + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

zw_fwfile_format_t zw_fwfile_guess(const char *name, const uint8_t *data, size_t len)
{
    if (name == NULL) {
        name = "";
    }
    if (ends_with(name, ".otz") || ends_with(name, ".hec") || ends_with(name, ".zip") ||
        (len >= 4u && data[0] == 'P' && data[1] == 'K' && data[2] == 3u && data[3] == 4u)) {
        return ZW_FWFILE_UNSUPPORTED;
    }
    if (len >= 4u && data[0] == 0xEBu && data[1] == 0x17u && data[2] == 0xA6u && data[3] == 0x03u) {
        return ZW_FWFILE_GECKO; /* the GBL header tag, whatever the name */
    }
    if (ends_with(name, ".gbl")) {
        return ZW_FWFILE_UNKNOWN; /* a .gbl without its tag is no Gecko file */
    }
    if (ends_with(name, ".hex") || (len > 0u && data[0] == ':')) {
        return ZW_FWFILE_HEX;
    }
    if (ends_with(name, ".bin") || ends_with(name, ".ota")) {
        return ZW_FWFILE_BINARY;
    }
    return ZW_FWFILE_UNKNOWN;
}

static int nibble(uint8_t c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* One HEX line's bytes (after ':') into rec; its byte count, 0 when damaged. */
static size_t line_bytes(const uint8_t *s, size_t n, uint8_t *rec, size_t cap)
{
    if (n < 10u || (n & 1u) != 0u || n / 2u > cap) {
        return 0u;
    }
    uint8_t sum = 0u;
    for (size_t i = 0; i < n / 2u; i++) {
        const int hi = nibble(s[2u * i]);
        const int lo = nibble(s[2u * i + 1u]);
        if (hi < 0 || lo < 0) {
            return 0u;
        }
        rec[i] = (uint8_t)(hi << 4 | lo);
        sum = (uint8_t)(sum + rec[i]);
    }
    const size_t count = n / 2u;
    if (sum != 0u || (size_t)rec[0] + 5u != count) {
        return 0u; /* checksum, or its length byte not its own length */
    }
    return count;
}

/* Two passes: the lowest and highest address, then the bytes. */
static bool walk(const uint8_t *hex, size_t len, uint8_t *out, size_t cap, uint32_t *lo,
                 uint32_t *hi, bool fill)
{
    uint32_t base = 0u;
    bool eof = false;
    size_t i = 0u;
    while (i < len && !eof) {
        while (i < len && (hex[i] == '\r' || hex[i] == '\n' || hex[i] == ' ' || hex[i] == '\t')) {
            i++;
        }
        if (i >= len) {
            break;
        }
        if (hex[i] != ':') {
            return false;
        }
        size_t j = i + 1u;
        while (j < len && hex[j] != '\r' && hex[j] != '\n') {
            j++;
        }
        uint8_t rec[4u + 5u + 255u];
        const size_t count = line_bytes(&hex[i + 1u], j - i - 1u, rec, sizeof rec);
        if (count == 0u) {
            return false;
        }
        const uint8_t n = rec[0];
        const uint32_t addr = (uint32_t)rec[1] << 8 | (uint32_t)rec[2];
        switch (rec[3]) {
        case 0x00u: {
            const uint32_t at = base + addr;
            if (at < base) {
                return false;
            }
            if (!fill) {
                if (at < *lo) {
                    *lo = at;
                }
                if (at + (uint32_t)n > *hi) {
                    *hi = at + (uint32_t)n;
                }
            } else {
                memcpy(&out[at - *lo], &rec[4], n);
            }
            break;
        }
        case 0x01u:
            eof = true;
            break;
        case 0x02u:
            if (n != 2u) {
                return false;
            }
            base = ((uint32_t)rec[4] << 8 | (uint32_t)rec[5]) << 4;
            break;
        case 0x04u:
            if (n != 2u) {
                return false;
            }
            base = ((uint32_t)rec[4] << 8 | (uint32_t)rec[5]) << 16;
            break;
        case 0x03u:
        case 0x05u:
            break; /* start addresses: nothing for the image */
        default:
            return false;
        }
        i = j;
    }
    (void)cap;
    return eof;
}

size_t zw_fwfile_hex_to_bin(const uint8_t *hex, size_t len, uint8_t *out, size_t cap)
{
    if (hex == NULL || out == NULL) {
        return 0u;
    }
    uint32_t lo = UINT32_MAX;
    uint32_t hi = 0u;
    if (!walk(hex, len, out, cap, &lo, &hi, false) || hi <= lo || hi - lo > cap) {
        return 0u;
    }
    const size_t n = hi - lo;
    memset(out, 0xFF, n);
    if (!walk(hex, len, out, cap, &lo, &hi, true)) {
        return 0u;
    }
    return n;
}
