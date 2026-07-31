---
status: proposed
updated: 2026-07-31
authority: design
---
# Terrain Rendering Design Document

**Legacy note:** This document references the deprecated view/layer system
(`view system (removed)`) and `VkrLayer*` callback signatures in examples. Render
orchestration now uses the render graph; view modules are render helpers
invoked by pass executors.

## Purpose

Implement a **heightmap-based terrain rendering system** for the Vulkan renderer. The system supports large terrains through chunking, level-of-detail (LOD) for performance, texture splatting for varied surface materials, and seamless integration with existing renderer systems (frustum culling, shadows, picking).

This document is **LLM-consumable**: explicit file paths, concrete APIs, data structures, shader code, and a phased implementation plan.

---

## Current State

### Relevant Systems

- **Geometry System** (`lib/src/renderer/systems/vkr_geometry_system.h`): Manages vertex/index buffers
- **Mesh Manager** (`lib/src/renderer/systems/vkr_mesh_manager.h`): Manages drawable meshes with transforms
- **Texture System** (`lib/src/renderer/systems/vkr_texture_system.h`): Texture loading with batch support
- **Material System** (`lib/src/renderer/systems/vkr_material_system.h`): Phong materials with texture slots
- **Frustum Culling** (`lib/src/math/vkr_frustum.h`): Bounding sphere tests for visibility
- **Shadow System** (planned): CSM for directional light shadows

### Vertex Format

`VkrVertex3d` in `lib/src/renderer/vkr_buffer.h`:
```c
typedef struct VkrVertex3d {
  Vec3 position;   // Position in object space
  Vec3 normal;     // Vertex normal
  Vec2 texcoord;   // Texture coordinate
  Vec4 colour;     // Vertex color (RGBA)
  Vec4 tangent;    // Tangent + handedness
} VkrVertex3d;
```

---

## Goals

1. **Heightmap terrain**: Load 16-bit or 8-bit heightmaps to generate terrain geometry
2. **Chunked terrain**: Split large terrains into manageable chunks for culling/streaming
3. **LOD system**: Multiple detail levels per chunk based on camera distance
4. **Texture splatting**: Blend up to 4 terrain layers using a splatmap
5. **Normal mapping**: Per-layer normal maps for surface detail
6. **Shadow integration**: Terrain casts and receives shadows
7. **Picking support**: Terrain chunks are pickable for editor interaction

### Non-Goals (Initial Implementation)

- Procedural terrain generation (noise-based)
- Terrain editing/sculpting at runtime
- Tessellation-based LOD (geometry shader/tessellation)
- Terrain streaming from disk (all chunks loaded at init)
- Water/ocean rendering
- Vegetation instancing on terrain

---

## Architecture Overview

### System Components

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Terrain System Architecture                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                        VkrTerrainConfig                              │    │
│  │  - Heightmap path, dimensions, height scale                         │    │
│  │  - Chunk size, LOD levels, LOD distances                            │    │
│  │  - Texture layer paths (albedo, normal per layer)                   │    │
│  │  - Splatmap path                                                    │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                    │                                         │
│                                    ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                        VkrTerrainSystem                              │    │
│  │  - Owns terrain chunks array                                        │    │
│  │  - Manages LOD selection per chunk                                  │    │
│  │  - Handles terrain-specific uniforms                                │    │
│  │  - Updates chunk visibility via frustum culling                     │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                    │                                         │
│              ┌─────────────────────┼─────────────────────┐                   │
│              ▼                     ▼                     ▼                   │
│  ┌───────────────────┐ ┌───────────────────┐ ┌───────────────────┐          │
│  │  VkrTerrainChunk  │ │  VkrTerrainChunk  │ │  VkrTerrainChunk  │  ...     │
│  │  - LOD geometries │ │  - LOD geometries │ │  - LOD geometries │          │
│  │  - World bounds   │ │  - World bounds   │ │  - World bounds   │          │
│  │  - Current LOD    │ │  - Current LOD    │ │  - Current LOD    │          │
│  └───────────────────┘ └───────────────────┘ └───────────────────┘          │
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                     Terrain Shader Resources                         │    │
│  │  - Splatmap texture (RGBA = layer weights)                          │    │
│  │  - Layer textures (4x albedo + 4x normal)                           │    │
│  │  - Terrain uniform buffer (texture scales, height range)            │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Data Flow

