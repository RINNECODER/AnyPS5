# Copied unchanged to a fresh external configure source by the native profile.
# Attach the existing finite fixture fragments after production targets exist.
# ANYPS5_DIAGNOSTIC_SOURCE is the supplied runtime checkout. It must already
# contain the qualified runner; this tooling branch carries no runtime commits.
cmake_minimum_required(VERSION 3.24)
project(NativeDiagnosticControls LANGUAGES C CXX OBJCXX)
enable_testing()
if(NOT IS_ABSOLUTE "${ANYPS5_DIAGNOSTIC_SOURCE}")
    message(FATAL_ERROR "An explicit native diagnostic source directory is required")
endif()
add_subdirectory("${ANYPS5_DIAGNOSTIC_SOURCE}" source)
get_target_property(diagnosticCpuSource anyps5_cpu SOURCE_DIR)
get_target_property(diagnosticCpuBinary anyps5_cpu BINARY_DIR)
set(ANYPS5_DIAGNOSTIC_CPU_SOURCE_DIR "${diagnosticCpuSource}" CACHE PATH "Compiled CPU source directory" FORCE)
set(ANYPS5_DIAGNOSTIC_CPU_BINARY_DIR "${diagnosticCpuBinary}" CACHE PATH "Compiled CPU binary directory" FORCE)
if(NOT BUILD_TESTING OR NOT ANYPS5_CPU_NATIVE_MODULE_RUNNER OR
   NOT TARGET anyps5_native_module_runner OR NOT TARGET anyps5_cpu_agc)
    message(FATAL_ERROR "Native diagnostic profile requires the compiled native runner and testing")
endif()
add_subdirectory("${ANYPS5_DIAGNOSTIC_SOURCE}/core/cpu/videoout" native-videoout)
add_subdirectory("${ANYPS5_DIAGNOSTIC_SOURCE}/core/cpu/flip" native-flip)
