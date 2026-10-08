# Engine resources every game package carries and an installed editor serves
# from its content directory: the render graphs of both pipeline classes
# (ADR-087), the runtime's fonts with
# every file their configurations name, and the default mannequin the FPS
# module spawns at a Player Start with its materials, textures and credits
# notice (listed by the generated vkr_mannequin_content.cmake).
# VKR_ENGINE_INCLUDE lists the closure roots a package walks,
# VKR_ENGINE_FILES every file they reach. Changing this set changes every
# package.
include("${CMAKE_CURRENT_LIST_DIR}/vkr_mannequin_content.cmake")
set(VKR_ENGINE_INCLUDE
    assets/render_graphs/main.rendergraph.json
    assets/render_graphs/tiled.rendergraph.json
    assets/fonts/UbuntuMono-cooked.fontcfg
    assets/fonts/UbuntuMono-bitmap.fontcfg
    assets/fonts/UbuntuMono21px.fnt.vkf
    assets/textures/UbuntuMono21px_0.png
    assets/fonts/NotoSansCJK.fontcfg
    assets/fonts/NotoSansCJK-Windows.fontcfg
    # The font licences travel with every package, since any package may
    # carry these fonts (text fonts join only when used).
    assets/fonts/ubuntu-font-licence-1.0.txt
    assets/fonts/UbuntuMono-R.ttf.license.md
    assets/fonts/UbuntuMono-Bold.ttf.license.md
    assets/fonts/Inter-OFL.txt
    # Dev grid materials brush faces use by default and every material of
    # the editor's brush palette (editor_viewport.c), since a level built
    # from the palette names them all (docs/proposals/level-design-toolkit.md).
    assets/materials/dev/dev_grid.mt
    assets/materials/dev/dev_floor.mt
    assets/materials/dev/dev_wall.mt
    assets/materials/dev/dev_orange.mt
    assets/materials/dev/dev_blue.mt
    assets/materials/dev/dev_trigger.mt
    assets/materials/dev/dev_clip.mt
    assets/materials/dev/dev_decal.mt
    assets/materials/dev/dev_concrete.mt
    assets/materials/dev/dev_metal.mt
    assets/materials/dev/dev_dark.mt
    assets/materials/dev/dev_tile.mt
    assets/materials/dev/dev_wood.mt
    assets/materials/dev/dev_hazard.mt
    assets/materials/dev/dev_red.mt
    assets/materials/dev/dev_green.mt
    assets/materials/dev/dev_light.mt
    ${VKR_MANNEQUIN_INCLUDE})
set(VKR_ENGINE_FILES
    ${VKR_ENGINE_INCLUDE}
    assets/fonts/UbuntuMono-cooked.vkfa
    assets/fonts/UbuntuMono21px.fnt
    assets/textures/UbuntuMono21px_0.png.vkt
    assets/textures/dev/dev_grid.png
    assets/textures/dev/dev_grid.png.vkt
    assets/fonts/NotoSansCJK-Regular.ttc
    ${VKR_MANNEQUIN_FILES})

# Engine text fonts past the default (SceneTextSettings.font): roots and
# every file they reach. A package carries one only when a scene's text
# names it, since packing follows references from the roots above; player
# templates and an installed editor's content root hold them all.
set(VKR_TEXT_FONT_INCLUDE
    assets/fonts/UbuntuMono-Bold-cooked.fontcfg
    assets/fonts/Inter-Regular-cooked.fontcfg
    assets/fonts/Inter-SemiBold-cooked.fontcfg)
set(VKR_TEXT_FONT_FILES
    ${VKR_TEXT_FONT_INCLUDE}
    assets/fonts/UbuntuMono-Bold-cooked.vkfa
    assets/fonts/Inter-Regular-cooked.vkfa
    assets/fonts/Inter-SemiBold-cooked.vkfa)

# Engine textures the files above name, as `source|class`, and paired
# normal/roughness bakes, as `normal|metal-roughness|normal output|
# metal-roughness output|normal scale|roughness factor`. Textures are
# host-native and untracked (ADR-012), so the build cooks each `<source>.vkt`
# beside its source, and each pair to its outputs, with vkr_bakery before a
# package or install copies them.
set(VKR_ENGINE_TEXTURES
    "assets/textures/UbuntuMono21px_0.png|color-srgb"
    "assets/textures/dev/dev_grid.png|color-srgb"
    ${VKR_MANNEQUIN_TEXTURES})
set(VKR_ENGINE_TEXTURE_PAIRS
    ${VKR_MANNEQUIN_TEXTURE_PAIRS})

