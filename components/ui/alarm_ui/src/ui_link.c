/*
 * ui_link.c - the panel link on the touchscreen (docs/features/11, BL-048): a panel is the alarm
 * panel itself (the master, every panel before BL-048) or a keypad for an alarm panel elsewhere in
 * the house, over Wi-Fi.
 *   - On the alarm panel, installer mode, Keypads and devices: the keypads on Wi-Fi, adding one
 *     with the same code compared on both screens (LINK-004, LINK-016), a keypad's page and
 *     removing it with a hold (LINK-014); the numbers keypads hold are left out of the table.
 *   - On a keypad: Home while it connects, without system communication or not paired (LINK-008),
 *     the Emergency view's words (LINK-023), About's keypad rows, "Make it a stand-alone panel"
 *     (a factory reset; without a code once the keypad is in maintenance, LINK-025), and the home
 *     partition the alarm panel gives it.
 * The setup's role and pairing steps are ui_wizard.c's, a keypad's Time and language ui_net.c's,
 * the role gate in Settings ui_settings.c's. Everything is read through ui_backend_t.link_*, at
 * most once a second (UI-027); NULL entries (a firmware without the link), or link_info answering
 * false (not known yet), show nothing of it: every page is as it was before BL-048.
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define POLL_MS      1000u
#define LINK_KEYPADS 8u /* keypad numbers 1-8, shared with Zigbee keypads (LINK-001) */

static struct {
    bool have;           /* link_info answered: a firmware with the link that knows its role */
    ui_link_info_t info; /* what it said last */
    uint32_t polled;     /* lv_tick of the last read */
    bool gen_known;      /* home_gen was seen once */
    uint32_t home_gen;   /* ... and what it was */
    bool standalone_ok;  /* the installer code opened "Make it a stand-alone panel" */
    uint32_t page_sig;   /* the open page's live words, as they were drawn */
} L;

/* The master's keypads, read when a page needs them (PSRAM on a panel that has it). */
static ui_link_keypad_t s_kps[LINK_KEYPADS] UI_SLOW_BSS;
static size_t s_nkps;

/* The add page (PAGE_LINK_ADD) */
static struct {
    bool owner;       /* this page opened pairing: leaving it closes pairing */
    uint8_t refused;  /* why pairing didn't open (ui_link_result_t), UI_LINK_REQ_OK = it did */
    ui_link_pair_t p; /* what the page shows */
} A;

static uint8_t s_kp_device; /* the keypad whose page is open */

/* ---- reading -------------------------------------------------------------------------------- */

