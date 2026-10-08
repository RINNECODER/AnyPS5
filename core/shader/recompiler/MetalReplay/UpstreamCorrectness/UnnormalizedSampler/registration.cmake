if(NOT TARGET anyps5_metal_native_execution OR NOT TARGET anyps5_metal_utilities)
    message(FATAL_ERROR "Register pixel samplers after the native Metal execution targets")
endif()
add_executable(anyps5_metal_pixel_sampler_replay "${CMAKE_CURRENT_LIST_DIR}/UnnormalizedSamplerReplay.mm")
target_compile_options(anyps5_metal_pixel_sampler_replay PRIVATE -fobjc-arc)
target_link_libraries(anyps5_metal_pixel_sampler_replay PRIVATE anyps5_metal_native_execution anyps5_metal_utilities)
if(BUILD_TESTING)
    get_target_property(pixelSamplerUtilityLibrary anyps5_metal_utilities ANYPS5_METALLIB)
    foreach(pixelSamplerMode native mixed rejections cache)
        add_test(NAME anyps5_metal_pixel_sampler_${pixelSamplerMode}
            COMMAND anyps5_metal_pixel_sampler_replay "${pixelSamplerUtilityLibrary}" ${pixelSamplerMode})
        set_tests_properties(anyps5_metal_pixel_sampler_${pixelSamplerMode} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
    endforeach()
    set_tests_properties(anyps5_metal_pixel_sampler_cache PROPERTIES
        ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=0;ANYPS5_SHADER_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/pixel-sampler-cache")
endif()