# Tracked textures the repository's default scene, fixture scenes and CPU
# tests read; packages never carry them. An entry without a class takes the
# class Bakery infers from its name, as `vkr_bakery build assets/bakery.json`
# does for assets/textures.
set(VKR_REPOSITORY_TEXTURES
    "assets/textures/cobblestone_NRM.png"
    "assets/textures/cobblestone_SPEC.png"
    "assets/textures/defaultwhite.jpg"
    "assets/textures/logo_white.png"
    "assets/textures/test512.png|color-srgb"
    "assets/textures/transparency-test-diffuse-texture.png"
    "tests/fixtures/rendering/anisotropy/direction_y.png|data-mask"
    "tests/fixtures/rendering/clearcoat/base_normal_plus_x.png|normal-rg"
    "tests/fixtures/rendering/clearcoat/coat_mask_one.png|data-mask"
    "tests/fixtures/rendering/clearcoat/coat_normal_minus_x.png|normal-rg"
    "tests/fixtures/rendering/clearcoat/coat_normal_plus_x.png|normal-rg"
    "tests/fixtures/rendering/diffuse_sheet/sheet_base_color.png|color-srgb"
    "tests/fixtures/rendering/local_shadow_transmission/transmission_step.png|data-mask"
    "tests/fixtures/rendering/sheen/sheen_color.png|color-srgb"
    "tests/fixtures/rendering/sheen/sheen_roughness.png|data-mask")

# Defines `vkr_engine_textures` once, for every consumer that includes this
# file: the engine and repository cooks above. Each cook reruns only when its
# source changes; Ninja runs at most two at once, since each encodes on every
# core.
if(NOT TARGET vkr_engine_textures)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS vkr_texture_cook=2)
    set(_vkr_texture_outputs "")
    foreach(entry IN LISTS VKR_ENGINE_TEXTURES VKR_REPOSITORY_TEXTURES)
        string(REPLACE "|" ";" _vkr_parts "${entry}")
        list(GET _vkr_parts 0 _vkr_source)
        set(_vkr_class_arguments "")
        list(LENGTH _vkr_parts _vkr_part_count)
        if(_vkr_part_count GREATER 1)
            list(GET _vkr_parts 1 _vkr_class)
            set(_vkr_class_arguments --texture-class ${_vkr_class})
        endif()
        set(_vkr_output "${CMAKE_SOURCE_DIR}/${_vkr_source}.vkt")
        add_custom_command(OUTPUT "${_vkr_output}"
            COMMAND $<TARGET_FILE:vkr_bakery> tool texture
                --output "${_vkr_output}" --type 2d
                --layer "${CMAKE_SOURCE_DIR}/${_vkr_source}"
                ${_vkr_class_arguments} --no-progress
            DEPENDS "${CMAKE_SOURCE_DIR}/${_vkr_source}" vkr_bakery
            COMMENT "Cooking texture ${_vkr_source}"
            JOB_POOL vkr_texture_cook
            VERBATIM)
        list(APPEND _vkr_texture_outputs "${_vkr_output}")
    endforeach()
    foreach(entry IN LISTS VKR_ENGINE_TEXTURE_PAIRS)
        string(REPLACE "|" ";" _vkr_parts "${entry}")
        list(GET _vkr_parts 0 _vkr_normal)
        list(GET _vkr_parts 1 _vkr_metal_roughness)
        list(GET _vkr_parts 2 _vkr_normal_output)
        list(GET _vkr_parts 3 _vkr_metal_roughness_output)
        list(GET _vkr_parts 4 _vkr_normal_scale)
        list(GET _vkr_parts 5 _vkr_roughness_factor)
        set(_vkr_outputs
            "${CMAKE_SOURCE_DIR}/${_vkr_normal_output}"
            "${CMAKE_SOURCE_DIR}/${_vkr_metal_roughness_output}")
        add_custom_command(OUTPUT ${_vkr_outputs}
            COMMAND $<TARGET_FILE:vkr_bakery> tool texture
                --paired-normal "${CMAKE_SOURCE_DIR}/${_vkr_normal_output}"
                --output "${CMAKE_SOURCE_DIR}/${_vkr_metal_roughness_output}"
                --type 2d
                --layer "${CMAKE_SOURCE_DIR}/${_vkr_normal}"
                --layer "${CMAKE_SOURCE_DIR}/${_vkr_metal_roughness}"
                --normal-scale ${_vkr_normal_scale}
                --roughness-factor ${_vkr_roughness_factor} --no-progress
            DEPENDS "${CMAKE_SOURCE_DIR}/${_vkr_normal}"
                "${CMAKE_SOURCE_DIR}/${_vkr_metal_roughness}" vkr_bakery
            COMMENT "Baking paired normal/roughness ${_vkr_normal_output}"
            JOB_POOL vkr_texture_cook
            VERBATIM)
        list(APPEND _vkr_texture_outputs ${_vkr_outputs})
    endforeach()
    add_custom_target(vkr_engine_textures DEPENDS ${_vkr_texture_outputs})
endif()
