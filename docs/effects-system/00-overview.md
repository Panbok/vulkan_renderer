---
status: proposed
updated: 2026-07-31
authority: design
---
# Effects System - Implementation Overview

## Purpose

This document set describes the design and implementation plan for a GPU-based effects system that applies compute shader effects to meshes/entities. The primary demo is a wave deformation effect on Sponza's fabric curtains.

## Current State Analysis

### What Exists
- **Pipeline Registry**: Manages graphics pipelines with domain-based render pass switching
- **Mesh Manager**: Handles meshes with multiple submeshes, each with geometry/material/pipeline handles
- **Scene System**: ECS-based entity management with transform hierarchy, visibility, render IDs
- **Render Graph**: JSON-authored passes with named executors and declared
  resource dependencies

### What's Missing
1. **Compute Pipeline Support**: Compute domains/pass kinds exist in contracts,
   but the shipped frame path does not exercise a real compute dispatch
2. **Entity Tagging System**: No mechanism to tag entities/meshes for effect selection
3. **Effects System**: No infrastructure for registering, applying, or managing GPU effects
4. **Vertex Buffer Modification**: Buffers are immutable after creation; no update path

## Implementation Roadmap

```
Phase 1: Tagging System (01-tagging-system.md)
    │
    ├─► Add SceneTag component to ECS
    ├─► Extend VkrSubMesh with effect_tags bitmask
    ├─► Scene JSON format extension for tags
    └─► Tag query/filter API

Phase 2: Compute Pipeline Support (02-compute-pipeline-support.md)
    │
    ├─► VkrComputePipelineDescription type
    ├─► Compute shader loading in shader system
    ├─► VkComputePipeline creation in Vulkan backend
    ├─► Dispatch command infrastructure
    └─► Storage buffer support for read-write

Phase 3: Effects System Core (03-effects-system-design.md)
    │
    ├─► VkrEffectSystem with effect registry
    ├─► VkrEffect struct (compute pipeline + parameters)
    ├─► Effect binding to entities via tags
    ├─► Effect execution before/after world render
    └─► Per-effect parameter uniforms

Phase 4: Wave Effect Demo (04-wave-effect-demo.md)
    │
    ├─► wave.slang compute shader
    ├─► Fabric tag definition and scene tagging
    ├─► Wave effect parameter struct (amplitude, frequency, phase)
    └─► Time-based animation integration

Phase 5: Scene Integration (05-scene-integration.md)
    │
    ├─► Scene JSON schema for effects
    ├─► Effect loading during scene load
    ├─► Effect lifecycle tied to scene lifecycle
    └─► Editor UI considerations

Phase 6: Implementation Checklist (06-implementation-checklist.md)
    │
    └─► Step-by-step tasks with file references
```

## Architecture Decisions

### Vertex Modification Strategy

**Option A: Compute Shader with Storage Buffers** (Recommended)
- Original vertex buffer remains immutable
- Compute shader writes to separate "deformed" storage buffer
- Graphics pipeline reads from deformed buffer
- Supports temporal effects (velocity, history)

**Option B: Double-Buffered Vertex Buffers**
- Swap between two vertex buffers per frame
- Compute writes to back buffer, graphics reads front
- Higher memory usage but cleaner synchronization

**Option C: Vertex Shader Displacement**
- Pass effect parameters via uniforms
- Compute displacement in vertex shader
- Simpler but limited to per-vertex effects without history

**Decision: Option A** - Provides flexibility for complex effects while maintaining original geometry.

### Effect Selection via Tags

Tags are 64-bit bitmasks supporting up to 64 distinct tags per entity/submesh:
- Bits 0-15: Built-in tags (fabric, water, foliage, glass, etc.)
- Bits 16-47: User-defined tags
- Bits 48-63: Reserved for future use

Effects declare which tags they affect. During render, entities matching the tag mask have the effect applied.

### Memory Management

Following `AGENTS.md` and the `vkr-memory` skill:
- Effect definitions: Arena-backed (long-lived)
- Per-entity effect state: DMemory (freeable on entity removal)
- Per-frame effect data: Scratch allocator
- Compute buffers: Vulkan device memory with host-visible staging

## File Structure

New files to create:
```
lib/src/renderer/systems/
├── vkr_tag_system.h          # Tag component and query API
├── vkr_tag_system.c
├── vkr_effect_system.h       # Effect registry and application
├── vkr_effect_system.c
└── effects/
    ├── vkr_effect_wave.h     # Wave effect definition
    └── vkr_effect_wave.c

lib/src/renderer/vulkan/
├── vulkan_compute.h          # Compute pipeline types
└── vulkan_compute.c          # Compute dispatch implementation

assets/shaders/
└── effects/
    └── wave.slang            # Wave compute shader
```

Files to modify:
```
lib/src/renderer/
├── vkr_renderer.h            # Add compute types, effect handle
├── renderer_frontend.c/h     # Add compute dispatch API
├── systems/
│   ├── vkr_scene_system.c/h  # Add SceneTag component
│   ├── vkr_mesh_manager.c/h  # Add effect_tags to VkrSubMesh
│   └── vkr_pipeline_registry.c/h # Compute pipeline support
└── vulkan/
    ├── vulkan_types.h        # Add s_ComputePipeline
    └── vulkan_backend.c      # Compute pipeline creation
```

## Dependencies Between Documents

```
01-tagging-system.md
    └──► prerequisite for 03-effects-system-design.md

02-compute-pipeline-support.md
    └──► prerequisite for 03-effects-system-design.md
    └──► prerequisite for 04-wave-effect-demo.md

03-effects-system-design.md
    └──► prerequisite for 04-wave-effect-demo.md
    └──► prerequisite for 05-scene-integration.md

04-wave-effect-demo.md
    └──► uses 05-scene-integration.md for scene format
```

## LLM Implementation Notes

When implementing from these documents:
1. Follow document order (01 → 02 → 03 → 04 → 05)
2. Each document is self-contained with clear inputs/outputs
3. Code snippets show exact signatures and structures
4. File paths are absolute from project root
5. Test points are indicated at each phase
6. Memory and lifetime rules from `AGENTS.md` and `vkr-memory` apply throughout

## Success Criteria

Phase completion checklist:
- [ ] Phase 1: Tags queryable via `vkr_tag_system_query_entities()`
- [ ] Phase 2: Compute shader runs and produces visible output
- [ ] Phase 3: Effect applies to tagged entities during render
- [ ] Phase 4: Sponza fabric visibly waves with time
- [ ] Phase 5: Effect persists across scene save/load

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| GPU synchronization bugs | Use memory barriers explicitly; validate with sync validation layer |
| Performance regression | Profile before/after; batch effect dispatches |
| Memory leaks on scene reload | Follow arena/DMemory patterns; test repeated scene loads |
| Shader compilation failures | Test incrementally; use Slang's error reporting |