static bool read_info(ui_link_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (S.be.link_info == NULL || !S.be.link_info(S.be.ctx, out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->master_name[sizeof(out->master_name) - 1u] = '\0';
    out->master_addr[sizeof(out->master_addr) - 1u] = '\0';
    out->home_name[sizeof(out->home_name) - 1u] = '\0';
    return true;
}

static bool read_pair(ui_link_pair_t *out)
{
    memset(out, 0, sizeof(*out));
    if (S.be.link_pair_status == NULL || !S.be.link_pair_status(S.be.ctx, out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->code[sizeof(out->code) - 1u] = '\0';
    out->peer_name[sizeof(out->peer_name) - 1u] = '\0';
    out->peer_addr[sizeof(out->peer_addr) - 1u] = '\0';
    return true;
}

static size_t read_keypads(void)
{
    s_nkps = 0;
    if (!ui_link_master() || S.be.link_keypads == NULL) {
        return 0;
    }
    size_t n = S.be.link_keypads(S.be.ctx, s_kps, LINK_KEYPADS);
    n = n > LINK_KEYPADS ? LINK_KEYPADS : n;
    for (size_t i = 0; i < n; i++) {
        s_kps[i].name[sizeof(s_kps[i].name) - 1u] = '\0';
        s_kps[i].addr[sizeof(s_kps[i].addr) - 1u] = '\0';
        s_kps[i].fw[sizeof(s_kps[i].fw) - 1u] = '\0';
    }
    s_nkps = n;
    return n;
}

static uint8_t request(uint8_t kind, bool allow, uint8_t device)
{
    if (S.be.link_request == NULL) {
        return UI_LINK_REQ_REFUSED;
    }
    ui_link_req_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.allow = allow;
    req.device = device;
    return S.be.link_request(S.be.ctx, &req);
}

bool ui_link_master(void)
{
    return L.have && L.info.role == UI_LINK_ROLE_MASTER;
}

bool ui_link_keypad(void)
{
    return L.have && L.info.role == UI_LINK_ROLE_KEYPAD;
}

/* ---- its alarm panel's pages (BL-223, LINK-057..LINK-063) ---- */

static uint32_t s_remote_areas; /* the last the alarm panel said: kept while the link is down */

bool ui_link_remote(uint32_t areas)
{
    if (!ui_link_keypad()) {
        return false;
    }
    if (L.info.state == UI_LINK_ONLINE && L.info.remote_areas != 0u) {
        /* (none while a new session's grant is on its way: the pages stay, and so does installer
         * mode; an older alarm panel never says any) */
        s_remote_areas = L.info.remote_areas;
    }
    return areas != 0u && (s_remote_areas & areas) == areas;
}

bool ui_link_remote_down(void)
{
    return ui_link_keypad() && L.info.state != UI_LINK_ONLINE;
}

bool ui_link_remote_installer(void)
{
    return ui_link_keypad() && L.info.state == UI_LINK_ONLINE && L.info.remote_installer;
}

uint8_t ui_link_remote_holder(void)
{
    return L.have ? L.info.remote_holder : UI_LINK_HOLDER_NONE;
}

bool ui_link_maintenance(void)
{
    return ui_link_keypad() && L.info.maintenance;
}

bool ui_link_holds_home(void)
{
    return ui_link_keypad() && L.info.state != UI_LINK_ONLINE;
}

/* It hears its alarm panel but can't read the configuration it sends (a newer one than this
 * firmware reads), so it never shows the state: this update is the way out. */
static bool needs_update(void)
{
    return ui_link_holds_home() && L.info.needs_update;
}

bool ui_link_update_open(void)
{
    return ui_link_maintenance() || needs_update();
}

bool ui_link_settings_waived(void)
{
    return ui_link_holds_home() && ui_link_update_open();
}

bool ui_link_local_alarm(void)
{
    return ui_link_holds_home() && L.info.local_alarm;
}

uint8_t ui_link_paired_home(char *name, size_t len)
{
    ui_link_info_t now;
    name[0] = '\0';
    if (!read_info(&now)) {
        return 0u;
    }
    i18n_utf8_copy(name, len, now.home_name);
    return now.home;
}

const char *ui_link_master_name(void)
{
    if (L.info.master_name[0] != '\0') {
        return L.info.master_name; /* a name the household gave: a value */
    }
    const uint8_t p = S.opt.partition;
    if (S.cfg != NULL && p >= 1u && p <= ALARM_MAX_PARTITIONS &&
        S.cfg->partitions[p - 1u].name[0] != '\0') {
        return S.cfg->partitions[p - 1u]
            .name; /* the system's name, from the alarm panel's settings */
    }
    return NOTR("-");
}

/* A partition's name, or "Partition 2" when nobody named it. */
static const char *partition_name(uint8_t p, char *buf, size_t len)
{
    if (S.cfg != NULL && p >= 1u && p <= ALARM_MAX_PARTITIONS &&
        S.cfg->partitions[p - 1u].name[0] != '\0') {
        i18n_utf8_copy(buf, len, S.cfg->partitions[p - 1u].name);
    } else {
        I18N_FORMAT(buf, len, TR_C("default partition name", "Partition {0}"),
                    (unsigned)(p == 0u ? 1u : p));
    }
    return buf;
}

void ui_link_keypad_title(char *out, size_t len, uint8_t device, const char *master)
{
    /* TR: what this panel is: keypad number {0} of the alarm panel named {1} ("Keypad 2 of Harbor
     * House"); About on a keypad and the setup's last step. At most 60 bytes */
    I18N_FORMAT(out, len, TR("Keypad {0} of {1}"), (unsigned)device, master);
}

/* A keypad's name at the alarm panel: its own, or "Keypad 2" until it is renamed (decided
 * 2026-10-15). */
static const char *keypad_name(const ui_link_keypad_t *k, char *buf, size_t len)
{
    if (k->name[0] != '\0') {
        i18n_utf8_copy(buf, len, k->name);
    } else {
        /* TR: the name of a keypad on Wi-Fi nobody has named; {0} is its number. At most 40 bytes
         */
        I18N_FORMAT(buf, len, TR_C("default keypad name", "Keypad {0}"), (unsigned)k->device);
    }
    return buf;
}

/* "588 460": two groups of three are easier to compare (as the Home Assistant page). */
static void code_text(const char *code, char *out, size_t len)
{
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; code[i] != '\0' && n + 2u < len; i++) {
        if (i == 3u) {
            out[n++] = ' ';
        }
        out[n++] = code[i];
        out[n] = '\0';
    }
}

/* Two facts on one line, " · " between them (the same in every language), cut on a whole
 * character where the line is full. */
static void two_facts(char *out, size_t len, const char *a, const char *b)
{
    i18n_utf8_copy(out, len, a);
    size_t n = strlen(out);
    if (a[0] != '\0' && b[0] != '\0' && n + 1u < len) {
        i18n_utf8_copy(out + n, len - n, " \xC2\xB7 ");
        n = strlen(out);
    }
    if (n + 1u < len) {
        i18n_utf8_copy(out + n, len - n, b);
    }
}

static uint32_t hash_str(uint32_t h, const char *s)
{
    for (; s != NULL && *s != '\0'; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return (h ^ 0xFFu) * 16777619u;
}

static uint32_t hash_int(uint32_t h, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        h = (h ^ ((v >> (8 * i)) & 0xFFu)) * 16777619u;
    }
    return h;
}

/* A request's refusal, said in a toast. */
static const char *refused_words(uint8_t r)
{
    switch (r) {
    /* The words the Zigbee pages say for the same refusals (ui_zigbee.c) */
    case UI_LINK_REQ_NOT_ALLOWED: return TR("Only in installer mode, with everything disarmed");
    case UI_LINK_REQ_FAILED: return TR("Not saved");
    default: return TR("That can't be done now");
    }
}

/* ---- Home on a keypad (LINK-008) ------------------------------------------------------------ */

bool ui_link_home_words(ui_link_home_t *out)
{
    if (!ui_link_holds_home()) {
        return false;
    }
    const char *master = ui_link_master_name();
    out->tone = ui_tone_gray();
    out->glyph = UI_ICON_UNLINK;
    switch (L.info.state) {
    case UI_LINK_CONNECTING:
        out->glyph = UI_ICON_WIFI;
        out->tone = ui_tone_blue();
        /* TR: Home on a keypad, the state card's headline from its start until the alarm panel
         * answers (a second or two). At most 47 bytes */
        i18n_utf8_copy(out->headline, sizeof(out->headline), TR_C("keypad state", "Connecting"));
        /* TR: Home on a keypad while it connects, under the headline; {0} is the alarm panel's
         * name. At most 80 bytes */
        I18N_FORMAT(out->detail, sizeof(out->detail), TR("To {0}"), master);
        /* TR: Home on a keypad while it connects, the foot of the state card. At most 127 bytes */
        i18n_utf8_copy(out->note, sizeof(out->note), TR("Arming waits until it answers"));
        break;
    case UI_LINK_LOST: {
        /* TR: Home on a keypad that lost the alarm panel (LINK-008): the state card's headline,
         * over two lines. At most 47 bytes */
        i18n_utf8_copy(out->headline, sizeof(out->headline), TR("No system communication"));
        /* TR: Home on a keypad without system communication, under the headline; {0} is the alarm
         * panel's name. When it last answered follows on a line of its own. At most 70 bytes */
        I18N_FORMAT(out->detail, sizeof(out->detail), TR("{0} hasn't answered"), master);
        if (L.info.since_epoch_ms > 0) {
            /* a line of its own: a line broken inside "10:41" reads badly (LVGL breaks at ':') */
            char time[32];
            char since[48];
            ui_time_or_day_text(L.info.since_epoch_ms, time, sizeof(time));
            /* TR: Home: since when, the state card's foot when armed and a keypad's line when the
             * alarm panel last answered; {0} is a time of day ("10:41 PM", 8 bytes). At most 15
             * bytes */
            I18N_FORMAT(since, sizeof(since), TR("Since {0}"), time);
            const size_t n = strlen(out->detail);
            if (n + 2u < sizeof(out->detail)) {
                out->detail[n] = '\n';
                i18n_utf8_copy(out->detail + n + 1u, sizeof(out->detail) - n - 1u, since);
            }
        }
        /* TR: Home on a keypad without system communication, the foot of the state card: its
         * state is unknown and it can't arm. At most 127 bytes */
        i18n_utf8_copy(out->note, sizeof(out->note), TR("State unknown · Arming is off"));
        break;
    }
    default:
        /* TR: Home on a keypad with no alarm panel (removed there, or never paired): the state
         * card's headline. At most 47 bytes */
        i18n_utf8_copy(out->headline, sizeof(out->headline), TR("Not paired"));
        if (L.info.removed) {
            /* TR: Home on a keypad the alarm panel removed; {0} is the alarm panel's name. At most
             * 80 bytes */
            I18N_FORMAT(out->detail, sizeof(out->detail), TR("{0} removed this keypad"), master);
        } else {
            /* TR: Home on a keypad that has no alarm panel to show. At most 127 bytes */
            i18n_utf8_copy(out->detail, sizeof(out->detail),
                           TR("It isn't paired with an alarm panel"));
        }
        /* TR: Home on a keypad that isn't paired, the foot of the state card. At most 127 bytes */
        i18n_utf8_copy(out->note, sizeof(out->note), TR("Arming is off"));
        break;
    }
    if (needs_update()) {
        /* TR: Home on a keypad that can't read its alarm panel's settings (they are newer than
         * this keypad's software): the foot of the state card, one line (cut with "..." where it
         * runs out), steady: where to update it (About has Software update). At most 47 bytes */
        i18n_utf8_copy(out->note, sizeof(out->note), TR("Update this keypad: Settings > About"));
    }
    return true;
}

bool ui_link_silence_words(char *title, size_t tlen, char *note, size_t nlen)
{
    if (!ui_link_local_alarm()) {
        return false;
    }
    /* TR: Home on a keypad without system communication that keeps sounding a fire or CO alarm:
     * the large red button that stops it here (the code pad's Silence). At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Silence"));
    /* TR: Home on a keypad without system communication that keeps sounding a fire or CO alarm:
     * under Silence. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("Stops the alarm sound on this keypad"));
    return true;
}

bool ui_link_unpaired(char *title, size_t tlen, char *note, size_t nlen)
{
    if (!ui_link_keypad() || L.info.state != UI_LINK_NOT_PAIRED) {
        return false;
    }
    /* TR: Home on a keypad that isn't paired: the large button that pairs it again. At most 47
     * bytes */
    i18n_utf8_copy(title, tlen, TR("Pair again"));
    /* TR: Home on a keypad that isn't paired, under Pair again; {0} is the alarm panel's name. At
     * most 90 bytes */
    I18N_FORMAT(note, nlen, TR("With {0} or another alarm panel"), ui_link_master_name());
    return true;
}

void ui_link_pair_again(void)
{
    if (ui_link_keypad() && L.info.state == UI_LINK_NOT_PAIRED) {
        ui_wizard_pair_again();
    }
}

/* What a keypad's screen says outside the state card: its role, the Emergency view, the dimmed
 * arming (subject_link_home). */
static int32_t home_value(void)
{
    if (!ui_link_keypad()) {
        return 0;
    }
    switch (L.info.state) {
    case UI_LINK_CONNECTING: return 1;
    case UI_LINK_LOST: return 2;
    case UI_LINK_NOT_PAIRED: return 3;
    default: return 0;
    }
}

static void subjects_follow(void)
{
    const bool keypad = ui_link_keypad();
    ui_set_int(&subject_link_role, keypad ? 1 : 0);
    /* Smart Home on a keypad follows the master's rule (ui_devices.c): its alarm panel's devices
     * come through the link (BL-216) */
    ui_set_int(&subject_link_home, home_value());
    if (keypad) {
        char hint[128];
        /* TR: the Emergency view on a keypad, under its title; {0} is the alarm panel's name. At
         * most 100 bytes */
        I18N_FORMAT(hint, sizeof(hint),
                    TR("Hold a button for 2 seconds to set off the alarm on {0}"),
                    ui_link_master_name());
        ui_set_str(&subject_panic_hint, hint);
    }
    /* LINK-023: a panic keyed without system communication is still sent, for a minute */
    const bool trying = keypad && (L.info.state == UI_LINK_LOST ||
                                   (L.info.state == UI_LINK_CONNECTING && L.info.panic_pending));
    ui_set_int(&subject_panic_link, trying ? 1 : 0);
}

/* KPD-006: only physical/remote input renews idle time, across every view. */
static uint32_t s_partition_input_at, s_partition_sent_at, s_partition_tick_at;
static lv_obj_t *s_partition_row;
static uint8_t s_partition_choices[ALARM_MAX_PARTITIONS];
static ui_choice_t s_partition_items[ALARM_MAX_PARTITIONS];
static char s_partition_names[ALARM_MAX_PARTITIONS][ALARM_NAME_LEN + 16];
static size_t s_partition_count;

void ui_partition_input(lv_event_t *e)
{
    (void)e;
    s_partition_input_at = lv_tick_get();
}

static uint8_t partition_default(void)
{
    const uint8_t home = ui_link_keypad() ? L.info.home : S.opt.partition_home;
    return home >= 1u && home <= ALARM_MAX_PARTITIONS ? home : 1u;
}

static void partition_shown(uint8_t target)
{
    s_partition_input_at = lv_tick_get();
    s_partition_sent_at = s_partition_input_at;
    ui_partition_view(target);
}

static void partition_selected(const ui_call_t *c)
{
    if (c->r != ALARM_OK || !ui_link_keypad()) {
        ui_toast(TR("Partition switching unavailable"));
        return;
    }
    L.info.selected = c->partition;
    partition_shown(c->partition);
}

static void partition_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= s_partition_count) return;
    const uint8_t target = s_partition_choices[index];
    if (ui_link_keypad()) {
        if (L.info.state != UI_LINK_ONLINE || S.be.partition_select == NULL) return;
        if (!ui_call_partition(target, false, partition_selected, 0u)) {
            ui_toast(TR("Partition switching unavailable"));
        }
        return; /* the master's answer shows it (partition_selected) */
    } else if (!S.opt.switch_allowed) return;
    partition_shown(target);
}

/* The input's report: sent once answered; until then the next tick doesn't send another. */
static bool s_activity_out;

static void activity_reported(const ui_call_t *c)
{
    s_activity_out = false;
    if (c->r == ALARM_OK) s_partition_sent_at = (uint32_t)c->tag;
}

static void partition_pick(lv_event_t *e)
{
    (void)e;
    if (S.cfg == NULL || (ui_link_keypad() ? !L.info.switch_allowed || L.info.state != UI_LINK_ONLINE
                                          : !S.opt.switch_allowed)) return;
    s_partition_count = 0u;
    int32_t selected = -1;
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        if (!S.cfg->partitions[p - 1u].enabled) continue;
        const size_t n = s_partition_count++;
        partition_name(p, s_partition_names[n], sizeof s_partition_names[n]);
        s_partition_choices[n] = p;
        s_partition_items[n] = (ui_choice_t){UI_ICON_WAREHOUSE, ui_tone_blue(), s_partition_names[n], NULL};
        if (p == S.opt.partition) selected = (int32_t)n;
    }
    ui_pick_sheet(TR("Choose a partition"), NULL, s_partition_items, s_partition_count, selected, partition_chosen);
}

