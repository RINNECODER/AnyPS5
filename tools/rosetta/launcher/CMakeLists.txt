# The Rosetta engine launcher (anyps5_rosetta_run) and the stub runtime it injects.
# Built only in the x86_64 Rosetta libs mode (cmake/RosettaLibs.cmake). The build
# tree's rosetta/ directory has the engine package layout: bin/anyps5_rosetta_run,
# bin/relinker, libs/*.prx and share/aps5_stub_runtime.dylib, so the launcher runs
# from the build tree exactly as from an installed package.
enable_language(C)
set(ANYPS5_ROSETTA_ENGINE_VERSION "dev" CACHE STRING "Version string anyps5_rosetta_run reports")

add_library(anyps5_rosetta_launcher_core STATIC MachO.cpp Unself.cpp)
target_compile_features(anyps5_rosetta_launcher_core PUBLIC cxx_std_20)
target_include_directories(anyps5_rosetta_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(anyps5_rosetta_run Launcher.cpp)
target_link_libraries(anyps5_rosetta_run PRIVATE anyps5_rosetta_launcher_core)
target_compile_definitions(anyps5_rosetta_run PRIVATE ANYPS5_ROSETTA_ENGINE_VERSION="${ANYPS5_ROSETTA_ENGINE_VERSION}")
set_target_properties(anyps5_rosetta_run PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rosetta/bin)

# The stub dylib the launcher writes binds _aps5_stub_common/_aps5_trace_common
# here, and this library reads the stub dylib's tables by flat lookup.
add_library(aps5_stub_runtime SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../stub_runtime.c)
set_target_properties(aps5_stub_runtime PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rosetta/share)
target_compile_options(aps5_stub_runtime PRIVATE -O1)
target_link_options(aps5_stub_runtime PRIVATE LINKER:-undefined,dynamic_lookup LINKER:-no_fixup_chains)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/rosetta/bin/relinker
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/rosetta/bin
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:relinker> ${CMAKE_BINARY_DIR}/rosetta/bin/relinker
    DEPENDS relinker
    VERBATIM)
add_custom_target(rosetta_engine ALL DEPENDS anyps5_rosetta_run aps5_stub_runtime rosetta_libs ${CMAKE_BINARY_DIR}/rosetta/bin/relinker)

if(ANYPS5_ROSETTA_BUILD_TESTING)
    add_executable(rosetta_launcher_tests tests/LauncherTests.cpp)
    target_link_libraries(rosetta_launcher_tests PRIVATE anyps5_rosetta_launcher_core)
    add_dependencies(rosetta_launcher_tests aps5_stub_runtime)
    add_test(NAME rosetta_launcher_tests
        COMMAND rosetta_launcher_tests $<TARGET_FILE:aps5_stub_runtime> ${CMAKE_CURRENT_BINARY_DIR}/test-work)
    set_tests_properties(rosetta_launcher_tests PROPERTIES TIMEOUT 60 LABELS rosetta)
endif()
