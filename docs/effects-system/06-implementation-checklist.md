---
status: proposed
updated: 2026-09-04
authority: design
---
# Phase 6: Implementation Checklist

## Overview

This document provides a step-by-step implementation checklist with file references, dependencies, and verification points. Use this as a guide when implementing the effects system.

## Implementation Order

```
1. Tagging System (Foundation)
   ├── vkr_tag_system.h/c
   ├── SceneTag component in vkr_scene_system
   └── Scene JSON tag parsing

2. Compute Pipeline Support (Infrastructure)
   ├── Vulkan compute types
   ├── Compute pipeline creation
   ├── Storage buffer support
   └── Dispatch infrastructure

3. Effects System Core (Main System)
   ├── vkr_effect_system.h/c
   ├── Effect registration
   ├── Effect instantiation
   └── Deformed buffer management

4. Wave Effect Demo (Validation)
   ├── wave.slang shader
   ├── vkr_effect_wave.h/c
   └── Sponza fabric tagging

5. Scene Integration (Polish)
   ├── Scene JSON v3 format
   ├── Effect parsing
   └── Lifecycle management
```

## Detailed Checklist

### Phase 1: Tagging System

#### Files to Create
- [ ] `lib/src/renderer/systems/vkr_tag_system.h`
- [ ] `lib/src/renderer/systems/vkr_tag_system.c`

#### Files to Modify
- [ ] `lib/src/renderer/systems/vkr_scene_system.h` - Add SceneTag component
- [ ] `lib/src/renderer/systems/vkr_scene_system.c` - Register component, propagate tags
- [ ] `lib/src/renderer/resources/vkr_resources.h` - Add effect_tags to VkrSubMesh
- [ ] `lib/src/renderer/resources/loaders/scene_loader.c` - Parse tags JSON
- [ ] `lib/src/renderer/resources/loaders/mesh_loader.c` - Infer tags from materials
- [ ] `lib/CMakeLists.txt` - Add new source files

#### Implementation Tasks
```
[ ] Define VkrTagMask type (uint64_t)
[ ] Define built-in tag constants (VKR_TAG_FABRIC, etc.)
[ ] Implement tag operations (has_all, has_any, add, remove)
[ ] Implement vkr_tag_parse_name()
[ ] Implement vkr_tag_mask_to_string()
[ ] Define SceneTag component struct
[ ] Register SceneTag in vkr_scene_init()
[ ] Implement tag propagation in vkr_scene_update()
[ ] Add effect_tags field to VkrSubMesh
[ ] Parse "tags" field in scene JSON entity
[ ] Parse "tags_inherit" field in scene JSON
[ ] Implement vkr_tag_query_entities()
[ ] Implement vkr_tag_query_submeshes()
[ ] Add tag inference in mesh loader based on material name
```

#### Verification
```
[ ] Unit test: vkr_tag_parse_name parses "fabric|water" correctly
[ ] Unit test: vkr_tag_has_all/has_any work correctly
[ ] Unit test: Tag inheritance propagates in hierarchy
[ ] Integration: Scene loads with tagged entities
[ ] Integration: Query returns correct entities by tag
```

---

### Phase 2: Compute Pipeline Support

#### Files to Create
- [ ] `lib/src/renderer/vulkan/vulkan_compute.h`
- [ ] `lib/src/renderer/vulkan/vulkan_compute.c`

#### Files to Modify
- [ ] `lib/src/renderer/vkr_renderer.h` - Add compute types
- [ ] `lib/src/renderer/vulkan/vulkan_types.h` - Add s_ComputePipeline
- [ ] `lib/src/renderer/vulkan/vulkan_buffer.c` - Add storage buffer support
- [ ] `lib/src/renderer/renderer_frontend.h` - Add compute API
- [ ] `lib/src/renderer/renderer_frontend.c` - Implement compute API
- [ ] `lib/src/renderer/vulkan/vulkan_backend.c` - Add compute to backend interface
- [ ] `lib/CMakeLists.txt` - Add new source files

