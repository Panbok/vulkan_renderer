---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-028: One GPU-driven world topology

## Status

Accepted.

## Context

CPU-built world draw lists duplicate classification and make submission cost
scale with candidate count. Visibility, material evaluation and transparency
need explicit shared GPU data and graph dependencies.

## Decision

Build a bounded static-first main instance/submesh candidate stream plus a
transmission side stream, with stable identity and nonzero publication generations. Each backend packs completion-
protected GPU tables, classifies camera and cascade views, compacts state buckets
and emits native indirect commands: Metal ICBs and Vulkan indirect-count draws.
There is no CPU opaque/transmission/shadow draw fallback or legacy world selector.

Opaque/cutout raster writes visibility identity and depth. Compute resolves
geometry/material data into the G-buffer, evaluates GTAO, and shades HDR lighting.
Depth-writing visibility, shadow and picking passes use strict less-than depth
tests; depth-read-only blend and text accept equal depth. Culling sphere scale
uses the longest transformed model axis. HZB reductions include an unpaired
source row or column in the last destination texel's maximum.
The graph also builds HZB history, handles visibility-based picking, and schedules
ADR-018's four transmission peels. Compacted transmission shading retains a
fullscreen diagnostic launch shape, not a second world renderer.

Ordinary alpha blend is a narrow CPU exception: conservative camera culling and
back-to-front sorting produce feature-local direct draws. UI and text have their
own streams. Shadow candidates are not derived from the camera-culled blend list.

HZB rejection requires completed compatible history, matching world epoch and
view-projection; camera motion cannot silently relax those gates. Candidate
capacity is checked before recording. Completion-gated GPU diagnostics expose
visible/bucket/overflow/resolve-invalid counts; overflow is not permission to
silently claim a complete frame.

## Consequences

World work has one topology and one representation. Indirect submission and
visibility resolve require ABI and native GPU evidence. Exact HZB history gates
limit reuse under motion. A GPU-driven topology does not imply mesh shaders,
meshlets or automatic LOD.

## Alternatives considered

The CPU batching/MDI plan in former ADR-013 was replaced. Weighted OIT does not
preserve ordered refraction. ADR-032 declined a second visibility phase for its
measured workload.

## Revisit when

Candidate scale, deforming geometry or measured visibility cost justifies a new
classification policy without weakening visibility correctness.

## Implementation

[`vkr_visibility.c`](../../lib/src/renderer/vkr_visibility.c),
[`vkr_gpu_abi.h`](../../lib/src/renderer/vkr_gpu_abi.h),
[`vkr_vulkan_deferred.c`](../../lib/src/renderer/vulkan/vkr_vulkan_deferred.c),
[`vkr_metal_packet_commands.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_commands.inc),
and [`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json).
