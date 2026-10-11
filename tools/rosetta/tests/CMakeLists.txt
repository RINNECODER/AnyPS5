# Rosetta direct-execution runtime tests (cmake/RosettaLibs.cmake, BUILD_TESTING).
# Each test builds small guest fixtures, relinks them with --macos --to-rosetta
# and runs them under arch -x86_64 against the x86_64 libs/*.prx.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Every import one libs/*.prx takes from another must be in that library's export trie.
add_test(NAME rosetta_lib_binds
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_lib_binds.py ${CMAKE_BINARY_DIR}/rosetta/libs)
set_tests_properties(rosetta_lib_binds PROPERTIES TIMEOUT 120 LABELS rosetta)

find_program(ANYPS5_ROSETTA_LLD ld.lld HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin)
if(NOT ANYPS5_ROSETTA_LLD)
    message(STATUS "Rosetta runtime tests skipped: ld.lld not found (the guest fixtures are ELF)")
    return()
endif()

add_custom_target(rosetta_runtime_tests DEPENDS rosetta_libs relinker)
foreach(mode threads fault)
    add_test(NAME rosetta_runtime_${mode}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_runtime_test.py
            $<TARGET_FILE:relinker> ${CMAKE_BINARY_DIR}/rosetta/libs ${CMAKE_CURRENT_BINARY_DIR}/work ${mode})
    set_tests_properties(rosetta_runtime_${mode} PROPERTIES TIMEOUT 180 LABELS rosetta)
endforeach()
