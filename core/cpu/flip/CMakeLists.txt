# Include explicitly after CPU, AGC and native Metal targets exist. The root
# runner owns activation; this finite fragment does not modify parent linkage.
if(NOT TARGET anyps5_cpu OR NOT TARGET anyps5_cpu_agc OR NOT TARGET anyps5_metal_native_execution)
    message(FATAL_ERROR "Flip fixtures require CPU, qualified AGC and native Metal targets")
endif()
if(BUILD_TESTING)
    get_target_property(flipCpuBuildDir anyps5_cpu BINARY_DIR)
    if(ANYPS5_CPU_BACKEND STREQUAL "TCG")
        set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH};${flipCpuBuildDir}/tcg")
    endif()
    find_program(flipGuestClang clang HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(flipGuestObjcopy llvm-objcopy HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    set(flipGuestObject "${CMAKE_CURRENT_BINARY_DIR}/target-flip-caller.o")
    set(flipGuestCallers)
    set(flipGuestExtract)
    foreach(service emit wait sequence submit)
        set(caller "${CMAKE_CURRENT_BINARY_DIR}/target-flip-${service}.bin")
        list(APPEND flipGuestCallers "${caller}")
        list(APPEND flipGuestExtract --dump-section ".text.flip_target_${service}=${caller}")
    endforeach()
    add_custom_command(OUTPUT ${flipGuestCallers}
        BYPRODUCTS "${flipGuestObject}"
        COMMAND "${flipGuestClang}" --target=x86_64-unknown-none-elf -O2 -ffreestanding
            -ffunction-sections -fno-stack-protector -fno-asynchronous-unwind-tables
            -fno-optimize-sibling-calls -mno-red-zone
            -c "${CMAKE_CURRENT_LIST_DIR}/TargetFlipCaller.c" -o "${flipGuestObject}"
        COMMAND "${flipGuestObjcopy}" ${flipGuestExtract} "${flipGuestObject}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/TargetFlipCaller.c" VERBATIM)
    add_custom_target(anyps5_flip_callers DEPENDS ${flipGuestCallers})
    add_executable(anyps5_cpu_flip_contract_fixture "${CMAKE_CURRENT_LIST_DIR}/FlipContractFixture.cpp")
    add_dependencies(anyps5_cpu_flip_contract_fixture anyps5_flip_callers)
    target_compile_features(anyps5_cpu_flip_contract_fixture PRIVATE cxx_std_20)
    target_compile_options(anyps5_cpu_flip_contract_fixture PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(anyps5_cpu_flip_contract_fixture PRIVATE anyps5_cpu_agc)
    add_test(NAME anyps5_cpu_flip_contract COMMAND anyps5_cpu_flip_contract_fixture ${flipGuestCallers})

    add_executable(anyps5_cpu_flip_native_fixture "${CMAKE_CURRENT_LIST_DIR}/NativeFlipFixture.mm")
    add_dependencies(anyps5_cpu_flip_native_fixture anyps5_flip_callers)
    target_compile_features(anyps5_cpu_flip_native_fixture PRIVATE cxx_std_20)
    target_compile_options(anyps5_cpu_flip_native_fixture PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
    find_library(flipAppKit AppKit REQUIRED)
    target_link_libraries(anyps5_cpu_flip_native_fixture PRIVATE anyps5_cpu_agc "${flipAppKit}")
    get_target_property(flipUtilityLibrary anyps5_metal_utilities ANYPS5_METALLIB)
    add_test(NAME anyps5_cpu_flip_native_qualified
        COMMAND anyps5_cpu_flip_native_fixture "${flipUtilityLibrary}" ${flipGuestCallers})
    add_test(NAME anyps5_cpu_flip_native_pending_shutdown
        COMMAND anyps5_cpu_flip_native_fixture "${flipUtilityLibrary}" ${flipGuestCallers} shutdown)
    set_tests_properties(anyps5_cpu_flip_contract PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    set_tests_properties(anyps5_cpu_flip_native_qualified anyps5_cpu_flip_native_pending_shutdown
        PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
endif()
