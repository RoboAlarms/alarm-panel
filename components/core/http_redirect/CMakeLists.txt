# http_redirect - where a plain-HTTP request to the panel is sent: the same page over HTTPS, with
# the Host header repeated only when it is plainly a host (docs/features/31 WEB-023). Portable C11;
# components/services/panel_web runs the port-80 listener with it.
set(HTTP_REDIRECT_SRCS
    src/http_redirect.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${HTTP_REDIRECT_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(http_redirect STATIC ${HTTP_REDIRECT_SRCS})
    target_include_directories(http_redirect PUBLIC include)
    target_compile_features(http_redirect PUBLIC c_std_11)
    set_target_properties(http_redirect PROPERTIES C_EXTENSIONS OFF)
endif()
