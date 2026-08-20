---
status: implemented
updated: 2026-08-20
authority: adr
---

# ADR-018: Graph-declared transmission feedback

## Status

**Accepted** — implemented on 2026-08-04.

## Context

glTF transmission is independent of base-color alpha. Treating
`KHR_materials_transmission` as ordinary blending loses refraction, IOR, and
volume attenuation, while sampling the active HDR attachment would create an
undefined read/write feedback loop. The former world executor also submitted
opaque and blended draws in one pass, leaving no graph-owned point at which to
preserve opaque scene color.

## Decision

Preserve transmission factor and texture, IOR, thickness and texture, and
volume attenuation as typed PBR material data. Classify a material with
non-zero transmission into its own draw list regardless of glTF `alphaMode`.

The fullscreen and editor graphs use the same declared sequence:

1. skybox and opaque draws write a pre-transmission RGBA16F image;
2. a transfer pass copies that image into the ordinary HDR scene-color target;
3. transmissive draws sample the immutable pre-transmission image while writing
   scene color; and
4. ordinary alpha-blended draws execute last, in their existing back-to-front
   order.

The transfer source/destination and sampled feedback access are explicit graph
resources. The copy executor resolves only those declared images and records a
backend image copy; it does not hide barriers or alias sampling with an active
attachment. Opaque fragment output alpha is one.

The initial transmission shader uses screen-space refraction, Fresnel-weighted
surface reflection, thickness, and Beer-Lambert attenuation.

ADR-028 P18 fired this ADR's ordered-layer revisit trigger. Both backend graphs
now own a fixed four-layer visibility/depth peel and an RGBA16F feedback image.
Every peel starts from opaque depth and rejects depths already captured by the
preceding layer. Shading runs layers 3 through 0 through an immutable background
chain. Four covers the entry/exit surfaces of both closed transmissive meshes
in the current layered witness; deeper overlap is outside the accepted bound.

## Consequences

- Fullscreen and editor paths share one material and ordering contract.
- Transmission no longer changes ordinary alpha classification or shadow
  cutout routing.
- The graph owns the extra RGBA16F pre-transmission image and full-resolution
  copy, so synchronization is inspectable and validation-testable.
- Metal and Vulkan support four explicitly ordered transmissive surfaces
  through their graph-owned feedback chains.
- Ordinary blend draws remain after transmission; they do not appear through
  the current refractive source.

## Alternatives Considered

**Map transmission to blend alpha.** Rejected because it discards the extension
semantics and cannot represent IOR or volume attenuation.

**Sample and write one HDR attachment.** Rejected because it creates an
undeclared read/write feedback hazard.

**Copy inside the world executor.** Rejected because it hides resource access
and barriers from the render graph.

## Revisit When

- Ordered or order-independent multi-layer transmission is required.
- A resolved opaque-color pyramid is added for higher-quality refraction.
- Graph pass culling can prove the feedback image and copy unnecessary for
  frames with no transmissive draws.
