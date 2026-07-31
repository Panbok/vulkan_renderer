---
status: investigation
updated: 2026-07-31
authority: investigation
---

> **Archived.** Superseded by [`../architecture/renderer-architecture-spec.md`](../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Skybox & Cube Map Implementation Post-Mortem

**Legacy note:** This postmortem references the removed view/layer system and
its sorting logic. Render orchestration now uses the render graph; view modules
are render helpers invoked by pass executors.

## Summary

Implementation of cube map support and skybox rendering in the Vulkan renderer. The feature took significant debugging effort due to multiple interconnected issues across the rendering pipeline.

---

## Issues Encountered and Solutions (13 Total)

### 1. Unknown Pipeline Domain Error

**Error:**
```
[FATAL]: Unknown pipeline domain: 6
```

**Cause:** Added `VKR_PIPELINE_DOMAIN_SKYBOX` enum but forgot to handle it in `vulkan_framebuffer_regenerate_for_domain()`.

**Fix:** Added `case VKR_PIPELINE_DOMAIN_SKYBOX:` in `vulkan_framebuffer.c` with color + depth attachments.

**Prevention:** When adding new enum values, grep for all switch statements on that enum type.

---

### 2. Shader File Not Found

**Error:**
```
[FATAL]: Shader file does not exist: shaders/default.skybox.spv
```

**Cause:** Incorrect path in `.shadercfg` file - missing `assets/` prefix.

**Fix:** Changed `stagefiles=shaders/default.skybox.spv` to `stagefiles=assets/shaders/default.skybox.spv`.

**Prevention:** Use consistent path conventions. Consider validating paths at config parse time.

---

### 3. Zero-Sized Buffer Creation

**Error:**
```
[ERROR]: vkCreateBuffer(): pCreateInfo->size is zero.
[FATAL]: Failed to create Vulkan instance uniform buffer
```

**Cause:** Skybox shader has no instance uniforms (only a sampler), so `instance_ubo_size == 0`. The code unconditionally tried to create an instance UBO.

**Fix:** Modified `vulkan_shader_object_create()` to skip instance UBO creation when `instance_ubo_stride == 0`:
```c
if (out_shader_object->instance_ubo_stride > 0) {
    // Create instance uniform buffer
} else {
    MemZero(&out_shader_object->instance_uniform_buffer, ...);
}
```

**Prevention:** Always handle edge cases where optional resources may not be needed.

---

### 4. Descriptor Binding Mismatch

**Error:**
```
[ERROR]: SPIR-V uses descriptor [Set 1, Binding 4] but was not declared in pipeline layout.
```

**Cause:** Shader used `[[vk::binding(4, 1)]]` for sampler, but the C code generates descriptor layouts dynamically with sequential bindings (0, 1, 2...).

**Fix:** Changed shader bindings to match the generated layout:
- Binding 0: Instance UBO (if present)
- Binding 1: Combined image sampler

**Prevention:** Document the descriptor set layout generation logic. Consider using reflection to validate shader bindings against generated layouts.

---

### 5. Null Instance State Access

**Error:**
```
runtime error: index 4294967295 out of bounds
```

**Cause:** Attempting to use shader instance resources without first acquiring an instance state ID.

**Fix:** Added proper instance state acquisition flow:
```c
vkr_pipeline_registry_acquire_instance_state(&rf->pipeline_registry,
    state->pipeline, &state->instance_state, &instance_err);
```

**Prevention:** Follow established patterns from working passes (pass.ui, pass.world) when implementing new passes.

---

### 6. Image Layout Transition Error

**Error:**
```
[ERROR]: vkQueuePresentKHR(): images passed to present must be in layout
VK_IMAGE_LAYOUT_PRESENT_SRC_KHR but is in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
```

**Cause:** The World renderpass was configured as the first pass (with `prev_name = {0}`), so its `initialLayout` was `VK_IMAGE_LAYOUT_UNDEFINED` and `finalLayout` was `COLOR_ATTACHMENT_OPTIMAL`. But with Skybox added before World, the layout chain was broken.

**Fix:** Updated `renderer_frontend.c` to define the correct renderpass chain:
```c
// Skybox: prev_name = {0}, next_name = "World"
// World: prev_name = "Skybox", next_name = "UI"
// UI: prev_name = "World", next_name = {0}
```

Also fixed World renderpass to use `VKR_RENDERPASS_USE_DEPTH` (LOAD color) instead of `CLEAR_COLOR`.

**Prevention:** Renderpass chaining is fragile. Consider automatic layout deduction based on pass order.

---

### 7. Layer Sorting Bug

**Error:** World layer rendered before Skybox despite Skybox having `order = -10`.

**Cause:** The `order` field in `VkrLayerSortEntry` was declared as `uint32_t`, causing negative orders to wrap to large positive values.

**Fix:** Changed to `int32_t`:
```c
typedef struct VkrLayerSortEntry {
    int32_t order;  // Was uint32_t
    // ...
} VkrLayerSortEntry;
```

**Prevention:** Use signed types for values that can be negative. Add assertions to catch sorting anomalies.

---

### 8. Vertex Stride Mismatch

**Symptom:** Skybox geometry appeared corrupted or missing.

**Cause:** `Vec3` type is SIMD-aligned to 16 bytes, making `VkrVertex3d` 80 bytes. The skybox vertex data was defined with incorrect stride assumptions.

**Fix:** Used `VkrVertex3d` struct directly for skybox vertices:
```c
vkr_global const VkrVertex3d skybox_cube_vertices[] = {
    {.position = {-1.0f, -1.0f, -1.0f}, .normal = {0}, ...},
    // ...
};
```

**Prevention:** Always use the same vertex struct type that the shader expects. Avoid manual float arrays for vertex data.

---

### 9. UBO Memory Layout Mismatch (Critical!)

**Symptom:** Skybox appeared as a flat 2D image that rotated with the camera but didn't respond to camera direction.

**Cause:** The shader's UBO struct had `view` first, then `projection`:
```glsl
struct GlobalUniformBufferObject {
    float4x4 view;        // WRONG ORDER
    float4x4 projection;
};
```

But the C struct had `projection` first:
```c
typedef struct VkrGlobalMaterialState {
    Mat4 projection;  // Correct order
    Mat4 view;
    // ...
} VkrGlobalMaterialState;
```

The matrices were being read from the wrong memory locations!

**Fix:** Aligned shader struct with C struct:
```glsl
struct GlobalUniformBufferObject {
    float4x4 projection;  // Must match C struct order!
    float4x4 view;
};
```

**Prevention:**
- Generate shader UBO structs from C headers or vice versa
- Add compile-time assertions to verify struct sizes match
- Use reflection to validate memory layouts
- Document UBO layouts in a single source of truth

---

### 10. Depth Attachment Layout Mismatch

**Error:**
```
vkCreateRenderPass(): pAttachments[1] format is VK_FORMAT_D32_SFLOAT and loadOp is
VK_ATTACHMENT_LOAD_OP_LOAD, but initialLayout is VK_IMAGE_LAYOUT_UNDEFINED.
```

**Cause:** The depth attachment's `initialLayout` was always set to `VK_IMAGE_LAYOUT_UNDEFINED`, but when `loadOp` is `LOAD` (using `VKR_RENDERPASS_USE_DEPTH` without `CLEAR_DEPTH`), the Vulkan spec requires initialLayout to NOT be `UNDEFINED`.

**Fix:** Set depth `initialLayout` based on whether we're clearing or loading:
```c
bool clear_depth = (cfg->clear_flags & VKR_RENDERPASS_CLEAR_DEPTH) != 0;
VkImageLayout depth_initial_layout = clear_depth
    ? VK_IMAGE_LAYOUT_UNDEFINED
    : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
```

Also changed `storeOp` from `DONT_CARE` to `STORE` to preserve depth for subsequent passes.

**Prevention:** When `loadOp` is `LOAD`, always ensure `initialLayout` is a valid layout (not `UNDEFINED`).

---

### 12. Null Buffer Destruction Crash

**Error:**
```
[DEBUG]: Destroying Vulkan buffer: 0x0
[FATAL]: Assertion Failure: allocator->ctx != NULL
```

**Cause:** `vulkan_buffer_destroy()` tried to destroy the allocator for a buffer that was never created (skybox has no instance UBO).

**Fix:** Added early-exit check:
```c
void vulkan_buffer_destroy(VulkanBackendState *state, VulkanBuffer *buffer) {
    if (buffer->handle == VK_NULL_HANDLE) {
        return;  // Buffer was never created
    }
    // ... rest of destruction
}
```

**Prevention:** Always check for null/invalid handles before cleanup. Consider using a "valid" flag or wrapper type.

---

### 13. Cube Face Culling

**Symptom:** Skybox showed black screen with one cull mode, visible with the other.

**Cause:** Initial confusion about inside vs outside rendering. With `cull_mode=back`, we see front faces (CCW winding). With `cull_mode=front`, we see back faces (CW winding). The cube indices were wound for viewing from inside.

**Fix:** Used `cull_mode=front` in `.shadercfg` to cull front faces and render the inside of the cube.

**Prevention:** Document expected winding conventions. Consider visualization tools for debugging culling issues.

---

## Key Lessons Learned

1. **Memory Layout Alignment**: Shader and C struct layouts MUST match exactly. This is easy to get wrong and hard to debug.

2. **Renderpass Chaining**: Adding a new renderpass requires updating the entire chain configuration.

3. **Optional Resources**: Code paths must handle cases where resources (like instance UBOs) are not needed.

4. **Type Safety**: Use appropriate types (signed vs unsigned) based on the data's semantics.

5. **Validation Layer Messages**: Always read Vulkan validation errors carefully - they usually point directly to the issue.

6. **Reference Implementations**: Having a working reference (kohi engine) was invaluable for comparing approaches.

---

## Files Modified

| File | Changes |
|------|---------|
| `vkr_renderer.h` | Added `VkrCullMode`, `VkrSkybox`, `VKR_PIPELINE_DOMAIN_SKYBOX` |
| `vkr_resources.h` | Added `cull_mode` to `VkrShaderConfig` |
| `shader_loader.c` | Parse `cull_mode=` from config |
| `vulkan_image.c` | Cube map image/view creation |
| `vulkan_backend.c` | Cube map texture creation |
| `vulkan_pipeline.c` | Dynamic cull mode support |
| `vulkan_framebuffer.c` | Skybox domain handling |
| `vulkan_renderpass.c` | Skybox domain detection, layout handling |
| `vulkan_shaders.c` | Conditional instance UBO creation |
| `vulkan_buffer.c` | Null buffer destruction guard |
| `vkr_texture_system.c` | Cube map loading |
| `renderer_frontend.c` | Skybox pass config, stateless system init |
| `vkr_render_graph.c` | Data-driven pass ordering |
| `vkr_skybox_system.c/h` | Skybox resource system |
| `default.skybox.slang` | Skybox shader |
| `default.skybox.shadercfg` | Skybox shader config |

---

## Recommendations for Future Work

1. **UBO Validation**: Add compile-time or runtime validation that shader UBO layouts match C struct layouts.

2. **Renderpass Graph**: Implement automatic renderpass ordering and layout deduction based on dependencies.

3. **Resource Lifecycle**: Consider RAII-style resource management to prevent null resource access.

4. **Shader Reflection**: Use SPIR-V reflection to validate descriptor bindings at pipeline creation time.

5. **Debug Visualization**: Add debug modes to visualize cube map faces, winding, and coordinate systems.