```
1. Load heightmap →
2. Generate chunk meshes (per LOD level) →
3. Create geometry handles →
4. Load splatmap + layer textures →
5. Per-frame: LOD selection + frustum culling →
6. Render visible chunks with terrain shader
```

---

## Data Structures

### Terrain Configuration

Add `lib/src/renderer/systems/vkr_terrain_system.h`:

```c
#define VKR_TERRAIN_MAX_LOD_LEVELS 5
#define VKR_TERRAIN_MAX_TEXTURE_LAYERS 4
#define VKR_TERRAIN_CHUNK_SIZE_DEFAULT 64

typedef struct VkrTerrainLayerConfig {
  String8 albedo_path;         // Diffuse texture path
  String8 normal_path;         // Normal map path (optional)
  float32_t texture_scale;     // UV tiling scale
  float32_t normal_strength;   // Normal map intensity
} VkrTerrainLayerConfig;

typedef struct VkrTerrainConfig {
  // Heightmap
  String8 heightmap_path;      // Path to heightmap image (8/16-bit grayscale)
  float32_t height_scale;      // World-space height multiplier
  float32_t horizontal_scale;  // World-space XZ scale per texel

  // Chunking
  uint32_t chunk_size;         // Vertices per chunk edge (power of 2, e.g., 64)

  // LOD
  uint32_t lod_count;                              // Number of LOD levels (1-5)
  float32_t lod_distances[VKR_TERRAIN_MAX_LOD_LEVELS];  // Distance thresholds

  // Texturing
  String8 splatmap_path;       // RGBA splatmap for layer blending
  VkrTerrainLayerConfig layers[VKR_TERRAIN_MAX_TEXTURE_LAYERS];
  uint32_t layer_count;        // Active layer count (1-4)

  // World placement
  Vec3 origin;                 // World-space terrain origin (corner)

  // Features
  bool8_t cast_shadows;        // Include in shadow pass
  bool8_t receive_shadows;     // Sample shadow map in shader
} VkrTerrainConfig;

#define VKR_TERRAIN_CONFIG_DEFAULT ((VkrTerrainConfig){       \
  .height_scale = 100.0f,                                     \
  .horizontal_scale = 1.0f,                                   \
  .chunk_size = 64,                                           \
  .lod_count = 4,                                             \
  .lod_distances = {50.0f, 150.0f, 400.0f, 1000.0f, 2500.0f}, \
  .layer_count = 1,                                           \
  .origin = {0, 0, 0},                                        \
  .cast_shadows = true_v,                                     \
  .receive_shadows = true_v,                                  \
})
```

### Terrain Chunk

```c
typedef struct VkrTerrainChunkLOD {
  VkrGeometryHandle geometry;    // Geometry for this LOD level
  uint32_t vertex_count;         // Vertex count at this LOD
  uint32_t index_count;          // Index count at this LOD
} VkrTerrainChunkLOD;

typedef struct VkrTerrainChunk {
  // Grid position
  uint32_t grid_x;               // Chunk X index in grid
  uint32_t grid_z;               // Chunk Z index in grid

  // LOD data
  VkrTerrainChunkLOD lods[VKR_TERRAIN_MAX_LOD_LEVELS];
  uint32_t lod_count;            // Number of available LODs
  uint32_t current_lod;          // Currently selected LOD

  // World-space bounds
  Vec3 bounds_center;            // AABB center for culling
  Vec3 bounds_extents;           // AABB half-extents
  float32_t bounds_radius;       // Bounding sphere radius

  // Height range for this chunk
  float32_t min_height;          // Minimum vertex height
  float32_t max_height;          // Maximum vertex height

  // Visibility
  bool8_t visible;               // Frustum culling result
  float32_t camera_distance;     // Distance to camera (for LOD)
} VkrTerrainChunk;

Array(VkrTerrainChunk);
```

### Terrain System

```c
typedef struct VkrTerrainSystem {
  Arena *arena;
  VkrAllocator allocator;

  // Configuration
  VkrTerrainConfig config;

  // Heightmap data
  float32_t *heightmap;          // Normalized heights [0,1]
  uint32_t heightmap_width;      // Heightmap resolution X
  uint32_t heightmap_height;     // Heightmap resolution Z

  // Chunks
  Array_VkrTerrainChunk chunks;
  uint32_t chunks_x;             // Number of chunks in X
  uint32_t chunks_z;             // Number of chunks in Z

  // Textures
  VkrTextureHandle splatmap;
  VkrTextureHandle layer_albedo[VKR_TERRAIN_MAX_TEXTURE_LAYERS];
  VkrTextureHandle layer_normal[VKR_TERRAIN_MAX_TEXTURE_LAYERS];

  // Pipeline
  VkrPipelineHandle terrain_pipeline;
  VkrPipelineHandle terrain_shadow_pipeline;
  VkrShaderConfig shader_config;

  // Material/instance state
  VkrRendererInstanceStateHandle instance_state;

  // Statistics
  uint32_t visible_chunks;
  uint32_t rendered_triangles;

  // State
  bool8_t initialized;
} VkrTerrainSystem;
```

