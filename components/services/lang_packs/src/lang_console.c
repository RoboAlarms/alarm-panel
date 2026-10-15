/*
 * lang_console.c - the `lang` and `asset` service console commands (docs/features/37
 * language-packs.md "Console", docs/features/39 asset-packs.md "The panel").
 * Plain English: the console is never translated.
 *
 *   lang                  the installed packs, the update source's list, the last download
 *   lang load <locale>    load an installed pack into memory now
 *   lang rm <locale>      remove an installed pack (refused while armed)
 *   lang fetch <locale>   download and install the pack the update source lists
 *   lang verify           check every installed pack's signature from flash
 *   asset                 each asset kind's pack: state, version, match, bytes (docs/features/39)
 *   asset verify          check every installed asset pack again from flash
 *
 * Load, remove and both verifies run on the language service's own task (an internal-RAM stack): they read flash
 * with the cache off or write it, and the console's stack is not ours to assume.
 */
#include "lang_packs.h"

#include "sdkconfig.h"

#if CONFIG_ALARM_FEATURE_LANG_PACKS

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "asset_packs.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_console.h"
#include "lang_packs_internal.h"

static const char *TAG = "lang_con";

static const char *const STATE[] = {"ready", "other version", "refused", "damaged"};

static void print_list(void)
{
    /* Static, in PSRAM: the console task's stack is small and internal RAM is the reserve. */
    static EXT_RAM_BSS_ATTR lang_pack_info_t list[LANG_PACKS_LIST_MAX];
    static EXT_RAM_BSS_ATTR lang_available_t av[LANG_PACKS_LIST_MAX];
    const size_t n = lang_packs_list(list, LANG_PACKS_LIST_MAX);
    printf("Installed language packs: %u\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        const lang_pack_info_t *p = &list[i];
        printf("  %-10s %-16s version %-8s %-13s %7u bytes%s%s%s\n", p->locale, p->name, p->version,
               p->state < sizeof(STATE) / sizeof(STATE[0]) ? STATE[p->state] : "?", (unsigned)p->size,
               p->loaded ? ", loaded" : "", p->is_default ? ", default" : "", p->draft ? ", draft" : "");
    }
    printf("Free in the store: %u bytes\n", (unsigned)lang_packs_free_bytes());
    printf("Service task stack, lowest free so far: %u bytes\n", (unsigned)lang_packs_task_stack_free());
    const size_t na = lang_packs_available(av, LANG_PACKS_LIST_MAX);
    printf("Offered by the update source: %u\n", (unsigned)na);
    for (size_t i = 0; i < na; i++) {
        printf("  %-10s version %-8s %7u bytes\n", av[i].locale, av[i].version[0] ? av[i].version : "?",
               (unsigned)av[i].size);
    }
    lang_fetch_status_t f;
    lang_packs_fetch_status(&f);
    static const char *const PHASE[] = {"idle", "downloading", "done", "failed"};
    printf("Last download: %s%s%s%s\n", f.phase < 4u ? PHASE[f.phase] : "?", f.locale[0] ? " " : "", f.locale,
           f.phase >= 2u ? (f.result == LANG_OK ? " (ok)" : " (see result below)") : "");
    if (f.phase == 3u) {
        printf("  result: %s\n", lang_result_name((lang_result_t)f.result));
    }
}

typedef struct {
    char locale[16];
    lang_result_t result;
} job_t;

static void job_load(void *arg)
{
    job_t *j = arg;
    j->result = lang_packs_load_now(j->locale);
}

static void job_remove(void *arg)
{
    job_t *j = arg;
    j->result = lang_packs_remove(j->locale);
}

static void job_verify(void *arg)
{
    lang_verify_t *v = arg; /* LANG_PACKS_LIST_MAX + 1 entries: [0].err is the count */
    const size_t n = lang_packs_verify_all(v + 1, LANG_PACKS_LIST_MAX);
    v[0].err = (esp_err_t)n;
}

static int run_job(void (*fn)(void *), const char *locale, const char *what)
{
    static job_t job; /* static: a job never outlives the call, but a stack this small is not ours */
    memset(&job, 0, sizeof(job));
    snprintf(job.locale, sizeof(job.locale), "%s", locale);
    if (!lang_packs_run_on_task(fn, &job)) {
        printf("The language service is not running.\n");
        return 1;
    }
    printf("%s %s: %s\n", what, locale, lang_result_name(job.result));
    return job.result == LANG_OK ? 0 : 1;
}

