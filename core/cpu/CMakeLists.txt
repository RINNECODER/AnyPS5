enable_language(C)
find_program(unicornPkgConfig pkg-config REQUIRED)
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/3rdparty/unicorn/include/unicorn/unicorn.h")
    message(FATAL_ERROR "Initialize the pinned 3rdparty/unicorn submodule")
endif()
set(UNICORN_ARCH "x86" CACHE STRING "Guest translation architectures" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(UNICORN_LEGACY_STATIC_ARCHIVE OFF CACHE BOOL "" FORCE)
set(savedArchFlags "$ENV{ARCHFLAGS}")
set(ENV{ARCHFLAGS} "-arch arm64")
file(REMOVE "${CMAKE_BINARY_DIR}/config-host.mak" "${CMAKE_BINARY_DIR}/config-host.h")
add_subdirectory("${PROJECT_SOURCE_DIR}/3rdparty/unicorn" "${CMAKE_CURRENT_BINARY_DIR}/unicorn")
set(ENV{ARCHFLAGS} "${savedArchFlags}")
file(READ "${CMAKE_BINARY_DIR}/config-host.h" unicornHostConfig)
if(NOT unicornHostConfig MATCHES "CONFIG_DARWIN" OR NOT unicornHostConfig MATCHES "CONFIG_INT128")
    message(FATAL_ERROR "Unicorn host feature detection failed; inspect configure diagnostics")
endif()

set(ANYPS5_CPU_BACKEND "TCG" CACHE STRING "Embedded application CPU engine: TCG or UNICORN")
set_property(CACHE ANYPS5_CPU_BACKEND PROPERTY STRINGS TCG UNICORN)
set(cpuEngineSource src/Cpu.cpp)
set(cpuEngineLibrary unicorn)
set(cpuModernTcg 0)
if(ANYPS5_CPU_BACKEND STREQUAL "TCG")
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/3rdparty/anyps5-tcg/include/qemu/anyps5-cpu.h")
        message(FATAL_ERROR "Initialize the pinned 3rdparty/anyps5-tcg submodule")
    endif()
    include(ExternalProject)
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    find_program(tcgNinja ninja REQUIRED)
    execute_process(COMMAND "${unicornPkgConfig}" --exists "glib-2.0 >= 2.66"
        RESULT_VARIABLE tcgGlibResult)
    if(NOT tcgGlibResult EQUAL 0)
        message(FATAL_ERROR "The embedded TCG engine requires GLib >= 2.66 development files")
    endif()
    set(tcgBuildDirectory "${CMAKE_CURRENT_BINARY_DIR}/tcg")
    set(tcgLibrary "${tcgBuildDirectory}/libqemu-x86_64-softmmu.dylib")
    ExternalProject_Add(anyps5_tcg_build
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/3rdparty/anyps5-tcg"
        BINARY_DIR "${tcgBuildDirectory}"
        PREFIX "${CMAKE_CURRENT_BINARY_DIR}/tcg-project"
        CONFIGURE_COMMAND <SOURCE_DIR>/configure --python=${Python3_EXECUTABLE} --ninja=${tcgNinja}
            --target-list=x86_64-softmmu --enable-shared-lib --enable-tcg
            --disable-hvf --disable-kvm --disable-cocoa --disable-sdl --disable-tools
            --disable-docs --disable-plugins --without-default-features --disable-pixman --disable-fdt
            "--extra-cflags=-arch arm64" "--extra-ldflags=-arch arm64"
        BUILD_COMMAND "${tcgNinja}" -C <BINARY_DIR> -j 8 libqemu-x86_64-softmmu.dylib
        BUILD_ALWAYS ON
        BUILD_BYPRODUCTS "${tcgLibrary}"
        INSTALL_COMMAND "")
    add_library(anyps5_tcg SHARED IMPORTED)
    set_target_properties(anyps5_tcg PROPERTIES IMPORTED_LOCATION "${tcgLibrary}"
        INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/3rdparty/anyps5-tcg/include")
    set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH};${tcgBuildDirectory}")
    add_dependencies(anyps5_tcg anyps5_tcg_build)
    set(cpuEngineSource src/CpuQemu.cpp)
    set(cpuEngineLibrary anyps5_tcg)
    set(cpuModernTcg 1)
elseif(NOT ANYPS5_CPU_BACKEND STREQUAL "UNICORN")
    message(FATAL_ERROR "ANYPS5_CPU_BACKEND must be TCG or UNICORN")
endif()

add_library(anyps5_cpu STATIC ${cpuEngineSource} src/ElfLoader.cpp src/Runtime.cpp src/SystemMachine.cpp src/SceImports.cpp src/SceElf.cpp src/SceModules.cpp src/Self.cpp src/SceTls.cpp src/GuestFiles.cpp src/SceKernelImports.cpp src/SceUserImports.cpp src/SceSystemImports.cpp src/SceLibcBootstrapImports.cpp src/SceAudioOut2Imports.cpp src/NativeAudioOutput.cpp
    "${PROJECT_SOURCE_DIR}/core/libs/nid/src/NidCompute.cpp"
    "${PROJECT_SOURCE_DIR}/core/libs/nid/src/Sha1.cpp")
target_include_directories(anyps5_cpu PUBLIC include)
target_sources(anyps5_cpu PRIVATE src/SceLifecycleImports.cpp)
target_sources(anyps5_cpu PRIVATE src/GuestThreads.cpp src/SceThreadImports.cpp)
target_sources(anyps5_cpu PRIVATE src/GuestMemoryRuntime.cpp src/SceMemoryImports.cpp src/SceNpLocalImports.cpp src/SceNetAddressImports.cpp src/SceCommonDialogImports.cpp)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB_RECURSE sceNidNameSources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/core/libs/prx/*.cpp")
set(sceNidNames "${CMAKE_CURRENT_BINARY_DIR}/generated/SceNidNames.inc")
add_custom_command(OUTPUT "${sceNidNames}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_sce_nid_names.py" "${PROJECT_SOURCE_DIR}/core/libs/prx" "${sceNidNames}"
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/generate_sce_nid_names.py" "${PROJECT_SOURCE_DIR}/tools/nid_names.py" ${sceNidNameSources} VERBATIM)
target_sources(anyps5_cpu PRIVATE src/SceHostTrampolines.cpp src/SceImportStubs.cpp src/SceNpOfflineImports.cpp "${sceNidNames}")
target_include_directories(anyps5_cpu PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(anyps5_cpu PRIVATE "${PROJECT_SOURCE_DIR}/core/libs/nid/include")
target_link_libraries(anyps5_cpu PRIVATE ${cpuEngineLibrary} unicorn)
find_library(cpuAudioToolbox AudioToolbox REQUIRED)
find_library(cpuCoreAudio CoreAudio REQUIRED)
target_link_libraries(anyps5_cpu PRIVATE ${cpuAudioToolbox} ${cpuCoreAudio})
target_compile_definitions(anyps5_cpu PUBLIC ANYPS5_CPU_MODERN_TCG=${cpuModernTcg})
target_compile_options(anyps5_cpu PRIVATE -Wall -Wextra -Wpedantic)
option(ANYPS5_CPU_NATIVE_MODULE_RUNNER "Activate the native Metal SCE module runner (requires CMake 3.24)" OFF)
add_executable(anyps5_cpu_run src/Main.cpp)
target_compile_definitions(anyps5_cpu_run PRIVATE
    ANYPS5_CPU_NATIVE_MODULE_RUNNER=$<BOOL:${ANYPS5_CPU_NATIVE_MODULE_RUNNER}>)
target_link_libraries(anyps5_cpu_run PRIVATE anyps5_cpu)
add_executable(anyps5_system_probe src/SystemMain.cpp)
target_link_libraries(anyps5_system_probe PRIVATE anyps5_cpu)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(guestClang clang REQUIRED)
    find_program(guestLinker ld.lld HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(guestObjcopy llvm-objcopy HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    foreach(kernelName IN ITEMS kernel-mmu kernel-pagefault)
        set(kernelDefine)
        if(kernelName STREQUAL "kernel-pagefault")
            set(kernelDefine -DFAULT_CASE=1)
        endif()
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${kernelName}.bin"
            COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu ${kernelDefine} -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/KernelMmu.S" -o "${CMAKE_CURRENT_BINARY_DIR}/${kernelName}.o"
            COMMAND "${guestLinker}" -static -e _start --image-base=0 -Ttext=0x1000 -o "${CMAKE_CURRENT_BINARY_DIR}/${kernelName}.elf" "${CMAKE_CURRENT_BINARY_DIR}/${kernelName}.o"
            COMMAND "${guestObjcopy}" -O binary --only-section=.text "${CMAKE_CURRENT_BINARY_DIR}/${kernelName}.elf" "${CMAKE_CURRENT_BINARY_DIR}/${kernelName}.bin"
            DEPENDS fixtures/KernelMmu.S VERBATIM)
    endforeach()
    add_custom_target(anyps5_system_fixtures ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/kernel-mmu.bin" "${CMAKE_CURRENT_BINARY_DIR}/kernel-pagefault.bin")
    add_test_executable(anyps5_system_cpu_tests tests/SystemMachineTests.cpp)
    target_link_libraries(anyps5_system_cpu_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_system_mmu COMMAND anyps5_system_cpu_tests "${CMAKE_CURRENT_BINARY_DIR}/kernel-mmu.bin" "${CMAKE_CURRENT_BINARY_DIR}/kernel-pagefault.bin")
    set_tests_properties(anyps5_system_mmu PROPERTIES TIMEOUT 30)
    set(guestFixture "${CMAKE_CURRENT_BINARY_DIR}/cpu-homebrew.elf")
    add_custom_command(OUTPUT "${guestFixture}"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -march=x86-64 -mno-avx -mno-avx2 -ffreestanding -fno-stack-protector -fno-builtin -fno-pic -O2 -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/Homebrew.c" -o "${CMAKE_CURRENT_BINARY_DIR}/cpu-homebrew.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/Start.S" -o "${CMAKE_CURRENT_BINARY_DIR}/cpu-start.o"
        COMMAND "${guestLinker}" -static -e _start -o "${guestFixture}" "${CMAKE_CURRENT_BINARY_DIR}/cpu-start.o" "${CMAKE_CURRENT_BINARY_DIR}/cpu-homebrew.o"
        DEPENDS fixtures/Homebrew.c fixtures/Start.S VERBATIM)
    add_custom_target(anyps5_cpu_fixture ALL DEPENDS "${guestFixture}")
    set(sceFixture "${CMAKE_CURRENT_BINARY_DIR}/sce-homebrew.elf")
    add_custom_command(OUTPUT "${sceFixture}"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -march=x86-64 -mno-avx -mno-avx2 -ffreestanding -fno-stack-protector -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables -fPIC -O2 -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceHomebrew.c" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-homebrew.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceStart.S" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-start.o"
        COMMAND "${guestLinker}" -shared -Bsymbolic -e _start --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/sce-homebrew-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-start.o" "${CMAKE_CURRENT_BINARY_DIR}/sce-homebrew.o"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildSceFixture.py" "${CMAKE_CURRENT_BINARY_DIR}/sce-homebrew-linked.elf" "${sceFixture}"
        DEPENDS fixtures/SceHomebrew.c fixtures/SceStart.S tests/BuildSceFixture.py VERBATIM)
    add_custom_target(anyps5_sce_fixture ALL DEPENDS "${sceFixture}")
    set(sceModuleGuest "${CMAKE_CURRENT_BINARY_DIR}/SceModuleGuest.prx")
    set(sceModuleMain "${CMAKE_CURRENT_BINARY_DIR}/sce-module-main.elf")
    add_custom_command(OUTPUT "${sceModuleGuest}" "${sceModuleMain}"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceModuleGuest.c" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-module-guest.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceModuleGuest.S" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-module-guest-start.o"
        COMMAND "${guestLinker}" -shared -soname SceModuleGuest.prx -init SceModuleInit -fini SceModuleFini -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/sce-module-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-module-guest.o" "${CMAKE_CURRENT_BINARY_DIR}/sce-module-guest-start.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceModuleMain.c" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-module-main.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceModuleMain.S" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-module-main-start.o"
        COMMAND "${guestLinker}" -shared -e _start -init SceModuleMainInit -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro --no-as-needed -o "${CMAKE_CURRENT_BINARY_DIR}/sce-module-main-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-module-main.o" "${CMAKE_CURRENT_BINARY_DIR}/sce-module-main-start.o" "${CMAKE_CURRENT_BINARY_DIR}/sce-module-guest-linked.elf"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildSceModulesFixture.py" "${CMAKE_CURRENT_BINARY_DIR}/sce-module-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-module-main-linked.elf" "${sceModuleGuest}" "${sceModuleMain}"
        DEPENDS fixtures/SceModuleGuest.c fixtures/SceModuleGuest.S fixtures/SceModuleMain.c fixtures/SceModuleMain.S tests/BuildSceModulesFixture.py VERBATIM)
    add_custom_target(anyps5_sce_module_fixture ALL DEPENDS "${sceModuleGuest}" "${sceModuleMain}")
    set(sceCrtDirectory "${CMAKE_CURRENT_BINARY_DIR}/sce-crt")
    set(sceCrtReceipt "${sceCrtDirectory}/crt-receipt.txt")
    add_custom_command(OUTPUT "${sceCrtReceipt}" "${sceCrtDirectory}/sce-crt-main.elf" "${sceCrtDirectory}/raw/SceCrtGuest.prx" "${sceCrtDirectory}/plain-self/SceCrtGuest.prx"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceCrtGuest.c" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-crt-guest.o"
        COMMAND "${guestLinker}" -shared -soname SceCrtGuest.prx -init SceCrtInit -fini SceCrtFini -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/sce-crt-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-crt-guest.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceCrtMain.S" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-crt-main.o"
        COMMAND "${guestLinker}" -shared -e _start -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro --no-as-needed -o "${CMAKE_CURRENT_BINARY_DIR}/sce-crt-main-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-crt-main.o" "${CMAKE_CURRENT_BINARY_DIR}/sce-crt-guest-linked.elf"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildSceCrtFixture.py" "${CMAKE_CURRENT_BINARY_DIR}/sce-crt-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-crt-main-linked.elf" "${sceCrtDirectory}"
        DEPENDS fixtures/SceCrtGuest.c fixtures/SceCrtMain.S tests/BuildSceCrtFixture.py VERBATIM)
    add_custom_target(anyps5_sce_crt_fixture ALL DEPENDS "${sceCrtReceipt}" "${sceCrtDirectory}/sce-crt-main.elf" "${sceCrtDirectory}/raw/SceCrtGuest.prx" "${sceCrtDirectory}/plain-self/SceCrtGuest.prx")
    add_test_executable(anyps5_sce_modules_tests tests/SceModulesTests.cpp)
    target_link_libraries(anyps5_sce_modules_tests PRIVATE anyps5_cpu)
    add_dependencies(anyps5_sce_modules_tests anyps5_sce_module_fixture anyps5_sce_crt_fixture)
    add_test(NAME anyps5_sce_modules COMMAND anyps5_sce_modules_tests "${sceModuleMain}" "${sceModuleGuest}" "${sceCrtReceipt}")
    set_tests_properties(anyps5_sce_modules PROPERTIES TIMEOUT 60)
    add_test(NAME anyps5_sce_module_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/SceModuleCliTest.py" "$<TARGET_FILE:anyps5_cpu_run>" "${CMAKE_CURRENT_BINARY_DIR}/sce-module-main.elf" "${CMAKE_CURRENT_BINARY_DIR}/SceModuleGuest.prx" "${guestFixture}")
    set_tests_properties(anyps5_sce_module_cli PROPERTIES TIMEOUT 60)
    set(mainLifecycleDirectory "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle")
    set(mainLifecycleReceipt "${mainLifecycleDirectory}/SceMainLifecycleReceipt.txt")
    add_custom_command(OUTPUT "${mainLifecycleReceipt}"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceMainLifecycleRuntime.c" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-runtime.o"
        COMMAND "${guestLinker}" -shared -soname SceMainLifecycleRuntime.prx -init SceMainLifecycleDependencyInit -fini SceMainLifecycleDependencyFini -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-runtime-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-runtime.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceMainLifecycleGuest.c" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-guest.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceMainLifecycleStart.S" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-start.o"
        COMMAND "${guestLinker}" -shared -e _start -init SceMainLifecycleInit -fini SceMainLifecycleFini -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro --no-as-needed -o "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-main-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-guest.o" "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-start.o" "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-runtime-linked.elf"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/SceMainLifecycleBuild.py" "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-runtime-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-main-lifecycle-main-linked.elf" "${mainLifecycleDirectory}"
        DEPENDS fixtures/SceMainLifecycleRuntime.c fixtures/SceMainLifecycleGuest.c fixtures/SceMainLifecycleStart.S tests/SceMainLifecycleBuild.py tests/BuildSceCrtFixture.py VERBATIM)
    add_custom_target(anyps5_sce_main_lifecycle_fixture ALL DEPENDS "${mainLifecycleReceipt}")
    add_test_executable(anyps5_sce_main_lifecycle_tests tests/SceMainLifecycleTests.cpp)
    target_link_libraries(anyps5_sce_main_lifecycle_tests PRIVATE anyps5_cpu)
    add_dependencies(anyps5_sce_main_lifecycle_tests anyps5_sce_main_lifecycle_fixture)
    add_test(NAME anyps5_sce_main_lifecycle COMMAND anyps5_sce_main_lifecycle_tests "${mainLifecycleReceipt}")
    set_tests_properties(anyps5_sce_main_lifecycle PROPERTIES TIMEOUT 60)
    set(libcInternalDirectory "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal")
    set(libcInternalReceipt "${libcInternalDirectory}/receipt.txt")
    add_custom_command(OUTPUT "${libcInternalReceipt}"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/tests/SceLibcInternalGuest.c" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-guest.o"
        COMMAND "${guestLinker}" -shared -soname SceInternalLibc.prx -init SceInternalInit -fini SceInternalFini -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-guest.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/tests/SceLibcInternalConsumer.c" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-consumer.o"
        COMMAND "${guestLinker}" -shared -soname SceInternalConsumer.prx -init SceInternalConsumerInit -fini SceInternalConsumerFini -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-consumer-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-consumer.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/tests/SceLibcInternalMain.c" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-main.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/tests/SceLibcInternalStart.S" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-start.o"
        COMMAND "${guestLinker}" -shared -e _start -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro --no-as-needed -o "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-main-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-main.o" "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-start.o" "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-consumer-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-guest-linked.elf"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildSceLibcInternalFixture.py" "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-consumer-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-libc-internal-main-linked.elf" "${libcInternalDirectory}"
        DEPENDS tests/SceLibcInternalGuest.c tests/SceLibcInternalConsumer.c tests/SceLibcInternalMain.c tests/SceLibcInternalStart.S tests/BuildSceLibcInternalFixture.py tests/BuildSceCrtFixture.py VERBATIM)
    add_custom_target(anyps5_sce_libc_internal_fixture ALL DEPENDS "${libcInternalReceipt}")
    add_test_executable(anyps5_sce_libc_internal_tests tests/SceLibcInternalTests.cpp)
    target_link_libraries(anyps5_sce_libc_internal_tests PRIVATE anyps5_cpu)
    add_dependencies(anyps5_sce_libc_internal_tests anyps5_sce_libc_internal_fixture)
    add_test(NAME anyps5_sce_libc_internal COMMAND anyps5_sce_libc_internal_tests "${libcInternalReceipt}")
    set_tests_properties(anyps5_sce_libc_internal PROPERTIES TIMEOUT 60)
    add_test_executable(anyps5_cpu_tests tests/CpuTests.cpp)
    target_link_libraries(anyps5_cpu_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_cpu_instructions COMMAND anyps5_cpu_tests)
    if(ANYPS5_CPU_BACKEND STREQUAL "TCG")
        set(threadGuest "${CMAKE_CURRENT_BINARY_DIR}/ThreadGuest.prx")
        set(threadMain "${CMAKE_CURRENT_BINARY_DIR}/thread-main.elf")
        add_custom_command(OUTPUT "${threadGuest}" "${threadMain}"
            COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -DTHREAD_DEPENDENCY=1 -mavx2 -mno-red-zone -O2 -ffreestanding -fPIC -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ThreadHomebrew.c" -o "${CMAKE_CURRENT_BINARY_DIR}/thread-dependency.o"
            COMMAND "${guestLinker}" -shared -soname ThreadGuest.prx -init ThreadDependencyInit -fini ThreadDependencyFini -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/thread-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/thread-dependency.o"
            COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -mavx2 -mno-red-zone -O2 -ffreestanding -fPIC -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ThreadHomebrew.c" -o "${CMAKE_CURRENT_BINARY_DIR}/thread-main.o"
            COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceStart.S" -o "${CMAKE_CURRENT_BINARY_DIR}/thread-start.o"
            COMMAND "${guestLinker}" -shared -e _start -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro --no-as-needed -o "${CMAKE_CURRENT_BINARY_DIR}/thread-main-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/thread-start.o" "${CMAKE_CURRENT_BINARY_DIR}/thread-main.o" "${CMAKE_CURRENT_BINARY_DIR}/thread-guest-linked.elf"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/BuildThreadFixture.py" "${CMAKE_CURRENT_BINARY_DIR}/thread-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/thread-main-linked.elf" "${threadGuest}" "${threadMain}"
            DEPENDS fixtures/ThreadHomebrew.c fixtures/SceStart.S fixtures/BuildThreadFixture.py tests/BuildSceModulesFixture.py VERBATIM)
        add_custom_target(anyps5_guest_thread_fixture ALL DEPENDS "${threadGuest}" "${threadMain}")
        add_test_executable(anyps5_guest_thread_tests tests/GuestThreadsTests.cpp)
        target_link_libraries(anyps5_guest_thread_tests PRIVATE anyps5_cpu)
        add_dependencies(anyps5_guest_thread_tests anyps5_guest_thread_fixture)
        add_test(NAME anyps5_guest_threads COMMAND anyps5_guest_thread_tests "${threadMain}" "${threadGuest}")
        set_tests_properties(anyps5_guest_threads PROPERTIES TIMEOUT 60)
        add_test_executable(anyps5_cpu_context_tests tests/ContextTests.cpp)
        target_link_libraries(anyps5_cpu_context_tests PRIVATE anyps5_cpu)
        add_test(NAME anyps5_cpu_contexts COMMAND anyps5_cpu_context_tests)
        set_tests_properties(anyps5_cpu_contexts PROPERTIES TIMEOUT 30)
    endif()
    add_test_executable(anyps5_cpu_loader_tests tests/ElfLoaderTests.cpp)
    target_link_libraries(anyps5_cpu_loader_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_cpu_loader COMMAND anyps5_cpu_loader_tests)
    add_test_executable(anyps5_cpu_runtime_tests tests/RuntimeTests.cpp)
    target_link_libraries(anyps5_cpu_runtime_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_cpu_runtime COMMAND anyps5_cpu_runtime_tests)
    add_test_executable(anyps5_sce_import_stub_tests tests/SceImportStubsTests.cpp)
    target_link_libraries(anyps5_sce_import_stub_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_sce_import_stubs COMMAND anyps5_sce_import_stub_tests)
    set_tests_properties(anyps5_sce_import_stubs PROPERTIES TIMEOUT 30)
    add_test_executable(anyps5_sce_import_tests tests/SceImportsTests.cpp)
    target_link_libraries(anyps5_sce_import_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_sce_imports COMMAND anyps5_sce_import_tests)
    add_test_executable(anyps5_sce_loader_tests tests/SceElfTests.cpp)
    target_link_libraries(anyps5_sce_loader_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_sce_loader COMMAND anyps5_sce_loader_tests)
    set(tlsFixture "${CMAKE_CURRENT_BINARY_DIR}/sce-tls.bin")
    add_custom_command(OUTPUT "${tlsFixture}"
        COMMAND "${guestClang}" --target=x86_64-unknown-freebsd -DANYPS5_GUEST_TLS_FIXTURE -x c -ftls-model=local-exec -ffreestanding -fno-stack-protector -fno-pic -O2 -c "${CMAKE_CURRENT_SOURCE_DIR}/tests/SceTlsTests.cpp" -o "${CMAKE_CURRENT_BINARY_DIR}/sce-tls.o"
        COMMAND "${guestLinker}" -static -e GuestTlsProbe --image-base=0 -Ttext=0x1000 -o "${CMAKE_CURRENT_BINARY_DIR}/sce-tls.elf" "${CMAKE_CURRENT_BINARY_DIR}/sce-tls.o"
        COMMAND "${guestObjcopy}" -O binary --only-section=.text "${CMAKE_CURRENT_BINARY_DIR}/sce-tls.elf" "${tlsFixture}"
        DEPENDS tests/SceTlsTests.cpp VERBATIM)
    add_custom_target(anyps5_sce_tls_fixture ALL DEPENDS "${tlsFixture}")
    add_test_executable(anyps5_sce_tls_tests tests/SceTlsTests.cpp)
    target_link_libraries(anyps5_sce_tls_tests PRIVATE anyps5_cpu)
    add_dependencies(anyps5_sce_tls_tests anyps5_sce_tls_fixture)
    add_test(NAME anyps5_sce_tls COMMAND anyps5_sce_tls_tests "${tlsFixture}")
    add_test_executable(anyps5_guest_files_tests tests/GuestFilesTests.cpp)
    add_test_executable(anyps5_guest_memory_tests tests/GuestMemoryRuntimeTests.cpp)
    target_link_libraries(anyps5_guest_memory_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_guest_memory COMMAND anyps5_guest_memory_tests)
    add_test_executable(anyps5_sce_memory_import_tests tests/SceMemoryImportsTests.cpp)
    target_link_libraries(anyps5_sce_memory_import_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_sce_memory_imports COMMAND anyps5_sce_memory_import_tests)
    set_tests_properties(anyps5_guest_memory anyps5_sce_memory_imports PROPERTIES TIMEOUT 30)
    target_link_libraries(anyps5_guest_files_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_guest_files COMMAND anyps5_guest_files_tests)
    add_test_executable(anyps5_sce_kernel_import_tests tests/SceKernelImportsTests.cpp)
    target_link_libraries(anyps5_sce_kernel_import_tests PRIVATE anyps5_cpu)
    # The sanitizer replacement tables must agree with the prx ABI structures they mirror.
    target_include_directories(anyps5_sce_kernel_import_tests PRIVATE "${PROJECT_SOURCE_DIR}/core/libs")
    add_test(NAME anyps5_sce_kernel_imports COMMAND anyps5_sce_kernel_import_tests)
    set_tests_properties(anyps5_sce_tls anyps5_guest_files anyps5_sce_kernel_imports PROPERTIES TIMEOUT 30)
    add_test_executable(anyps5_sce_user_import_tests tests/SceUserImportsTest.cpp)
    target_link_libraries(anyps5_sce_user_import_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_sce_user_imports COMMAND anyps5_sce_user_import_tests)
    set_tests_properties(anyps5_sce_user_imports PROPERTIES TIMEOUT 30)
    add_test_executable(anyps5_sce_system_import_tests tests/SceSystemImportsTest.cpp)
    target_link_libraries(anyps5_sce_system_import_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_sce_system_imports COMMAND anyps5_sce_system_import_tests)
    set_tests_properties(anyps5_sce_system_imports PROPERTIES TIMEOUT 30)
    add_test(NAME anyps5_cpu_cli_stop_signals COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CliStopSignalTest.py" "$<TARGET_FILE:anyps5_cpu_run>")
    set_tests_properties(anyps5_cpu_cli_stop_signals PROPERTIES TIMEOUT 90)
    set(platformGuest "${CMAKE_CURRENT_BINARY_DIR}/PlatformServiceGuest.prx")
    set(platformMain "${CMAKE_CURRENT_BINARY_DIR}/platform-service-main.elf")
    add_custom_command(OUTPUT "${platformGuest}" "${platformMain}"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -DBUILD_PLATFORM_DEPENDENCY=1 -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/PlatformServiceHomebrew.c" -o "${CMAKE_CURRENT_BINARY_DIR}/platform-guest.o"
        COMMAND "${guestLinker}" -shared -soname PlatformServiceGuest.prx -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/platform-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/platform-guest.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -DBUILD_PLATFORM_DEPENDENCY=0 -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/PlatformServiceHomebrew.c" -o "${CMAKE_CURRENT_BINARY_DIR}/platform-main.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceStart.S" -o "${CMAKE_CURRENT_BINARY_DIR}/platform-start.o"
        COMMAND "${guestLinker}" -shared -e _start -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro --no-as-needed -o "${CMAKE_CURRENT_BINARY_DIR}/platform-main-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/platform-main.o" "${CMAKE_CURRENT_BINARY_DIR}/platform-start.o" "${CMAKE_CURRENT_BINARY_DIR}/platform-guest-linked.elf"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildPlatformServiceFixture.py" "${CMAKE_CURRENT_BINARY_DIR}/platform-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/platform-main-linked.elf" "${platformGuest}" "${platformMain}"
        DEPENDS fixtures/PlatformServiceHomebrew.c fixtures/SceStart.S tests/BuildPlatformServiceFixture.py tests/BuildSceModulesFixture.py VERBATIM)
    add_custom_target(anyps5_platform_service_fixture ALL DEPENDS "${platformGuest}" "${platformMain}")
    if(ANYPS5_CPU_BACKEND STREQUAL "TCG")
        add_test(NAME anyps5_platform_service_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/PlatformServiceCliTest.py" "$<TARGET_FILE:anyps5_cpu_run>" "${platformMain}" "${platformGuest}")
        set_tests_properties(anyps5_platform_service_cli PROPERTIES TIMEOUT 30)
    endif()
    set(lazyImportGuest "${CMAKE_CURRENT_BINARY_DIR}/LazyImportGuest.prx")
    set(lazyImportMain "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-main.elf")
    add_custom_command(OUTPUT "${lazyImportGuest}" "${lazyImportMain}"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -DBUILD_LAZY_DEPENDENCY=1 -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/LazyImportHomebrew.c" -o "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-guest.o"
        COMMAND "${guestLinker}" -shared -soname LazyImportGuest.prx -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-guest.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -DBUILD_LAZY_DEPENDENCY=0 -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/LazyImportHomebrew.c" -o "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-main.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceStart.S" -o "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-start.o"
        COMMAND "${guestLinker}" -shared -e _start -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro --no-as-needed -o "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-main-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-main.o" "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-start.o" "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-guest-linked.elf"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildLazyImportFixture.py" "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/lazy-import-main-linked.elf" "${lazyImportGuest}" "${lazyImportMain}"
        DEPENDS fixtures/LazyImportHomebrew.c fixtures/SceStart.S tests/BuildLazyImportFixture.py tests/BuildPlatformServiceFixture.py tests/BuildSceModulesFixture.py VERBATIM)
    add_custom_target(anyps5_lazy_import_fixture ALL DEPENDS "${lazyImportGuest}" "${lazyImportMain}")
    add_test(NAME anyps5_lazy_import_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/LazyImportCliTest.py" "$<TARGET_FILE:anyps5_cpu_run>" "${lazyImportMain}" "${lazyImportGuest}")
    set_tests_properties(anyps5_lazy_import_cli PROPERTIES TIMEOUT 60)
    set(manyImportsSource "${CMAKE_CURRENT_BINARY_DIR}/many-imports.c")
    set(manyImportsGuest "${CMAKE_CURRENT_BINARY_DIR}/ManyImportsGuest.prx")
    set(manyImportsMain "${CMAKE_CURRENT_BINARY_DIR}/many-imports-main.elf")
    add_custom_command(OUTPUT "${manyImportsGuest}" "${manyImportsMain}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildManyImportsFixture.py" source "${manyImportsSource}"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -DBUILD_MANY_DEPENDENCY=1 -c "${manyImportsSource}" -o "${CMAKE_CURRENT_BINARY_DIR}/many-imports-guest.o"
        COMMAND "${guestLinker}" -shared -soname ManyImportsGuest.prx -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/many-imports-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/many-imports-guest.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -ffreestanding -fPIC -O2 -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -DBUILD_MANY_DEPENDENCY=0 -c "${manyImportsSource}" -o "${CMAKE_CURRENT_BINARY_DIR}/many-imports-main.o"
        COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/SceStart.S" -o "${CMAKE_CURRENT_BINARY_DIR}/many-imports-start.o"
        COMMAND "${guestLinker}" -shared -e _start -Bsymbolic --hash-style=sysv -z max-page-size=4096 -z separate-code -z norelro --no-as-needed -o "${CMAKE_CURRENT_BINARY_DIR}/many-imports-main-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/many-imports-main.o" "${CMAKE_CURRENT_BINARY_DIR}/many-imports-start.o" "${CMAKE_CURRENT_BINARY_DIR}/many-imports-guest-linked.elf"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildManyImportsFixture.py" package "${CMAKE_CURRENT_BINARY_DIR}/many-imports-guest-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/many-imports-main-linked.elf" "${manyImportsGuest}" "${manyImportsMain}"
        DEPENDS fixtures/SceStart.S tests/BuildManyImportsFixture.py tests/BuildPlatformServiceFixture.py tests/BuildSceModulesFixture.py VERBATIM)
    add_custom_target(anyps5_many_imports_fixture ALL DEPENDS "${manyImportsGuest}" "${manyImportsMain}")
    add_test(NAME anyps5_many_imports_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ManyImportsCliTest.py" "$<TARGET_FILE:anyps5_cpu_run>" "${manyImportsMain}" "${manyImportsGuest}")
    set_tests_properties(anyps5_many_imports_cli PROPERTIES TIMEOUT 120)
    add_test_executable(anyps5_sce_common_dialog_import_tests tests/SceCommonDialogImportsTest.cpp)
    target_link_libraries(anyps5_sce_common_dialog_import_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_sce_common_dialog_imports COMMAND anyps5_sce_common_dialog_import_tests)
    set_tests_properties(anyps5_sce_common_dialog_imports PROPERTIES TIMEOUT 30)
    add_test_executable(anyps5_sce_np_local_import_tests tests/SceNpLocalImportsTest.cpp)
    target_link_libraries(anyps5_sce_np_local_import_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_sce_np_local_imports COMMAND anyps5_sce_np_local_import_tests)
    add_test_executable(anyps5_sce_net_address_import_tests tests/SceNetAddressImportsTest.cpp)
    target_link_libraries(anyps5_sce_net_address_import_tests PRIVATE anyps5_cpu)
    set_tests_properties(anyps5_sce_np_local_imports PROPERTIES TIMEOUT 30)
    if(ANYPS5_CPU_BACKEND STREQUAL "TCG")
        add_test(NAME anyps5_sce_net_address_imports COMMAND anyps5_sce_net_address_import_tests)
        set_tests_properties(anyps5_sce_net_address_imports PROPERTIES TIMEOUT 30)
    endif()
    add_test_executable(anyps5_sce_libc_bootstrap_import_tests tests/SceLibcBootstrapImportsTest.cpp)
    target_link_libraries(anyps5_sce_libc_bootstrap_import_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_sce_libc_bootstrap_imports COMMAND anyps5_sce_libc_bootstrap_import_tests)
    set_tests_properties(anyps5_sce_libc_bootstrap_imports PROPERTIES TIMEOUT 30)
    add_test_executable(anyps5_sce_audioout2_import_tests tests/SceAudioOut2ImportsTest.cpp)
    target_link_libraries(anyps5_sce_audioout2_import_tests PRIVATE anyps5_cpu)
    add_test(NAME anyps5_sce_audioout2_imports COMMAND anyps5_sce_audioout2_import_tests)
    set_tests_properties(anyps5_sce_audioout2_imports PROPERTIES TIMEOUT 30)
    add_test_executable(anyps5_self_tests tests/SelfTests.cpp)
    target_link_libraries(anyps5_self_tests PRIVATE anyps5_cpu)
    add_dependencies(anyps5_self_tests anyps5_sce_fixture)
    add_test(NAME anyps5_plain_self COMMAND anyps5_self_tests "${sceFixture}")
    add_test(NAME anyps5_sce_homebrew COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/SceHomebrewTest.py" "$<TARGET_FILE:anyps5_cpu_run>" "${sceFixture}")
    add_test(NAME anyps5_cpu_homebrew COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/HomebrewTest.py" "$<TARGET_FILE:anyps5_cpu_run>" "${guestFixture}")
    add_test(NAME anyps5_cpu_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/CliTest.py" "$<TARGET_FILE:anyps5_cpu_run>" "${guestFixture}" "${ANYPS5_CPU_BACKEND}")
    add_test(NAME anyps5_cpu_run_limits COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/RunLimitsCliTest.py" "$<TARGET_FILE:anyps5_cpu_run>")
    set_tests_properties(anyps5_cpu_run_limits PROPERTIES TIMEOUT 240)
    set_tests_properties(anyps5_cpu_instructions anyps5_cpu_loader anyps5_cpu_runtime anyps5_sce_imports anyps5_sce_loader anyps5_plain_self anyps5_sce_homebrew anyps5_cpu_homebrew anyps5_cpu_cli PROPERTIES TIMEOUT 30)
    if(cpuModernTcg)
        set_tests_properties(anyps5_sce_homebrew PROPERTIES TIMEOUT 60)
    endif()
endif()

set(ANYPS5_CPU_METAL_SOURCE_DIR "" CACHE PATH "Committed native Metal source for CPU integration")
if(ANYPS5_CPU_NATIVE_MODULE_RUNNER AND (NOT cpuModernTcg OR NOT ANYPS5_CPU_METAL_SOURCE_DIR))
    message(FATAL_ERROR "The native module runner requires TCG and ANYPS5_CPU_METAL_SOURCE_DIR")
endif()
if(ANYPS5_CPU_METAL_SOURCE_DIR)
    if(NOT BUILD_TESTING AND NOT ANYPS5_CPU_NATIVE_MODULE_RUNNER)
        message(FATAL_ERROR "The CPU to Metal guest fixture requires BUILD_TESTING=ON")
    endif()
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(ANYPS5_METAL_UTILITIES_ONLY ON)
    set(ANYPS5_METAL_SHADER_BRIDGE ON)
    set(ANYPS5_METAL_GUEST_REPLAY ON)
    set(ANYPS5_CPU_RUNTIME_ONLY OFF)
    add_subdirectory("${ANYPS5_CPU_METAL_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/metal-source")
    target_sources(anyps5_cpu PRIVATE src/GuestMemoryMetal.cpp src/SceVideoOutImports.cpp src/SceNativeVideoOutBackend.cpp)
    target_link_libraries(anyps5_cpu PRIVATE anyps5_metal_native_execution)
    find_library(cpuAppKit AppKit REQUIRED)
    get_target_property(cpuMetalLibrary anyps5_metal_utilities ANYPS5_METALLIB)
    if(BUILD_TESTING)
        add_test_executable(anyps5_cpu_metal_test tests/MetalGuestTest.mm)
        target_compile_options(anyps5_cpu_metal_test PRIVATE -fobjc-arc)
        target_link_libraries(anyps5_cpu_metal_test PRIVATE anyps5_cpu anyps5_metal_native_execution "${cpuAppKit}")
        set(metalFixture "${CMAKE_CURRENT_BINARY_DIR}/cpu-metal-homebrew.elf")
        add_custom_command(OUTPUT "${metalFixture}"
            COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -march=x86-64 -mno-avx -mno-avx2 -ffreestanding -fno-stack-protector -fno-builtin -fno-pic -O2 -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/MetalHomebrew.c" -o "${CMAKE_CURRENT_BINARY_DIR}/cpu-metal-homebrew.o"
            COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/Start.S" -o "${CMAKE_CURRENT_BINARY_DIR}/cpu-metal-start.o"
            COMMAND "${guestLinker}" -static -e _start --image-base=0x4000000 -o "${metalFixture}" "${CMAKE_CURRENT_BINARY_DIR}/cpu-metal-start.o" "${CMAKE_CURRENT_BINARY_DIR}/cpu-metal-homebrew.o"
            DEPENDS fixtures/MetalHomebrew.c fixtures/Start.S VERBATIM)
        add_custom_target(anyps5_cpu_metal_fixture ALL DEPENDS "${metalFixture}")
        add_test(NAME anyps5_cpu_metal_guest COMMAND anyps5_cpu_metal_test "${metalFixture}" "${cpuMetalLibrary}")
        set_tests_properties(anyps5_cpu_metal_guest PROPERTIES TIMEOUT 120)
        add_test_executable(anyps5_cpu_memory_metal_test tests/MemoryMetalGuestTest.mm)
        target_compile_options(anyps5_cpu_memory_metal_test PRIVATE -fobjc-arc)
        target_link_libraries(anyps5_cpu_memory_metal_test PRIVATE anyps5_cpu anyps5_metal_native_execution "${cpuAppKit}")
        set(memoryMetalFixture "${CMAKE_CURRENT_BINARY_DIR}/cpu-memory-metal-homebrew.elf")
        add_custom_command(OUTPUT "${memoryMetalFixture}"
            COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -march=x86-64 -mno-avx -mno-avx2 -ffreestanding -fno-stack-protector -fno-builtin -fno-pic -O2 -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/MemoryMetalHomebrew.c" -o "${CMAKE_CURRENT_BINARY_DIR}/cpu-memory-metal-homebrew.o"
            COMMAND "${guestClang}" --target=x86_64-unknown-linux-gnu -c "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/Start.S" -o "${CMAKE_CURRENT_BINARY_DIR}/cpu-memory-metal-start.o"
            COMMAND "${guestLinker}" -static -e _start --image-base=0x4000000 -o "${memoryMetalFixture}" "${CMAKE_CURRENT_BINARY_DIR}/cpu-memory-metal-start.o" "${CMAKE_CURRENT_BINARY_DIR}/cpu-memory-metal-homebrew.o"
            DEPENDS fixtures/MemoryMetalHomebrew.c fixtures/Start.S VERBATIM)
        add_custom_target(anyps5_cpu_memory_metal_fixture ALL DEPENDS "${memoryMetalFixture}")
        add_test(NAME anyps5_cpu_memory_metal_guest COMMAND anyps5_cpu_memory_metal_test "${memoryMetalFixture}" "${cpuMetalLibrary}")
        if(cpuModernTcg)
            add_test(NAME anyps5_cpu_memory_metal_guest_owned COMMAND anyps5_cpu_memory_metal_test "${memoryMetalFixture}" "${cpuMetalLibrary}" owned)
            add_test(NAME anyps5_cpu_memory_metal_guest_composition COMMAND anyps5_cpu_memory_metal_test "${memoryMetalFixture}" "${cpuMetalLibrary}" composition)
            set_tests_properties(anyps5_cpu_memory_metal_guest_owned anyps5_cpu_memory_metal_guest_composition PROPERTIES TIMEOUT 120)
            add_test_executable(anyps5_owned_memory_metal_mutation_test tests/OwnedMemoryMetalMutationTest.mm)
            target_compile_options(anyps5_owned_memory_metal_mutation_test PRIVATE -fobjc-arc)
            target_link_libraries(anyps5_owned_memory_metal_mutation_test PRIVATE anyps5_cpu anyps5_metal_native_execution "${cpuAppKit}")
            add_test(NAME anyps5_owned_memory_metal_mutation COMMAND anyps5_owned_memory_metal_mutation_test "${cpuMetalLibrary}")
            set_tests_properties(anyps5_owned_memory_metal_mutation PROPERTIES TIMEOUT 120)
        endif()
        add_test_executable(anyps5_sce_videoout_import_tests tests/SceVideoOutImportsTests.cpp)
        target_link_libraries(anyps5_sce_videoout_import_tests PRIVATE anyps5_cpu anyps5_metal_native_execution)
        add_test(NAME anyps5_sce_videoout_imports COMMAND anyps5_sce_videoout_import_tests)
        add_test_executable(anyps5_sce_native_videoout_tests tests/SceNativeVideoOutBackendTest.mm)
        target_compile_options(anyps5_sce_native_videoout_tests PRIVATE -fobjc-arc)
        target_link_libraries(anyps5_sce_native_videoout_tests PRIVATE anyps5_cpu anyps5_metal_native_execution "${cpuAppKit}")
        add_test(NAME anyps5_sce_native_videoout COMMAND anyps5_sce_native_videoout_tests "${cpuMetalLibrary}")
        set_tests_properties(anyps5_cpu_memory_metal_guest anyps5_sce_videoout_imports anyps5_sce_native_videoout PROPERTIES TIMEOUT 120)
        add_test_executable(anyps5_native_shutdown_tests tests/NativeShutdownTest.mm)
        target_compile_options(anyps5_native_shutdown_tests PRIVATE -fobjc-arc)
        target_link_libraries(anyps5_native_shutdown_tests PRIVATE anyps5_cpu anyps5_metal_native_execution "${cpuAppKit}")
        foreach(shutdownMode rendering-wait gpu-ready)
            add_test(NAME anyps5_native_shutdown_${shutdownMode}
                COMMAND anyps5_native_shutdown_tests "${cpuMetalLibrary}" "${shutdownMode}")
            set_tests_properties(anyps5_native_shutdown_${shutdownMode} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
                ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
        endforeach()
    endif()
    if(ANYPS5_CPU_NATIVE_MODULE_RUNNER)
        if(NOT TARGET anyps5_platform_components)
            add_subdirectory(platform)
        endif()
        include("${CMAKE_CURRENT_SOURCE_DIR}/platform/np-identity/NativeNpIdentity.cmake")
        anyps5_add_native_np_identity(anyps5_cpu anyps5_platform_components)
        include("${CMAKE_CURRENT_SOURCE_DIR}/platform/http-uri/NativeUriTests.cmake")
        anyps5_add_native_uri_tests(anyps5_cpu)
        add_library(anyps5_native_module_runner STATIC src/NativeModuleRunner.mm)
        target_include_directories(anyps5_native_module_runner PUBLIC include)
        target_compile_features(anyps5_native_module_runner PUBLIC cxx_std_20)
        target_compile_options(anyps5_native_module_runner PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
        target_link_libraries(anyps5_native_module_runner PUBLIC
            anyps5_cpu anyps5_cpu_agc anyps5_platform_components anyps5_metal_native_execution
            anyps5_native_np_identity anyps5_native_uri_components)
        target_link_libraries(anyps5_cpu_run PRIVATE anyps5_native_module_runner)
        if(BUILD_TESTING)
            add_dependencies(anyps5_platform_kernel_events_owner_test anyps5_guest_thread_fixture)
            add_test(NAME cpu09_owner_graph COMMAND anyps5_platform_kernel_events_owner_test "${threadMain}" "${threadGuest}")
            set_tests_properties(cpu09_owner_graph PROPERTIES TIMEOUT 30)
            include("${CMAKE_CURRENT_SOURCE_DIR}/NativeModuleRunnerTests.cmake")
        endif()
        add_custom_target(anyps5_native_runner_resources
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:anyps5_cpu_run>/../fixtures"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${cpuMetalLibrary}"
                "$<TARGET_FILE_DIR:anyps5_cpu_run>/../fixtures/AnyPS5Utilities.metallib"
            DEPENDS anyps5_metal_shaders VERBATIM)
        add_dependencies(anyps5_cpu_run anyps5_native_runner_resources)
    endif()
endif()