---

## Heightmap Loading

### Supported Formats

- **8-bit grayscale**: 0-255 mapped to [0, 1]
- **16-bit grayscale**: 0-65535 mapped to [0, 1]
- **RAW format**: Headerless 16-bit data

```c
bool8_t vkr_terrain_load_heightmap(VkrTerrainSystem *system,
                                    const VkrTerrainConfig *config) {
  // Load image via stb_image or raw file
  int width, height, channels;

  // Try 16-bit first
  uint16_t *data16 = stbi_load_16(string8_cstr(&config->heightmap_path),
                                   &width, &height, &channels, 1);
  if (data16) {
    system->heightmap = vkr_allocator_alloc(
        &system->allocator,
        sizeof(float32_t) * width * height,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

    for (int i = 0; i < width * height; ++i) {
      system->heightmap[i] = (float32_t)data16[i] / 65535.0f;
    }
    stbi_image_free(data16);
  } else {
    // Fall back to 8-bit
    uint8_t *data8 = stbi_load(string8_cstr(&config->heightmap_path),
                               &width, &height, &channels, 1);
    if (!data8) {
      log_error("Failed to load heightmap: %s",
                string8_cstr(&config->heightmap_path));
      return false_v;
    }

    system->heightmap = vkr_allocator_alloc(
        &system->allocator,
        sizeof(float32_t) * width * height,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

    for (int i = 0; i < width * height; ++i) {
      system->heightmap[i] = (float32_t)data8[i] / 255.0f;
    }
    stbi_image_free(data8);
  }

  system->heightmap_width = width;
  system->heightmap_height = height;
  return true_v;
}
```

### Height Sampling

```c
// Bilinear interpolation for smooth height sampling
float32_t vkr_terrain_sample_height(const VkrTerrainSystem *system,
                                     float32_t u, float32_t v) {
  // Clamp to valid range
  u = vkr_clamp_f32(u, 0.0f, 1.0f);
  v = vkr_clamp_f32(v, 0.0f, 1.0f);

  float32_t fx = u * (system->heightmap_width - 1);
  float32_t fz = v * (system->heightmap_height - 1);

  uint32_t x0 = (uint32_t)fx;
  uint32_t z0 = (uint32_t)fz;
  uint32_t x1 = vkr_min_u32(x0 + 1, system->heightmap_width - 1);
  uint32_t z1 = vkr_min_u32(z0 + 1, system->heightmap_height - 1);

  float32_t tx = fx - (float32_t)x0;
  float32_t tz = fz - (float32_t)z0;

  float32_t h00 = system->heightmap[z0 * system->heightmap_width + x0];
  float32_t h10 = system->heightmap[z0 * system->heightmap_width + x1];
  float32_t h01 = system->heightmap[z1 * system->heightmap_width + x0];
  float32_t h11 = system->heightmap[z1 * system->heightmap_width + x1];

  float32_t h0 = h00 * (1.0f - tx) + h10 * tx;
  float32_t h1 = h01 * (1.0f - tx) + h11 * tx;

  return h0 * (1.0f - tz) + h1 * tz;
}
```

---

## Mesh Generation

### Chunk Mesh Generation

