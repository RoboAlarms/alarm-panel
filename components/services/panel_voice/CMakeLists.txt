# panel_voice - the panel's voice (docs/features/08 voice-first-build.md): spoken zone names from
# a signed voice pack. The Opus adapter (voice_codec, D10) and the utterance renderer
# (voice_render, D11) are plain C11 that the host tests also build; the service (panel_voice.c:
# the pack copy and its leases, the decode worker, the policy's wiring, Preview, install, D12)
# and its console command are ESP-IDF. Optional: ALARM_FEATURE_VOICE; off, the service answers
# with stubs.
idf_component_register(SRCS "src/voice_codec.c" "src/voice_render.c" "src/panel_voice.c" "src/voice_console.c"
                       INCLUDE_DIRS "include"
                       REQUIRES voice_pack alarm_core esp_common
                       PRIV_REQUIRES esphome__micro-opus annunciator lang_packs pack_store platform_esp alarm_mem sounder
                                     bootloader_support console esp_timer freertos heap log mbedtls nvs_flash)
