---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Writable Textures, Resize, and Region Writes (Vulkan)

## Goal
Introduce CPU-writable textures with region write support and resize-with-preserve behavior, fully integrated across the public frontend (`vkr_renderer.h`), texture system (`vkr_texture_system.*`), and Vulkan backend. This prepares the renderer for render targets, configurable render passes, and later cube maps.

## Scope & Decisions
- Support sub-region writes (x/y/width/height) with mip-level and array-layer selection.
- Resize should preserve contents when possible (same format/usage), otherwise recreate empty as fallback.
- Writable textures are flagged via `VKR_TEXTURE_PROPERTY_WRITABLE_BIT` and are not auto-released by the texture system.
- All writable textures are created with usage enabling sampling, transfer, and color-attachment (forward-compatible with render targets).

## API Changes (Public)
### vkr_renderer.h
1) New write-region type:
- `VkrTextureWriteRegion { uint32_t mip_level; uint32_t array_layer; uint32_t x, y, width, height; }`

2) New frontend API:
- `vkr_renderer_create_writable_texture(renderer, desc, out_error)`
- `vkr_renderer_write_texture(renderer, texture, const void* data, uint64_t size)` (full image, mip=0)
- `vkr_renderer_write_texture_region(renderer, texture, const VkrTextureWriteRegion* region, const void* data, uint64_t size)`
- `vkr_renderer_resize_texture(renderer, texture, uint32_t new_width, uint32_t new_height, bool8_t preserve_contents)`

3) Backend interface extensions:
- `texture_write(backend_state, handle, region, data, size)`
- `texture_resize(backend_state, handle, new_width, new_height, bool8_t preserve_contents)`

4) Keep existing `VkrTextureDescription` and use `VKR_TEXTURE_PROPERTY_WRITABLE_BIT` to mark writable.

Code references:
```384:389:lib/src/renderer/vkr_renderer.h
typedef enum VkrTexturePropertyBits {
  VKR_TEXTURE_PROPERTY_WRITABLE_BIT = 1 << 0,
  VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT = 1 << 1,
} VkrTexturePropertyBits;
```

```819:861:lib/src/renderer/vkr_renderer.h
VkrBackendResourceHandle (*texture_create)(void *backend_state,
                                           const VkrTextureDescription *desc,
                                           const void *initial_data);
VkrRendererError (*texture_update)(void *backend_state,
                                   VkrBackendResourceHandle handle,
                                   const VkrTextureDescription *desc);
void (*texture_destroy)(void *backend_state, VkrBackendResourceHandle handle);
// ...
uint64_t (*get_and_reset_descriptor_writes_avoided)(void *backend_state);
```

## Texture System Changes
### vkr_texture_system.h/.c
Add high-level helpers with system-owned lifetime and mapping:
- `vkr_texture_system_create_writable(system, name, desc, out_handle, out_error)`
  - Sets `desc.properties |= VKR_TEXTURE_PROPERTY_WRITABLE_BIT`
  - Adds to map with `auto_release = false` (not auto-released)
- `vkr_texture_system_write(system, handle, data, size)` (full-image write)
- `vkr_texture_system_write_region(system, handle, region, data, size)`
- `vkr_texture_system_resize(system, handle, new_w, new_h, preserve)`
  - On success, update `texture->description.width/height` and increment `generation`
  - Return updated `VkrTextureHandle` (same id, new generation)
- `vkr_texture_system_register_external(system, name, backend_handle, desc, out_handle)`
  - Registers an externally-created texture; sets `auto_release = false`

Auto-release enforcement (no unload when refcount hits 0 for writable):
```291:304:lib/src/renderer/systems/vkr_texture_system.c
if (entry->ref_count == 0 && entry->auto_release) {
  // ... unload path ...
}
```

## Frontend Implementation
### renderer_frontend.c
- Implement new `vkr_renderer_*` functions by delegating to backend interface and managing any required frontend state.
- For `create_writable_texture`: call the existing backend `texture_create` with `initial_data = NULL` and `desc.properties` containing `WRITABLE`.
- For `write_*`: call `backend.texture_write`.
- For `resize`: call `backend.texture_resize` with `preserve` flag.

## Vulkan Backend Implementation
### Creation path (allow NULL initial_data for writable)
Relax the assert in `renderer_vulkan_create_texture` to allow `initial_data == NULL` when `desc.properties` has WRITABLE, and initialize the image to `SHADER_READ_ONLY_OPTIMAL` directly:
```975:979:lib/src/renderer/vulkan/vulkan_backend.c
assert_log(backend_state != NULL, "Backend state is NULL");
assert_log(desc != NULL, "Texture description is NULL");
assert_log(initial_data != NULL, "Initial data is NULL");
```
Change: only assert when not writable; if writable, create image+sampler and transition `UNDEFINED -> SHADER_READ_ONLY_OPTIMAL` without copy.

We will reuse usage flags already used for creation:
```1013:1042:lib/src/renderer/vulkan/vulkan_backend.c
const VkrBufferDescription staging_buffer_desc = {
    .size = image_size,
    .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
    .memory_properties = vkr_memory_property_flags_from_bits(
        VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
    .buffer_type = buffer_type,
    .bind_on_create = true_v,
};
```

