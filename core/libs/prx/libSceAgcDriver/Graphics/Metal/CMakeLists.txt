find_program(ANYPS5_XCRUN xcrun REQUIRED)
find_library(ANYPS5_FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(ANYPS5_METAL_FRAMEWORK Metal REQUIRED)
find_library(ANYPS5_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)

set(metalShaderDirectory "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(metalAirFiles)
foreach(shaderName IN ITEMS TextureDetile ColorTransfer MeshArguments SampleCounter Presentation)
    set(shaderSource "${CMAKE_CURRENT_SOURCE_DIR}/${shaderName}.metal")
    set(shaderAir "${metalShaderDirectory}/${shaderName}.air")
    add_custom_command(
        OUTPUT "${shaderAir}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${metalShaderDirectory}"
        COMMAND "${ANYPS5_XCRUN}" -sdk macosx metal -c "${shaderSource}" -o "${shaderAir}"
        DEPENDS "${shaderSource}"
        VERBATIM
    )
    list(APPEND metalAirFiles "${shaderAir}")
endforeach()

set(metalLibrary "${metalShaderDirectory}/AnyPS5Utilities.metallib")
add_custom_command(
    OUTPUT "${metalLibrary}"
    COMMAND "${ANYPS5_XCRUN}" -sdk macosx metallib ${metalAirFiles} -o "${metalLibrary}"
    DEPENDS ${metalAirFiles}
    VERBATIM
)
add_custom_target(anyps5_metal_shaders DEPENDS "${metalLibrary}")

add_library(anyps5_metal_utilities STATIC
    MetalDevice.mm
    MetalPresenter.mm
    MetalGuestMemory.mm
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Execution/src/AspectFit.cpp"
)
target_compile_options(anyps5_metal_utilities PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_include_directories(anyps5_metal_utilities PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/core/libs"
    "${PROJECT_SOURCE_DIR}/core/shader/recompiler"
)
target_link_libraries(anyps5_metal_utilities PUBLIC
    "${ANYPS5_FOUNDATION_FRAMEWORK}"
    "${ANYPS5_METAL_FRAMEWORK}"
    "${ANYPS5_QUARTZCORE_FRAMEWORK}"
)
add_dependencies(anyps5_metal_utilities anyps5_metal_shaders)
set_target_properties(anyps5_metal_utilities PROPERTIES ANYPS5_METALLIB "${metalLibrary}")

if(ANYPS5_METAL_SHADER_BRIDGE)
    target_sources(anyps5_metal_utilities PRIVATE
        MetalShaderPipeline.mm
        MetalSampler.mm
        ../src/GuestSamplerResource.cpp
        ../src/GuestTextureResource.cpp
        ../src/TextureTiling.cpp
        ../src/TextureFormat.cpp
    )
    target_include_directories(anyps5_metal_utilities PRIVATE "${PROJECT_SOURCE_DIR}/3rdparty/Vulkan-Headers/include")
    target_compile_definitions(anyps5_metal_utilities PUBLIC ANYPS5_METAL_SHADER_BRIDGE=1)
    target_link_libraries(anyps5_metal_utilities PUBLIC anyps5_metal_shader_bridge)
endif()

if(BUILD_TESTING)
    set(metalTestDirectory "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/metal")
    add_test_executable(metal_utility_tests
        "${metalTestDirectory}/MetalTestSupport.mm"
        "${metalTestDirectory}/Main.mm"
        "${metalTestDirectory}/TextureDetile.mm"
        "${metalTestDirectory}/ColorTransfer.mm"
        "${metalTestDirectory}/MeshArguments.mm"
        "${metalTestDirectory}/SampleCounter.mm"
        "${metalTestDirectory}/Presentation.mm"
        "${metalTestDirectory}/GuestMemory.mm"
    )
    target_compile_options(metal_utility_tests PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(metal_utility_tests PRIVATE anyps5_metal_utilities)
    if(ANYPS5_METAL_SHADER_BRIDGE)
        target_sources(metal_utility_tests PRIVATE "${metalTestDirectory}/ShaderPipeline.mm")
        target_sources(metal_utility_tests PRIVATE "${metalTestDirectory}/ShaderAlignment.mm")
        target_include_directories(metal_utility_tests PRIVATE "${PROJECT_SOURCE_DIR}/3rdparty/SPIRV-Cross")
    endif()
    add_dependencies(metal_utility_tests anyps5_metal_shaders)
    add_test(NAME metal_utility_tests COMMAND metal_utility_tests "${metalLibrary}")
    set_tests_properties(metal_utility_tests PROPERTIES TIMEOUT 120)
endif()
