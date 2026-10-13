# Fork-only: AudioOut2 on the x86_64 Rosetta build. audio_out2_pacing (AudioOut2Pacing.cpp) checks output
# pacing with fake guests; the library's own AudioOut2 tests run here too, since the Rosetta build is the
# one that builds libSceAudioOut on macOS. All play through SDL's disk driver.
set(AUDIO_OUT2_TEST_SOURCES
    audio_out2_pacing ${CMAKE_CURRENT_SOURCE_DIR}/AudioOut2Pacing.cpp
    audio_out2_timing ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libSceAudioOut/tests/AudioOut2Timing.cpp
    audio_out2_port_buffers ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libSceAudioOut/tests/AudioOut2PortBuffers.cpp
    audio_out2_port_layouts ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libSceAudioOut/tests/AudioOut2PortLayouts.cpp
    audio_out2_latency ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libSceAudioOut/tests/AudioOut2Latency.cpp)
add_custom_target(audio_out2_rosetta_tests)
while(AUDIO_OUT2_TEST_SOURCES)
    list(POP_FRONT AUDIO_OUT2_TEST_SOURCES TEST_NAME TEST_SOURCE)
    add_test_executable(${TEST_NAME}_tests ${TEST_SOURCE})
    add_dependencies(audio_out2_rosetta_tests ${TEST_NAME}_tests)
    target_include_directories(${TEST_NAME}_tests PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT})
    target_link_libraries(${TEST_NAME}_tests PRIVATE libSceAudioOut libc)
    set_target_properties(${TEST_NAME}_tests PROPERTIES BUILD_RPATH ${UNPATCHED_DIR})
    add_test(NAME ${TEST_NAME} COMMAND arch -x86_64 $<TARGET_FILE:${TEST_NAME}_tests>)
    set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endwhile()
