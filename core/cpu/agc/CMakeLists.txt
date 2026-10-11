# Included by the native Metal build after both runtime libraries exist.
if(TARGET anyps5_cpu AND TARGET anyps5_metal_native_execution)
    # Upstream libSceAgc builders that encode packets into a host scratch
    # command buffer; SceAgcImports copies the words into guest memory.
    set(agcUpstream "${PROJECT_SOURCE_DIR}/core/libs/prx/libSceAgc")
    add_library(anyps5_cpu_agc STATIC
        "${PROJECT_SOURCE_DIR}/core/cpu/src/SceAgcImports.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/NativeAgcBackend.cpp" "${CMAKE_CURRENT_LIST_DIR}/AgcRegisterDefaults.cpp"
        "${agcUpstream}/Shader/src/ShaderUtils.cpp"
        "${agcUpstream}/Command/src/Packet.cpp" "${agcUpstream}/Command/src/Memory.cpp"
        "${agcUpstream}/Command/src/Control.cpp" "${agcUpstream}/DcbFlow/src/Memory.cpp"
        "${agcUpstream}/DcbFlow/src/Sync.cpp" "${agcUpstream}/Acb/src/Memory.cpp" "${agcUpstream}/Acb/src/Sync.cpp"
        "${agcUpstream}/Cb/src/Sync.cpp" "${agcUpstream}/Cb/src/Registers.cpp"
        "${agcUpstream}/Cb/src/Branch.cpp" "${agcUpstream}/Cb/src/Dispatch.cpp" "${agcUpstream}/Command/src/Draw.cpp"
        "${agcUpstream}/DcbDraw/src/DrawIndexed.cpp" "${agcUpstream}/DcbDraw/src/DrawNonIndexed.cpp"
        "${agcUpstream}/DcbDraw/src/IndexBuffer.cpp" "${agcUpstream}/DcbDraw/src/Instancing.cpp" "${agcUpstream}/DcbDraw/src/Stats.cpp"
        "${agcUpstream}/DcbFlow/src/Control.cpp" "${agcUpstream}/DcbFlow/src/Dispatch.cpp"
        "${agcUpstream}/DcbState/src/ContextState.cpp" "${agcUpstream}/DcbState/src/Display.cpp" "${agcUpstream}/DcbState/src/Marker.cpp"
        "${agcUpstream}/DcbState/src/Predication.cpp" "${agcUpstream}/DcbState/src/Registers.cpp"
        "${agcUpstream}/Acb/src/Control.cpp" "${agcUpstream}/Acb/src/Dispatch.cpp"
        "${agcUpstream}/Misc/src/PacketInfo.cpp" "${agcUpstream}/Misc/src/Platform.cpp" "${agcUpstream}/DcbState/src/Init.cpp"
        "${agcUpstream}/Predication/src/Predication.cpp" "${agcUpstream}/Command/src/RegisterDefaults.cpp"
        "${agcUpstream}/Patch/src/Branch.cpp" "${agcUpstream}/Patch/src/CondExec.cpp" "${agcUpstream}/Patch/src/Dma.cpp"
        "${agcUpstream}/Patch/src/IndirectRegisters.cpp" "${agcUpstream}/Patch/src/Jump.cpp" "${agcUpstream}/Patch/src/Nop.cpp"
        "${agcUpstream}/Patch/src/QueueEndOfPipeAction.cpp" "${agcUpstream}/Patch/src/Rewind.cpp" "${agcUpstream}/Patch/src/WaitRegMem.cpp"
        "${agcUpstream}/Misc/src/RegisterDefaults.cpp" "${agcUpstream}/Shader/src/PrimState.cpp"
        "${PROJECT_SOURCE_DIR}/core/cpu/src/upstream-prx/AgcDriver.cpp")
    target_compile_features(anyps5_cpu_agc PUBLIC cxx_std_20)
    target_compile_options(anyps5_cpu_agc PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(anyps5_cpu_agc PUBLIC
        "${PROJECT_SOURCE_DIR}/core/cpu/include"
        "${CMAKE_CURRENT_LIST_DIR}")
    target_include_directories(anyps5_cpu_agc PRIVATE "${PROJECT_SOURCE_DIR}/core/libs/nid/include")
    target_link_libraries(anyps5_cpu_agc PUBLIC anyps5_cpu anyps5_metal_native_execution)
    if(BUILD_TESTING)
        find_program(agcGuestClang clang HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
        find_program(agcGuestObjcopy llvm-objcopy HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
        set(agcGuestObject "${CMAKE_CURRENT_BINARY_DIR}/target-agc-caller.o")
        set(agcGuestWrite "${CMAKE_CURRENT_BINARY_DIR}/target-agc-write.bin")
        set(agcGuestSubmit "${CMAKE_CURRENT_BINARY_DIR}/target-agc-submit.bin")
        set(agcGuestShader "${CMAKE_CURRENT_BINARY_DIR}/target-agc-shader.bin")
        set(agcGuestDispatch "${CMAKE_CURRENT_BINARY_DIR}/target-agc-dispatch.bin")
        set(agcGuestAcbWrite "${CMAKE_CURRENT_BINARY_DIR}/target-agc-acb-write.bin")
        add_custom_command(OUTPUT "${agcGuestWrite}" "${agcGuestSubmit}" "${agcGuestShader}" "${agcGuestDispatch}" "${agcGuestAcbWrite}"
            COMMAND "${agcGuestClang}" --target=x86_64-unknown-none-elf -O2 -ffreestanding
                -ffunction-sections -fno-stack-protector -fno-asynchronous-unwind-tables
                -mno-red-zone -c "${CMAKE_CURRENT_LIST_DIR}/TargetAgcCaller.c" -o "${agcGuestObject}"
            COMMAND "${agcGuestObjcopy}"
                --dump-section ".text.agc_target_write=${agcGuestWrite}"
                --dump-section ".text.agc_target_submit=${agcGuestSubmit}"
                --dump-section ".text.agc_target_shader=${agcGuestShader}"
                --dump-section ".text.agc_target_dispatch=${agcGuestDispatch}"
                --dump-section ".text.agc_target_acb_write=${agcGuestAcbWrite}" "${agcGuestObject}"
            DEPENDS "${CMAKE_CURRENT_LIST_DIR}/TargetAgcCaller.c" VERBATIM)
        add_custom_target(anyps5_agc_target_callers DEPENDS "${agcGuestWrite}" "${agcGuestSubmit}" "${agcGuestShader}" "${agcGuestDispatch}" "${agcGuestAcbWrite}")
        get_target_property(agcUtilityLibrary anyps5_metal_utilities ANYPS5_METALLIB)
        add_executable(anyps5_cpu_agc_native_fixture "${CMAKE_CURRENT_LIST_DIR}/NativeAgcFixture.mm")
        add_dependencies(anyps5_cpu_agc_native_fixture anyps5_agc_target_callers)
        target_compile_features(anyps5_cpu_agc_native_fixture PRIVATE cxx_std_20)
        target_compile_options(anyps5_cpu_agc_native_fixture PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
        target_link_libraries(anyps5_cpu_agc_native_fixture PRIVATE anyps5_cpu_agc)
        add_test(NAME anyps5_cpu_agc_native_provider
            COMMAND anyps5_cpu_agc_native_fixture "${agcUtilityLibrary}" "${agcGuestWrite}" "${agcGuestSubmit}" "${agcGuestShader}" "${agcGuestDispatch}" "${agcGuestAcbWrite}")
        add_executable(anyps5_cpu_agc_capability_fixture "${CMAKE_CURRENT_LIST_DIR}/AgcCapabilityFixture.cpp")
        target_compile_features(anyps5_cpu_agc_capability_fixture PRIVATE cxx_std_20)
        target_link_libraries(anyps5_cpu_agc_capability_fixture PRIVATE anyps5_cpu_agc)
        add_test(NAME anyps5_cpu_agc_capability COMMAND anyps5_cpu_agc_capability_fixture)
        set_tests_properties(anyps5_cpu_agc_capability PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
        add_executable(anyps5_cpu_agc_builder_fixture "${CMAKE_CURRENT_LIST_DIR}/AgcBuilderFixture.cpp")
        target_compile_features(anyps5_cpu_agc_builder_fixture PRIVATE cxx_std_20)
        target_compile_options(anyps5_cpu_agc_builder_fixture PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(anyps5_cpu_agc_builder_fixture PRIVATE anyps5_cpu_agc)
        add_test(NAME anyps5_cpu_agc_builder COMMAND anyps5_cpu_agc_builder_fixture)
        set_tests_properties(anyps5_cpu_agc_builder PROPERTIES TIMEOUT 30)
        add_executable(anyps5_cpu_agc_driver_leaf_fixture "${CMAKE_CURRENT_LIST_DIR}/AgcDriverLeafFixture.cpp")
        target_compile_features(anyps5_cpu_agc_driver_leaf_fixture PRIVATE cxx_std_20)
        target_compile_options(anyps5_cpu_agc_driver_leaf_fixture PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(anyps5_cpu_agc_driver_leaf_fixture PRIVATE anyps5_cpu_agc)
        add_test(NAME anyps5_cpu_agc_driver_leaf COMMAND anyps5_cpu_agc_driver_leaf_fixture)
        set_tests_properties(anyps5_cpu_agc_driver_leaf PROPERTIES TIMEOUT 30)
        # The direct CPU+Metal build does not define platform components. Use
        # the actual existing event provider source in this focused fixture,
        # preserving both direct-build support and wrapper target ordering.
        find_program(agcEventsLinker NAMES ld.lld HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin REQUIRED)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(agcEventsGuest "${CMAKE_CURRENT_BINARY_DIR}/target-agc-events.elf")
        add_custom_command(OUTPUT "${agcEventsGuest}"
            COMMAND "${agcGuestClang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC
                -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
                -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_LIST_DIR}/TargetAgcEventsGuest.c"
                -o "${CMAKE_CURRENT_BINARY_DIR}/target-agc-events.o"
            COMMAND "${agcEventsLinker}" -shared -e _start --hash-style=sysv
                -z max-page-size=4096 -z separate-code -z norelro
                -o "${CMAKE_CURRENT_BINARY_DIR}/target-agc-events-linked.elf"
                "${CMAKE_CURRENT_BINARY_DIR}/target-agc-events.o"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/BuildTargetAgcEventsFixture.py"
                "${CMAKE_CURRENT_BINARY_DIR}/target-agc-events-linked.elf" "${agcEventsGuest}"
            DEPENDS TargetAgcEventsGuest.c BuildTargetAgcEventsFixture.py
                ../platform/tests/BuildKernelEventsFixture.py ../platform/tests/BuildKernelMutexThreadsFixture.py
                ../tests/BuildSceCrtFixture.py VERBATIM)
        add_custom_target(anyps5_agc_target_events_guest DEPENDS "${agcEventsGuest}")
        add_executable(anyps5_cpu_agc_target_events_fixture "${CMAKE_CURRENT_LIST_DIR}/TargetAgcEventsFixture.mm"
            "${PROJECT_SOURCE_DIR}/core/cpu/platform/KernelEvents.cpp")
        add_dependencies(anyps5_cpu_agc_target_events_fixture anyps5_agc_target_events_guest)
        target_compile_features(anyps5_cpu_agc_target_events_fixture PRIVATE cxx_std_20)
        target_compile_options(anyps5_cpu_agc_target_events_fixture PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
        target_include_directories(anyps5_cpu_agc_target_events_fixture PRIVATE "${PROJECT_SOURCE_DIR}/core/cpu/platform")
        target_link_libraries(anyps5_cpu_agc_target_events_fixture PRIVATE anyps5_cpu_agc)
        add_test(NAME anyps5_cpu_agc_target_events
            COMMAND anyps5_cpu_agc_target_events_fixture "${agcEventsGuest}" "${agcUtilityLibrary}")
        set_tests_properties(anyps5_cpu_agc_target_events PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
        set_tests_properties(anyps5_cpu_agc_native_provider PROPERTIES
            TIMEOUT 60 RUN_SERIAL TRUE
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
    endif()
endif()