static int cmd_lang(int argc, char **argv)
{
    if (argc < 2) {
        print_list();
        return 0;
    }
    if (strcmp(argv[1], "load") == 0 && argc == 3) {
        return run_job(job_load, argv[2], "load");
    }
    if (strcmp(argv[1], "rm") == 0 && argc == 3) {
        return run_job(job_remove, argv[2], "remove");
    }
    if (strcmp(argv[1], "fetch") == 0 && argc == 3) {
        const lang_result_t r = lang_packs_fetch(argv[2]);
        if (r == LANG_OK) {
            printf("fetch %s: queued; `lang` shows how it goes\n", argv[2]);
        } else {
            printf("fetch %s: %s\n", argv[2], lang_result_name(r));
        }
        return r == LANG_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "verify") == 0 && argc == 2) {
        static EXT_RAM_BSS_ATTR lang_verify_t v[LANG_PACKS_LIST_MAX + 1];
        memset(v, 0, sizeof(v));
        if (!lang_packs_run_on_task(job_verify, v)) {
            printf("The language service is not running.\n");
            return 1;
        }
        const size_t n = (size_t)v[0].err;
        int bad = 0;
        for (size_t i = 1; i <= n; i++) {
            printf("  %-10s %s\n", v[i].locale, v[i].err == ESP_OK ? "signature verifies" : esp_err_to_name(v[i].err));
            bad += v[i].err != ESP_OK;
        }
        printf("%u pack(s) checked, %d failed\n", (unsigned)n, bad);
        return bad == 0 ? 0 : 1;
    }
    printf("usage: lang | lang load <locale> | lang rm <locale> | lang fetch <locale> | lang verify\n");
    return 1;
}

/* ---- asset packs (docs/features/39 asset-packs.md "The panel", OPT-006) ---- */

static void job_asset_verify(void *arg)
{
    asset_verify_t *v = arg; /* ASSET_PACKS_KINDS + 1 entries: [0].err is the count */
    const size_t n = asset_packs_verify_all(v + 1, ASSET_PACKS_KINDS);
    v[0].err = (esp_err_t)n;
}

static int cmd_asset(int argc, char **argv)
{
    if (argc < 2) {
        asset_info_t list[ASSET_PACKS_KINDS];
        const size_t n = asset_packs_list(list, ASSET_PACKS_KINDS);
        if (n == 0) {
            printf("No pack store: every kind uses its built-in fallback.\n");
            return 0;
        }
        printf("Asset packs:\n");
        for (size_t i = 0; i < n; i++) {
            const asset_info_t *a = &list[i];
            printf("  %-12s %-8s %-13s", asset_kind_name(a->kind), a->name, asset_state_name(a->state));
            if (a->bytes != 0u) {
                printf(" version %-8s match 0x%08x %7u bytes%s", a->version[0] ? a->version : "?",
                       (unsigned)a->match, (unsigned)a->bytes, a->draft ? ", draft" : "");
            }
            printf("%s%s\n", a->why[0] ? ": " : "", a->why);
        }
        printf("Free in the store: %u bytes\n", (unsigned)asset_packs_free_bytes());
        return 0;
    }
    if (strcmp(argv[1], "verify") == 0 && argc == 2) {
        static EXT_RAM_BSS_ATTR asset_verify_t v[ASSET_PACKS_KINDS + 1];
        memset(v, 0, sizeof(v));
        if (!lang_packs_run_on_task(job_asset_verify, v)) {
            printf("The pack service is not running (no pack store).\n");
            return 1;
        }
        const size_t n = (size_t)v[0].err;
        int bad = 0;
        for (size_t i = 1; i <= n; i++) {
            printf("  %-12s %s\n", asset_kind_name(v[i].kind),
                   v[i].err == ESP_OK                    ? "signature, format and the kind's rules hold"
                   : v[i].err == ESP_ERR_INVALID_VERSION ? "made for another firmware version: not used"
                                                         : esp_err_to_name(v[i].err));
            bad += v[i].err != ESP_OK;
        }
        printf("%u asset pack(s) checked, %d failed\n", (unsigned)n, bad);
        return bad == 0 ? 0 : 1;
    }
    printf("usage: asset | asset verify\n");
    return 1;
}

esp_err_t lang_packs_console_register(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "lang",
        .help = "Language packs: list, load, rm, fetch, verify",
        .hint = "[load|rm|fetch <locale>|verify]",
        .func = cmd_lang,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register lang");
    static const esp_console_cmd_t asset = {
        .command = "asset",
        .help = "Asset packs (letters, picture, certificates): list, verify",
        .hint = "[verify]",
        .func = cmd_asset,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&asset), TAG, "register asset");
    return ESP_OK;
}

#else /* !CONFIG_ALARM_FEATURE_LANG_PACKS */

esp_err_t lang_packs_console_register(void)
{
    return ESP_OK;
}

#endif
