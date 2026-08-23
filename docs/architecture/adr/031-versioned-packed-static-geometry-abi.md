---
status: proposed
updated: 2026-08-23
authority: adr
---

# ADR-031: Tight packed static-geometry GPU ABI

## Status

Proposed.

## Context

`VkrVertex3d` is a shared 64-byte reflected ABI used by Metal and Vulkan. The
Vulkan publisher allocates its geometry megabuffer with that exact stride and
expands all loaded indices to `uint32_t`. The Metal packet renderer does the
same. Consequently a meshoptimizer codec that decodes back to `VkrVertex3d`
reduces disk bytes only: its GPU uploads and resident geometry are byte-for-byte
equivalent to today's raw payload.

The existing address-based vertex-pulling path uses `VkrGpuGeometryRow` to
locate megabuffer records. The Vulkan deferred visibility resolve and Metal
G-buffer resolve each pull three full vertex records to reconstruct a visible
triangle. A smaller static record can therefore reduce resident/upload bytes
and shader geometry reads. It also adds unpack arithmetic, and cache-line and
compiler behaviour decide whether it changes frame time.

Changing the vertex layout affects shader declarations, ABI reflection,
publishers, GPU geometry rows, deferred/visibility draw consumption, capture
parity, and GPU lifetime accounting. It cannot be a loader-private stride
choice.

## Decision

Make this the third implementation step: first offline cooking, second
per-load runtime source optimization, then this packed static GPU ABI. Add a
distinct, versioned static layout rather than changing `VkrVertex3d` in place.

The initial candidate is six 32-bit words, or 24 bytes:

| words | contents |
| --- | --- |
| 0 | position X/Y as UNORM16 |
| 1 | position Z as UNORM16; tangent sign and format flags as UNORM16 |
| 2 | octahedral normal as SNORM16×2 |
| 3 | octahedral tangent as SNORM16×2 |
| 4 | UV as float16×2 |
| 5 | color as RGBA8 UNORM |

Position decode uses a per-publication scale and bias. A persistent GPU decode
record holds those values and format flags. The geometry row references that
record through an address or stable index. The row also records
`VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1` and a 24-byte stride. The exact C and
shader declarations are one reflected ABI. Use scalar-word loads; a 24-byte
stride does not give each record 16-byte vector alignment.

Both selected implementations and shared shaders must validate that layout
through the same reflected ABI checks. `VkrVertex3d` remains the compatibility
layout; callers do not infer an ABI from `vertex_stride`. `VkrGeometryConfig`'s
existing byte-size fields are insufficient as the packed-layout contract.

Partition packed and compatibility geometry into the matching pipeline/shader
path during publication or draw batching. The normal vertex and resolve pull
loops do not branch on `vertex_layout`. The layout tag validates and routes a
publication; it is not a hot-loop capability probe.

The initial packed layout still uses 32-bit indices. A future 16-bit variant
requires an explicit index type through publication and draw encoding, rather
than relying on loader input size or Vulkan's conversion path.

## Consequences

- GPU uploads and residency can decrease only after this whole vertical slice
  ships; the target is not attributed to the meshoptimizer codec alone.
- Shader decode adds arithmetic and metadata fetches. It is a trade-off that
  requires Release measurement on both bandwidth- and ALU-sensitive scenes.
- The byte target is explicit. The vertex stream falls from 64 to 24 bytes, a
  62.5% reduction. With three 32-bit indices per vertex, total raw geometry
  falls from 76 to 36 bytes per vertex-equivalent, a 52.6% reduction before
  megabuffer alignment and fragmentation. This is a storage calculation, not a
  frame-time claim.
- Quantization has explicit error budgets and visual validation. Position
  bounds, normal/tangent angular error, UV range/precision, color precision,
  tangent-sign encoding, and degenerate-range policy must be part of the
  artifact contract.
- Existing mesh assets and UI/other `VkrVertex3d` callers retain a compatible
  path during migration. There is no global in-place struct change.
- The layout tag provides an N+1 path for future skinned, position-only, or
  meshlet-oriented layouts without turning each draw into a capability probe.
- A position/attribute split is deferred. Current visibility resolve reads all
  five attributes for reconstructed triangles, so extra stream addresses do
  not avoid its reads. Revisit split streams after a profile identifies a
  position-only geometry bottleneck.

## Alternatives Considered

**Codec-only integration.** Rejected as the complete solution. It helps asset
bytes and CPU input but cannot reduce the current resident geometry ABI.

**Change `VkrVertex3d` in place.** Rejected. It would invalidate every shared
reflection assertion and make compatibility failures look like source bugs
instead of a deliberate layout transition.

**Pack each field to its smallest imaginable bit count.** Rejected. A layout
that saves a few more bytes but adds unaligned vector loads, per-vertex layout
branches, or visible quantization error loses the point of the megabuffer. The
24-byte scalar-word record is the first measured compromise, not a permanent
claim that it is universally optimal.

**Split position and attributes immediately.** Rejected for the first packed
layout. The current deferred resolves pull all attributes for a triangle, so
the split would add metadata/address reads without avoiding fetches there.

**Decompress packed data on the GPU at draw time.** Rejected for the indexed
draw path. It adds per-draw/per-dispatch work and a new GPU pipeline before a
consumer exists. Revisit only with a measured mesh-shader or streaming design.

**Add meshlets now.** Rejected. Meshlet codecs require meshlet construction,
cluster metadata, culling, and a mesh-shader/cluster draw consumer. Current
indexed geometry has none of those seams.

## Revisit When

- Visual or performance evidence rejects the proposed precision/layout.
- Both selected implementations cannot share one decode contract.
- Same-configuration profiling shows a 32-byte aligned record or split streams
  beat the 24-byte record on representative targets.
- A mesh-shader/cluster renderer becomes a measured production path and changes
  the preferred storage and decode boundary.
