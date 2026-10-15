# pack_store - the table of blobs (language packs, asset packs) in the assets partition (docs/features/37
# language-packs.md). Portable C11, no dependencies.
set(PACK_STORE_SRCS
    src/pack_store.c
    src/asset_pack.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${PACK_STORE_SRCS}
                           INCLUDE_DIRS include
                           PRIV_INCLUDE_DIRS src)
else()
    add_library(pack_store STATIC ${PACK_STORE_SRCS})
    target_include_directories(pack_store PUBLIC include PRIVATE src)
    target_compile_features(pack_store PUBLIC c_std_11)
    set_target_properties(pack_store PROPERTIES C_EXTENSIONS OFF)
endif()
