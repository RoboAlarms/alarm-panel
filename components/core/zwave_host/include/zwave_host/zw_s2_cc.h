/*
 * zw_s2_cc.h - the Security 2 command class's wire format (docs/features/36, security.md §5-7,
 * §9): the key-exchange commands, the nonce commands, Message Encapsulation with its extensions
 * and Commands Supported. It is the security manager's own format (zw_s2.h), so it lives here and
 * not with the other command classes in zwave_proto.
 *
 * Conventions as zwave_proto's zw_cc.h: a codec works on the whole command (`9F | command |
 * parameters`); build() writes both ID bytes and returns the command's length, or 0 when it does
 * not fit or an argument is invalid; parse() checks both ID bytes and the minimum length, never
 * reads past `len`, and accepts trailing bytes. Keys, entropy inputs and lists are views into the
 * buffer parsed. No state, no heap.
 */
#ifndef ZW_S2_CC_H
#define ZW_S2_CC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_S2_CC 0x9Fu

#define ZW_S2_NONCE_GET                  0x01u
#define ZW_S2_NONCE_REPORT               0x02u
#define ZW_S2_MESSAGE_ENCAPSULATION      0x03u
#define ZW_S2_KEX_GET                    0x04u
#define ZW_S2_KEX_REPORT                 0x05u
#define ZW_S2_KEX_SET                    0x06u
#define ZW_S2_KEX_FAIL                   0x07u
#define ZW_S2_PUBLIC_KEY_REPORT          0x08u
#define ZW_S2_NETWORK_KEY_GET            0x09u
#define ZW_S2_NETWORK_KEY_REPORT         0x0Au
#define ZW_S2_NETWORK_KEY_VERIFY         0x0Bu
#define ZW_S2_TRANSFER_END               0x0Cu
#define ZW_S2_COMMANDS_SUPPORTED_GET     0x0Du
#define ZW_S2_COMMANDS_SUPPORTED_REPORT  0x0Eu

/* §1: the key bits of the KEX key masks. */
#define ZW_S2_KEY_UNAUTH 0x01u
#define ZW_S2_KEY_AUTH   0x02u
#define ZW_S2_KEY_ACCESS 0x04u
#define ZW_S2_KEY_S0     0x80u
#define ZW_S2_KEYS_S2    (ZW_S2_KEY_UNAUTH | ZW_S2_KEY_AUTH | ZW_S2_KEY_ACCESS)

/* §5: KEX Report / KEX Set. flags: bit 0 echo, bit 1 CSA (requested in a Report, permitted in a
 * Set), bits 7..2 reserved (an echo repeats them). */
#define ZW_S2_KEX_ECHO         0x01u
#define ZW_S2_KEX_CSA          0x02u
#define ZW_S2_KEX_RESERVED     0xFCu
#define ZW_S2_KEX_SCHEME_1     0x02u /* the schemes byte: bit 1 */
#define ZW_S2_KEX_CURVE_25519  0x01u /* the curves byte: bit 0 */

/* §5: KEX Fail types. */
#define ZW_S2_KEX_FAIL_KEY        0x01u /* no key match */
#define ZW_S2_KEX_FAIL_SCHEME     0x02u
#define ZW_S2_KEX_FAIL_CURVES     0x03u
#define ZW_S2_KEX_FAIL_DECRYPT    0x05u
#define ZW_S2_KEX_FAIL_CANCEL     0x06u
#define ZW_S2_KEX_FAIL_AUTH       0x07u /* wrong security level */
#define ZW_S2_KEX_FAIL_KEY_GET    0x08u /* a key that was not granted */
#define ZW_S2_KEX_FAIL_KEY_VERIFY 0x09u
#define ZW_S2_KEX_FAIL_KEY_REPORT 0x0Au /* a different key */

#define ZW_S2_PUBKEY_INCLUDING 0x01u /* Public Key Report: sent by the including node */
#define ZW_S2_PUBKEY_LEN       32u
#define ZW_S2_NETKEY_LEN       16u

#define ZW_S2_TE_COMPLETE 0x01u /* Transfer End: key request complete */
#define ZW_S2_TE_VERIFIED 0x02u /* Transfer End: key verified */

