---
status: implemented
updated: 2026-07-31
authority: design
---
# Texture Format & Color Space Design (sRGB vs Linear UNORM)

This document describes a minimal, renderer-friendly solution for choosing the **correct GPU texture format** (sRGB vs UNORM/linear) when loading images from disk, while keeping **font atlases and other data textures correct**.

It is written to be easily consumable by LLMs (Claude/GPT): each section states intent, constraints, and concrete implementation points.

---

## Problem Statement

Currently, image textures loaded from files are effectively created as **UNORM** formats (ex: `VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM`) in the texture system, regardless of their intended usage.

Consequences:
- **Color textures** (albedo/baseColor/UI images/skybox) are sampled as linear UNORM, which causes incorrect gamma handling unless shaders manually compensate.
- The renderer already contains **manual gamma correction** in at least one shader (`assets/shaders/default.world.slang` uses `pow(x, 2.2)` on diffuse samples), which will cause **double correction** if we later switch to hardware sRGB sampling without cleaning up.
- **Data textures** (fonts, normal maps, masks, MSDF/MTSDF atlases) must remain **linear**; sampling them as sRGB breaks their meaning.

Relevant code locations:
- Texture decode + format selection: `lib/src/renderer/systems/vkr_texture_system.c`
- Texture resource loader wrapper: `lib/src/renderer/resources/loaders/texture_loader.c`
- Material parsing & texture loading: `lib/src/renderer/resources/loaders/material_loader.c`
- Mesh loader `.mt` generation: `lib/src/renderer/resources/loaders/mesh_loader.c`
- Font loaders:
  - System fonts create atlas textures directly: `lib/src/renderer/resources/loaders/system_font_loader.c`
  - Bitmap fonts load atlas pages via texture resource system: `lib/src/renderer/resources/loaders/bitmap_font_loader.c`
  - MTSDF fonts load atlas via texture resource system: `lib/src/renderer/resources/loaders/mtsdf_font_loader.c`
- Vulkan format mapping is already correct:
  - `lib/src/renderer/vulkan/vulkan_utils.c` maps `VKR_TEXTURE_FORMAT_*_SRGB` to `VK_FORMAT_*_SRGB`.

---

## Goals

- **G1: Correct gamma for color textures** using GPU sRGB formats (`*_SRGB`) where appropriate.
- **G2: Keep data textures linear** (UNORM/linear), especially:
  - normal maps
  - packed masks (roughness/metalness/AO/spec masks)
  - font atlases (bitmap/system/MSDF/MTSDF)
- **G3: Do not attempt to “guess” texture intent from pixel data.**
- **G4: Keep the change minimal** and compatible with existing resource system design (string-keyed loads and caching).
- **G5: Preserve old behavior by default** unless a caller explicitly opts into sRGB.
- **G6: Avoid cache collisions** and “first-load wins” behavior when the same image is requested in different color spaces.

Non-goals (for now):
- HDR/float image formats (EXR, HDR, 16-bit PNG).
- Automatic detection of “this file is a normal map” etc. (naming heuristics can be added later if desired).

---

## Key Idea

Treat color space as **part of the resource identity** (a “texture variant”), and choose the GPU format based on **explicit intent** provided by the caller.

Concretely:
- Materials (`.mt`) declare which slots are **sRGB** vs **linear**.
- The texture system parses an optional **query suffix** on texture names to select color space, while using the **base path** for file I/O and cache lookup.

This matches the existing pattern already used by the MTSDF font loader, which parses a query string on the resource name.

---

## Proposed Data Model

### 1) “Color space” as a request parameter

Introduce a concept (implementation may be enum or simple parsing):

- `linear` (default)
- `srgb`

This maps to `VkrTextureFormat` at creation time:
- `srgb` + RGBA8 => `VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB` (or `B8G8R8A8_SRGB` if needed)
- `linear` => `VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM`

### 2) Texture resource key syntax (variant key)

Allow textures to be requested with an optional query:

- `assets/textures/foo.png?cs=srgb`
- `assets/textures/foo.png?cs=linear`

Notes:
- The **resource key** includes the query (so the hash table can hold both variants).
- The **file path for file operations** must strip the query (because `file_open`/`file_stats` cannot open a path containing `?`).
- The query may contain multiple params (ex: `?size=64&cs=srgb`); only `cs` is consumed for format selection.
- `cs` should be parsed case-insensitively; unknown values should log once and default to `linear`.
- When adding a color space to a path that already has a query, append with `&cs=...` instead of `?cs=...`.

