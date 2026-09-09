---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-062: Layered clearcoat with coat-priority SSR

## Status

Accepted. Both native implementations and the offline baker are integrated.
Metal execution and API validation pass; native Vulkan execution remains
unavailable on the current host.

## Context

A transparent dielectric coat has its own normal and roughness. Sharing the
base normal loses that distinction, while tracing both specular lobes would
exceed the accepted single-ray SSR budget.

## Decision

Add optional clearcoat factor, roughness and normal maps to prepared PBR
materials. Factor uses R, roughness uses G, and the independent normal uses RG
with positive-Z reconstruction. Scalars are `clearcoat_factor` (default 0),
`clearcoat_roughness` (default 0), and `clearcoat_normal_scale` (default 1).
Factor and roughness must be finite in [0,1]; normal scale must be finite and
may be signed, with zero producing a flat normal. All three maps are linear.
An absent coat normal uses the interpolated surface normal before base normal
mapping. The normal is oriented toward the view for lighting.

glTF cooking accepts external images, untransformed UV0, repeat addressing and
linear/mip-linear sampling. It rejects unsupported views and incompatible unlit
or specular-glossiness materials before publishing generated materials.
The parameter semantics follow [KHR_materials_clearcoat](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_clearcoat).
This is a bounded subset, not general glTF extension support.

The coat uses fixed IOR 1.5 / F0 0.04 and [ADR-053](053-energy-compensated-ggx.md)'s
compensated correlated Smith GGX. Prepare its directional DFG reflectance once.
Base transmission is `1 - factor * coat_reflectance`. Multiply base diffuse,
specular, emission and transmitted radiance by that value, then add the factor
weighted coat specular. Each direct lobe uses its own normal and light cosine.
Opaque indirect coat lighting and SSR probe removal also use the coat normal for
GTAO cone occlusion, decoding bent direction against that normal and using the
packed coat roughness. SSR's incoming history uses filtered coat roughness.
This keeps probe removal equal to the term deferred lighting actually added.
Runtime lights, LTC and probe/global IBL share this allocation. The offline
path/photon BSDF uses the same layer and a corresponding mixture PDF. It tracks
the geometric normal separately from both mapped normals so reflection and
refraction agree with real surface crossings. Shading-normal horizon clipping
and split-sum IBL remain approximations for strongly tilted normals.
This retains ADR-053's directional approximation and is not an exact reciprocal
multiple-scattering layered BSDF. Zero factor takes the existing base path.

Coated opaque pixels use the existing SSR ray and history for the coat normal
and roughness. A valid hit replaces only coat environment specular; the base
retains probes. Misses retain both environment lobes. Uncoated pixels retain
base SSR. The existing roughness cutoff, traversal and history budgets stay in
force. Temporal validation uses the selected lobe's normal and roughness.
Material/texture publication changes the radiance revision, invalidating completed
SSR history from a different base/coat selection without another identity field.

SSGI's source includes the source surface's layered direct lighting and
attenuated emission. Its receiver composite attenuates only the receiver's
base diffuse response; SSGI does not add a coat lobe at that receiver.

## Ownership and lifetime

Material texture slots grow from eight to eleven. Immutable native material
rows own their enabled texture references until recorded/submitted GPU uses
complete. Native row layouts are independently pinned: Metal 240 bytes,
Vulkan 192 bytes. CPU material loading resolves scalar and texture intent before
publication. The bake texture store retains decoded images for the bake lifetime.

The graph owns one additional full-resolution `R8G8B8A8_UNORM` PER_IMAGE image:
factor, roughness and octahedral normal XY. Resolve writes it; deferred lighting,
SSGI composite, and SSR trace/temporal/composite read it. No extra pass is added.
At 1280×720 this adds 3.515625 MiB per image, 10.546875 MiB for three images or
28.125 MiB for eight, before native alignment. Vulkan uses one sampled and one
storage descriptor per image within its existing graph descriptor pools.
Graph generation retirement and frame completion govern image reuse.

## Consequences

Coat highlights can move independently of the base, and one additional coat
lookup/allocation is needed on coated surfaces. Coated pixels spend SSR on one
lobe; base screen-space reflections are deliberately unavailable there.
Coat normal/roughness maps are independent and do not use the base map's paired
normal-variance roughness recipe. The baker retains its existing bounded
caustic estimator and first-diffuse-receiver policy.

## Alternatives considered

Tracing both layers would need more rays and history storage. Reusing the base
normal would lose authored coat detail. Per-light Schlick base attenuation would
conflict with the existing integrated-energy allocation.

## Revisit when

Assets require transformed or alternate UV sets, custom samplers, coupled coat
mip filtering, two-lobe SSR, or a reciprocal layered transport model.

## Implementation

[Material contract](../../renderer/src/vkr_render_resources.h),
[shared layer math](../../renderer/src/shaders/shared/clearcoat_kernel.slangh),
[glTF preparation](../../tools/assets/mesh_loader_gltf.c), and
[offline BSDF](../../tools/bake/vkr_bake_bsdf.cpp).

## Verification

The production Release wrapper and actual Vulkan SPIR-V reflection validate all
four resolve variants, deferred, SSR trace/temporal/composite and SSGI composite.
Material and root offsets match independently pinned native layouts. Metal
initialization compiles the production MSL and validates its reflected roots.

A 480×480 unit-environment Metal capture checks 36 coated dielectric/metal samples
across opaque, blend and transmission paths and six roughnesses. Its maximum
absolute RGB error is 0.00048828125. The zero-coat final image is byte-identical
to the retained pre-change image; three HDR pixels differ by one FP16 step
(0.00048828125). Independent CPU integration and sampling check aligned furnace
energy, tilted/opposed normals and thin/thick glass event classification.

The 512×384 material fixture checks factor and roughness response, independent
coat normals and green light through coated glass. A floor with base roughness
1 and coat roughness .04 produces 682 SSR hits, including 33 red-emitter hits
inside the floor region; its base is ineligible for SSR. A serial Metal API
validation run with SSR and SSGI enabled completes a 1024×768 → 514×386 → 1024×768
drawable resize, with three selected target images and no API diagnostics.

A coated nested-glass bake completes 20,000 emitted photons with 1,605 caustic
deposits, 100 valid probes and 32 valid cells. All 4,032 stored SH components are
finite. Import checks reject UV1, transforms, custom samplers, embedded images,
unlit combinations and invalid factors before generated-material publication.
A direct material-loader check verifies R/G channel use, independent RG normal,
default/zero normal scale and invalid scalar/texture-intent rejection.

Reproduction uses [the material case](../../tools/cases/local/clearcoat_local.case.json),
[its checker and cooking command](../../tools/checks/check_clearcoat_fixture.py),
[the furnace checker](../../tools/checks/check_clearcoat_furnace.py), and
[the resize case](../../tools/cases/local/clearcoat_resize_local.case.json).
These are local correctness observations, not matched performance measurements
or bilateral native parity. ADR-044 remains **UNALIGNED** for this domain until
native Vulkan and same-revision pixel comparison gates pass.
