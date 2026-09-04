---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-041: Stable fits and retained directional shadow cascades

## Status

Accepted.

## Context

Directional shadow quality depends on fit stability and receiver sampling.
Rerendering valid static cascades wastes work, but omission requires both a
geometric containment proof and valid retained image contents.

## Decision

Use cascaded directional depth maps with four cascades by default. CPU fitting
owns light-space orientation/anchor, texel snapping, fit hysteresis and optional
scene-bounds Z fitting clipped against each final cascade XY rectangle.

Track static/dynamic caster and publication generations. Pack static candidate
and instance rows per completion-protected slot; refresh on publication changes
and copy dynamic ranges independently. Each physical target-image cascade keeps
its submitted fit and signature. A guard-contained static cascade with valid
retained content can omit its authored pass. Dynamic overlap, generation drift,
invalid contents or incomplete publication forces rendering. Pending fits and
content validity commit only after successful submit. Reused cascades publish
the fit that actually produced their depth.

The optional proactive refresh scheduler selects low-margin cascades within a
bounded budget; the production budget is zero. ADR-033's SDSM is also opt-in.
GPU camera/cascade classification remains ADR-028's one-phase topology.

Receivers use shared progressive rotated Poisson comparison-PCF at 1/4/9/16/32
taps, an optional uniform-region early-out at high tap counts, cascade cross-fade
and distance fade. Constant/slope/normal-offset bias is authored in shadow texels
and converted using each cascade's texel size and fitted depth span. Backend
raster-bias lowering preserves those units. Cutout casters use alpha testing.

## Consequences

Fit and content retention reduce repeated work only when every reuse condition
holds. More aggressive fitting/bias/filter choices alter quality and must be
measured. Point/spot shadows and arbitrary light occlusion remain absent.

## Alternatives considered

Rendering every cascade is the safe forced-update control. Retaining allocation
without content validity is insufficient. Two-phase visibility was declined in
ADR-032; SDSM is not the default quality policy.

## Revisit when

A focused scene exposes containment, bias, transition or distance artifacts, or
matched quality/cost evidence justifies changing defaults.

## Implementation

[`vkr_shadow_system.c`](../../lib/src/renderer/systems/vkr_shadow_system.c),
[`vkr_candidate_residency.h`](../../lib/src/renderer/vkr_candidate_residency.h),
[`shadow_kernel.slangh`](../../lib/src/renderer/shaders/shared/shadow_kernel.slangh), and
[`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json).