### 3) Material file (`.mt`) extensions

Add optional keys (example naming; choose one consistent scheme):

Option A (recommended): per-slot color space keys
- `diffuse_colorspace=srgb|linear`
- `specular_colorspace=srgb|linear`
- `normal_colorspace=srgb|linear`

Option B: per-slot format keys
- `diffuse_format=srgb|unorm`
- ...

Why prefer “colorspace” over “format”:
- It expresses intent (color vs data) instead of a specific pixel encoding.
- It keeps room for future formats (BCn compression, R16F, etc.).

Default behavior if key is missing:
- **linear** (to preserve legacy behavior and protect fonts/data textures).

---

## Loading Flow (End-to-End)

### Step 1: Mesh loader generates `.mt` (optional upgrade)

`lib/src/renderer/resources/loaders/mesh_loader.c` currently emits:
- `diffuse_texture=...`
- `specular_texture=...`
- `norm_texture=...`

Upgrade (recommended defaults):
- `diffuse_colorspace=srgb`
- `specular_colorspace=linear` (specular mask is sampled as scalar in current world shader)
- `normal_colorspace=linear`

This ensures newly generated materials “do the right thing” without requiring users to hand-edit.

### Step 2: Material loader parses `.mt` and constructs variant texture names

`lib/src/renderer/resources/loaders/material_loader.c` already batch-loads texture paths.

Extend it:
- Parse `*_colorspace` keys.
- When building the texture batch list, convert:
  - `assets/textures/foo.png` + `srgb` => `assets/textures/foo.png?cs=srgb`
  - `assets/textures/foo.png` + `linear` => keep as-is (or `?cs=linear` explicitly; either is fine)
- If the texture path already contains `cs=...`, do not append another; treat it as an explicit override (optionally warn if it conflicts with `*_colorspace`).
- If the texture path has a query without `cs`, append `&cs=...` to preserve existing params.

This keeps all format decisions at the *material intent* level.

### Step 3: Texture loader/system parses query and chooses GPU format

Where to implement parsing:
- In the texture system (`lib/src/renderer/systems/vkr_texture_system.c`) so all call sites benefit.

What parsing must do:
- Given a requested resource name (possibly with `?cs=...`):
  - Compute `base_path` (strip query) for `file_stats`, `file_read_all`, stb decode.
  - Compute requested color space from query. If missing: default to `linear`.
  - Choose `VkrTextureFormat` accordingly at GPU creation time.
  - Ignore unrelated query parameters (only `cs` affects format selection).

### Step 4: Cache `.vkt` should be **color-space agnostic**

Important: sRGB vs UNORM does **not** change the decoded bytes for 8-bit PNG/JPG/TGA; it only changes how the GPU interprets samples.

Therefore the `.vkt` cache should store:
- magic, version
- source mtime
- width, height
- channels (RGBA)
- transparency flag
- raw RGBA8 bytes

And **must not be keyed by color space**.

This solves:
- “same file requested as sRGB and linear” does **not** need two disk caches.
- avoids cache collisions like `foo.png?cs=srgb.vkt`.

Implementation note:
- bump `VKR_TEXTURE_CACHE_VERSION` and remove/ignore `format` in `VkrTextureCacheHeader`.
  - If removing the field is too invasive, you can keep the field but treat it as deprecated and do not use it to decide GPU format.

### Step 5: Fonts remain correct

Fonts must never be loaded as sRGB.

Guaranteed by:
- Default texture color space is linear if no query is present.
- Font loaders should either:
  - keep using plain paths (no query) → gets linear by default, or
  - explicitly request `?cs=linear` for clarity.

Note:
- System font loader creates its atlas texture directly and already uses UNORM.
- Bitmap/MTSDF loaders load their atlases via texture resource system and should remain linear.

---

## Shader Gamma Cleanup Plan

Once diffuse/baseColor textures start sampling as sRGB (`*_SRGB`), remove any manual sRGB decode in shaders to avoid double-correction.

Current example:
- `assets/shaders/default.world.slang` applies `pow(diffuse_sample.rgb, 2.2)`.

Target:
- For sRGB textures, hardware will decode to linear automatically on sampling.
- The renderer should operate in linear space for lighting.
- The final output to an sRGB render target (swapchain/offscreen) will be encoded by the GPU when writing to an sRGB attachment format.

