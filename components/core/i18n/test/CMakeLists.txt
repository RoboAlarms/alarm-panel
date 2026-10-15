# Lookup, plurals, the {n} formatter and the UTF-8 helpers (I18N-NNN, docs/features/37).
# test_i18n.c also compiles the catalog table as a release build does (I18N-005), which needs
# the private header it includes.
alarm_add_test(i18n SOURCES test_i18n.c LIBS i18n unity INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# Regions, dates and numbers (I18N-016, I18N-017).
alarm_add_test(i18n_date SOURCES test_i18n_date.c LIBS i18n unity)
# The language pack reader: the frozen fixture, every truncation and mutation (I18N-040, I18N-043).
alarm_add_test(i18n_pack SOURCES test_i18n_pack.c LIBS i18n unity)