#define ZW_S2_NONCE_SOS 0x01u /* Nonce Report: singlecast out of sync, receiver EI follows */
#define ZW_S2_NONCE_MOS 0x02u /* Nonce Report: multicast out of sync */
#define ZW_S2_EI_BYTES  16u

/* §7: Message Encapsulation flags and extensions. */
#define ZW_S2_ENCAP_EXT      0x01u /* unencrypted extensions follow */
#define ZW_S2_ENCAP_EXT_ENC  0x02u /* encrypted extensions inside */
#define ZW_S2_EXT_MORE       0x80u
#define ZW_S2_EXT_CRITICAL   0x40u
#define ZW_S2_EXT_TYPE       0x3Fu
#define ZW_S2_EXT_SPAN       0x01u
#define ZW_S2_EXT_MPAN       0x02u
#define ZW_S2_EXT_MGRP       0x03u
#define ZW_S2_EXT_MOS        0x04u
#define ZW_S2_EXT_SPAN_LEN   18u
#define ZW_S2_EXT_MPAN_LEN   19u
#define ZW_S2_EXT_MGRP_LEN   3u
#define ZW_S2_EXT_MOS_LEN    2u
#define ZW_S2_TAG_BYTES      8u
#define ZW_S2_ENCAP_HEADER   4u  /* 9F 03 seq flags */
/* The associated data (AAD): 2 node IDs of at most 2 bytes, home ID, length, seq, flags and the
 * unencrypted extensions - bounded by the frame they come from. */
#define ZW_S2_AAD_FIXED      10u /* 2 x 2 node ID bytes, 4 home ID, 2 length */

/* ---- The key exchange (§5) ----------------------------------------------------------------- */

size_t zw_s2_kex_get_build(uint8_t *out, size_t cap);
bool zw_s2_kex_get_parse(const uint8_t *p, size_t len);

/* KEX Report and KEX Set share one layout: flags (raw, reserved bits kept), schemes, curves,
 * keys (requested in a Report, granted in a Set). */
typedef struct {
    uint8_t flags;
    uint8_t schemes;
    uint8_t curves;
    uint8_t keys;
} zw_s2_kex_t;
size_t zw_s2_kex_report_build(uint8_t *out, size_t cap, const zw_s2_kex_t *in);
bool zw_s2_kex_report_parse(const uint8_t *p, size_t len, zw_s2_kex_t *out);
size_t zw_s2_kex_set_build(uint8_t *out, size_t cap, const zw_s2_kex_t *in);
bool zw_s2_kex_set_parse(const uint8_t *p, size_t len, zw_s2_kex_t *out);

size_t zw_s2_kex_fail_build(uint8_t *out, size_t cap, uint8_t type);
bool zw_s2_kex_fail_parse(const uint8_t *p, size_t len, uint8_t *type);

/* Public Key Report: `flags | key (32)`; flags bit 0 = sent by the including node. */
typedef struct {
    bool including;
    const uint8_t *key; /* ZW_S2_PUBKEY_LEN bytes */
} zw_s2_pubkey_t;
size_t zw_s2_pubkey_build(uint8_t *out, size_t cap, const zw_s2_pubkey_t *in);
bool zw_s2_pubkey_parse(const uint8_t *p, size_t len, zw_s2_pubkey_t *out);

size_t zw_s2_netkey_get_build(uint8_t *out, size_t cap, uint8_t key);
bool zw_s2_netkey_get_parse(const uint8_t *p, size_t len, uint8_t *key);

/* Network Key Report: `key (one bit) | network key (16)`. */
typedef struct {
    uint8_t key;
    const uint8_t *netkey; /* ZW_S2_NETKEY_LEN bytes */
} zw_s2_netkey_report_t;
size_t zw_s2_netkey_report_build(uint8_t *out, size_t cap, const zw_s2_netkey_report_t *in);
bool zw_s2_netkey_report_parse(const uint8_t *p, size_t len, zw_s2_netkey_report_t *out);

size_t zw_s2_netkey_verify_build(uint8_t *out, size_t cap);
bool zw_s2_netkey_verify_parse(const uint8_t *p, size_t len);