static void partition_tick(void)
{
    /* LVGL does not forward PRESSING to indev listeners. Poll held fingers too:
     * scrolling and a touch on a disabled/already-selected control are activity. */
    for (lv_indev_t *i = lv_indev_get_next(NULL); i != NULL; i = lv_indev_get_next(i)) {
        if (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED) ui_partition_input(NULL);
    }
    if (lv_tick_elaps(s_partition_tick_at) < 1000u) return;
    s_partition_tick_at = lv_tick_get();
    const bool remote = ui_link_keypad();
    const bool allowed = remote ? L.info.switch_allowed : S.opt.switch_allowed;
    const uint8_t home = partition_default();
    if (remote) {
        uint8_t selected = L.info.state == UI_LINK_ONLINE ? L.info.selected : home;
        if (selected == 0u) selected = home;
        ui_partition_view(selected);
        if (selected != home && s_partition_input_at != s_partition_sent_at &&
            S.be.partition_select != NULL && L.info.state == UI_LINK_ONLINE && !s_activity_out) {
            s_activity_out = true;
            if (!ui_call_partition(selected, true, activity_reported, (uintptr_t)s_partition_input_at))
                s_activity_out = false;
        }
    } else {
        const uint32_t delay = (uint32_t)(S.opt.return_s != 0u ? S.opt.return_s : 30u) * 1000u;
        if (!allowed || (!S.opt.return_never && lv_tick_elaps(s_partition_input_at) >= delay))
            ui_partition_view(home);
    }
    unsigned count = 0u;
    if (S.cfg != NULL) for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++)
        if (S.cfg->partitions[p].enabled) count++;
    lv_obj_t *host = ui_find(S.main, "partition_switch");
    if (host == NULL) return;
    const bool show = allowed && count > 1u && (!remote || L.info.state == UI_LINK_ONLINE);
    if (show) lv_obj_remove_flag(host, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(host, LV_OBJ_FLAG_HIDDEN);
    if (s_partition_row != NULL) {
        char name[ALARM_NAME_LEN + 16];
        ui_set_label(s_partition_row, "title_label", partition_name(S.opt.partition, name, sizeof name));
        /* a row's detail isn't relabelled with the language (pages rebuild theirs); this row is
         * Home's, so it follows here, unchanged words left alone */
        ui_set_label(s_partition_row, "detail_label", TR("Switch partition"));
    }
}

/* A reassignment changes the saved default but a temporary selection never writes preferences. */
static void home_partition_follow(void)
{
    if (!ui_link_keypad()) return;
    S.opt.partition_home = partition_default();
    const uint8_t selected = L.info.state == UI_LINK_ONLINE && L.info.selected != 0u
                                 ? L.info.selected : partition_default();
    ui_partition_view(selected);
}

/* ---- the alarm panel: Keypads and devices ---------------------------------------------------- */

/* One keypad's row: its words, worked out once for the row and for the page's live check. */
typedef struct {
    char title[ALARM_NAME_LEN + 48];
    char detail[ALARM_NAME_LEN + 48];
    const char *value;
    char sub[64];
    lv_color_t tone;
    bool alert;    /* not responding: its trouble is raised (amber) */
    bool updating; /* restarting with its update: the value in blue */
} kp_words_t;

static void heard_words(const ui_link_keypad_t *k, char *out, size_t len)
{
    out[0] = '\0';
    if (k->heard_epoch_ms > 0) {
        char when[32];
        ui_time_or_day_text(k->heard_epoch_ms, when, sizeof(when));
        /* TR: a keypad on Wi-Fi that stopped answering: when it was last heard ("10:12 PM" or
         * "Sep 19"). At most 40 bytes */
        I18N_FORMAT(out, len, TR("Not heard since {0}"), when);
    }
}

static void kp_words(const ui_link_keypad_t *k, kp_words_t *w)
{
    memset(w, 0, sizeof(*w));
    char part[ALARM_NAME_LEN + 16];
    partition_name(k->home, part, sizeof(part));
    if (k->name[0] != '\0') {
        i18n_utf8_copy(w->title, sizeof(w->title), k->name);
        char number[48];
        keypad_name(&(ui_link_keypad_t){.device = k->device}, number, sizeof(number));
        two_facts(w->detail, sizeof(w->detail), number, part);
    } else {
        keypad_name(k, w->title, sizeof(w->title));
        i18n_utf8_copy(w->detail, sizeof(w->detail), part);
    }
    if (k->connected) {
        w->tone = ui_tone_green();
        /* TR: a keypad on Wi-Fi that answers the alarm panel; the value of its row (keep it
         * short) */
        w->value = TR_C("keypad state", "Connected");
    } else if (k->waiting) {
        w->tone = ui_tone_blue();
        /* TR: a keypad on Wi-Fi just added, not heard yet; the value of its row (keep it short) */
        w->value = TR_C("keypad state", "Connecting");
    } else if (k->fw_state == UI_LINK_FW_RESTARTING) {
        /* restarting with its update (up to 10 minutes): its trouble is held meanwhile */
        w->tone = ui_tone_blue();
        w->updating = true;
        /* TR: a keypad on Wi-Fi restarting with a software update this panel sent it; the value
         * of its row instead of "Not responding" (keep it short) */
        w->value = TR_C("keypad state", "Updating");
    } else {
        w->tone = k->down ? ui_tone_amber() : ui_tone_gray();
        w->alert = k->down;
        /* TR: a keypad on Wi-Fi that stopped answering (a trouble); the value of its row (keep it
         * short) */
        w->value = TR_C("keypad state", "Not responding");
        heard_words(k, w->sub, sizeof(w->sub));
    }
}

static uint32_t keypads_sig(void)
{
    uint32_t h = hash_int(2166136261u, (uint32_t)s_nkps);
    for (size_t i = 0; i < s_nkps; i++) {
        kp_words_t w;
        kp_words(&s_kps[i], &w);
        h = hash_int(h, s_kps[i].device);
        h = hash_str(hash_str(hash_str(hash_str(h, w.title), w.detail), w.value), w.sub);
        h = hash_int(h, (w.alert ? 1u : 0u) | (w.updating ? 2u : 0u));
    }
    return h;
}

static void open_add(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_LINK_ADD, 0);
}

static void open_keypad(void *ctx)
{
    ui_page_open(PAGE_LINK_KEYPAD, (int32_t)(intptr_t)ctx);
}

static lv_obj_t *keypad_row(lv_obj_t *body, const ui_link_keypad_t *k)
{
    kp_words_t w;
    kp_words(k, &w);
    lv_obj_t *row = ui_row_link(body, UI_ICON_KEYBOARD, w.tone, w.title, w.detail, w.value,
                                UI_ICON_CHEVRON_RIGHT, open_keypad, (void *)(intptr_t)k->device);
    if (w.alert || w.updating) {
        lv_obj_t *value = ui_find(row, "value_label");
        if (value != NULL) {
            lv_obj_set_style_text_color(value, w.tone, 0);
        }
    }
    if (w.sub[0] != '\0') {
        ui_row_sub(row, w.sub, w.alert ? ui_tone_amber() : ui_tone_gray());
    }
    return row;
}

uint8_t ui_link_keypad_count(void)
{
    return (uint8_t)read_keypads();
}

