# Fork-only: a key in the Rosetta direct path's native window reaches the pad (DirectInputTest.mm).
add_test_executable(rosetta_direct_input_tests DirectInputTest.mm)
target_compile_options(rosetta_direct_input_tests PRIVATE -fobjc-arc)
target_include_directories(rosetta_direct_input_tests PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT})
target_link_libraries(rosetta_direct_input_tests PRIVATE libSceVideoOut libScePad libkernel libc "${ANYPS5_ROSETTA_APPKIT}")
set_target_properties(rosetta_direct_input_tests PROPERTIES BUILD_RPATH ${UNPATCHED_DIR})
add_dependencies(rosetta_direct_input_tests rosetta_metallib)
add_test(NAME rosetta_direct_input COMMAND arch -x86_64 $<TARGET_FILE:rosetta_direct_input_tests>)
set_tests_properties(rosetta_direct_input PROPERTIES TIMEOUT 60
    ENVIRONMENT "ANYPS5_METAL_UTILITY_LIBRARY=${PATCHED_DIR}/AnyPS5Utilities.metallib")
