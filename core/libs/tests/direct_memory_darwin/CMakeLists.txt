cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(DirectMemoryDarwinTests LANGUAGES CXX)
    enable_testing()
    set(ANYPS5_TEST_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

find_package(Threads REQUIRED)
add_executable(direct_memory_darwin_tests
    ../DirectMemoryDarwin.cpp
    ../../prx/libkernel/DirectMemory/DirectMemory.cpp
    ../../prx/libkernel/DirectMemory/Export.cpp
    ../../prx/libkernel/DirectMemory/MemoryPool.cpp
    ../../prx/libc/src/GuestAllocations.cpp
    ../../prx/libc/src/GuestArena.cpp
    ../../prx/libc/src/GpuSharedMemory.cpp
    ../../prx/libc/src/GuestWriteWatch.cpp
    ../../prx/libkernel/AppMetadata/src/ParamJsonParser.cpp
    ResolvePathStub.cpp)
target_include_directories(direct_memory_darwin_tests PRIVATE ../..)
target_compile_features(direct_memory_darwin_tests PRIVATE cxx_std_20)
target_compile_definitions(direct_memory_darwin_tests PRIVATE APS5_ENABLE_TIMING_LOG=0)
target_link_libraries(direct_memory_darwin_tests PRIVATE Threads::Threads)
set_target_properties(direct_memory_darwin_tests PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "$<1:${ANYPS5_TEST_OUTPUT_DIRECTORY}>")

add_test(NAME direct_memory_darwin COMMAND direct_memory_darwin_tests)
set_tests_properties(direct_memory_darwin PROPERTIES TIMEOUT 60)
