cmake_minimum_required(VERSION 3.27)

project(vkr_editor)

include("${CMAKE_SOURCE_DIR}/cmake/vkr_application_target.cmake")

add_executable(vkr_editor
    src/main.c
    src/editor_application.c
    src/editor_ui.c
    src/editor_dock.c
    src/editor_windows.c
    src/editor_console.c
    src/editor_bakery.c
    src/editor_scene_panels.c
)
target_include_directories(vkr_editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
vkr_configure_application_target(vkr_editor)

add_dependencies(vkr_editor vkr_mesh_cooker vkr_font_cooker vkr_vkt_packer)
target_compile_definitions(vkr_editor PRIVATE
    VKR_EDITOR_MESH_COOKER_PATH="$<TARGET_FILE:vkr_mesh_cooker>"
    VKR_EDITOR_FONT_COOKER_PATH="$<TARGET_FILE:vkr_font_cooker>"
    VKR_EDITOR_TEXTURE_COOKER_PATH="$<TARGET_FILE:vkr_vkt_packer>"
)
