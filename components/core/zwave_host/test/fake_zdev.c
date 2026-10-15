/*
 * fake_zdev.c - a simulated Z-Wave device with a joining-side S2 (security.md §5 "The joining
 * side"). See fake_zdev.h.
 */
#include "fake_zdev.h"

#include <string.h>

#include "fake_zac93.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_cc_switch.h"
#include "zwave_proto/zw_dsk.h"

#define ECHO_EVERY_MS 10000u /* §5: the KEX Set echo again about every 10 s until answered */

/* Its own sends. */
#define TAG_KEX    0x5A440001u
#define TAG_CLOSE  0x5A440002u /* the final Transfer End: then it forgets the temporary key */
#define TAG_REPORT 0x5A440003u
#define TAG_LEAVE  0x5A440004u /* Device Reset Locally: then it is out of the network */

static zw_sec_class_t class_of(uint8_t key)
{
    switch (key) {
    case ZW_S2_KEY_UNAUTH: return ZW_SEC_S2_UNAUTH;
    case ZW_S2_KEY_AUTH: return ZW_SEC_S2_AUTH;
    case ZW_S2_KEY_ACCESS: return ZW_SEC_S2_ACCESS;
    default: return ZW_SEC_NONE;
    }
}

static int key_index(uint8_t key)
{
    switch (key) {
    case ZW_S2_KEY_UNAUTH: return 0;
    case ZW_S2_KEY_AUTH: return 1;
    case ZW_S2_KEY_ACCESS: return 2;
    default: return -1;
    }
}

static void dsend(fake_zdev_t *d, zw_sec_class_t cls, const uint8_t *cmd, size_t len, uint32_t tag)
{
    if (len > 0u) {
        (void)zw_s2_send(&d->s2, d->now, FAKE_ZDEV_PANEL, cls, cmd, len, tag);
    }
}

static void failed(fake_zdev_t *d)
{
    d->status = FAKE_ZDEV_FAILED;
    d->step = FAKE_KEX_NONE;
    zw_s2_clear_temp_key(&d->s2);
    if (d->nif_fallback) {
        /* the protocol's fallback: it works without security now, every CC it has in the clear -
         * its secure CCs join its node information */
        for (size_t i = 0; i < d->m.secure_n; i++) {
            if (memchr(d->nif, d->m.secure_ccs[i], d->nif_len) == NULL &&
                d->nif_len < sizeof d->nif) {
                d->nif[d->nif_len++] = d->m.secure_ccs[i];
            }
        }
        d->have = 0u;
        (void)zw_s2_set_granted(&d->s2, FAKE_ZDEV_PANEL, 0u);
    }
}

/* A fault at this step: silence (nothing sent, it waits for ever) or a KEX Fail instead. */
static bool fault_at(fake_zdev_t *d, uint8_t step)
{
    if (d->silent_at == step) {
        d->step = FAKE_KEX_NONE;
        return true;
    }
    if (d->fail_at == step) {
        uint8_t f[3];
        dsend(d, ZW_SEC_NONE, f, zw_s2_kex_fail_build(f, sizeof f, d->fail_type), TAG_KEX);
        failed(d);
        return true;
    }
    return false;
}

/* ---- Its side of the key exchange (§5) ------------------------------------------------------ */

static void send_echo(fake_zdev_t *d)
{
    if (fault_at(d, FAKE_KEX_ECHO)) {
        return;
    }
    zw_s2_kex_t e = d->set;
    e.flags = (uint8_t)(e.flags | ZW_S2_KEX_ECHO | d->echo_flags_or);
    uint8_t f[6];
    d->damage_next = d->damage_first_echo && d->echoes == 0u;
    dsend(d, ZW_SEC_TEMP, f, zw_s2_kex_set_build(f, sizeof f, &e), TAG_KEX);
    d->echoes++;
    d->step = FAKE_KEX_ECHO;
    d->echo_due = d->now + ECHO_EVERY_MS;
}

