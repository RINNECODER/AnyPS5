# New component registration only. An external harness supplies a CPU target;
# production resolution and the retained component CMake files are untouched.
function(anyps5_add_native_socket_tests cpu_target)
    if(NOT TARGET "${cpu_target}")
        message(FATAL_ERROR "Native socket acceptance requires an existing CPU target")
    endif()
    set(socket_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    add_library(anyps5_native_socket_components STATIC "${socket_source}/NativeSocketServices.cpp")
    target_include_directories(anyps5_native_socket_components PUBLIC "${socket_source}" "${socket_source}/../include")
    target_compile_features(anyps5_native_socket_components PUBLIC cxx_std_20)
    target_compile_options(anyps5_native_socket_components PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(anyps5_native_socket_components PUBLIC "${cpu_target}")
    if(NOT BUILD_TESTING)
        return()
    endif()
    find_program(socket_clang NAMES clang HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(socket_linker NAMES ld.lld HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin REQUIRED)
    find_program(socket_objcopy NAMES llvm-objcopy HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(socket_guest "${CMAKE_CURRENT_BINARY_DIR}/native-socket-guest.bin")
    set(socket_elf "${CMAKE_CURRENT_BINARY_DIR}/NativeSocketGuest.elf")
    set(socket_packager "${CMAKE_CURRENT_BINARY_DIR}/BuildNativeSocketFixture.py")
    # The retained packager validates real linked STT_FUNC, three PLT and
    # RELATIVE relocations. Only its public import family declarations change.
    file(WRITE "${socket_packager}" "import importlib.util\nfrom pathlib import Path\nimport sys\nsys.dont_write_bytecode = True\nhelper = Path(r'${socket_source}/tests/BuildScePlatformFixture.py')\nspec = importlib.util.spec_from_file_location('socket_fixture_packager', helper)\nmodule = importlib.util.module_from_spec(spec)\nsource = helper.read_text()\nold = 'for name, identity in ((\"libSceHttp\",1),(\"libSceAjm\",2)):'\nassert source.count(old) == 1, 'Retained packager metadata loop changed'\nsource = source.replace(old, 'for name, identity in sorted({(row[1], row[2]) for row in IMPORTS.values()}):')\nadmission = 'PublicAbiFixtures Network/Audio only; no production ABI inference'\nassert source.count(admission) == 1, 'Retained packager admission receipt changed'\nsource = source.replace(admission, 'NativeSocket public fixture candidate only; retail admission denied; no production registration')\nexec(compile(source, str(helper), 'exec'), module.__dict__)\nmodule.IMPORTS = {'sceNetSocket': ('Q4qBuN-c0ZM', 'libSceNet', 1, 'B'), 'sceNetSocketClose': ('45ggEzakPJQ', 'libSceNet', 1, 'B'), 'sceNetErrnoLoc': ('HQOwnfMGipQ', 'libSceNet', 1, 'B')}\nmodule.package(*sys.argv[1:])\n")
    add_custom_command(OUTPUT "${socket_guest}" "${socket_elf}"
        COMMAND "${socket_clang}" --target=x86_64-unknown-linux-gnu -O1 -fPIC
            -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
            -fno-asynchronous-unwind-tables -mno-red-zone
            -c "${socket_source}/fixtures/NativeSocketGuest.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/native-socket-guest.o"
        COMMAND "${socket_linker}" -shared -e _start --hash-style=sysv
            -z max-page-size=4096 -z separate-code -z norelro
            -o "${CMAKE_CURRENT_BINARY_DIR}/native-socket-linked.elf" "${CMAKE_CURRENT_BINARY_DIR}/native-socket-guest.o"
        COMMAND "${socket_objcopy}" -O binary --only-section=.text
            "${CMAKE_CURRENT_BINARY_DIR}/native-socket-linked.elf" "${socket_guest}"
        COMMAND "${Python3_EXECUTABLE}" "${socket_packager}"
            "${CMAKE_CURRENT_BINARY_DIR}/native-socket-linked.elf" "${socket_elf}"
        DEPENDS "${socket_source}/fixtures/NativeSocketGuest.c" "${socket_packager}"
            "${socket_source}/tests/BuildScePlatformFixture.py" "${socket_source}/../tests/BuildSceCrtFixture.py" VERBATIM)
    add_custom_target(anyps5_native_socket_guest DEPENDS "${socket_guest}" "${socket_elf}")
    add_executable(anyps5_native_socket_test "${socket_source}/tests/NativeSocketServicesTest.cpp")
    target_link_libraries(anyps5_native_socket_test PRIVATE anyps5_native_socket_components)
    target_compile_options(anyps5_native_socket_test PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(anyps5_native_socket_test anyps5_native_socket_guest)
    foreach(socket_case IN ITEMS scope resources errno names)
        add_test(NAME anyps5_native_socket_${socket_case}
            COMMAND anyps5_native_socket_test "${socket_guest}" "${socket_elf}" "${socket_case}")
        set_tests_properties(anyps5_native_socket_${socket_case} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    endforeach()
endfunction()
