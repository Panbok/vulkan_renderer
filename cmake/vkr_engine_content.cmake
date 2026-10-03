# Engine resources every game package carries and an installed editor serves
# from its content directory: the render graph, the runtime's fonts with
# every file their configurations name, and the default mannequin the FPS
# module spawns at a Player Start with its materials, textures and credits
# notice (listed by the generated vkr_mannequin_content.cmake).
# VKR_ENGINE_INCLUDE lists the closure roots a package walks,
# VKR_ENGINE_FILES every file they reach. Changing this set changes every
# package.
include("${CMAKE_CURRENT_LIST_DIR}/vkr_mannequin_content.cmake")
set(VKR_ENGINE_INCLUDE
    assets/render_graphs/main.rendergraph.json
    assets/fonts/UbuntuMono-cooked.fontcfg
    assets/fonts/UbuntuMono-bitmap.fontcfg
    assets/fonts/UbuntuMono21px.fnt.vkf
    assets/textures/UbuntuMono21px_0.png
    assets/fonts/NotoSansCJK.fontcfg
    assets/fonts/NotoSansCJK-Windows.fontcfg
    ${VKR_MANNEQUIN_INCLUDE})
set(VKR_ENGINE_FILES
    ${VKR_ENGINE_INCLUDE}
    assets/fonts/UbuntuMono-cooked.vkfa
    assets/fonts/UbuntuMono21px.fnt
    assets/textures/UbuntuMono21px_0.png.vkt
    assets/fonts/NotoSansCJK-Regular.ttc
    ${VKR_MANNEQUIN_FILES})

# Engine textures the files above name, as `source|class`. Textures are
# host-native and untracked (ADR-012), so the build cooks each `<source>.vkt`
# beside its source with vkr_bakery before a package or install copies it.
set(VKR_ENGINE_TEXTURES
    "assets/textures/UbuntuMono21px_0.png|color-srgb"
    ${VKR_MANNEQUIN_TEXTURES})

# Defines `vkr_engine_textures` once, for every consumer that includes this
# file. Each cook reruns only when its source changes.
if(NOT TARGET vkr_engine_textures)
    set(_vkr_engine_texture_outputs "")
    foreach(entry IN LISTS VKR_ENGINE_TEXTURES)
        string(REPLACE "|" ";" _vkr_parts "${entry}")
        list(GET _vkr_parts 0 _vkr_source)
        list(GET _vkr_parts 1 _vkr_class)
        set(_vkr_output "${CMAKE_SOURCE_DIR}/${_vkr_source}.vkt")
        add_custom_command(OUTPUT "${_vkr_output}"
            COMMAND $<TARGET_FILE:vkr_bakery> tool texture
                --output "${_vkr_output}" --type 2d
                --layer "${CMAKE_SOURCE_DIR}/${_vkr_source}"
                --texture-class ${_vkr_class} --no-progress
            DEPENDS "${CMAKE_SOURCE_DIR}/${_vkr_source}" vkr_bakery
            COMMENT "Cooking engine texture ${_vkr_source}"
            VERBATIM)
        list(APPEND _vkr_engine_texture_outputs "${_vkr_output}")
    endforeach()
    add_custom_target(vkr_engine_textures DEPENDS ${_vkr_engine_texture_outputs})
endif()