/* The next granted key it does not have (lowest first), or the final Transfer End. */
static void ask_next_key(fake_zdev_t *d)
{
    const uint8_t missing = (uint8_t)(d->set.keys & ZW_S2_KEYS_S2 & (uint8_t)~d->have);
    const uint8_t next = (uint8_t)(missing & (uint8_t)(0u - missing)); /* its lowest bit */
    uint8_t f[3];
    if (next == 0u) {
        if (!fault_at(d, FAKE_KEX_TRANSFER_END)) {
            const uint8_t flags = d->te_flags_set ? d->te_flags : ZW_S2_TE_COMPLETE;
            dsend(d, ZW_SEC_TEMP, f, zw_s2_transfer_end_build(f, sizeof f, flags), TAG_CLOSE);
            d->step = FAKE_KEX_NONE;
            /* its temporary key stays for the Transfer End's resend window; the panel's frames
             * meanwhile come under its highest key (a real device has dropped the temporary key
             * already, §5 step 6) */
            zw_s2_set_exchange_class(&d->s2, (d->have & ZW_S2_KEY_ACCESS) != 0u ? ZW_SEC_S2_ACCESS
                                             : (d->have & ZW_S2_KEY_AUTH) != 0u ? ZW_SEC_S2_AUTH
                                                                               : ZW_SEC_S2_UNAUTH);
        }
        return;
    }
    if (!fault_at(d, FAKE_KEX_NETKEY_GET)) {
        const uint8_t ask = d->ask_key != 0u ? d->ask_key : next;
        d->ask_key = 0u;
        dsend(d, d->netkey_get_plain ? ZW_SEC_NONE : ZW_SEC_TEMP, f,
              zw_s2_netkey_get_build(f, sizeof f, ask), TAG_KEX);
        d->step = FAKE_KEX_VERIFY;
    }
}

static void on_kex_get(fake_zdev_t *d)
{
    if (d->status != FAKE_ZDEV_JOINED) {
        return;
    }
    d->status = FAKE_ZDEV_EXCHANGING;
    d->have = 0u;
    if (fault_at(d, FAKE_KEX_REPORT)) {
        return;
    }
    d->sent_report = d->kex;
    zw_s2_kex_t out = d->kex;
    if (d->mitm_report_keys != 0u) {
        out.keys = d->mitm_report_keys; /* changed on its way to the panel */
    }
    uint8_t f[6];
    dsend(d, ZW_SEC_NONE, f, zw_s2_kex_report_build(f, sizeof f, &out), TAG_KEX);
    d->step = FAKE_KEX_PUBKEY;
}

static void on_kex_set(fake_zdev_t *d, const zw_s2_rx_t *rx)
{
    zw_s2_kex_t k;
    if (d->step != FAKE_KEX_PUBKEY || rx->security != ZW_SEC_NONE ||
        !zw_s2_kex_set_parse(rx->command, rx->len, &k) || (k.flags & ZW_S2_KEX_ECHO) != 0u) {
        return;
    }
    if (d->mitm_set_keys != 0u) {
        k.keys = d->mitm_set_keys; /* changed on its way from the panel */
    }
    d->set = k;
    if (fault_at(d, FAKE_KEX_PUBKEY)) {
        return;
    }
    uint8_t key[32];
    memcpy(key, d->pub, sizeof key);
    if ((k.keys & (ZW_S2_KEY_AUTH | ZW_S2_KEY_ACCESS)) != 0u) {
        key[0] = key[1] = 0u; /* §4: the PIN is typed at the panel */
    }
    const zw_s2_pubkey_t pk = {false, key};
    uint8_t f[3u + ZW_S2_PUBKEY_LEN];
    dsend(d, ZW_SEC_NONE, f, zw_s2_pubkey_build(f, sizeof f, &pk), TAG_KEX);
    d->step = FAKE_KEX_ECHO;
}

