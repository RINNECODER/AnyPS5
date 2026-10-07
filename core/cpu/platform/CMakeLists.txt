cmake_minimum_required(VERSION 3.24)

# May be included after anyps5_cpu, or built alone for component acceptance.
# Neither mode changes the production executable's resolver or host modules.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AnyPS5PlatformComponents LANGUAGES C CXX)
    include(CTest)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(ANYPS5_PLATFORM_TCG_LIBRARY "" CACHE FILEPATH "Pinned ARM64 TCG dylib")
    set(ANYPS5_PLATFORM_TCG_INCLUDE "" CACHE PATH "Pinned TCG include directory")
    if(NOT EXISTS "${ANYPS5_PLATFORM_TCG_LIBRARY}" OR
       NOT EXISTS "${ANYPS5_PLATFORM_TCG_INCLUDE}/qemu/anyps5-cpu.h")
        message(FATAL_ERROR "Standalone acceptance needs an explicit pinned TCG library and headers")
    endif()
    add_library(anyps5_platform_machine STATIC ../src/CpuQemu.cpp ../src/SceNpLocalImports.cpp
        ../src/SceElf.cpp ../src/Self.cpp ../src/SceTls.cpp
        ../src/GuestThreads.cpp ../src/SceThreadImports.cpp ../src/SceModules.cpp ../src/SceLifecycleImports.cpp
        ../../libs/nid/src/NidCompute.cpp ../../libs/nid/src/Sha1.cpp)
    target_include_directories(anyps5_platform_machine PUBLIC ../include
        PRIVATE "${ANYPS5_PLATFORM_TCG_INCLUDE}" ../../libs/nid/include)
    target_compile_definitions(anyps5_platform_machine PUBLIC ANYPS5_CPU_MODERN_TCG=1)
    target_compile_options(anyps5_platform_machine PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(anyps5_platform_machine PUBLIC "${ANYPS5_PLATFORM_TCG_LIBRARY}")
    get_filename_component(platformTcgDirectory "${ANYPS5_PLATFORM_TCG_LIBRARY}" DIRECTORY)
    set(CMAKE_BUILD_RPATH "${platformTcgDirectory}")
    set(platformCpuTarget anyps5_platform_machine)
else()
    if(NOT TARGET anyps5_cpu)
        message(FATAL_ERROR "Include platform components after anyps5_cpu")
    endif()
    set(platformCpuTarget anyps5_cpu)
endif()

add_library(anyps5_platform_components STATIC KernelPrimitives.cpp ContentServices.cpp
    NetworkServices.cpp NpServices.cpp AudioServices.cpp RtcServices.cpp CandidateComponents.cpp)
target_include_directories(anyps5_platform_components PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" ../include)
target_compile_features(anyps5_platform_components PUBLIC cxx_std_20)
target_compile_options(anyps5_platform_components PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(anyps5_platform_components PUBLIC ${platformCpuTarget})

if(BUILD_TESTING)
    find_program(platformGuestClang NAMES clang HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(platformGuestLinker NAMES ld.lld HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(platformGuestObjcopy NAMES llvm-objcopy HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    function(platform_component_test domain source entry)
        set(guest "${CMAKE_CURRENT_BINARY_DIR}/${domain}-guest.bin")
        add_custom_command(OUTPUT "${guest}"
            COMMAND "${platformGuestClang}" --target=x86_64-unknown-linux-gnu
                -O1 -ffreestanding -fno-pic -fno-builtin -fno-stack-protector
                -fno-unwind-tables -fno-asynchronous-unwind-tables
                -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${source}Guest.c"
                -o "${CMAKE_CURRENT_BINARY_DIR}/${domain}-guest.o"
            COMMAND "${platformGuestLinker}" -static -e "${entry}"
                -T "${CMAKE_CURRENT_SOURCE_DIR}/tests/GuestText.ld"
                -o "${CMAKE_CURRENT_BINARY_DIR}/${domain}-guest.elf"
                "${CMAKE_CURRENT_BINARY_DIR}/${domain}-guest.o"
            COMMAND "${platformGuestObjcopy}" -O binary --only-section=.text
                "${CMAKE_CURRENT_BINARY_DIR}/${domain}-guest.elf" "${guest}"
            DEPENDS "fixtures/${source}Guest.c" tests/GuestText.ld VERBATIM)
        add_custom_target(anyps5_platform_${domain}_guest DEPENDS "${guest}")
        add_executable(anyps5_platform_${domain}_test "tests/${source}Test.cpp")
        target_link_libraries(anyps5_platform_${domain}_test PRIVATE anyps5_platform_components)
        target_compile_options(anyps5_platform_${domain}_test PRIVATE -Wall -Wextra -Wpedantic)
        add_dependencies(anyps5_platform_${domain}_test anyps5_platform_${domain}_guest)
        add_test(NAME anyps5_platform_${domain}
            COMMAND anyps5_platform_${domain}_test "${guest}")
        set_tests_properties(anyps5_platform_${domain} PROPERTIES TIMEOUT 30)
    endfunction()
    platform_component_test(kernel KernelPrimitives KernelProbe)
    platform_component_test(content ContentServices content_guest)
    platform_component_test(network NetworkServices NetworkServicesGuest)
    platform_component_test(np NpServices NpServicesGuest)
    platform_component_test(audio AudioServices _start)
    platform_component_test(rtc RtcServices rtc_guest)
    add_executable(anyps5_platform_registration_test tests/CandidateComponentsTest.cpp)
    target_link_libraries(anyps5_platform_registration_test PRIVATE anyps5_platform_components)
    add_test(NAME anyps5_platform_registration COMMAND anyps5_platform_registration_test)
    set_tests_properties(anyps5_platform_registration PROPERTIES TIMEOUT 30)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(scePlatformFixture "${CMAKE_CURRENT_BINARY_DIR}/sce-platform.elf")
    add_custom_command(OUTPUT "${scePlatformFixture}"
        COMMAND "${platformGuestClang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC
            -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
            -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ScePlatformGuest.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/sce-platform.o"
        COMMAND "${platformGuestLinker}" -shared -e _start --hash-style=sysv
            -z max-page-size=4096 -z separate-code -z norelro
            -o "${CMAKE_CURRENT_BINARY_DIR}/sce-platform-linked.elf"
            "${CMAKE_CURRENT_BINARY_DIR}/sce-platform.o"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildScePlatformFixture.py"
            "${CMAKE_CURRENT_BINARY_DIR}/sce-platform-linked.elf" "${scePlatformFixture}"
        DEPENDS fixtures/ScePlatformGuest.c tests/BuildScePlatformFixture.py
            ../tests/BuildSceCrtFixture.py VERBATIM)
    add_custom_target(anyps5_platform_sce_fixture DEPENDS "${scePlatformFixture}")
    add_executable(anyps5_platform_sce_test tests/ScePlatformFixtureTest.cpp)
    target_link_libraries(anyps5_platform_sce_test PRIVATE anyps5_platform_components)
    add_dependencies(anyps5_platform_sce_test anyps5_platform_sce_fixture)
    add_test(NAME anyps5_platform_sce COMMAND anyps5_platform_sce_test "${scePlatformFixture}")
    set_tests_properties(anyps5_platform_sce PROPERTIES TIMEOUT 30)
    set(kernelMutexFixture "${CMAKE_CURRENT_BINARY_DIR}/kernel-mutex-threads.elf")
    add_custom_command(OUTPUT "${kernelMutexFixture}"
        COMMAND "${platformGuestClang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC
            -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
            -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/KernelMutexThreadsGuest.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/kernel-mutex-threads.o"
        COMMAND "${platformGuestLinker}" -shared -e _start --hash-style=sysv
            -z max-page-size=4096 -z separate-code -z norelro
            -o "${CMAKE_CURRENT_BINARY_DIR}/kernel-mutex-threads-linked.elf"
            "${CMAKE_CURRENT_BINARY_DIR}/kernel-mutex-threads.o"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildKernelMutexThreadsFixture.py"
            "${CMAKE_CURRENT_BINARY_DIR}/kernel-mutex-threads-linked.elf" "${kernelMutexFixture}"
        DEPENDS fixtures/KernelMutexThreadsGuest.c tests/BuildKernelMutexThreadsFixture.py
            ../tests/BuildSceCrtFixture.py VERBATIM)
    add_custom_target(anyps5_platform_kernel_mutex_fixture DEPENDS "${kernelMutexFixture}")
    add_executable(anyps5_platform_kernel_mutex_threads_test tests/KernelMutexThreadsTest.cpp)
    target_link_libraries(anyps5_platform_kernel_mutex_threads_test PRIVATE anyps5_platform_components)
    target_compile_options(anyps5_platform_kernel_mutex_threads_test PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(anyps5_platform_kernel_mutex_threads_test anyps5_platform_kernel_mutex_fixture)
    add_test(NAME anyps5_platform_kernel_mutex_threads
        COMMAND anyps5_platform_kernel_mutex_threads_test "${kernelMutexFixture}")
    set_tests_properties(anyps5_platform_kernel_mutex_threads PROPERTIES TIMEOUT 30)
    set(kernelPriorityFixture "${CMAKE_CURRENT_BINARY_DIR}/kernel-priority.elf")
    add_custom_command(OUTPUT "${kernelPriorityFixture}"
        COMMAND "${platformGuestClang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC
            -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
            -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/KernelPriorityGuest.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/kernel-priority.o"
        COMMAND "${platformGuestLinker}" -shared -e _start --hash-style=sysv
            -z max-page-size=4096 -z separate-code -z norelro
            -o "${CMAKE_CURRENT_BINARY_DIR}/kernel-priority-linked.elf"
            "${CMAKE_CURRENT_BINARY_DIR}/kernel-priority.o"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildKernelPriorityFixture.py"
            "${CMAKE_CURRENT_BINARY_DIR}/kernel-priority-linked.elf" "${kernelPriorityFixture}"
        DEPENDS fixtures/KernelPriorityGuest.c tests/BuildKernelPriorityFixture.py
            tests/BuildKernelMutexThreadsFixture.py ../tests/BuildSceCrtFixture.py VERBATIM)
    add_custom_target(anyps5_platform_kernel_priority_fixture DEPENDS "${kernelPriorityFixture}")
    add_executable(anyps5_platform_kernel_priority_test tests/KernelPriorityTest.cpp)
    target_link_libraries(anyps5_platform_kernel_priority_test PRIVATE anyps5_platform_components)
    target_compile_options(anyps5_platform_kernel_priority_test PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(anyps5_platform_kernel_priority_test anyps5_platform_kernel_priority_fixture)
    add_test(NAME anyps5_platform_kernel_priority
        COMMAND anyps5_platform_kernel_priority_test "${kernelPriorityFixture}")
    set_tests_properties(anyps5_platform_kernel_priority PROPERTIES TIMEOUT 30)
endif()
