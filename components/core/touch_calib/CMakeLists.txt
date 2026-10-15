# touch_calib - resistive touch: calibration (raw -> screen) and filtering (pressure, median,
# settling, release debounce, dead band) for the 4" boards' XPT2046 (docs/features/19 CYD-008/009).
# Portable C11, no heap: the host tests and the board drivers use it.
set(TOUCH_CALIB_SRCS
    src/touch_calib.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${TOUCH_CALIB_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(touch_calib STATIC ${TOUCH_CALIB_SRCS})
    target_include_directories(touch_calib PUBLIC include)
    target_compile_features(touch_calib PUBLIC c_std_11)
    set_target_properties(touch_calib PROPERTIES C_EXTENSIONS OFF)
endif()
