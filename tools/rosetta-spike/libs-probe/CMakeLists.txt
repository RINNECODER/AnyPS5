# Compile probe: build the CPU-side system libraries (core/libs/prx) for
# x86_64 macOS, which is what a relinked title loads under Rosetta. Graphics
# and media libraries that need submodules (libSceAgcDriver, libSceAgc,
# libSceVideoOut, ...) are left out; pass -DPROBE_LIBS=... to change the set.
#
#   cmake -S tools/rosetta-spike/libs-probe -B <build> -G Ninja \
#         -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=Release
#   ninja -C <build> -k 0
cmake_minimum_required(VERSION 3.24)
project(AnyPS5RosettaLibsProbe C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(APS5_ENABLE_TIMING_LOG=0)
get_filename_component(ANYPS5_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(LIBS_ROOT ${ANYPS5_ROOT}/core/libs)
set(LIBS_INCLUDE_DIR ${LIBS_ROOT})
set(PATCHED_DIR ${CMAKE_BINARY_DIR}/libs)
set(UNPATCHED_DIR ${CMAKE_BINARY_DIR}/libs/unpatched)

function(add_test_executable target)
    add_executable(${target} EXCLUDE_FROM_ALL ${ARGN})
endfunction()
function(configure_windows_unwind target)
endfunction()

set(PROBE_LIBS
    libkernel libSceLibcInternal libSceSysmodule libSceFiber libSceRtc libSceUserService
    libSceSystemService libScePad libSceMouse libSceIme libSceConvertKeycode libSceSystemGesture
    libSceAppContent libSceNet libSceNetCtl libSceHttp libSceHttp2 libSceSsl libSceJson2
    libSceNpManager libSceNpAuth libSceNpTrophy2 libSceNpWebApi2 libSceNpCommerce
    libSceNpEntitlementAccess libSceNpGameIntent libSceNpUniversalDataSystem libSceCommonDialog
    libSceErrorDialog libSceSigninDialog libSceImeDialog libScePlayerInvitationDialog
    libSceSaveData.native libSceSaveDataDialog.native libSceMsgDialog.native libSceRazorCpu libSceAcm
    libSceAudioOut libSceAudio3d libHttpServer.Prospero
    CACHE STRING "Libraries to probe")

add_library(libc SHARED)
add_subdirectory(${LIBS_ROOT}/prx/libc ${CMAKE_BINARY_DIR}/prx/libc)
target_include_directories(libc PRIVATE ${LIBS_ROOT})
set(PROBE_TARGETS libc)
foreach(LIB_NAME ${PROBE_LIBS})
    set(LIB_DIR ${LIBS_ROOT}/prx/${LIB_NAME})
    if(NOT IS_DIRECTORY ${LIB_DIR})
        message(STATUS "probe: ${LIB_NAME} has no source directory")
        continue()
    endif()
    if(EXISTS ${LIB_DIR}/CMakeLists.txt)
        add_subdirectory(${LIB_DIR} ${CMAKE_BINARY_DIR}/prx/${LIB_NAME})
    else()
        add_library(${LIB_NAME} SHARED ${LIB_DIR}/Export.cpp)
        target_include_directories(${LIB_NAME} PRIVATE ${LIBS_ROOT})
    endif()
    if(LIB_NAME STREQUAL "libkernel")
        target_link_libraries(${LIB_NAME} PRIVATE libc)
    else()
        target_link_libraries(${LIB_NAME} PRIVATE libkernel libc)
    endif()
    set_target_properties(${LIB_NAME} PROPERTIES EXCLUDE_FROM_ALL OFF)
    list(APPEND PROBE_TARGETS ${LIB_NAME})
endforeach()
set_target_properties(${PROBE_TARGETS} PROPERTIES PREFIX "" SUFFIX .prx
    LIBRARY_OUTPUT_DIRECTORY ${UNPATCHED_DIR} BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "@loader_path")
