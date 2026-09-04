---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-018: Ordered transmission with declared feedback

## Status

Accepted.

## Context

Refraction must sample a completed background and preserve specular reflection.
Reading and writing the same attached color image is not a portable feedback
contract; unordered transparency cannot represent per-layer refraction.

## Decision

Use four ordered visibility peels and shade from deeper layers toward the
nearest. The graph declares separate feedback sources and outputs. Ordinary
alpha blend follows transmission with its depth-ordered direct draws. A fifth
peel is an explicit diagnostic and is absent from ordinary frames.

Both shaders implement linear radiance composition
`R + (1 - T)D + T(1 - M)(1 - F_rgb)B_tinted + E`.
Transmission replaces diffuse response; it preserves reflection and emission.
Resolve transmission/thickness textures, IOR, scaled thickness and attenuation
before composition. Zero thickness samples the original pixel. Positive
thickness reprojects the refracted exit and uses Beer attenuation along the
resolved path.

Zero roughness samples the ordered feedback chain. Positive roughness samples
an immutable six-level opaque-color pyramid at a continuous IOR-adjusted LOD.
This is a bounded approximation: frosted layers do not recursively blur all
other transmissive layers. Compact pixel lists partition thin factor-only and
extended work; production shader variants select temporal work only for layer 0.
Metal transmission indirect buffers inherit the parent peel root.

The importer treats omitted metallic/roughness factors without an ORM texture
on positive-transmission glass as dielectric/smooth; explicit factors remain
authoritative.

## Consequences

Four layers bound work and can omit deeper surfaces. Per-layer coverage and
overflow diagnostics expose that limit. Correctness and cost must be compared
with matching nonempty layer coverage; equal repeated peels are not valid
performance evidence.

## Alternatives considered

Weighted OIT does not preserve per-layer refracted backgrounds. Building a
roughness pyramid for every layer adds passes and was rejected. Increasing the
layer bound requires evidence of an unacceptable bound-specific image.

## Revisit when

A fifth-layer diagnostic exposes unacceptable loss, or accepted frosted-volume
quality requires a different feedback model.

## Implementation

[`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json),
[`transmission_kernel.slangh`](../../lib/src/renderer/shaders/shared/transmission_kernel.slangh),
[`vkr_vulkan_deferred.c`](../../lib/src/renderer/vulkan/vkr_vulkan_deferred.c), and
[`vkr_metal_packet_commands.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_commands.inc).
