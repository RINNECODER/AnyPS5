# Included only after production NativeModuleRunner and its existing tests exist.
if(BUILD_TESTING AND cpuModernTcg AND TARGET anyps5_native_module_runner)
    set(service_source "${CMAKE_CURRENT_SOURCE_DIR}/platform/native-service-integration")
    find_program(service_clang NAMES clang HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(service_linker NAMES ld.lld HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin REQUIRED)
    set(service_outputs)
    foreach(service_kind IN ITEMS np-identity http-uri)
        set(service_dir "${CMAKE_CURRENT_BINARY_DIR}/native-service-integration/${service_kind}")
        if(service_kind STREQUAL "np-identity")
            set(service_np 1)
            set(service_main "${service_dir}/NativeNpIdentityGuest.elf")
            set(service_np_main "${service_main}")
            set(service_np_dep "${service_dir}/NativeServiceDependency.prx")
        else()
            set(service_np 0)
            set(service_main "${service_dir}/NativeUriGuest.elf")
            set(service_uri_main "${service_main}")
            set(service_uri_dep "${service_dir}/NativeServiceDependency.prx")
        endif()
        set(service_dep "${service_dir}/NativeServiceDependency.prx")
        set(service_wrong "${service_dir}/wrong-row")
        get_filename_component(service_name "${service_main}" NAME)
        add_custom_command(OUTPUT "${service_main}" "${service_dep}" "${service_wrong}/${service_name}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${service_dir}"
            COMMAND "${service_clang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC -ffreestanding
                -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -mno-red-zone
                -DBUILD_SERVICE_DEPENDENCY=1 -c "${service_source}/NativeServiceGuest.c" -o "${service_dir}/dependency.o"
            COMMAND "${service_linker}" -shared -e NativeServiceDependency --hash-style=sysv -soname NativeServiceDependency.prx
                -z max-page-size=4096 -z separate-code -z norelro -o "${service_dir}/dependency-linked.elf" "${service_dir}/dependency.o"
            COMMAND "${service_clang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC -ffreestanding
                -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -mno-red-zone
                "-DBUILD_SERVICE_NP=${service_np}" -c "${service_source}/NativeServiceGuest.c" -o "${service_dir}/main.o"
            COMMAND "${service_linker}" -shared -e _start --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro
                -o "${service_dir}/main-linked.elf" "${service_dir}/main.o" "${service_dir}/dependency-linked.elf"
            COMMAND "${Python3_EXECUTABLE}" "${service_source}/BuildNativeServiceFixture.py" "${service_kind}"
                "${service_dir}/dependency-linked.elf" "${service_dir}/main-linked.elf" "${service_dep}" "${service_main}"
            DEPENDS "${service_source}/NativeServiceGuest.c" "${service_source}/BuildNativeServiceFixture.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildPlatformServiceFixture.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildSceModulesFixture.py" VERBATIM)
        list(APPEND service_outputs "${service_main}" "${service_dep}" "${service_wrong}/${service_name}")
    endforeach()
    add_custom_target(anyps5_native_service_integration_guests DEPENDS ${service_outputs})
    add_test_executable(anyps5_native_service_integration_test "${service_source}/NativeServiceIntegrationTest.mm")
    target_compile_options(anyps5_native_service_integration_test PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
    target_link_libraries(anyps5_native_service_integration_test PRIVATE anyps5_native_module_runner "${cpuAppKit}")
    add_dependencies(anyps5_native_service_integration_test anyps5_native_service_integration_guests anyps5_metal_shaders)
    foreach(service_kind IN ITEMS np-identity http-uri)
        if(service_kind STREQUAL "np-identity")
            set(service_main "${service_np_main}")
            set(service_dep "${service_np_dep}")
        else()
            set(service_main "${service_uri_main}")
            set(service_dep "${service_uri_dep}")
        endif()
        foreach(service_case IN ITEMS compiled admission retained snapshot)
            add_test(NAME "anyps5_native_service_${service_kind}_${service_case}"
                COMMAND anyps5_native_service_integration_test "${service_main}" "${service_dep}"
                    "${cpuMetalLibrary}" "${service_kind}" "${service_case}")
            set_tests_properties("anyps5_native_service_${service_kind}_${service_case}" PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
                ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
        endforeach()
    endforeach()
    add_test(NAME anyps5_native_service_main_cli COMMAND "${Python3_EXECUTABLE}"
        "${service_source}/NativeServiceIntegrationCli.py" "$<TARGET_FILE:anyps5_cpu_run>"
        "${service_np_main}" "${service_np_dep}" "${service_uri_main}" "${service_uri_dep}" "${cpuMetalLibrary}")
    set_tests_properties(anyps5_native_service_main_cli PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE
        ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
endif()