/* The panel's public key: the temporary key (the panel's public key first, on both sides). */
static void on_panel_key(fake_zdev_t *d, const zw_s2_rx_t *rx)
{
    zw_s2_pubkey_t pk;
    if (d->step != FAKE_KEX_ECHO || rx->security != ZW_SEC_NONE ||
        !zw_s2_pubkey_parse(rx->command, rx->len, &pk) || !pk.including) {
        return;
    }
    uint8_t shared[32], prk[16], key_ccm[16], pstring[32];
    const bool ok = zw_s2_shared_secret(&d->crypto, d->priv, pk.key, shared) &&
                    zw_s2_temp_key_extract(&d->crypto, shared, pk.key, d->pub, prk) &&
                    zw_s2_temp_key_expand(&d->crypto, prk, key_ccm, pstring) &&
                    zw_s2_set_temp_key(&d->s2, FAKE_ZDEV_PANEL, key_ccm, pstring);
    if (!ok) {
        failed(d);
        return;
    }
    send_echo(d);
}

static void on_report_echo(fake_zdev_t *d, const zw_s2_rx_t *rx)
{
    zw_s2_kex_t r;
    if (d->step != FAKE_KEX_ECHO || rx->security != ZW_SEC_TEMP ||
        !zw_s2_kex_report_parse(rx->command, rx->len, &r)) {
        return;
    }
    const zw_s2_kex_t *s = &d->sent_report;
    if (r.flags != (uint8_t)(s->flags | ZW_S2_KEX_ECHO) || r.schemes != s->schemes ||
        r.curves != s->curves || r.keys != s->keys) {
        /* not the report it sent: someone changed it on the way */
        uint8_t f[3];
        dsend(d, ZW_SEC_NONE, f, zw_s2_kex_fail_build(f, sizeof f, ZW_S2_KEX_FAIL_AUTH), TAG_KEX);
        failed(d);
        return;
    }
    ask_next_key(d);
}

static void on_netkey_report(fake_zdev_t *d, const zw_s2_rx_t *rx)
{
    zw_s2_netkey_report_t nk;
    const int i = zw_s2_netkey_report_parse(rx->command, rx->len, &nk) ? key_index(nk.key) : -1;
    if (d->step != FAKE_KEX_VERIFY || rx->security != ZW_SEC_TEMP || i < 0) {
        return;
    }
    memcpy(d->netkeys[i], nk.netkey, 16u);
    d->have = (uint8_t)(d->have | nk.key);
    if (!zw_s2_set_key(&d->s2, nk.key, nk.netkey) ||
        !zw_s2_set_granted(&d->s2, FAKE_ZDEV_PANEL, d->have)) {
        failed(d);
        return;
    }
    if (fault_at(d, FAKE_KEX_VERIFY)) {
        return;
    }
    uint8_t f[2];
    dsend(d, d->verify_with_temp ? ZW_SEC_TEMP : class_of(nk.key), f,
          zw_s2_netkey_verify_build(f, sizeof f), TAG_KEX);
    d->step = FAKE_KEX_NETKEY_GET; /* the panel's Transfer End next */
}

static void on_transfer_end(fake_zdev_t *d, const zw_s2_rx_t *rx)
{
    uint8_t flags = 0u;
    if (d->step != FAKE_KEX_NETKEY_GET || rx->security != ZW_SEC_TEMP ||
        !zw_s2_transfer_end_parse(rx->command, rx->len, &flags) ||
        (flags & ZW_S2_TE_VERIFIED) == 0u) {
        return;
    }
    ask_next_key(d);
}

/* The key exchange's commands; false for anything else. */
static bool kex_command(fake_zdev_t *d, const zw_s2_rx_t *rx)
{
    switch (rx->command[1]) {
    case ZW_S2_KEX_GET:
        on_kex_get(d);
        return true;
    case ZW_S2_KEX_SET:
        on_kex_set(d, rx);
        return true;
    case ZW_S2_PUBLIC_KEY_REPORT:
        on_panel_key(d, rx);
        return true;
    case ZW_S2_KEX_REPORT:
        on_report_echo(d, rx);
        return true;
    case ZW_S2_NETWORK_KEY_REPORT:
        on_netkey_report(d, rx);
        return true;
    case ZW_S2_TRANSFER_END:
        on_transfer_end(d, rx);
        return true;
    case ZW_S2_KEX_FAIL:
        (void)zw_s2_kex_fail_parse(rx->command, rx->len, &d->fail_got);
        if (d->status == FAKE_ZDEV_EXCHANGING) {
            failed(d);
        }
        return true;
    default:
        return false;
    }
}

