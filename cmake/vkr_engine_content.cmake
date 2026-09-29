# Engine resources every game package carries and an installed editor serves
# from its content directory: the render graph and the runtime's fonts with
# every file their configurations name. VKR_ENGINE_INCLUDE lists the closure
# roots a package walks, VKR_ENGINE_FILES every file they reach. Changing this
# set changes every package.
set(VKR_ENGINE_INCLUDE
    assets/render_graphs/main.rendergraph.json
    assets/fonts/UbuntuMono-cooked.fontcfg
    assets/fonts/UbuntuMono-bitmap.fontcfg
    assets/fonts/UbuntuMono21px.fnt.vkf
    assets/textures/UbuntuMono21px_0.png
    assets/fonts/NotoSansCJK.fontcfg)
set(VKR_ENGINE_FILES
    ${VKR_ENGINE_INCLUDE}
    assets/fonts/UbuntuMono-cooked.vkfa
    assets/fonts/UbuntuMono21px.fnt
    assets/textures/UbuntuMono21px_0.png.vkt
    assets/fonts/NotoSansCJK-Regular.ttc)
