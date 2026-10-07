# Included by the native Metal build after both runtime libraries exist.
if(TARGET anyps5_cpu AND TARGET anyps5_metal_native_execution)
    add_library(anyps5_cpu_agc STATIC
        "${PROJECT_SOURCE_DIR}/core/cpu/src/SceAgcImports.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/NativeAgcBackend.cpp"
        "${PROJECT_SOURCE_DIR}/core/libs/prx/libSceAgc/Shader/src/ShaderUtils.cpp")
    target_compile_features(anyps5_cpu_agc PUBLIC cxx_std_20)
    target_compile_options(anyps5_cpu_agc PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(anyps5_cpu_agc PUBLIC
        "${PROJECT_SOURCE_DIR}/core/cpu/include"
        "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(anyps5_cpu_agc PUBLIC anyps5_cpu anyps5_metal_native_execution)
    if(BUILD_TESTING)
        get_target_property(agcUtilityLibrary anyps5_metal_utilities ANYPS5_METALLIB)
        add_executable(anyps5_cpu_agc_native_fixture "${CMAKE_CURRENT_LIST_DIR}/NativeAgcFixture.mm")
        target_compile_features(anyps5_cpu_agc_native_fixture PRIVATE cxx_std_20)
        target_compile_options(anyps5_cpu_agc_native_fixture PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
        target_link_libraries(anyps5_cpu_agc_native_fixture PRIVATE anyps5_cpu_agc)
        add_test(NAME anyps5_cpu_agc_native_provider
            COMMAND anyps5_cpu_agc_native_fixture "${agcUtilityLibrary}")
        set_tests_properties(anyps5_cpu_agc_native_provider PROPERTIES
            TIMEOUT 60 RUN_SERIAL TRUE
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
    endif()
endif()
