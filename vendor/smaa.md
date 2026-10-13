# SMAA lookup textures

`vendor/smaa/` holds unmodified files from
[iryoku/smaa](https://github.com/iryoku/smaa) at commit
`71c806a838bdd7d517df19192a20f0c61b3ca29d` (MIT, `LICENSE.txt`):
`Textures/AreaTex.h` (160 × 560, R8G8) and `Textures/SearchTex.h`
(64 × 16, R8), the precomputed area and search tables of Enhanced Subpixel
Morphological Antialiasing (Jimenez et al. 2012).

The Vulkan renderer uploads both once at device creation
([`vkr_vulkan_resources.c`](../renderer/src/vulkan/vkr_vulkan_resources.c))
for the desktop pipeline's SMAA passes, whose shader
([`post/smaa.slang`](../renderer/src/shaders/vulkan/slang/post/smaa.slang))
ports `SMAA.hlsl` from the same commit and keeps its licence notice.
