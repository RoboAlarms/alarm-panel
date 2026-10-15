# Included by every ESP-IDF firmware project (firmware/<board>/CMakeLists.txt) before ESP-IDF's
# project.cmake. Set ALARM_BOARD (the project's folder name, e.g. crowpanel_p4) first, and for a
# board on a family library its family (ALARM_BOARD_FAMILY, cmake/alarm_idf.cmake) and the
# project its firmware is built on (ALARM_BOARD_BASE, below).
#
#   set(ALARM_BOARD <board>)
#   set(ALARM_BOARD_FAMILY <name>_family)   # optional
#   set(ALARM_BOARD_BASE <board>)           # optional
#   include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/alarm_firmware.cmake)
#   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
#   idf_build_set_property(MINIMAL_BUILD ON)
#   project(${ALARM_PROJECT_NAME})
#   alarm_check_build_profile()
#   alarm_optimize_hot_paths()   # optional: LVGL at -O2, for a board with flash to spare
#
# Gives the project its components (alarm_idf.cmake: the tiers, and of the boards only its own),
# its version (PROJECT_VER) and its signed project name per build profile (ALARM_PROJECT_NAME,
# alarm_<board>[-dev|-release]); alarm_check_build_profile() refuses a configuration that
# disagrees with the profile, and sets up the language packs and the image's build date.
# project() itself stays in the project's CMakeLists: CMake wants it called there literally.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/alarm_idf.cmake)

set(ALARM_FIRMWARE alarm_${ALARM_BOARD})

# A board whose firmware is another board's with a few changes (the CrowPanel 10.1" on the 7"'s:
# one main board) names that project in ALARM_BOARD_BASE and holds only what differs. Every
# sdkconfig.defaults fragment the build names (SDKCONFIG_DEFAULTS, from the wrappers) is read
# from the base first - all of them, in order - and then from this project where it has one, so
# this project's lines win. Its main/ uses the base's sources the same way
# (firmware/crowpanel_10/main/CMakeLists.txt).
if(ALARM_BOARD_BASE)
    set(ALARM_BASE_DIR ${ALARM_REPO}/firmware/${ALARM_BOARD_BASE})
    if(NOT EXISTS ${ALARM_BASE_DIR}/sdkconfig.defaults)
        message(FATAL_ERROR "ALARM_BOARD_BASE ${ALARM_BOARD_BASE}: no firmware project at "
                            "${ALARM_BASE_DIR}")
    endif()
    set(alarm_fragments ${SDKCONFIG_DEFAULTS})
    if(NOT alarm_fragments)
        set(alarm_fragments sdkconfig.defaults)
    endif()
    set(alarm_stacked)
    foreach(alarm_dir ${ALARM_BASE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
        foreach(alarm_fragment ${alarm_fragments})
            if(IS_ABSOLUTE ${alarm_fragment})
                message(FATAL_ERROR "SDKCONFIG_DEFAULTS names fragments by file name on a board "
                                    "with ALARM_BOARD_BASE: ${alarm_fragment}")
            endif()
            if(EXISTS ${alarm_dir}/${alarm_fragment})
                list(APPEND alarm_stacked ${alarm_dir}/${alarm_fragment})
            endif()
        endforeach()
    endforeach()
    # A normal variable shadows the wrapper's cache entry; ESP-IDF's project() reads this one.
    set(SDKCONFIG_DEFAULTS ${alarm_stacked})
endif()

# Firmware version: the VERSION file at the repo root, or ALARM_FW_VERSION from the environment
# (test builds, e.g. 0.6.1-test). Semantic versions only: OTA compares them (docs/features/13).
set(ALARM_VERSION_FILE ${ALARM_REPO}/VERSION)
if(DEFINED ENV{ALARM_FW_VERSION} AND NOT "$ENV{ALARM_FW_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{ALARM_FW_VERSION}")
else()
    file(STRINGS ${ALARM_VERSION_FILE} PROJECT_VER LIMIT_COUNT 1)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ALARM_VERSION_FILE})

