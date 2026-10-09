if(NOT TARGET anyps5_metal_guest_recompiler OR NOT TARGET anyps5_metal_utilities)
    message(FATAL_ERROR "Register static scalar calls after the native Metal recompiler targets")
endif()
add_executable(anyps5_metal_scalar_static_call_replay "${CMAKE_CURRENT_LIST_DIR}/../../ScalarStaticCallReplay.mm")
target_compile_options(anyps5_metal_scalar_static_call_replay PRIVATE -fobjc-arc)
target_link_libraries(anyps5_metal_scalar_static_call_replay PRIVATE anyps5_metal_guest_recompiler anyps5_metal_utilities)
if(BUILD_TESTING)
    add_executable(anyps5_metal_scalar_static_call_guards
        "${CMAKE_CURRENT_LIST_DIR}/../../../../../libs/prx/libSceAgcDriver/tests/ControlFlow.cpp")
    target_link_libraries(anyps5_metal_scalar_static_call_guards PRIVATE anyps5_metal_guest_recompiler)
    add_test(NAME anyps5_metal_scalar_static_call_guards COMMAND anyps5_metal_scalar_static_call_guards)
    set_tests_properties(anyps5_metal_scalar_static_call_guards PROPERTIES TIMEOUT 120)
    add_test(NAME anyps5_metal_scalar_static_calls COMMAND anyps5_metal_scalar_static_call_replay)
    set_tests_properties(anyps5_metal_scalar_static_calls PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE
        ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
endif()
