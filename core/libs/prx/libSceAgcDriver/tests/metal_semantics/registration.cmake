# Upstream execution tests (core/libs/prx/libSceAgcDriver/tests/execution) run unchanged on Metal.
# Each wrapper pre-defines the Vulkan test-device include guard and puts the shim headers first,
# so the test compiles against MetalTestDevice (see shim/) instead of a Vulkan device.
if(NOT TARGET anyps5_metal_guest_recompiler OR NOT TARGET anyps5_metal_utilities)
    message(FATAL_ERROR "Register shader semantics after the native Metal recompiler targets")
endif()
set(semanticsDir "${CMAKE_CURRENT_LIST_DIR}")
set(semanticsUpstreamDir "${CMAKE_CURRENT_LIST_DIR}/../execution")
cmake_path(NORMAL_PATH semanticsUpstreamDir)
add_library(anyps5_metal_semantics_device STATIC "${semanticsDir}/MetalTestDevice.mm")
target_compile_options(anyps5_metal_semantics_device PRIVATE -fobjc-arc)
target_include_directories(anyps5_metal_semantics_device PUBLIC "${semanticsDir}/shim")
target_link_libraries(anyps5_metal_semantics_device PUBLIC anyps5_metal_guest_recompiler anyps5_metal_utilities)
foreach(semanticsTest IN ITEMS ValuMaskInactiveLanes VCvtF32F64ResultModes LdsAddF32 LdsSrc2LaneOrder)
    string(TOLOWER "${semanticsTest}" semanticsLower)
    set(semanticsWrapper "${CMAKE_CURRENT_BINARY_DIR}/semantics_${semanticsTest}.cpp")
    file(GENERATE OUTPUT "${semanticsWrapper}" CONTENT
"#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_VULKANTESTDEVICE_HPP\n#include \"MetalTestDevice.hpp\"\n#include \"${semanticsUpstreamDir}/${semanticsTest}.cpp\"\n")
    add_executable(anyps5_metal_semantics_${semanticsLower} "${semanticsWrapper}")
    target_include_directories(anyps5_metal_semantics_${semanticsLower} BEFORE PRIVATE "${semanticsDir}/shim")
    target_link_libraries(anyps5_metal_semantics_${semanticsLower} PRIVATE anyps5_metal_semantics_device)
    set_target_properties(anyps5_metal_semantics_${semanticsLower} PROPERTIES CXX_EXTENSIONS OFF)
    if(BUILD_TESTING)
        add_test(NAME anyps5_metal_semantics_${semanticsLower} COMMAND anyps5_metal_semantics_${semanticsLower})
        set_tests_properties(anyps5_metal_semantics_${semanticsLower} PROPERTIES
            TIMEOUT 180 RUN_SERIAL TRUE
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
    endif()
endforeach()
