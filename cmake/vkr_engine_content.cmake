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
    ${VKR_MANNEQUIN_INCLUDE})
set(VKR_ENGINE_FILES
    ${VKR_ENGINE_INCLUDE}
    assets/fonts/UbuntuMono-cooked.vkfa
    assets/fonts/UbuntuMono21px.fnt
    assets/textures/UbuntuMono21px_0.png.vkt
    assets/fonts/NotoSansCJK-Regular.ttc
    ${VKR_MANNEQUIN_FILES})
