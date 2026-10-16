# Host tests of the panel link (docs/features/11, LINK-NNN). The crypto port on the PC is
# zwave_host's test port (AES and X25519 from the standards, seeded random numbers).
set(PLINK_HOST_CRYPTO ${CMAKE_CURRENT_LIST_DIR}/../../zwave_host/test/zw_host_crypto.c)
set(PLINK_TEST_INC ${CMAKE_CURRENT_LIST_DIR}/../../zwave_host/test ${CMAKE_CURRENT_LIST_DIR})

# Each suite is added once its file exists, so the pieces can land one by one.
foreach(suite plink_crypto plink_frame plink_msg plink_view plink_store plink_pair plink_session
              plink_vfeed)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/test_${suite}.c)
        alarm_add_test(${suite} SOURCES test_${suite}.c plink_test_port.c ${PLINK_HOST_CRYPTO}
                       LIBS panel_link alarm_sim alarm_proto zwave_host unity
                       INCLUDES ${PLINK_TEST_INC} STRICT)
    endif()
endforeach()
# The managers, alone and together over a pretend network; the master side drives the real engine
# (alarm_sim, components/core/alarm_core/test).
foreach(suite plink_master plink_keypad)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/test_${suite}.c)
        alarm_add_test(${suite} SOURCES test_${suite}.c plink_test_port.c plink_test_net.c
                       ${PLINK_HOST_CRYPTO}
                       LIBS panel_link alarm_sim alarm_proto zwave_host unity
                       INCLUDES ${PLINK_TEST_INC})
    endif()
endforeach()
# Both together; a keypad's voice policy (annunciator) fed through plink_vfeed is compared with the
# master's, fed straight from the engine (LINK-028).
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/test_plink_link.c)
    alarm_add_test(plink_link SOURCES test_plink_link.c plink_test_port.c plink_test_net.c
                   ${PLINK_HOST_CRYPTO}
                   LIBS panel_link annunciator voice_pack alarm_sim alarm_proto zwave_host unity
                   INCLUDES ${PLINK_TEST_INC})
endif()