```c
typedef struct VkrTerrainMeshGenParams {
  uint32_t chunk_x;
  uint32_t chunk_z;
  uint32_t lod_level;        // 0 = highest detail
  uint32_t vertices_per_edge; // For this LOD level
} VkrTerrainMeshGenParams;

bool8_t vkr_terrain_generate_chunk_mesh(
    VkrTerrainSystem *system,
    RendererFrontend *rf,
    const VkrTerrainMeshGenParams *params,
    VkrTerrainChunkLOD *out_lod)
{
  const VkrTerrainConfig *cfg = &system->config;

  // LOD step: skip vertices based on LOD level
  uint32_t step = 1 << params->lod_level;
  uint32_t verts_per_edge = (cfg->chunk_size / step) + 1;

  uint32_t vertex_count = verts_per_edge * verts_per_edge;
  uint32_t quad_count = (verts_per_edge - 1) * (verts_per_edge - 1);
  uint32_t index_count = quad_count * 6;

  // Allocate temporary buffers
  VkrAllocatorScope scope = vkr_allocator_begin_scope(&rf->allocator);

  VkrVertex3d *vertices = vkr_allocator_alloc(
      &rf->allocator,
      sizeof(VkrVertex3d) * vertex_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  uint32_t *indices = vkr_allocator_alloc(
      &rf->allocator,
      sizeof(uint32_t) * index_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  // Base world position for this chunk
  float32_t chunk_world_x = cfg->origin.x +
      (float32_t)(params->chunk_x * cfg->chunk_size) * cfg->horizontal_scale;
  float32_t chunk_world_z = cfg->origin.z +
      (float32_t)(params->chunk_z * cfg->chunk_size) * cfg->horizontal_scale;

  // Generate vertices
  uint32_t vi = 0;
  for (uint32_t z = 0; z < verts_per_edge; ++z) {
    for (uint32_t x = 0; x < verts_per_edge; ++x) {
      // Global heightmap coordinates
      uint32_t hx = params->chunk_x * cfg->chunk_size + x * step;
      uint32_t hz = params->chunk_z * cfg->chunk_size + z * step;

      // Clamp to heightmap bounds
      hx = vkr_min_u32(hx, system->heightmap_width - 1);
      hz = vkr_min_u32(hz, system->heightmap_height - 1);

      float32_t height = system->heightmap[hz * system->heightmap_width + hx];

      // World position
      vertices[vi].position = vec3_new(
          chunk_world_x + (float32_t)(x * step) * cfg->horizontal_scale,
          cfg->origin.y + height * cfg->height_scale,
          chunk_world_z + (float32_t)(z * step) * cfg->horizontal_scale
      );

      // Texture coordinates (0-1 within chunk, or global based on terrain size)
      vertices[vi].texcoord = vec2_new(
          (float32_t)hx / (float32_t)(system->heightmap_width - 1),
          (float32_t)hz / (float32_t)(system->heightmap_height - 1)
      );

      // Default color (can be used for vertex-based splatting)
      vertices[vi].colour = vec4_new(1, 1, 1, 1);

      vi++;
    }
  }

  // Generate indices (triangle strip pattern)
  uint32_t ii = 0;
  for (uint32_t z = 0; z < verts_per_edge - 1; ++z) {
    for (uint32_t x = 0; x < verts_per_edge - 1; ++x) {
      uint32_t tl = z * verts_per_edge + x;
      uint32_t tr = tl + 1;
      uint32_t bl = (z + 1) * verts_per_edge + x;
      uint32_t br = bl + 1;

      // First triangle (top-left, bottom-left, top-right)
      indices[ii++] = tl;
      indices[ii++] = bl;
      indices[ii++] = tr;

      // Second triangle (top-right, bottom-left, bottom-right)
      indices[ii++] = tr;
      indices[ii++] = bl;
      indices[ii++] = br;
    }
  }

  // Compute normals
  vkr_terrain_compute_normals(vertices, vertex_count, indices, index_count);

  // Compute tangents for normal mapping
  vkr_geometry_system_generate_tangents(&rf->allocator, vertices, vertex_count,
                                        indices, index_count);

  // Create geometry
  char name[64];
  snprintf(name, sizeof(name), "terrain_chunk_%u_%u_lod%u",
           params->chunk_x, params->chunk_z, params->lod_level);

  VkrGeometryConfig geo_cfg = {
    .vertex_size = sizeof(VkrVertex3d),
    .vertex_count = vertex_count,
    .vertices = vertices,
    .index_size = sizeof(uint32_t),
    .index_count = index_count,
    .indices = indices,
  };
  MemCopy(geo_cfg.name, name, vkr_min_u64(strlen(name) + 1, GEOMETRY_NAME_MAX_LENGTH));

  // Compute bounds
  vkr_terrain_compute_geometry_bounds(vertices, vertex_count, &geo_cfg);

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  out_lod->geometry = vkr_geometry_system_create(&rf->geometry_system, &geo_cfg,
                                                  true_v, &err);
  out_lod->vertex_count = vertex_count;
  out_lod->index_count = index_count;

  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  return out_lod->geometry.id != 0;
}
```

### Normal Computation

