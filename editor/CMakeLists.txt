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
    src/editor_install.c
    src/editor_scene_panels.c
    src/editor_details.c
    src/editor_labels.c
    src/editor_physics.c
    src/editor_physics_settings.c
    src/editor_graphics.c
    src/editor_content.c
    src/editor_projects.c
    src/editor_project_store.c
    src/editor_scripts.c
    src/editor_code.c
)
target_include_directories(vkr_editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
vkr_configure_application_target(vkr_editor)

# Every cooker, table, shader compile, project job, scene bake and material
# preview runs through vkr_bakery; the harness and preview tool remain
# separate renderer programs.
add_dependencies(vkr_editor vkr_bakery vkr_harness vkr_asset_preview
    vkr_script_sdk)
# Build packages the open project with the player template beside
# vkr_player (docs/proposals/project-packaging.md).
if(TARGET vkr_player)
    add_dependencies(vkr_editor vkr_player)
endif()
target_compile_definitions(vkr_editor PRIVATE
    VKR_EDITOR_ASSET_PREVIEW_PATH="$<TARGET_FILE:vkr_asset_preview>"
    VKR_EDITOR_BAKERY_PATH="$<TARGET_FILE:vkr_bakery>"
    VKR_EDITOR_HARNESS_PATH="$<TARGET_FILE:vkr_harness>"
    # Linked script modules, such as the FPS sample, keep their sources in
    # this tree (ADR-079).
    VKR_EDITOR_SCRIPT_SDK_ROOT="${CMAKE_SOURCE_DIR}"
    # Script completion reads the staged script SDK unless `sdk` lies beside
    # the editor.
    VKR_EDITOR_SCRIPT_SDK_DIR="${VKR_SCRIPT_SDK_DIR}"
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

# Editor distribution (docs/adr/078-project-build-and-packaging.md):
# `cmake --install <build> --prefix <dir>` lays out one relocatable folder.
#   vkr_editor, vkr_bakery, vkr_harness, vkr_asset_preview
#   libvulkan.1.dylib          macOS: the Vulkan loader the programs link
#   resources/editor/          the bootstrap UI resources above
#   shaders/<backend>/         the shader catalog
#   content/                   engine content, the installed content root
#   templates/player/          the player template (player/CMakeLists.txt)
#   sdk/                       the script SDK headers (cmake/vkr_script_sdk.cmake)
# The programs find each other, the content and the catalog beside
# themselves, and keep writable state in per-user directories.
include("${CMAKE_SOURCE_DIR}/cmake/vkr_engine_content.cmake")
add_dependencies(vkr_editor vkr_engine_textures)
install(TARGETS vkr_editor vkr_bakery vkr_harness vkr_asset_preview
    RUNTIME DESTINATION . COMPONENT editor)
if(APPLE)
    install(FILES "${VKR_VULKAN_LOADER}" DESTINATION .
        RENAME libvulkan.1.dylib COMPONENT editor)
endif()
install(DIRECTORY "$<TARGET_FILE_DIR:vkr_editor>/resources/"
    DESTINATION resources COMPONENT editor)
install(DIRECTORY "${VKR_SHADER_CATALOG_DIR}/"
    DESTINATION shaders COMPONENT editor)
install(DIRECTORY "${VKR_SCRIPT_SDK_DIR}/"
    DESTINATION sdk COMPONENT editor)
# Probe bakes copy the offscreen profile into each job. Text fonts install
# beside engine content so packages can carry the ones their text uses.
foreach(resource IN LISTS VKR_ENGINE_FILES VKR_TEXT_FONT_FILES ITEMS
        tools/profiles/local-offscreen.json)
    get_filename_component(directory "${resource}" DIRECTORY)
    install(FILES "${CMAKE_SOURCE_DIR}/${resource}"
        DESTINATION "content/${directory}" COMPONENT editor)
endforeach()

# Starter scenes are editor content. A created project's managed import owns
# its copied scene geometry and collision closure; games carry only assets
# referenced by that project.
foreach(template blank fps_arena rpg_grounds)
    install(FILES
        "${CMAKE_SOURCE_DIR}/assets/templates/${template}.scene.json"
        "${CMAKE_SOURCE_DIR}/assets/templates/${template}.scene.json.editor.json"
        "${CMAKE_SOURCE_DIR}/assets/templates/${template}.glb"
        "${CMAKE_SOURCE_DIR}/assets/templates/${template}_collision.glb"
        "${CMAKE_SOURCE_DIR}/assets/templates/${template}_collision.vkc"
        DESTINATION content/assets/templates COMPONENT editor)
    install(FILES "${CMAKE_SOURCE_DIR}/assets/templates/previews/${template}.png"
        DESTINATION content/assets/templates/previews COMPONENT editor)
endforeach()
install(FILES "${CMAKE_SOURCE_DIR}/assets/templates/measurements.json"
    DESTINATION content/assets/templates COMPONENT editor)
