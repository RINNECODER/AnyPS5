find_package(Threads REQUIRED)

add_library(anyps5_metal_native_runtime INTERFACE)
target_include_directories(anyps5_metal_native_runtime INTERFACE "${PROJECT_SOURCE_DIR}/core/libs")
target_link_libraries(anyps5_metal_native_runtime INTERFACE Threads::Threads)
include("${CMAKE_CURRENT_SOURCE_DIR}/../CMakeLists.txt")
add_shader_recompiler(anyps5_metal_guest_recompiler anyps5_metal_native_runtime)

add_executable(anyps5_metal_guest_replay GuestShaderReplay.mm)
target_compile_options(anyps5_metal_guest_replay PRIVATE -fobjc-arc)
target_link_libraries(anyps5_metal_guest_replay PRIVATE
    anyps5_metal_guest_recompiler anyps5_metal_utilities)
if(BUILD_TESTING)
    add_test(NAME anyps5_metal_guest_replay COMMAND anyps5_metal_guest_replay)
    set_tests_properties(anyps5_metal_guest_replay PROPERTIES TIMEOUT 120)
endif()