/* ---- Its S2's port -------------------------------------------------------------------------- */

static bool d_send(void *ctx, uint16_t node, const uint8_t *f, size_t len, uint8_t opt,
                   uint32_t tag)
{
    fake_zdev_t *d = ctx;
    (void)node;
    (void)opt;
    bool ok = true;
    d->sent_frames++;
    uint8_t damaged[ZW_S2_FRAME_MAX];
    if (d->damage_next && len >= 12u && len <= sizeof damaged && f[0] == ZW_S2_CC &&
        f[1] == ZW_S2_MESSAGE_ENCAPSULATION) {
        d->damage_next = false; /* the encapsulation, not its Nonce Get */
        memcpy(damaged, f, len);
        damaged[len - 1u] ^= 0x5Au;
        f = damaged;
    }
    if (d->lose_out > 0u) {
        d->lose_out--;
        ok = false; /* the module never heard it: no ACK */
    } else {
        fake_zac93_from_device(d->zac, d->now, d, f, len);
        if (d->repeat_out > 0u) {
            d->repeat_out--;
            fake_zac93_from_device(d->zac, d->now, d, f, len); /* it missed the ACK: again */
        }
    }
    if (d->acks_n < 8u) {
        d->acks[d->acks_n] = tag;
        d->ack_ok[d->acks_n] = ok;
        d->acks_n++;
    }
    return true;
}

static void d_received(void *ctx, const zw_s2_rx_t *rx)
{
    fake_zdev_t *d = ctx;
    if (rx->len >= 2u && rx->command[0] == ZW_S2_CC && !rx->multi_channel && !rx->supervised &&
        kex_command(d, rx)) {
        return;
    }
    if (rx->multi_channel) {
        if (rx->security == ZW_SEC_NONE) {
            d->mc_clear++;
        } else {
            d->mc_secure++;
        }
    }
    zw_cc_supervision_report_t sr;
    if (zw_cc_supervision_report_parse(rx->command, rx->len, &sr)) {
        d->spv_reports++;
        d->spv_session = sr.session_id;
        d->spv_status = sr.status;
        d->spv_sec = rx->security;
        d->spv_at = d->now;
    }
    (void)fake_zdev_answer(d, rx);
    if (d->got_n < FAKE_ZDEV_GOT && rx->len <= FAKE_ZDEV_CMD) {
        memcpy(d->got[d->got_n], rx->command, rx->len);
        d->got_len[d->got_n] = rx->len;
        d->got_sec[d->got_n] = rx->security;
        d->got_n++;
    }
}

static void d_send_done(void *ctx, uint32_t tag, zw_s2_result_t result)
{
    fake_zdev_t *d = ctx;
    if (tag == TAG_CLOSE && d->status == FAKE_ZDEV_EXCHANGING) {
        zw_s2_clear_temp_key(&d->s2); /* §5 step 6: then forget the temporary key */
        d->status = result == ZW_S2_OK ? FAKE_ZDEV_SECURE : FAKE_ZDEV_FAILED;
    } else if (tag == TAG_LEAVE) {
        d->status = FAKE_ZDEV_OUT; /* it said so: now it has left */
        d->home_id = 0u;
        d->have = 0u;
    }
}

/* A door sensor as the Zooz ZSE41 describes itself: sleeping, S2, Notification (access control,
 * home security), battery, a lifeline in group 1 (Z-Wave Plus, AGI). */
