# Include explicitly after the CPU and native Metal targets exist. This finite
# fragment intentionally does not change the production parent's activation.
if(NOT TARGET anyps5_cpu OR NOT TARGET anyps5_metal_native_execution)
    message(FATAL_ERROR "VideoOut fixture requires CPU and native Metal targets")
endif()
if(BUILD_TESTING)
    get_target_property(videoCpuBuildDir anyps5_cpu BINARY_DIR)
    if(ANYPS5_CPU_BACKEND STREQUAL "TCG")
        set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH};${videoCpuBuildDir}/tcg")
    endif()
    find_program(videoGuestClang clang HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(videoGuestObjcopy llvm-objcopy HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    set(videoGuestObject "${CMAKE_CURRENT_BINARY_DIR}/videoout-caller.o")
    set(videoGuestCallers)
    set(videoGuestExtract)
    foreach(service open status close attribute register rate unregister)
        set(caller "${CMAKE_CURRENT_BINARY_DIR}/videoout-${service}.bin")
        list(APPEND videoGuestCallers "${caller}")
        list(APPEND videoGuestExtract --dump-section ".text.videoout_${service}=${caller}")
    endforeach()
    add_custom_command(OUTPUT ${videoGuestCallers}
        COMMAND "${videoGuestClang}" --target=x86_64-unknown-none-elf -O2 -ffreestanding
            -ffunction-sections -fno-stack-protector -fno-asynchronous-unwind-tables
            -fno-optimize-sibling-calls -mno-red-zone -c "${CMAKE_CURRENT_LIST_DIR}/VideoOutCaller.c" -o "${videoGuestObject}"
        COMMAND "${videoGuestObjcopy}" ${videoGuestExtract} "${videoGuestObject}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/VideoOutCaller.c" VERBATIM)
    add_custom_target(anyps5_videoout_callers DEPENDS ${videoGuestCallers})
    add_executable(anyps5_cpu_videoout_native_fixture "${CMAKE_CURRENT_LIST_DIR}/NativeVideoOutFixture.mm")
    add_dependencies(anyps5_cpu_videoout_native_fixture anyps5_videoout_callers)
    target_compile_features(anyps5_cpu_videoout_native_fixture PRIVATE cxx_std_20)
    target_compile_options(anyps5_cpu_videoout_native_fixture PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
    find_library(videoAppKit AppKit REQUIRED)
    target_link_libraries(anyps5_cpu_videoout_native_fixture PRIVATE anyps5_cpu anyps5_metal_native_execution "${videoAppKit}")
    get_target_property(videoUtilityLibrary anyps5_metal_utilities ANYPS5_METALLIB)
    add_test(NAME anyps5_cpu_videoout_native_qualified
        COMMAND anyps5_cpu_videoout_native_fixture "${videoUtilityLibrary}" ${videoGuestCallers})
    add_executable(anyps5_cpu_videoout_admission_fixture "${CMAKE_CURRENT_LIST_DIR}/AdmissionFixture.cpp")
    add_dependencies(anyps5_cpu_videoout_admission_fixture anyps5_videoout_callers)
    target_compile_features(anyps5_cpu_videoout_admission_fixture PRIVATE cxx_std_20)
    target_compile_options(anyps5_cpu_videoout_admission_fixture PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(anyps5_cpu_videoout_admission_fixture PRIVATE anyps5_cpu)
    add_test(NAME anyps5_cpu_videoout_admission COMMAND anyps5_cpu_videoout_admission_fixture ${videoGuestCallers})
    set_tests_properties(anyps5_cpu_videoout_admission PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    set_tests_properties(anyps5_cpu_videoout_native_qualified PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
endif()
