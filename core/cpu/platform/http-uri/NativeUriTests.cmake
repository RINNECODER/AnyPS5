function(anyps5_add_native_uri_tests cpu_target)
    if(NOT TARGET "${cpu_target}")
        message(FATAL_ERROR "Native URI acceptance requires an existing CPU target")
    endif()
    set(uri_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    add_library(anyps5_native_uri_components STATIC "${uri_source}/NativeUriEscape.cpp" "${uri_source}/../NetworkServices.cpp")
    target_include_directories(anyps5_native_uri_components PUBLIC "${uri_source}" "${uri_source}/../../include")
    target_compile_features(anyps5_native_uri_components PUBLIC cxx_std_20)
    target_compile_options(anyps5_native_uri_components PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(anyps5_native_uri_components PUBLIC "${cpu_target}")
    if(NOT BUILD_TESTING)
        return()
    endif()
    find_program(uri_clang NAMES clang HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(uri_linker NAMES ld.lld HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(uri_objcopy NAMES llvm-objcopy HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(uri_elf "${CMAKE_CURRENT_BINARY_DIR}/NativeUriGuest.elf")
    set(uri_nid_elf "${CMAKE_CURRENT_BINARY_DIR}/uri-wrong-nid/NativeUriGuest.elf")
    set(uri_abi_elf "${CMAKE_CURRENT_BINARY_DIR}/uri-wrong-abi/NativeUriGuest.elf")
    set(uri_library_elf "${CMAKE_CURRENT_BINARY_DIR}/uri-wrong-library/NativeUriGuest.elf")
    add_custom_command(OUTPUT "${uri_elf}" "${uri_nid_elf}" "${uri_abi_elf}" "${uri_library_elf}"
        COMMAND "${uri_clang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC
            -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
            -fno-asynchronous-unwind-tables -mno-red-zone
            -c "${uri_source}/fixtures/NativeUriGuest.c" -o "${CMAKE_CURRENT_BINARY_DIR}/native-uri-guest.o"
        COMMAND "${uri_linker}" -shared -e _start --hash-style=sysv
            -z max-page-size=4096 -z separate-code -z norelro
            -o "${CMAKE_CURRENT_BINARY_DIR}/native-uri-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/native-uri-guest.o"
        COMMAND "${Python3_EXECUTABLE}" "${uri_source}/tests/BuildNativeUriFixture.py"
            "${CMAKE_CURRENT_BINARY_DIR}/native-uri-linked.elf" "${uri_elf}"
        DEPENDS "${uri_source}/fixtures/NativeUriGuest.c" "${uri_source}/tests/BuildNativeUriFixture.py"
            "${uri_source}/../tests/BuildScePlatformFixture.py" "${uri_source}/../../tests/BuildSceCrtFixture.py" VERBATIM)
    add_custom_target(anyps5_native_uri_guest DEPENDS "${uri_elf}" "${uri_nid_elf}" "${uri_abi_elf}" "${uri_library_elf}")
    add_executable(anyps5_native_uri_test "${uri_source}/tests/NativeUriEscapeTest.cpp")
    target_link_libraries(anyps5_native_uri_test PRIVATE anyps5_native_uri_components)
    target_compile_options(anyps5_native_uri_test PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(anyps5_native_uri_test anyps5_native_uri_guest)
    foreach(uri_case IN ITEMS scope compiled memory lifetime)
        add_test(NAME anyps5_native_uri_${uri_case} COMMAND anyps5_native_uri_test "${uri_elf}" "${uri_case}")
        set_tests_properties(anyps5_native_uri_${uri_case} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    endforeach()
    # Existing NetworkServicesTest remains the algorithm/error policy owner.
    # No copied algorithm table or modified retained test is introduced here.
    set(uri_retained_guest "${CMAKE_CURRENT_BINARY_DIR}/retained-network-guest.bin")
    add_custom_command(OUTPUT "${uri_retained_guest}"
        COMMAND "${uri_clang}" --target=x86_64-unknown-linux-gnu -O1 -ffreestanding -fno-pic
            -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
            -c "${uri_source}/../fixtures/NetworkServicesGuest.c" -o "${CMAKE_CURRENT_BINARY_DIR}/retained-network-guest.o"
        COMMAND "${uri_linker}" -static -e NetworkServicesGuest -T "${uri_source}/../tests/GuestText.ld"
            -o "${CMAKE_CURRENT_BINARY_DIR}/retained-network-guest.elf" "${CMAKE_CURRENT_BINARY_DIR}/retained-network-guest.o"
        COMMAND "${uri_objcopy}" -O binary --only-section=.text
            "${CMAKE_CURRENT_BINARY_DIR}/retained-network-guest.elf" "${uri_retained_guest}"
        DEPENDS "${uri_source}/../fixtures/NetworkServicesGuest.c" "${uri_source}/../tests/GuestText.ld" VERBATIM)
    add_custom_target(anyps5_native_uri_retained_guest DEPENDS "${uri_retained_guest}")
    add_executable(anyps5_native_uri_retained_network_test "${uri_source}/../tests/NetworkServicesTest.cpp")
    target_link_libraries(anyps5_native_uri_retained_network_test PRIVATE anyps5_native_uri_components)
    target_compile_options(anyps5_native_uri_retained_network_test PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(anyps5_native_uri_retained_network_test anyps5_native_uri_retained_guest)
    add_test(NAME anyps5_native_uri_retained_network
        COMMAND anyps5_native_uri_retained_network_test "${uri_retained_guest}")
    set_tests_properties(anyps5_native_uri_retained_network PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endfunction()
