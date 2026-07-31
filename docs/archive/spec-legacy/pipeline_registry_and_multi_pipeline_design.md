---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Pipeline Registry and Multi-Pipeline Design Specification

## Overview

This document describes a registry for graphics pipelines organized by domain (e.g., WORLD, UI). The registry centralizes pipeline creation and selection, ensuring that materials route to an appropriate pipeline and that domain-specific states (depth/blend) are consistently applied.

Related: [Multi-Render Pass System](./multi_render_pass_system_design.md), [Material System](./material_system_design.md), [Render Flow](./render_flow_and_state_updates_design.md).

## Architecture

```
┌──────────────────────────────────────────────┐
│              VkrPipelineRegistry             │
│  - map: name → PipelineHandle                │
│  - domain routing (WORLD, UI, …)             │
│  - composition helpers (vertex input, etc.)  │
└───────────────┬──────────────────────────────┘
                │ creates
                ▼
         Graphics Pipelines (per domain)
           WORLD: depth test/write ON, blend OFF
           UI:    depth OFF, alpha blending ON
```

## API

```c
typedef enum VkrPipelineDomain { VKR_PIPELINE_DOMAIN_WORLD, VKR_PIPELINE_DOMAIN_UI, VKR_PIPELINE_DOMAIN_COUNT } VkrPipelineDomain;

typedef struct VkrGraphicsPipelineDescription {
    // shader modules, vertex input, descriptor set layouts, etc.
    VkrPipelineDomain domain; // required
} VkrGraphicsPipelineDescription;

typedef struct VkrPipelineRegistry { /* storage */ } VkrPipelineRegistry;

bool8_t vkr_pipeline_registry_initialize(VkrPipelineRegistry *registry, RendererFrontend *renderer);
void    vkr_pipeline_registry_shutdown(VkrPipelineRegistry *registry);

PipelineHandle vkr_pipeline_registry_create_graphics_pipeline(VkrPipelineRegistry *registry,
    const VkrGraphicsPipelineDescription *desc, String8 name);

PipelineHandle vkr_pipeline_registry_find_by_domain(VkrPipelineRegistry *registry, VkrPipelineDomain domain);
PipelineHandle vkr_pipeline_registry_find_by_name(VkrPipelineRegistry *registry, String8 name);
```

## Design Decisions

1) Domain required on pipeline descriptions
- Rationale: Ensures correct render pass and fixed-function states.
- Implementation: `VkrGraphicsPipelineDescription.domain` is mandatory.

2) Registry owns naming and lookup
- Rationale: Centralizes lifecycle; avoids ad-hoc creation in the app layer.
- Implementation: Create by name; materials may reference pipelines by domain.

3) Per-domain fixed states
- Rationale: WORLD vs UI commonly differ in depth/blend configs.
- Implementation: Apply defaults at creation; allow overrides in description when necessary.

## Usage Examples

```c
VkrGraphicsPipelineDescription world_desc = { .domain = VKR_PIPELINE_DOMAIN_WORLD /* + shaders, inputs... */ };
vkr_pipeline_registry_create_graphics_pipeline(&registry, &world_desc, string_lit("world"));

VkrGraphicsPipelineDescription ui_desc = { .domain = VKR_PIPELINE_DOMAIN_UI /* + shaders, inputs... */ };
vkr_pipeline_registry_create_graphics_pipeline(&registry, &ui_desc, string_lit("ui"));

// Route by material
PipelineHandle pipeline = vkr_pipeline_registry_find_by_domain(&registry, material->pipeline);
```

## Performance Considerations

- Cache default pipelines by domain to avoid repeated lookups.
- Keep pipeline composition helpers reusable to minimize duplication.

## Testing

- `test_pipeline_registry_create_find` – create and retrieve by name/domain.
- `test_pipeline_domain_states` – verify depth/blend configuration per domain.

## Revision History

- Version 1.0 (2025-10-11): Initial specification of domain-based pipeline registry and defaults.


