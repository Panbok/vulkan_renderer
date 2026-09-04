cmake_minimum_required(VERSION 3.27)

project(vkr_editor)

include("${CMAKE_SOURCE_DIR}/cmake/vkr_application_target.cmake")

add_executable(vkr_editor
    src/main.c
    src/editor_application.c
    src/editor_ui.c
    src/editor_dock.c
    src/editor_windows.c
)
target_include_directories(vkr_editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
vkr_configure_application_target(vkr_editor)