static void door_model(fake_model_t *m)
{
    static const fake_cc_version_t VERSIONS[] = {
        {0x5E, 2}, {0x55, 2}, {0x9F, 1}, {0x6C, 1}, {0x86, 3}, {0x72, 2}, {0x5A, 1},
        {0x85, 2}, {0x8E, 3}, {0x59, 3}, {0x71, 8}, {0x80, 1}, {0x84, 2}, {0x70, 4},
    };
    static const uint8_t SECURE[] = {0x86, 0x72, 0x5A, 0x85, 0x8E, 0x59,
                                     0x71, 0x80, 0x84, 0x70, 0x6C};
    static const uint8_t AGI_CMDS[] = {0x71, 0x05, 0x80, 0x03, 0x5A, 0x01};
    memset(m, 0, sizeof *m);
    m->manufacturer = 0x027Au;
    m->product_type = 0x7000u;
    m->product_id = 0xE001u;
    memcpy(m->versions, VERSIONS, sizeof VERSIONS);
    m->versions_n = sizeof VERSIONS / sizeof VERSIONS[0];
    memcpy(m->secure_ccs, SECURE, sizeof SECURE);
    m->secure_n = sizeof SECURE;
    m->zwp_role = 0x06u; /* sleeping reporting end node */
    m->icon = 0x0C07u;
    m->wake_interval = 43200u;
    m->wake_min = 600u;
    m->wake_max = 86400u;
    m->wake_default = 43200u;
    m->wake_step = 60u;
    m->awake_ms = 30000u;
    m->awake_extend_ms = 2000u;
    m->auto_wake = true;
    m->groups = 1u;
    m->mca_groups = 1u;
    m->agi_profile = 0x0001u;
    memcpy(m->agi_cmds, AGI_CMDS, sizeof AGI_CMDS);
    m->agi_cmds_n = sizeof AGI_CMDS;
    m->notif_types[0] = 0x06u; /* access control: door open / closed */
    m->notif_types[1] = 0x07u; /* home security: tamper */
    m->notif_n = 2u;
    m->battery = 90u;
    m->fw_major = 1u;
    m->fw_minor = 10u;
}

/* ---- API ------------------------------------------------------------------------------------- */

void fake_zdev_init(fake_zdev_t *d, uint64_t seed)
{
    static const uint8_t DOOR_NIF[] = {0x5E, 0x55, 0x9F, 0x6C};
    memset(d, 0, sizeof *d);
    zw_host_crypto_init(&d->hc, seed);
    d->crypto = zw_host_crypto_port(&d->hc);
    d->basic = 0x04u;    /* routing end node */
    d->generic = 0x07u;  /* notification sensor */
    d->specific = 0x01u;
    memcpy(d->nif, DOOR_NIF, sizeof DOOR_NIF);
    d->nif_len = sizeof DOOR_NIF;
    d->kex.flags = 0u;
    d->kex.schemes = ZW_S2_KEX_SCHEME_1;
    d->kex.curves = ZW_S2_KEX_CURVE_25519;
    d->kex.keys = ZW_S2_KEY_ACCESS | ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH;
    door_model(&d->m);
    d->watch_cc = 0xFFu; /* nothing watched */
    if (zw_s2_keypair(&d->crypto, d->priv, d->pub)) {
        (void)zw_dsk_to_text(d->pub, d->dsk, sizeof d->dsk);
        d->pin = zw_dsk_pin(d->pub);
    }
    const zw_s2_port_t port = {d, d_send, d_received, d_send_done, NULL};
    const zw_s2_config_t cfg = {false, 0u};
    zw_s2_init(&d->s2, &d->crypto, &port, &cfg);
}

void fake_zdev_no_s2(fake_zdev_t *d)
{
    static const uint8_t NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x71, 0x80, 0x84};
    memcpy(d->nif, NIF, sizeof NIF);
    d->nif_len = sizeof NIF;
}

void fake_zdev_joined(fake_zdev_t *d, uint32_t home_id, uint16_t node)
{
    d->home_id = home_id;
    d->node = node;
    d->status = FAKE_ZDEV_JOINED;
    d->step = FAKE_KEX_NONE;
    d->have = 0u;
    d->awake_until = d->now + d->m.awake_ms; /* after joining it stays awake a while */
    d->next_wake = 0u;                         /* no interval to keep before the panel's */
    if (zw_s2_start(&d->s2, home_id, node, NULL)) {
        (void)zw_s2_add_node(&d->s2, FAKE_ZDEV_PANEL, 0u, NULL, 0u);
    }
}

