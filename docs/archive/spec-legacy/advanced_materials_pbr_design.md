---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Advanced Materials and PBR Design Specification

## Overview

This document extends the material system to support multiple textures and parameters for a Metallic-Roughness PBR workflow. It defines descriptor layout options, uniform data, file format updates, and compatibility with the current v1 single-texture path.

Related: [Material System](./material_system_design.md), [Pipeline Registry](./pipeline_registry_and_multi_pipeline_design.md), [Render Flow](./render_flow_and_state_updates_design.md).

## Architecture

```
VkrMaterial (v2)
  base_color:        TextureHandle (sRGB)
  normal:            TextureHandle (linear)
  metallic_roughness:TextureHandle (linear; B=metallic, G=roughness)
  emissive:          TextureHandle (sRGB)
  factors:           baseColorFactor (Vec4), emissiveFactor (Vec3), metallic, roughness

Descriptor Set (set 1)
  Option A: distinct bindings per texture (compact, simple)
  Option B: descriptor array; index by slot (flexible scaling)

Uniforms (aligned 256B)
  struct MaterialUniforms { Vec4 baseColorFactor; Vec3 emissiveFactor; float metallic; float roughness; }
```

## API

```c
typedef struct VkrMaterialPbrUniforms {
    Vec4 baseColorFactor;
    Vec3 emissiveFactor; float metallic;
    float roughness; float _pad[3];
} VkrMaterialPbrUniforms;

bool8_t vkr_material_set_normal(VkrMaterialSystem *sys, VkrMaterialHandle h, VkrTextureHandle tex);
bool8_t vkr_material_set_metallic_roughness(VkrMaterialSystem *sys, VkrMaterialHandle h, VkrTextureHandle tex);
bool8_t vkr_material_set_emissive(VkrMaterialSystem *sys, VkrMaterialHandle h, VkrTextureHandle tex);
bool8_t vkr_material_set_pbr_params(VkrMaterialSystem *sys, VkrMaterialHandle h,
                                    Vec4 baseColor, Vec3 emissive, float metallic, float roughness);

// Bridge: write PBR uniforms and set all texture slots
bool8_t vkr_material_apply_pbr(const VkrMaterial *material);
```

### `.mt` Material File Format (v2)

```
pipeline=world
base_color=assets/albedo.png
normal=assets/normal.png
metallic_roughness=assets/mr.png   # B=metallic, G=roughness
emissive=assets/emissive.png
base_color_factor=1,1,1,1
emissive_factor=0,0,0
metallic=0.1
roughness=0.8
```

## Design Decisions

1) Metallic-Roughness texture packing
- Rationale: Common convention (GLTF); reduces sampler count.
- Implementation: `mr.png` uses B for metallic, G for roughness.

2) Distinct bindings vs descriptor array
- Rationale: Start with fixed bindings (A) for simplicity; migrate to arrays when adding more maps.
- Implementation: Define bindings: 0 baseColor, 1 normal, 2 metallicRoughness, 3 emissive.

3) Backward compatibility
- Rationale: v1 content should render under v2 shaders with defaults.
- Implementation: Missing textures fall back to defaults; `vkr_material_apply` remains valid.

4) Color space correctness
- Rationale: sRGB for color/emissive, linear for normal/MR.
- Implementation: Ensure correct image formats and sampling conversions.

5) Alignment and padding
- Rationale: Keep uniform block at 256B; pad as needed.
- Implementation: `VkrMaterialPbrUniforms` padded appropriately.

## Usage Examples

```c
VkrMaterialHandle m = vkr_material_system_acquire(&sys, string_lit("assets/pbr/metal.mt"));
VkrMaterial *mat = vkr_material_system_get_by_handle(&sys, m);
vkr_material_apply_pbr(mat);
vkr_renderer_update_local_state(renderer, pipeline_for(mat), &model);
```

## Performance Considerations

- Prefer BC-compressed normal/albedo maps where supported.
- Share samplers across bindings to reduce descriptor count.
- Batch draws by material to minimize rebinds across multiple textures.

## Testing

- `test_mt_v2_parse` – parse v2 file with all maps.
- `test_pbr_uniforms_write` – verify uniform values written correctly.
- `test_missing_maps_defaults` – missing textures replaced with defaults.

## Revision History

- Version 1.0 (2025-10-11): Initial multi-texture and PBR specification.


