---
status: proposed
updated: 2026-09-12
authority: proposal
---

# Renderer feature and performance audit

The audit below describes the pinned snapshot, not the current implementation.
The requested corrections are implemented in the working tree: normal-strength
semantics and recipe v2; unclamped scene HDR fog composition and extrapolated fog
rays; an opt-in balanced SSR tier; conditional material planes; an opt-in
post-transform intermediate; cached SSGI receivers; shared layered-light
traversals; volume-first diffuse selection; and a diffuse/emission SSGI source.
Current contracts and evidence limits live in the owning ADRs linked from
[the documentation index](../../INDEX.md). The
[implementation evidence](../../../assets/verification/renderer-features/renderer-features-perf.txt)
records exact checks, local alternative evaluations and unavailable gates.

Remaining acceptance work is native bilateral comparison, RX 6700 XT execution
of this revision, and authoritative base-M1 timing/temporal quality evidence.
The high SSR and analytic post-transform defaults remain in force. The coarser
SSR tier changes fallback quality without reducing full-resolution histories.
LUT-based transforms, reflective-tile dispatch, variable ray density, shared SSR/SSGI hierarchy routing,
and different diffuse-volume coefficient storage remain profiling candidates;
none is implied by the current implementation or by a source-level load count.

## Original pinned-snapshot findings

**Several features are implemented well, but I would not sign off on the new feature set yet. I found three concrete correctness problems, plus substantial opportunities to reduce GPU work and memory consumption.**

The biggest concerns are **normal-map strength handling, two volumetric-fog errors, full-resolution SSR’s cost, and expensive work that ordinary materials now pay for even when they do not use the new features.**

I reviewed the pinned snapshot **`0fb24ee817cf89dcbcca8e568bfc2c387c9a48a2`**, including production shader helpers, Metal and Vulkan consumers, texture cooking, render-graph resources, and checked-in verification records. I also ran independent CPU arithmetic reproductions of the mathematical issues below. **I did not execute the renderer on Metal or Vulkan**; timings quoted below are your repository’s recorded observations, not new benchmarks.

## 1. Normal-map strength is applied incorrectly

**Priority: High — confirmed correctness issue.**

**Locations:** `renderer/src/shaders/shared/normal_map_kernel.slangh`, `tools/vkr_vkt_normal_roughness.h`, and `tools/assets/mesh_loader_gltf.c`.

The runtime decoder effectively does this:

```cpp
xy = decoded_xy * strength;
z = sqrt(max(0, 1 - dot(xy, xy)));
```

The new paired normal/roughness cooker follows the same order. Consequently, changing strength changes the reconstructed Z component, rather than preserving the original Z while scaling X/Y. This behavior existed in the runtime before the feature work, but the new cooker now incorporates it into generated textures.

For glTF, the intended operation is to scale the decoded normal’s X/Y components and then normalize, leaving its decoded Z unchanged before normalization. For your two-channel representation, that means **reconstructing Z before applying strength**. ([Khronos Registry][1])

### Why this matters

For an original tangent-space normal of `(0.6, 0, 0.8)`:

| Strength | Current normalized result | Expected result       | Angular error |
| -------- | ------------------------- | --------------------- | ------------: |
| 0.5      | `(0.3000, 0, 0.9539)`     | `(0.3511, 0, 0.9363)` |         3.10° |
| 1.0      | `(0.6000, 0, 0.8000)`     | Same                  |            0° |
| 2.0      | `(1.0000, 0, 0.0000)`     | `(0.8321, 0, 0.5547)` |    **33.69°** |

These are independent arithmetic calculations, not rendered measurements. At high strength, the current implementation can force normals onto the tangent plane. That changes direct highlights, environment reflections, and SSR directions substantially.

There is a second problem in the importer: it tests `normal_texture.scale != 0.0f` to distinguish an authored value from the default. This treats an explicit zero as a request for the default strength. Zero and an omitted property are not equivalent.  ([Khronos Registry][1])

### Recommended correction

Preserve your existing Y-axis convention, but change the operation order:

```cpp
float2 xy = encoded.xy * 2.0f - 1.0f;
float z = sqrt(max(0.0f, 1.0f - dot(xy, xy)));

float3 n = float3(xy * strength, z);
n.y = -n.y; // Preserve the renderer's existing convention.

float length_squared = dot(n, n);
return length_squared > 1e-12f
    ? n * rsqrt(length_squared)
    : float3(0.0f, 0.0f, 1.0f);
```

