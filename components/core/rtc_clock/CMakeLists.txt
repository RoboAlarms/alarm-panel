# rtc_clock - the DS3231 real-time clock as data (BCD time, identification, the control values
# for a clock on its coin cell) and the clock keeper: which clock the panel believes, and when the
# module is read, written or followed (docs/features/35). Portable C11; components/drivers/rtc_ds3231
# does the I2C and components/services/panel_clock runs it.
set(RTC_CLOCK_SRCS
    src/ds3231.c
    src/clock_keeper.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${RTC_CLOCK_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(rtc_clock STATIC ${RTC_CLOCK_SRCS})
    target_include_directories(rtc_clock PUBLIC include)
    target_compile_features(rtc_clock PUBLIC c_std_11)
    set_target_properties(rtc_clock PROPERTIES C_EXTENSIONS OFF)
endif()
