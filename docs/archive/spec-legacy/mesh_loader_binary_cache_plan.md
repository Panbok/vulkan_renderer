---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Mesh Loader Binary Cache Plan

## Context
- `lib/src/renderer/resources/loaders/mesh_loader.c` parses Wavefront `.obj` files every load, allocates CPU buffers per subset, generates tangents, deduplicates vertices, and returns `VkrMeshLoaderResult` for `vkr_mesh_manager` consumption. There is no caching layer, so large meshes (e.g., sponza) reparse on every run.
- We want a tiny binary cache written after the first successful `.obj` parse and reused on subsequent loads. No invalidation is required beyond an on-disk version/name check.

## Goals
- Define a minimal, self-describing binary format that captures the loader output (subsets + geometry buffers + metadata) so a second load can skip text parsing.
- Integrate read-first/write-after-parse flow into `mesh_loader.c` without changing the loader API or the data structures it returns.
- Keep the implementation portable and arena-friendly: all allocations for read data still come from the loader allocator/arena.

## Non-Goals
- No checksum, timestamp, or dependency invalidation. If the `.obj` changes, the cache may become stale and should be manually deleted.
- No compression or streaming; just straightforward little-endian blobs.

## Binary Format Proposal (`.vkb`)
- **Location:** alongside the `.obj` using the same stem: `assets/models/foo.obj` -> `assets/models/foo.vkb`. Directory creation is already handled for materials; reuse filesystem helpers here.
- **Header (little-endian):**
  - `u32 magic`: `0x564B4D48` (`'VKMH'`) to guard against wrong files.
  - `u32 version`: start at `1`.
  - `u32 obj_name_len` + `obj_name` bytes: store the source path (relative string). If it does not match the requested load name, fall back to `.obj`.
  - `u32 subset_count`.
- **Subset record (repeated `subset_count` times):**
  - `u32 subset_name_len` + bytes (builder name or `"default"`).
  - `u32 material_path_len` + bytes (generated `.mt` path from the loader).
  - `u32 shader_override_len` + bytes (can be zero).
  - `u32 pipeline_domain`.
  - `u32 vertex_stride` (expect `sizeof(VkrVertex3d)` but stored for safety).
  - `u32 vertex_count`, `u32 index_size` (expect 4), `u32 index_count`.
  - `Vec3 center`, `Vec3 min_extents`, `Vec3 max_extents` (9 x `float32`).
  - Raw vertex bytes (`vertex_stride * vertex_count`) copied directly from `VkrVertex3d` array.
  - Raw index bytes (`index_size * index_count`) copied from `uint32_t` array.
- **Notes:** All strings are stored without null terminators. Alignment is implicit (write/read sequentially).

## Loader Flow Changes
- Add `vkr_mesh_loader_cache_path(arena, obj_path)` to compute `.vkb` path.
- Pre-load step in `vkr_mesh_loader_load`:
  - Attempt `vkr_mesh_loader_load_binary(...)`. If header magic/version match and the `obj_name` matches the requested path (case-insensitive), build `VkrMeshLoaderResult` from the binary data and return.
  - On any failure, fall back to current `.obj` parsing.
- Post-parse step:
  - After successful `.obj` parse and subset finalization, call `vkr_mesh_loader_write_binary(...)` to emit the cache. Log warning-only on write failures so mesh loads still succeed.
- Unload remains unchanged; arenas still own the CPU buffers regardless of whether they came from text or binary.

## Implementation Steps
1. **Helpers:** Add small read/write helpers for `u32`, `float`, and string blocks to `mesh_loader.c` to keep the format code compact. Use existing `file_*` APIs with `FILE_MODE_BINARY`.
2. **Cache path + constants:** Introduce magic/version constants and a cache extension string near the top of `mesh_loader.c`.
3. **Binary load path:** Implement `vkr_mesh_loader_load_binary` that:
   - Opens the `.vkb`, validates magic/version/name, and reads subset count.
   - For each subset, allocates buffers from `loader_allocator`, fills `VkrGeometryConfig`, and populates `VkrMeshLoaderSubset` (material path/shader override/pipeline domain).
   - Returns `VkrMeshLoaderResult` identical to the `.obj` path.
4. **Binary write path:** Implement `vkr_mesh_loader_write_binary` that:
   - Writes header + subsets using the data already produced by the `.obj` parser.
   - Emits vertices/indices as contiguous blocks without extra padding.
5. **Loader integration:** Update `vkr_mesh_loader_load` to try binary first, then parse, then write cache. Ensure `out_error` semantics stay the same and that failures in cache load/write do not poison the main load path.
6. **Logging:** Add concise logs for cache hits/misses/writes to aid debugging, gated behind existing logger.

## Testing & Verification
- Manual: load a known model (e.g., `assets/models/falcon.obj`) twice; verify first load logs cache write and second load logs cache hit and skips `.obj` parsing.
- Negative: corrupt the `.vkb` header/version/name and ensure loader gracefully falls back to `.obj`.
- Basic perf sanity: load a large `.obj` (e.g., sponza) and confirm second load is noticeably faster (at least in log timing).