```c
void vkr_terrain_compute_normals(VkrVertex3d *vertices, uint32_t vertex_count,
                                  const uint32_t *indices, uint32_t index_count) {
  // Zero all normals
  for (uint32_t i = 0; i < vertex_count; ++i) {
    vertices[i].normal = vec3_zero();
  }

  // Accumulate face normals
  for (uint32_t i = 0; i < index_count; i += 3) {
    uint32_t i0 = indices[i];
    uint32_t i1 = indices[i + 1];
    uint32_t i2 = indices[i + 2];

    Vec3 v0 = vertices[i0].position;
    Vec3 v1 = vertices[i1].position;
    Vec3 v2 = vertices[i2].position;

    Vec3 edge1 = vec3_sub(v1, v0);
    Vec3 edge2 = vec3_sub(v2, v0);
    Vec3 face_normal = vec3_cross(edge1, edge2);

    // Accumulate (area-weighted)
    vertices[i0].normal = vec3_add(vertices[i0].normal, face_normal);
    vertices[i1].normal = vec3_add(vertices[i1].normal, face_normal);
    vertices[i2].normal = vec3_add(vertices[i2].normal, face_normal);
  }

  // Normalize
  for (uint32_t i = 0; i < vertex_count; ++i) {
    vertices[i].normal = vec3_normalize(vertices[i].normal);
  }
}
```

---

## LOD System

### LOD Selection Algorithm

```c
void vkr_terrain_update_lod(VkrTerrainSystem *system, const Vec3 *camera_pos) {
  const VkrTerrainConfig *cfg = &system->config;

  for (uint32_t i = 0; i < system->chunks.length; ++i) {
    VkrTerrainChunk *chunk = &system->chunks.data[i];

    // Calculate distance to chunk center
    chunk->camera_distance = vec3_distance(chunk->bounds_center, *camera_pos);

    // Select LOD based on distance thresholds
    uint32_t new_lod = cfg->lod_count - 1;  // Start with lowest detail
    for (uint32_t lod = 0; lod < cfg->lod_count; ++lod) {
      if (chunk->camera_distance < cfg->lod_distances[lod]) {
        new_lod = lod;
        break;
      }
    }

    chunk->current_lod = new_lod;
  }
}
```

### LOD Transition Handling (Optional: Geomorphing)

To avoid popping artifacts during LOD transitions:

```c
// In terrain shader - blend between LOD levels
float morph_factor = compute_morph_factor(distance, lod_distances[current_lod],
                                           lod_distances[current_lod + 1]);

// Lerp vertex height toward next LOD's height
float morphed_height = mix(high_lod_height, low_lod_height, morph_factor);
```

---

## Texture Splatting

### Splatmap Format

- **RGBA texture**: Each channel represents weight for one terrain layer
- **Weights normalized**: R + G + B + A should sum to 1.0 (or renormalize in shader)
- **Resolution**: Typically matches or is lower than terrain resolution

### Shader Implementation

