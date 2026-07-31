---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Resource System and Loaders Design Specification

## Overview

This document defines a generic resource system that manages asset lifetime via name-based acquisition and ref-counted handles, delegating actual loading to pluggable loaders. The system deduplicates requests, validates handles by generation, and centralizes asset ownership and unload policies.

Related: [Material System](./material_system_design.md), [Render Flow](./render_flow_and_state_updates_design.md), [Loader Extensions](./loader_extensions_design.md) (optional).

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         VkrResourceSystem                        │
│  - Registry: name → {type, handle, ref_count, auto_release}     │
│  - Loaders: vtable array                                        │
│  - Acquire/Release entrypoints                                  │
└───────────────┬─────────────────────────────────────────────────┘
                │ delegates to loader by type
                ▼
┌─────────────────────────────────┐     ┌──────────────────────────┐
│    Material Loader (.mt)        │     │  Texture Loader (.png/…) │
│  - parse → create material      │     │  - decode → upload       │
└─────────────────────────────────┘     └──────────────────────────┘
```

## API

```c
typedef enum VkrResourceType {
    VKR_RESOURCE_TYPE_UNKNOWN = 0,
    VKR_RESOURCE_TYPE_TEXTURE,
    VKR_RESOURCE_TYPE_MATERIAL,
    // future: geometry, shader, etc.
} VkrResourceType;

typedef struct VkrResourceRecord {
    VkrResourceType type;
    String8 name;
    uint32_t ref_count;
    uint32_t loader_id;
    bool8_t auto_release;
    // handle is type-specific; store as union or tagged payload
} VkrResourceRecord;

typedef struct VkrResourceLoaderVTable {
    bool8_t (*can_load)(String8 path);
    bool8_t (*load)(Arena *scratch, String8 path, void *out_typed_desc);
    bool8_t (*finalize)(void *renderer, const void *typed_desc, void *out_handle);
    void    (*unload)(void *handle);
} VkrResourceLoaderVTable;

typedef struct VkrResourceSystem {
    // registry storage, loader tables, arenas
} VkrResourceSystem;

bool8_t vkr_resource_system_initialize(VkrResourceSystem *rsys);
void    vkr_resource_system_shutdown(VkrResourceSystem *rsys);

// Loader registration
uint32_t vkr_resource_system_register_loader(VkrResourceSystem *rsys, VkrResourceType type,
                                             VkrResourceLoaderVTable vtable);

// Acquire/Release by name
bool8_t vkr_resource_system_acquire(VkrResourceSystem *rsys, String8 name,
                                    VkrResourceType type, void *out_handle);
void    vkr_resource_system_release(VkrResourceSystem *rsys, VkrResourceType type, void *handle);
```

### Loader Responsibilities

- `can_load(path)`: quick filter on extension/content.
- `load(scratch, path, out_desc)`: parse/decode into an intermediate description.
- `finalize(renderer, desc, out_handle)`: create GPU/renderer resources, fill handle; free scratch.
- `unload(handle)`: renderer-side destruction.

## Design Decisions

1) Vtable-based loader indirection
- Rationale: Extend to new asset types without changing the resource system.
- Implementation: Register loaders at init; dispatch by `VkrResourceType` and `can_load`.

2) Name-based deduplication
- Rationale: Avoid duplicate loads and memory churn.
- Implementation: Registry maps names to records; increments `ref_count` on subsequent acquires.

3) Type-tagged handles with generation checks
- Rationale: Prevent cross-type misuse and stale handle usage.
- Implementation: Each system validates `{id,generation}` and type on lookup.

4) Auto-release policy
- Rationale: Simplifies one-shot resources (e.g., UI icons) during frame transitions.
- Implementation: Records can be flagged for release at frame end or when `ref_count` reaches 0.

5) Scratch allocation during load
- Rationale: Minimize persistent CPU memory; free after finalize.
- Implementation: Parsing/decoding uses transient arenas; only GPU objects and minimal metadata persist.

## Usage Examples

```c
// Register loaders
uint32_t tex_loader = vkr_resource_system_register_loader(rsys, VKR_RESOURCE_TYPE_TEXTURE, texture_loader_vtable());
uint32_t mat_loader = vkr_resource_system_register_loader(rsys, VKR_RESOURCE_TYPE_MATERIAL, material_loader_vtable());

// Acquire resources by name
VkrTextureHandle tex; vkr_resource_system_acquire(rsys, string_lit("assets/paving.png"), VKR_RESOURCE_TYPE_TEXTURE, &tex);
VkrMaterialHandle mat; vkr_resource_system_acquire(rsys, string_lit("assets/default.mt"), VKR_RESOURCE_TYPE_MATERIAL, &mat);

// Release when no longer needed
vkr_resource_system_release(rsys, VKR_RESOURCE_TYPE_MATERIAL, &mat);
vkr_resource_system_release(rsys, VKR_RESOURCE_TYPE_TEXTURE, &tex);
```

## Performance Considerations

- Avoid per-frame name lookups; cache resolved pointers in scene objects.
- Keep loader `load()` pure and CPU-bound; `finalize()` performs GPU work.
- Batch destruction via deferred queues to avoid stalls.

## Testing

- `test_resource_acquire_dedup` – same name returns same handle, refcounts increment.
- `test_resource_release_destroy` – destroy when refcount reaches 0.
- `test_loader_dispatch_by_ext` – correct loader selected by extension.
- `test_material_loader_parses_mt` – `.mt` → material with expected defaults.

## Revision History

- Version 1.0 (2025-10-11): Initial specification covering registry, loaders, and acquisition lifecycle.


