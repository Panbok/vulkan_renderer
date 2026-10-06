---
status: implemented
updated: 2026-10-06
authority: adr
---

# ADR-065: Thin-sheet diffuse transmission

## Status

Accepted. Desktop-pipeline lighting on Vulkan, material publication and
offline transport are integrated. The tiled pipeline does not draw diffuse
transmission yet ([ADR-087](087-gpu-class-graphics-pipelines.md), decision 8).
The native output checks below ran on the Metal desktop implementation,
removed on 2026-10-06; native Vulkan execution is not yet recorded.

## Context

Leaves and thin cloth need backlighting without refractive background sampling.
The user approved material-wide strength/tint, direct-light backlighting,
matching offline transport, opaque/cutout surfaces without refraction, and no
new graph images. Spatial diffusion for skin or wax is outside this scope.

## Decision

The [PBR material](../../renderer/src/vkr_render_resources.h) adds
`diffuse_transmission_strength` in [0,1] and linear RGB
`diffuse_transmission_color` in [0,1], defaulting to zero strength and white tint.
The same keys are authored in `.mt` files. They select PBR; a later explicit
Phong type is rejected when strength is positive. This is a VKR material subset,
not a glTF extension implementation. Texture controls are unsupported.

At material loading and publication, reject nonfinite/out-of-range controls.
Positive authored strength rejects blended alpha, refractive transmission and
positive thickness, even when another factor would suppress the lobe. Existing
opaque/cutout coverage and authored sidedness remain in force.

Partition the existing residual base diffuse response into front reflection
weighted by `1-strength` and opposite-hemisphere Lambert transmission weighted
by `strength*tint`. Both use base albedo, metallic suppression, directional GGX
residual energy and the existing clearcoat/sheen allocation. Tint below white
absorbs energy from the transmitted fraction. Specular reflection and emission
retain their existing treatment. Exact zero takes the existing shading path.

Runtime transmission receives direct sun, punctual and rectangle light only.
The front reflection fraction also applies to global/probe diffuse, baked-volume
receivers and SSGI. No backside environment or screen-space query is added.
SSGI’s direct source includes visible backlighting. Punctual and sun shadows use
light-facing receiver bias for the backside lobe; independently oriented coats
retain their existing shadow query when needed. Rectangles use the existing
unshadowed runtime policy and a Lambert integral over the flipped hemisphere.

The [offline BSDF](../../tools/bake/vkr_bake_bsdf.cpp) evaluates and cosine-samples
the same front/back split. A diffuse crossing has unit eta ratio, is not delta,
and changes no medium stack. NEE includes the backside lobe. Diffuse crossings
do not create a caustic chain; caustic photons incident on the opposite side of
an active sheet can contribute through the BSDF. Shadow connections through a
sheet remain blocked because diffuse scattering cannot preserve a straight
light connection. The offline integrator can propagate multiple diffuse bounces;
the runtime’s direct-only backside policy remains an explicit approximation.

The material row appends one float4: RGB tint and W strength. Metal rows grow
from 320 to 336 bytes; Vulkan rows grow from 256 to 272. Existing material-table
owners, publication generations and GPU retirement govern the extra 16 bytes
per material per retained copy. No texture slots, images, history or passes are
added. Deferred and SSGI composite borrow the existing visible-draw buffer and
look up material through the visibility image. Their graph buffer bindings are
13 and 11 in both fullscreen and editor paths. On Vulkan the deferred root is
192 bytes and the SSGI composite root 432 bytes. Frame roots and their two-cell
Metal allocation remain unchanged.

## Consequences

Thin surfaces gain tinted backlighting with bounded diffuse energy. Runtime
indirect backside lighting and spatial transport inside a solid are absent.
Material-table reads increase in deferred lighting and SSGI composite; no frame
time improvement is claimed. A backside light on a coated sheet may require a
second shadow query to preserve the independent coat’s original bias.

## Verification and limits

Nine affected SPIR-V modules pass compiled layout checks and validation.

Independent CPU checks find maximum Lambert error 8.55e-9, sampled/quadrature
error .000500, identical sample/evaluation PDFs and 1,029,446 non-delta
unit-eta crossings. 131,072 zero-strength samples are bit-identical to prior
production, including glass. Actual integrator NEE and opposite-side photon
gathering agree with independent Lambert values within 8.94e-9. Input checks
cover defaults, linear tint, opaque/cutout, type ordering and invalid
range/media/map inputs. Runtime rejection also covers uppercase unsupported map
keys, matching the loader's case-insensitive key handling; the final editor
build includes this fix. Prepare the source fixtures with `python3
tools/checks/prepare_diffuse_sheet.py`; the script cooks flat-sheet geometry
and applies the authored custom material fields. Pack its
`tests/fixtures/rendering/diffuse_sheet` texture folder before the cutout case.
Native captures use this command with the respective case names, which are now
pinned to Vulkan:

```sh
env -u VKR_DISPLAY_OUTPUT -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS \
  ./build_release/tools/vkr_harness snapshot \
  --case tools/cases/local/diffuse_sheet_back_local.case.json \
  --profile tools/profiles/local-brdf-display-validation.json
```

On the Metal desktop implementation, removed on 2026-10-06, the `back`,
`front`, `off`, `black`, `shadow`, `rectangle`, `sun` and `cutout` cases passed
the independent
[`check_diffuse_sheet.py`](../../tools/checks/check_diffuse_sheet.py) oracle:
maximum absolute point-light error .000317, rectangle error .000138, sun error
.000073. Black tint gives zero response; a blocker removes backlighting and
cutout holes remain uncovered. The rectangle reference uses independent emitter
area quadrature, with 128²→256² convergence delta 2.73e-6. These are local
numeric checks, not a performance measurement or universal scene-quality
guarantee.

On the same implementation, the layered SSR/SSGI scene passed material/glass
checks with 682 floor SSR hits, including 33 red hits. The existing clearcoat
scene with zero sheet strength preserved all eight captured channels
byte-for-byte against the preceding anisotropy implementation. Its report
SHA256 is `23fb0a613cac83f71f17182dadf222aa7d089bb1b56daa7a795777ce4c3ba324`.

A real scene bake combines the thin sheet, anisotropy, blended surfaces, nested
glass and 20,000 photons: 100 valid probes, 30 valid cells, 1,550 caustic deposits
and 4,032 finite SH floats. The 17,116-byte cooked output SHA256 is
`62ec80db491e8b89bfcb294441ae9b0d13f31a03fe2061b4d2c2f50bc3cf8c67`.
Native Vulkan execution remains open.

## Alternatives considered

A new per-pixel G-buffer is unnecessary for material-wide controls. Refractive
background sampling does not model diffuse transmission. Screen-space diffusion
and a volumetric/BSSRDF offline model require separate authoring, storage,
visibility and transport decisions.

## Revisit when

Add mapped controls, refractive combinations or spatial diffusion only after
settling their material, transport and resource contracts. Revisit direct-only
runtime backlighting when assets require directional indirect transmission.
