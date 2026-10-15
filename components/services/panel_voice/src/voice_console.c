// voice_console.c - the `voice` service console command (docs/features/08 voice-first-build.md).
// Plain English: the console is never translated.
//
//   voice                     the state, the pack, the counters and the speech ring's underruns
//   voice preview <zone>      preview a zone's saved spoken name (as the editor's Preview does)
//   voice say <clip-id>...    preview raw clip IDs, hex (0x8001) or decimal, up to 8
#include "panel_voice.h"

#include "sdkconfig.h"

#if CONFIG_ALARM_FEATURE_VOICE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_service.h"
#include "annunciator/annunciator.h"
#include "esp_check.h"
#include "esp_console.h"
#include "panel_voice_internal.h"
#include "sounder.h"

static const char *TAG = "voice_con";

static void print_status(void)
{
    panel_voice_status_t st;
    panel_voice_status(&st);
    printf("Voice: %s%s%s\n", pv_state_name(st.state), st.why[0] ? " - " : "", st.why);
    if (st.state == PANEL_VOICE_READY) {
        printf("  pack:     %s %s, revision %u, %u clips, %u bytes%s\n", st.locale, st.voice_id,
               (unsigned)st.pack_revision, (unsigned)st.clips, (unsigned)st.file_bytes, st.draft ? ", DRAFT" : "");
    }
    printf("  speaks:   Voice %u %%, %u ms between words, announcements %s\n", (unsigned)st.volume,
           (unsigned)st.word_gap_ms, st.enabled ? "on" : "off");
    printf("  spoken:   %u started, %u completed, %u cancelled, %u underruns, %u decode errors\n",
           (unsigned)st.utterances, (unsigned)st.completed, (unsigned)st.cancelled, (unsigned)st.underruns,
           (unsigned)st.decode_errors);
    sounder_speech_status_t ss;
    sounder_speech_status(&ss);
    printf("  ring:     %u Hz, %u samples free, %u underruns, %u refused\n", (unsigned)ss.rate_hz,
           (unsigned)ss.free_samples, (unsigned)ss.underruns, (unsigned)ss.rejected);
}

static int preview_zone(const char *arg)
{
    char *end = NULL;
    const unsigned long zone = strtoul(arg, &end, 10);
    if (end == arg || *end != '\0' || zone == 0u || zone > ALARM_MAX_ZONES) {
        printf("No such zone: %s\n", arg);
        return 1;
    }
    alarm_config_t *cfg = alarm_mem_slow_malloc(sizeof(*cfg));
    if (cfg == NULL || alarm_service_get_config(cfg) != ESP_OK) {
        free(cfg);
        printf("The configuration could not be read\n");
        return 1;
    }
    const alarm_zone_config_t *zc = &cfg->zones[zone - 1u];
    uint16_t ids[ALARM_ZONE_WORDS];
    const unsigned n = alarm_zone_word_count(zc);
    for (unsigned i = 0u; i < n && i < ALARM_ZONE_WORDS; i++) {
        ids[i] = alarm_zone_word(zc, i);
    }
    free(cfg);
    if (n == 0u) {
        printf("Zone %lu has no spoken name\n", zone);
        return 1;
    }
    const panel_voice_preview_t r = panel_voice_preview(ids, n);
    printf("Preview of zone %lu: %s\n", zone, pv_preview_name(r));
    return r == PANEL_VOICE_PREVIEW_OK ? 0 : 1;
}

static int say(int argc, char **argv)
{
    uint16_t ids[ANN_CLIPS_MAX];
    if (argc < 1 || argc > (int)ANN_CLIPS_MAX) {
        printf("Usage: voice say <clip-id>... (1 to %u IDs, hex 0x8001 or decimal)\n", (unsigned)ANN_CLIPS_MAX);
        return 1;
    }
    for (int i = 0; i < argc; i++) {
        char *end = NULL;
        const unsigned long id = strtoul(argv[i], &end, 0);
        if (end == argv[i] || *end != '\0' || id == 0u || id > 0xFFFFu) {
            printf("Not a clip ID: %s\n", argv[i]);
            return 1;
        }
        ids[i] = (uint16_t)id;
    }
    const panel_voice_preview_t r = pv_preview_raw(ids, (size_t)argc);
    printf("Say: %s\n", pv_preview_name(r));
    return r == PANEL_VOICE_PREVIEW_OK ? 0 : 1;
}

static int cmd_voice(int argc, char **argv)
{
    if (argc == 1) {
        print_status();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "preview") == 0) {
        return preview_zone(argv[2]);
    }
    if (argc >= 3 && strcmp(argv[1], "say") == 0) {
        return say(argc - 2, argv + 2);
    }
    printf("Usage: voice | voice preview <zone> | voice say <clip-id>...\n");
    return 1;
}

esp_err_t panel_voice_console_register(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "voice",
        .help = "Voice: status, preview a zone's spoken name, say clip IDs",
        .hint = "[preview <zone>|say <clip-id>...]",
        .func = cmd_voice,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register voice");
    return ESP_OK;
}

#else /* !CONFIG_ALARM_FEATURE_VOICE */

esp_err_t panel_voice_console_register(void)
{
    return ESP_OK;
}

#endif
