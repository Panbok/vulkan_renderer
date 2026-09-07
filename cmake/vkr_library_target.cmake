# Shared VKR compilation policy; dependencies and source ownership stay local.
function(vkr_configure_library target)
    vkr_require_declared_c_functions(${target})
    target_compile_definitions(${target} PRIVATE
        $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:LOG_LEVEL=1>
        $<$<BOOL:${VKR_EDITOR_LOGGING}>:LOG_LEVEL=5>
        $<$<BOOL:${VKR_EDITOR_LOGGING}>:VKR_EDITOR_LOGGING=1>
        $<$<CONFIG:Release>:ASSERT_LOG=0>
        $<$<AND:$<CONFIG:RelWithDebInfo>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:LOG_LEVEL=3>
        $<$<CONFIG:RelWithDebInfo>:ASSERT_LOG=0>
        $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:LOG_LEVEL=4>
        $<$<CONFIG:Debug>:ASSERT_LOG=1>
        $<$<CONFIG:Release>:VKR_ALLOCATOR_DISABLE_STATS=1>
        $<$<CONFIG:RelWithDebInfo>:VKR_ALLOCATOR_DISABLE_STATS=1>
        $<$<CONFIG:Debug>:VKR_ALLOCATOR_DISABLE_STATS=0>
        $<$<CONFIG:Release>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
        $<$<CONFIG:RelWithDebInfo>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
        $<$<CONFIG:Debug>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
    )
endfunction()