Apply the same semantics in the cooker. Distinguish a missing scale from an explicitly authored zero, bump the paired-recipe version, and regenerate affected variants.

**Acceptance test:** compare cooked and uncooked materials at strengths `0`, `0.5`, `1`, and `2`, including oblique normals and compressed normal textures. Testing only strength `1` will miss this issue.

---

## 2. Volumetric fog clamps the underlying scene’s HDR radiance

**Priority: High — confirmed correctness issue.**

**Location:** `renderer/src/shaders/shared/froxel_fog_kernel.slangh`, particularly `vkr_froxel_apply_integrated()` and `vkr_froxel_apply_local_over_feedback()`.

The fog implementation defines a 5,000-radiance cap. However, the final application function applies that cap to:

```text
scene_radiance × transmittance + fog_inscatter
```

—not just to fog-generated radiance. The function is used by the actual Metal and Vulkan composition paths.

### Observable failure

An empty medium should be an identity operation:

```text
Transmittance = 1
Inscatter     = 0
Output        = input
```

But with an input of `(12000, 6000, 1000)`, the current helper produces approximately:

```text
(5000, 2500, 416.67)
```

These are finite HDR values; this is not merely protection against NaNs or overflow.

Zero density is accepted by the feature’s validator. Its preparation and graph activation depend on the enabled volume/grid rather than requiring positive density, so this is relevant to the integrated feature, not just an unused helper.

### Recommended correction

Keep numerical safeguards for fog coefficients and accumulation, but **do not impose the fog subsystem’s working cap on unrelated scene radiance**.

The final composite should preserve the renderer’s established HDR range. An exact zero-density bypass is also worthwhile, but it is not the entire fix: a nearly transparent nonzero medium should not suddenly clamp a bright surface either.

**Acceptance test:** enable fog over emissive surfaces spanning values below and above 5,000, using zero and very small densities. Inspect raw scene-linear HDR before exposure and tonemapping. Repeat for opaque, blended, and transmitting surfaces.

---

## 3. Fog reconstruction is wrong when fog extends beyond the camera far plane

**Priority: High for affected camera configurations — confirmed range-handling issue.**

**Locations:** `vkr_froxel_world_center()`, `vkr_froxel_world_at_view_depth()`, and `vkr_froxel_fog_projection_valid()`.

The world-position helpers reconstruct the camera’s near and far points, calculate the interpolation parameter for a requested fog depth, and then **clamp that parameter to `[0,1]`**. This prevents them from producing positions beyond the camera far plane.

However, the fog validator checks that the configured fog distance exceeds the near distance; it does not enforce that fog distance stays within the camera far plane. The sky application path explicitly requests a position at the configured fog maximum.

For example:

```text
Camera near:      0.1 m
Camera far:      50.0 m
Fog maximum:    200.0 m
```

The requested 200 m position becomes a position at 50 m. Froxel samples beyond the far plane also collapse onto that plane.

That can misplace density-box and lighting evaluations and cause the sky to sample fog at the wrong depth. For context, homogeneous absorption with extinction `0.01/m` gives analytic transmittance of approximately **0.607 at 50 m versus 0.135 at 200 m**, starting at the near plane. Those numbers illustrate the scale of the discrepancy; they are not captured renderer outputs.

### Recommended correction

Construct a camera ray from two finite unprojected points and extrapolate to the requested positive view depth without clamping it to the raster far plane.

You already use a suitable approach in `select_local_lights()`: it unprojects depths `0` and `0.5`, then extrapolates to the fog range. Reusing that convention would also make selection bounds and injection positions consistent.

Alternatively, explicitly restrict the supported fog range and reject incompatible configurations. Silently evaluating a different distance is the problematic part.

**Acceptance test:** hold the medium and fog distance fixed while changing the camera far plane across that distance. Include homogeneous fog and a density box beyond the raster far plane, visible against the sky.

---

# Performance findings

## 4. Full-resolution SSR needs an explicit quality tier

**Priority: High for the M1 performance target. This is a deliberate quality/cost tradeoff, not a logic bug.**

The SSR implementation has several strong choices: incoming-radiance history, reflected-hit reprojection, separate receiver/reflected-object identities, individually validated history taps, and replacement of the corresponding probe contribution instead of additive double-counting. Those are worth preserving.

The concern is the cost of the current full-resolution configuration.

### Recorded GPU cost

Your latest full-resolution tracing record reports the following on **M1 Pro**, at **1025×577 internal resolution**, **1280×720 output**, with MetalFX:

