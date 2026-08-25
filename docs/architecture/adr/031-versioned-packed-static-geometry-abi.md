---
status: accepted
updated: 2026-08-25
authority: adr
---

# ADR-031: Tight packed static-geometry GPU ABI

## Status

Accepted and implemented.

## Context

`VkrVertex3d` was a shared 64-byte reflected ABI used directly by both Metal
and Vulkan geometry megabuffers. Consequently a meshoptimizer codec that
decoded back to `VkrVertex3d` reduced disk bytes only: GPU uploads and resident
geometry remained byte-for-byte equivalent to the raw payload.

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

Use a distinct, versioned static layout rather than changing `VkrVertex3d` in
place. The implemented record is eight 32-bit words, or 32 bytes:

| words | contents |
| --- | --- |
| 0 | position X/Y as UNORM16 |
| 1 | position Z as UNORM16; tangent sign, remaining bits zero |
| 2 | octahedral normal as SNORM16×2 |
| 3 | octahedral tangent as SNORM16×2 |
| 4 | UV X as float32 |
| 5 | UV Y as float32 |
| 6 | color as RGBA8 UNORM |
| 7 | reserved, zero |

Position decode uses a per-publication scale and bias in a persistent 32-byte
GPU decode record. `VkrGpuGeometryRow.decode_address` references that record;
the row also records `VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1` and a 32-byte
stride. C, Slang, native MSL, Metal reflection, and Vulkan reflection share one
ABI definition and validation contract.

The first implementation tested the proposed 24-byte record with float16 UVs.
New Sponza range 26 produced 7.8418 absolute UV error against the 0.02 budget;
large tiled authoring coordinates made float16 unsuitable. The 32-byte record
retains full float32 UVs and removes that content-dependent failure while
keeping natural scalar and record alignment.

All static geometry is normalized and packed at a cold publication boundary.
Cooked artifacts already contain the packed representation; source meshes pack
after mandatory cache/fetch optimization; procedural `VkrVertex2d` and
`VkrVertex3d` configs pack before backend publication. The Metal and Vulkan
vertex and visibility-resolve pull loops therefore decode one layout without a
per-vertex layout branch. `VkrVertex3d` remains an authoring/import and
procedural compatibility type, not a resident static-geometry path.

The cold boundary rejects non-finite decode data and UVs, invalid tangent
handedness, nonzero reserved decode fields, nonzero word 7, and unknown tangent
flag bits. Megabuffer relocation preserves the decode-record byte offset when
it replaces a buffer generation.

The packed layout still uses 32-bit indices. A future 16-bit variant requires
an explicit index type through publication and draw encoding, rather than
relying on loader input size or backend conversion.

## Consequences

- GPU uploads and vertex residency decrease only because the complete
  artifact-to-shader vertical slice ships; the reduction is not attributed to
  the meshoptimizer codec alone.
- Shader decode adds arithmetic and one publication-level metadata fetch.
  Release frame-time evidence remains backend- and workload-specific.
- The vertex stream falls from 64 to 32 bytes, a 50% reduction. With three
  32-bit indices per vertex, total raw geometry falls from 76 to 44 bytes per
  vertex-equivalent, a 42.1% reduction before megabuffer alignment and
  fragmentation. This is storage arithmetic, not a frame-time claim.
- Quantization budgets are part of version-14 `.vkb`: position relative error,
  normal/tangent angular error, UV absolute error, color error, tangent sign,
  and degenerate position ranges are validated before publication.
- The layout tag and decode record keep the ABI version explicit without
  turning each draw into a capability probe.
- A position/attribute split remains deferred. Current visibility resolve reads
  every attribute for reconstructed triangles, so another stream address would
  not avoid those reads.

## Evidence

A matched clean-worktree five-repetition Release profile completed every
repetition before and after the cutover, but both reports failed only the
warmup-stability authority gate. The results are observations, not a speed
claim:

- geometry live bytes fell from 14,132,636 to 8,715,228, a 38.3% measured
  residency reduction for the fixture;
- the packed result was 5,417,920 vertex bytes, 3,296,796 index bytes, and 512
  decode-metadata bytes with zero reported fragmentation;
- `frame.wall` mean moved from 8.526 ms to 8.425 ms, but neither timing report is
  authoritative.

The canonical Sponza snapshot completed final-color, depth, shadow, and picking
captures, and one serial Metal API plus shader-validation replay passed. Native
Vulkan runtime validation remains unavailable on the configured host; SPIR-V
compilation and shared reflection are not substitutes for that gate.

## Alternatives Considered

**Codec-only integration.** Rejected as the complete solution. It helps asset
bytes and CPU input but cannot reduce the current resident geometry ABI.

**Change `VkrVertex3d` in place.** Rejected. It would invalidate every shared
reflection assertion and make compatibility failures look like source bugs
instead of a deliberate layout transition.

**Pack each field to its smallest imaginable bit count.** Rejected. The tested
24-byte float16-UV record violated the quantization budget on production
content. The 32-byte record preserves UVs exactly, remains naturally aligned,
and still halves the vertex stream without per-vertex layout branches.

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

- Visual or performance evidence rejects the implemented precision/layout.
- Both selected implementations cannot share one decode contract.
- A split position/attribute stream wins a matched profile on representative
  targets.
- A mesh-shader/cluster renderer becomes a measured production path and changes
  the preferred storage and decode boundary.