### Write region
Add `renderer_vulkan_write_texture(backend_state, handle, region, data, size)`:
- Allocate staging buffer; upload `data`.
- Single-use command buffer:
  - Transition target subresource: `SHADER_READ_ONLY_OPTIMAL -> TRANSFER_DST_OPTIMAL`
  - `vkCmdCopyBufferToImage` with `VkBufferImageCopy` configured for `region`
  - Transition back: `TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL`
- Submit using the current frame fence as in texture creation.

Transitions are supported by existing helper:
```172:195:lib/src/renderer/vulkan/vulkan_image.c
if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
    new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
  // ...
} else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
           new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
  // ...
} else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
           new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
  // ...
} else {
  log_fatal("Unsupported layout transition!");
  return false_v;
}
```

Note: for region writes we do not regenerate mipmaps. Full-image writes to mip-0 can optionally trigger mip generation later if needed.

### Resize with preserve
Add `renderer_vulkan_resize_texture(backend_state, handle, new_w, new_h, preserve)`:
- Wait for idle or ensure serialization (same approach as sampler update)
- Create a new image (usage: transfer src/dst, sampled, color attachment) with new dimensions and computed mip-levels
- If `preserve == true` and format/usage allow:
  - Temporary transitions on old/new:
    - old: to `TRANSFER_SRC_OPTIMAL`
    - new: to `TRANSFER_DST_OPTIMAL`
  - Use `vkCmdBlitImage` (filter `VK_FILTER_LINEAR` if supported; fallback to `vkCmdCopyImage`)
  - Transition new to `SHADER_READ_ONLY_OPTIMAL`
- Destroy old image and sampler, then recreate sampler for new mip configuration
- Update fields in the same `s_TextureHandle` to keep the opaque handle stable

We reuse the staging/cmd buffer patterns you already use when copying to images:
```1082:1094:lib/src/renderer/vulkan/vulkan_backend.c
if (!vulkan_image_copy_from_buffer(state, &texture->texture.image,
                                   staging_buffer->buffer.handle,
                                   &temp_command_buffer)) {
  log_fatal("Failed to copy buffer to image");
  // ...
}
```

### Usage and layout guarantees
- All writable textures will be created with: `TRANSFER_SRC | TRANSFER_DST | SAMPLED | COLOR_ATTACHMENT` usage to enable future render targets.
- Initial layout: transition to `SHADER_READ_ONLY_OPTIMAL` so they are immediately sampleable.

## Future-proofing for Cube Maps
- The new `VkrTextureWriteRegion` includes `array_layer` and `mip_level` to naturally extend to cube maps (array_layers = 6). A later revision can extend `VkrTextureDescription` with `array_layers` and a cube-map view type.

## Testing & Validation
- Create a writable 2D texture (RGBA8), write a checkerboard region, sample in a material; verify visuals.
- Resize the texture up/down with `preserve=true`; verify content scales/copied as expected and texture remains sampleable.
- Ensure writable textures aren’t auto-released when refcounts drop to 0.
- Verify swapchain/frame pacing unaffected (single-use command buffers + fences).

## Key Edits Summary
- Public API additions in `lib/src/renderer/vkr_renderer.h`.
- Texture system API and behavior in `lib/src/renderer/systems/vkr_texture_system.h/.c` (auto_release=false for writable, registry helpers, write/resize wrappers).
- Frontend glue in `renderer_frontend.c` (call into backend for new ops).
- Vulkan backend in `lib/src/renderer/vulkan/vulkan_backend.c` (allow `NULL` initial_data for writable; add write/resize implementations) and leverage helpers in `lib/src/renderer/vulkan/vulkan_image.c`.

Code references:
```291:304:lib/src/renderer/systems/vkr_texture_system.c
if (entry->ref_count == 0 && entry->auto_release) {
  // ... unload path ...
}
```

```975:979:lib/src/renderer/vulkan/vulkan_backend.c
assert_log(backend_state != NULL, "Backend state is NULL");
assert_log(desc != NULL, "Texture description is NULL");
assert_log(initial_data != NULL, "Initial data is NULL");
```

```1013:1042:lib/src/renderer/vulkan/vulkan_backend.c
const VkrBufferDescription staging_buffer_desc = {
    .size = image_size,
    .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_SRC),
    .memory_properties = vkr_memory_property_flags_from_bits(
        VKR_MEMORY_PROPERTY_HOST_VISIBLE | VKR_MEMORY_PROPERTY_HOST_COHERENT),
    .buffer_type = buffer_type,
    .bind_on_create = true_v,
};
```

```1082:1094:lib/src/renderer/vulkan/vulkan_backend.c
if (!vulkan_image_copy_from_buffer(state, &texture->texture.image,
                                   staging_buffer->buffer.handle,
                                   &temp_command_buffer)) {
  log_fatal("Failed to copy buffer to image");
  // ...
}
```

```172:195:lib/src/renderer/vulkan/vulkan_image.c
if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
    new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
  // ...
} else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
           new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
  // ...
} else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
           new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
  // ...
} else {
  log_fatal("Unsupported layout transition!");
  return false_v;
}
```


