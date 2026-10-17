/*
 * fw_image.c - an update image before it is written, and a master's own image for its keypads
 * (see proto_fw.h, docs/features/11 "Updates through the link", docs/features/13 OTA-014).
 *
 * Read byte by byte, little-endian, so the host reads what the device writes: the layouts are
 * ESP-IDF's esp_image_header_t (24 bytes), esp_image_segment_header_t (8) and esp_app_desc_t (256),
 * which panel_ota.c checks against these offsets at build time.
 */
#include <string.h>

#include "alarm_proto/ota_profile.h"
#include "alarm_proto/proto_fw.h"
#include "alarm_proto/proto_fwsrv.h"

#define IMG_MAGIC       0xE9u
#define APP_MAGIC       0xABCD5432u
#define SIG_MAGIC       0xE7u /* the first byte of a Secure Boot v2 signature block */
#define IMG_HDR_LEN     24u
#define SEG_HDR_LEN     8u
#define SEGMENTS_MAX    16u
#define SECTOR          4096u
/* offsets in the image header */
#define OFF_SEGMENTS    1u
#define OFF_CHIP_ID     12u
#define OFF_MIN_REV     15u
#define OFF_MAX_REV     17u
#define OFF_HASH        23u
/* offsets of the application description, which follows the first segment's header */
#define OFF_APP         (IMG_HDR_LEN + SEG_HDR_LEN)
#define OFF_APP_VERSION (OFF_APP + 16u)
#define OFF_APP_PROJECT (OFF_APP + 48u)
#define APP_FIELD_LEN   32u

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8u));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

/* A fixed-size, maybe unterminated text field. */
static void field(char *out, size_t cap, const uint8_t *in, size_t len)
{
    size_t n = 0;
    while (n < len && n + 1u < cap && in[n] != 0u) {
        out[n] = (char)in[n];
        n++;
    }
    out[n] = '\0';
}

bool proto_fw_cap_allows(proto_fw_cap_t cap, const char *cap_version, const char *version)
{
    if (cap == PROTO_FW_CAP_NONE) {
        return true;
    }
    if (cap != PROTO_FW_CAP_AT) {
        return false; /* not heard yet, or a cap this firmware doesn't know: nothing passes */
    }
    proto_version_t v;
    proto_version_t c;
    return proto_version_parse(version, &v) && proto_version_parse(cap_version, &c) &&
           proto_version_compare(&v, &c) <= 0;
}

bool proto_fw_chip_rev_fits(uint16_t rev_full, uint16_t min_full, uint16_t max_full)
{
    if (rev_full < min_full) {
        return false;
    }
    return max_full == 0u || max_full == 0xFFFFu || rev_full <= max_full;
}

bool proto_fw_header_read(const uint8_t *buf, size_t len, proto_fw_header_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (buf == NULL || len < PROTO_FW_HEADER_LEN) {
        return false;
    }
    out->image_magic = buf[0] == IMG_MAGIC;
    out->segments = buf[OFF_SEGMENTS];
    out->chip_id = le16(buf + OFF_CHIP_ID);
    out->min_rev_full = le16(buf + OFF_MIN_REV);
    out->max_rev_full = le16(buf + OFF_MAX_REV);
    out->hash_appended = buf[OFF_HASH] == 1u;
    out->app_magic = le32(buf + OFF_APP) == APP_MAGIC;
    field(out->version, sizeof(out->version), buf + OFF_APP_VERSION, APP_FIELD_LEN);
    field(out->project, sizeof(out->project), buf + OFF_APP_PROJECT, APP_FIELD_LEN);
    return true;
}

proto_fw_hdr_verdict_t proto_fw_header_judge(const proto_fw_header_t *h, const proto_fw_self_t *me)
{
    if (h == NULL || me == NULL || !h->image_magic || !h->app_magic) {
        return PROTO_FW_HDR_NOT_AN_IMAGE;
    }
    if (h->chip_id != me->chip_id) {
        return PROTO_FW_HDR_OTHER_CHIP;
    }
    if (!proto_fw_chip_rev_fits(me->rev_full, h->min_rev_full, h->max_rev_full)) {
        return PROTO_FW_HDR_OTHER_REVISION;
    }
    if (!ota_profile_accepts(me->project != NULL ? me->project : "", h->project)) {
        return PROTO_FW_HDR_OTHER_PROJECT;
    }
    if (!proto_version_is_upgrade(me->version, h->version, me->allow_older)) { /* OTA-014 */
        return PROTO_FW_HDR_NOT_NEWER;
    }
    if (!proto_fw_cap_allows(me->cap, me->cap_version, h->version)) { /* LINK-029 */
        return PROTO_FW_HDR_PAST_CAP;
    }
    return PROTO_FW_HDR_OK;
}

proto_fw_why_t proto_fw_header_why(proto_fw_hdr_verdict_t v)
{
    switch (v) {
    case PROTO_FW_HDR_OK:
        return PROTO_FW_WHY_OK;
    case PROTO_FW_HDR_OTHER_CHIP:
    case PROTO_FW_HDR_OTHER_REVISION:
        return PROTO_FW_WHY_CHIP;
    case PROTO_FW_HDR_OTHER_PROJECT:
        return PROTO_FW_WHY_PROJECT;
    case PROTO_FW_HDR_NOT_NEWER:
        return PROTO_FW_WHY_NOT_NEWER;
    case PROTO_FW_HDR_PAST_CAP:
        return PROTO_FW_WHY_WRONG_VERSION;
    case PROTO_FW_HDR_NOT_AN_IMAGE:
    default:
        return PROTO_FW_WHY_SIGNATURE; /* not an image ESP-IDF's check would take */
    }
}

