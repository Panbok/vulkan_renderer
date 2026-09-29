# Installed macOS programs find the libraries they ship beside themselves (an
# editor distribution) or in a game's Contents/Frameworks; the build tree
# keeps the linker's paths as well.
function(vkr_configure_installed_rpath target)
    if(APPLE)
        set(paths "@executable_path" "@executable_path/../Frameworks")
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH ${paths})
        set_property(TARGET ${target} PROPERTY INSTALL_RPATH ${paths})
    endif()
endfunction()

function(vkr_configure_application_target target)
    vkr_require_declared_c_functions(${target})
    vkr_set_main_stack_reserve(${target})
    vkr_configure_installed_rpath(${target})

    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        set_property(TARGET ${target} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY
                     "${CMAKE_SOURCE_DIR}/")
    endif()
    if(CMAKE_GENERATOR MATCHES "Xcode")
        set_property(TARGET ${target} PROPERTY XCODE_GENERATE_SCHEME TRUE)
        set_property(TARGET ${target} PROPERTY XCODE_SCHEME_WORKING_DIRECTORY
                     "${CMAKE_SOURCE_DIR}")
    endif()

    # Repository applications register the FPS sample script module.
    target_link_libraries(${target} PRIVATE vkr_sample_runtime vkr_script_fps)
    target_compile_definitions(${target} PRIVATE
        $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:LOG_LEVEL=3>
        $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:VKR_LOG_DEFAULT_LEVEL=1>
        $<$<BOOL:${VKR_EDITOR_LOGGING}>:LOG_LEVEL=5>
        $<$<BOOL:${VKR_EDITOR_LOGGING}>:VKR_EDITOR_LOGGING=1>
        $<$<CONFIG:Release>:ASSERT_LOG=0>
        $<$<AND:$<CONFIG:RelWithDebInfo>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:LOG_LEVEL=3>
        $<$<CONFIG:RelWithDebInfo>:ASSERT_LOG=0>
        $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:LOG_LEVEL=4>
        $<$<CONFIG:Debug>:ASSERT_LOG=1>
    )
endfunction()
