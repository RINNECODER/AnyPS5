# Fork-only: surround Opus through libSceAjm.native (AjmOpusSurround.cpp).
add_test_executable(ajm_opus_surround_tests AjmOpusSurround.cpp)
target_include_directories(ajm_opus_surround_tests PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT})
target_link_libraries(ajm_opus_surround_tests PRIVATE libSceAjm.native libc)
set_target_properties(ajm_opus_surround_tests PROPERTIES BUILD_RPATH ${UNPATCHED_DIR})
add_test(NAME ajm_opus_surround COMMAND arch -x86_64 $<TARGET_FILE:ajm_opus_surround_tests>)
set_tests_properties(ajm_opus_surround PROPERTIES TIMEOUT 60)
