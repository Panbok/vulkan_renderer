---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Resource Configuration and Hot-Reload Design Specification

## Overview

This document specifies a configuration system for resource paths and loader policies, and a hot-reload mechanism that watches for asset changes and safely reloads them, bumping generations to propagate updates through the renderer.

Related: [Resource System & Loaders](./resource_system_and_loaders_design.md), [Material System](./material_system_design.md), [Render Flow](./render_flow_and_state_updates_design.md).

## Architecture

```
VkrResourceConfig
  ├─ search_paths: ["assets/", "assets/materials/", ...]
  ├─ defaults: fallback material/texture names
  └─ loader options (e.g., texture decode flags)

VkrHotReload
  ├─ file watcher (platform abstraction)
  ├─ change queue (debounced)
  └─ apply: parse → finalize → generation++ (per type)
```

## API

```c
typedef struct VkrResourceConfig {
    // arrays of search paths, defaults, loader knobs
} VkrResourceConfig;

bool8_t vkr_resource_config_load_from_file(VkrResourceConfig *cfg, String8 path);
bool8_t vkr_resource_config_apply(const VkrResourceConfig *cfg, VkrResourceSystem *rsys);

typedef struct VkrHotReload {
    // watcher state, queues
} VkrHotReload;

bool8_t vkr_hotreload_initialize(VkrHotReload *hr, const VkrResourceConfig *cfg);
void    vkr_hotreload_shutdown(VkrHotReload *hr);
bool8_t vkr_hotreload_poll(VkrHotReload *hr, VkrResourceSystem *rsys);
```

Config file (example):

```
[paths]
search=assets/
search=assets/materials/
search=assets/textures/

[defaults]
material=assets/default.mt
texture=assets/paving.png

[texture]
keep_cpu_pixels=false
```

## Design Decisions

1) Search path resolution
- Rationale: Allow short names in content; robust asset discovery.
- Implementation: Probe paths in order; store canonical name in registry.

2) Generation bump on reload
- Rationale: Ensure descriptor updates and cache invalidation flow naturally.
- Implementation: When reloading, keep handle id; increment generation and update data.

3) Debounced file watching
- Rationale: Editors produce multiple rapid events per save.
- Implementation: Coalesce changes within a small time window.

4) Frame-safe apply
- Rationale: Avoid tearing or stalls.
- Implementation: Queue changes; apply at frame boundaries with deferred destruction.

5) Optional asset copy to build dir
- Rationale: Keep runtime assets colocated with binary.
- Implementation: Configurable copy step on build or init.

## Usage Examples

```c
VkrResourceConfig cfg = {0};
vkr_resource_config_load_from_file(&cfg, string_lit("assets/resources.cfg"));
vkr_resource_config_apply(&cfg, &app->resource_system);

VkrHotReload hr = {0};
vkr_hotreload_initialize(&hr, &cfg);

// Per frame
vkr_hotreload_poll(&hr, &app->resource_system); // applies pending changes
```

## Performance Considerations

- Watcher overhead should be minimal compared to render workload; poll at ~10–30 Hz.
- Apply reloads in batches to reduce descriptor churn.

## Testing

- `test_config_parse_paths` – parse multiple search paths.
- `test_hotreload_texture_change` – modify image on disk; texture data updated; generation++.
- `test_hotreload_material_change` – modify `.mt`; color/texture update applied.

## Revision History

- Version 1.0 (2025-10-11): Initial specification for resource config and hot-reload.


