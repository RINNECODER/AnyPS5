# Fork-only: native ARM64 AOT graphics runtime on the fork's Metal backend.
# Included by the top-level CMakeLists when ANYPS5_NATIVE_AOT_GRAPHICS=ON.
#
# The upstream AOT graphics closure (core/libs/native/CMakeLists.txt) builds libSceAgcDriver and
# libSceVideoOut from the Vulkan driver and needs MoltenVK at runtime. This path instead builds
# the fork's Metal backend (cmake/RosettaGraphics.cmake source lists), so the gpu_cube fixture
# runs on Metal with no Vulkan loader.

if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "ANYPS5_NATIVE_AOT_GRAPHICS requires Apple ARM64")
endif()
if(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "ANYPS5_NATIVE_AOT_GRAPHICS requires CMAKE_OSX_ARCHITECTURES=arm64")
endif()

enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

foreach(aotGraphicsDep IN ITEMS SPIRV-Cross SPIRV-Headers Vulkan-Headers glslang SDL2 libjpeg-turbo stb)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/${aotGraphicsDep}")
        message(FATAL_ERROR "ANYPS5_NATIVE_AOT_GRAPHICS needs the 3rdparty/${aotGraphicsDep} submodule")
    endif()
endforeach()

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
add_subdirectory(${CMAKE_SOURCE_DIR}/3rdparty/SDL2 ${CMAKE_BINARY_DIR}/3rdparty/SDL2 EXCLUDE_FROM_ALL)

add_subdirectory(${CMAKE_SOURCE_DIR}/core/shader/recompiler)
add_subdirectory(${CMAKE_SOURCE_DIR}/core/Decoder/Jpeg ${CMAKE_BINARY_DIR}/Decoder/Jpeg)
add_subdirectory(${CMAKE_SOURCE_DIR}/core/Decoder/Png ${CMAKE_BINARY_DIR}/Decoder/Png)

# AOT compiler, native image and native runtime (core/aot/CMakeLists.txt).
add_subdirectory(${CMAKE_SOURCE_DIR}/core/aot)

# Component targets (libc, libkernel, libSceAgc, ...). EXCLUDE_FROM_ALL keeps the .prx targets
# from building; core/libs/native reads their SOURCES into anyps5_native_graphics.
add_subdirectory(${CMAKE_SOURCE_DIR}/core/libs EXCLUDE_FROM_ALL)

# Fork Metal graphics runtime and the gpu_cube fixture.
add_subdirectory(${CMAKE_SOURCE_DIR}/core/libs/native ${CMAKE_BINARY_DIR}/core/libs/native)
