---
status: implemented
updated: 2026-08-03
authority: design
---
# PBR Material System Design (Simplified + Implementation-Accurate)

This document defines the **current** PBR material contract used by the renderer.
It replaces older draft content that described APIs/layouts not present in the codebase.

## Review Findings Fixed In This Revision

1. Removed stale API proposals (`vkr_material_system_create_pbr*`) that do not exist in `vkr_material_system.h`.
2. Corrected data model to match `vkr_resources.h` (no material union, `alpha_cutoff` and `alpha_mode` live on `VkrMaterial`, not inside `VkrPbrProperties`).
3. Corrected shader config syntax to current loader format (`uniform=type,scope,name`), replacing obsolete INI section examples.
4. Replaced obsolete texture request suffix examples (`?cs=...` only) with the actual slot-intent mechanism (`tc=...` with optional `cs=...`).
5. Removed v1 claims about IBL/environment BRDF LUT support from required scope; current runtime path is direct + ambient + emissive.
6. Added missing alpha-mode inference and texture-flag validity rules used by runtime material application.

## Scope

### In scope (v1)

1. Metallic-roughness PBR materials loaded from `.mt` and generated from glTF.
2. Coexistence with existing Phong materials in one material system.
3. Texture intent tagging (colorspace + data class) for correct GPU upload/interpretation.
4. Runtime binding via `vkr_material_system_apply_instance()` for world/UI domains.

### Out of scope (v1)

1. Image-based lighting (irradiance/prefilter cubemaps, BRDF LUT).
2. Clearcoat/transmission and full specular-glossiness shading. The glTF
   importer implements only a diffuse-compatible specular-glossiness conversion.
3. Separate PBR-only material subsystem.

## Canonical Data Model

Source of truth: `lib/src/renderer/resources/vkr_resources.h`.

```c
typedef enum VkrMaterialType {
  VKR_MATERIAL_TYPE_PHONG = 0,
  VKR_MATERIAL_TYPE_PBR = 1,
} VkrMaterialType;

typedef enum VkrMaterialAlphaMode {
  VKR_MATERIAL_ALPHA_OPAQUE = 0,
  VKR_MATERIAL_ALPHA_CUTOUT = 1,
  VKR_MATERIAL_ALPHA_BLEND = 2,
} VkrMaterialAlphaMode;

typedef struct VkrPbrProperties {
  Vec4 base_color;
  float32_t metallic;
  float32_t roughness;
  float32_t normal_scale;
  float32_t occlusion_strength;
  Vec3 emissive_factor;
} VkrPbrProperties;

typedef struct VkrMaterial {
  uint32_t id;
  uint32_t pipeline_id;
  uint32_t generation;
  const char *name;
  const char *shader_name;

  VkrMaterialType material_type;
  VkrMaterialAlphaMode alpha_mode;
  bool8_t alpha_mode_explicit;

  VkrPhongProperties phong;
  VkrPbrProperties pbr;
  float32_t alpha_cutoff;

  VkrMaterialTexture textures[VKR_TEXTURE_SLOT_COUNT];
} VkrMaterial;
```

### Design simplification

`VkrMaterial` keeps both `phong` and `pbr` payloads instead of a union. This avoids mode-dependent memory aliasing and keeps editor/loader code straightforward.

## Texture Slots and Intent

Source of truth: `lib/src/renderer/resources/vkr_resources.h`, `material_loader.c`, `vkr_material_system.c`.

| Slot | PBR meaning | Default fallback handle | Default texture class intent |
|------|-------------|-------------------------|------------------------------|
| `VKR_TEXTURE_SLOT_DIFFUSE` | Base color | Default diffuse | `color_srgb` (when colorspace is sRGB) |
| `VKR_TEXTURE_SLOT_NORMAL` | Normal map | Default normal | `normal_rg` |
| `VKR_TEXTURE_SLOT_SPECULAR` | Unused by PBR path | Default specular | `data_mask` |
| `VKR_TEXTURE_SLOT_EMISSION` | Emissive | Default diffuse | `color_srgb` (when colorspace is sRGB) |
| `VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS` | MR packed (G=roughness, B=metallic) | Default specular | `data_mask` |
| `VKR_TEXTURE_SLOT_OCCLUSION` | AO | Default diffuse | `data_mask` |