# Bench fault images (docs/features/13 "Still to run on the bench"): ALARM_BENCH_FAULT in the
# environment breaks one part of the panel on purpose, so the update's self-test and the health
# check can be seen to catch it. Only a test version with "-fault" in it may carry one, so a
# fault can never slip into an image anyone installs for real:
#   no_zone_service  the zone sources are never started
#   no_touch         every read of the touch controller fails, as if it were unplugged
#   zones_stop       the zone service's task stops three minutes after the start
set(ALARM_BENCH_FAULT "$ENV{ALARM_BENCH_FAULT}")
if(NOT ALARM_BENCH_FAULT STREQUAL "")
    if(NOT PROJECT_VER MATCHES "-fault")
        message(FATAL_ERROR "ALARM_BENCH_FAULT=${ALARM_BENCH_FAULT} needs a test version with "
                            "-fault in it (-Version x.y.z-fault.<name>)")
    endif()
    if(NOT ALARM_BENCH_FAULT MATCHES "^(no_zone_service|no_touch|zones_stop)$")
        message(FATAL_ERROR "ALARM_BENCH_FAULT must be no_zone_service, no_touch or zones_stop")
    endif()
    string(TOUPPER "${ALARM_BENCH_FAULT}" ALARM_BENCH_FAULT_UPPER)
    add_compile_definitions(ALARM_BENCH_FAULT_${ALARM_BENCH_FAULT_UPPER}=1)
    message(WARNING "Bench fault image ${PROJECT_VER} (${ALARM_BENCH_FAULT}): for a bench panel "
                    "only, never for one protecting a house")
elseif(PROJECT_VER MATCHES "-fault")
    message(FATAL_ERROR "Version ${PROJECT_VER} says -fault but ALARM_BENCH_FAULT is not set")
endif()

# The project name is inside esp_app_desc_t and covered by the app signature, so a panel accepts
# only images of its own board and profile: an explicit release cannot accept a development or
# legacy image even when forced (docs/features/30).
set(ALARM_PROJECT_NAME ${ALARM_FIRMWARE})
if("$ENV{ALARM_BUILD_PROFILE}" STREQUAL "dev")
    set(ALARM_PROJECT_NAME ${ALARM_FIRMWARE}-dev)
elseif("$ENV{ALARM_BUILD_PROFILE}" STREQUAL "release")
    set(ALARM_PROJECT_NAME ${ALARM_FIRMWARE}-release)
endif()

