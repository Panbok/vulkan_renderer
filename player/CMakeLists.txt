cmake_minimum_required(VERSION 3.27)

project(vkr_player)

include("${CMAKE_SOURCE_DIR}/cmake/vkr_application_target.cmake")

# The player template a project package copies
# (docs/proposals/project-packaging.md): a development and a shipping player
# beside each other, the engine resources every package carries, and
# template.json naming them for `vkr_bakery bundle <project>`. Neither player
# is compiled per project.
function(vkr_add_player target shipping)
    set(sources src/main.c src/player_ui.c)
    if(NOT shipping)
        # The development player keeps the sample's debug overlay.
        list(APPEND sources "${CMAKE_SOURCE_DIR}/app/src/debug_overlay.c")
    endif()
    add_executable(${target} ${sources})
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/app/src")
    vkr_configure_application_target(${target})
    # Release trees already compile assertions' logging out of both.
    target_compile_definitions(${target} PRIVATE
        VKR_PLAYER_SHIPPING=$<BOOL:${shipping}>)
endfunction()

vkr_add_player(vkr_player FALSE)
vkr_add_player(vkr_player_shipping TRUE)
add_dependencies(vkr_player vkr_player_shipping)

include("${CMAKE_SOURCE_DIR}/cmake/vkr_engine_content.cmake")
add_dependencies(vkr_player vkr_engine_textures)
set(VKR_PLAYER_TEMPLATE "$<TARGET_FILE_DIR:vkr_player>")
# Text fonts sit beside engine content; a package carries one only when a
# scene's text names it, because packing follows references.
foreach(resource IN LISTS VKR_ENGINE_FILES VKR_TEXT_FONT_FILES)
    get_filename_component(directory "${resource}" DIRECTORY)
    add_custom_command(TARGET vkr_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${VKR_PLAYER_TEMPLATE}/engine/${directory}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/${resource}"
            "${VKR_PLAYER_TEMPLATE}/engine/${resource}")
endforeach()
# `libraries` holds what a package ships beside the player: on macOS the
# Vulkan loader, which a game's Contents/Frameworks carries.
set(VKR_PLAYER_LIBRARIES_JSON "")
if(APPLE)
    add_custom_command(TARGET vkr_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VKR_PLAYER_TEMPLATE}/lib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VKR_VULKAN_LOADER}"
            "${VKR_PLAYER_TEMPLATE}/lib/libvulkan.1.dylib")
    set(VKR_PLAYER_LIBRARIES_JSON "\n  \"libraries\": \"lib\",")
endif()

set(VKR_PLAYER_ENGINE_JSON_ITEMS ${VKR_ENGINE_INCLUDE})
list(TRANSFORM VKR_PLAYER_ENGINE_JSON_ITEMS PREPEND "\"")
list(TRANSFORM VKR_PLAYER_ENGINE_JSON_ITEMS APPEND "\"")
list(JOIN VKR_PLAYER_ENGINE_JSON_ITEMS ", " VKR_PLAYER_ENGINE_JSON)
# The build tree's template names this tree's shader catalog; an editor
# distribution's (editor/CMakeLists.txt) names the installed one and the
# installed engine content, relative to itself.
function(vkr_player_template_json output engine shaders)
    file(GENERATE OUTPUT "${output}" CONTENT
"{
  \"version\": 1,
  \"players\": {
    \"development\": \"$<TARGET_FILE_NAME:vkr_player>\",
    \"shipping\": \"$<TARGET_FILE_NAME:vkr_player_shipping>\"
  },
  \"engine\": \"${engine}\",
  \"engine_include\": [${VKR_PLAYER_ENGINE_JSON}],${VKR_PLAYER_LIBRARIES_JSON}
  \"shaders\": \"${shaders}\"
}
")
endfunction()
vkr_player_template_json("${VKR_PLAYER_TEMPLATE}/template.json" "engine"
    "${VKR_SHADER_CATALOG_DIR}")
vkr_player_template_json("${CMAKE_CURRENT_BINARY_DIR}/installed/template.json"
    "../../content" "../../shaders")

# The bakery finds this build tree's template unless one sits beside it in
# templates/player, as in an editor distribution.
target_compile_definitions(vkr_bakery PRIVATE
    VKR_BAKERY_PLAYER_TEMPLATE_DEFAULT="${VKR_PLAYER_TEMPLATE}")

install(TARGETS vkr_player vkr_player_shipping
    RUNTIME DESTINATION templates/player COMPONENT editor)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/installed/template.json"
    DESTINATION templates/player COMPONENT editor)
if(APPLE)
    install(FILES "${VKR_VULKAN_LOADER}" DESTINATION templates/player/lib
        RENAME libvulkan.1.dylib COMPONENT editor)
endif()
