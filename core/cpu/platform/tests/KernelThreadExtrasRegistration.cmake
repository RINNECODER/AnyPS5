set(kernelThreadExtrasFixture "${CMAKE_CURRENT_BINARY_DIR}/kernel-thread-extras.elf")
# Unwind tables and PT_GNU_EH_FRAME stay in this fixture: module information reports them.
add_custom_command(OUTPUT "${kernelThreadExtrasFixture}"
    COMMAND "${platformGuestClang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC
        -ffreestanding -fno-builtin -fno-stack-protector -funwind-tables
        -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/KernelThreadExtrasGuest.c"
        -o "${CMAKE_CURRENT_BINARY_DIR}/kernel-thread-extras.o"
    COMMAND "${platformGuestLinker}" -shared -e _start --hash-style=sysv --eh-frame-hdr
        -z max-page-size=4096 -z separate-code -z norelro
        -o "${CMAKE_CURRENT_BINARY_DIR}/kernel-thread-extras-linked.elf"
        "${CMAKE_CURRENT_BINARY_DIR}/kernel-thread-extras.o"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildKernelThreadExtrasFixture.py"
        "${CMAKE_CURRENT_BINARY_DIR}/kernel-thread-extras-linked.elf" "${kernelThreadExtrasFixture}"
    DEPENDS fixtures/KernelThreadExtrasGuest.c tests/BuildKernelThreadExtrasFixture.py
        tests/BuildKernelMutexThreadsFixture.py ../tests/BuildSceCrtFixture.py VERBATIM)
add_custom_target(anyps5_platform_kernel_thread_extras_fixture DEPENDS "${kernelThreadExtrasFixture}")
add_executable(anyps5_platform_kernel_thread_extras_test tests/KernelThreadExtrasTest.cpp)
target_link_libraries(anyps5_platform_kernel_thread_extras_test PRIVATE anyps5_platform_components)
target_compile_options(anyps5_platform_kernel_thread_extras_test PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(anyps5_platform_kernel_thread_extras_test anyps5_platform_kernel_thread_extras_fixture)
foreach(extrasCase IN ITEMS rwlock once tsd sync controls module debug)
    add_test(NAME anyps5_platform_kernel_thread_extras_${extrasCase}
        COMMAND anyps5_platform_kernel_thread_extras_test "${kernelThreadExtrasFixture}" "${extrasCase}")
    set_tests_properties(anyps5_platform_kernel_thread_extras_${extrasCase} PROPERTIES TIMEOUT 30)
endforeach()
