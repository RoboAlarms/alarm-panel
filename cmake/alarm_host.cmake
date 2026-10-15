# Shared by every PC build of the project: the host tests (host/), the UI preview
# (host/ui_preview) and the interactive simulator (host/ui_sim).
#
# One way to build the portable components (components/core) for the PC, one set of warning
# flags, one way to declare a test. A component's tests live in its own test/ folder and are
# found here, so adding or dropping a component takes its tests with it (ADR-038).
include_guard(GLOBAL)

set(ALARM_REPO ${CMAKE_CURRENT_LIST_DIR}/.. CACHE INTERNAL "")
get_filename_component(ALARM_REPO ${ALARM_REPO} ABSOLUTE)
set(ALARM_COMPONENTS_DIR ${ALARM_REPO}/components)

# The portable components' own flags (CONTRIBUTING.md: warning-free with -Wconversion -Werror).
set(ALARM_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
                   -Wmissing-prototypes -Wformat=2 -Werror)
# Test harnesses and executables: strict, but they may convert freely.
set(ALARM_TEST_WARNINGS -Wall -Wextra -Werror)
set(ALARM_SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(ALARM_GNU_LIKE TRUE)
else()
    set(ALARM_GNU_LIKE FALSE)
endif()

# Every portable component, by folder name (= its library target name).
function(alarm_core_component_names out)
    file(GLOB entries LIST_DIRECTORIES true ${ALARM_COMPONENTS_DIR}/core/*)
    set(names)
    foreach(entry ${entries})
        if(IS_DIRECTORY ${entry} AND EXISTS ${entry}/CMakeLists.txt)
            get_filename_component(name ${entry} NAME)
            list(APPEND names ${name})
        endif()
    endforeach()
    list(SORT names)
    set(${out} ${names} PARENT_SCOPE)
endfunction()

# alarm_add_core_components([WARNINGS] [SANITIZE])
#   Adds every portable component as a static library named after its folder.
#   WARNINGS applies ALARM_WARNINGS to each library, SANITIZE the address/undefined
#   sanitizers. Both are applied per target: a directory-wide option set here would not reach
#   targets created inside the components' own directories (R19).
function(alarm_add_core_components)
    cmake_parse_arguments(ARG "WARNINGS;SANITIZE" "" "" ${ARGN})
    alarm_core_component_names(names)
    foreach(name ${names})
        if(NOT TARGET ${name})
            add_subdirectory(${ALARM_COMPONENTS_DIR}/core/${name} ${CMAKE_BINARY_DIR}/components/${name})
        endif()
        get_target_property(type ${name} TYPE)
        if(ALARM_GNU_LIKE AND NOT type STREQUAL "INTERFACE_LIBRARY")
            if(ARG_WARNINGS)
                target_compile_options(${name} PRIVATE ${ALARM_WARNINGS})
            endif()
            if(ARG_SANITIZE)
                target_compile_options(${name} PRIVATE ${ALARM_SANITIZE_FLAGS})
            endif()
        endif()
    endforeach()
endfunction()

# alarm_add_test(<suite> SOURCES <files...> LIBS <targets...> [STRICT])
#   One Unity test executable test_<suite>, registered with CTest as <suite>. STRICT holds the
#   test to the portable components' own warning set instead of the harness one.
function(alarm_add_test suite)
    cmake_parse_arguments(ARG "STRICT" "" "SOURCES;LIBS;INCLUDES" ${ARGN})
    add_executable(test_${suite} ${ARG_SOURCES})
    # Every test executable lands in the build folder itself (build/host/test_<suite>), where
    # the frozen fixtures' regeneration notes expect it (DUMP_FIXTURE=1 build/host/test_codec).
    set_target_properties(test_${suite} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_link_libraries(test_${suite} PRIVATE ${ARG_LIBS})
    if(ARG_INCLUDES)
        target_include_directories(test_${suite} PRIVATE ${ARG_INCLUDES})
    endif()
    if(ALARM_GNU_LIKE)
        if(ARG_STRICT)
            target_compile_options(test_${suite} PRIVATE ${ALARM_WARNINGS})
        else()
            target_compile_options(test_${suite} PRIVATE ${ALARM_TEST_WARNINGS})
        endif()
    endif()
    add_test(NAME ${suite} COMMAND test_${suite})
endfunction()

# alarm_add_component_tests()
#   Adds components/<tier>/<name>/test/CMakeLists.txt for every component that has one.
function(alarm_add_component_tests)
    file(GLOB test_lists ${ALARM_COMPONENTS_DIR}/*/*/test/CMakeLists.txt)
    list(SORT test_lists)
    foreach(list_file ${test_lists})
        get_filename_component(test_dir ${list_file} DIRECTORY)
        get_filename_component(component_dir ${test_dir} DIRECTORY)
        get_filename_component(name ${component_dir} NAME)
        add_subdirectory(${test_dir} ${CMAKE_BINARY_DIR}/tests/${name})
    endforeach()
endfunction()
