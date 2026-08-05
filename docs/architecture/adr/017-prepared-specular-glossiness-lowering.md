---
status: implemented
updated: 2026-08-04
authority: adr
---

# ADR-017: Prepared specular-glossiness lowering

## Status

**Accepted** — implemented on 2026-08-04.

## Context

VKR's runtime PBR material contract is metallic-roughness. The legacy glTF
`KHR_materials_pbrSpecularGlossiness` extension carries diffuse RGBA plus
specular RGB and glossiness alpha, so the former compatibility import discarded
the specular/glossiness texture and specular factor. Bistro contains 234 such
materials and 109 packed specular/glossiness textures.

Keeping both workflows at runtime would add descriptor and pipeline variants to
the draw path. Converting only the factors is insufficient because the authored
workflow is per texel. The conversion therefore has to happen before a material
becomes renderable.

## Decision

Lower specular-glossiness to the metallic-roughness runtime path in the
prepared glTF import stage while retaining authored dielectric reflectance.

- Decode diffuse and specular RGB from sRGB before conversion. Diffuse alpha and
  glossiness alpha remain linear.
- Bake both texture samples and their material factors into generated images.
  Base color is stored as sRGB RGBA. Metallic-roughness is stored as a linear
  data mask with roughness in G and metallic in B. When the source has a packed
  specular/glossiness texture, retain converted dielectric F0 in a third linear
  RGB companion; factor-only materials use a `dielectric_specular` uniform and
  do not create or sample that companion.
- Use the Khronos reference conversion shape with dielectric F0 `0.04`, solving
  metallic from perceived diffuse/specular brightness and reconstructing base
  color from both workflows. Per the reference algorithm, perceived specular
  below dielectric F0 is explicitly non-metallic; the quadratic solve must not
  turn dark, zero-specular texels into conductors.
- Derive grazing reflectance as `F90 = saturate(max(F0) * 25)` for direct and
  split-sum IBL Fresnel. The legacy workflow does not imply white F90 when
  authored F0 is zero; doing so recreated a circular, camera-moving environment
  highlight on Bistro's zero-specular Lumberyard sign.
- When source dimensions differ, evaluate both images at output texel centers
  using the authored wrap/filter sampler and the larger extent. Different UV
  sets and `KHR_texture_transform` are rejected instead of silently baking the
  wrong correspondence.
- Publish every required image and generated material file atomically under
  `assets/textures/generated/gltf_sg<version>_<source-path-hash>/`. A pair is
  reused only when its required outputs are at least as new as the glTF and
  source images. The corrected Khronos edge condition uses generated namespace
  version 2. The optional companion is named
  `material_<index>_dielectric_specular_v1.png`; an interrupted upgrade can
  derive just that missing image from the current metal/rough output and source
  specular texture instead of rewriting the existing pair. Mesh-cache version
  12 invalidates stale material references and zero/truncated publications.
- Generated images are persistent derived cache assets, not scene-owned runtime
  allocations. Scene unload releases the ordinary texture/material handles but
  does not delete the cache. Test fixtures and cache-maintenance tools may remove
  a known generated namespace after proving that no scene owns it.

Factor-only materials are converted numerically without an extra image. The
runtime remains one PBR shader/material path rather than a separate
specular-glossiness pipeline variant.

## Consequences

- Legacy glTF materials retain per-texel diffuse, dielectric F0, and glossiness
  without adding a fragment workflow branch. Zero authored specular remains
  zero at both normal and grazing angles.
- First import can be expensive for large assets. Each output and material file
  is atomic and independently reusable, so an interrupted import resumes rather
  than restarting all conversions. The ordinary offline texture packer may
  then create `.vkt` sidecars through ADR-012.
- Textured legacy materials consume one additional sampled image. The matched
  local/dirty 3200×2400 Release observation recorded the opaque GPU pass at
  93.27 ms p50 versus 87.32 ms before the complete lighting/material change;
  the fingerprints differ, so this is diagnostic evidence rather than an
  isolated or authoritative regression claim.
- Generated cache storage can be substantially larger than the source packed
  texture, especially for the 109 materials that need the F0 companion. It is
  intentionally outside scene lifetime and requires explicit cache pruning
  rather than deletion during unload.
- Mtime invalidation follows the existing mesh dependency cache. Content-hash
  manifests used by renderer evidence are a separate workload-identity concern
  and do not replace the local derived-cache version.
- Conversion tests include Khronos reference edge vectors and representative
  Bistro room texels with zero authored specular. These remain dielectric with
  metallic `0`, rather than acquiring near-unity metallic from a dark diffuse
  sample.
- Embedded images and `.vkt`-only sources remain unsupported by this prepared
  converter because their source texels are unavailable at the importer seam.

## Alternatives Considered

**Runtime specular-glossiness shader variant.** Rejected because it widens the
material descriptor/pipeline surface and keeps a legacy workflow in the
fragment path.

**Factor-only approximation.** Rejected because it discards the extension's
principal per-texel data and caused the Bistro loss diagnosed by the shading
investigation.

**Require authors to convert assets manually.** Rejected because imported glTF
must be deterministic and self-contained from the source asset.

## Revisit When

- A project-wide asset cooker replaces prepared runtime import.
- The texture pipeline gains a direct HDR/high-precision material conversion
  target that materially reduces quantization or cache size.
- glTF removes the legacy extension from all supported content.
