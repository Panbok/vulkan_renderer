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

# Engine resources: the render graph and the runtime's fonts with every file
# their configurations name. Changing this set changes every package.
set(VKR_PLAYER_ENGINE_INCLUDE
    assets/render_graphs/main.rendergraph.json
    assets/fonts/UbuntuMono-cooked.fontcfg
    assets/fonts/UbuntuMono-bitmap.fontcfg
    assets/fonts/UbuntuMono21px.fnt.vkf
    assets/textures/UbuntuMono21px_0.png
    assets/fonts/NotoSansCJK.fontcfg)
set(VKR_PLAYER_ENGINE_FILES
    ${VKR_PLAYER_ENGINE_INCLUDE}
    assets/fonts/UbuntuMono-cooked.vkfa
    assets/fonts/UbuntuMono21px.fnt
    assets/textures/UbuntuMono21px_0.png.vkt
    assets/fonts/NotoSansCJK-Regular.ttc)
set(VKR_PLAYER_TEMPLATE "$<TARGET_FILE_DIR:vkr_player>")
foreach(resource IN LISTS VKR_PLAYER_ENGINE_FILES)
    get_filename_component(directory "${resource}" DIRECTORY)
    add_custom_command(TARGET vkr_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${VKR_PLAYER_TEMPLATE}/engine/${directory}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/${resource}"
            "${VKR_PLAYER_TEMPLATE}/engine/${resource}")
endforeach()

list(TRANSFORM VKR_PLAYER_ENGINE_INCLUDE PREPEND "\"")
list(TRANSFORM VKR_PLAYER_ENGINE_INCLUDE APPEND "\"")
list(JOIN VKR_PLAYER_ENGINE_INCLUDE ", " VKR_PLAYER_ENGINE_JSON)
file(GENERATE OUTPUT "${VKR_PLAYER_TEMPLATE}/template.json" CONTENT
"{
  \"version\": 1,
  \"players\": {
    \"development\": \"$<TARGET_FILE_NAME:vkr_player>\",
    \"shipping\": \"$<TARGET_FILE_NAME:vkr_player_shipping>\"
  },
  \"engine\": \"engine\",
  \"engine_include\": [${VKR_PLAYER_ENGINE_JSON}],
  \"shaders\": \"${VKR_SHADER_CATALOG_DIR}\"
}
")

# The bakery and the editor find the template in this build tree; a
# distributed editor later ships it beside itself.
target_compile_definitions(vkr_bakery PRIVATE
    VKR_BAKERY_PLAYER_TEMPLATE_DEFAULT="${VKR_PLAYER_TEMPLATE}")