void fake_zdev_hear(fake_zdev_t *d, uint64_t now, const uint8_t *cmd, size_t len)
{
    d->now = now;
    d->heard_n++;
    if (!fake_zdev_awake(d, now)) {
        d->heard_asleep++; /* the module delivered to a device that sleeps: a fake's bug */
    } else if (!d->m.listening && !d->m.flirs &&
               d->awake_until < now + d->m.awake_extend_ms) {
        d->awake_until = now + d->m.awake_extend_ms; /* it stays awake while spoken to */
    }
    if (d->cross_next) {
        /* it was sending too, on the same SPAN position */
        d->cross_next = false;
        (void)fake_zdev_report(d, now, ZW_SEC_AUTO, d->cross, d->cross_len);
    }
    zw_s2_receive(&d->s2, now, FAKE_ZDEV_PANEL, cmd, len);
}

void fake_zdev_poll(fake_zdev_t *d, uint64_t now)
{
    d->now = now;
    while (d->acks_n > 0u) {
        const uint32_t tag = d->acks[0];
        const bool ok = d->ack_ok[0];
        d->acks_n--;
        memmove(d->acks, &d->acks[1], d->acks_n * sizeof d->acks[0]);
        memmove(d->ack_ok, &d->ack_ok[1], d->acks_n * sizeof d->ack_ok[0]);
        zw_s2_sent(&d->s2, now, tag, ok);
    }
    zw_s2_poll(&d->s2, now);
    while (d->later_n > 0u && now >= d->later[0].at) {
        /* an answer it sends later (split_ms): it speaks, so it listens a moment too */
        if (d->awake_until < now + d->m.awake_extend_ms) {
            d->awake_until = now + d->m.awake_extend_ms;
        }
        d->answers++;
        (void)zw_s2_send(&d->s2, now, FAKE_ZDEV_PANEL, d->later[0].cls, d->later[0].cmd,
                         d->later[0].len, TAG_REPORT);
        d->later_n--;
        memmove(&d->later[0], &d->later[1], d->later_n * sizeof d->later[0]);
    }
    if (d->step == FAKE_KEX_ECHO && d->echoes > 0u && d->status == FAKE_ZDEV_EXCHANGING &&
        now >= d->echo_due) {
        send_echo(d);
    }
    if (d->m.auto_wake && d->next_wake != 0u && now >= d->next_wake &&
        d->status != FAKE_ZDEV_OUT) {
        fake_zdev_wake(d, now);
    }
}

uint64_t fake_zdev_next_deadline(const fake_zdev_t *d)
{
    if (d->acks_n > 0u) {
        return d->now;
    }
    uint64_t t = zw_s2_next_deadline(&d->s2);
    if (d->later_n > 0u && d->later[0].at < t) {
        t = d->later[0].at;
    }
    if (d->step == FAKE_KEX_ECHO && d->echoes > 0u && d->status == FAKE_ZDEV_EXCHANGING &&
        d->echo_due < t) {
        t = d->echo_due;
    }
    if (d->m.auto_wake && d->next_wake != 0u && d->status != FAKE_ZDEV_OUT && d->next_wake < t) {
        t = d->next_wake;
    }
    return t;
}

bool fake_zdev_awake(const fake_zdev_t *d, uint64_t now)
{
    return !d->silent && (!d->m.sleeps || d->m.listening || d->m.flirs || now < d->awake_until);
}

void fake_zdev_wake(fake_zdev_t *d, uint64_t now)
{
    d->now = now;
    d->awake_until = now + d->m.awake_ms;
    d->next_wake = d->m.wake_interval != 0u ? now + (uint64_t)d->m.wake_interval * 1000u : 0u;
    if (d->status == FAKE_ZDEV_OUT || d->m.wake_node != FAKE_ZDEV_PANEL) {
        return; /* its notification goes nowhere the panel hears */
    }
    static const uint8_t WUN[2] = {0x84u, 0x07u};
    d->wakeups++;
    (void)zw_s2_send(&d->s2, now, FAKE_ZDEV_PANEL, ZW_SEC_AUTO, WUN, sizeof WUN, TAG_REPORT);
}