#### Implementation Tasks
```
[ ] Define VkrComputePipelineDescription struct
[ ] Define VkrComputeDispatch struct
[ ] Define VkrStorageBuffer struct
[ ] Define VkrBufferUsage and VkrBufferFlags enums
[ ] Define s_ComputePipeline internal struct
[ ] Implement vulkan_compute_pipeline_create()
[ ] Implement vulkan_compute_pipeline_destroy()
[ ] Implement vulkan_compute_pipeline_bind()
[ ] Implement vulkan_compute_dispatch()
[ ] Implement vulkan_compute_barrier_to_vertex()
[ ] Implement vulkan_compute_barrier_from_vertex()
[ ] Implement vulkan_storage_buffer_create()
[ ] Implement vulkan_storage_buffer_destroy()
[ ] Implement vkr_renderer_create_compute_pipeline()
[ ] Implement vkr_renderer_destroy_compute_pipeline()
[ ] Implement vkr_renderer_begin_compute()
[ ] Implement vkr_renderer_end_compute()
[ ] Implement vkr_renderer_dispatch_compute()
[ ] Implement vkr_renderer_create_storage_buffer()
[ ] Implement vkr_renderer_destroy_storage_buffer()
[ ] Implement vkr_renderer_bind_storage_buffer()
```

#### Shader Tasks
```
[ ] Create assets/shaders/effects/ directory
[ ] Create test_compute.slang (simple test shader)
[ ] Update build.sh to compile compute shaders
[ ] Verify test_compute.comp.spv generated
```

#### Verification
```
[ ] Unit test: Compute pipeline creates without errors
[ ] Unit test: Storage buffer creates and maps
[ ] Unit test: Compute dispatch executes (validation layers clean)
[ ] Integration: Test shader modifies buffer data
[ ] Validation: No synchronization errors with effects passes enabled in the render graph
```

---

### Phase 3: Effects System Core

#### Files to Create
- [ ] `lib/src/renderer/systems/vkr_effect_system.h`
- [ ] `lib/src/renderer/systems/vkr_effect_system.c`

#### Files to Modify
- [ ] `lib/src/renderer/renderer_frontend.h` - Add effect system member
- [ ] `lib/src/renderer/renderer_frontend.c` - Initialize/shutdown effect system
- [ ] `lib/src/renderer/passes/vkr_pass_world.c` - Check for deformed buffers
- [ ] `lib/CMakeLists.txt` - Add new source files

#### Implementation Tasks
```
[ ] Define VkrEffectHandle struct
[ ] Define VkrEffectPhase enum
[ ] Define VkrEffectParamType enum
[ ] Define VkrEffectParamDef struct
[ ] Define VkrEffectDefinition struct
[ ] Define VkrEffectInstance struct
[ ] Define VkrDeformedBuffer struct
[ ] Define VkrEffectSystemConfig struct
[ ] Define VkrEffectSystem struct
[ ] Implement vkr_effect_system_init()
[ ] Implement vkr_effect_system_shutdown()
[ ] Implement vkr_effect_system_set_scene()
[ ] Implement vkr_effect_system_update()
[ ] Implement vkr_effect_system_execute()
[ ] Implement vkr_effect_system_register()
[ ] Implement vkr_effect_system_unregister()
[ ] Implement vkr_effect_system_find()
[ ] Implement vkr_effect_system_instantiate()
[ ] Implement vkr_effect_system_instantiate_by_tags()
[ ] Implement vkr_effect_system_destroy_instance()
[ ] Implement vkr_effect_instance_set_param()
[ ] Implement vkr_effect_instance_set_param_index()
[ ] Implement vkr_effect_instance_set_enabled()
[ ] Implement vkr_effect_system_get_deformed_buffer()
[ ] Implement vkr_effect_system_reset_deformed_buffer()
[ ] Implement vkr_effect_system_bind_deformed_buffer()
[ ] Add effect system to renderer frontend struct
[ ] Initialize effect system in vkr_renderer_init()
[ ] Shutdown effect system in vkr_renderer_shutdown()
[ ] Call vkr_effect_system_update() in begin_frame
[ ] Register a named effects executor and declare its pass before `pass.world`
[ ] Check for deformed buffer in world pass rendering
```