Migration approach:
- Phase 1: introduce new system (materials can request sRGB), but keep old materials unchanged.
- Phase 2: update selected materials to `diffuse_colorspace=srgb`, compare visual output.
- Phase 3: remove shader-side decode once the pipeline is consistently sRGB-correct.
- Verify the swapchain/offscreen target uses an sRGB format before removing any final gamma output.

---

## Compatibility and Migration

### Backwards compatibility
- Existing `.mt` files without `*_colorspace` remain **linear**, preserving current look.
- Existing fonts remain unaffected.

### Cache migration
- Bump `VKR_TEXTURE_CACHE_VERSION` so outdated `.vkt` files are invalidated cleanly.
- Ensure cache path is derived from the **base path** (query stripped).

### Resource identity
- The texture system’s hash key should remain the full request string (including query) so both:
  - `foo.png` (linear)
  - `foo.png?cs=srgb`
  can coexist as separate GPU resources.

---

## Edge Cases / Risks

- **Double gamma correction**: if a texture is sampled as sRGB and shader still does `pow(x, 2.2)` → image becomes too dark. Fix via staged migration.
- **Cache collision**: if cache path includes query suffix. Must strip query before generating `.vkt` path.
- **Loader extension parsing**: `texture_loader_can_load` must validate extension on the base path, not the full string with query.
- **Memory duplication**: the same file loaded as both linear and sRGB creates two GPU textures. This is correct and expected if both variants are used simultaneously.
- **Conflicting intent**: a material declares `*_colorspace` while the texture path already includes `cs=...`. Pick one source of truth (recommended: path wins) and optionally log a warning.

---

## Concrete Implementation Checklist (Files)

This is intentionally prescriptive so it can be executed quickly.

### Texture system
- `lib/src/renderer/systems/vkr_texture_system.c`
  - Parse `?cs=` on requested name.
  - Strip query for all file operations and for `.vkt` cache path generation.
  - Choose `VkrTextureFormat` based on requested color space at creation time.
  - Update `.vkt` cache header/version to be color-space agnostic.
  - Parse `cs` case-insensitively; default to `linear` on unknown values.

### Texture resource loader
- `lib/src/renderer/resources/loaders/texture_loader.c`
  - Update `can_load()` to strip query before checking extension.

### Material loader
- `lib/src/renderer/resources/loaders/material_loader.c`
  - Parse per-slot `*_colorspace` keys.
  - Apply suffix `?cs=srgb` to texture paths when requested.

### Mesh loader (.mt generation)
- `lib/src/renderer/resources/loaders/mesh_loader.c`
  - Emit `diffuse_colorspace=srgb` for generated materials.
  - Emit `norm_texture` as linear (default).
  - Emit `specular_colorspace=linear` (default).

### Shaders
- `assets/shaders/default.world.slang`
  - Remove manual diffuse `pow(x, 2.2)` only after diffuse maps are consistently requested as sRGB.

---

## Recommended Defaults (Policy)

If a caller can declare intent (via `.mt`), use:
- **Diffuse/BaseColor/UI images/Skybox**: `srgb`
- **Normals/Masks/Roughness/Metalness/AO/Spec masks**: `linear`
- **Fonts (bitmap/MSDF/MTSDF/system)**: `linear`

If the caller cannot declare intent:
- Default to `linear` (UNORM) to avoid breaking data textures.

---

## Test Plan (Practical)

- **Unit-ish tests (fast)**
  - Path parsing: `base_path("foo.png?cs=srgb") == "foo.png"`
  - Loader extension parsing works with query suffix.
  - Cache path generation ignores query.
  - Query parsing handles multiple params: `foo.png?size=64&cs=srgb`.
  - Unknown `cs` value falls back to `linear` and logs once.

- **Visual tests (high confidence)**
  - Pick one known diffuse texture with midtones and compare:
    - (old) UNORM + shader pow
    - (new) SRGB + no shader pow
    Expect similar or improved appearance, no darkening.
  - Normal map sanity: ensure normals look identical before/after (must remain linear).
  - Font atlas: ensure MSDF edges and bitmap alpha remain crisp (must remain linear).

---

## Summary (LLM-friendly)

We will fix texture “format correctness” by:
- making color space explicit (materials declare it),
- using a query suffix to represent texture variants (`?cs=srgb`),
- stripping that query for file I/O and caching,
- keeping `.vkt` caches independent of color space,
- and then cleaning shaders to remove manual gamma correction once sRGB sampling is used.
