/*
 * cc1101_regs.c - the CC1101's receive table and arithmetic (see cc1101_regs.h).
 */
#include "cc1101_regs/cc1101_regs.h"

/* OOK receive, asynchronous serial output on GDO0, in the order it is written. The table working
 * open-source receivers converge on (rtl_433_ESP, the ELECHOUSE driver) and TI DN022's OOK
 * values; FREQ and TEST0 are added per frequency. */
static const cc1101_reg_t OOK_TABLE[] = {
    {0x00, 0x2E}, /* IOCFG2: GDO2 high impedance (not wired) */
    {0x01, 0x2E}, /* IOCFG1: GDO1 is the SPI MISO */
    {0x02, CC1101_IOCFG0_ASYNC}, /* IOCFG0: serial data out, asynchronous serial mode */
    {0x03, 0x47}, /* FIFOTHR: DN022 for RX filter bandwidths up to 325 kHz */
    {0x06, 0x00}, /* PKTLEN: unused in infinite length */
    {0x07, 0x04}, /* PKTCTRL1: unused in asynchronous mode */
    {0x08, 0x32}, /* PKTCTRL0: asynchronous serial mode, infinite packet length */
    {0x0A, 0x00}, /* CHANNR: the base frequency only */
    {0x0B, 0x06}, /* FSCTRL1: 152 kHz IF */
    {0x0C, 0x00}, /* FSCTRL0: no frequency offset */
    {0x10, 0x57}, /* MDMCFG4: 325 kHz RX filter bandwidth, data rate exponent 7 */
    {0x11, 0x83}, /* MDMCFG3: 4.8 kBaud - the demodulator samples at 8x this (TI, async mode) */
    {0x12, 0x30}, /* MDMCFG2: DC filter on, ASK/OOK, no Manchester, no sync word */
    {0x13, 0x22}, /* MDMCFG1 */
    {0x14, 0xF8}, /* MDMCFG0 */
    {0x15, 0x47}, /* DEVIATN: no effect on OOK */
    {0x16, 0x07}, /* MCSM2: RX_TIME_RSSI off (it breaks asynchronous mode) */
    {0x17, 0x30}, /* MCSM1 */
    {0x18, 0x18}, /* MCSM0: calibrate on IDLE -> RX, PO_TIMEOUT 2 */
    {0x19, 0x14}, /* FOCCFG: FOC_LIMIT 0 - frequency offset compensation is not for OOK */
    {0x1A, 0x1C}, /* BSCFG: bit sync unused in asynchronous mode */
    {0x1B, 0x07}, /* AGCCTRL2: OOK (DN022: 0x03..0x07) */
    {0x1C, 0x00}, /* AGCCTRL1: OOK */
    {0x1D, 0x91}, /* AGCCTRL0: OOK decision boundary; 0x92 and 0x93 made noise on the bench */
    {0x20, 0xFB}, /* WORCTRL */
    {0x21, 0xB6}, /* FREND1: RX front end for bandwidths over 101 kHz */
    {0x22, 0x11}, /* FREND0: PA table index for OOK (TX only, never used) */
    {0x23, 0xE9}, /* FSCAL3 */
    {0x24, 0x2A}, /* FSCAL2 */
    {0x25, 0x00}, /* FSCAL1 */
    {0x26, 0x1F}, /* FSCAL0 */
    {0x29, 0x59}, /* FSTEST (its reset value). 0x2A next to it is PTEST, test-only: never
                   * written (it was, with this value, until 2026-09-25: docs/bugs/B011) */
    {0x2C, 0x81}, /* TEST2: bandwidths up to 325 kHz */
    {0x2D, 0x35}, /* TEST1 */
};

bool cc1101_band_ok(uint32_t freq_khz)
{
    return freq_khz >= CC1101_FREQ_MIN_KHZ && freq_khz <= CC1101_FREQ_MAX_KHZ &&
           !(freq_khz > 348000u && freq_khz < 387000u);
}

uint32_t cc1101_freq_word(uint32_t freq_khz)
{
    return (uint32_t)(((uint64_t)freq_khz * 1000u * 65536u + CC1101_XTAL_HZ / 2u) / CC1101_XTAL_HZ);
}

