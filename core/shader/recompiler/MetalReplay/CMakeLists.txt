find_package(Threads REQUIRED)

add_library(anyps5_metal_native_runtime INTERFACE)
target_include_directories(anyps5_metal_native_runtime INTERFACE "${PROJECT_SOURCE_DIR}/core/libs")
target_link_libraries(anyps5_metal_native_runtime INTERFACE Threads::Threads)
include("${CMAKE_CURRENT_SOURCE_DIR}/../CMakeLists.txt")
add_shader_recompiler(anyps5_metal_guest_recompiler anyps5_metal_native_runtime)

set(agcDriverRoot "${PROJECT_SOURCE_DIR}/core/libs/prx/libSceAgcDriver")
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/3rdparty/Vulkan-Headers/include/vulkan/vulkan.h")
    message(FATAL_ERROR "Initialize the pinned 3rdparty/Vulkan-Headers submodule for native driver state metadata")
endif()
add_library(anyps5_metal_native_execution STATIC
    "${agcDriverRoot}/Execution/src/Pm4.cpp"
    "${agcDriverRoot}/Execution/src/ComputeDispatch.cpp"
    "${agcDriverRoot}/Execution/src/DrawDispatch.cpp"
    "${agcDriverRoot}/Execution/src/NativeGuestMemory.cpp"
    "${agcDriverRoot}/Graphics/src/ComputeStageInfo.cpp"
    "${agcDriverRoot}/Graphics/src/State.cpp"
    "${agcDriverRoot}/Graphics/src/SurfaceMetadata.cpp"
    "${agcDriverRoot}/Graphics/src/ShaderInputState.cpp"
    "${agcDriverRoot}/Graphics/src/ColorTargetLayout.cpp"
    "${agcDriverRoot}/Graphics/src/TextureTiling.cpp"
    "${agcDriverRoot}/Graphics/src/TextureFormat.cpp"
    "${agcDriverRoot}/Graphics/src/GuestTextureResource.cpp"
    "${agcDriverRoot}/Graphics/src/GuestSamplerResource.cpp"
    "${agcDriverRoot}/Graphics/Metal/MetalComputeDispatch.mm"
    "${agcDriverRoot}/Graphics/Metal/MetalRenderState.mm"
    "${agcDriverRoot}/Graphics/Metal/MetalTexture.mm"
    "${agcDriverRoot}/Graphics/Metal/MetalSampler.mm"
    "${agcDriverRoot}/Graphics/Metal/MetalDraw.mm"
    "${agcDriverRoot}/Graphics/Metal/MetalShaderResources.mm"
    "${agcDriverRoot}/Graphics/Metal/MetalDepthSurface.mm"
)
target_compile_options(anyps5_metal_native_execution PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_compile_definitions(anyps5_metal_native_execution PUBLIC ANYPS5_METAL_GUEST_REPLAY=1)
target_include_directories(anyps5_metal_native_execution PUBLIC
    "${PROJECT_SOURCE_DIR}/core/libs"
    "${PROJECT_SOURCE_DIR}/3rdparty/Vulkan-Headers/include"
)
target_link_libraries(anyps5_metal_native_execution PUBLIC
    anyps5_metal_guest_recompiler
    anyps5_metal_utilities
)

add_test_executable(anyps5_metal_pm4_compute_replay Pm4ComputeReplay.mm)
target_compile_options(anyps5_metal_pm4_compute_replay PRIVATE -fobjc-arc)
target_link_libraries(anyps5_metal_pm4_compute_replay PRIVATE anyps5_metal_native_execution)

add_test_executable(anyps5_metal_render_state_tests "${agcDriverRoot}/tests/metal/RenderState.mm")
target_compile_options(anyps5_metal_render_state_tests PRIVATE -fobjc-arc)
target_link_libraries(anyps5_metal_render_state_tests PRIVATE anyps5_metal_native_execution)

add_test_executable(anyps5_metal_guest_draw_replay GuestDrawReplay.mm)
target_compile_options(anyps5_metal_guest_draw_replay PRIVATE -fobjc-arc)
target_link_libraries(anyps5_metal_guest_draw_replay PRIVATE anyps5_metal_native_execution)

add_executable(anyps5_metal_guest_replay GuestShaderReplay.mm)
target_compile_options(anyps5_metal_guest_replay PRIVATE -fobjc-arc)
target_link_libraries(anyps5_metal_guest_replay PRIVATE
    anyps5_metal_guest_recompiler anyps5_metal_utilities)
if(BUILD_TESTING)
    get_target_property(metalUtilityLibrary anyps5_metal_utilities ANYPS5_METALLIB)
    target_sources(metal_utility_tests PRIVATE
        "${agcDriverRoot}/tests/metal/TextureResource.mm"
        "${agcDriverRoot}/tests/metal/SamplerResource.mm"
        "${agcDriverRoot}/tests/metal/DepthResource.mm"
    )
    target_link_libraries(metal_utility_tests PRIVATE anyps5_metal_native_execution)
    add_test(NAME anyps5_metal_guest_draw_replay COMMAND anyps5_metal_guest_draw_replay "${metalUtilityLibrary}")
    set_tests_properties(anyps5_metal_guest_draw_replay PROPERTIES TIMEOUT 120)
    add_test(NAME anyps5_metal_pm4_compute_replay COMMAND anyps5_metal_pm4_compute_replay)
    set_tests_properties(anyps5_metal_pm4_compute_replay PROPERTIES TIMEOUT 120)
    add_test(NAME anyps5_metal_render_state COMMAND anyps5_metal_render_state_tests)
    set_tests_properties(anyps5_metal_render_state PROPERTIES TIMEOUT 120)
    add_test(NAME anyps5_metal_guest_replay COMMAND anyps5_metal_guest_replay)
    set_tests_properties(anyps5_metal_guest_replay PROPERTIES TIMEOUT 120)
endif()
