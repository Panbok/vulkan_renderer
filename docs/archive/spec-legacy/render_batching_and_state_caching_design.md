---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Render Batching and State Caching Design Specification

## Overview

This document defines strategies to reduce driver and CPU overhead by caching bound state (pipeline and descriptor sets) and by batching draw calls through sorting keys (pipeline, material, geometry layout). The goal is to minimize redundant `vkCmdBindPipeline`/descriptor binds and improve cache locality.

Related: [Render Flow](./render_flow_and_state_updates_design.md), [Material System](./material_system_design.md).

## Architecture

```
RendererStateCache
  current_pipeline
  bound_descriptor_sets[set] per frame

Batching
  sort_key = (domain, pipeline_id, material_id, geometry_layout)
  renderables sorted → reduce state changes
```

## API

Internal caching hooks (shape):

```c
void renderer_state_cache_reset(RendererStateCache *cache);
bool renderer_bind_pipeline_cached(RendererStateCache *cache, PipelineHandle pipeline);
bool renderer_bind_descriptor_sets_cached(RendererStateCache *cache, /* args */);
```

Batching utilities:

```c
typedef struct RenderSortKey { uint16_t domain, pipeline, material, layout; } RenderSortKey;
int render_sort_key_compare(const void *a, const void *b);
```

## Design Decisions

1) Cache current pipeline
- Rationale: Avoid redundant `vkCmdBindPipeline` for identical pipeline across draws.
- Implementation: Track `current_pipeline` and early-out on matches.

2) Cache descriptor set bindings
- Rationale: Descriptor rebinds are expensive and often unchanged across draws with same material.
- Implementation: Store last bound descriptor sets and generations; skip writes/binds when unchanged.

3) Sort renderables by key
- Rationale: Cluster draws to minimize state changes.
- Implementation: Construct `RenderSortKey` from domain/pipeline/material/layout and sort each frame.

## Usage Examples

```c
// Build sort keys
for (i: renderables) keys[i] = make_key(renderables[i]);
qsort(keys, count, sizeof(keys[0]), render_sort_key_compare);

// Draw in sorted order with caching
renderer_state_cache_reset(&cache);
for (k in keys) {
    renderer_bind_pipeline_cached(&cache, k.pipeline);
    // apply material if changed (generation check)
    // update local state and draw geometry
}
```

## Performance Considerations

- Expect significant reduction in pipeline/descriptor binds.
- Sorting adds O(N log N) cost; offset by reduced driver overhead for N ≫ 1.

## Testing

- `test_pipeline_bind_cached` – repeated binds coalesce to one.
- `test_descriptor_bind_cached` – unchanged material skips descriptor rebind.
- `test_sorting_reduces_switches` – validate fewer state changes after sort.

## Revision History

- Version 1.0 (2025-10-11): Initial specification for state caching and batching.


