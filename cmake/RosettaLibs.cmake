
if(NOT APPLE OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    message(FATAL_ERROR "ANYPS5_ROSETTA_LIBS requires macOS and CMAKE_OSX_ARCHITECTURES=x86_64 (the libraries run inside a Rosetta process)")
endif()

# Libraries whose dependencies are not built for x86_64 in this mode yet.
# Graphics comes with the Metal backend (design doc phase 3).
set(ANYPS5_ROSETTA_EXCLUDED_LIBS
    )

# Imported by titles but without a source directory in core/libs/prx.
set(ANYPS5_ROSETTA_SOURCELESS_LIBS libHttpServer.Prospero)

# libc.prx defines the guest's __stack_chk_guard and ld64 binds the host stack
# protector's ___stack_chk_guard to it; the NID patcher then renames that export
# and dyld cannot load the libraries. Covers SDL2 (linked into libSceAudioOut).
add_compile_options(-fno-stack-protector)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_X11 OFF CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
if(NOT ANYPS5_USE_SYSTEM_DEPS)
    add_subdirectory(${CMAKE_SOURCE_DIR}/3rdparty/SDL2 ${CMAKE_BINARY_DIR}/3rdparty/SDL2 EXCLUDE_FROM_ALL)
endif()

set(ANYPS5_ROSETTA_LIBS_ROOT ${CMAKE_SOURCE_DIR}/core/libs)
set(LIBS_INCLUDE_DIR ${ANYPS5_ROSETTA_LIBS_ROOT})
set(UNPATCHED_DIR ${CMAKE_BINARY_DIR}/rosetta/libs/unpatched)
set(PATCHED_DIR ${CMAKE_BINARY_DIR}/rosetta/libs)

function(configure_windows_unwind target)
endfunction()

file(GLOB ANYPS5_NID_PATCHER_SOURCES ${ANYPS5_ROSETTA_LIBS_ROOT}/nid/src/*.cpp)
add_executable(nid_patcher ${ANYPS5_NID_PATCHER_SOURCES})
target_include_directories(nid_patcher PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT}/nid/include)
target_compile_features(nid_patcher PRIVATE cxx_std_20)

set(ANYPS5_ROSETTA_BUILD_TESTING ${BUILD_TESTING})
set(BUILD_TESTING OFF)

add_subdirectory(${CMAKE_SOURCE_DIR}/core/relinker ${CMAKE_BINARY_DIR}/core/relinker)
# The relinker is a plain host tool: ship it universal so the launcher runs it
# natively on Apple silicon (about a third faster than under Rosetta).
set_target_properties(relinker PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")

# Third-party code the media libraries link, built for x86_64 (static, PIC).
# ffmpeg is the prebuilt package that 3rdparty/ffmpeg-core downloads; the
# freetype, libjpeg-turbo and stb inputs are the vendored submodules.
set(ARCHITECTURE x86_64)
add_subdirectory(${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg-core ${CMAKE_BINARY_DIR}/rosetta/3rdparty/ffmpeg-core)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/3rdparty/freetype ${CMAKE_BINARY_DIR}/rosetta/3rdparty/freetype EXCLUDE_FROM_ALL)
set_target_properties(freetype PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_subdirectory(${CMAKE_SOURCE_DIR}/core/Decoder/Jpeg ${CMAKE_BINARY_DIR}/rosetta/Decoder/Jpeg)
add_subdirectory(${CMAKE_SOURCE_DIR}/core/Decoder/Png ${CMAKE_BINARY_DIR}/rosetta/Decoder/Png)

add_library(libc SHARED EXCLUDE_FROM_ALL)
add_subdirectory(${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libc ${CMAKE_BINARY_DIR}/rosetta/prx/libc)
target_include_directories(libc PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT})
set(ANYPS5_ROSETTA_TARGETS libc)

include(${CMAKE_SOURCE_DIR}/cmake/RosettaGraphics.cmake)

file(GLOB ANYPS5_ROSETTA_LIB_DIRS LIST_DIRECTORIES true ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/*)
foreach(LIB_DIR ${ANYPS5_ROSETTA_LIB_DIRS})
    get_filename_component(LIB_NAME ${LIB_DIR} NAME)
    if(NOT IS_DIRECTORY ${LIB_DIR} OR LIB_NAME STREQUAL "libc")
        continue()
    endif()
    if(LIB_NAME IN_LIST ANYPS5_ROSETTA_EXCLUDED_LIBS)
        message(STATUS "Rosetta libs: ${LIB_NAME} excluded (dependencies not built for x86_64 yet)")
        continue()
    endif()
    if(LIB_NAME IN_LIST ANYPS5_ROSETTA_METAL_LIBS)
        # Defined on the Metal backend by cmake/RosettaGraphics.cmake.
    elseif(EXISTS ${LIB_DIR}/CMakeLists.txt)
        add_subdirectory(${LIB_DIR} ${CMAKE_BINARY_DIR}/rosetta/prx/${LIB_NAME})
    else()
        add_library(${LIB_NAME} SHARED EXCLUDE_FROM_ALL ${LIB_DIR}/Export.cpp)
        target_include_directories(${LIB_NAME} PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT})
    endif()
    # ld64 has no --exclude-libs (GNU ld); the NID patcher decides what is exported.
    get_target_property(LIB_LINK_OPTIONS ${LIB_NAME} LINK_OPTIONS)
    if(LIB_LINK_OPTIONS)
        list(FILTER LIB_LINK_OPTIONS EXCLUDE REGEX "--exclude-libs")
        set_target_properties(${LIB_NAME} PROPERTIES LINK_OPTIONS "${LIB_LINK_OPTIONS}")
    endif()
    if(LIB_NAME STREQUAL "libkernel")
        # Fork-only start, TLS and fault hooks the relinked Mach-O binds to (design doc phase 2).
        target_sources(${LIB_NAME} PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libkernel/Darwin/GuestRuntime.cpp
            ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libkernel/Darwin/GuestProfiler.cpp)
        target_link_libraries(${LIB_NAME} PRIVATE libc)
    else()
        target_link_libraries(${LIB_NAME} PRIVATE libkernel libc)
    endif()
    list(APPEND ANYPS5_ROSETTA_TARGETS ${LIB_NAME})
endforeach()
# Stub libraries live in tools/rosetta/libs/<name>/Export.cpp (fork-only files).
foreach(LIB_NAME ${ANYPS5_ROSETTA_SOURCELESS_LIBS})
    set(STUB_SOURCE ${CMAKE_SOURCE_DIR}/tools/rosetta/libs/${LIB_NAME}/Export.cpp)
    if(IS_DIRECTORY ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/${LIB_NAME} OR NOT EXISTS ${STUB_SOURCE})
        message(STATUS "Rosetta libs: ${LIB_NAME} not built (no stub source)")
        continue()
    endif()
    add_library(${LIB_NAME} SHARED EXCLUDE_FROM_ALL ${STUB_SOURCE})
    target_include_directories(${LIB_NAME} PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT})
    target_link_libraries(${LIB_NAME} PRIVATE libkernel libc)
    list(APPEND ANYPS5_ROSETTA_TARGETS ${LIB_NAME})
endforeach()

set(BUILD_TESTING ${ANYPS5_ROSETTA_BUILD_TESTING})

set_target_properties(${ANYPS5_ROSETTA_TARGETS} PROPERTIES
    PREFIX ""
    SUFFIX .prx
    LIBRARY_OUTPUT_DIRECTORY ${UNPATCHED_DIR}
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@loader_path"
    INSTALL_NAME_DIR "@rpath")

foreach(LIB_NAME ${ANYPS5_ROSETTA_TARGETS})
    target_link_options(${LIB_NAME} PRIVATE LINKER:-fixup_chains)
endforeach()

set(LIBC_UNPATCHED ${UNPATCHED_DIR}/libc.prx)
set(ANYPS5_ROSETTA_PATCHED)
foreach(LIB_NAME ${ANYPS5_ROSETTA_TARGETS})
    set(PATCHED_FILE ${PATCHED_DIR}/${LIB_NAME}.prx)
    if(LIB_NAME STREQUAL "libc")
        set(PRESERVE_EXPORTS)
        set(LIBC_DEPENDENCY)
    else()
        set(PRESERVE_EXPORTS --preserve-exports ${LIBC_UNPATCHED})
        set(LIBC_DEPENDENCY libc)
    endif()
    add_custom_command(
        OUTPUT ${PATCHED_FILE}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${LIB_NAME}> ${PATCHED_FILE}
        COMMAND $<TARGET_FILE:nid_patcher> ${LIB_NAME} ${PRESERVE_EXPORTS} ${PATCHED_FILE}
        DEPENDS nid_patcher ${LIB_NAME} ${LIBC_DEPENDENCY}
        COMMENT "Patching NIDs for ${LIB_NAME} (x86_64 Mach-O)"
        VERBATIM)
    add_custom_target(rosetta_patch_${LIB_NAME} DEPENDS ${PATCHED_FILE})
    list(APPEND ANYPS5_ROSETTA_PATCHED rosetta_patch_${LIB_NAME})
endforeach()

add_custom_target(rosetta_libs ALL)
add_dependencies(rosetta_libs ${ANYPS5_ROSETTA_PATCHED} relinker rosetta_metallib)

# The engine package's launcher and stub runtime (tools/rosetta/launcher; target rosetta_engine).
add_subdirectory(${CMAKE_SOURCE_DIR}/tools/rosetta/launcher ${CMAKE_BINARY_DIR}/rosetta/launcher)

if(BUILD_TESTING)
    add_subdirectory(${ANYPS5_ROSETTA_LIBS_ROOT}/tests/direct_memory_darwin ${CMAKE_BINARY_DIR}/rosetta/tests/direct_memory_darwin)
    add_subdirectory(${ANYPS5_ROSETTA_LIBS_ROOT}/tests/gpu_shared_memory ${CMAKE_BINARY_DIR}/rosetta/tests/gpu_shared_memory)
    add_subdirectory(${ANYPS5_ROSETTA_LIBS_ROOT}/tests/pad_input_script ${CMAKE_BINARY_DIR}/rosetta/tests/pad_input_script)
    anyps5_rosetta_graphics_tests()
    add_subdirectory(${ANYPS5_ROSETTA_LIBS_ROOT}/tests/rosetta_direct_input ${CMAKE_BINARY_DIR}/rosetta/tests/direct_input)
    add_subdirectory(${ANYPS5_ROSETTA_LIBS_ROOT}/tests/ajm_opus_surround ${CMAKE_BINARY_DIR}/rosetta/tests/ajm_opus_surround)
    add_subdirectory(${ANYPS5_ROSETTA_LIBS_ROOT}/tests/audio_out2_pacing ${CMAKE_BINARY_DIR}/rosetta/tests/audio_out2_pacing)
    add_subdirectory(${ANYPS5_ROSETTA_LIBS_ROOT}/tests/shutdown_stop ${CMAKE_BINARY_DIR}/rosetta/tests/shutdown_stop)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tools/rosetta/tests ${CMAKE_BINARY_DIR}/rosetta/tests/runtime)
endif()