```hlsl
// Terrain textures
[[vk::binding(1, 1)]] Texture2D<float4> splatmap;
[[vk::binding(2, 1)]] Texture2D<float4> layer0_albedo;
[[vk::binding(3, 1)]] Texture2D<float4> layer1_albedo;
[[vk::binding(4, 1)]] Texture2D<float4> layer2_albedo;
[[vk::binding(5, 1)]] Texture2D<float4> layer3_albedo;
[[vk::binding(6, 1)]] Texture2D<float4> layer0_normal;
[[vk::binding(7, 1)]] Texture2D<float4> layer1_normal;
[[vk::binding(8, 1)]] Texture2D<float4> layer2_normal;
[[vk::binding(9, 1)]] Texture2D<float4> layer3_normal;

[[vk::binding(10, 1)]] SamplerState terrain_sampler;
[[vk::binding(11, 1)]] SamplerState splatmap_sampler;

struct TerrainUniform {
  float4 layer_scales;      // Texture tiling per layer
  float4 normal_strengths;  // Normal map intensity per layer
  float2 terrain_size;      // Total terrain size (for UV calculation)
  float height_scale;
  float padding;
};

[[vk::binding(0, 1)]] ConstantBuffer<TerrainUniform> terrain_ubo;

float4 sample_terrain_albedo(float2 uv, float4 splat) {
  // Tile UVs per layer
  float2 uv0 = uv * terrain_ubo.layer_scales.x;
  float2 uv1 = uv * terrain_ubo.layer_scales.y;
  float2 uv2 = uv * terrain_ubo.layer_scales.z;
  float2 uv3 = uv * terrain_ubo.layer_scales.w;

  // Sample each layer
  float4 c0 = layer0_albedo.Sample(terrain_sampler, uv0);
  float4 c1 = layer1_albedo.Sample(terrain_sampler, uv1);
  float4 c2 = layer2_albedo.Sample(terrain_sampler, uv2);
  float4 c3 = layer3_albedo.Sample(terrain_sampler, uv3);

  // Blend by splatmap weights
  return c0 * splat.r + c1 * splat.g + c2 * splat.b + c3 * splat.a;
}

float3 sample_terrain_normal(float2 uv, float4 splat, float3x3 tbn) {
  float2 uv0 = uv * terrain_ubo.layer_scales.x;
  float2 uv1 = uv * terrain_ubo.layer_scales.y;
  float2 uv2 = uv * terrain_ubo.layer_scales.z;
  float2 uv3 = uv * terrain_ubo.layer_scales.w;

  // Sample normal maps (tangent space)
  float3 n0 = unpack_normal(layer0_normal.Sample(terrain_sampler, uv0).xyz);
  float3 n1 = unpack_normal(layer1_normal.Sample(terrain_sampler, uv1).xyz);
  float3 n2 = unpack_normal(layer2_normal.Sample(terrain_sampler, uv2).xyz);
  float3 n3 = unpack_normal(layer3_normal.Sample(terrain_sampler, uv3).xyz);

  // Apply per-layer strength
  n0 = lerp(float3(0, 0, 1), n0, terrain_ubo.normal_strengths.x);
  n1 = lerp(float3(0, 0, 1), n1, terrain_ubo.normal_strengths.y);
  n2 = lerp(float3(0, 0, 1), n2, terrain_ubo.normal_strengths.z);
  n3 = lerp(float3(0, 0, 1), n3, terrain_ubo.normal_strengths.w);

  // Blend
  float3 blended = normalize(n0 * splat.r + n1 * splat.g + n2 * splat.b + n3 * splat.a);

  // Transform to world space
  return normalize(mul(tbn, blended));
}

[shader("fragment")]
float4 fragmentMain(TerrainVertexOutput input) : SV_Target {
  // Sample splatmap (uses global terrain UVs)
  float4 splat = splatmap.Sample(splatmap_sampler, input.global_uv);

  // Renormalize weights
  float weight_sum = splat.r + splat.g + splat.b + splat.a;
  if (weight_sum > 0.001) {
    splat /= weight_sum;
  }

  // Sample blended albedo
  float4 albedo = sample_terrain_albedo(input.global_uv, splat);

  // Compute TBN matrix
  float3x3 tbn = float3x3(
    normalize(input.tangent.xyz),
    normalize(cross(input.normal, input.tangent.xyz) * input.tangent.w),
    normalize(input.normal)
  );

  // Sample blended normal
  float3 normal = sample_terrain_normal(input.global_uv, splat, tbn);

  // Standard lighting calculation (same as world shader)
  float3 view_dir = normalize(g_ubo.view_position - input.world_pos);

  float4 color = calculate_directional_light_with_shadow(
      directional_light, normal, view_dir, g_ubo.ambient_color,
      RENDER_MODE_DEFAULT, albedo, 0.0, calculate_shadow(input.world_pos, normal, input.view_depth));

  // Tone mapping
  color.rgb = color.rgb / (color.rgb + 1.0);

  return color;
}
```

---

## Frustum Culling Integration

### Per-Chunk Culling

```c
void vkr_terrain_update_visibility(VkrTerrainSystem *system,
                                    const VkrFrustum *frustum) {
  system->visible_chunks = 0;

  for (uint32_t i = 0; i < system->chunks.length; ++i) {
    VkrTerrainChunk *chunk = &system->chunks.data[i];

    // Use bounding sphere test (fast)
    chunk->visible = vkr_frustum_test_sphere(frustum,
                                              chunk->bounds_center,
                                              chunk->bounds_radius);

    if (chunk->visible) {
      system->visible_chunks++;
    }
  }
}
```

---

## Shadow Integration

### Shadow Pass

Terrain participates in the shadow pass for CSM:

```c
void vkr_terrain_render_shadow(VkrTerrainSystem *system,
                                RendererFrontend *rf,
                                uint32_t cascade_index) {
  if (!system->config.cast_shadows) return;

  // Bind shadow pipeline (depth-only)
  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry,
                                       system->terrain_shadow_pipeline, &err);

  for (uint32_t i = 0; i < system->chunks.length; ++i) {
    VkrTerrainChunk *chunk = &system->chunks.data[i];
    if (!chunk->visible) continue;

    // Use highest LOD for shadows (or LOD 0/1 for quality)
    uint32_t shadow_lod = vkr_min_u32(chunk->current_lod, 1);

    VkrTerrainChunkLOD *lod = &chunk->lods[shadow_lod];
    if (lod->geometry.id == 0) continue;

    // Push light-space transform
    Mat4 model = mat4_identity();  // Terrain is in world space already

    // Render geometry
    vkr_geometry_system_render(rf, &rf->geometry_system, lod->geometry, 1);
  }
}
```

---

## Terrain System API

### Public Interface

```c
// Initialization
bool8_t vkr_terrain_system_init(VkrTerrainSystem *system,
                                 RendererFrontend *rf,
                                 const VkrTerrainConfig *config);
void vkr_terrain_system_shutdown(VkrTerrainSystem *system,
                                  RendererFrontend *rf);

// Per-frame update
void vkr_terrain_system_update(VkrTerrainSystem *system,
                                const VkrCamera *camera,
                                const VkrFrustum *frustum);

// Rendering
void vkr_terrain_system_render(VkrTerrainSystem *system,
                                RendererFrontend *rf);
void vkr_terrain_system_render_shadow(VkrTerrainSystem *system,
                                       RendererFrontend *rf,
                                       uint32_t cascade_index);

// Queries
float32_t vkr_terrain_system_get_height_at(const VkrTerrainSystem *system,
                                            float32_t world_x,
                                            float32_t world_z);
Vec3 vkr_terrain_system_get_normal_at(const VkrTerrainSystem *system,
                                       float32_t world_x,
                                       float32_t world_z);

// Debug
void vkr_terrain_system_get_stats(const VkrTerrainSystem *system,
                                   uint32_t *out_visible_chunks,
                                   uint32_t *out_total_chunks,
                                   uint32_t *out_rendered_triangles);
```

---

## Integration Points

### World View Render Helper

Modify `lib/src/renderer/passes/vkr_pass_world.c`:

```c
void vkr_pass_world_execute(RendererFrontend *rf, uint32_t image_index,
                           float64_t delta_time) {
  (void)image_index;
  (void)delta_time;

  // ... existing mesh rendering ...

  // Render terrain
  if (rf->terrain_system.initialized) {
    vkr_terrain_system_render(&rf->terrain_system, rf);
  }
}
```

### Shadow View Render Helper

```c
// In shadow pass, after rendering shadow-casting meshes:
if (rf->terrain_system.initialized) {
  vkr_terrain_system_render_shadow(&rf->terrain_system, rf, cascade_index);
}
```

---

## File Structure

### New Files

| File | Purpose |
|------|---------|
| `lib/src/renderer/systems/vkr_terrain_system.h` | Terrain system public API |
| `lib/src/renderer/systems/vkr_terrain_system.c` | Terrain system implementation |
| `lib/src/renderer/resources/loaders/heightmap_loader.h` | Heightmap loading utilities |
| `lib/src/renderer/resources/loaders/heightmap_loader.c` | Heightmap loading implementation |
| `assets/shaders/terrain.slang` | Terrain rendering shader |
| `assets/shaders/terrain.shadercfg` | Terrain shader configuration |
| `assets/shaders/terrain_shadow.slang` | Terrain shadow pass shader |
| `assets/shaders/terrain_shadow.shadercfg` | Terrain shadow shader config |

### Modified Files

| File | Changes |
|------|---------|
| `lib/src/renderer/renderer_frontend.h` | Add `VkrTerrainSystem` member |
| `lib/src/renderer/renderer_frontend.c` | Initialize/shutdown terrain system |
| `lib/src/renderer/passes/vkr_pass_world.c` | Call terrain render |
| `lib/src/renderer/passes/vkr_pass_shadow.c` | Call terrain shadow render |

---

## Implementation Phases

### Phase 1: Basic Terrain Rendering

1. Implement heightmap loader (8-bit and 16-bit support)
2. Create terrain system structure
3. Generate single-chunk terrain mesh
4. Create basic terrain shader (single texture)
5. Integrate with world pass for rendering

**Validation**: Heightmap renders as 3D terrain with correct heights

### Phase 2: Chunking and Culling

