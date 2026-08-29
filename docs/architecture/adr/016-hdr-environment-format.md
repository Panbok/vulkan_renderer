---
status: implemented
updated: 2026-08-29
authority: adr
---

# ADR-016: Equirectangular HDR delivery, cubemap runtime

## Status

**Accepted** — implemented on 2026-08-03. Implementation spec and evidence:
[hdr-environment-ibl-spec.md](../../rendering/hdr-environment-ibl-spec.md).

**2026-08-29 amendment:** ADR-038 replaced only the shader-facing diffuse
response cubemap with normalized L2 SH coefficients. The equirectangular
delivery, load-time source cubemap, skybox sampling, and GGX specular prefilter
decisions below remain current. References to diffuse convolution and universal
runtime cubemap sampling describe the implementation accepted in 2026-08-03 and
are historical for diffuse lighting. Vulkan keeps the ordinary cube view for
those retained filtered uses and lazily publishes a sampled 2D-array alias when
ADR-038 projection needs exact integer face/texel loads; both views share the
same source image and completion-gated lifetime.

## Context

Before this decision, the environment probe loaded six LDR JPEG cube faces
(`assets/textures/skybox_{r,l,u,d,f,b}.jpg`) through a path that hard-codes
`VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB`, commented "LDR cubemap source faces
(jpg/png)". Both IBL bake outputs are then stored as `R8G8B8A8_UNORM`.

[bistro-baseline-shading-investigation.md](../../rendering/bistro-baseline-shading-investigation.md)
established that this chain cannot preserve HDR range and identified 8-bit
linear IBL storage as the leading code-backed explanation for concentric
chromatic banding on glossy surfaces. It did not capture an isolated
format-only ablation, and the missing analytic sun independently accounted for
the baseline's directionless lighting.

Moving to HDR forces a format decision, because no HDRI is distributed as six
cube faces. Every practical source — Poly Haven, HDRI Haven, in-house captures —
ships a single equirectangular `.hdr` or `.exr`. A measured example,
`assets/textures/citrus_orchard_puresky_4k.hdr`, decodes as `4096×2048`,
aspect exactly 2.00, `FORMAT=32-bit_rle_rgbe`. Its source, CC0 license,
authors, and checksum are recorded beside the asset.

So the question is not whether to accept equirectangular input — we must — but
whether equirectangular should also be the **runtime sampling** format, replacing
the cubemap.

## Decision

**Accept equirectangular as the delivery and storage format. Convert to a
cubemap once at load. Sample cubemaps at runtime.**

Environment assets are authored and committed as a single equirectangular
`.hdr`. A bake pass projects it to a cubemap before any other IBL stage runs.
The skybox draw and specular prefilter consume `TextureCube`. The original
implementation also used one for diffuse convolution; ADR-038 now projects a
selected source-cubemap mip into GPU-resident SH coefficients instead.

Rationale, in order of weight:

1. **Texel density.** Equirectangular solid angle per texel scales as
   `1/sin(θ)` and is unbounded at the poles — a `4096×2048` source spends its
   top and bottom rows on a single direction. Cubemap density variation is
   bounded at `(√3)³ ≈ 5.2×` between face centre and corner. Matching equatorial
   density, equirect `4096×2048` = 8.4M texels against `6 × 1024²` = 6.3M for a
   cubemap: more memory, distributed worse.
2. **The azimuth seam is a real artifact.** `u = atan2(d.z, d.x)/2π + 0.5` jumps
   from ~1 to ~0 across the ±180° meridian. Hardware LOD is derived from
   `ddx/ddy(u)`, which explodes there, so the sampler selects the smallest mip
   and produces a blurred vertical line through the sky. Correcting it requires
   manual LOD or `SampleGrad` with hand-computed derivatives. Vulkan cube
   filtering is seamless across faces with no such handling.
3. **Pole pinching.** All `u` converge at `θ = 0` and `θ = π`, producing a
   pinwheel artifact directly overhead — visible in any scene where the camera
   can look up.
4. **The conversion is not avoidable by choosing equirect.** The skybox,
   specular prefilter, and current SH projection all consume a source cubemap.
   Sampling equirect at runtime would move rather than remove conversion work.
