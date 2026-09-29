cmake_minimum_required(VERSION 3.27)

project(vkr_editor)

include("${CMAKE_SOURCE_DIR}/cmake/vkr_application_target.cmake")

add_executable(vkr_editor
    src/main.c
    src/editor_application.c
    src/editor_ui.c
    src/editor_dock.c
    src/editor_windows.c
    src/editor_cmd.c
    src/editor_cmd_eval.c
    src/editor_viewport.c
    src/editor_animation.c
    src/editor_console.c
    src/editor_bakery.c
    src/editor_bakery_service.c
    src/editor_build.c
    src/editor_scene_panels.c
    src/editor_details.c
    src/editor_labels.c
    src/editor_physics.c
    src/editor_physics_settings.c
    src/editor_graphics.c
    src/editor_content.c
    src/editor_projects.c
    src/editor_project_store.c
)
target_include_directories(vkr_editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
vkr_configure_application_target(vkr_editor)

# Every cooker, table, shader compile, project job, scene bake and material
# preview runs through vkr_bakery; the harness and preview tool remain
# separate renderer programs.
add_dependencies(vkr_editor vkr_bakery vkr_harness vkr_asset_preview)
# Build packages the open project with the player template beside
# vkr_player (docs/proposals/project-packaging.md).
if(TARGET vkr_player)
    add_dependencies(vkr_editor vkr_player)
endif()
target_compile_definitions(vkr_editor PRIVATE
    VKR_EDITOR_ASSET_PREVIEW_PATH="$<TARGET_FILE:vkr_asset_preview>"
    VKR_EDITOR_BAKERY_PATH="$<TARGET_FILE:vkr_bakery>"
    VKR_EDITOR_HARNESS_PATH="$<TARGET_FILE:vkr_harness>"
)

# Installed bootstrap UI resources allow the chooser to run before a workspace
# exists. Managed workspace creation copies this bounded bundle.
set(VKR_EDITOR_BOOTSTRAP_FONTS
    NotoSansCJK.fontcfg NotoSansCJK-Windows.fontcfg NotoSansCJK-Regular.ttc
    UbuntuMono.fontcfg UbuntuMono-bitmap.fontcfg UbuntuMono21px.fnt
    UbuntuMono-cooked.fontcfg UbuntuMono-cooked.vkfa UbuntuMono-R.ttf
    UbuntuMono-Bold-cooked.fontcfg UbuntuMono-Bold-cooked.vkfa UbuntuMono-Bold.ttf
    ubuntu-font-licence-1.0.txt UbuntuMono-R.ttf.license.md UbuntuMono-Bold.ttf.license.md
    Inter-Regular-cooked.fontcfg Inter-Regular-cooked.vkfa Inter-Regular.ttf
    Inter-Regular-editor.charset
    Inter-SemiBold-cooked.fontcfg Inter-SemiBold-cooked.vkfa Inter-SemiBold.ttf
    Inter-OFL.txt
    Phosphor-cooked.fontcfg Phosphor-cooked.vkfa Phosphor.ttf
    Phosphor-editor.charset
    Phosphor-Fill-cooked.fontcfg Phosphor-Fill-cooked.vkfa Phosphor-Fill.ttf
    Phosphor-Fill-editor.charset Phosphor-LICENSE.txt)
add_custom_command(TARGET vkr_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vkr_editor>/resources/editor/fonts")
foreach(resource IN LISTS VKR_EDITOR_BOOTSTRAP_FONTS)
    add_custom_command(TARGET vkr_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/fonts/${resource}"
        "$<TARGET_FILE_DIR:vkr_editor>/resources/editor/fonts/${resource}")
endforeach()
foreach(resource UbuntuMono21px_0.png)
    add_custom_command(TARGET vkr_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/textures/${resource}"
        "$<TARGET_FILE_DIR:vkr_editor>/resources/editor/fonts/${resource}")
endforeach()
