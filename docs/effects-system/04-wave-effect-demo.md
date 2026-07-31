---
status: proposed
updated: 2026-07-31
authority: design
---
# Phase 4: Wave Effect Demo

## Overview

Implement a wave deformation effect that animates Sponza's fabric curtains using the effects system. This serves as a proof-of-concept demonstrating the full pipeline from scene tagging to GPU compute execution.

## Prerequisites

- **Phase 1**: Tagging system implemented
- **Phase 2**: Compute pipeline support working
- **Phase 3**: Effects system core functional

## Demo Goal

Sponza scene contains fabric curtains. Apply a procedural wave animation that:
1. Deforms vertices in a sinusoidal pattern
2. Varies based on vertex world position
3. Animates over time
4. Responds to configurable parameters (amplitude, frequency, speed)

## Wave Effect Design

### Parameters

```c
// lib/src/renderer/systems/effects/vkr_effect_wave.h

/**
 * @brief Wave effect parameter block (matches shader uniform layout).
 */
typedef struct VkrWaveEffectParams {
  float amplitude;      // Wave height (default: 0.1)
  float frequency;      // Spatial frequency (default: 2.0)
  float speed;          // Animation speed (default: 1.0)
  float phase_offset;   // Initial phase offset (default: 0.0)

  Vec3 wave_direction;  // Wave propagation direction (default: 0, 0, 1)
  float _pad0;

  Vec3 wind_direction;  // Secondary wind direction (default: 1, 0, 0)
  float wind_strength;  // Wind influence (default: 0.3)

  float vertical_bias;  // Bias toward vertical movement (default: 0.5)
  float damping;        // Damping at vertex Y=0 (default: 1.0)
  float noise_scale;    // Noise contribution (default: 0.2)
  float _pad1;
} VkrWaveEffectParams;

#define VKR_WAVE_EFFECT_PARAMS_DEFAULT \
  ((VkrWaveEffectParams){              \
      .amplitude = 0.1f,               \
      .frequency = 2.0f,               \
      .speed = 1.0f,                   \
      .phase_offset = 0.0f,            \
      .wave_direction = {0.0f, 0.0f, 1.0f}, \
      .wind_direction = {1.0f, 0.0f, 0.0f}, \
      .wind_strength = 0.3f,           \
      .vertical_bias = 0.5f,           \
      .damping = 1.0f,                 \
      .noise_scale = 0.2f,             \
  })
```

### Shader

Create `assets/shaders/effects/wave.slang`:

```slang
// Wave deformation compute shader
// Applies sinusoidal displacement to vertices based on position and time

struct Vertex {
    float3 position;
    float3 normal;
    float2 texcoord;
    float4 color;
    float3 tangent;
};

// Storage buffer: deformed vertices (read-write)
[[vk::binding(0, 0)]]
RWStructuredBuffer<Vertex> vertices;

// Uniform buffer: effect parameters + time
[[vk::binding(1, 0)]]
cbuffer WaveParams {
    float amplitude;
    float frequency;
    float speed;
    float phase_offset;

    float3 wave_direction;
    float _pad0;

    float3 wind_direction;
    float wind_strength;

    float vertical_bias;
    float damping;
    float noise_scale;
    float _pad1;

    float time;           // Current time in seconds
    uint vertex_count;    // Total vertices
    float2 _pad2;
};

// Simple noise function (for variation)
float hash(float3 p) {
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    return lerp(lerp(lerp(hash(i + float3(0, 0, 0)), hash(i + float3(1, 0, 0)), f.x),
                     lerp(hash(i + float3(0, 1, 0)), hash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(hash(i + float3(0, 0, 1)), hash(i + float3(1, 0, 1)), f.x),
                     lerp(hash(i + float3(0, 1, 1)), hash(i + float3(1, 1, 1)), f.x), f.y), f.z);
}

[numthreads(64, 1, 1)]
void computeMain(uint3 dispatch_id : SV_DispatchThreadID) {
    uint idx = dispatch_id.x;
    if (idx >= vertex_count) return;

    Vertex v = vertices[idx];
    float3 original_pos = v.position;

    // Calculate wave phase based on position along wave direction
    float wave_phase = dot(original_pos, wave_direction) * frequency;
    float wind_phase = dot(original_pos, wind_direction) * frequency * 0.7;

    // Time-based animation
    float t = time * speed + phase_offset;

    // Primary wave
    float wave1 = sin(wave_phase + t);
    float wave2 = sin(wave_phase * 1.3 + t * 1.1) * 0.5;
    float wave3 = sin(wind_phase + t * 0.8) * wind_strength;

    // Combine waves
    float combined_wave = (wave1 + wave2 + wave3) * amplitude;

    // Add noise for natural variation
    float noise_value = noise(original_pos * noise_scale + float3(t * 0.2, 0, 0));
    combined_wave += (noise_value - 0.5) * amplitude * noise_scale;

    // Height-based damping (less movement at bottom, more at top)
    // Assumes Y is up, and vertices at Y=0 are anchored
    float height_factor = saturate(original_pos.y * damping);
    height_factor = height_factor * height_factor;  // Quadratic falloff

    // Apply displacement
    float3 displacement = float3(0, 0, 0);

    // Horizontal displacement (in wave and wind directions)
    displacement += wave_direction * combined_wave * (1.0 - vertical_bias) * height_factor;
    displacement += wind_direction * wave3 * 0.5 * height_factor;

    // Vertical displacement
    displacement.y += combined_wave * vertical_bias * height_factor;

    v.position = original_pos + displacement;

    // Approximate normal update (simplified - proper would need neighbor info)
    // This is a rough approximation that slightly rotates the normal
    float3 wave_tangent = normalize(cross(wave_direction, float3(0, 1, 0)));
    float normal_tilt = combined_wave * frequency * 0.3 * height_factor;
    v.normal = normalize(v.normal + wave_tangent * normal_tilt);

    vertices[idx] = v;
}
```

### Effect Registration