void ui_link_devices_section(lv_obj_t *body, uint16_t *held)
{
    *held = 0;
    if (!ui_link_master()) {
        return;
    }
    read_keypads();
    /* TR: Installer > Keypads and devices, the heading over the keypads on Wi-Fi (other RoboAlarms
     * panels). At most 47 bytes */
    ui_section(body, TR("Keypads"));
    ui_row_action(body, UI_ICON_PLUS, TR("Add a keypad"), ui_tone_blue(), open_add, NULL);
    ui_row_link(body, UI_ICON_KEYBOARD, ui_tone_blue(), TR("This panel"), TR("Partition settings"),
                NULL, UI_ICON_CHEVRON_RIGHT, open_keypad, NULL);
    for (size_t i = 0; i < s_nkps; i++) {
        keypad_row(body, &s_kps[i]);
        if (s_kps[i].device >= 1u && s_kps[i].device <= 16u) {
            *held |= (uint16_t)(1u << (s_kps[i].device - 1u));
        }
    }
    if (s_nkps == 0u) {
        ui_paragraph(body,
                     TR("Another RoboAlarms panel on this Wi-Fi can be a keypad for this one."),
                     ui_tone_gray());
    }
    /* TR: Installer > Keypads and devices, the heading over the installer's own table of
     * supervised devices. At most 47 bytes */
    ui_section(body, TR("Supervised devices"));
    L.page_sig = keypads_sig();
}

/* ---- adding a keypad (LINK-004, LINK-016) ---------------------------------------------------- */

static uint32_t add_sig(const ui_link_pair_t *p)
{
    uint32_t h = hash_int(2166136261u, A.refused);
    h = hash_int(h, p->state);
    h = hash_int(h, p->fail);
    h = hash_int(h, p->attempts);
    return hash_str(hash_str(h, p->code), p->peer_addr);
}

static void add_open_window(void)
{
    const uint8_t r = request(UI_LINK_REQ_PAIR_OPEN, false, 0u);
    A.refused = r;
    A.owner = r == UI_LINK_REQ_OK;
    read_pair(&A.p);
}

/* "Waiting for a keypad · 1 min 47 s left": the line under the title, once a second (UI-020). */
static void add_wait_note(const ui_link_pair_t *p, char *note, size_t nlen)
{
    const unsigned left = (unsigned)p->left_s;
    if (left >= 60u) {
        char secs[4];
        snprintf(secs, sizeof(secs), "%02u", left % 60u);
        /* TR: Add a keypad, the line under the title while pairing is open; {0} minutes, {1}
         * seconds ("05"). At most 63 bytes */
        I18N_FORMAT(note, nlen, TR("Waiting for a keypad · {0} min {1} s left"), left / 60u, secs);
    } else if (left > 0u) {
        /* TR: Add a keypad, the line under the title in pairing's last minute; {0} seconds. At
         * most 63 bytes */
        I18N_FORMAT(
            note, nlen,
            TRN("Waiting for a keypad · {0} s left", "Waiting for a keypad · {0} s left", left),
            left);
    } else {
        /* TR: Add a keypad, the line under the title while pairing opens. At most 63 bytes */
        i18n_utf8_copy(note, nlen, TR("Waiting for a keypad"));
    }
}

static void add_finish(void)
{
    char text[64];
    /* TR: a toast once a keypad was added; {0} is its number. At most 60 bytes */
    I18N_FORMAT(text, sizeof(text), TR("Added as Keypad {0}"), (unsigned)A.p.device);
    ui_page_back(); /* the list shows it; leaving closes pairing */
    ui_toast(text);
}

static void add_stop(void *ctx)
{
    (void)ctx;
    ui_page_back(); /* leaving the page closes pairing (ui_link_stop) */
}

static void add_retry(void *ctx)
{
    (void)ctx;
    add_open_window();
    ui_page_rebuild();
}

static void add_decide(bool allow)
{
    const uint8_t r = request(UI_LINK_REQ_PAIR_DECIDE, allow, 0u);
    if (r != UI_LINK_REQ_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(refused_words(r));
    }
    read_pair(&A.p);
    if (A.p.state == UI_LINK_PAIR_DONE) {
        add_finish();
        return;
    }
    ui_page_rebuild();
}

static void add_reject(void *ctx)
{
    (void)ctx;
    add_decide(false);
}

static void add_allow(void *ctx)
{
    (void)ctx;
    add_decide(true);
}

/* "On your Wi-Fi · 192.0.2.57" */
static void new_keypad_row(lv_obj_t *body, const ui_link_pair_t *p, bool allowed)
{
    char detail[96];
    if (allowed) {
        /* TR: Add a keypad, after Yes, allow: the keypad's own answer is still to come. At most
         * 95 bytes */
        i18n_utf8_copy(detail, sizeof(detail), TR("Now tap Yes, pair on the keypad"));
        /* TR: Add a keypad, the row after Yes, allow. At most 47 bytes */
        ui_row_link(body, UI_ICON_CIRCLE_CHECK, ui_tone_green(), TR("Allowed here"), detail, NULL,
                    NULL, NULL, NULL);
        return;
    }
    /* TR: Add a keypad: where the keypad that asked is (its address follows). At most 47 bytes */
    const char *where = TR("On your Wi-Fi");
    two_facts(detail, sizeof(detail), where, p->peer_addr);
    /* TR: Add a keypad: a keypad asked to pair (it has no name yet). At most 47 bytes */
    ui_row_link(body, UI_ICON_KEYBOARD, ui_tone_blue(), TR("A new keypad"), detail, NULL, NULL,
                NULL, NULL);
}

static void add_failed(lv_obj_t *body, const ui_link_pair_t *p, char *note, size_t nlen)
{
    /* TR: Add a keypad, the line under the title once pairing closed. At most 63 bytes */
    i18n_utf8_copy(note, nlen, TR("Pairing closed"));
    const char *title;
    const char *detail;
    if (p->attempts == 0u && (p->fail == UI_LINK_FAIL_TIMEOUT || p->fail == UI_LINK_FAIL_CLOSED)) {
        /* TR: Add a keypad, once pairing's 2 minutes ran out with no keypad. At most 47 bytes */
        title = TR("No keypad asked to pair");
    } else {
        /* TR: Add a keypad, once pairing ended without a keypad added. At most 47 bytes */
        title = TR("No keypad was added");
    }
    if (p->fail == UI_LINK_FAIL_LIMIT) {
        /* TR: Add a keypad: pairing closed after 3 failed attempts (LINK-016). At most 63 bytes */
        detail = TR("Closed after 3 tries");
    } else if (p->fail == UI_LINK_FAIL_TIMEOUT || p->fail == UI_LINK_FAIL_CLOSED) {
        /* TR: Add a keypad: pairing stays open for 2 minutes. At most 63 bytes */
        detail = TR("The 2 minutes ran out");
    } else {
        /* TR: Add a keypad, the pairing setup's: it ended before anything was stored. At most 63
         * bytes */
        detail = TR("Nothing was saved");
    }
    ui_row_link(body, UI_ICON_CIRCLE_DOT, ui_tone_gray(), title, detail, NULL, NULL, NULL, NULL);
    ui_paragraph(body,
                 TR("Check that the new keypad is on the same Wi-Fi and on its pairing step, then "
                    "try again."),
                 ui_tone_gray());
    ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_retry, NULL);
}

