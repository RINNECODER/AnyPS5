# Fork-only: ANYPS5_INPUT_SCRIPT parsing and timing (core/libs/prx/libScePad/include/InputScript.hpp).
add_executable(pad_input_script_tests PadInputScript.cpp)
target_include_directories(pad_input_script_tests PRIVATE ../..)
target_compile_features(pad_input_script_tests PRIVATE cxx_std_20)
set_target_properties(pad_input_script_tests PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "$<1:${ANYPS5_TEST_OUTPUT_DIRECTORY}>")
add_test(NAME pad_input_script COMMAND pad_input_script_tests)
set_tests_properties(pad_input_script PROPERTIES TIMEOUT 30)
