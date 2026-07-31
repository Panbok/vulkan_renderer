---
status: proposed
updated: 2026-07-31
authority: design
---
# Render Pass and Render Target System Improvements

**Legacy note:** This document references the deprecated view/layer system
(`view system (removed)`) which has been removed. Render orchestration now uses the
render graph; view modules act as render helpers invoked by pass executors.

## Document Purpose

This document reviews the current render pass + render target systems (as implemented today), calls out correctness/perf/maintainability issues, and proposes a refactor path that improves configurability for new rendering features (MSAA, multi-attachment, post chains, picking, shadows, offscreen rendering).

This update is aligned with the Vulkan backend behavior (view system references
below are historical):
- Render passes are started explicitly via `vkr_renderer_begin_render_pass()`
  (render graph, picking system).
- `VkrPipelineDomain` is primarily a **pipeline-state preset** and a default render-pass selector for pipeline creation.
- Render targets (framebuffers) are created per pass via
  `vkr_renderer_render_target_create()` (including swapchain-backed targets
  created by the render graph).

---

## Table of Contents

1. [Current Architecture Overview (As Implemented)](#1-current-architecture-overview-as-implemented)
2. [Critical Issues](#2-critical-issues)
3. [Limitations](#3-limitations)
4. [Goals](#4-goals)
5. [Proposed Improvements](#5-proposed-improvements)
6. [Implementation Phases](#6-implementation-phases)
7. [API Sketch](#7-api-sketch)

---

## 1. Current Architecture Overview (As Implemented)

### 1.1 Render Pass System

The current render pass system has two “axes” that are easy to conflate:

1) **Named render passes** (created via `vkr_renderer_renderpass_create_desc()`).
- Stored in a registry in the Vulkan backend (`render_pass_registry`).
- Used by pass executors (skybox/world/UI) and by feature systems (shadow/picking/offscreen).
- Attachment load/store/layouts are explicit per attachment; there is no `prev_name/next_name` coupling.

2) **Pipeline domains** (`VkrPipelineDomain`).
- Used to choose pipeline state defaults (depth/blend presets) in `lib/src/renderer/vulkan/vulkan_pipeline.c`.
- Used to select a default `VkRenderPass` at pipeline creation time when `VkrGraphicsPipelineDescription.renderpass == NULL`.

**Public API (`lib/src/renderer/vkr_renderer.h`):**
```c
typedef enum VkrPipelineDomain {
  VKR_PIPELINE_DOMAIN_WORLD = 0,
  VKR_PIPELINE_DOMAIN_UI = 1,
  VKR_PIPELINE_DOMAIN_SHADOW = 2,
  VKR_PIPELINE_DOMAIN_POST = 3,
  VKR_PIPELINE_DOMAIN_COMPUTE = 4,
  VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT = 5,
  VKR_PIPELINE_DOMAIN_SKYBOX = 6,
  VKR_PIPELINE_DOMAIN_PICKING = 7,
  VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT = 8,
  VKR_PIPELINE_DOMAIN_WORLD_OVERLAY = 9,
  VKR_PIPELINE_DOMAIN_PICKING_OVERLAY = 10,
  VKR_PIPELINE_DOMAIN_COUNT
} VkrPipelineDomain;

typedef enum VkrAttachmentLoadOp {
  VKR_ATTACHMENT_LOAD_OP_LOAD = 0,
  VKR_ATTACHMENT_LOAD_OP_CLEAR,
  VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
} VkrAttachmentLoadOp;

typedef enum VkrAttachmentStoreOp {
  VKR_ATTACHMENT_STORE_OP_STORE = 0,
  VKR_ATTACHMENT_STORE_OP_DONT_CARE,
} VkrAttachmentStoreOp;

typedef union VkrClearValue {
  struct {
    float32_t r, g, b, a;
  } color_f32;
  struct {
    uint32_t r, g, b, a;
  } color_u32;
  struct {
    float32_t depth;
    uint32_t stencil;
  } depth_stencil;
} VkrClearValue;

typedef struct VkrRenderPassAttachmentDesc {
  VkrTextureFormat format;
  VkrSampleCount samples;
  VkrAttachmentLoadOp load_op;
  VkrAttachmentStoreOp store_op;
  VkrAttachmentLoadOp stencil_load_op;
  VkrAttachmentStoreOp stencil_store_op;
  VkrTextureLayout initial_layout;
  VkrTextureLayout final_layout;
  VkrClearValue clear_value;
} VkrRenderPassAttachmentDesc;

typedef struct VkrResolveAttachmentRef {
  uint8_t src_attachment_index;
  uint8_t dst_attachment_index;
} VkrResolveAttachmentRef;

typedef struct VkrRenderPassDesc {
  String8 name;
  VkrPipelineDomain domain;
  uint8_t color_attachment_count;
  VkrRenderPassAttachmentDesc *color_attachments;
  VkrRenderPassAttachmentDesc *depth_stencil_attachment; // NULL if none
  uint8_t resolve_attachment_count;
  VkrResolveAttachmentRef *resolve_attachments;
} VkrRenderPassDesc;
```

**Domain → Vulkan render pass mapping (current Vulkan backend):**

This describes the *default* render pass created for each domain. Named passes
may differ because they provide explicit attachment layouts/clears.

| Domain | Attachments | Color Final Layout | Depth Final Layout | Notes |
|--------|-------------|--------------------|--------------------|-------|
| WORLD | Swapchain color + depth | `COLOR_ATTACHMENT_OPTIMAL` | `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` | Chained to UI by leaving swapchain color in attachment-optimal layout. |
| WORLD_TRANSPARENT | Same as WORLD | Same | Same | Domain is used for pipeline state (blend on, depth write off); render pass is WORLD (aliased in backend). |
| WORLD_OVERLAY | Same as WORLD | Same | Same | Domain is used for pipeline state (no depth); render pass is WORLD (aliased in backend). |
| SKYBOX | Swapchain color + depth | `COLOR_ATTACHMENT_OPTIMAL` | `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` | In current backend, SKYBOX uses the same attachment config as WORLD. |
| UI | Swapchain color | `PRESENT_SRC_KHR` | N/A | UI expects to composite on existing color (`LOAD`) in the default domain pass. |
| POST | Swapchain color | `PRESENT_SRC_KHR` | N/A | Typical fullscreen pass. |
| SHADOW | Depth only | N/A | `DEPTH_STENCIL_READ_ONLY_OPTIMAL` | Final layout supports sampling the shadow map. |
| PICKING | `R32_UINT` + depth | `TRANSFER_SRC_OPTIMAL` | `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` | Color ends in transfer layout for readback. |
| PICKING_TRANSPARENT | Same as PICKING | Same | Same | Domain is used for depth state (test on, write off). |
| PICKING_OVERLAY | Same as PICKING | Same | Same | Domain is used for “no depth” picking overlay. |
| COMPUTE | N/A | N/A | N/A | No render pass. |

### 1.2 Render Target System

Render targets are thin framebuffer wrappers:

**Internal (`lib/src/renderer/vulkan/vulkan_types.h`):**
```c
struct s_RenderTarget {
  VkFramebuffer handle;
  uint32_t width, height;
  uint32_t layer_count;
  bool8_t sync_to_window_size;
  uint8_t attachment_count;
  struct s_TextureHandle *attachments[VKR_RENDER_TARGET_MAX_ATTACHMENTS];
  VkImageView attachment_views[VKR_RENDER_TARGET_MAX_ATTACHMENTS];
  bool8_t attachment_view_owned[VKR_RENDER_TARGET_MAX_ATTACHMENTS];
};
```

**Public API (`lib/src/renderer/vkr_renderer.h`):**
```c
typedef struct VkrRenderTargetAttachmentRef {
  VkrTextureOpaqueHandle texture;
  uint32_t mip_level;
  uint32_t base_layer;
  uint32_t layer_count;
} VkrRenderTargetAttachmentRef;

typedef struct VkrRenderTargetDesc {
  bool8_t sync_to_window_size;
  uint32_t width;
  uint32_t height;
  uint8_t attachment_count;
  VkrRenderTargetAttachmentRef *attachments;
} VkrRenderTargetDesc;
```

**How render targets are used today:**
- The render graph creates *per-pass, per-swapchain-image* render targets by
  combining `vkr_renderer_window_attachment_get(image_index)` with
  `vkr_renderer_depth_attachment_get()` and calling
  `vkr_renderer_render_target_create()` (see
  `lib/src/renderer/vkr_rg_compile.c`).
- Feature systems (picking/shadows/offscreen) also create dedicated render targets, including mip/layer subresource targets.

### 1.3 Integration Pattern (Reality Check)

The current execution path is **explicit pass begin/end**, not “automatic domain switching when pipeline binds”.

```
Render Graph / Feature Systems
  - resolve renderpass handles (by name)
  - build per-image render targets (framebuffers)
  - call vkr_renderer_begin_render_pass(pass, target)
  - bind pipelines + draw
  - call vkr_renderer_end_render_pass()

Vulkan Backend
  - creates VkRenderPass objects (domain defaults + named passes)
  - creates VkFramebuffer objects via render_target_create()
  - records vkCmdBeginRenderPass/vkCmdEndRenderPass in begin/end calls
```

The backend no longer maintains per-domain framebuffers; render targets are
owned by the render graph/feature systems.

---

## 2. Critical Issues

### 2.1 Render Target Destruction: Resolved

Render targets now allocate from a reclaimable pool and are freed via the
deferred destruction queue. Owned subresource views and the framebuffer are
released after the GPU is finished, which prevents arena high-water growth
during resize/offscreen churn.

### 2.2 Domain Render Pass Override Is Explicit

Domain overrides now use `vkr_renderer_domain_renderpass_set()` with a
compatibility policy. Overrides invalidate framebuffer caches and update
aliased domains. Auto-assignment only occurs when a domain has no pass yet,
avoiding silent replacement of existing domain passes.

### 2.3 Layout Policy Is Explicit

`prev_name/next_name` has been removed from the render pass API. Layouts and
load/store behavior are now explicit per attachment in `VkrRenderPassDesc`,
and the view/feature systems control pass ordering directly.

### 2.4 Render Target Attachment Lifetime Tracking

Render targets still reference external texture handles, but they now capture
debug-generation values at creation and validate liveness at begin-pass time.
Subresource views created for attachments are tracked and destroyed by the
render target on teardown, and destruction is deferred to GPU-safe points.

### 2.5 Deferred Destruction Is In Place

The backend uses a deferred destruction queue keyed to submission serials and
frame fences. Some higher-level systems still call `vkr_renderer_wait_idle()`
for simplicity during resize/offscreen rebuilds, but core Vulkan resource
teardown no longer requires global waits.

---

## 3. Limitations

### 3.1 MSAA Requires Call-Site Adoption

MSAA is supported in render pass descriptors, pipeline multisample state, and
render target textures (including explicit resolves). Remaining limitation:
higher-level systems still assume 1x and do not build resolve attachments or
MSAA color targets by default.

### 3.2 Multi-Attachment Usage Is Sparse

The descriptor API supports multiple color attachments, but most built-in
passes still configure a single color attachment. MRT usage
needs explicit pass/target setup in systems that require it.

### 3.3 Layered Rendering Is Available but Underused

Render targets can address mip levels and array layers via
`VkrRenderTargetAttachmentRef`. Current systems mostly target layer 0;
cubemap/array workflows still need integration.

### 3.4 Clear Values Are Explicit

Clear values are specified per attachment via `VkrClearValue`. The remaining
limitation is that higher-level systems often use fixed clears instead of
exposing per-pass customization.

### 3.5 Layout Policy Still Requires Manual Coordination

`VkrTextureLayout` now includes PRESENT/TRANSFER/DEPTH_READ_ONLY, but layout
transitions across passes are still manual at the view/feature level (no pass
graph).

### 3.6 Load/Store Ops Are Explicit

Load/store operations are specified per attachment in `VkrRenderPassDesc`.
The remaining limitation is that higher-level systems still rely on fixed
defaults rather than exposing these knobs to gameplay/editor configuration.

---

## 4. Goals

### 4.1 Correctness and Debuggability
- Validate that render target attachments match render pass requirements (format, sample count, extent, layer count, usage/aspect).
- Make ordering/layout constraints explicit and verifiable.
- Avoid CPU-side UAF hazards from raw attachment pointers (at least in debug builds).

### 4.2 Configurability for New Features
- Multi-attachment render passes (MRT).
- MSAA with resolves.
- Array layers and cubemap rendering.
- More complete layout states (present, transfer, depth read-only).

### 4.3 Performance and Maintainability
- Replace broad `wait_idle` usage with deferred destruction.
- Reduce framebuffer churn by caching/reuse.
- Separate “pipeline state presets” (domains) from “render pass signature” and “pass scheduling”.

---

## 5. Proposed Improvements

### 5.1 Introduce a Render Pass Signature (Compatibility + Validation Anchor)

Add a lightweight “signature” that captures what must match for:
- framebuffer compatibility
- pipeline compatibility assumptions
- render target validation

Example:
```c
typedef struct VkrRenderPassSignature {
  uint8_t color_attachment_count;
  VkrTextureFormat color_formats[VKR_MAX_COLOR_ATTACHMENTS];
  VkrSampleCount color_samples[VKR_MAX_COLOR_ATTACHMENTS];
  bool8_t has_depth_stencil;
  VkrTextureFormat depth_stencil_format;
  VkrSampleCount depth_stencil_samples;
} VkrRenderPassSignature;
```

Store this signature alongside the backend render pass object, populated at creation time (both domain and named passes).

This enables:
- checking render target attachments *before* creating a framebuffer,
- deriving pipeline multisample state from the pass being targeted,
- enforcing “domain pass override must be signature-compatible” (or else trigger pipeline rebuild).

### 5.2 Replace `prev_name/next_name` With Explicit Attachment State

Legacy `VkrRenderPassConfig` has been removed. Render pass creation uses
explicit attachment descriptors only:
- layouts + load/store are per-attachment,
- resolve attachments are modeled explicitly,
- pass ordering stays a view/system concern.

Current types:
```c
typedef enum VkrAttachmentLoadOp {
  VKR_ATTACHMENT_LOAD_OP_LOAD = 0,
  VKR_ATTACHMENT_LOAD_OP_CLEAR,
  VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
} VkrAttachmentLoadOp;

typedef enum VkrAttachmentStoreOp {
  VKR_ATTACHMENT_STORE_OP_STORE = 0,
  VKR_ATTACHMENT_STORE_OP_DONT_CARE,
} VkrAttachmentStoreOp;

typedef union VkrClearValue {
  struct {
    float32_t r, g, b, a;
  } color_f32;
  struct {
    uint32_t r, g, b, a;
  } color_u32;
  struct {
    float32_t depth;
    uint32_t stencil;
  } depth_stencil;
} VkrClearValue;

typedef struct VkrRenderPassAttachmentDesc {
  VkrTextureFormat format;
  VkrSampleCount samples;
  VkrAttachmentLoadOp load_op;
  VkrAttachmentStoreOp store_op;
  VkrAttachmentLoadOp stencil_load_op;
  VkrAttachmentStoreOp stencil_store_op;
  VkrTextureLayout initial_layout;
  VkrTextureLayout final_layout;
  VkrClearValue clear_value;
} VkrRenderPassAttachmentDesc;

typedef struct VkrResolveAttachmentRef {
  uint8_t src_attachment_index;
  uint8_t dst_attachment_index;
} VkrResolveAttachmentRef;

typedef struct VkrRenderPassDesc {
  String8 name;
  VkrPipelineDomain domain; // still useful for pipeline state presets
  uint8_t color_attachment_count;
  VkrRenderPassAttachmentDesc *color_attachments;
  VkrRenderPassAttachmentDesc *depth_stencil_attachment; // NULL if none
  uint8_t resolve_attachment_count;
  VkrResolveAttachmentRef *resolve_attachments;
} VkrRenderPassDesc;
```

Notes:
- This intentionally treats “what is this pass” separately from “when does it run”.
- It also makes integer clears (picking) a first-class concept instead of a format-based special case.

### 5.3 Add a Render Target Attachment Reference (Mip + Layer Addressing)

Render targets can bind subresources:

```c
typedef struct VkrRenderTargetAttachmentRef {
  VkrTextureOpaqueHandle texture;
  uint32_t mip_level;
  uint32_t base_layer;
  uint32_t layer_count;
} VkrRenderTargetAttachmentRef;

typedef struct VkrRenderTargetDesc {
  bool8_t sync_to_window_size;
  uint32_t width;
  uint32_t height;
  uint8_t attachment_count;
  VkrRenderTargetAttachmentRef *attachments;
} VkrRenderTargetDesc;
```

This unlocks:
- cubemaps (layer_count=1 with base_layer per face, or layer_count=6),
- cascaded shadows in a single array texture,
- layered rendering features without proliferating “one texture per layer”.

### 5.4 Framebuffer Cache (Deferred)

Render targets currently own their framebuffers directly (no shared cache).
A cache can be reintroduced later once a stable compatibility key and lifetime
model are in place.

### 5.5 Deferred Destruction (Use Existing `submit_serial`)

Introduce a small deferred destruction system in Vulkan backend:

- Every end-frame increments `state->submit_serial`.
- Destruction queues store `(submit_serial, kind, payload)`.
- At begin-frame (after waiting for the current frame fence), retire entries whose serial is known-safe.

Kinds should cover at minimum:
- `VkFramebuffer`
- `VkRenderPass`
- `VkImage` + `VkImageView`
- `VkSampler`
- wrapper structs allocated from pools

This replaces widespread `wait_idle` calls and keeps resize/reload interactive.

### 5.6 Validation Requires Texture Metadata

Render target validation now relies on Vulkan image metadata:
- `VkSampleCountFlagBits samples`
- `array_layers`, `mip_levels`

Usage flag validation is still limited to the render-target texture creation
path; general textures do not yet retain full usage metadata.

### 5.7 MSAA Model (Implemented)

MSAA support should be modeled as:
- an MSAA color attachment (samples > 1),
- a resolve attachment (samples = 1),
- and optionally an MSAA depth attachment (samples > 1).

Pipeline creation must set:
- `VkPipelineMultisampleStateCreateInfo.rasterizationSamples = samples`.

Render target creation must support:
- MSAA images for the MSAA attachments,
- single-sample images for resolves (swapchain image often serves as the resolve target).

### 5.8 Dynamic Rendering vs Subpasses (Pick One First)

Both are valid, but implementing both tends to duplicate effort.

If the near-term goal is “more configurability” (MRT, MSAA, varied layouts) with less boilerplate:
- Prefer **VK_KHR_dynamic_rendering** first.
- Keep subpass support as a later optimization for tile-based GPUs and input attachments.

---

## 6. Implementation Phases

### Phase 1: Correctness + Lifetime ✅ COMPLETED
- ✅ Add `VkrRenderPassSignature` generation + store on render pass creation.
  - Added `VkrRenderPassSignature` struct to `vkr_renderer.h`
  - Added `signature` field to `VulkanRenderPass` in `vulkan_types.h`
  - Populated signature in `vulkan_renderpass_create_from_desc()` and domain creation
  - Added `vkr_renderer_renderpass_get_signature()` API
- ✅ Make render target wrapper allocation reclaimable (pool/dmemory), not arena.
  - Added `render_target_pool` and `render_target_alloc` to `VulkanBackendState`
  - Render targets now allocated from pool, properly freed on destroy
- ✅ Add debug-only validation that render target attachments are still live when beginning a pass.
  - Added `generation` counter to texture handles
  - Render targets capture attachment generations at creation
  - `renderer_vulkan_begin_render_pass()` validates generations in debug builds
- ✅ Make domain pass override an explicit API; enforce "only during reload" policy.
  - Added `vkr_renderer_domain_renderpass_set()` with `VkrDomainOverridePolicy`
  - Supports `REQUIRE_COMPATIBLE` and `FORCE` policies
  - Invalidates framebuffer cache and updates aliased domains (WORLD_TRANSPARENT, WORLD_OVERLAY)

**Note on Framebuffer Cache:** The cache was removed; each render target owns its
framebuffer. A cache can be reintroduced later once compatibility keys and
lifetime rules are solid.

### Phase 2: New Configurable APIs ✅ COMPLETED
- ✅ Add `VkrRenderPassDesc` + `vkr_renderer_renderpass_create_desc()`.
  - Added `VkrAttachmentLoadOp`, `VkrAttachmentStoreOp`, `VkrClearValue` enums/unions
  - Added `VkrRenderPassAttachmentDesc` with full load/store/layout control
  - Added `VkrResolveAttachmentRef` for MSAA resolve configuration
  - Added `VkrRenderPassDesc` supporting up to 8 color attachments + depth + resolves
  - Implemented `vulkan_renderpass_create_from_desc()` with full Vulkan mapping
- ✅ Add `VkrRenderTargetDesc` + `vkr_renderer_render_target_create()`.
  - Added `VkrRenderTargetAttachmentRef` with mip_level, base_layer, layer_count
  - Added `VkrRenderTargetDesc` for advanced render target creation
  - Implemented subresource view creation for mip/layer addressing
  - Supports variable layer counts for cubemap/array rendering
- ✅ Expand `VkrTextureLayout` to include missing states.
  - Added `VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL`
  - Added `VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL`
  - Added `VKR_TEXTURE_LAYOUT_TRANSFER_DST_OPTIMAL`
  - Added `VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR`
  - Added conversion function `vkr_texture_layout_to_vk()`

### Phase 3: Deferred Destruction ✅ COMPLETED
- ✅ Implement backend deferred destroy queue keyed to `submit_serial`.
  - Added `VkrDeferredDestroyKind` enum (FRAMEBUFFER, RENDERPASS, IMAGE, IMAGE_VIEW, SAMPLER, BUFFER, wrappers)
  - Added `VkrDeferredDestroyEntry` and `VkrDeferredDestroyQueue` (256 entries)
  - Added `deferred_destroy_queue` to `VulkanBackendState`
  - Implemented `vulkan_deferred_destroy_enqueue()`, `vulkan_deferred_destroy_process()`, `vulkan_deferred_destroy_flush()`
  - Processing at begin_frame after fence wait; flush at shutdown

### Phase 4: MSAA ✅ COMPLETED
- ✅ Add per-attachment sample counts + resolve modeling.
  - Added `VkrSampleCount` enum (1, 2, 4, 8, 16, 32, 64)
  - Added `sample_count` to `VkrTextureDescription`
  - Added `samples` field to `VulkanImage`
  - `VkrRenderPassDesc` supports resolve attachments via `VkrResolveAttachmentRef`
- ✅ Update pipeline multisample state from render pass signature.
  - Modified `vulkan_graphics_graphics_pipeline_create()` to derive `rasterizationSamples` from render pass signature
  - Handles both color attachment samples and depth-only pass samples
- ✅ Update render target texture creation to support MSAA images.
  - Added `samples` parameter to `vulkan_image_create()`
  - Updated all callers to pass sample count
  - Added `vkr_renderer_create_render_target_texture_msaa()` API

### Phase 5: Array Layers + Cubemaps (Medium Priority)
- ✅ Partially completed via `VkrRenderTargetDesc` and `VkrRenderTargetAttachmentRef`
- Subresource view creation for mip/layer addressing implemented
- TODO: Full cubemap rendering workflow validation

### Phase 6: Dynamic Rendering or Subpasses (Low Priority, Strategic)
- Not yet started
- If choosing dynamic rendering:
  - add begin/end rendering APIs and pipeline rendering info.
- If choosing subpasses:
  - add subpass descriptors + dependency descriptors + input attachments.

---

## 7. API Sketch

This section summarizes the current pass/target API surface.

### 7.1 New Types
```c
typedef enum VkrSampleCount {
  VKR_SAMPLE_COUNT_1 = 1,
  VKR_SAMPLE_COUNT_2 = 2,
  VKR_SAMPLE_COUNT_4 = 4,
  VKR_SAMPLE_COUNT_8 = 8,
  VKR_SAMPLE_COUNT_16 = 16,
} VkrSampleCount;

typedef struct VkrRenderPassSignature VkrRenderPassSignature;
typedef struct VkrRenderPassDesc VkrRenderPassDesc;
typedef struct VkrRenderTargetAttachmentRef VkrRenderTargetAttachmentRef;
typedef struct VkrRenderTargetDesc VkrRenderTargetDesc;
```

### 7.2 New Functions
```c
bool8_t vkr_renderer_renderpass_get_signature(
    VkrRendererFrontendHandle renderer,
    VkrRenderPassHandle pass,
    VkrRenderPassSignature *out_signature);

VkrRenderPassHandle vkr_renderer_renderpass_create_desc(
    VkrRendererFrontendHandle renderer,
    const VkrRenderPassDesc *desc,
    VkrRendererError *out_error);

VkrRenderTargetHandle vkr_renderer_render_target_create(
    VkrRendererFrontendHandle renderer,
    const VkrRenderTargetDesc *desc,
    VkrRenderPassHandle pass,
    VkrRendererError *out_error);

// Optional: explicit domain override instead of name-based override.
bool8_t vkr_renderer_domain_renderpass_set(
    VkrRendererFrontendHandle renderer,
    VkrPipelineDomain domain,
    VkrRenderPassHandle pass,
    VkrDomainOverridePolicy policy,
    VkrRendererError *out_error);
```

### 7.3 Backwards Compatibility
- Legacy `VkrRenderPassConfig` and `vkr_renderer_renderpass_create()` have been
  removed.
- `VkrRenderTargetDesc` and `vkr_renderer_render_target_create()` are the
  primary render target API (explicit mip/layer refs).

---

## Related Documents

- `docs/rendering/renderer_frontend_refactoring.md` (overlapping concerns: resource lifetime, reload paths, handle systems).

---

*Document Version: 1.2*
*Updated: 2026-01-19*
*Status: Proposal - Pending Review*