#### Verification
```
[ ] Unit test: Effect registers with compute pipeline
[ ] Unit test: Effect instantiates for entity
[ ] Unit test: Tag-based instantiation finds entities
[ ] Unit test: Parameters set correctly
[ ] Integration: Effect executes in render loop
[ ] Integration: Deformed buffer used for rendering
```

---

### Phase 4: Wave Effect Demo

#### Files to Create
- [ ] `lib/src/renderer/systems/effects/vkr_effect_wave.h`
- [ ] `lib/src/renderer/systems/effects/vkr_effect_wave.c`
- [ ] `assets/shaders/effects/wave.slang`

#### Files to Modify
- [ ] `runtime/src/vkr_sample_runtime.c` - Register wave effect, apply to scene
- [ ] `lib/CMakeLists.txt` - Add effect source files
- [ ] `build.sh` - Add wave shader compilation

#### Implementation Tasks
```
[ ] Define VkrWaveEffectParams struct
[ ] Define VKR_WAVE_EFFECT_PARAMS_DEFAULT
[ ] Define wave effect parameter definitions array
[ ] Implement vkr_effect_wave_register()
[ ] Implement vkr_effect_wave_create_instance()
[ ] Implement vkr_effect_wave_set_params()
[ ] Write wave.slang compute shader
  [ ] Define Vertex struct matching VkrVertex3d
  [ ] Define WaveParams uniform buffer
  [ ] Implement hash() noise function
  [ ] Implement noise() 3D noise
  [ ] Implement computeMain()
    [ ] Read vertex position
    [ ] Calculate wave phase
    [ ] Apply time animation
    [ ] Combine multiple wave frequencies
    [ ] Add noise variation
    [ ] Apply height-based damping
    [ ] Calculate displacement vector
    [ ] Update normal approximation
    [ ] Write modified vertex
[ ] Add wave shader compilation to build
[ ] Register wave effect in app init
[ ] Tag Sponza fabric materials
[ ] Create wave instances for fabric
[ ] Test parameter adjustments
```

#### Verification
```
[ ] Shader compiles to valid SPIR-V
[ ] Effect registers without errors
[ ] Sponza fabric submeshes tagged
[ ] Wave instances created for fabric
[ ] Fabric visibly animates
[ ] Amplitude parameter affects wave height
[ ] Speed parameter affects animation speed
[ ] No GPU validation errors
[ ] Frame time impact < 1ms
```

---

### Phase 5: Scene Integration

#### Files to Modify
- [ ] `lib/src/renderer/resources/loaders/scene_loader.h` - Add effect structs
- [ ] `lib/src/renderer/resources/loaders/scene_loader.c` - Parse effects
- [ ] `lib/src/renderer/systems/vkr_scene_system.h` - Add effect apply API
- [ ] `lib/src/renderer/systems/vkr_scene_system.c` - Implement effect apply
- [ ] `assets/scenes/default.scene.json` - Add effects section

#### Implementation Tasks
```
[ ] Define VkrSceneEffectDef struct
[ ] Define VkrSceneEffectOverride struct
[ ] Implement vkr__parse_effect_phase()
[ ] Implement vkr__parse_effect_param_value()
[ ] Implement vkr__parse_scene_effects()
[ ] Implement vkr__parse_entity_effect_overrides()
[ ] Implement vkr_scene_apply_effects()
[ ] Implement vkr_scene_apply_entity_effects()
[ ] Implement vkr__apply_scene_params_to_instance()
[ ] Update vkr_scene_load() to parse effects
[ ] Implement vkr_effect_system_destroy_scene_instances()
[ ] Update vkr_scene_shutdown() to cleanup effects
[ ] Update default.scene.json to v3 with effects
[ ] Test scene reload preserves no stale instances
```

#### Verification
```
[ ] Scene v3 JSON parses without errors
[ ] Effects auto-instantiate on scene load
[ ] Parameters from JSON applied correctly
[ ] Per-entity overrides work
[ ] Scene unload destroys effect instances
[ ] Reload creates fresh instances
[ ] v2 scenes still load (backward compatible)
```

---

## File Dependency Graph