static void build_add(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    /* TR: Installer > Keypads and devices > Add a keypad, the page title. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Add a keypad"));
    if (S.page_fresh) {
        add_open_window(); /* opening the page opens pairing for 2 minutes */
    } else if (A.refused == UI_LINK_REQ_OK) {
        read_pair(&A.p);
    }
    L.page_sig = add_sig(&A.p);
    const ui_link_pair_t *p = &A.p;
    if (A.refused == UI_LINK_REQ_FULL ||
        (p->state == UI_LINK_PAIR_FAILED && p->fail == UI_LINK_FAIL_FULL)) {
        /* LINK-001: keypads on Wi-Fi share numbers 1-8 with Zigbee keypads and the table */
        ui_paragraph(body, TR("Not started: keypad numbers 1 to 8 are all in use."),
                     ui_tone_amber());
        ui_paragraph(
            body,
            TR("Keypads on Wi-Fi share them with Zigbee keypads and the supervised devices. "
               "Remove one you no longer use first."),
            ui_tone_gray());
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_retry, NULL);
        return;
    }
    if (A.refused != UI_LINK_REQ_OK) {
        /* The Zigbee add page's words for the same refusals (ui_zigbee.c) */
        ui_paragraph(body,
                     A.refused == UI_LINK_REQ_NOT_ALLOWED
                         ? TR("Not started: only in installer mode, with everything disarmed.")
                         : TR("Not started: that can't be done now."),
                     ui_tone_amber());
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_retry, NULL);
        return;
    }
    char code[16];
    code_text(p->code, code, sizeof(code));
    switch (p->state) {
    case UI_LINK_PAIR_WORKING:
    case UI_LINK_PAIR_COMPARE:
        /* TR: Add a keypad, the line under the title once a keypad asked. At most 63 bytes */
        i18n_utf8_copy(note, nlen, TR("A keypad asked to pair"));
        new_keypad_row(body, p, false);
        if (p->state == UI_LINK_PAIR_COMPARE) {
            ui_big_value(body, TR("Does the keypad show this code?"), code);
            ui_action_pair(body, UI_ICON_CIRCLE_X, TR("No, reject"), ui_tone_red(), add_reject,
                           UI_ICON_CIRCLE_CHECK, TR("Yes, allow"), ui_tone_green(), add_allow,
                           NULL);
            ui_paragraph(
                body,
                TR("If the codes differ, something on your network is in between: reject it."),
                ui_tone_amber());
        }
        break;
    case UI_LINK_PAIR_WAITING:
    case UI_LINK_PAIR_DONE:
        /* TR: Add a keypad, the line under the title after Yes, allow. At most 63 bytes */
        i18n_utf8_copy(note, nlen, TR("Allowed here · waiting for the keypad"));
        new_keypad_row(body, p, true);
        ui_big_value(body, TR("The code on both screens"), code);
        /* TR: the button that ends adding a keypad (as Zigbee's Stop) */
        ui_row_action(body, UI_ICON_X, TR("Stop"), ui_tone_gray(), add_stop, NULL);
        break;
    case UI_LINK_PAIR_FAILED: add_failed(body, p, note, nlen); break;
    default: { /* open: waiting for a keypad */
        add_wait_note(p, note, nlen);
        /* TR: Add a keypad, the heading over what to do on the new keypad. At most 47 bytes */
        ui_section(body, TR("On the new keypad"));
        ui_wifi_info_t w;
        memset(&w, 0, sizeof(w));
        if (S.be.wifi_info != NULL) {
            S.be.wifi_info(S.be.ctx, &w);
        }
        char detail[96];
        /* TR: Add a keypad: the new keypad joins the network this panel is on; {0} is its name. At
         * most 70 bytes */
        I18N_FORMAT(detail, sizeof(detail), TR("{0}, as this panel"),
                    w.ssid[0] != '\0' ? w.ssid : NOTR("-"));
        ui_row_link(body, UI_ICON_WIFI, ui_tone_blue(), TR("Join the same Wi-Fi"), detail, NULL,
                    NULL, NULL, NULL);
        /* TR: Add a keypad: on the new keypad's setup, the role "A keypad". At most 47 bytes */
        ui_row_link(body, UI_ICON_KEYBOARD, ui_tone_blue(), TR("Choose A keypad"),
                    TR("Its setup asks how it will be used"), NULL, NULL, NULL, NULL);
        ui_row_link(body, UI_ICON_CIRCLE_CHECK, ui_tone_blue(), TR("Compare the codes"),
                    TR("This page shows the code when it asks"), NULL, NULL, NULL, NULL);
        ui_row_action(body, UI_ICON_X, TR("Stop"), ui_tone_gray(), add_stop, NULL);
        ui_paragraph(body, TR("Leaving this page closes pairing."), ui_tone_gray());
        break;
    }
    }
}

/* Once a second while the add page shows: a new state builds the page again, the time left only
 * changes the line under the title. */
static void add_tick(bool can_rebuild)
{
    if (A.refused != UI_LINK_REQ_OK) {
        return;
    }
    ui_link_pair_t p;
    read_pair(&p);
    if (p.state == UI_LINK_PAIR_DONE) {
        A.p = p;
        if (can_rebuild) {
            add_finish();
        }
        return;
    }
    if (add_sig(&p) != L.page_sig) {
        if (can_rebuild) {
            A.p = p;
            ui_page_rebuild();
        }
        return;
    }
    A.p = p;
    if (p.state == UI_LINK_PAIR_OPEN || p.state == UI_LINK_PAIR_IDLE) {
        char note[96];
        add_wait_note(&p, note, sizeof(note));
        ui_page_note(note);
    }
}

/* ---- a keypad's page ------------------------------------------------------------------------- */

static const ui_link_keypad_t *find_keypad(uint8_t device)
{
    static ui_link_keypad_t local;
    if (device == 0u) {
        memset(&local, 0, sizeof local);
        local.home = S.opt.partition_home;
        local.switch_allowed = S.opt.switch_allowed;
        local.return_never = S.opt.return_never;
        local.return_s = S.opt.return_s;
        return &local;
    }
    read_keypads();
    for (size_t i = 0; i < s_nkps; i++) {
        if (s_kps[i].device == device) {
            return &s_kps[i];
        }
    }
    return NULL;
}

static void supervision_words(uint16_t s, char *out, size_t len)
{
    if (s == 0u) {
        i18n_utf8_copy(out, len, TR("Not supervised"));
    } else {
        /* TR: a number of minutes in a number row; {0} is the number. At most 20 bytes */
        I18N_FORMAT(out, len, TRN("{0} min", "{0} min", (uint32_t)((s + 59u) / 60u)),
                    (unsigned)((s + 59u) / 60u));
    }
}

/* The status row on top: what it is doing now, at most once a second. */
static void status_words(const ui_link_keypad_t *k, const char **title, char *detail, size_t len,
                         lv_color_t *tone)
{
    detail[0] = '\0';
    if (k->connected) {
        *tone = ui_tone_green();
        *title = TR_C("keypad state", "Connected");
        char version[48] = "";
        if (k->fw[0] != '\0') {
            /* TR: Settings > About, the panel's line: its version {0}. At most 63 bytes */
            I18N_FORMAT(version, sizeof(version), TR("Version {0}"), k->fw);
        }
        two_facts(detail, len, version, k->addr);
    } else if (k->waiting) {
        *tone = ui_tone_blue();
        *title = TR_C("keypad state", "Connecting");
        i18n_utf8_copy(detail, len, k->addr);
    } else if (k->fw_state == UI_LINK_FW_RESTARTING) {
        *tone = ui_tone_blue();
        *title = TR_C("keypad state", "Updating");
        /* TR: a keypad's page while it restarts with a software update this panel sent it, the
         * line under "Updating". At most 95 bytes */
        i18n_utf8_copy(detail, len, TR("Restarting with the new version"));
    } else {
        *tone = k->down ? ui_tone_amber() : ui_tone_gray();
        *title = TR_C("keypad state", "Not responding");
        heard_words(k, detail, len);
    }
}

static uint32_t keypad_sig(const ui_link_keypad_t *k)
{
    const char *title;
    char detail[96];
    lv_color_t tone;
    if (k == NULL) {
        return 1u;
    }
    status_words(k, &title, detail, sizeof(detail), &tone);
    uint32_t h = hash_str(hash_str(2166136261u, title), detail);
    h = hash_str(h, k->name);
    h = hash_int(h, k->home);
    h = hash_int(h, k->switch_allowed);
    h = hash_int(h, k->return_never);
    h = hash_int(h, k->return_s);
    return hash_int(h, k->supervision_s);
}

/* Changes are saved as they are made: the page's line says so (saved when you go back). */
static void keypad_set(const char *name, uint8_t home, uint16_t supervision_s)
{
    if (!S.program) return;
    if (s_kp_device == 0u) {
        S.opt.partition_home = home;
        ui_partition_view(home);
        ui_options_changed();
        ui_page_rebuild();
        return;
    }
    ui_link_req_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_LINK_REQ_KEYPAD_SET;
    req.device = s_kp_device;
    req.home = home;
    req.supervision_s = supervision_s;
    i18n_utf8_copy(req.name, sizeof(req.name), name);
    const uint8_t r =
        S.be.link_request != NULL ? S.be.link_request(S.be.ctx, &req) : UI_LINK_REQ_REFUSED;
    if (r != UI_LINK_REQ_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(refused_words(r));
    }
    ui_page_rebuild();
}

static void keypad_renamed(const char *text)
{
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    if (k != NULL) {
        keypad_set(text, k->home, k->supervision_s);
    }
}

static void rename_keypad(void *ctx)
{
    (void)ctx;
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    if (k == NULL) {
        return;
    }
    ui_text_sheet(TR("Keypad name"), TR("Hall keypad, Bedroom keypad..."), k->name, false,
                  (uint32_t)(sizeof(k->name) - 1u), keypad_renamed);
}

static ui_choice_t s_choices[ALARM_MAX_PARTITIONS + 1u];
static uint32_t s_values[ALARM_MAX_PARTITIONS + 1u];
static char s_titles[ALARM_MAX_PARTITIONS + 1u][ALARM_NAME_LEN + 16];

static void home_chosen(int32_t index)
{
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    if (k != NULL && index >= 0 && index <= (int32_t)ALARM_MAX_PARTITIONS) {
        keypad_set(k->name, (uint8_t)s_values[index], k->supervision_s);
    } else {
        ui_page_rebuild();
    }
}

static void pick_home(void *ctx)
{
    (void)ctx;
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    if (k == NULL || S.cfg == NULL) {
        return;
    }
    size_t n = 0;
    int32_t selected = -1;
    for (uint8_t p = 1; p <= ALARM_MAX_PARTITIONS; p++) {
        if (!S.cfg->partitions[p - 1u].enabled && p != k->home) {
            continue;
        }
        partition_name(p, s_titles[n], sizeof(s_titles[0]));
        s_choices[n] = (ui_choice_t){UI_ICON_WAREHOUSE, ui_tone_violet(), s_titles[n], NULL};
        s_values[n] = p;
        if (p == k->home) {
            selected = (int32_t)n;
        }
        n++;
    }
    ui_pick_sheet(TR("Default partition"), NULL, s_choices, n,
                  selected, home_chosen);
}

