---
status: implemented
updated: 2026-10-05
authority: adr
---

# ADR-031: One 32-byte packed static vertex ABI

## Status

Accepted.

## Context

Static geometry is read repeatedly by visibility and material resolve. Packing
must preserve UV precision and use the same decode contract on both backends.

## Decision

Use one 32-byte `VkrPackedStaticVertex` plus a 32-byte
`VkrGpuGeometryDecodeRecord` per independently quantized range. Positions are
range-local quantized values; normals/tangents use packed encodings, while UVs
remain float32. Candidate/geometry tables carry the range decode identity.

Cooking and runtime source preparation validate quantization budgets and
file-controlled data before GPU publication. Static assertions and shared shader
decode pin the ABI. Source CPU vertices are preparation data rather than an
alternate production static draw format. ADR-030 owns cooked artifact layout,
provenance and optimization order.

The rejected 24-byte float16-UV candidate is not a selectable runtime mode.

Word 7 is zero unless the range's decode record also sets
`VKR_GPU_GEOMETRY_DECODE_LIGHTMAP_UV`; then it holds a lightmap UV pair in
[0,1] as two unorm16 values, U in the low half
([ADR-087](087-gpu-class-graphics-pipelines.md)). Validation rejects any other
flag bit and a nonzero word 7 without the flag.
`vkr_packed_geometry_set_lightmap_uv` and `vkr_packed_geometry_lightmap_uv`
pack and decode it on the CPU; `vkr_decode_packed_lightmap_uv` decodes it in
[`gpu_draw.slangh`](../../renderer/src/shaders/shared/gpu_draw.slangh) and its
Metal mirror in
[`draw.metalh`](../../renderer/src/shaders/metal/msl/common/draw.metalh). No
production shader reads it yet.

## Consequences

All production static geometry consumes one branchless representation. Packed
precision must be checked per range, and stream/cooker version changes require
coordinated readers and shaders. This record claims no universal speedup.

## Alternatives considered

The 24-byte candidate lost required UV precision. Multiple runtime formats would
add per-draw selection and duplicate ABI maintenance. Unpacked vertices retain
larger memory traffic.

A smaller record (24 bytes with range-local unorm16 UVs and without the unused
eighth word, or 16 bytes with a 32-bit tangent frame) was not built, because
buffer reads do not limit the passes it would serve. Metal GPU counters on the
M1 Pro over the production Bistro orbit (Release, `bistro_metal_production_040`
with one repetition, Instruments Metal GPU Counters for 6 s, 2026-10-03)
averaged, per pass: `VBuffer.Opaque` 9.0% buffer-read limiter, 6.8% ALU, 17.6%
last-level cache and 5.0% vertex occupancy; local shadow faces 7.0%, 5.3%,
14.5% and 3.6%; cascades 8.6%, 6.9%, 18.2% and 4.7%; `GBuffer.Resolve` 2.7%
buffer read and 46.7% ALU. No unit limits the raster passes, which matches the
per-draw cost the [meshlet proposal](../proposals/meshlet-cluster-culling.md)
measured, and the resolve leans on ALU. The 24-byte record would save about
14 MB of Bistro's 56 MB of vertices.

## Revisit when

A new geometry class cannot meet the declared quantization budget or measured
workload cost justifies a different ABI.

## Implementation

[`vkr_gpu_abi.h`](../../renderer/src/vkr_gpu_abi.h),
[`vkr_packed_geometry.c`](../../renderer/src/vkr_packed_geometry.c), and
[`gpu_draw.slangh`](../../renderer/src/shaders/shared/gpu_draw.slangh).
