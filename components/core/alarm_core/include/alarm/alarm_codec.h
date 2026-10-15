/*
 * alarm_codec.h - the configuration as a versioned, self-describing binary blob.
 *
 * Used for storage (NVS on ESP-IDF) and later for backups. Every field carries a stable id, so:
 *   - newer firmware reads older blobs: missing fields keep their defaults,
 *   - older firmware reads newer blobs (after an OTA rollback): unknown fields are skipped,
 *   - the in-memory struct layout (capacities, field order, padding) can change freely.
 *
 * Layout (little-endian):
 *   header  "ACFG" | format u8 | compat u8 | header_len u16 | payload_len u32 | crc32 u32
 *   payload items: id u16 | len u8 | value[len]
 * An item whose id ends in 0x00 selects the partition/zone/user/device the following items of
 * that group describe. A writer raises 'compat' only when older readers would misread it.
 * Field ids are an external contract: never reuse or renumber one (see codec.c).
 */
#ifndef ALARM_CODEC_H
#define ALARM_CODEC_H

#include "alarm/alarm_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_CONFIG_BLOB_FORMAT 1u /* what this firmware writes */
#define ALARM_CONFIG_BLOB_COMPAT 1u /* newest format this firmware reads */

/* Upper bound of an encoded configuration with the compiled capacities. The voice fields
 * (docs/features/08 voice-first-build.md D07) add 7 system bytes (0x1E) and 40 per zone
 * (0x0B: 3 + 16, 0x0C..0x0E: 3 + 4 each). */
#define ALARM_CONFIG_BLOB_MAX                                                                  \
    (279u + (64u + ALARM_NAME_BLOB_EXTRA) * ALARM_MAX_PARTITIONS +                            \
     (120u + ALARM_NAME_BLOB_EXTRA) * ALARM_MAX_ZONES +                                        \
     (80u + ALARM_NAME_BLOB_EXTRA) * ALARM_MAX_USERS +                                        \
     (48u + ALARM_NAME_BLOB_EXTRA) * ALARM_MAX_DEVICES)
/* A name longer than the legacy field adds its whole-name item (3 + up to ALARM_NAME_LEN - 1). */
#define ALARM_NAME_BLOB_EXTRA (3u + ALARM_NAME_LEN)

/* Encode cfg into buf. Unused zones, empty user slots and blank devices are left out.
 * Returns ALARM_ERR_BAD_ARG when buf is too small. */
alarm_result_t alarm_config_encode(const alarm_config_t *cfg, uint8_t *buf, size_t cap,
                                   size_t *len_out);

/* Decode and validate. Starts from alarm_config_defaults() (no zones, no users) and applies
 * every known item. ALARM_ERR_BAD_ARG for a damaged or incompatible blob, an index beyond this
 * build's capacities, or a configuration alarm_config_validate() rejects. */
alarm_result_t alarm_config_decode(const uint8_t *buf, size_t len, alarm_config_t *out);

/* CRC-32 (IEEE 802.3, reflected, as zlib). Chain calls by passing the previous result;
 * start with 0. */
uint32_t alarm_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_CODEC_H */
