if(BUILD_TESTING AND cpuModernTcg AND TARGET anyps5_native_module_runner)
    add_test_executable(anyps5_native_semaphore_runner_test tests/NativeSemaphoreRunnerTest.mm)
    target_compile_options(anyps5_native_semaphore_runner_test PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
    target_link_libraries(anyps5_native_semaphore_runner_test PRIVATE anyps5_native_module_runner "${cpuAppKit}")
    add_dependencies(anyps5_native_semaphore_runner_test anyps5_sce_module_fixture anyps5_metal_shaders)
    add_test(NAME anyps5_native_semaphore_runner
        COMMAND anyps5_native_semaphore_runner_test "${sceModuleMain}" "${cpuMetalLibrary}")
    set_tests_properties(anyps5_native_semaphore_runner PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
        ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
endif()
