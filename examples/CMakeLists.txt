# Executable clients exercise the public library seams without sample policy.
add_executable(vkr_renderer_example renderer/main.c)
vkr_configure_library(vkr_renderer_example)
target_link_libraries(vkr_renderer_example PRIVATE renderer_lib)
if(VKR_BUILD_RUNTIME)
    add_executable(vkr_host_example host/main.c)
    vkr_configure_library(vkr_host_example)
    target_link_libraries(vkr_host_example PRIVATE vkr_runtime)
endif()
