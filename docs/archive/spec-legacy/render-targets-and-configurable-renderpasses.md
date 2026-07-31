---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Render Targets + Configurable Render Passes (Design Plan)

## Goals
- Add named, configurable render passes with a backend registry (query by name).
- Introduce explicit render targets (framebuffers built from textures) usable by passes.
- Keep current domain-based auto pass switching, but add explicit begin/end-by-renderpass APIs.
- Extend shader config (.shadercfg) to specify `renderpass: "Renderpass.Builtin.World"`; fallback to World when missing.
- Initialize built-in World/UI passes and window/depth render targets; regenerate on swapchain resize.
- Prepare the path for cube map render targets (skybox) in subsequent work.

## Public API & Types (lib/src/renderer/vkr_renderer.h)
- Add clear flags:
  - `typedef enum VkrRenderPassClearFlags { VKR_RENDERPASS_CLEAR_NONE = 0, VKR_RENDERPASS_CLEAR_COLOR = 1<<0, VKR_RENDERPASS_CLEAR_DEPTH = 1<<1, VKR_RENDERPASS_CLEAR_STENCIL = 1<<2 } VkrRenderPassClearFlags;`
- Add render pass config (frontend-visible):
  - `typedef struct VkrRenderPassConfig { String8 name; String8 prev_name; String8 next_name; Vec4 render_area; Vec4 clear_color; uint8_t clear_flags; } VkrRenderPassConfig;`
- Render pass and render target handles (opaque):
  - `typedef struct s_RenderPass* VkrRenderPassHandle;`
  - `typedef struct s_RenderTarget* VkrRenderTargetHandle;`
- Render target description (frontend-visible):
  - `typedef struct VkrRenderTargetDesc { bool8_t sync_to_window_size; uint8_t attachment_count; VkrTextureOpaqueHandle* attachments; uint32_t width; uint32_t height; } VkrRenderTargetDesc;`
- Frontend API additions:
  - Render pass lifecycle: `VkrRenderPassHandle vkr_renderer_renderpass_create(const VkrRenderPassConfig* cfg); void vkr_renderer_renderpass_destroy(VkrRenderPassHandle pass); VkrRenderPassHandle vkr_renderer_renderpass_get(String8 name);`
  - Render target lifecycle: `VkrRenderTargetHandle vkr_renderer_render_target_create(const VkrRenderTargetDesc* desc, VkrRenderPassHandle pass); void vkr_renderer_render_target_destroy(VkrRenderTargetHandle target, bool8_t free_internal_memory);`
  - Frame usage (explicit path): `VkrRendererError vkr_renderer_begin_render_pass_ex(VkrRenderPassHandle pass, VkrRenderTargetHandle target); VkrRendererError vkr_renderer_end_render_pass_ex();`
  - Swapchain attachments for building targets: `VkrTextureOpaqueHandle vkr_renderer_window_attachment_get(uint32_t image_index); VkrTextureOpaqueHandle vkr_renderer_depth_attachment_get();`
  - Backend initialization config (mirrors kohi):
    - `typedef struct VkrRendererBackendConfig { const char* application_name; uint16_t renderpass_count; VkrRenderPassConfig* pass_configs; void (*on_render_target_refresh_required)(); } VkrRendererBackendConfig;`

## Backend Interface Extensions (lib/src/renderer/vkr_renderer.h)
- Extend `VkrRendererBackendInterface` with:
  - Render pass registry: `VkrRenderPassHandle (*renderpass_create)(void* backend_state, const VkrRenderPassConfig* cfg); void (*renderpass_destroy)(void* backend_state, VkrRenderPassHandle pass); VkrRenderPassHandle (*renderpass_get)(void* backend_state, const char* name);`
  - Render target: `VkrRenderTargetHandle (*render_target_create)(void* backend_state, const VkrRenderTargetDesc* desc, VkrRenderPassHandle pass); void (*render_target_destroy)(void* backend_state, VkrRenderTargetHandle target);`
  - Explicit usage: `VkrRendererError (*begin_render_pass_ex)(void* backend_state, VkrRenderPassHandle pass, VkrRenderTargetHandle target); VkrRendererError (*end_render_pass_ex)(void* backend_state);`
  - Swapchain attachments: `VkrTextureOpaqueHandle (*window_attachment_get)(void* backend_state, uint32_t image_index); VkrTextureOpaqueHandle (*depth_attachment_get)(void* backend_state);`
  - Backend init config: pass through `VkrRendererBackendConfig` at initialize time.
- Keep existing domain-based methods for backward compatibility.

## Vulkan Backend: Data & Registry (lib/src/renderer/vulkan/vulkan_types.h)
- Add backend-level opaque structs:
  - `struct s_RenderPass { VulkanRenderPass* vk; String8 name; VkrRenderPassConfig cfg; };`
  - `struct s_RenderTarget { VkFramebuffer handle; uint32_t width, height; bool8_t sync_to_window_size; uint8_t attachment_count; struct s_TextureHandle** attachments; };`
- Extend `VulkanBackendState` with:
  - A small registry mapping `String8 name → s_RenderPass*` (linear array is fine for now).
  - Arrays of wrapper textures for swapchain images: `struct s_TextureHandle** swapchain_image_textures;` and a single `struct s_TextureHandle* depth_texture;`
  - Hook for `on_render_target_refresh_required` callback.

