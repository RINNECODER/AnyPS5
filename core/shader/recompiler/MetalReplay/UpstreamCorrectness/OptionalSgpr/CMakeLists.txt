if(NOT TARGET anyps5_metal_native_execution)
    message(FATAL_ERROR "Optional SGPR replay requires the native Metal execution target")
endif()
if(BUILD_TESTING)
    add_test_executable(anyps5_metal_optional_sgpr_replay OptionalSgprReplay.mm)
    target_compile_options(anyps5_metal_optional_sgpr_replay PRIVATE -fobjc-arc)
    target_link_libraries(anyps5_metal_optional_sgpr_replay PRIVATE anyps5_metal_native_execution)
    get_target_property(optionalSgprMetalLibrary anyps5_metal_utilities ANYPS5_METALLIB)
    foreach(optionalSgprMode IN ITEMS compute draw-vertex draw-fragment required)
        add_test(NAME anyps5_metal_optional_sgpr_${optionalSgprMode}
            COMMAND anyps5_metal_optional_sgpr_replay "${optionalSgprMode}" "${optionalSgprMetalLibrary}")
        set_tests_properties(anyps5_metal_optional_sgpr_${optionalSgprMode} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
    endforeach()
endif()
