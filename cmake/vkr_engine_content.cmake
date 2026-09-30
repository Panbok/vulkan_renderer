# Engine resources every game package carries and an installed editor serves
# from its content directory: the render graph, the runtime's fonts with
# every file their configurations name, and the default mannequin the FPS
# module spawns at a Player Start with its credits notice. VKR_ENGINE_INCLUDE
# lists the closure roots a package walks, VKR_ENGINE_FILES every file they
# reach. Changing this set changes every package.
set(VKR_ENGINE_INCLUDE
    assets/render_graphs/main.rendergraph.json
    assets/fonts/UbuntuMono-cooked.fontcfg
    assets/fonts/UbuntuMono-bitmap.fontcfg
    assets/fonts/UbuntuMono21px.fnt.vkf
    assets/textures/UbuntuMono21px_0.png
    assets/fonts/NotoSansCJK.fontcfg
    assets/characters/mannequin/mannequin.vkb
    assets/characters/mannequin/mannequin.vka
    assets/characters/mannequin/NOTICE.md)
set(VKR_ENGINE_FILES
    ${VKR_ENGINE_INCLUDE}
    assets/fonts/UbuntuMono-cooked.vkfa
    assets/fonts/UbuntuMono21px.fnt
    assets/textures/UbuntuMono21px_0.png.vkt
    assets/fonts/NotoSansCJK-Regular.ttc
    assets/characters/mannequin/materials/gltf_mat_95137e68ef96798b_0.mt
    assets/characters/mannequin/materials/gltf_mat_95137e68ef96798b_1.mt
    assets/characters/mannequin/materials/gltf_mat_95137e68ef96798b_2.mt)
