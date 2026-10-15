/*
 * fw_shared.h - helpers fw.c shares with fwsrv.c inside alarm_proto (not part of its API).
 */
#ifndef ALARM_PROTO_FW_SHARED_H
#define ALARM_PROTO_FW_SHARED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_proto/proto_fw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 64 hex digits (either case) into 32 bytes; false for anything else. */
bool proto_fwi_parse_sha256(const char *hex, uint8_t out[32]);

/* url relative to base_url: "file.bin" -> same folder, "/x/file.bin" -> same host; an absolute
 * url (or no base) as it is. false when the result doesn't fit. */
bool proto_fwi_resolve_url(const char *base_url, const char *url, char *out, size_t cap);

/* Reads the "languages" array the reader is positioned at, with the rules of
 * proto_fw_manifest_languages, appending to out[*n..max). An entry's empty version stays "".
 * false: not an array, or a syntax error (r->error). */
bool proto_fwi_read_languages(pj_reader_t *r, const char *base_url, proto_fw_language_t *out,
                              size_t max, size_t *n);

/* The same for the "assets" array (proto_fw_manifest_assets' rules). */
bool proto_fwi_read_assets(pj_reader_t *r, const char *base_url, proto_fw_asset_t *out, size_t max,
                           size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_PROTO_FW_SHARED_H */