void fake_zdev_reset_locally(fake_zdev_t *d, uint64_t now)
{
    static const uint8_t DRL[2] = {0x5Au, 0x01u};
    d->now = now;
    d->m.auto_wake = false;
    d->awake_until = now + d->m.awake_ms; /* long enough for its notification to go */
    (void)zw_s2_send(&d->s2, now, FAKE_ZDEV_PANEL, ZW_SEC_AUTO, DRL, sizeof DRL, TAG_LEAVE);
}

void fake_zdev_listening(fake_zdev_t *d)
{
    fake_model_t *m = &d->m;
    m->listening = true;
    m->auto_wake = false;
    m->wake_interval = 0u;
    m->zwp_role = 0x05u; /* always-on end node */
    size_t k = 0u;
    for (size_t i = 0; i < m->versions_n; i++) {
        if (m->versions[i].cc != 0x84u && m->versions[i].cc != 0x80u) {
            m->versions[k++] = m->versions[i];
        }
    }
    m->versions_n = k;
    k = 0u;
    for (size_t i = 0; i < m->secure_n; i++) {
        if (m->secure_ccs[i] != 0x84u && m->secure_ccs[i] != 0x80u) {
            m->secure_ccs[k++] = m->secure_ccs[i];
        }
    }
    m->secure_n = k;
    k = 0u;
    for (size_t i = 0; i < d->nif_len; i++) {
        if (d->nif[i] != 0x84u && d->nif[i] != 0x80u) {
            d->nif[k++] = d->nif[i];
        }
    }
    d->nif_len = k;
}

void fake_zdev_switch_press(fake_zdev_t *d, uint64_t now, uint8_t ep, uint8_t cc, uint8_t value)
{
    if (ep > FAKE_ZDEV_EPS) {
        return;
    }
    d->m.sw[ep] = value;
    const zw_cc_switch_report_t r = {value, false, 0u, 0u};
    uint8_t rep[6];
    const size_t n = zw_cc_switch_report_build(rep, sizeof rep, cc, &r);
    if (ep == 0u) {
        (void)fake_zdev_report(d, now, ZW_SEC_AUTO, rep, n);
        return;
    }
    uint8_t mc[16];
    const size_t k = zw_cc_wrap_multi_channel(mc, sizeof mc, ep, 0u, false, rep, n);
    (void)fake_zdev_report(d, now, ZW_SEC_AUTO, mc, k);
}

zw_s2_result_t fake_zdev_report(fake_zdev_t *d, uint64_t now, zw_sec_class_t cls,
                                const uint8_t *cmd, size_t len)
{
    d->now = now;
    if (d->awake_until < now + d->m.awake_extend_ms) {
        d->awake_until = now + d->m.awake_extend_ms; /* it listens a moment after it speaks */
    }
    return zw_s2_send(&d->s2, now, FAKE_ZDEV_PANEL, cls, cmd, len, TAG_REPORT);
}

void fake_zdev_restart(fake_zdev_t *d)
{
    static const uint8_t BITS[3] = {ZW_S2_KEY_UNAUTH, ZW_S2_KEY_AUTH, ZW_S2_KEY_ACCESS};
    d->acks_n = 0u;
    if (!zw_s2_start(&d->s2, d->home_id, d->node, NULL)) {
        return;
    }
    for (size_t i = 0; i < 3u; i++) {
        if ((d->have & BITS[i]) != 0u && !zw_s2_set_key(&d->s2, BITS[i], d->netkeys[i])) {
            d->status = FAKE_ZDEV_FAILED;
        }
    }
    (void)zw_s2_add_node(&d->s2, FAKE_ZDEV_PANEL, d->have, NULL, 0u);
}
