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
    src/editor_labels.c
)
target_include_directories(vkr_editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
vkr_configure_application_target(vkr_editor)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_dependencies(vkr_editor vkr_mesh_cooker vkr_font_cooker vkr_vkt_packer
    vkr_dfg_cooker vkr_sheen_cooker vkr_anisotropy_cooker
    vkr_diffuse_baker vkr_hdr_cube_packer vkr_harness)
target_compile_definitions(vkr_editor PRIVATE
    VKR_EDITOR_DFG_COOKER_PATH="$<TARGET_FILE:vkr_dfg_cooker>"
    VKR_EDITOR_SHEEN_COOKER_PATH="$<TARGET_FILE:vkr_sheen_cooker>"
    VKR_EDITOR_ANISOTROPY_COOKER_PATH="$<TARGET_FILE:vkr_anisotropy_cooker>"
    VKR_EDITOR_DIFFUSE_BAKER_PATH="$<TARGET_FILE:vkr_diffuse_baker>"
    VKR_EDITOR_HDR_CUBE_PACKER_PATH="$<TARGET_FILE:vkr_hdr_cube_packer>"
    VKR_EDITOR_HARNESS_PATH="$<TARGET_FILE:vkr_harness>"
    VKR_EDITOR_PYTHON_PATH="${Python3_EXECUTABLE}"
    VKR_EDITOR_MESH_COOKER_PATH="$<TARGET_FILE:vkr_mesh_cooker>"
    VKR_EDITOR_FONT_COOKER_PATH="$<TARGET_FILE:vkr_font_cooker>"
    VKR_EDITOR_TEXTURE_COOKER_PATH="$<TARGET_FILE:vkr_vkt_packer>"
)