/* LINK-007: 1, 5 (the default) or 15 minutes, or not supervised (decided 2026-10-15); a value set
 * another way stays offered as it is. */
static const uint16_t SUPERVISION_S[] = {60u, 300u, 900u, 0u};
#define SUPERVISION_N (sizeof(SUPERVISION_S) / sizeof(SUPERVISION_S[0]))

static void supervision_chosen(int32_t index)
{
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    if (k != NULL && index >= 0 && (size_t)index <= SUPERVISION_N) {
        keypad_set(k->name, k->home, (uint16_t)s_values[index]);
    } else {
        ui_page_rebuild();
    }
}

static void pick_supervision(void *ctx)
{
    (void)ctx;
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    if (k == NULL) {
        return;
    }
    const uint16_t now = k->supervision_s;
    bool listed = false;
    for (size_t i = 0; i < SUPERVISION_N; i++) {
        listed = listed || SUPERVISION_S[i] == now;
    }
    size_t n = 0;
    for (size_t i = 0; i < SUPERVISION_N; i++) {
        if (!listed && now != 0u && (SUPERVISION_S[i] == 0u || now < SUPERVISION_S[i])) {
            s_values[n++] = now; /* in its place among the times */
            listed = true;
        }
        s_values[n++] = SUPERVISION_S[i];
    }
    int32_t selected = -1;
    for (size_t i = 0; i < n; i++) {
        supervision_words((uint16_t)s_values[i], s_titles[i], sizeof(s_titles[0]));
        s_choices[i] = (ui_choice_t){NULL, ui_tone_blue(), s_titles[i], NULL};
        if (s_values[i] == now) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("Supervision"), TR("Silent this long: a trouble"), s_choices, n, selected,
                  supervision_chosen);
}

/* Held for 2 seconds: the hold is the confirmation, no dialog follows (decided 2026-10-15). */
static void remove_keypad(void *ctx)
{
    (void)ctx;
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    char name[ALARM_NAME_LEN + 48];
    if (k == NULL) {
        ui_page_back();
        return;
    }
    keypad_name(k, name, sizeof(name));
    const uint8_t r = request(UI_LINK_REQ_KEYPAD_REMOVE, false, s_kp_device);
    if (r != UI_LINK_REQ_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(refused_words(r));
        ui_page_rebuild();
        return;
    }
    char text[ALARM_NAME_LEN + 80];
    /* TR: a toast once a keypad on Wi-Fi was removed; {0} is its name. At most 80 bytes */
    I18N_FORMAT(text, sizeof(text), TR("{0} removed"), name);
    ui_page_back();
    ui_toast(text);
}

static void policy_set(bool allowed, bool never, uint16_t seconds)
{
    if (!S.program) return;
    if (s_kp_device == 0u) {
        S.opt.switch_allowed = allowed;
        S.opt.return_never = never;
        S.opt.return_s = seconds;
        ui_options_changed();
        ui_partition_view(partition_default());
    } else {
        ui_link_req_t req = {0};
        req.kind = UI_LINK_REQ_PARTITION_POLICY;
        req.device = s_kp_device;
        req.switch_allowed = allowed;
        req.return_never = never;
        req.return_s = seconds;
        const uint8_t r = S.be.link_request != NULL ? S.be.link_request(S.be.ctx, &req) : UI_LINK_REQ_REFUSED;
        if (r != UI_LINK_REQ_OK) ui_toast(refused_words(r));
    }
    ui_page_rebuild();
}

static void policy_allowed(bool on, void *ctx)
{
    (void)ctx;
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    if (k != NULL) policy_set(on, k->return_never, k->return_s != 0u ? k->return_s : 30u);
}
static void policy_never(bool on, void *ctx)
{
    (void)ctx;
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    if (k != NULL) policy_set(k->switch_allowed, on, k->return_s != 0u ? k->return_s : 30u);
}
static void policy_delay(int32_t value, void *ctx)
{
    (void)ctx;
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    if (k != NULL && k->switch_allowed && !k->return_never && value >= 1 && value <= 3600)
        policy_set(k->switch_allowed, false, (uint16_t)value);
}
static void policy_rows(lv_obj_t *body, const ui_link_keypad_t *k)
{
    char part[ALARM_NAME_LEN + 16];
    ui_row_link(body, UI_ICON_WAREHOUSE, ui_tone_blue(), TR("Default partition"), NULL,
                partition_name(k->home, part, sizeof part), UI_ICON_CHEVRON_RIGHT, pick_home, NULL);
    ui_row_toggle(body, TR("Allow partition switching"), TR("Off locks this panel to its default partition"),
                  k->switch_allowed, policy_allowed, NULL);
    lv_obj_t *delay = ui_row_number(body, TR("Return after inactivity"), TR("Seconds · Default 30"),
                                    k->return_s != 0u ? k->return_s : 30u, 1, 3600, 1, UNIT_SECONDS,
                                    NULL, policy_delay, NULL);
    if (k->return_never || !k->switch_allowed) ui_row_disable(delay);
    lv_obj_t *never = ui_row_toggle(body, TR("Never return automatically"), NULL, k->return_never, policy_never, NULL);
    if (!k->switch_allowed) ui_row_disable(never);
}

static void build_keypad(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                         size_t nlen)
{
    s_kp_device = (uint8_t)arg;
    const ui_link_keypad_t *k = find_keypad(s_kp_device);
    L.page_sig = keypad_sig(k);
    if (s_kp_device == 0u) {
        i18n_utf8_copy(title, tlen, TR("This panel"));
        i18n_utf8_copy(note, nlen, TR("Installer · Partition settings"));
        if (S.program) policy_rows(body, k);
        return;
    }
    if (k == NULL) {
        I18N_FORMAT(title, tlen, TR_C("default keypad name", "Keypad {0}"), (unsigned)s_kp_device);
        /* TR: a keypad's page once it is no longer paired (removed meanwhile) */
        ui_paragraph(body, TR("This keypad is no longer paired."), ui_tone_gray());
        return;
    }
    char name[ALARM_NAME_LEN + 48];
    i18n_utf8_copy(title, tlen, keypad_name(k, name, sizeof(name)));
    /* TR: the line under a keypad's title on the alarm panel; {0} is its number. At most 100
     * bytes */
    I18N_FORMAT(note, nlen, TR("Keypad {0} · saved when you go back"), (unsigned)k->device);
    const char *state;
    char detail[96];
    lv_color_t tone;
    status_words(k, &state, detail, sizeof(detail), &tone);
    ui_row_link(body, UI_ICON_KEYBOARD, tone, state, detail, NULL, NULL, NULL, NULL);
    /* TR: a keypad's page, the heading over its settings. At most 47 bytes */
    ui_section(body, TR("The keypad"));
    ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("Name"), NULL, k->name, UI_ICON_CHEVRON_RIGHT,
                rename_keypad, NULL);
    policy_rows(body, k);
    char sup[32];
    supervision_words(k->supervision_s, sup, sizeof(sup));
    ui_row_link(body, UI_ICON_CLOCK_ALERT, ui_tone_blue(), TR("Supervision"),
                TR("Silent this long: a trouble"), sup, UI_ICON_CHEVRON_RIGHT, pick_supervision,
                NULL);
    ui_section(body, "");
    ui_row_hold(body, TR("Remove this keypad"), remove_keypad, NULL);
    ui_paragraph(body, TR("It stops working at once. To use it again, pair it again."),
                 ui_tone_gray());
}

/* ---- a keypad: About, and making it a stand-alone panel again -------------------------------- */

/* The keypad's row under the panel's own: what it belongs to and whether it answers. */
static void about_words(char *title, size_t tlen, char *detail, size_t dlen, lv_color_t *tone,
                        bool *alert)
{
    const char *master = ui_link_master_name();
    *alert = false;
    if (L.info.state == UI_LINK_NOT_PAIRED) {
        *tone = ui_tone_gray();
        i18n_utf8_copy(title, tlen, TR("Not paired"));
        if (L.info.removed) {
            I18N_FORMAT(detail, dlen, TR("{0} removed this keypad"), master);
        } else {
            i18n_utf8_copy(detail, dlen, TR("It isn't paired with an alarm panel"));
        }
        return;
    }
    ui_link_keypad_title(title, tlen, L.info.device, master);
    switch (L.info.state) {
    case UI_LINK_ONLINE: {
        *tone = ui_tone_green();
        char part[ALARM_NAME_LEN + 16];
        /* TR: About on a keypad, under "Keypad 2 of Harbor House"; {0} is its home partition's
         * name. At most 80 bytes */
        I18N_FORMAT(
            detail, dlen, TR("Connected · shows and arms {0}"),
            partition_name(L.info.home != 0u ? L.info.home : S.opt.partition, part, sizeof(part)));
        break;
    }
    case UI_LINK_CONNECTING:
        *tone = ui_tone_blue();
        i18n_utf8_copy(detail, dlen, TR_C("keypad state", "Connecting"));
        break;
    default:
        *tone = ui_tone_amber();
        *alert = true;
        i18n_utf8_copy(detail, dlen, TR("No system communication"));
        break;
    }
}

