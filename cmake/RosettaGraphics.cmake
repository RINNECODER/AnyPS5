# Fork-only: the graphics libraries of the Rosetta direct path (docs/design/rosetta-direct-execution.md,
# phase 3), included by cmake/RosettaLibs.cmake.
#
# libSceAgcDriver and libSceVideoOut are built on the fork's Metal backend instead of upstream's
# Vulkan driver: the native Metal execution sources, the SPIR-V to MSL bridge and the shader
# recompiler, all for x86_64. On this path guest pointers are host pointers, so the driver
# borrows the guest allocation registry as identity ranges (Execution/Metal/DirectDriver.mm)
# instead of going through a TCG translation layer. libSceAgc builds from its upstream
# CMakeLists through the generic loop. The targets defined here are skipped by that loop's
# add_subdirectory and otherwise get the same treatment (libkernel/libc links, NID patching).

enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

foreach(rosettaGraphicsSubmodule IN ITEMS SPIRV-Cross SPIRV-Headers Vulkan-Headers glslang)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/${rosettaGraphicsSubmodule}/CMakeLists.txt")
        message(FATAL_ERROR "Rosetta graphics libraries need the 3rdparty/${rosettaGraphicsSubmodule} submodule")
    endif()
endforeach()

set(ANYPS5_ROSETTA_METAL_LIBS libSceAgcDriver libSceVideoOut)

