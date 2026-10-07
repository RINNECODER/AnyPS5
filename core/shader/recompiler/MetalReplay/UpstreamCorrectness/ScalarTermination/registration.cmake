if(NOT TARGET anyps5_metal_guest_recompiler OR NOT TARGET anyps5_metal_utilities)
    message(FATAL_ERROR "Register scalar termination after the native Metal recompiler targets")
endif()
add_executable(anyps5_metal_scalar_termination_replay "${CMAKE_CURRENT_LIST_DIR}/ScalarTerminationReplay.mm")
target_compile_options(anyps5_metal_scalar_termination_replay PRIVATE -fobjc-arc)
target_link_libraries(anyps5_metal_scalar_termination_replay PRIVATE anyps5_metal_guest_recompiler anyps5_metal_utilities)
if(BUILD_TESTING)
    foreach(scalarTerminationMode IN ITEMS native decode)
        add_test(NAME anyps5_metal_scalar_termination_${scalarTerminationMode}
            COMMAND anyps5_metal_scalar_termination_replay "${scalarTerminationMode}")
        set_tests_properties(anyps5_metal_scalar_termination_${scalarTerminationMode} PROPERTIES
            TIMEOUT 180 RUN_SERIAL TRUE
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
    endforeach()
endif()