### Texture request query keys

- `tc=` or `class=`: texture semantic class (`color_srgb`, `color_linear`, `normal_rg`, `data_mask`).
- `cs=`: explicit colorspace override (`srgb` or `linear`).

`vkr_material_apply_texture_request_intent()` appends missing intent query parameters and warns on conflicts.

## Material File Contract (`.mt`)

### Minimal PBR example

```ini
name=damaged_helmet
type=pbr
shader=shader.pbr.world
pipeline=world

base_color=1.0,1.0,1.0,1.0
metallic=1.0
roughness=1.0
normal_scale=1.0
occlusion_strength=1.0
emissive_factor=0.0,0.0,0.0

alpha_mode=opaque
alpha_cutoff=0.0

base_color_texture=assets/textures/helmet_basecolor.png
base_color_colorspace=srgb
metallic_roughness_texture=assets/textures/helmet_mr.png
normal_texture=assets/textures/helmet_normal.png
occlusion_texture=assets/textures/helmet_occlusion.png
emissive_texture=assets/textures/helmet_emissive.png
emissive_colorspace=srgb
```

Authoring note: material parser defaults all texture colorspaces to `linear`; set `*_colorspace=srgb` (or explicit path query `cs=srgb`) for base-color and emissive maps.

### Supported aliases (compatibility)

- `base_color_texture` or `diffuse_texture`
- `emissive_texture` or `emission_texture`
- `normal_texture` or `norm_texture`
- `base_color` or `diffuse_color`
- `emissive_factor` or `emission_color`

### Material-type inference rules

`material_loader.c` upgrades parsed type to PBR when any of the following are present:

1. `type=pbr`
2. Any PBR scalar key (`metallic`, `roughness`, `normal_scale`, `occlusion_strength`)
3. Any PBR-only texture slot (`metallic_roughness_texture`, `occlusion_texture`, `emissive_texture`)
4. `base_color` or `emissive_factor`

## glTF -> `.mt` Mapping

Source of truth: `lib/src/renderer/resources/loaders/mesh_loader_gltf.c`.

### Field mapping

- `pbrMetallicRoughness.baseColorFactor` -> `base_color`
- `metallicFactor` -> `metallic`
- `roughnessFactor` -> `roughness`
- `normalTexture.scale` -> `normal_scale` (default 1.0)
- `occlusionTexture.strength` -> `occlusion_strength` (default 1.0)
- `emissiveFactor` -> `emissive_factor`
- glTF alpha mode -> `alpha_mode`
- glTF alpha cutoff -> `alpha_cutoff` (mask defaults to 0.5 if missing)

Legacy `KHR_materials_pbrSpecularGlossiness` compatibility maps
`diffuseFactor`/`diffuseTexture` to base color, uses `metallic = 0`, and maps
scalar glossiness to `roughness = 1 - glossiness`. Its packed RGB-specular and
alpha-glossiness texture is not compatible with the current G-roughness/
B-metallic texture contract and is intentionally not bound.

### Texture path mapping

Generated material files encode texture intent directly in path queries:

- Base color/emissive: `cs=srgb&tc=color_srgb`
- Metallic-roughness/occlusion: `tc=data_mask`
- Normal: `tc=normal_rg`

External glTF URIs retain their nested path beneath `assets/textures/` when the
source or its `.vkt` sidecar exists there; basename flattening is only a final
compatibility fallback.

## Shader Contract (`pbr.world`)

Source of truth:
- `assets/shaders/pbr.world.shadercfg`
- `assets/shaders/pbr.world.slang`

### Shader asset names

- Shader config asset path: `assets/shaders/pbr.world.shadercfg`
- Runtime shader name: `shader.pbr.world`
- Domain variants resolved by suffix when available:
  - `shader.pbr.world.transparent`
  - `shader.pbr.world.overlay`

### Instance uniform block (`LocalUniformObject`)

```c
struct LocalUniformObject {
  float4 base_color;
  float metallic;
  float roughness;
  float normal_scale;
  float occlusion_strength;
  float3 emissive_factor;
  uint32_t alpha_mode;
  float alpha_cutoff;
  uint32_t texture_flags;
  float2 _padding;
};
```