size_t cc1101_ook_table(uint32_t freq_khz, cc1101_reg_t *out, size_t max)
{
    const size_t base = sizeof(OOK_TABLE) / sizeof(OOK_TABLE[0]);
    if (!cc1101_band_ok(freq_khz) || max < base + 4u) {
        return 0u;
    }
    for (size_t i = 0; i < base; i++) {
        out[i] = OOK_TABLE[i];
    }
    const uint32_t w = cc1101_freq_word(freq_khz);
    out[base] = (cc1101_reg_t){CC1101_FREQ2, (uint8_t)(w >> 16)};
    out[base + 1u] = (cc1101_reg_t){CC1101_FREQ1, (uint8_t)(w >> 8)};
    out[base + 2u] = (cc1101_reg_t){CC1101_FREQ0, (uint8_t)w};
    /* TEST0: 0x0B below 322.88 MHz, 0x09 above (SmartRF Studio; VCO_SEL_CAL_EN) */
    out[base + 3u] = (cc1101_reg_t){CC1101_TEST0, freq_khz < 322880u ? 0x0Bu : 0x09u};
    return base + 4u;
}

size_t cc1101_verify(const cc1101_reg_t *table, size_t n, const uint8_t *read)
{
    for (size_t i = 0; i < n; i++) {
        if (read[i] != table[i].value) {
            return i;
        }
    }
    return n;
}

int cc1101_rssi_dbm(uint8_t raw)
{
    const int v = raw >= 128u ? (int)raw - 256 : (int)raw;
    return v / 2 - 74;
}

cc1101_rssi_result_t cc1101_rssi_settle(cc1101_read_fn read, void *ctx, unsigned max_reads,
                                        uint8_t *raw, uint8_t *lowest)
{
    uint8_t before = 0u;
    uint8_t low = 0u;
    for (unsigned i = 0u; i < max_reads; i++) {
        uint8_t v = 0u;
        if (!read(ctx, &v)) {
            return CC1101_RSSI_BUS_ERROR;
        }
        if (i > 0u && v == before) {
            *raw = v;
            return CC1101_RSSI_STABLE;
        }
        /* in dBm: the register is two's complement (0x80 is the weakest, 0x7F the strongest) */
        if (i == 0u || cc1101_rssi_dbm(v) < cc1101_rssi_dbm(low)) {
            low = v;
        }
        before = v;
    }
    if (max_reads > 0u && lowest != NULL) {
        *lowest = low;
    }
    return CC1101_RSSI_UNSTABLE;
}

uint32_t cc1101_edges_per_s(uint32_t count, uint32_t last, uint64_t elapsed_us)
{
    const uint64_t edges = (uint64_t)(uint32_t)(count - last) * 2u; /* unsigned: wraps once */
    const uint64_t us = elapsed_us < 1000000u ? 1000000u : elapsed_us;
    const uint64_t rate = edges * 1000000u / us; /* < 2^33 * 10^6: fits */
    return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}

const char *cc1101_state_name(uint8_t marcstate)
{
    switch (marcstate) {
    case 0x00: return "sleep";
    case 0x01: return "idle";
    case 0x02: return "xoff";
    case 0x03: case 0x04: case 0x05: return "calibrating";
    case 0x06: case 0x07: return "settling";
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: return "calibrating";
    case 0x0D: return "rx";
    case 0x0E: return "rx end";
    case 0x0F: return "rx reset";
    case 0x11: return "rx overflow";
    case 0x13: return "tx";
    case 0x16: return "tx underflow";
    case 0xFF: return "no answer";
    default: return "other";
    }
}

cc1101_identity_t cc1101_identify(uint8_t partnum, uint8_t version, uint8_t version_again)
{
    /* A floating bus reads 0x00 or 0xFF everywhere; a chip has PARTNUM 0 and a version that is
     * neither and stays put between reads. */
    if (partnum != 0x00u || version == 0x00u || version == 0xFFu || version != version_again) {
        return CC1101_ABSENT;
    }
    return version == 0x14u || version == 0x04u ? CC1101_GENUINE : CC1101_CLONE;
}

bool cc1101_lost_config(uint8_t iocfg0)
{
    return iocfg0 != CC1101_IOCFG0_ASYNC;
}