include(${CMAKE_SOURCE_DIR}/core/shader/recompiler/CMakeLists.txt)
set(ANYPS5_METAL_SHADER_BRIDGE ON)
add_subdirectory(${CMAKE_SOURCE_DIR}/core/shader/recompiler/MetalBackend ${CMAKE_BINARY_DIR}/rosetta/metal/bridge)
set(BUILD_TESTING ${ANYPS5_ROSETTA_BUILD_TESTING})
add_subdirectory(${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libSceAgcDriver/Graphics/Metal ${CMAKE_BINARY_DIR}/rosetta/metal/utilities)
set(BUILD_TESTING OFF)
get_target_property(ANYPS5_ROSETTA_METALLIB anyps5_metal_utilities ANYPS5_METALLIB)

find_library(ANYPS5_ROSETTA_APPKIT AppKit REQUIRED)
find_library(ANYPS5_ROSETTA_CARBON Carbon REQUIRED)

set(rosettaAgcDriver ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libSceAgcDriver)
add_library(libSceAgcDriver SHARED EXCLUDE_FROM_ALL
    # Upstream exports that do not depend on the Vulkan driver.
    ${rosettaAgcDriver}/Unimplemented.cpp
    ${rosettaAgcDriver}/Eq/src/Event.cpp
    ${rosettaAgcDriver}/Eq/src/Query.cpp
    ${rosettaAgcDriver}/Resource/src/Lifecycle.cpp
    ${rosettaAgcDriver}/Resource/src/Registration.cpp
    ${rosettaAgcDriver}/State/src/Configuration.cpp
    ${rosettaAgcDriver}/State/src/Status.cpp
    ${rosettaAgcDriver}/Submit/src/Acb.cpp
    ${rosettaAgcDriver}/Submit/src/CommandBuffer.cpp
    ${rosettaAgcDriver}/Submit/src/Dcb.cpp
    ${rosettaAgcDriver}/Submit/src/Submission.cpp
    ${rosettaAgcDriver}/Execution/src/AspectFit.cpp
    # The native Metal execution (the same set core/shader/recompiler/MetalReplay links).
    ${rosettaAgcDriver}/Execution/src/Pm4.cpp
    ${rosettaAgcDriver}/Execution/src/ComputeDispatch.cpp
    ${rosettaAgcDriver}/Execution/src/DrawDispatch.cpp
    ${rosettaAgcDriver}/Execution/src/IndirectDraw.cpp
    ${rosettaAgcDriver}/Execution/src/NativeGuestMemory.cpp
    ${rosettaAgcDriver}/Execution/src/SubmissionCapture.cpp
    ${rosettaAgcDriver}/Execution/src/ShaderCapture.cpp
    ${rosettaAgcDriver}/Execution/src/SubmissionValidation.cpp
    ${rosettaAgcDriver}/Execution/src/ShaderMemory.cpp
    ${rosettaAgcDriver}/Execution/src/Driver/EntryPoints.cpp
    ${rosettaAgcDriver}/Execution/src/DisplayBuffer.cpp
    ${rosettaAgcDriver}/Execution/src/DisplayFormat.cpp
    ${rosettaAgcDriver}/Execution/Metal/MetalDriver.mm
    ${rosettaAgcDriver}/Execution/Metal/MetalPacketExecution.mm
    ${rosettaAgcDriver}/Execution/Metal/DirectDriver.mm
    ${rosettaAgcDriver}/Graphics/src/ComputeStageInfo.cpp
    ${rosettaAgcDriver}/Graphics/src/State.cpp
    ${rosettaAgcDriver}/Graphics/src/SurfaceMetadata.cpp
    ${rosettaAgcDriver}/Graphics/src/DccClearEncoding.cpp
    ${rosettaAgcDriver}/Graphics/src/ShaderInputState.cpp
    ${rosettaAgcDriver}/Graphics/src/ColorTargetLayout.cpp
    ${rosettaAgcDriver}/Graphics/src/MeshDraw.cpp
    ${rosettaAgcDriver}/Graphics/Metal/MetalComputeDispatch.mm
    ${rosettaAgcDriver}/Graphics/Metal/MetalRenderState.mm
    ${rosettaAgcDriver}/Graphics/Metal/MetalTexture.mm
    ${rosettaAgcDriver}/Graphics/Metal/MetalDraw.mm
    ${rosettaAgcDriver}/Graphics/Metal/MetalShaderResources.mm
    ${rosettaAgcDriver}/Graphics/Metal/MetalDepthSurface.mm
    ${rosettaAgcDriver}/Graphics/Metal/MetalPresentation.mm
    ${rosettaAgcDriver}/Graphics/Metal/FrameCapture.mm
    ${rosettaAgcDriver}/Graphics/Metal/MetalDccExtent.cpp
    ${rosettaAgcDriver}/Graphics/Metal/MetalHostExports.cpp)
target_compile_options(libSceAgcDriver PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_compile_definitions(libSceAgcDriver PRIVATE ANYPS5_METAL_GUEST_REPLAY=1 ANYPS5_METAL_BACKEND=1 ANYPS5_ROSETTA_DIRECT=1)
target_include_directories(libSceAgcDriver PRIVATE
    ${ANYPS5_ROSETTA_LIBS_ROOT}
    ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
    ${ANYPS5_SDL2_INCLUDE_DIR})
add_shader_recompiler(anyps5_rosetta_shader_recompiler libc)
target_link_libraries(libSceAgcDriver PRIVATE
    anyps5_rosetta_shader_recompiler
    anyps5_metal_utilities
    "-framework ImageIO"
    "-framework CoreGraphics")
set_target_properties(libSceAgcDriver PROPERTIES CXX_EXTENSIONS OFF)

set(rosettaVideoOut ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libSceVideoOut)
add_library(libSceVideoOut SHARED EXCLUDE_FROM_ALL
    # Upstream sources, except the SDL window, which a native AppKit window replaces.
    ${rosettaVideoOut}/Unimplemented.cpp
    ${rosettaVideoOut}/src/Event.cpp
    ${rosettaVideoOut}/src/BufferMetadata.cpp
    ${rosettaVideoOut}/src/VideoOutState.cpp
    ${rosettaVideoOut}/src/Buffer.cpp
    ${rosettaVideoOut}/src/Output.cpp
    ${rosettaVideoOut}/src/VideoOutDriver.cpp
    ${rosettaVideoOut}/src/PadInput.cpp
    ${rosettaVideoOut}/src/MouseInput.cpp
    ${rosettaVideoOut}/src/KeyboardInput.cpp
    ${rosettaVideoOut}/src/ImeTextInput.cpp
    ${rosettaVideoOut}/src/InputMapping.cpp
    ${ANYPS5_ROSETTA_LIBS_ROOT}/prx/libkernel/Time/TimedWait.cpp
    ${rosettaVideoOut}/src/NativeHostWindow.mm
    ${rosettaVideoOut}/src/DirectHostApplication.mm
    ${rosettaVideoOut}/src/DirectDisplayWindow.mm)
target_compile_options(libSceVideoOut PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_compile_definitions(libSceVideoOut PRIVATE ANYPS5_METAL_BACKEND=1 ANYPS5_ROSETTA_DIRECT=1)
target_include_directories(libSceVideoOut PRIVATE
    ${ANYPS5_ROSETTA_LIBS_ROOT}
    ${ANYPS5_SDL2_INCLUDE_DIR}
    ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(libSceVideoOut PRIVATE
    libSceAgcDriver libScePad libSceMouse libSceKeyboard libSceIme SDL2-static
    "${ANYPS5_ROSETTA_APPKIT}" "${ANYPS5_ROSETTA_CARBON}" "-framework QuartzCore" "-framework Metal")
set_target_properties(libSceVideoOut PROPERTIES CXX_EXTENSIONS OFF)

# The utility metallib ships next to the libraries; DirectDriver.mm loads it from there.
add_custom_command(
    OUTPUT ${PATCHED_DIR}/AnyPS5Utilities.metallib
    COMMAND ${CMAKE_COMMAND} -E copy ${ANYPS5_ROSETTA_METALLIB} ${PATCHED_DIR}/AnyPS5Utilities.metallib
    DEPENDS anyps5_metal_shaders ${ANYPS5_ROSETTA_METALLIB}
    VERBATIM)
add_custom_target(rosetta_metallib DEPENDS ${PATCHED_DIR}/AnyPS5Utilities.metallib)

function(anyps5_rosetta_graphics_tests)
    if(NOT ANYPS5_ROSETTA_BUILD_TESTING)
        return()
    endif()
    add_test_executable(rosetta_direct_graphics_tests
        ${ANYPS5_ROSETTA_LIBS_ROOT}/tests/rosetta_direct_graphics/DirectGraphicsTest.mm)
    target_compile_options(rosetta_direct_graphics_tests PRIVATE -fobjc-arc)
    target_include_directories(rosetta_direct_graphics_tests PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
    target_link_libraries(rosetta_direct_graphics_tests PRIVATE
        libSceAgcDriver libSceVideoOut libkernel libc "${ANYPS5_ROSETTA_APPKIT}" "-framework ImageIO" "-framework CoreGraphics")
    set_target_properties(rosetta_direct_graphics_tests PROPERTIES
        BUILD_RPATH ${UNPATCHED_DIR})
    add_dependencies(rosetta_direct_graphics_tests rosetta_metallib)
    add_test(NAME rosetta_direct_graphics COMMAND arch -x86_64 $<TARGET_FILE:rosetta_direct_graphics_tests> ${PATCHED_DIR}/AnyPS5Utilities.metallib)
    set_tests_properties(rosetta_direct_graphics PROPERTIES TIMEOUT 60)
    # The Metal driver's cross-draw image cache bookkeeping; plain C++, no GPU.
    add_test_executable(rosetta_image_cache_tests ${ANYPS5_ROSETTA_LIBS_ROOT}/tests/rosetta_direct_graphics/ImageCacheTest.cpp)
    target_include_directories(rosetta_image_cache_tests PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT})
    target_compile_features(rosetta_image_cache_tests PRIVATE cxx_std_20)
    add_test(NAME rosetta_image_cache COMMAND arch -x86_64 $<TARGET_FILE:rosetta_image_cache_tests>)
    set_tests_properties(rosetta_image_cache PROPERTIES TIMEOUT 30 LABELS rosetta)


    add_test_executable(rosetta_direct_host_window_tests
        ${ANYPS5_ROSETTA_LIBS_ROOT}/tests/rosetta_direct_graphics/DirectHostWindowTest.mm)
    target_compile_options(rosetta_direct_host_window_tests PRIVATE -fobjc-arc)
    target_include_directories(rosetta_direct_host_window_tests PRIVATE ${ANYPS5_ROSETTA_LIBS_ROOT})
    target_link_libraries(rosetta_direct_host_window_tests PRIVATE
        libSceVideoOut libSceAgcDriver libkernel libc "${ANYPS5_ROSETTA_APPKIT}" "-framework CoreGraphics")
    set_target_properties(rosetta_direct_host_window_tests PROPERTIES
        BUILD_RPATH ${UNPATCHED_DIR})
    add_dependencies(rosetta_direct_host_window_tests rosetta_metallib)
    add_test(NAME rosetta_direct_host_window
        COMMAND arch -x86_64 $<TARGET_FILE:rosetta_direct_host_window_tests> ${PATCHED_DIR}/AnyPS5Utilities.metallib)
    set_tests_properties(rosetta_direct_host_window PROPERTIES TIMEOUT 90)
endfunction()
