# Rosetta direct-execution runtime tests (cmake/RosettaLibs.cmake, BUILD_TESTING).
# Each test builds small guest fixtures, relinks them with --macos --to-rosetta
# and runs them under arch -x86_64 against the x86_64 libs/*.prx.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Every import one libs/*.prx takes from another must be in that library's export trie.
add_test(NAME rosetta_lib_binds
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_lib_binds.py ${CMAKE_BINARY_DIR}/rosetta/libs)
set_tests_properties(rosetta_lib_binds PROPERTIES TIMEOUT 120 LABELS rosetta)

# The relinker's --to-rosetta CPUID stub, linked on a page and run under Rosetta.
set(ANYPS5_RELINKER_ROOT ${CMAKE_SOURCE_DIR}/core/relinker)
add_test_executable(rosetta_cpuid_tests
    ${ANYPS5_RELINKER_ROOT}/codegen/tests/RosettaCpuidTests.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/Amd64OnlyConverter.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/InstructionScanner.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/X64InstructionDecoder.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/DecodedInstruction.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/Amd64OnlyInstructionMatcher.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/Sse4aOperands.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/Sse4aLowering.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/StubBodyBuilder.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/Sha256Operands.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/Sha256Lowering.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/Sha1Operands.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/Sha1Lowering.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/ClzeroOperands.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/ClzeroLowering.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/ReciprocalOperands.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/ScalarCompareOperands.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/ReciprocalLowering.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/WideStoreOperands.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/WideStoreLowering.cpp
    ${ANYPS5_RELINKER_ROOT}/codegen/src/x86/AndnLowering.cpp)
target_include_directories(rosetta_cpuid_tests PRIVATE ${ANYPS5_RELINKER_ROOT}/domain/include ${ANYPS5_RELINKER_ROOT}/codegen/include)
target_compile_features(rosetta_cpuid_tests PRIVATE cxx_std_20)
add_test(NAME rosetta_cpuid COMMAND rosetta_cpuid_tests)
set_tests_properties(rosetta_cpuid PROPERTIES TIMEOUT 60 LABELS rosetta)

find_program(ANYPS5_ROSETTA_LLD ld.lld HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin)
if(NOT ANYPS5_ROSETTA_LLD)
    message(STATUS "Rosetta runtime tests skipped: ld.lld not found (the guest fixtures are ELF)")
    return()
endif()

add_custom_target(rosetta_runtime_tests DEPENDS rosetta_libs relinker rosetta_engine)
foreach(mode threads fault)
    add_test(NAME rosetta_runtime_${mode}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_runtime_test.py
            $<TARGET_FILE:relinker> ${CMAKE_BINARY_DIR}/rosetta/libs ${CMAKE_CURRENT_BINARY_DIR}/work ${mode})
    set_tests_properties(rosetta_runtime_${mode} PROPERTIES TIMEOUT 180 LABELS rosetta)
endforeach()

# A synthetic title through the engine launcher with MacPS's arguments, from the build's
# rosetta/ tree (the package layout); build_package.py runs the same script on the package.
add_test(NAME rosetta_launcher_smoke
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_smoke.py ${CMAKE_BINARY_DIR}/rosetta ${CMAKE_CURRENT_BINARY_DIR}/launcher-smoke)
set_tests_properties(rosetta_launcher_smoke PROPERTIES TIMEOUT 300 LABELS rosetta)