```c
// lib/src/renderer/systems/effects/vkr_effect_wave.c

#include "renderer/systems/effects/vkr_effect_wave.h"
#include "renderer/systems/vkr_effect_system.h"
#include "renderer/systems/vkr_tag_system.h"

static const VkrEffectParamDef s_wave_params[] = {
    {"amplitude", VKR_EFFECT_PARAM_FLOAT, offsetof(VkrWaveEffectParams, amplitude),
     sizeof(float), {.default_float = {0.1f}}},
    {"frequency", VKR_EFFECT_PARAM_FLOAT, offsetof(VkrWaveEffectParams, frequency),
     sizeof(float), {.default_float = {2.0f}}},
    {"speed", VKR_EFFECT_PARAM_FLOAT, offsetof(VkrWaveEffectParams, speed),
     sizeof(float), {.default_float = {1.0f}}},
    {"phase_offset", VKR_EFFECT_PARAM_FLOAT, offsetof(VkrWaveEffectParams, phase_offset),
     sizeof(float), {.default_float = {0.0f}}},
    // wave_direction handled as float3
    {"wave_direction_x", VKR_EFFECT_PARAM_FLOAT,
     offsetof(VkrWaveEffectParams, wave_direction), sizeof(float),
     {.default_float = {0.0f}}},
    {"wave_direction_y", VKR_EFFECT_PARAM_FLOAT,
     offsetof(VkrWaveEffectParams, wave_direction) + 4, sizeof(float),
     {.default_float = {0.0f}}},
    {"wave_direction_z", VKR_EFFECT_PARAM_FLOAT,
     offsetof(VkrWaveEffectParams, wave_direction) + 8, sizeof(float),
     {.default_float = {1.0f}}},
    // wind_direction
    {"wind_direction_x", VKR_EFFECT_PARAM_FLOAT,
     offsetof(VkrWaveEffectParams, wind_direction), sizeof(float),
     {.default_float = {1.0f}}},
    {"wind_direction_y", VKR_EFFECT_PARAM_FLOAT,
     offsetof(VkrWaveEffectParams, wind_direction) + 4, sizeof(float),
     {.default_float = {0.0f}}},
    {"wind_direction_z", VKR_EFFECT_PARAM_FLOAT,
     offsetof(VkrWaveEffectParams, wind_direction) + 8, sizeof(float),
     {.default_float = {0.0f}}},
    {"wind_strength", VKR_EFFECT_PARAM_FLOAT,
     offsetof(VkrWaveEffectParams, wind_strength), sizeof(float),
     {.default_float = {0.3f}}},
    {"vertical_bias", VKR_EFFECT_PARAM_FLOAT,
     offsetof(VkrWaveEffectParams, vertical_bias), sizeof(float),
     {.default_float = {0.5f}}},
    {"damping", VKR_EFFECT_PARAM_FLOAT, offsetof(VkrWaveEffectParams, damping),
     sizeof(float), {.default_float = {1.0f}}},
    {"noise_scale", VKR_EFFECT_PARAM_FLOAT, offsetof(VkrWaveEffectParams, noise_scale),
     sizeof(float), {.default_float = {0.2f}}},
};

static const uint32_t s_wave_param_count =
    sizeof(s_wave_params) / sizeof(s_wave_params[0]);

bool8_t vkr_effect_wave_register(VkrEffectSystem *system,
                                  VkrEffectHandle *out_handle) {
  VkrEffectDefinition def = {
      .name = "wave",
      .compute_shader_path = string8_lit("assets/shaders/effects/wave.comp.spv"),
      .compute_entry_point = string8_lit("computeMain"),
      .target_tags = VKR_TAG_FABRIC,
      .exclude_tags = VKR_TAG_NONE,
      .phase = VKR_EFFECT_PHASE_PRE_RENDER,
      .priority = 0,
      .workgroup_size = {64, 1, 1},
      .param_defs = s_wave_params,
      .param_count = s_wave_param_count,
      .param_block_size = sizeof(VkrWaveEffectParams),
  };

  return vkr_effect_system_register(system, &def, out_handle);
}

VkrEffectInstance *vkr_effect_wave_create_instance(VkrEffectSystem *system,
                                                    VkrEntity entity,
                                                    uint32_t submesh_index) {
  VkrEffectHandle effect = vkr_effect_system_find(system, "wave");
  if (effect.id == 0) {
    log_error("Wave effect not registered");
    return NULL;
  }

  VkrEffectInstance *instance;
  if (!vkr_effect_system_instantiate(system, effect, entity, submesh_index,
                                      &instance)) {
    return NULL;
  }

  return instance;
}

void vkr_effect_wave_set_params(VkrEffectInstance *instance,
                                 const VkrWaveEffectParams *params) {
  if (!instance || !instance->param_data) return;
  memcpy(instance->param_data, params, sizeof(VkrWaveEffectParams));
}
```

## Scene Integration

### Tagging Sponza Fabric

The Sponza model contains materials with "fabric" or "curtain" in their names. The mesh loader (from Phase 1) automatically applies `VKR_TAG_FABRIC` to these submeshes.

For explicit tagging via scene JSON:

```json
{
  "version": 2,
  "entities": [
    {
      "name": "Sponza",
      "parent": null,
      "transform": {
        "pos": [0.0, 0.0, -15.0],
        "rot": [0.0, 0.0, 0.0, 1.0],
        "scale": [0.0085, 0.0085, 0.0085]
      },
      "mesh": {
        "path": "assets/models/sponza.obj",
        "pipeline_domain": "world"
      },
      "tags": ["animated"],
      "effects": [
        {
          "name": "wave",
          "target_tags": "fabric",
          "params": {
            "amplitude": 0.15,
            "frequency": 2.5,
            "speed": 0.8,
            "wind_strength": 0.4
          }
        }
      ]
    }
  ]
}
```

### Alternative: Identify Fabric Submeshes by Material

If the mesh loader tag inference isn't sufficient, scan submeshes after load:

```c
void vkr_demo_tag_sponza_fabric(VkrMeshManager *mesh_manager,
                                 VkrMaterialSystem *material_system,
                                 uint32_t sponza_mesh_index) {
  VkrMesh *mesh = vkr_mesh_manager_get(mesh_manager, sponza_mesh_index);
  if (!mesh) return;

  // Known Sponza fabric material names (varies by version)
  const char *fabric_materials[] = {
      "fabric_blue",
      "fabric_red",
      "fabric_green",
      "curtain",
      "cloth",
      "00_mrtenie_plavac",  // Some Sponza versions use this
      "01_mrtenie_crvena",
      "02_mrtenie_zelena",
  };
  const uint32_t fabric_count = sizeof(fabric_materials) / sizeof(char *);

  for (uint32_t i = 0; i < mesh->submeshes.length; i++) {
    VkrSubMesh *submesh = &mesh->submeshes.data[i];

    // Get material name
    VkrMaterial *mat = vkr_material_system_get_by_handle(
        material_system, submesh->material);
    if (!mat) continue;

    // Check against known fabric names
    for (uint32_t j = 0; j < fabric_count; j++) {
      if (strstr(mat->name, fabric_materials[j]) != NULL) {
        submesh->effect_tags |= VKR_TAG_FABRIC;
        log_debug("Tagged submesh %u as fabric (material: %s)", i, mat->name);
        break;
      }
    }
  }
}
```

## Application Integration

### Setup in Main Application

```c
// app/src/main.c or app/src/application.c

#include "renderer/systems/vkr_effect_system.h"
#include "renderer/systems/effects/vkr_effect_wave.h"

typedef struct AppState {
  VkrRendererFrontendHandle renderer;
  VkrScene *current_scene;
  VkrEffectSystem effect_system;
  VkrEffectHandle wave_effect;
  // ... other state
} AppState;

bool8_t app_init(AppState *app) {
  // ... initialize renderer, scene, etc.

  // Initialize effect system
  if (!vkr_effect_system_init(&app->effect_system, app->renderer,
                               &app->mesh_manager, &app->geometry_system,
                               NULL)) {
    log_error("Failed to initialize effect system");
    return false_v;
  }

  // Register wave effect
  if (!vkr_effect_wave_register(&app->effect_system, &app->wave_effect)) {
    log_error("Failed to register wave effect");
    return false_v;
  }

  return true_v;
}

void app_on_scene_load(AppState *app, VkrScene *scene) {
  // Set scene for effect system
  vkr_effect_system_set_scene(&app->effect_system, scene);

  // Auto-instantiate effects by tags
  uint32_t wave_count = 0;
  vkr_effect_system_instantiate_by_tags(&app->effect_system, app->wave_effect,
                                         &wave_count);
  log_info("Created %u wave effect instances", wave_count);

  // Or manually create for specific entities:
  /*
  VkrEntity sponza = vkr_scene_find_entity_by_name(scene, "Sponza");
  if (sponza != VKR_ENTITY_INVALID) {
    VkrEffectInstance *wave = vkr_effect_wave_create_instance(
        &app->effect_system, sponza, UINT32_MAX);
    if (wave) {
      VkrWaveEffectParams params = VKR_WAVE_EFFECT_PARAMS_DEFAULT;
      params.amplitude = 0.12f;
      params.speed = 0.9f;
      vkr_effect_wave_set_params(wave, &params);
    }
  }
  */
}

void app_update(AppState *app, float delta_time) {
  // Update effect system (updates elapsed time, etc.)
  vkr_effect_system_update(&app->effect_system, delta_time);
}

void app_render(AppState *app) {
  // Begin frame
  vkr_renderer_begin_frame(app->renderer, app->delta_time);

  // Execute pre-render effects (computes deformed buffers)
  vkr_effect_system_execute(&app->effect_system, VKR_EFFECT_PHASE_PRE_RENDER);

  // Render world (will use deformed buffers where applicable)
  vkr_renderer_draw_world(app->renderer, app->current_scene);

  // Execute post-world effects
  vkr_effect_system_execute(&app->effect_system, VKR_EFFECT_PHASE_POST_WORLD);

  // Render UI
  vkr_renderer_draw_ui(app->renderer);

  // End frame
  vkr_renderer_end_frame(app->renderer);
}

void app_shutdown(AppState *app) {
  vkr_effect_system_shutdown(&app->effect_system);
  // ... shutdown other systems
}
```

## Shader Compilation

Add to `build.sh`:

```bash
# Compile effect shaders
EFFECT_SHADERS="wave"

for shader in $EFFECT_SHADERS; do
  slangc -profile sm_6_0 -target spirv -entry computeMain \
    assets/shaders/effects/${shader}.slang \
    -o build/app/assets/shaders/effects/${shader}.comp.spv
done
```

