# The table format, mount, first fit, commits, power cuts after every step and the frozen table
# fixture (I18N-044, docs/features/37); the envelope and the install transaction (AUD-027).
alarm_add_test(pack_store SOURCES test_pack_store.c LIBS pack_store unity)
# The asset pack reader, RAST format 1, and its frozen fixture (OPT-012, docs/features/39
# asset-packs.md).
alarm_add_test(asset_pack SOURCES test_asset_pack.c LIBS pack_store unity)
