---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Texture Filtering and Mipmaps Plan

## Overview
We need to wire the new texture filtering options in `lib/src/renderer/vkr_renderer.h` through the Vulkan backend, add mipmap generation for trilinear/anisotropic sampling, expose a `vkr_renderer_update_texture` entry point for runtime filter changes, and surface keybindings in `app/src/main.c` to toggle modes. The plan below sequences the API, backend, system, and app work.

## Current State
- Sampler creation in `lib/src/renderer/vulkan/vulkan_backend.c` is hard-coded to linear filtering with `maxLod=0` and no generated mips; `VkrTextureDescription` repeat/filter fields are unused.
- Textures are created with a single mip level; there is no texture update API (only create/destroy).
- Descriptor updates in `lib/src/renderer/vulkan/vulkan_shaders.c` rely on `texture->description.generation`, so sampler/image changes must bump generation.
- The app has no filtering toggles; materials/textures use whatever default sampler is built.

## Plan
1) API and defaults
- Define the mapping from `VkrTextureFilterMode` to Vulkan sampler settings (min/mag filters, mipmapMode, anisotropy flag, maxLod). Target defaults: bilinear filtering, repeat wrap, max anisotropy clamped to device limits.
- Decide mip allocation policy to allow runtime switches (recommended: allocate full mip chain by default using `floor(log2(max(width, height))) + 1` for 2D textures).
- Ensure `VkrTextureDescription` instances (default textures in `lib/src/renderer/systems/vkr_texture_system.c`, loader in `lib/src/renderer/resources/loaders/texture_loader.c`, any hard-coded descriptions) populate repeat/filter modes with sane defaults.

2) Vulkan backend: sampler creation and mip pipeline
- Add a helper to build `VkSamplerCreateInfo` from `VkrTextureDescription`, device capabilities (`samplerAnisotropy`), and computed mip levels; map `VkrTextureRepeatMode` to `VkSamplerAddressMode` and select border color for clamp-to-border.
- Update `renderer_vulkan_create_texture` to:
  - Compute mip level count based on the chosen filter mode/policy.
  - Create images/views with that mip count and keep it in `VulkanImage`.
  - After uploading level 0, generate mips on-GPU (e.g., `vkCmdBlitImage` chain) with proper per-level layout transitions; detect formats that lack linear blit support and fall back to single-level sampling or copy.
  - Set sampler `minLod/maxLod` to `(0, mip_levels-1)` and enable anisotropy only when supported.
- Extend `vulkan_image_transition_layout` or add a dedicated mip-generation helper to handle per-level transitions (TRANSFER_DST → TRANSFER_SRC/SHADER_READ).

3) Texture update API
- Add `vkr_renderer_update_texture` to `lib/src/renderer/vkr_renderer.h`/`renderer_frontend.c` and a backend hook `texture_update` in the interface (`lib/src/renderer/vulkan/vulkan_backend.h`).
- Define an update payload (reuse `VkrTextureDescription` or a smaller `VkrTextureUpdateInfo`) to change filter/repeat modes (and, if needed later, pixel data).
- Implement `renderer_vulkan_update_texture` to rebuild the sampler (and regenerate mips if the mip policy changes), bump `texture->description.generation`, and trigger descriptor refresh safely (idle/wait or deferred update strategy).
- Keep CPU-side descriptions in sync in the texture system to reflect the active filter/mip state.

4) Frontend/system plumbing
- Add a texture-system helper to apply filter changes to a `VkrTextureHandle` by updating its description, calling `vkr_renderer_update_texture`, and bumping generation/ref counts as needed.
- If materials or loaders expose filter hints, thread them through to texture creation/update; otherwise rely on defaults and runtime toggles.
- Ensure default scene setup uses the new defaults so descriptor generations match the backend sampler state.

5) App keybindings and UX
- Choose target textures to showcase filtering switches (e.g., the world material’s diffuse map or a designated demo texture handle from the texture system).
- Add keybindings in `app/src/main.c` to cycle through NEAREST → LINEAR → BILINEAR → TRILINEAR → ANISOTROPIC, calling the texture-system helper to update the sampler. Log the active mode and note fallback if anisotropy is unavailable.

6) Validation and testing
- Sanity: assert `texture->texture.image.mip_levels` matches the sampler `maxLod+1`; verify descriptor generations change on filter updates (`vulkan_shaders.c` binding logic).
- Visual/manual: capture frames per mode to confirm mip transitions and anisotropy differences; watch for validation-layer warnings on layout/barrier usage.
- Device caps: exercise a device without `samplerAnisotropy` to ensure we gracefully fall back to trilinear/bilinear.

7) Open questions / risks
- Memory/perf trade-off of always allocating full mip chains vs. conditional mips; decide acceptable default.
- What anisotropy level to pick (device max vs. configurable cap)?
- Handling clamp-to-border color selection and SRGB vs. UNORM formats when blitting mips.
- Whether we need to persist CPU-side pixels for rebuilds or rely solely on GPU mip generation.