# After project(), on a board with flash to spare (the P4): the image is built for size
# (CONFIG_COMPILER_OPTIMIZATION_SIZE), but LVGL's software drawing is where the panel spends
# its time, so LVGL alone is built for speed. The last -O on the command line wins.
macro(alarm_optimize_hot_paths)
    idf_component_get_property(alarm_lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_options(${alarm_lvgl_lib} PRIVATE -O2)
    if(CONFIG_COMPILER_SAVE_RESTORE_LIBCALLS)
        # The shared save/restore code costs a call on every function: drawing keeps it inline.
        target_compile_options(${alarm_lvgl_lib} PRIVATE -mno-save-restore)
    endif()
endmacro()

# After project(): the configuration and the wrapper's profile agree, and a release carries no
# developer or diagnostic access, and no draft language (docs/features/37 I18N-005: the
# generated catalog table leaves a draft out when I18N_NO_DRAFTS is defined).
macro(alarm_check_build_profile)
    if(CONFIG_ALARM_BUILD_RELEASE)
        if(NOT "$ENV{ALARM_BUILD_PROFILE}" STREQUAL "release" OR CONFIG_ALARM_BUILD_DEV OR
           CONFIG_ALARM_CRYPTO_DIAGNOSTICS OR
           CONFIG_HA_LINK_DIAGNOSTICS OR CONFIG_ALARM_SERVICE_CONSOLE)
            message(FATAL_ERROR "Contaminated release profile: developer/diagnostic access forbidden")
        endif()
        if(TARGET idf::i18n)
            idf_component_get_property(alarm_i18n_lib i18n COMPONENT_LIB)
            target_compile_definitions(${alarm_i18n_lib} PRIVATE I18N_NO_DRAFTS)
        endif()
    elseif(CONFIG_ALARM_BUILD_DEV)
        if(NOT "$ENV{ALARM_BUILD_PROFILE}" STREQUAL "dev")
            message(FATAL_ERROR "Development sdkconfig requires --profile dev / -Profile dev")
        endif()
    elseif(NOT "$ENV{ALARM_BUILD_PROFILE}" STREQUAL "" AND
           NOT "$ENV{ALARM_BUILD_PROFILE}" STREQUAL "legacy")
        message(FATAL_ERROR "Profile and sdkconfig disagree; use an isolated configuration")
    endif()
    alarm_language_packs()
    alarm_asset_packs()
    alarm_stamp_build_date()
endmacro()

# The image's own build date and time (BL-176), which System details shows. ESP-IDF writes
# __DATE__ and __TIME__ into esp_app_desc when it compiles esp_app_desc.c, and an incremental
# build compiles that only when its own inputs change: an image rebuilt after a change elsewhere
# kept an older build's date. Making that object depend on the other components' archives would
# be a dependency cycle (Ninja orders most archives after esp_app_format's), so the date is
# written into the ELF right after each link instead (tools/stamp_app_desc.py), before elf2image
# hashes the ELF and makes the image. A build that changes nothing doesn't link, so it keeps its
# date. Without CONFIG_APP_COMPILE_TIME_DATE (a reproducible build turns it off) the image has no
# date and nothing is stamped. Called by alarm_check_build_profile().
macro(alarm_stamp_build_date)
    if(CONFIG_APP_COMPILE_TIME_DATE AND NOT CONFIG_IDF_TARGET_LINUX)
        idf_build_get_property(alarm_stamp_python PYTHON)
        idf_build_get_property(alarm_stamp_elf EXECUTABLE)
        add_custom_command(TARGET ${alarm_stamp_elf} POST_BUILD
            COMMAND ${alarm_stamp_python} ${ALARM_REPO}/tools/stamp_app_desc.py
                    $<TARGET_FILE:${alarm_stamp_elf}>
            VERBATIM)
        # A changed script links (and so stamps) again.
        set_property(TARGET ${alarm_stamp_elf} APPEND PROPERTY LINK_DEPENDS
                     ${ALARM_REPO}/tools/stamp_app_desc.py)
    endif()
endmacro()

# Language packs (docs/features/37 language-packs.md): without built-in catalogs the i18n
# component's generated table is empty (I18N_NO_BUILTIN, I18N-039), and with ALARM_FEATURE_LANG_PACKS
# the build makes every language's signed pack into <build>/lang/ with the image's signing key
# (tools/i18n.py pack), a release leaving drafts out. Called by alarm_check_build_profile().
macro(alarm_language_packs)
    if(NOT CONFIG_ALARM_I18N_BUILTIN AND TARGET idf::i18n)
        idf_component_get_property(alarm_i18n_lib i18n COMPONENT_LIB)
        target_compile_definitions(${alarm_i18n_lib} PRIVATE I18N_NO_BUILTIN)
    endif()
    if(CONFIG_ALARM_FEATURE_LANG_PACKS AND CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)
        idf_build_get_property(alarm_pack_python PYTHON)
        # The IDF build resolves the signing key against the firmware project's folder.
        get_filename_component(alarm_pack_key "${CONFIG_SECURE_BOOT_SIGNING_KEY}" ABSOLUTE
                               BASE_DIR "${PROJECT_SOURCE_DIR}")
        set(alarm_pack_args --version ${PROJECT_VER} --out ${CMAKE_BINARY_DIR}/lang
                            --key ${alarm_pack_key})
        if(CONFIG_ALARM_BUILD_RELEASE)
            list(APPEND alarm_pack_args --release)
        endif()
        file(GLOB alarm_pack_po CONFIGURE_DEPENDS ${ALARM_REPO}/components/core/i18n/po/*)
        set(alarm_pack_stamp ${CMAKE_BINARY_DIR}/lang/.packs.stamp)
        add_custom_command(OUTPUT ${alarm_pack_stamp}
            COMMAND ${alarm_pack_python} ${ALARM_REPO}/tools/i18n.py pack ${alarm_pack_args}
            COMMAND ${CMAKE_COMMAND} -E touch ${alarm_pack_stamp}
            DEPENDS ${alarm_pack_po} ${ALARM_REPO}/tools/i18n.py ${alarm_pack_key}
            VERBATIM)
        add_custom_target(alarm_lang_packs ALL DEPENDS ${alarm_pack_stamp})
    endif()
endmacro()

# Asset packs (docs/features/39 asset-packs.md "Building packs", OPT-006): with
# ALARM_FEATURE_LANG_PACKS and signed binaries, the build makes each kind's signed pack into
# <build>/assets/ with the image's signing key (tools/asset_pack.py; a draft unless
# ALARM_BUILD_RELEASE), version PROJECT_VER, from committed inputs:
#   letters.roboasset  the layout's fonts/letters/ (tools/lvpro_gen.py --fonts makes them):
#                      letters.json's fingerprint as `match`, each role's .bin as a part; only a
#                      layout that has letters.json gets one
#   saver.roboasset    the saver's photo, components/ui/alarm_ui/images/saver_forest.jpg
#   certs.roboasset    the full certificate list as one `bundle` part: ESP-IDF's
#                      gen_crt_bundle.py with the inputs and options of ESP-IDF's DEFAULT_FULL
#                      branch (components/mbedtls/CMakeLists.txt), so the pack holds exactly what
#                      the image embedded before it kept only the common list (OPT-013)
# Called by alarm_check_build_profile().
macro(alarm_asset_packs)
    if(CONFIG_ALARM_FEATURE_LANG_PACKS AND CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)
        idf_build_get_property(alarm_asset_python PYTHON)
        get_filename_component(alarm_asset_key "${CONFIG_SECURE_BOOT_SIGNING_KEY}" ABSOLUTE
                               BASE_DIR "${PROJECT_SOURCE_DIR}")
        set(alarm_asset_dir ${CMAKE_BINARY_DIR}/assets)
        set(alarm_asset_tool ${ALARM_REPO}/tools/asset_pack.py)
        set(alarm_asset_tools ${alarm_asset_tool} ${ALARM_REPO}/tools/flasher/roboflash/packstore.py)
        set(alarm_asset_common --version ${PROJECT_VER} --out ${alarm_asset_dir} --key ${alarm_asset_key})
        if(NOT CONFIG_ALARM_BUILD_RELEASE)
            list(APPEND alarm_asset_common --draft)
        endif()
        set(alarm_asset_outputs)
        # The layout the board names (as components/ui/alarm_ui picks it).
        set(alarm_asset_layout "1024x600")
        if(DEFINED CONFIG_ALARM_UI_LAYOUT AND NOT "${CONFIG_ALARM_UI_LAYOUT}" STREQUAL "")
            set(alarm_asset_layout "${CONFIG_ALARM_UI_LAYOUT}")
        endif()
        set(alarm_letters_dir
            ${ALARM_REPO}/components/ui/alarm_ui/layouts/${alarm_asset_layout}/alarm_ui/fonts/letters)
        if(EXISTS ${alarm_letters_dir}/letters.json)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${alarm_letters_dir}/letters.json)
            file(READ ${alarm_letters_dir}/letters.json alarm_letters_json)
            string(JSON alarm_letters_match GET "${alarm_letters_json}" fingerprint)
            # The pack service compares an installed letters pack with this image's fonts when it
            # fetches (OPT-007): the fingerprint this layout's fonts were generated with.
            if(TARGET idf::lang_packs)
                idf_component_get_property(alarm_lang_packs_lib lang_packs COMPONENT_LIB)
                target_compile_definitions(${alarm_lang_packs_lib} PRIVATE
                                           ALARM_LETTERS_FINGERPRINT=${alarm_letters_match}u)
            endif()
            set(alarm_letters_parts)
            set(alarm_letters_inputs ${alarm_letters_dir}/letters.json)
            # The roles in globals.xml's order (asset-packs.md "The kinds"): the part table's order.
            foreach(alarm_role caption body label title)
                string(JSON alarm_role_file ERROR_VARIABLE alarm_role_missing
                       GET "${alarm_letters_json}" fonts ${alarm_role})
                if(alarm_role_missing)
                    message(FATAL_ERROR "${alarm_letters_dir}/letters.json has no `${alarm_role}` font: "
                                        "run tools/lvpro_gen.py")
                endif()
                list(APPEND alarm_letters_parts --part ${alarm_role}=font:${alarm_letters_dir}/${alarm_role_file})
                list(APPEND alarm_letters_inputs ${alarm_letters_dir}/${alarm_role_file})
            endforeach()
            add_custom_command(OUTPUT ${alarm_asset_dir}/letters.roboasset
                COMMAND ${alarm_asset_python} ${alarm_asset_tool} build --kind letters ${alarm_asset_common}
                        --match ${alarm_letters_match} ${alarm_letters_parts}
                DEPENDS ${alarm_letters_inputs} ${alarm_asset_tools} ${alarm_asset_key}
                VERBATIM)
            list(APPEND alarm_asset_outputs ${alarm_asset_dir}/letters.roboasset)
        endif()
        set(alarm_saver_photo ${ALARM_REPO}/components/ui/alarm_ui/images/saver_forest.jpg)
        add_custom_command(OUTPUT ${alarm_asset_dir}/saver.roboasset
            COMMAND ${alarm_asset_python} ${alarm_asset_tool} build --kind picture ${alarm_asset_common}
                    --part photo=jpeg:${alarm_saver_photo}
            DEPENDS ${alarm_saver_photo} ${alarm_asset_tools} ${alarm_asset_key}
            VERBATIM)
        list(APPEND alarm_asset_outputs ${alarm_asset_dir}/saver.roboasset)
        # The certificates pack (kind 5, `certs`): the bundle IDF's DEFAULT_FULL would embed. Keep
        # this in step with components/mbedtls/CMakeLists.txt of the IDF in use.
        if(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
            idf_build_get_property(alarm_idf_path IDF_PATH)
            set(alarm_crt_dir ${alarm_idf_path}/components/mbedtls/esp_crt_bundle)
            set(alarm_crt_paths ${alarm_crt_dir}/cacrt_all.pem)
            if(NOT CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY)
                list(APPEND alarm_crt_paths ${alarm_crt_dir}/cacrt_local.pem)
            endif()
            if(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEPRECATED_LIST)
                list(APPEND alarm_crt_paths ${alarm_crt_dir}/cacrt_deprecated.pem)
            endif()
            if(CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE)
                get_filename_component(alarm_crt_custom ${CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH}
                                       ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
                list(APPEND alarm_crt_paths ${alarm_crt_custom})
            endif()
            # gen_crt_bundle.py writes x509_crt_bundle into its working folder.
            set(alarm_crt_work ${CMAKE_BINARY_DIR}/assets_certs)
            add_custom_command(OUTPUT ${alarm_asset_dir}/certs.roboasset
                COMMAND ${CMAKE_COMMAND} -E make_directory ${alarm_crt_work}
                COMMAND ${CMAKE_COMMAND} -E chdir ${alarm_crt_work}
                        ${alarm_asset_python} ${alarm_crt_dir}/gen_crt_bundle.py
                        --input ${alarm_crt_paths} -q
                        --max-certs ${CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS}
                COMMAND ${alarm_asset_python} ${alarm_asset_tool} build --kind certificates ${alarm_asset_common}
                        --part bundle=certs:${alarm_crt_work}/x509_crt_bundle
                DEPENDS ${alarm_crt_paths} ${alarm_crt_dir}/gen_crt_bundle.py ${alarm_asset_tools}
                        ${alarm_asset_key}
                VERBATIM)
            list(APPEND alarm_asset_outputs ${alarm_asset_dir}/certs.roboasset)
        endif()
        add_custom_target(alarm_asset_packs ALL DEPENDS ${alarm_asset_outputs})
    endif()
endmacro()
