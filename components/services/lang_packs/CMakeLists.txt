# lang_packs - language packs: the pack store on the assets partition, signed packs loaded into
# PSRAM, the registry the screen and the Panel Manager read, installing, removing and downloading
# (docs/features/37 language-packs.md). Optional: ALARM_FEATURE_LANG_PACKS; off, it answers with
# stubs (English and the built-in catalogs only). pack_assets is the one owner of the assets store
# for language and voice packs (docs/features/08 voice-first-build.md, AUD-027): built when either
# option is on. asset_packs loads the asset packs (letters, picture, certificates: docs/features/39
# asset-packs.md) from the same store, with ALARM_FEATURE_LANG_PACKS.
idf_component_register(SRCS "src/lang_packs.c" "src/lang_console.c" "src/pack_assets.c" "src/asset_packs.c"
                       INCLUDE_DIRS "include"
                       REQUIRES i18n pack_store
                       PRIV_REQUIRES platform_esp panel_ota alarm_core alarm_proto console
                                     esp_partition esp_app_format heap log freertos
                                     mbedtls bootloader_support esp_rom)