5. **Baking sidesteps the seam entirely.** The conversion pass samples with an
   explicit LOD, so the derivative-driven mip selection that causes the seam
   never runs.
6. **Cost.** Equirect lookup needs `atan2` + `acos` per fragment against a
   single hardware `TextureCube.Sample(dir)` with hardware face selection.

The conversion is cheap to build here: `vkr_world_resources_bake_cubemap()` is
already a generic per-face, per-mip driver parameterized by shader name and
source texture, using a 90° perspective per face. Equirect→cube is a third call
to it plus one small fragment shader.

## Consequences

- **Runtime directional environment sources are cubemaps.** Skybox and specular
  lighting sample them directly; diffuse lighting projects them into SH under
  ADR-038. A source format that is not projectable to a cubemap is out of scope
  without revisiting this ADR.
- **Six-face LDR loading stays** for backward compatibility with existing scenes
  and the `skybox_*.jpg` asset. `environment.cubemap` and `environment.equirect`
  are alternative scene keys.
- **A new texture format becomes public API.** `VkrTextureFormat` gains the
  required half-float entry, which is threaded through the mapping and metadata
  sites in
  `vulkan_utils.c`, `vulkan_backend.c`, `vkr_rg_compile.c`, and `vkr_rg_json.c`.
  This widens the ADR-005 reflection/manifest surface slightly.
- **A transient large allocation appears at load.** A 4K equirect at half-float
  is ~67 MB and is only needed until the bake completes. Ownership and release
  timing must be explicit; see the spec.
- **Half-float range is finite.** `R16G16B16A16_SFLOAT` saturates at 65504. A
  true-sun HDRI can exceed that range, so the loader clamps with one warning
  rather than silently overflowing to infinity.
- **This ADR does not by itself improve image quality.** Without the storage
  formats (investigation §7) and a tonemap (§4), an HDR source is clamped to 1.0
  during convolution and hard-clipped at the sRGB write. Sequencing is mandatory,
  not advisory.
- **Production activation includes a tonemap.** World and sky rendering target
  RGBA16F; render-packet version 3 carries finite, non-negative manual exposure
  (default `0.30`) and the fullscreen path applies it before an ACES-fitted
  curve into the sRGB present target. Editor and canonical HDR-capture paths use
  the same exposure and curve.

## Alternatives Considered

**Sample equirectangular directly at runtime.** Rejected on items 1–3 and 6
above. The asset-pipeline simplification it appears to offer is not real,
because item 4 means the bake shaders need rewriting either way.

**Octahedral mapping.** Near-uniform density, no poles, and a single 2D texture
whose only seam is the octahedron fold, handled with a border. Strictly better
than equirectangular for runtime sampling and the correct choice for probe
atlases. Rejected for the environment because it offers nothing over a cubemap
here while costing the same conversion, and because hardware cube filtering is
free whereas octahedral fold handling is not. Reconsider if and when a probe
atlas needs many environments in one texture.

**Offline equirect→cube conversion in `tools/`.** Would keep the runtime simpler,
but adds an asset pipeline stage, a build-time dependency, and six derived files
per environment to keep in sync with their source. Rejected because the runtime
bake reuses existing infrastructure and keeps one committed file per
environment. Revisit if load-time bake cost becomes measurable against the boot
budget.

**BC6H compressed cubemaps.** The right long-term answer for memory — a 1024²
half-float cube with mips is ~67 MB. Deferred rather than rejected: BC6H is not
in `VkrTextureFormat`, and the `.vkt`/KTX2 pipeline in ADR-012 has no HDR path.
`B10G11R11_UFLOAT_PACK32` is the cheap interim option and halves the footprint.

## Revisit When

- A probe atlas requires many environments in a single texture — octahedral
  becomes the better fit.
- Load-time bake cost shows up in boot metrics against the budget — move the
  conversion offline.
- ADR-012's `.vkt` pipeline gains an HDR path — BC6H supersedes the interim
  format choice.
- An environment source appears that cannot be projected to a cubemap.
