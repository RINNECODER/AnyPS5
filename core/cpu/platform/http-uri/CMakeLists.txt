cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AnyPS5NativeUriAcceptance LANGUAGES C CXX)
    include(CTest)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(ANYPS5_PLATFORM_TCG_LIBRARY "" CACHE FILEPATH "Pinned ARM64 TCG dylib")
    set(ANYPS5_PLATFORM_TCG_INCLUDE "" CACHE PATH "Matching pinned TCG headers")
    if(NOT EXISTS "${ANYPS5_PLATFORM_TCG_LIBRARY}" OR
       NOT EXISTS "${ANYPS5_PLATFORM_TCG_INCLUDE}/qemu/anyps5-cpu.h")
        message(FATAL_ERROR "URI acceptance requires explicitly pinned matching TCG library and headers")
    endif()
    # Same light machine sources as retained platform component acceptance;
    # this target never configures/rebuilds TCG or the renderer.
    add_library(anyps5_uri_machine STATIC ../../src/CpuQemu.cpp
        ../../src/SceElf.cpp ../../src/Self.cpp ../../src/SceTls.cpp
        ../../src/GuestThreads.cpp ../../src/SceThreadImports.cpp ../../src/SceModules.cpp
        ../../src/SceLifecycleImports.cpp ../../../libs/nid/src/NidCompute.cpp ../../../libs/nid/src/Sha1.cpp)
    target_include_directories(anyps5_uri_machine PUBLIC ../../include
        PRIVATE "${ANYPS5_PLATFORM_TCG_INCLUDE}" ../../../libs/nid/include)
    target_compile_definitions(anyps5_uri_machine PUBLIC ANYPS5_CPU_MODERN_TCG=1)
    target_compile_options(anyps5_uri_machine PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(anyps5_uri_machine PUBLIC "${ANYPS5_PLATFORM_TCG_LIBRARY}")
    get_filename_component(uriTcgDirectory "${ANYPS5_PLATFORM_TCG_LIBRARY}" DIRECTORY)
    set(CMAKE_BUILD_RPATH "${uriTcgDirectory}")
    set(uriCpuTarget anyps5_uri_machine)
else()
    if(NOT TARGET anyps5_cpu)
        message(FATAL_ERROR "Include native URI leaf after anyps5_cpu")
    endif()
    set(uriCpuTarget anyps5_cpu)
endif()
include(NativeUriTests.cmake)
anyps5_add_native_uri_tests(${uriCpuTarget})
