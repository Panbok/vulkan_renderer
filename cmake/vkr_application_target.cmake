function(vkr_configure_application_target target)
    vkr_require_declared_c_functions(${target})

    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        set_property(TARGET ${target} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY
                     "${CMAKE_SOURCE_DIR}/")
    endif()
    if(CMAKE_GENERATOR MATCHES "Xcode")
        set_property(TARGET ${target} PROPERTY XCODE_GENERATE_SCHEME TRUE)
        set_property(TARGET ${target} PROPERTY XCODE_SCHEME_WORKING_DIRECTORY
                     "${CMAKE_SOURCE_DIR}")
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /arch:AVX2)
    else()
        target_compile_options(${target} PRIVATE -march=native)
    endif()

    target_link_libraries(${target} PRIVATE vkr_sample_runtime)
    target_compile_definitions(${target} PRIVATE
        $<$<CONFIG:Release>:LOG_LEVEL=1>
        $<$<CONFIG:Release>:ASSERT_LOG=0>
        $<$<CONFIG:RelWithDebInfo>:LOG_LEVEL=3>
        $<$<CONFIG:RelWithDebInfo>:ASSERT_LOG=0>
        $<$<CONFIG:Debug>:LOG_LEVEL=4>
        $<$<CONFIG:Debug>:ASSERT_LOG=1>
    )
endfunction()
