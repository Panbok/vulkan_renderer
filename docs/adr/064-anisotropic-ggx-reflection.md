---
status: implemented
updated: 2026-09-12
authority: adr
---

# ADR-064: Anisotropic GGX reflection

## Status

Accepted. Both native implementations, material import, offline transport and
deterministic table cooking are integrated. Available Metal checks pass; native
Vulkan execution and bilateral comparison remain unavailable on this host.

## Context

Brushed surfaces need a directional reflection lobe. The existing isotropic
GGX distribution, rectangle fit and environment convolution cannot represent
that direction. The user approved reflection transport, a bounded table and
G-buffer allocation, approximate environment and screen-space reflections, and
rejection of active anisotropy on refractive materials.

## Decision

`VkrPbrProperties` adds `anisotropy_strength` and `anisotropy_rotation`, defaulting
to zero. Strength must be finite and within [0,1]; rotation is finite radians.
Material texture slot 13 is a linear map: RG remaps to a direction in [-1,1],
and B multiplies strength. Normalize the direction before applying the scalar
counterclockwise rotation. A zero vector from filtered texels selects (1,0).
A positive strength with positive authored transmission is rejected at material
load and publication, even when a texture might suppress either value. Opaque,
cutout and nonrefractive alpha-blended materials retain their existing coverage.

The [glTF extension](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_anisotropy)
owns source units and channel semantics. The importer accepts the existing
external-image, untransformed UV0, repeat/linear/mip-linear texture subset.
Anisotropic primitives require NORMAL/TANGENT or a normal texture with UV0.
Missing normals become flat face normals before tangent generation. Authored
anisotropic tangents survive cooking. VKR flips image V on import: authored
tangent handedness and the resolved anisotropy direction's Y component are
negated together, preserving the glTF world direction. The baker retains cooked
vertex tangents for anisotropic materials instead of reconstructing their axes
from triangle UV derivatives. Project the rotated axis against the mapped normal.

With perceptual roughness `r` and strength `s`, the GGX widths are
`alpha_t = r*r + (1-r*r)*s*s` and `alpha_b = r*r`, using the existing 0.04 minimum
roughness. Direct reflection uses the anisotropic distribution and correlated
Smith visibility. Offline reflection uses matching evaluation, directional
energy allocation and visible-normal sampling. Existing clearcoat and sheen
remain outside the base GGX layer; clearcoat retains its isotropic distribution.

Three renderer-owned immutable RGBA16F array textures hold rectangle transforms
and directional DFG coefficients. Each has 64×64 texels and 64 layers: eight
strength samples times eight first-quadrant view azimuths. Total added storage is
6 MiB, independent of scene size. Existing isotropic DFG/LTC tables remain in use
at zero anisotropy and for clearcoat. Array sampling performs bilinear filtering
in roughness/view and explicit interpolation across strength/azimuth layers.

The transform encoding has positive exponential diagonals, triangular shears and
a unit support vector expressed relative to the transformed receiver plane.
Decode after interpolation. Positive diagonals preserve matrix orientation;
positive stored support Z bounds the physical hemisphere mass between FP16
texels. Rectangle integration clips both the physical receiver horizon and
the transformed cosine support. Separate rectangle Schlick A/B coefficients
preserve the old isotropic amplitudes at strength-zero knots; directional DFG
coefficients own point/IBL energy compensation. Divide rectangle amplitude by
the physical hemisphere mass. The tables approximate the angular distribution;
they do not replace exact offline BSDF evaluation.

