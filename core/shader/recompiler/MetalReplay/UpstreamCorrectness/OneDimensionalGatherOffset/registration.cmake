if(NOT TARGET anyps5_metal_native_execution OR NOT TARGET anyps5_metal_utilities)
    message(FATAL_ERROR "Register 1D gather offsets after the native Metal execution targets")
endif()
add_executable(anyps5_metal_1d_gather_offset_replay "${CMAKE_CURRENT_LIST_DIR}/GatherOffsetReplay.mm")
target_compile_options(anyps5_metal_1d_gather_offset_replay PRIVATE -fobjc-arc)
target_link_libraries(anyps5_metal_1d_gather_offset_replay PRIVATE anyps5_metal_native_execution anyps5_metal_utilities)
if(BUILD_TESTING)
    get_target_property(gatherOffsetUtilityLibrary anyps5_metal_utilities ANYPS5_METALLIB)
    add_test(NAME anyps5_metal_1d_gather_offset_native
        COMMAND anyps5_metal_1d_gather_offset_replay "${gatherOffsetUtilityLibrary}" native)
    add_test(NAME anyps5_metal_1d_gather_offset_rejections
        COMMAND anyps5_metal_1d_gather_offset_replay "${gatherOffsetUtilityLibrary}" reject)
    set_tests_properties(anyps5_metal_1d_gather_offset_native anyps5_metal_1d_gather_offset_rejections PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE
        ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;ANYPS5_NO_SHADER_CACHE=1")
endif()
