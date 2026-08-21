---
status: superseded
updated: 2026-08-21
authority: spec
---
# Opaque Index Compaction for MDI Megabuffer

> **Superseded.** P3 shipped the vertex/index megabuffer and P4/P5 shipped
> GPU candidate classification and compaction, which subsume this CPU-side
> opaque index compaction. See
> [ADR-028](../../../architecture/adr/028-gpu-driven-deferred-visibility-buffer.md).


> **Target**: Reduce opaque world/shadow draw overhead by building a compact
> opaque-only index buffer for merged megabuffers.
>
> **Scope**: World opaque path and opaque shadow cascades. Cutout/transparent
> paths keep their original ranges.

## Summary

Merged megabuffers can interleave opaque and alpha-tested indices. This feature
builds a dense opaque-only index buffer plus per-submesh offsets so opaque
passes can skip cutout ranges. The compacted buffer is created for merged
geometry with 32-bit indices and used only when present, otherwise the renderer
falls back to the original index buffer.

## Classification

Authoring-driven rule (same as shadow alpha-test classification):
- **Opaque** when `alpha_cutoff <= 0` or no diffuse texture is enabled.
- **Cutout** when `alpha_cutoff > 0` and diffuse texture is enabled.

## Data Layout

### VkrGeometry
- `opaque_index_buffer`: GPU index buffer containing only opaque indices.
- `opaque_index_count`: total indices stored in the opaque buffer.

### VkrSubMesh
- `opaque_first_index`: first index into `opaque_index_buffer` for this submesh.
- `opaque_index_count`: index count in `opaque_index_buffer` for this submesh.
- `opaque_vertex_offset`: vertex base (matches the original submesh range).

A zero `opaque_index_count` means the submesh has no compacted opaque range
(unmerged geometry or cutout submesh).

## Build Flow (Load Time)

1. Only merged meshes (`use_merged`) are eligible.
2. Skip compaction if the index size is not `uint32_t`.
3. Count total opaque indices across submeshes.
4. If `opaque_index_count == 0` or `opaque_index_count == total_indices`,
   skip compaction (no extra buffer needed).
5. Allocate a CPU array for opaque indices and a per-submesh range table.
6. For each opaque submesh:
   - copy its index range into the opaque array
   - record `opaque_first_index` and `opaque_index_count`
7. Upload the opaque array as `opaque_index_buffer` with the same index type.

## Runtime Flow

### World View
- `VKR_PIPELINE_DOMAIN_WORLD` uses the compacted ranges when present.
- Transparent/cutout draws use the original index buffer.
- For MDI batches, opaque compaction is only used if all submeshes in the batch
  support opaque ranges; otherwise the batch falls back to direct draws.

### Shadow View
- Opaque shadow pipeline uses compacted ranges when present.
- Alpha-tested shadow pipeline always uses the original index buffer.

## Limitations / Edge Cases

- Compaction applies only to merged meshes and 32-bit index buffers.
- Fully opaque or fully cutout meshes skip compaction entirely.
- Material reclassification requires mesh reload to rebuild ranges.
- Memory overhead: opaque indices are duplicated alongside the original buffer.

## Validation / Metrics

- Compare `shadow_draw_calls_opaque` vs `shadow_draw_calls_alpha` in the HUD.
- Verify `world` opaque draw count falls when opaque compaction is enabled.
- Ensure cutout/transparent materials still render correctly.

## Test Plan

- Load San Miguel and Sponza; confirm opaque shadow draws drop where expected.
- Validate alpha-tested materials still cast shadows via the alpha pipeline.
- Run world and shadow passes with MDI enabled/disabled and compare visuals.