static uint32_t about_sig(void)
{
    char title[96];
    char detail[128];
    lv_color_t tone;
    bool alert;
    about_words(title, sizeof(title), detail, sizeof(detail), &tone, &alert);
    return hash_str(hash_str(2166136261u, title), detail);
}

void ui_link_about_row(lv_obj_t *body)
{
    if (!ui_link_keypad()) {
        return;
    }
    char title[96];
    char detail[128];
    lv_color_t tone;
    bool alert;
    about_words(title, sizeof(title), detail, sizeof(detail), &tone, &alert);
    lv_obj_t *row =
        ui_row_link(body, UI_ICON_KEYBOARD, tone, title, detail, NULL, NULL, NULL, NULL);
    if (alert) {
        ui_row_set_detail(row, detail, true, ui_tone_amber());
    }
    L.page_sig = about_sig();
}

void ui_link_about_details(lv_obj_t *body)
{
    /* TR: About on a keypad, the heading over the alarm panel it belongs to. At most 47 bytes */
    ui_section(body, TR("Alarm panel"));
    ui_row_value(body, TR("Name"), ui_link_master_name());
    if (L.info.master_addr[0] != '\0') {
        /* TR: About on a keypad: the alarm panel's address on the network. At most 31 bytes */
        ui_row_value(body, TR("Address"), L.info.master_addr);
    }
    char number[8];
    if (L.info.device != 0u) {
        snprintf(number, sizeof(number), "%u", (unsigned)L.info.device);
    } else {
        snprintf(number, sizeof(number), "-");
    }
    /* TR: About on a keypad: its number at the alarm panel (1 to 8). At most 31 bytes */
    ui_row_value(body, TR("Keypad number"), number);
}

static void open_standalone(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_LINK_STANDALONE, 0);
}

void ui_link_about_end(lv_obj_t *body)
{
    ui_section(body, ""); /* apart, under a gap, where an alarm panel has Installer */
    ui_row_link(body, UI_ICON_ROTATE_CCW, ui_tone_red(), TR("Make it a stand-alone panel"),
                TR("A factory reset: erases the pairing and every setting"), NULL,
                UI_ICON_CHEVRON_RIGHT, open_standalone, NULL);
}

/* ---- keypad updates (docs/features/11 "Updates through the link", proposal P2) ---------------- *
 * Keypads follow their alarm panel: it tells each one which version to have, each gets it while it
 * keeps protecting, and they restart together once everything is disarmed. Nobody acts on any of
 * it, so nothing here can be tapped; a percent changes at most once a second (UI-020). */

const char *ui_link_fw_master(void)
{
    if (!ui_link_keypad() || !L.info.fw_follows || ui_link_update_open()) {
        return NULL; /* its own Install stays in maintenance and when it needs an update */
    }
    return ui_link_master_name();
}

bool ui_link_fw_from_master(void)
{
    return ui_link_fw_master() != NULL && L.info.fw_from_master;
}

/* The alarm panel's Software update: one row per keypad, its status line in its tone. */
typedef struct {
    char title[ALARM_NAME_LEN + 48];
    char detail[96];
    char state[96];
    lv_color_t tone;
} upd_words_t;

static struct {
    lv_obj_t *rows[LINK_KEYPADS];
    uint8_t devices[LINK_KEYPADS];
    uint32_t sigs[LINK_KEYPADS];
    size_t n;
} U;

static void upd_words(const ui_link_keypad_t *k, upd_words_t *w)
{
    memset(w, 0, sizeof(*w));
    keypad_name(k, w->title, sizeof(w->title));
    if (k->name[0] != '\0' && k->fw[0] != '\0') {
        /* TR: the alarm panel's Software update, a keypad's line under its name: its number {0}
         * and the version {1} it runs ("Keypad 2 · version 0.6.1"). At most 63 bytes */
        I18N_FORMAT(w->detail, sizeof(w->detail), TR("Keypad {0} · version {1}"),
                    (unsigned)k->device, k->fw);
    } else if (k->name[0] != '\0') {
        keypad_name(&(ui_link_keypad_t){.device = k->device}, w->detail, sizeof(w->detail));
    } else if (k->fw[0] != '\0') {
        I18N_FORMAT(w->detail, sizeof(w->detail), TR("Version {0}"), k->fw);
    }
    char pct[16];
    /* TR: a percent done ("45 %"); {0} is the number */
    I18N_FORMAT(pct, sizeof(pct), TR_C("percent", "{0} %"),
                (unsigned)(k->fw_percent > 100u ? 100u : k->fw_percent));
    w->tone = ui_tone_blue();
    switch (k->fw_state) {
    case UI_LINK_FW_UP_TO_DATE:
        /* TR: the alarm panel's Software update, a keypad's status line: it runs this panel's
         * version. At most 95 bytes */
        i18n_utf8_copy(w->state, sizeof(w->state), TR("Up to date"));
        w->tone = ui_tone_green();
        break;
    case UI_LINK_FW_DOWNLOADING:
        /* TR: a keypad's status line in Software update while it downloads the update itself;
         * {0} is the percent done ("45 %"). At most 80 bytes */
        I18N_FORMAT(w->state, sizeof(w->state), TR("Downloading · {0}"), pct);
        break;
    case UI_LINK_FW_RECEIVING:
        /* TR: the alarm panel's Software update, a keypad's status line while this panel sends it
         * the update; {0} is the percent done ("30 %"). At most 80 bytes */
        I18N_FORMAT(w->state, sizeof(w->state), TR("Receiving from this panel · {0}"), pct);
        break;
    case UI_LINK_FW_READY:
        /* TR: the alarm panel's Software update, a keypad's status line: it has the update and
         * restarts together with this panel. At most 95 bytes */
        i18n_utf8_copy(w->state, sizeof(w->state), TR("Ready · restarts with this panel"));
        break;
    case UI_LINK_FW_RESTARTING:
        /* TR: the alarm panel's Software update, a keypad's status line while it restarts with
         * the update. At most 95 bytes */
        i18n_utf8_copy(w->state, sizeof(w->state), TR_C("keypad update", "Restarting"));
        break;
    case UI_LINK_FW_NOT_UPDATED:
        /* TR: the alarm panel's Software update, a keypad's status line when it missed this
         * panel's version: nobody has to do anything. At most 95 bytes */
        i18n_utf8_copy(w->state, sizeof(w->state), TR("Not updated yet · tries again by itself"));
        w->tone = ui_tone_gray();
        break;
    case UI_LINK_FW_NEWER:
        /* TR: the alarm panel's Software update, a keypad's status line when it runs a newer
         * version than this panel (it is left alone). At most 95 bytes */
        i18n_utf8_copy(w->state, sizeof(w->state), TR("Newer than this panel"));
        w->tone = ui_tone_gray();
        break;
    default: w->tone = ui_tone_gray(); break; /* not known yet: no status line */
    }
}

static uint32_t upd_sig(const upd_words_t *w)
{
    uint32_t h = hash_str(hash_str(hash_str(2166136261u, w->title), w->detail), w->state);
    return hash_int(h, lv_color_to_u32(w->tone));
}

bool ui_link_update_keypads(lv_obj_t *body)
{
    U.n = 0;
    if (!ui_link_master() || read_keypads() == 0u) {
        return false;
    }
    /* TR: the alarm panel's Software update, the heading over its keypads. At most 47 bytes */
    ui_section(body, TR("Keypads"));
    /* TR: the alarm panel's Software update, under the Keypads heading. At most 127 bytes */
    ui_paragraph(body,
                 TR("Keypads update with this panel. They restart together once everything is "
                    "disarmed."),
                 ui_tone_gray());
    for (size_t i = 0; i < s_nkps; i++) {
        upd_words_t w;
        upd_words(&s_kps[i], &w);
        lv_obj_t *row = ui_row_link(body, UI_ICON_KEYBOARD, w.tone, w.title,
                                    w.detail[0] != '\0' ? w.detail : NULL, NULL, NULL, NULL, NULL);
        ui_row_sub(row, w.state, w.tone);
        U.rows[U.n] = row;
        U.devices[U.n] = s_kps[i].device;
        U.sigs[U.n] = upd_sig(&w);
        U.n++;
    }
    return true;
}