| Measurement           | Previous tracing | Full-resolution tracing |
| --------------------- | ---------------: | ----------------------: |
| Sum of SSR pass means |         2.854 ms |            **4.952 ms** |
| Trace                 |         0.821 ms |            **2.794 ms** |
| Temporal              |         1.558 ms |            **1.683 ms** |

The record explicitly labels these local/dirty observations with unstable CPU warmup—not an authoritative benchmark. Nevertheless, they identify tracing as the dominant increase. They also do **not** establish a base-M1 frame budget or represent total elapsed GPU-frame time.

### Memory cost

The graph declares full-resolution SSR resources with these per-pixel sizes:

| Resource              | Bytes per pixel per instance |
| --------------------- | ---------------------------: |
| Raw incoming radiance |                            8 |
| Raw hit metadata      |                           16 |
| History color         |                            8 |
| History geometry      |                           16 |
| History identity      |                           16 |

That is **40 bytes per pixel for each history tuple**, before raw images, the depth pyramid, alignment, or resize overlap.

For an illustrative configuration with **five history instances and three instances of each raw image**, the logical payload is:

| Internal resolution | SSR histories + raw images |
| ------------------- | -------------------------: |
| 1280×720            |              **239.1 MiB** |
| 1920×1080           |              **537.9 MiB** |
| 2560×1440           |              **956.3 MiB** |

These are calculated capacities, not measured allocations. Actual realized instance counts matter; your recorded run realized fewer raw instances.

### What I would change

Keep the current path as a high-quality reference. Introduce cheaper tracing configurations while retaining the new correspondence and rejection logic: selective reflective-tile work, reduced ray density for rough surfaces, and lower-cost fallback policies.

Do not recover performance by dropping reflected-object identity validation or retaining unsupported reflections. Likewise, do not blindly convert history depths to FP16 without checking quantization against your rejection tolerances.

There is also a quality reason not to solve everything with more rays: the verification record identifies a pixel whose decoded G-buffer normal changes by **29.66° between sampling phases**. Increasing tracing resolution does not fix that source instability.

**My assessment:** the current SSR is a defensible quality-first implementation, but not a proven baseline setting for all supported M1 machines.

## 5. Ordinary scenes pay for three additional material buffers

**Priority: Medium–high — avoidable baseline memory and bandwidth cost.**

In `main.rendergraph.json`, these full-resolution RGBA8 images are conditioned only on `scene_rendering`:

```text
gbuffer_clearcoat
gbuffer_sheen
gbuffer_anisotropy
```

They are not conditioned on whether the scene actually contains those material features.

Together, they add **12 bytes per internal pixel per physical image instance**. With three instances, that represents approximately:

| Internal resolution | Additional logical capacity |
| ------------------- | --------------------------: |
| 1280×720            |                    31.6 MiB |
| 1920×1080           |                    71.2 MiB |
| 2560×1440           |                   126.6 MiB |

This is before accounting for consumers’ reads. Deferred lighting and the screen-space composites inspect these material planes as part of their normal execution.

### Recommended correction

Aggregate active material features at publication/extraction time and use scene-level resource/pipeline variants. A scene without sheen should not require a full-resolution sheen plane.

Start with coarse feature gating rather than a complicated per-tile material architecture. It should recover costs for common scenes without greatly complicating dispatch.

One implementation detail matters: **a 1×1 fallback is not safe for arbitrary full-resolution `Load(pixel)` accesses**. Specialize those reads away or explicitly return the default material value.

## 6. AgX and grading are evaluated repeatedly inside FXAA and sharpening

**Priority: Medium–high — strong shader optimization candidate.**

Both backends perform grading, tonemapping, and display mapping inside the helper that fetches each post-processing sample.

Consequently, the current source evaluates that chain approximately:

| Path                      | Evaluations per output pixel |
| ------------------------- | ---------------------------: |
| No FXAA, no sharpening    |                            1 |
| Sharpening without FXAA   |                            5 |
| FXAA initial neighborhood |                            9 |
| FXAA edge-processing path |                       **13** |

The Metal and Vulkan implementations follow this same structure.

This was less costly with the simple rational ACES-style curve. Your AgX path now includes gamut transformations, logarithms, a polynomial, and display linearization, so repeating the whole chain deserves profiling.

### Recommended correction

Compare two implementations against the existing reference:

**A display-linear intermediate:** tonemap once, then perform FXAA/sharpening on that image. This trades additional image traffic for less repeated arithmetic.

**A validated LUT-based transform:** replace much of the repeated analytic transform with lookup work while retaining the desired grading and HDR-output behavior.

