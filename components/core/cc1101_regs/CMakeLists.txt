# cc1101_regs - the TI CC1101 as data and arithmetic (docs/features/25, RF-017): the asynchronous OOK
# receive table for a band, the frequency word, RSSI, identification, read-back verification and
# reset detection. Portable C11; components/drivers/rf_rx does the SPI with it.
set(CC1101_REGS_SRCS
    src/cc1101_regs.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${CC1101_REGS_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(cc1101_regs STATIC ${CC1101_REGS_SRCS})
    target_include_directories(cc1101_regs PUBLIC include)
    target_compile_features(cc1101_regs PUBLIC c_std_11)
    set_target_properties(cc1101_regs PROPERTIES C_EXTENSIONS OFF)
endif()