## Vulkan Backend: Render Pass Creation from Config (lib/src/renderer/vulkan/vulkan_renderpass.c/.h)
- Add `vulkan_renderpass_create_from_config(state, const VkrRenderPassConfig* cfg, VulkanRenderPass* out)`:
  - Build VkRenderPass from clear flags and attachment recipe:
    - World-like: color+(optional)depth; initial=UNDEFINED; final=COLOR_ATTACHMENT_OPTIMAL (chain to UI).
    - UI-like: color-only; load=LOAD; initial=COLOR_ATTACHMENT_OPTIMAL; final=PRESENT_SRC_KHR.
  - Derive load/store ops from `clear_flags` and defaults.
- Provide registry helpers in backend to store `(name → s_RenderPass)` and to fetch by name.
- Pre-register built-ins at backend init: `Renderpass.Builtin.World`, `Renderpass.Builtin.UI` using configs passed from frontend (or defaults if not provided).

## Vulkan Backend: Render Targets (lib/src/renderer/vulkan/vulkan_backend.c)
- Implement `renderer_vulkan_render_target_create`:
  - Validate the attachments (image views must exist). Create a `VkFramebuffer` against the VkRenderPass of the provided `VkrRenderPassHandle` and the provided attachments.
  - Width/height come from desc; if `sync_to_window_size`, use current swapchain extent.
- Implement `renderer_vulkan_render_target_destroy`.
- Implement explicit begin/end:
  - `renderer_vulkan_begin_render_pass_ex`: begin render pass with provided framebuffer; set viewport/scissor from cfg.render_area (clamped to extent).
  - `renderer_vulkan_end_render_pass_ex`: end and update `swapchain_image_is_present_ready` when final layout is PRESENT (UI/post style).
- Maintain existing domain-based auto-pass for current pipelines.

## Vulkan Backend: Swapchain Attachments as Textures (lib/src/renderer/vulkan/vulkan_swapchain.c)
- Wrap swapchain color images:
  - For each `VkImageView` create a lightweight `struct s_TextureHandle` that references the image/view (no sampler needed). Store in `state->swapchain_image_textures[i]`.
- Wrap depth attachment similarly into `state->depth_texture` and return via `depth_attachment_get`.
- Update on swapchain recreate: destroy old wrappers and recreate.

## Device Resize & Regeneration Flow (lib/src/renderer/vulkan/vulkan_backend.c)
- On resize (swapchain recreation), after framebuffers/cmd buffers are re-created:
  - Recreate wrapper textures for swapchain images and depth.
  - Trigger `on_render_target_refresh_required()` callback so frontend can rebuild render targets.

## Frontend Plumbing (renderer init + default scene)
- On renderer init (vkr_renderer_initialize + vkr_renderer_systems_initialize):
  - Provide `VkrRendererBackendConfig` with default pass configs for `Renderpass.Builtin.World` and `Renderpass.Builtin.UI`.
  - Cache `VkrRenderPassHandle world/ui` via `vkr_renderer_renderpass_get("Renderpass.Builtin.World")` and `...UI`.
- Implement `regenerate_render_targets()` (frontend-side):
  - For each swapchain image index:
    - Build World target with attachments: `[window_attachment_get(i), depth_attachment_get()]` (2 attachments).
    - Build UI target with attachments: `[window_attachment_get(i)]` (1 attachment).
  - Store `targets[]` per pass.
- Render loop (explicit path available):
  - `begin_frame → begin_render_pass_ex(World, world.targets[i]) → draw → end_render_pass_ex → begin_render_pass_ex(UI, ui.targets[i]) → draw → end_render_pass_ex → end_frame`.
  - Existing domain-based path remains supported.

## Shader Config (.shadercfg) Integration
- Extend shader config schema to accept:
  - `renderpass: "Renderpass.Builtin.World"` (string). If missing, fallback to `Renderpass.Builtin.World`.
- Shader system (on shader/pipeline creation):
  - If `renderpass` specified: look up `VkrRenderPassHandle` and pass its VkRenderPass to pipeline creation.
  - Else: fallback to current domain-based selection.
- Pipeline creation (`vulkan_graphics_pipeline_create`):
  - Accept an optional explicit VkRenderPass handle; use that when present instead of domain render pass.

## Built-ins & Future (Cube Maps/Skybox)
- Name built-ins: `Renderpass.Builtin.World`, `Renderpass.Builtin.UI`.
- Future (design-ready):
  - Add cube-map target support by allowing attachments with array layers (per-face); extend `VkrRenderTargetDesc` with optional `array_layer` and `mip_level` for each attachment.

## File Touch Points
- `lib/src/renderer/vkr_renderer.h`: Types, handles, backend interface, new frontend API.
- `lib/src/renderer/vulkan/vulkan_types.h`: Backend state additions, opaque structs.
- `lib/src/renderer/vulkan/vulkan_backend.c`: Interface export updates, registry, explicit begin/end, render target create/destroy, wrapper textures, resize callback.
- `lib/src/renderer/vulkan/vulkan_renderpass.c`: Creation from config; keep domain creators; add name-based creation.
- `lib/src/renderer/vulkan/vulkan_swapchain.c`: Create/destroy wrapper textures for swapchain images and depth; call into resize callback.
- Frontend (where applicable): default pass configs, target regeneration hook, explicit render path helpers.

## Acceptance Criteria
- World/UI built-in passes created and registered by name; retrievable via API.
- Frontend can build render targets per swapchain image using provided window/depth attachments.
- Explicit `begin_render_pass_ex/end_render_pass_ex` draws correctly; present path handles the transition when UI is last.
- Domain-based pipeline path remains functional and unbroken.
- On window resize, render targets are recreated and rendering continues.
- Shader config with `renderpass: "Renderpass.Builtin.UI"` causes pipeline creation against UI pass without code changes in draw loop.

## Small Example: .shadercfg snippet
```ini
name = "ui_textured"
renderpass = "Renderpass.Builtin.UI"
# ... other shader settings ...
```