Neither is automatically faster on both target architectures. Also, moving a nonlinear transform across filtering is **not pixel-equivalent**; assess edges and saturated highlights rather than expecting byte-identical output.

I would first measure how often FXAA is needed after each temporal mode. Do not pay the 9–13-evaluation path universally merely because it is available.

## 7. SSGI repeatedly reconstructs the same half-resolution receivers

**Priority: Medium — concrete redundant-work pattern.**

The half-resolution receiver-selection helper scans full-resolution depth and visibility, reconstructs positive depth, and selects the nearest covered source pixel.

That work is repeated in tracing, temporal filtering, and compositing. The temporal pass repeats it for its nine neighboring samples; the full-resolution composite repeats it for four half-resolution neighbors. Metal and Vulkan both follow this pattern.

On an ordinary fully covered 2×2 reduction footprint, the composite alone can inspect **16 source depth/visibility pairs per output pixel**, before its remaining reads.

That is a source-level access count, not a DRAM-transaction count; caching can reduce its external bandwidth impact.

### Recommended correction

Select each half-resolution receiver once and retain compact metadata—its source-pixel index or local offset, with the required depth information. Reuse it in later passes.

Preserve the current nearest-covered and odd-dimension policies. A compact half-resolution metadata image is a much smaller tradeoff than repeatedly reconstructing the same information throughout the pipeline.

Also evaluate sharing the current-frame depth hierarchy between SSR and SSGI where their reduction contracts match. Their traversal policies can remain different; this does not justify substituting GTAO’s differently filtered hierarchy.

## 8. Layered materials repeat light traversal

**Priority: Medium — workload-dependent optimization opportunity.**

Deferred lighting performs separate punctual-light evaluations for the base and coat, followed by a separate sheen-light evaluation. Rectangle lighting follows a similar pattern.

This makes it easy to maintain each lobe independently, but it exposes repeated light-list traversal, light-row access, position/distance calculations, attenuation, and potentially visibility work.

### Recommended correction

Evaluate the active lobes inside one traversal of each light list. Share light geometry and attenuation, and share visibility **only where the receiver-normal and bias policies genuinely match**.

Do not indiscriminately reuse the base shadow sample for a differently oriented coat normal or backlit thin surface. Those distinctions are meaningful.

The exact savings need compiled-shader inspection and measurement; separate source loops do not prove every intermediate survives optimization. Still, this is a better optimization target than broadly reducing lighting quality.

## 9. Baked diffuse-volume lookup is fetch-heavy, and fallback work is discarded

**Priority: Medium.**

`packet_diffuse_volume_terms()` explicitly accumulates **seven packed SH vectors from eight corners**: 56 vector texture loads, plus the validity lookup, for each successful volume evaluation.

The more immediate opportunity is in deferred lighting: it evaluates environment/probe diffuse lighting first, then evaluates the volume and overwrites the diffuse result when the volume is valid.

### Recommended correction

First resolve which diffuse source applies. For a valid volume cell, skip the environment diffuse calculation while retaining the required specular environment evaluation.

Then profile the volume representation. Hardware-filterable coefficient storage or a more suitable brick layout may reduce explicit interpolation work, but changes must preserve room validity, coefficient precision, and the `E/π` convention.

The 56 loads are not necessarily 896 bytes of uncached external traffic per pixel; neighboring pixels will often reuse the same probes. Avoid assuming that packing changes will provide a proportional speedup.

---

# A significant SSGI quality limitation

## 10. Its source contains camera-dependent specular lighting

**Classification: transport approximation, not a synchronization or numerical bug.**

The deferred pass writes the SSGI source from:

```text
direct diffuse
+ direct specular
+ clearcoat
+ sheen
+ emission
```

The SSGI trace then reads that source as incoming radiance at its hit.

The specular terms were evaluated toward the main camera—not toward the surface receiving the bounce. Therefore, a camera-visible highlight can become apparent diffuse bounce illumination elsewhere, and that contribution can change when only the camera moves.

The cosine-sampling estimator itself is handled sensibly: a miss remains a valid zero sample rather than being excluded from the average. The issue is the direction-dependent radiance being fed into it.

### Recommended treatment

Use **direct diffuse plus emission** as the conservative reference source. Compare it against the current implementation on a stationary glossy object, a stationary diffuse receiver, and an orbiting camera.

Supporting specular-to-diffuse paths more faithfully requires evaluating the hit’s outgoing response toward the bounce receiver, which is additional work. Treat that as an explicit quality option rather than describing the existing result as general physically correct diffuse GI.

