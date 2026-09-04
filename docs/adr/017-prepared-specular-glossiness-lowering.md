---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-017: Prepare PBR materials before publication

## Status

Accepted.

## Context

glTF specular-glossiness inputs must coexist with the metallic-roughness runtime
without branching on source workflow in every fragment.

## Decision

Lower specular-glossiness factors and textures during CPU preparation into the
runtime PBR representation, retaining dielectric specular response instead of
replacing it with a universal 0.04 value. Generated derivatives are cacheable.
The runtime carries metallic/roughness, base color, normal, occlusion, emissive,
alpha and transmission/volume data through immutable GPU material publications.

Color textures request sRGB interpretation; numerical maps request linear data.
Texture identity includes semantic/format intent. Tangent-space normal decoding
uses the shared positive-Z reconstruction. Explicit alpha mode controls opaque,
cutout or ordinary-blend routing; transmissive surfaces use ADR-018's peel path.

Materials can publish semantic defaults before texture residency completes.
Ready textures republish a replacement row and retire the old generation after
GPU use. No frontend `.shadercfg`, named-uniform staging or instance descriptor
system participates in the current material path.

## Consequences

Runtime shading consumes one prepared contract. Preparation owns source-workflow
conversion and derivative provenance. Clearcoat and sheen are absent; imported
material support is bounded by implemented loader and shader fields.

## Alternatives considered

A shader branch for each source material model duplicates runtime work. Dropping
dielectric reflectance changes authored non-metal response.

## Revisit when

A source extension cannot lower without losing accepted appearance, or a new
material lobe is authorized.

## Implementation

[`vkr_gltf_material_conversion.c`](../../lib/src/renderer/resources/loaders/vkr_gltf_material_conversion.c),
[`mesh_loader_gltf.c`](../../lib/src/renderer/resources/loaders/mesh_loader_gltf.c),
[`vkr_material_system.c`](../../lib/src/renderer/systems/vkr_material_system.c), and
[`normal_map_kernel.slangh`](../../lib/src/renderer/shaders/shared/normal_map_kernel.slangh).
