cmake_minimum_required(VERSION 3.27)

project(vulkan_renderer_app)

include("${CMAKE_SOURCE_DIR}/cmake/vkr_application_target.cmake")

add_executable(vulkan_renderer
    src/main.c
    src/app_ui.c
    src/debug_overlay.c
)
target_include_directories(vulkan_renderer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
vkr_configure_application_target(vulkan_renderer)
