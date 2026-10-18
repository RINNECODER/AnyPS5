cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(anyps5_aot_cpp CXX)
endif()

add_library(anyps5_aot_cpp STATIC
    src/json.cpp
    src/json_parse.cpp
    src/targets.cpp
    src/abi.cpp
    src/abi_contract.cpp
    src/atomics.cpp
    src/calls.cpp
    src/fma.cpp
    src/guest_regions.cpp
    src/guest_provider_calls.cpp
    src/integer.cpp
    src/optimize.cpp
    src/optimize_parse.cpp
    src/register_aliases.cpp
    src/register_values.cpp)
target_include_directories(anyps5_aot_cpp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(anyps5_aot_cpp PROPERTIES CXX_EXTENSIONS OFF CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

add_executable(aot_cpp_difftest difftest_main.cpp)
target_link_libraries(aot_cpp_difftest PRIVATE anyps5_aot_cpp)
set_target_properties(aot_cpp_difftest PROPERTIES CXX_EXTENSIONS OFF CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED BUILD_TESTING)
    option(BUILD_TESTING "Build and register the AOT C++ differential tests" ON)
endif()

if(BUILD_TESTING)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME aot_cpp_difftest
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONDONTWRITEBYTECODE=1"
            ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/difftest.py"
            --binary $<TARGET_FILE:aot_cpp_difftest>)
    set_tests_properties(aot_cpp_difftest PROPERTIES TIMEOUT 30)
endif()
