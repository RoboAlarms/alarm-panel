# The pulse decoders on frames the encoders make, sent through a model of the air and the
# receiver (RF-NNN, docs/features/25), and every capture in rf/ (the bench's `rf capture` or the
# Flasher's "Record 3 seconds", with an "# expect:" line). A new capture needs a re-configure.
alarm_add_test(rf_decode SOURCES test_rf_decode.c LIBS rf_decode unity)
file(GLOB RF_CAPTURES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/rf ${CMAKE_CURRENT_SOURCE_DIR}/rf/*.txt)
list(SORT RF_CAPTURES)
list(JOIN RF_CAPTURES "," RF_CAPTURE_LIST)
target_compile_definitions(test_rf_decode PRIVATE
    RF_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/rf"
    RF_FIXTURE_FILES="${RF_CAPTURE_LIST}")