const char *proto_fw_why_name(unsigned why)
{
    static const char *const t[] = {
        [PROTO_FWSRV_WHY_NO_NETWORK] = "no_network",
        [PROTO_FWSRV_WHY_NO_LOOKUP] = "no_lookup",
        [PROTO_FWSRV_WHY_NO_CONNECT] = "no_connect",
        [PROTO_FWSRV_WHY_SERVER_ERROR] = "server_error",
        [PROTO_FWSRV_WHY_NOT_A_SERVER] = "not_a_server",
        [PROTO_FWSRV_WHY_UNKNOWN_DEVICE] = "unknown_device",
        [PROTO_FWSRV_WHY_HELD_BACK] = "held_back",
        [PROTO_FWSRV_WHY_STEPPING_STONE] = "stepping_stone",
        [PROTO_FWSRV_WHY_OTHER_VERSION] = "other_version",
        [PROTO_FWSRV_WHY_UP_TO_DATE] = "up_to_date",
        [PROTO_FWSRV_WHY_UNKNOWN_VERSION] = "unknown_version",
        [PROTO_FWSRV_WHY_NOT_OFFERED] = "not_offered",
        [PROTO_FWSRV_WHY_REFUSED] = "refused",
        [PROTO_FW_WHY_BROKE_OFF] = "broke_off",
        [PROTO_FW_WHY_TOO_SLOW] = "too_slow",
        [PROTO_FW_WHY_BAD_SIZE] = "bad_size",
        [PROTO_FW_WHY_BAD_SHA] = "bad_sha",
        [PROTO_FW_WHY_STALLED] = "stalled",
        [PROTO_FW_WHY_TIMEOUT] = "timeout",
        [PROTO_FW_WHY_NO_MEMORY] = "no_memory",
        [PROTO_FW_WHY_FLASH] = "flash",
        [PROTO_FW_WHY_KEY] = "key",
        [PROTO_FW_WHY_CHIP] = "chip",
        [PROTO_FW_WHY_PROJECT] = "project",
        [PROTO_FW_WHY_NOT_NEWER] = "not_newer",
        [PROTO_FW_WHY_SIGNATURE] = "signature",
        [PROTO_FW_WHY_TOO_BIG] = "too_big",
        [PROTO_FW_WHY_UNKNOWN_KIND] = "unknown_kind",
        [PROTO_FW_WHY_UNKNOWN_OP] = "unknown_op",
        [PROTO_FW_WHY_BUDGET] = "budget",
        [PROTO_FW_WHY_BAD_ORDER] = "bad_order",
        [PROTO_FW_WHY_WRONG_VERSION] = "wrong_version",
        [PROTO_FW_WHY_MASTER_BUSY] = "master_busy",
        [PROTO_FW_WHY_NOT_QUIET] = "not_quiet",
        [PROTO_FW_WHY_PANIC_PENDING] = "panic_pending",
        [PROTO_FW_WHY_KEPT_TONE] = "kept_tone",
        [PROTO_FW_WHY_NO_STATUS] = "no_status",
        [PROTO_FW_WHY_BUSY] = "busy",
        [PROTO_FW_WHY_STOPPED] = "stopped",
        [PROTO_FW_WHY_CANCELLED] = "cancelled",
    };
    if (why < sizeof(t) / sizeof(t[0]) && t[why] != NULL) {
        return t[why];
    }
    return "unknown";
}

uint32_t proto_fw_image_size(proto_fw_read_fn read, void *ctx, uint32_t limit, uint16_t chip_id)
{
    uint8_t h[IMG_HDR_LEN];
    uint8_t app[4];
    if (read == NULL || limit < PROTO_FW_HEADER_LEN || !read(ctx, 0u, h, sizeof(h)) ||
        !read(ctx, OFF_APP, app, sizeof(app))) {
        return 0u;
    }
    const unsigned segments = h[OFF_SEGMENTS];
    if (h[0] != IMG_MAGIC || le16(h + OFF_CHIP_ID) != chip_id || le32(app) != APP_MAGIC ||
        segments == 0u || segments > SEGMENTS_MAX) {
        return 0u;
    }
    uint64_t off = IMG_HDR_LEN;
    for (unsigned i = 0; i < segments; i++) {
        uint8_t sh[SEG_HDR_LEN];
        if (off + SEG_HDR_LEN > limit || !read(ctx, (uint32_t)off, sh, sizeof(sh))) {
            return 0u;
        }
        off += SEG_HDR_LEN + (uint64_t)le32(sh + 4u);
        if (off > limit) {
            return 0u;
        }
    }
    off = (off + 1u + 15u) & ~(uint64_t)15u; /* the checksum byte, padded to 16 */
    if (h[OFF_HASH] == 1u) {
        off += 32u;
    }
    if (off > limit) {
        return 0u;
    }
    const uint64_t sig = (off + SECTOR - 1u) & ~(uint64_t)(SECTOR - 1u);
    uint8_t magic = 0;
    if (sig + SECTOR <= limit && read(ctx, (uint32_t)sig, &magic, 1u) && magic == SIG_MAGIC) {
        off = sig + SECTOR;
    }
    return (uint32_t)off;
}