1. Implement chunk generation with grid layout
2. Compute per-chunk bounding volumes
3. Integrate frustum culling
4. Test with large heightmaps (1024x1024+)

**Validation**: Large terrains render with proper culling

### Phase 3: LOD System

1. Generate multiple LOD levels per chunk
2. Implement distance-based LOD selection
3. Handle LOD boundaries (optional: stitching)

**Validation**: LOD transitions visible at distance thresholds

### Phase 4: Texture Splatting

1. Load splatmap texture
2. Load multiple layer textures (albedo + normal)
3. Implement splatting shader
4. Add texture tiling controls

**Validation**: Multiple terrain materials blend smoothly

### Phase 5: Shadow Integration

1. Create terrain shadow pipeline
2. Integrate with CSM system
3. Add shadow sampling to terrain shader

**Validation**: Terrain casts and receives shadows

### Phase 6: Optimization

1. Profile chunk generation
2. Add async terrain loading (optional)
3. Optimize shader (reduce texture samples)
4. Add terrain-specific GPU queries

**Validation**: Stable performance with large terrains

---

## Debug Visualization

### Wireframe Mode

```c
// Add to terrain config
bool8_t debug_wireframe;

// In pipeline creation
if (system->config.debug_wireframe) {
  pipeline_desc.polygon_mode = VKR_POLYGON_MODE_LINE;
}
```

### LOD Coloring

```hlsl
// In terrain fragment shader
static const float4 lod_colors[5] = {
  float4(0, 1, 0, 1),    // LOD 0 - Green (highest detail)
  float4(1, 1, 0, 1),    // LOD 1 - Yellow
  float4(1, 0.5, 0, 1),  // LOD 2 - Orange
  float4(1, 0, 0, 1),    // LOD 3 - Red
  float4(1, 0, 1, 1),    // LOD 4 - Magenta (lowest detail)
};

if (terrain_ubo.debug_show_lod) {
  color.rgb = lerp(color.rgb, lod_colors[input.lod_level].rgb, 0.5);
}
```

### Chunk Boundaries

```hlsl
// Highlight chunk edges
float2 chunk_uv = frac(input.global_uv * float2(chunks_x, chunks_z));
float edge = step(chunk_uv.x, 0.02) + step(chunk_uv.y, 0.02) +
             step(0.98, chunk_uv.x) + step(0.98, chunk_uv.y);
if (edge > 0 && terrain_ubo.debug_show_chunks) {
  color.rgb = float3(1, 0, 0);
}
```

---

## Performance Considerations

### Memory Budget

For a 4096x4096 heightmap with 64x64 chunks:
- Heightmap data: ~16 MB (float32)
- Chunk grid: 64x64 = 4096 chunks
- Per-chunk LOD geometry (5 levels): ~variable based on resolution
- Textures: 4 layers × 2 (albedo+normal) × resolution

### Optimization Tips

1. **Chunk size**: 64 vertices is a good balance; larger = fewer draw calls, smaller = better culling
2. **LOD distances**: Tune based on terrain scale and camera FOV
3. **Texture streaming**: For very large terrains, consider clipmaps or virtual texturing
4. **Shared index buffers**: All chunks at same LOD can share index buffer
5. **Instance rendering**: Multiple chunks with same LOD can be instanced

---

## Test Plan

### Unit Tests

1. **Heightmap loading**: Verify 8-bit, 16-bit, and RAW formats
2. **Height sampling**: Test bilinear interpolation accuracy
3. **Normal computation**: Verify normals point outward
4. **Chunk generation**: Verify vertex positions and indices

### Integration Tests

1. **Rendering**: Visual verification of terrain shape
2. **Splatting**: Verify texture blending matches splatmap
3. **LOD**: Verify LOD transitions at correct distances
4. **Culling**: Verify off-screen chunks are not rendered
5. **Shadows**: Verify terrain shadows cast/receive correctly

### Performance Tests

1. **Large heightmaps**: Test 4096x4096 and larger
2. **Many chunks**: Measure frame time with full grid visible
3. **LOD impact**: Compare performance across LOD levels

---

## References

- [GPU Gems 2: Terrain Rendering Using GPU-Based Geometry Clipmaps](https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry)
- [Real-Time Rendering Terrain Techniques](http://www.iquilezles.org/www/articles/terrainmarching/terrainmarching.htm)
- [Continuous Distance-Dependent Level of Detail for Rendering Heightmaps (CDLOD)](https://github.com/fstrugar/CDLOD)
