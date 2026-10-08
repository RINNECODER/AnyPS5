# Include after the existing CPU and platform-component targets. This leaf
# does not change the runner's registration policy or rebuild its TCG runtime.
function(anyps5_add_native_np_identity cpu_target np_target)
    if(NOT TARGET "${cpu_target}" OR NOT TARGET "${np_target}")
        message(FATAL_ERROR "Native NP identity needs existing CPU and NP component targets")
    endif()
    set(np_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    add_library(anyps5_native_np_identity STATIC "${np_source}/NativeNpIdentity.cpp")
    target_compile_features(anyps5_native_np_identity PUBLIC cxx_std_20)
    target_include_directories(anyps5_native_np_identity PUBLIC "${np_source}" "${np_source}/../../include")
    target_compile_options(anyps5_native_np_identity PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(anyps5_native_np_identity PUBLIC "${cpu_target}" "${np_target}")
    if(NOT BUILD_TESTING)
        return()
    endif()
    find_program(np_clang NAMES clang HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(np_linker NAMES ld.lld HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(np_fixture "${CMAKE_CURRENT_BINARY_DIR}/NativeNpIdentityGuest.elf")
    add_custom_command(OUTPUT "${np_fixture}"
        COMMAND "${np_clang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC -ffreestanding
            -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
            -mno-red-zone -c "${np_source}/tests/NativeNpIdentityGuest.c" -o "${CMAKE_CURRENT_BINARY_DIR}/np-identity-guest.o"
        COMMAND "${np_linker}" -shared -e _start --hash-style=sysv -z max-page-size=4096
            -z separate-code -z norelro -o "${CMAKE_CURRENT_BINARY_DIR}/np-identity-linked.elf"
            "${CMAKE_CURRENT_BINARY_DIR}/np-identity-guest.o"
        COMMAND "${Python3_EXECUTABLE}" "${np_source}/tests/BuildNativeNpIdentityFixture.py"
            "${CMAKE_CURRENT_BINARY_DIR}/np-identity-linked.elf" "${np_fixture}"
        DEPENDS "${np_source}/tests/NativeNpIdentityGuest.c" "${np_source}/tests/BuildNativeNpIdentityFixture.py"
            "${np_source}/../tests/BuildScePlatformFixture.py" "${np_source}/../../tests/BuildSceCrtFixture.py" VERBATIM)
    add_custom_target(anyps5_native_np_identity_guest DEPENDS "${np_fixture}")
    add_executable(anyps5_native_np_identity_test "${np_source}/tests/NativeNpIdentityTest.cpp")
    target_compile_options(anyps5_native_np_identity_test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(anyps5_native_np_identity_test PRIVATE anyps5_native_np_identity)
    add_dependencies(anyps5_native_np_identity_test anyps5_native_np_identity_guest)
    foreach(np_case IN ITEMS scope compiled)
        add_test(NAME anyps5_native_np_identity_${np_case}
            COMMAND anyps5_native_np_identity_test "${np_fixture}" "${np_case}")
        set_tests_properties(anyps5_native_np_identity_${np_case} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    endforeach()
endfunction()