size_t zw_s2_transfer_end_build(uint8_t *out, size_t cap, uint8_t flags);
bool zw_s2_transfer_end_parse(const uint8_t *p, size_t len, uint8_t *flags);

/* ---- Nonces (§6) --------------------------------------------------------------------------- */

size_t zw_s2_nonce_get_build(uint8_t *out, size_t cap, uint8_t seq);
bool zw_s2_nonce_get_parse(const uint8_t *p, size_t len, uint8_t *seq);

/* Nonce Report: `seq | flags | receiver EI (16, only with SOS)`. */
typedef struct {
    uint8_t seq;
    uint8_t flags;
    const uint8_t *receiver_ei; /* with SOS; NULL otherwise */
} zw_s2_nonce_report_t;
size_t zw_s2_nonce_report_build(uint8_t *out, size_t cap, const zw_s2_nonce_report_t *in);
bool zw_s2_nonce_report_parse(const uint8_t *p, size_t len, zw_s2_nonce_report_t *out);

/* ---- Message Encapsulation (§7) ------------------------------------------------------------ */

/* What the extensions of one place (unencrypted, or encrypted inside the plaintext) carry. */
typedef struct {
    bool span;
    const uint8_t *sender_ei; /* SPAN: 16 bytes */
    bool mgrp;
    uint8_t group;
    bool mos;
    bool mpan;                /* recognised and ignored: the panel decodes no multicast */
} zw_s2_ext_t;

/* Walks the extensions at p (the first one) of the unencrypted place (`encrypted` false) or the
 * encrypted one, §7's rules: a known extension in the wrong place, or with the wrong length, is
 * skipped; an unknown one is skipped unless critical. *used = the bytes they take. False - drop
 * the frame - on an unknown critical extension, a length below 2 or past `len`. */
bool zw_s2_ext_parse(const uint8_t *p, size_t len, bool encrypted, zw_s2_ext_t *out, size_t *used);

/* A received Message Encapsulation, split: seq, flags, the unencrypted extension bytes (they are
 * part of the AAD), the ciphertext and the tag. False: drop it (too short, or the extensions
 * say so). */
typedef struct {
    uint8_t seq;
    uint8_t flags;
    const uint8_t *ext;     /* the unencrypted extensions, raw */
    size_t ext_len;
    zw_s2_ext_t found;      /* what they carry */
    const uint8_t *ciphertext;
    size_t ciphertext_len;  /* may be 0: a frame of extensions only */
    const uint8_t *tag;     /* ZW_S2_TAG_BYTES */
} zw_s2_encap_t;
bool zw_s2_encap_parse(const uint8_t *p, size_t len, zw_s2_encap_t *out);

/* The header of a Message Encapsulation to send: `9F 03 seq flags`, then a SPAN extension with
 * sender_ei when it is not NULL, then (in the plaintext, not here) the encrypted part. Returns
 * the header's length (4 or 22), 0 when cap is too small. */
size_t zw_s2_encap_header_build(uint8_t *out, size_t cap, uint8_t seq, const uint8_t *sender_ei);

/* §7 associated data: `sender | destination` (1 byte each, 2 when either is 256 or more) |
 * `home ID (4)` | `message length (2)` | the frame's bytes from seq to the end of the
 * unencrypted extensions (`hdr`, `hdr_len`: frame + 2). Returns the AAD's length, 0 when cap is
 * too small. */
size_t zw_s2_aad_build(uint8_t *out, size_t cap, uint16_t sender, uint16_t destination,
                       uint32_t home_id, uint16_t message_len, const uint8_t *hdr,
                       size_t hdr_len);

/* ---- Commands Supported (§9) --------------------------------------------------------------- */

size_t zw_s2_commands_supported_get_build(uint8_t *out, size_t cap);
bool zw_s2_commands_supported_get_parse(const uint8_t *p, size_t len);
/* Report: the CC list (may be empty). */
size_t zw_s2_commands_supported_report_build(uint8_t *out, size_t cap, const uint8_t *ccs,
                                             size_t n);
bool zw_s2_commands_supported_report_parse(const uint8_t *p, size_t len, const uint8_t **ccs,
                                           size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* ZW_S2_CC_H */
