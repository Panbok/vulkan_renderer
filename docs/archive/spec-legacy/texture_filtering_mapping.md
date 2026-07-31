---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Texture Filtering Mapping (Legacy Names → New Fields)

The filter enum was split into orthogonal fields (`min_filter`, `mag_filter`, `mip_filter`, `anisotropy_enable`). Use these mappings when translating legacy names or UI options:

- `NEAREST` → `min=NEAREST`, `mag=NEAREST`, `mip=NEAREST`
- `LINEAR` → `min=LINEAR`, `mag=LINEAR`, `mip=NEAREST`
- `BILINEAR` → `min=LINEAR`, `mag=LINEAR`, `mip=NONE`
- `TRILINEAR` → `min=LINEAR`, `mag=LINEAR`, `mip=LINEAR`
- `POINT_NO_MIP` → `min=NEAREST`, `mag=NEAREST`, `mip=NONE`
- `ANISOTROPIC` → `min=LINEAR`, `mag=LINEAR`, `mip=LINEAR`, `anisotropy_enable=true` (cap anisotropy at device max)
