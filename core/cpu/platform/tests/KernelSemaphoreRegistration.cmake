set(kernelSemaphoreFixture "${CMAKE_CURRENT_BINARY_DIR}/kernel-semaphore.elf")
add_custom_command(OUTPUT "${kernelSemaphoreFixture}"
    COMMAND "${platformGuestClang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC
        -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
        -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/KernelSemaphoreGuest.c"
        -o "${CMAKE_CURRENT_BINARY_DIR}/kernel-semaphore.o"
    COMMAND "${platformGuestLinker}" -shared -e _start --hash-style=sysv
        -z max-page-size=4096 -z separate-code -z norelro
        -o "${CMAKE_CURRENT_BINARY_DIR}/kernel-semaphore-linked.elf"
        "${CMAKE_CURRENT_BINARY_DIR}/kernel-semaphore.o"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildKernelSemaphoreFixture.py"
        "${CMAKE_CURRENT_BINARY_DIR}/kernel-semaphore-linked.elf" "${kernelSemaphoreFixture}"
    DEPENDS fixtures/KernelSemaphoreGuest.c tests/BuildKernelSemaphoreFixture.py
        tests/BuildKernelMutexThreadsFixture.py ../tests/BuildSceCrtFixture.py VERBATIM)
add_custom_target(anyps5_platform_kernel_semaphore_fixture DEPENDS "${kernelSemaphoreFixture}")
add_executable(anyps5_platform_kernel_semaphore_test tests/KernelSemaphoreTest.cpp)
target_link_libraries(anyps5_platform_kernel_semaphore_test PRIVATE anyps5_platform_components)
target_compile_options(anyps5_platform_kernel_semaphore_test PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(anyps5_platform_kernel_semaphore_test anyps5_platform_kernel_semaphore_fixture)
foreach(semaphoreCase IN ITEMS priority deletion selected count overflow identity memory lifecycle continuation admission)
    add_test(NAME anyps5_platform_kernel_semaphore_${semaphoreCase}
        COMMAND anyps5_platform_kernel_semaphore_test "${kernelSemaphoreFixture}" "${semaphoreCase}")
    set_tests_properties(anyps5_platform_kernel_semaphore_${semaphoreCase} PROPERTIES TIMEOUT 30)
endforeach()