```
vkr_tag_system.h
    │
    └──► vkr_effect_system.h
              │
              ├──► vkr_effect_wave.h
              │
              └──► scene_loader.c (effects parsing)

vulkan_compute.h
    │
    └──► vkr_effect_system.c (compute dispatch)
              │
              └──► renderer_frontend.c (integration)

vkr_resources.h (VkrSubMesh.effect_tags)
    │
    ├──► mesh_loader.c (tag inference)
    │
    └──► vkr_effect_system.c (submesh queries)
```

## Build Integration

### CMakeLists.txt Additions

```cmake
# In lib/CMakeLists.txt

# Tag system
list(APPEND LIB_SOURCES
    src/renderer/systems/vkr_tag_system.c
)

# Effect system
list(APPEND LIB_SOURCES
    src/renderer/systems/vkr_effect_system.c
    src/renderer/systems/effects/vkr_effect_wave.c
)

# Compute support
list(APPEND LIB_SOURCES
    src/renderer/vulkan/vulkan_compute.c
)
```

### build.sh Additions

```bash
# Effect shader compilation
echo "Compiling effect shaders..."

EFFECT_SHADERS=(
    "wave"
)

for shader in "${EFFECT_SHADERS[@]}"; do
    if [ -f "assets/shaders/effects/${shader}.slang" ]; then
        slangc -profile sm_6_0 -target spirv -entry computeMain \
            "assets/shaders/effects/${shader}.slang" \
            -o "build/app/assets/shaders/effects/${shader}.comp.spv"
        echo "  Compiled ${shader}.slang"
    fi
done
```

## Testing Strategy

### Unit Tests

```
tests/
├── test_tag_system.c
│   ├── test_tag_parsing
│   ├── test_tag_operations
│   └── test_tag_to_string
│
├── test_compute_pipeline.c
│   ├── test_pipeline_creation
│   ├── test_storage_buffer
│   └── test_dispatch
│
└── test_effect_system.c
    ├── test_effect_registration
    ├── test_effect_instantiation
    ├── test_tag_based_instantiation
    └── test_parameter_setting
```

### Integration Tests

```
tests/integration/
├── test_wave_effect.c
│   ├── test_sponza_fabric_waves
│   └── test_performance_impact
│
└── test_scene_effects.c
    ├── test_scene_load_with_effects
    ├── test_scene_reload
    └── test_backward_compatibility
```

## Memory Validation

After implementation, verify memory management:

```bash
# Run with memory tracking enabled
VKR_MEMORY_TRACKING=1 ./build/app/vulkan_renderer

# Load/unload scene 10 times
# Check for memory leaks in:
# - Effect system arenas
# - Deformed buffers
# - Compute pipelines
# - Effect instances

# Expected: Memory usage stable after first scene load
```

## Performance Benchmarks

Target performance metrics:

| Metric | Target | Acceptable |
|--------|--------|------------|
| Effect system update | < 0.1ms | < 0.5ms |
| Compute dispatch (1000 verts) | < 0.2ms | < 0.5ms |
| Deformed buffer allocation | < 1ms | < 5ms |
| Frame time impact | < 1ms total | < 2ms |

## Final Validation

Before marking complete:

- [ ] All unit tests pass
- [ ] All integration tests pass
- [ ] No Vulkan validation errors
- [ ] No memory leaks on repeated scene load/unload
- [ ] Performance within targets
- [ ] Documentation accurate and complete
- [ ] Example scene works correctly
- [ ] Code follows project conventions (C11, naming, etc.)

## Troubleshooting Guide

### Common Issues

**Effect not visible:**
1. Check tags match between effect and entity
2. Verify compute shader compiles
3. Check deformed buffer bound correctly
4. Verify vertex stride matches

**GPU hang or crash:**
1. Enable validation layers
2. Check memory barriers
3. Verify descriptor set bindings
4. Check workgroup size vs vertex count

**Memory growth:**
1. Verify scene cleanup destroys instances
2. Check deformed buffer pool limits
3. Verify arena scopes end correctly

**Shader compilation fails:**
1. Check Slang syntax
2. Verify entry point name matches
3. Check binding indices match