The [deterministic cooker](../../tools/vkr_anisotropy_cooker.cpp) resamples the
author’s MIT-licensed [AnisoLTC fit](https://github.com/AakashKT/LTC-Anisotropic),
pinned at `aa4db7c54516bfd7a62a7773cd0964764c1e93c7`. The retained
[seed](../../tools/assets/anisotropy_ltc_seed.inc) includes its license.
Resampling the 8⁴ source improves lookup resolution without adding fit detail.
Strength-zero shapes use the existing isotropic LTC seed. Directional DFG uses
4096 deterministic visible-normal samples and bounds decoded A+B by one.
The [CPU sampler](../../renderer/src/vkr_anisotropy_lut.c) and native shaders
interpolate the same table coordinates.

One per-image `R8G8B8A8_UNORM` G-buffer stores octahedral world-axis direction in
RG, strength in B and zero in A. It exists only when the opaque/cutout material
aggregate contains nonzero anisotropy. The coarse feature and absent-binding
contract follows [ADR-062](062-layered-clearcoat.md). At 1280×720 this adds 10.546875 MiB for three
images, or 28.125 MiB at the supported eight-image maximum. Resolve writes binding
15; deferred lighting, SSGI composite and SSR composite read bindings 12, 10 and
12 respectively. Images use the existing graph resize and completion lifetime.

Immutable tables belong to each native renderer until shutdown after GPU drain.
Startup upload staging is retired after its submitted use completes. Metal
registers the resources for residency; Vulkan reserves three permanent sampled
image descriptors and reuses the existing clamp-linear DFG sampler. A small
per-slot table-reference record is borrowed through the frame root. Native roots
reuse their reserved field, preserving Metal's two 512-byte root cells. Material
rows are 320 bytes on Metal and 256 on Vulkan; frame roots remain 528 and 608
bytes. Table-reference records are 32 and 16 bytes respectively. Vulkan uploads
and transitions all 64 array layers before shader reads.

Probe IBL uses a bent reflection direction and one equivalent scalar roughness
mip. Its existing circular prefilter cannot reproduce an elliptical footprint.
SSR keeps its single mirror ray, existing scalar traversal/filtering and history;
clearcoat still has priority. Receiver energy and probe subtraction use the
anisotropic DFG. These are explicit approximations within the approved ray and
storage budgets.

## Consequences

Active anisotropic materials add table accesses and a full matrix polygon
integral for rectangle lighting. Zero strength selects the existing isotropic
lighting path. Cooked source tangents now matter to both runtime and offline
anisotropic reflection. Refractive anisotropy remains an input error rather than
an isotropic transmission approximation hidden behind anisotropic reflection.

## Verification and limits

Release application and production shader compilation pass with
`./build_release.sh` on Apple M1 Pro / Metal 4, with graphics validation unset.
The cooker reports unchanged output; the final data SHA256 is
`8a1ff4b8f7fa07fa0e711dc8db9d6afe5aea3392b50102f764bea49b3e3b3c97`.
The editor Release wrapper `./build_editor.sh Release` also passes; its 257×193
anisotropic layered case renders successfully. Nine affected SPIR-V modules
pass validation and compiled layout checks. This
proves compilation and ABI layout, not native Vulkan behavior.

Independent GGX/VNDF checks cover 1,738,109 valid reflection samples: maximum
analytic BRDF relative error is 1.17e-6 and sampled/quadrature integral relative
error is .000447. Input checks cover linear RG/B, rotation, V convention,
invalid materials, generated normals/tangents, and authored tangent retention
through mirrored baked-mesh decoding. A real scene bake includes anisotropic
reflection, alpha blend, nested isotropic glass and 20,000 photons; it produces
100 valid probes, 32 valid cells, 1,442 caustic deposits and finite SH values.

Conditioning checks cover all 262,144 records, 194,481 cell centers and 200,000
other interior points. Matrix determinants stay positive; the encoding proves
physical mass above .7405 for every convex interpolant. Independent area
quadrature characterizes the fit at unit radiance: 324 fixed rectangles have
maximum absolute error .021051; 192 heldout tilted rectangles have median,
90th-percentile and maximum absolute errors .001157, .004553 and .034736.
Dim responses can have relative errors above 50%. Across 108 rectangles, the
strength-zero to 1e-6 boundary changes by at most .010375 absolute or 5.44%
relative because of shape resampling. Exact zero retains the isotropic path.
Dense off-grid DFG quadrature gives maximum absolute A/B/sum errors
.020443/.042533/.023492. These measurements do not establish uniform relative
accuracy or a frame-time improvement.

Native cases use `tools/cases/local/anisotropy_*_local.case.json` with
`tools/profiles/local-brdf-display-validation.json` and the Release harness:

```sh
env -u VKR_DISPLAY_OUTPUT -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS \
  ./build_release/tools/vkr_harness snapshot --case <case> --profile <profile>
```

The scalar 90° rotation swaps highlight variance ratios from 3.80290 to .262957;
the direction map produces .263183. The existing clearcoat scene with zero
anisotropy preserves seven channels byte-for-byte, including final color; one
post-transmission HDR pixel changes by one half-float step (.00048828125).
The layered witness passes the existing material/glass/SSR checks with 682 floor
SSR hits, including 33 red hits. Previews were inspected. The isolated rectangle
capture is .424561 at the center against independent GGX area quadrature
.440796 (3.68% error). Adjacent inferred UNorm8 parameter codes bound the
reference to [.440600,.448650]; axis/roughness/strength were inferred from the
fixture’s generated tangent and default normal map, while the resolved normal
was captured. Independent integration of the decoded LTC density gives .424795
and an adjacent-code range [.424535,.431464], which contains the native value.
This characterizes the fit and does not establish a uniform tolerance. The checker is
[`check_anisotropy_rectangle.py`](../../tools/checks/check_anisotropy_rectangle.py).

One serial resize case with `MTL_DEBUG_LAYER=1`, shader validation unset, and
`local-metal-windowed-validation-serial.json` passes with no API errors. It
recreates the target at 514×386 and restores 1024×768 with SSR and SSGI enabled.
Report SHA256 is
`aa791b5d941df30c4396b98a9903d75a26787034d9933cd9d5503069f7844454`.
Native Vulkan execution, cross-backend pixels and performance remain unverified.

## Alternatives considered

An isotropic rectangle approximation loses the authored direction. Fixed sparse
area-light samples miss narrow GGX highlights. Anisotropic refraction requires
matching transmission transport and an elliptical runtime filtering policy;
that work was excluded from the approved first scope. Additional SSR rays and
history were also excluded.

## Revisit when

Revisit the fit resolution and approximation when measured rectangle or probe
errors exceed a scene's quality requirements. Add anisotropic refraction only
with an explicit transport/filtering design and budget. Native Vulkan and
bilateral evidence must pass before this domain becomes aligned under
[ADR-044](044-shader-cross-backend-contract.md).
