---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Multi-Render Pass System Design Specification

## Overview

This document specifies a domain-based multi-render pass system in the Vulkan backend. Render passes are created per pipeline domain (WORLD, UI, etc.) and are switched automatically based on the currently bound pipeline. The approach enables correct on-screen composition (world first, UI layered after) and prepares for off-screen passes (shadow, post-process).

Related: [Pipeline Registry](./pipeline_registry_and_multi_pipeline_design.md), [Render Flow](./render_flow_and_state_updates_design.md).

## Architecture

```
VulkanBackendState
  domain_render_passes[VKR_PIPELINE_DOMAIN_COUNT]
  domain_framebuffers[VKR_PIPELINE_DOMAIN_COUNT][BUFFERING_FRAMES]
  current_render_pass_domain
  render_pass_active

Flow per frame:
  begin_frame()              // no pass started
  bind world pipeline  → ensure WORLD pass begun
  render 3D
  bind UI pipeline     → end WORLD, begin UI pass (LOAD color)
  render UI
  end_frame()                // end active pass if any
```

## API

Backend-side (shape):

```c
bool8_t vulkan_create_domain_render_passes(VulkanBackendState *state);
bool8_t vulkan_create_domain_framebuffers(VulkanBackendState *state);

// Called at pipeline bind/update time
static void vulkan_ensure_render_pass_for_domain(VulkanBackendState *state, VkrPipelineDomain domain);

// Swapchain lifecycle
bool8_t vulkan_on_resize_recreate_domain_passes_and_framebuffers(VulkanBackendState *state);
```

Pass configurations:
- WORLD: color+depth, color.finalLayout = COLOR_ATTACHMENT_OPTIMAL, clear color+depth.
- UI (on-screen): color-only, initialLayout = COLOR_ATTACHMENT_OPTIMAL, loadOp=LOAD, finalLayout=PRESENT_SRC_KHR.
- UI (clear variant for UI-only frames): initialLayout=UNDEFINED, loadOp=CLEAR, finalLayout=PRESENT_SRC_KHR.

## Design Decisions

1) Domain-driven automatic pass switching
- Rationale: Keeps app code simple; ensures correct ordering and attachment policies.
- Implementation: Switch in `pipeline_update_state` when domain changes.

2) Separate framebuffers per domain
- Rationale: Framebuffers must be compatible with the render pass they are used with.
- Implementation: Build N framebuffers per domain for N swapchain images.

3) Preserve world contents for UI pass
- Rationale: UI composes over world output.
- Implementation: UI pass color loadOp=LOAD; world color finalLayout=COLOR_ATTACHMENT_OPTIMAL.

4) Resize/shutdown coverage
- Rationale: Multiple render passes require full lifecycle handling.
- Implementation: Recreate/destroy all domain render passes and framebuffers on swapchain events.

5) Domain information flow
- Rationale: Pass selection depends on pipeline domain.
- Implementation: Ensure pipeline creation and update paths carry `VkrPipelineDomain` from registry to backend.

## Usage Examples

```c
// App does not start or end passes explicitly
vkr_renderer_begin_frame(renderer, delta);

bind(world_pipeline); // world domain → begins WORLD pass
draw_world();

bind(ui_pipeline);    // UI domain → ends WORLD, begins UI (LOAD) pass
draw_ui();

vkr_renderer_end_frame(renderer, delta); // ends active pass
```

## Performance Considerations

- Minimize domain switches; sort draws by domain where possible.
- Re-emit dynamic state (viewport/scissor) after pass switches to be robust.

## Testing

- `test_world_then_ui_composition` – world content preserved under UI.
- `test_resize_recreates_all_domain_fb` – all domain framebuffers valid after resize.
- `test_no_draws_no_pass_started` – valid frame submission with no passes.

## Revision History

- Version 1.0 (2025-10-11): Initial specification of domain-specific passes and automatic switching.