Or add to CMake:

```cmake
# In lib/CMakeLists.txt or appropriate location
set(EFFECT_SHADERS
    wave
)

foreach(shader ${EFFECT_SHADERS})
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/app/assets/shaders/effects/${shader}.comp.spv
        COMMAND slangc -profile sm_6_0 -target spirv -entry computeMain
                ${CMAKE_SOURCE_DIR}/assets/shaders/effects/${shader}.slang
                -o ${CMAKE_BINARY_DIR}/app/assets/shaders/effects/${shader}.comp.spv
        DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/effects/${shader}.slang
        COMMENT "Compiling ${shader} effect shader"
    )
    list(APPEND COMPILED_EFFECT_SHADERS
        ${CMAKE_BINARY_DIR}/app/assets/shaders/effects/${shader}.comp.spv)
endforeach()

add_custom_target(effect_shaders ALL DEPENDS ${COMPILED_EFFECT_SHADERS})
```

## Testing

### Visual Verification

1. Load Sponza scene
2. Observe curtains/fabric elements
3. Verify visible wave animation
4. Adjust parameters and confirm changes

### Performance Test

```c
void test_wave_effect_performance(void) {
  VkrEffectSystem system;
  // Setup...

  // Create many instances
  const uint32_t NUM_INSTANCES = 100;
  for (uint32_t i = 0; i < NUM_INSTANCES; i++) {
    VkrEffectInstance *instance;
    vkr_effect_system_instantiate(&system, wave_effect, test_entities[i],
                                   UINT32_MAX, &instance);
  }

  // Measure execution time
  uint64_t start = platform_get_time_ns();

  for (uint32_t frame = 0; frame < 1000; frame++) {
    vkr_effect_system_update(&system, 0.016f);
    vkr_effect_system_execute(&system, VKR_EFFECT_PHASE_PRE_RENDER);
    // Wait for GPU...
  }

  uint64_t elapsed = platform_get_time_ns() - start;
  float avg_ms = (float)(elapsed / 1000) / 1000.0f;

  log_info("Average effect execution time: %.3f ms", avg_ms);

  // Should be < 1ms for reasonable performance
  assert(avg_ms < 1.0f);
}
```

### Synchronization Validation

Enable Vulkan validation layers and verify:
- No compute-to-graphics synchronization errors
- No buffer access violations
- Descriptors properly bound before dispatch

## Debugging

### Visual Debug: Color by Displacement

Modify shader temporarily to visualize displacement:

```slang
// Debug: encode displacement as color
float disp_magnitude = length(displacement);
v.color = float4(disp_magnitude * 5.0, 0.5, 1.0 - disp_magnitude * 5.0, 1.0);
```

### CPU Verification

Add readback to verify compute output:

```c
void debug_read_deformed_buffer(VkrEffectSystem *system,
                                 VkrDeformedBuffer *buffer) {
  // Map buffer or use staging buffer to read back
  void *data = vkr_renderer_map_storage_buffer(system->renderer, buffer->buffer);
  if (!data) return;

  Vertex *verts = (Vertex *)data;
  for (uint32_t i = 0; i < min(buffer->vertex_count, 10); i++) {
    log_debug("Vertex %u: pos=(%.3f, %.3f, %.3f)", i,
              verts[i].position[0], verts[i].position[1], verts[i].position[2]);
  }

  vkr_renderer_unmap_storage_buffer(system->renderer, buffer->buffer);
}
```

## Completion Criteria

- [ ] Wave shader compiles to SPIR-V
- [ ] Effect registers successfully
- [ ] Sponza fabric submeshes tagged correctly
- [ ] Effect instances created for fabric meshes
- [ ] Compute dispatch executes without errors
- [ ] Fabric visibly animates with wave motion
- [ ] Parameters (amplitude, speed) affect animation
- [ ] No GPU synchronization errors
- [ ] Performance acceptable (< 1ms overhead)

## Next Steps

After completing this phase:
- **05-scene-integration.md**: Full scene JSON format for effects
- Explore additional effects (water, foliage, particles)
- Add editor UI for effect parameter tweaking
