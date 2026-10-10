# Included by the production native runner branch after its target exists.
# Reuse linker-produced public SCE module images; no duplicate fixture inventory.
include("${CMAKE_CURRENT_SOURCE_DIR}/NativeSemaphoreRunnerTests.cmake")
if(BUILD_TESTING AND cpuModernTcg AND TARGET anyps5_native_module_runner)
    add_test_executable(anyps5_native_module_runner_test tests/NativeModuleRunnerTest.mm)
    target_compile_options(anyps5_native_module_runner_test PRIVATE -fobjc-arc)
    target_link_libraries(anyps5_native_module_runner_test PRIVATE anyps5_native_module_runner "${cpuAppKit}")
    add_dependencies(anyps5_native_module_runner_test anyps5_sce_module_fixture anyps5_metal_shaders)
    foreach(nativeRunnerCase lifecycle close initializer-failure title-agnostic)
        add_test(NAME anyps5_native_module_runner_${nativeRunnerCase}
            COMMAND anyps5_native_module_runner_test "${sceModuleMain}" "${sceModuleGuest}"
                "${cpuMetalLibrary}" "${nativeRunnerCase}")
        set_tests_properties(anyps5_native_module_runner_${nativeRunnerCase} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
            RESOURCE_LOCK appkit_focus LABELS gui
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
    endforeach()
    add_test(NAME anyps5_native_module_runner_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildNativeModuleRunnerFixture.py"
        "$<TARGET_FILE:anyps5_cpu_run>" "${sceModuleMain}" "${sceModuleGuest}" "${cpuMetalLibrary}")
    set_tests_properties(anyps5_native_module_runner_cli PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/platform/native-service-integration/NativeServiceIntegrationTests.cmake")