Also note that the SSGI composite skips valid baked-volume cells. That avoids double-counting, but it also means this path does not add dynamic SSGI inside those cells. Supporting a dynamic residual there would require distinguishing already-baked sources from dynamic sources.

---

# What I would retain

There is substantial implementation work here that I would **not** replace.

| Area                                   | Assessment                                                                                                                                                                                                                                                    |
| -------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Color and cutout mips**              | Linear-light color filtering, fractional odd-dimension coverage, alpha-weighted color, and post-chain coverage adjustment address the earlier problems. The material-specific recipe identities are also the right direction.                                 |
| **Normal/roughness moments**           | Keeping unnormalized moments between reductions avoids repeatedly filtering already-normalized directions and already-broadened roughness. Retain this structure after correcting strength semantics.                                                         |
| **GGX/DFG consistency**                | The shared material-energy preparation and matching DFG conventions are coherent. The documentation correctly identifies this as a directional scaled-GGX approximation, not an exact reciprocal multiple-scattering BSDF.                                    |
| **SSR correspondence and composition** | Incoming-radiance history, reflected-hit transport, independent history validation, and subtract/replace composition are worth preserving. The in-place final composite is not inherently a race: each invocation accesses its own HDR pixel after tracing.   |
| **Directional GTAO**                   | The implementation separates scalar visibility, bent-direction handling, and indirect-light application. The neutral disabled fallback and separate treatment of baked-volume visibility are deliberate rather than indiscriminate darkening of all lighting. |
| **Fog history representation**         | Reprojecting local scattering/extinction and reintegrating along the current camera’s rays is the right architecture. Do not replace it with reprojection of already-integrated fog. Fix the range and composite errors within the existing design.           |
| **Bounded PCSS**                       | The nearest-cascade restriction, fixed blocker/filter budgets, and world-distance-to-texel conversion provide a controlled extension of the existing shadows. This is preferable to an unbounded filter everywhere.                                           |

Some limitations are deliberate feature boundaries, not bugs: rectangle lights currently lack runtime area-light shadows; froxel scattering is isotropic and uses a bounded local-light selection; coat-priority SSR leaves the base layer on probes. Those should remain visible in quality settings and documentation.

I have **not** established full correctness of the offline multibounce/photon baker, every DOF/motion-blur/surface-diffusion kernel, or native HDR presentation. Their presence and integration should not be read as a blanket pass from this review.

# Native verification remains a release gate

Your current Windows/Vulkan checklist still lists native verification of the new features as pending, including SSR’s expanded roots and histories, SSGI ordering, froxel fog, layered materials, and extended-linear output. Earlier Vulkan validation of the renderer does not cover all these subsequent changes.

For your supported hardware, I would require the following evidence before calling the feature set correct and performant:

| Gate                                 | What it needs to establish                                                                                                                                    |
| ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **New correctness fixtures**         | Normal-strength semantics; empty-fog identity over bright HDR; fog ranges crossing the camera far plane.                                                      |
| **Native RX 6700 XT execution**      | Actual feature execution, synchronization validation, resource binding, resize/cancel/reload, and matching intermediate captures—not only SPIR-V compilation. |
| **Base-M1 profiling**                | Feature-by-feature and combined costs on the minimum machine, at explicit internal/output resolutions and image/history counts.                               |
| **Temporal quality before settling** | Motion, disappearance, thin emissive objects, material boundaries, and the frames before stationary accumulation freezes the result.                          |

The SSR evidence already distinguishes partial visual improvements from flicker-free output and reports its costs honestly. Keep that standard: settled screenshots and sums of individual pass means are useful evidence, but they do not establish smooth motion or total frame-time compliance.

## Recommended work order

**First, fix normal strength and both fog errors**, with independent regression fixtures and a version bump for affected cooked normal/roughness recipes.

**Next, reduce costs paid by ordinary scenes:** conditionally allocate material planes, measure repeated post-transform evaluation, and eliminate discarded environment-diffuse work.

**Then optimize the screen-space effects:** retain SSR’s correctness improvements while adding measured quality tiers, and cache SSGI receiver selection instead of repeatedly reconstructing it.

**Finally, complete native Vulkan acceptance and base-M1 profiling before making the expensive combinations the supported baseline.**

Overall, the implementation is **quality-first and structurally thoughtful, but not yet uniformly efficient or fully verified across your target platforms**. The highest-value next step is correcting the identified math and reducing repeated work—not adding another rendering effect.

[Independent arithmetic reproductions and resource-size calculations](renderer_audit_calculations.json)

[1]: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html "https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html"
