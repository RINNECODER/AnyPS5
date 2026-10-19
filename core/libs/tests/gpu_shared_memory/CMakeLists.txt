cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(GpuSharedMemoryTests LANGUAGES CXX)
    enable_testing()
    set(ANYPS5_TEST_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

find_package(Threads REQUIRED)

# The x86_64 side is the engine's own guest-memory code, run under Rosetta with
# ANYPS5_GPU_SHARED_MEMORY=1: the arena slide, the shared arena backing, the direct memory object
# and the registry the manifest is written from.
add_executable(gpu_shared_memory_writer
    SharedMemoryWriter.cpp
    ../../prx/libkernel/DirectMemory/DirectMemory.cpp
    ../../prx/libkernel/DirectMemory/Export.cpp
    ../../prx/libkernel/DirectMemory/MemoryPool.cpp
    ../../prx/libc/src/GuestAllocations.cpp
    ../../prx/libc/src/GuestArena.cpp
    ../../prx/libc/src/GpuSharedMemory.cpp
    ../../prx/libc/src/GuestWriteWatch.cpp
    ../../prx/libkernel/AppMetadata/src/ParamJsonParser.cpp)
target_include_directories(gpu_shared_memory_writer PRIVATE ../..)
target_compile_features(gpu_shared_memory_writer PRIVATE cxx_std_20)
target_compile_definitions(gpu_shared_memory_writer PRIVATE APS5_ENABLE_TIMING_LOG=0)
target_link_libraries(gpu_shared_memory_writer PRIVATE Threads::Threads)

# The proof: a native arm64 process that opens the published objects and maps the ranges at the
# addresses the x86_64 process registered. It links nothing from the engine.
add_executable(gpu_shared_memory_verifier SharedMemoryVerifier.cpp)
target_compile_features(gpu_shared_memory_verifier PRIVATE cxx_std_20)

# The registry contract: a partial remap or unmap keeps the fragments of the registered range
# around it, with their object offsets advanced by the bytes kept.
add_executable(gpu_shared_memory_registry
    RegistryFragments.cpp
    ../../prx/libc/src/GpuSharedMemory.cpp)
target_include_directories(gpu_shared_memory_registry PRIVATE ../..)
target_compile_features(gpu_shared_memory_registry PRIVATE cxx_std_20)

set_target_properties(gpu_shared_memory_writer gpu_shared_memory_verifier gpu_shared_memory_registry PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "$<1:${ANYPS5_TEST_OUTPUT_DIRECTORY}>")
set_target_properties(gpu_shared_memory_writer PROPERTIES OSX_ARCHITECTURES x86_64)
set_target_properties(gpu_shared_memory_verifier PROPERTIES OSX_ARCHITECTURES arm64)

add_test(NAME gpu_shared_memory_registry
    COMMAND gpu_shared_memory_registry ${CMAKE_CURRENT_BINARY_DIR}/fragments-manifest.txt)
set_tests_properties(gpu_shared_memory_registry PROPERTIES TIMEOUT 30)

add_test(NAME gpu_shared_memory_proof
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_proof.sh
        $<TARGET_FILE:gpu_shared_memory_writer>
        $<TARGET_FILE:gpu_shared_memory_verifier>
        ${CMAKE_CURRENT_BINARY_DIR}/proof)
set_tests_properties(gpu_shared_memory_proof PROPERTIES TIMEOUT 180)
