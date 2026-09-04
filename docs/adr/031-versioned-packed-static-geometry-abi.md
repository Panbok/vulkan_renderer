---
status: implemented
updated: 2026-09-05
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

## Consequences

All production static geometry consumes one branchless representation. Packed
precision must be checked per range, and stream/cooker version changes require
coordinated readers and shaders. This record claims no universal speedup.

## Alternatives considered

The 24-byte candidate lost required UV precision. Multiple runtime formats would
add per-draw selection and duplicate ABI maintenance. Unpacked vertices retain
larger memory traffic.

## Revisit when

A new geometry class cannot meet the declared quantization budget or measured
workload cost justifies a different ABI.

## Implementation

[`vkr_gpu_abi.h`](../../lib/src/renderer/vkr_gpu_abi.h),
[`vkr_packed_geometry.c`](../../lib/src/renderer/vkr_packed_geometry.c), and
[`gpu_draw.slangh`](../../lib/src/renderer/shaders/shared/gpu_draw.slangh).