bool ui_link_update_keypads_tick(void)
{
    if (U.n == 0u) {
        return ui_link_master() && read_keypads() != 0u; /* the first keypad came */
    }
    if (read_keypads() != U.n) {
        return true;
    }
    for (size_t i = 0; i < U.n; i++) {
        if (s_kps[i].device != U.devices[i]) {
            return true;
        }
    }
    for (size_t i = 0; i < U.n; i++) {
        upd_words_t w;
        upd_words(&s_kps[i], &w);
        const uint32_t sig = upd_sig(&w);
        if (sig == U.sigs[i]) {
            continue; /* nothing new: the row is not drawn again */
        }
        U.sigs[i] = sig;
        lv_obj_t *row = U.rows[i];
        ui_set_label(row, "title_label", w.title);
        if (w.detail[0] != '\0') {
            ui_row_set_detail(row, w.detail, false, w.tone);
        }
        ui_chip_tone(ui_find(row, "chip"), UI_ICON_KEYBOARD, w.tone);
        ui_row_sub(row, w.state, w.tone);
    }
    return false;
}

void ui_link_update_keypads_stop(void)
{
    memset(&U, 0, sizeof(U));
}

static bool standalone_code(const char *code);

static void standalone_checked(const ui_call_t *c)
{
    if (!ui_pad_asks(standalone_code)) {
        return; /* the pad closed while the code was checked */
    }
    const uint8_t user = c->user;
    if (c->r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(c->r == ALARM_ERR_INVALID_CODE ? TR("Wrong code, try again")
                                                    : ui_result_text(c->r));
        return;
    }
    if (user < 1u || user > ALARM_MAX_USERS ||
        S.cfg->users[user - 1u].authority != ALARM_AUTH_INSTALLER) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(TR("That isn't the installer code"));
        return;
    }
    L.standalone_ok = true;
    ui_pad_cancel();
    ui_page_rebuild();
}

/* The pad stays open; the answer opens the page (standalone_checked). */
static bool standalone_code(const char *code)
{
    if (!ui_call_authenticate(code, standalone_checked, 0u)) {
        ui_pad_error(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
    return false;
}

static void standalone_ask(void *ctx)
{
    (void)ctx;
    ui_pad_ask(HOST_SETTINGS, UI_ICON_ROTATE_CCW, TR("Make it a stand-alone panel"),
               TR("Enter the installer code"), ui_tone_red(), standalone_code);
}

static void standalone_now(void *ctx)
{
    (void)ctx;
    if (S.be.factory_reset == NULL) {
        ui_toast(TR("Not available here"));
        return;
    }
    ui_toast(TR("Erasing. The panel restarts with the setup"));
    S.be.factory_reset(S.be.ctx, true); /* a keypad keeps no event log */
}

static void build_standalone(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    i18n_utf8_copy(title, tlen, TR("Make it a stand-alone panel"));
    /* TR: Make it a stand-alone panel, the line under the title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("A factory reset"));
    const char *master = ui_link_master_name();
    char text[192];
    /* TR: Make it a stand-alone panel (a keypad's factory reset); {0} is the alarm panel's name.
     * At most 160 bytes */
    I18N_FORMAT(
        text, sizeof(text),
        TR("Erases the pairing with {0}, Wi-Fi and every setting, then restarts with the setup."),
        master);
    ui_paragraph(body, text, ui_tone_gray());
    /* TR: Make it a stand-alone panel, in amber; {0} is the alarm panel's name. At most 160 bytes
     */
    I18N_FORMAT(text, sizeof(text),
                TR("{0} shows a trouble for this keypad until it is removed there."), master);
    ui_paragraph(body, text, ui_tone_amber());
    if (L.standalone_ok || ui_link_maintenance()) {
        ui_row_hold(body, TR("Hold to erase everything"), standalone_now, NULL);
    } else {
        /* the installer code, checked by the alarm panel; after 10 minutes without it, none
         * (LINK-025) */
        ui_row_action(body, UI_ICON_KEY, TR("Enter the installer code"), ui_tone_amber(),
                      standalone_ask, NULL);
    }
    L.page_sig = ui_link_maintenance() ? 2u : 1u;
}

/* ---- pages and the tick ---------------------------------------------------------------------- */

bool ui_link_build(page_id_t id, int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                   size_t nlen)
{
    switch (id) {
    case PAGE_LINK_ADD: build_add(body, title, tlen, note, nlen); return true;
    case PAGE_LINK_KEYPAD: build_keypad(arg, body, title, tlen, note, nlen); return true;
    case PAGE_LINK_STANDALONE: build_standalone(body, title, tlen, note, nlen); return true;
    default: return false;
    }
}

static bool in_stack(page_id_t id)
{
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS) {
        return false;
    }
    for (size_t i = 0; i < S.depth; i++) {
        if (S.pages[i].id == id) {
            return true;
        }
    }
    return false;
}

void ui_link_stop(void)
{
    if (A.owner && !in_stack(PAGE_LINK_ADD)) {
        A.owner = false;
        (void)request(UI_LINK_REQ_PAIR_STOP, false, 0u); /* leaving the page closes pairing */
    }
    if (!in_stack(PAGE_LINK_STANDALONE)) {
        L.standalone_ok = false; /* the code opens it for one visit */
    }
}

/* A finger is on the screen: a page built again now would drop a hold under it. */
static bool touching(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i != NULL; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED) {
            return true;
        }
    }
    return false;
}

/* The open page's live words, once a second (UI-020, UI-023): built again only when they change. */
static void pages_tick(void)
{
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS || S.depth == 0u) {
        return;
    }
    const bool quiet = lv_subject_get_int(&subject_sheet) == SHEET_NONE &&
                       lv_subject_get_int(&subject_settings_pad) == 0 && !touching();
    const ui_page_t *pg = &S.pages[S.depth - 1u];
    switch (pg->id) {
    case PAGE_DEVICES:
        if (ui_link_master() && S.program) {
            read_keypads();
            if (keypads_sig() != L.page_sig && quiet) {
                ui_page_rebuild();
            }
        }
        break;
    case PAGE_LINK_ADD: add_tick(quiet); break;
    case PAGE_LINK_KEYPAD:
        if (keypad_sig(find_keypad((uint8_t)pg->arg)) != L.page_sig && quiet) {
            ui_page_rebuild();
        }
        break;
    case PAGE_ABOUT:
        if (ui_link_keypad() && about_sig() != L.page_sig && quiet) {
            ui_page_rebuild();
        }
        break;
    case PAGE_LINK_STANDALONE:
        if ((ui_link_maintenance() ? 2u : 1u) != L.page_sig && quiet) {
            ui_page_rebuild();
        }
        break;
    default: break;
    }
}

void ui_link_init(void)
{
    memset(&L, 0, sizeof(L));
    memset(&A, 0, sizeof(A));
    L.polled = lv_tick_get();
    L.have = read_info(&L.info);
    s_partition_input_at = s_partition_sent_at = lv_tick_get();
    s_partition_tick_at = lv_tick_get() - 1000u;
    lv_obj_t *host = ui_find(S.main, "partition_switch");
    if (host != NULL) {
        s_partition_row = ui_row_link(host, UI_ICON_WAREHOUSE, ui_tone_blue(), "", TR("Switch partition"),
                                     NULL, UI_ICON_CHEVRON_RIGHT, NULL, NULL);
        lv_obj_add_flag(s_partition_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_partition_row, partition_pick, LV_EVENT_CLICKED, NULL);
    }
    if (ui_link_keypad()) {
        /* the platform set the partition from the link record before the UI started; a home
         * given since is taken here, range-checked */
        L.gen_known = true;
        L.home_gen = L.info.home_gen;
        if (L.info.home >= 1u && L.info.home <= ALARM_MAX_PARTITIONS) {
            S.opt.partition = L.info.home;
        }
    }
    subjects_follow();
}

void ui_link_tick(void)
{
    partition_tick();
    if (S.be.link_info == NULL || lv_tick_elaps(L.polled) < POLL_MS) {
        return;
    }
    L.polled = lv_tick_get();
    const bool was_local = ui_link_local_alarm();
    ui_link_info_t now;
    const bool have = read_info(&now);
    const ui_link_info_t *was = &L.info;
    const bool changed = have != L.have || now.role != was->role || now.state != was->state ||
                         now.removed != was->removed || now.maintenance != was->maintenance ||
                         now.needs_update != was->needs_update ||
                         now.local_alarm != was->local_alarm ||
                         now.panic_pending != was->panic_pending || now.device != was->device ||
                         now.home != was->home || now.since_epoch_ms != was->since_epoch_ms ||
                         strcmp(now.master_name, was->master_name) != 0;
    /* the alarm panel's pages: what it serves, who holds installer mode, a newer answer */
    const bool remote_moved = now.remote_areas != was->remote_areas ||
                              now.remote_installer != was->remote_installer ||
                              now.remote_holder != was->remote_holder;
    const bool remote_gen = now.remote_gen != was->remote_gen || now.state != was->state;
    L.have = have;
    L.info = now;
    home_partition_follow();
    if (remote_moved) {
        ui_settings_gate(); /* the list's lines */
        ui_refresh(true);   /* the banner's words */
    }
    if (ui_link_keypad() && (remote_moved || remote_gen)) {
        ui_settings_remote_changed();
    }
    if (changed) {
        subjects_follow();
        ui_refresh(true); /* Home's card, the status bar's banner */
    }
    if (ui_link_local_alarm() && !was_local) {
        ui_app_wake(); /* a fire or CO tone kept here wakes the screen, as an alarm does */
    }
    pages_tick();
}
