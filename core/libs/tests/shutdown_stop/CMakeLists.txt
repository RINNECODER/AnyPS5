# Fork-only: a ProcessShutdown raised by a shutdown callback is the requested stop, not a failure.
add_test_executable(shutdown_stop_tests ShutdownStop.cpp)
target_include_directories(shutdown_stop_tests PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT})
target_link_libraries(shutdown_stop_tests PRIVATE libc)
set_target_properties(shutdown_stop_tests PROPERTIES BUILD_RPATH ${UNPATCHED_DIR})
add_test(NAME rosetta_shutdown_stop COMMAND arch -x86_64 $<TARGET_FILE:shutdown_stop_tests>)
set_tests_properties(rosetta_shutdown_stop PROPERTIES TIMEOUT 60)
