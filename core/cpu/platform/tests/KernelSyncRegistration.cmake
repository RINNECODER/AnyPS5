set(kernelSyncFixture "${CMAKE_CURRENT_BINARY_DIR}/kernel-sync.elf")
add_custom_command(OUTPUT "${kernelSyncFixture}"
    COMMAND "${platformGuestClang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC
        -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
        -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/KernelSyncGuest.c"
        -o "${CMAKE_CURRENT_BINARY_DIR}/kernel-sync.o"
    COMMAND "${platformGuestLinker}" -shared -e _start --hash-style=sysv
        -z max-page-size=4096 -z separate-code -z norelro
        -o "${CMAKE_CURRENT_BINARY_DIR}/kernel-sync-linked.elf"
        "${CMAKE_CURRENT_BINARY_DIR}/kernel-sync.o"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildKernelSyncFixture.py"
        "${CMAKE_CURRENT_BINARY_DIR}/kernel-sync-linked.elf" "${kernelSyncFixture}"
    DEPENDS fixtures/KernelSyncGuest.c tests/BuildKernelSyncFixture.py
        tests/BuildKernelMutexThreadsFixture.py ../tests/BuildSceCrtFixture.py VERBATIM)
add_custom_target(anyps5_platform_kernel_sync_fixture DEPENDS "${kernelSyncFixture}")
add_executable(anyps5_platform_kernel_sync_test tests/KernelSyncTest.cpp)
target_link_libraries(anyps5_platform_kernel_sync_test PRIVATE anyps5_platform_components)
target_compile_options(anyps5_platform_kernel_sync_test PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(anyps5_platform_kernel_sync_test anyps5_platform_kernel_sync_fixture)
foreach(syncCase IN ITEMS semaphore eventflag equeue stop)
    add_test(NAME anyps5_platform_kernel_sync_${syncCase}
        COMMAND anyps5_platform_kernel_sync_test "${kernelSyncFixture}" "${syncCase}")
    set_tests_properties(anyps5_platform_kernel_sync_${syncCase} PROPERTIES TIMEOUT 60)
endforeach()