Texture flag bits:

- `0x1`: base color bound from material (not fallback)
- `0x2`: normal bound from material (not fallback)
- `0x4`: metallic-roughness bound from material (not fallback)
- `0x8`: occlusion bound from material (not fallback)
- `0x10`: emissive bound from material (not fallback)

### Global lighting inputs

`pbr.world` consumes directional + point lights from the global UBO (`dir_*`, `point_light_*`) and shadow controls (`shadow_*`).

## Runtime Apply Contract

Source of truth: `lib/src/renderer/systems/vkr_material_system.c`.

### Required behavior

1. Resolve every requested texture handle to a valid 2D texture, with slot fallback when missing/not-ready/not-2D.
2. Set PBR uniforms only if present in the active shader (`*_optional` helpers).
3. Treat texture handle validity as source-of-truth for binding; `textures[slot].enabled` is not the binding gate in current apply path.
4. Set `texture_flags` from **requested-valid-not-default** textures only.
5. Bind samplers even when using fallbacks to keep descriptors valid.
6. Always apply shadow sampler bindings for world/shadow paths.

### Why this matters

Without fallback resolution and optional uniform binding, async loading windows and shader permutations can produce invalid descriptors or missing-uniform failures.

## Alpha Contract

Source of truth: `vkr_material_system_material_alpha_mode()`.

### Explicit mode wins

If `alpha_mode_explicit` is set, runtime uses that mode, except `CUTOUT` with `alpha_cutoff <= 0` degrades to `OPAQUE`.

### Inferred mode (when not explicit)

1. If base factor alpha (`pbr.base_color.w` or `phong.diffuse_color.w`) `< 0.999`, use `BLEND`.
2. Else inspect diffuse/base-color texture transparency metadata.
3. If texture has no transparency, use `OPAQUE`.
4. If texture has transparency but `alpha_cutoff <= 0`, use `OPAQUE`.
5. If texture has alpha-mask bit and cutoff > 0, use `CUTOUT`; otherwise `BLEND`.

### Cutoff default

When parsing `alpha_mode=cutout` with missing/zero cutoff, parser applies `VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT`.

## Shader Config Packing Notes

Source of truth: `lib/src/renderer/resources/loaders/shader_loader.c`.

The loader computes UBO offsets using std140-like alignment rules and explicit array stride handling (`element_stride = align_up(size, align)`). Keep shadercfg declarations and struct field order aligned to this behavior.

## Ownership, Lifetime, and Threading

1. Material name/shader strings are copied into material-system managed memory and freed on unload.
2. Texture handles referenced by materials are released during material unload/reset.
3. Material callbacks and shader binding are render-thread responsibilities.
4. Async material parse/finalize paths must not leave partially initialized texture references.

## Simplifications Adopted In This Spec

1. No separate PBR creation API: `.mt` loading + glTF generation is the canonical creation path.
2. No new texture slots: PBR reuses the existing slot enum.
3. No alternate shadercfg syntax: only the active CSV-line format is documented.
4. No v1 IBL requirements: keep PBR shading model to implemented direct + ambient + emissive behavior.
5. One alpha policy for both Phong and PBR: shared `VkrMaterialAlphaMode` contract.

## Validation Checklist

1. Load a glTF with metallic-roughness, normal, AO, emissive textures.
2. Confirm generated `.mt` contains `type=pbr`, `shader=shader.pbr.world`, and expected texture-intent queries.
3. Verify world opaque vs transparent draw list routing matches resolved alpha mode.
4. Verify `texture_flags` only mark non-fallback textures.
5. Validate no descriptor warnings during async load/unload transitions.

## Future Extensions (Explicitly Deferred)

1. IBL (irradiance + prefiltered env + BRDF LUT).
2. KHR material extensions (clearcoat, transmission, sheen, anisotropy).
3. Optional packed material UBO compaction after profiling.

## References

- `lib/src/renderer/resources/vkr_resources.h`
- `lib/src/renderer/resources/loaders/material_loader.c`
- `lib/src/renderer/resources/loaders/mesh_loader_gltf.c`
- `lib/src/renderer/systems/vkr_material_system.c`
- `assets/shaders/pbr.world.shadercfg`
- `assets/shaders/pbr.world.slang`
