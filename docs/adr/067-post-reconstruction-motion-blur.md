---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-067: Post-reconstruction motion blur

## Status

Accepted. Production compilation, compiled shader contracts and selected Metal
image/API checks pass. Native Vulkan execution remains unavailable.

## Decision

The user approved optional camera and rigid-object motion blur, a shutter angle
from 0 to 360 degrees, at most 32 color samples and a 16 output-pixel radius.
Transparent-covered pixels remain sharp because their motion and opaque depth
describe different surfaces. Initialization disables blur and sets a 180-degree
shutter. Zero shutter bypasses all blur resources and passes.

[Frame input](../../renderer/src/vkr_frame_input.h) version 43 adds the controls.
[Preparation](../../renderer/src/vkr_motion_blur.c) produces a 48-byte record
containing shutter fraction, perspective depth range, output-to-raster mapping
and output/tile extents. Frame delta must be finite and nonnegative. The native
roots occupy 96 bytes on Metal and 80 on Vulkan.

Motion vectors retain the existing previous-UV minus current-UV convention.
The shutter fraction is scaled by current frame duration divided by the elapsed
time of the actual transform predecessor. This handles completed predecessors
older than one frame. A committed scene clock and per-transform CPU timestamps
share the existing transform-history lifetime. Failed submissions do not commit
time or history. Cuts, reset epochs and zero duration produce zero blur.

When existing temporal selectors provide no predecessor, motion blur selects a
compatible completed transform history. It preserves scene, extent, age and GPU
last-use proofs. It does not enable camera jitter or change the predecessor
selected by TAA, SSR or SSGI.

The [graph](../../assets/render_graphs/main.rendergraph.json) executes tile maximum,
3×3 neighbor maximum and reconstruction after exposure metering and temporal
reconstruction, before depth of field and bloom. Metering and temporal color
history retain their original sources. Tile dimensions round up by 16; the tile
pass uses one 16×16 workgroup and 2 KiB of shared reduction storage per tile.
The other passes use 8×8 groups. Reconstruction includes the original color in
its 32-sample budget and preserves alpha.

Depth-ordered coverage allows moving foreground to cover background and moving
receivers to uncover farther samples. Metadata sampling rejects invalid and
transparent-covered receivers and taps conservatively. Normalized color reads
also support spatial scaling from a smaller internal image.

| Graph-owned images | Format | Extent | MiB per set at 1280×720 |
| --- | --- | --- | ---: |
| Tile and neighbor maximum | RG16F | 80×45 each | .02746582 |
| Composite | RGBA16F | 1280×720 | 7.03125 |
| Total | | | 7.05871582 |

Three sets require 21.17614746 MiB; eight require 56.46972656 MiB. Existing
per-image graph ownership, resize retirement and GPU completion govern the
images. There is no new image history. Pipelines belong to the renderer and
roots to their submission.

## Verification and limits

The Release wrapper compiles both production shader paths. Compiled SPIR-V
reflection and validation pass for all three passes, their root layouts and
dispatch sizes. An independent exposure oracle covers 11,800 shutter/history
cases with maximum radius error 1.81e-6 pixels. Directed depth coverage and
constant-color checks pass. Native fixtures pass camera and rigid-object motion, static output, zero shutter
and transparent coverage. Disabled and zero-shutter final/HDR payloads match
byte-for-byte; a static active pass preserves HDR bytes exactly. The moving
red edge's largest step falls from 4.990 to 2.794 at 180 degrees and 1.834 at
360 degrees. The transparent green edge stays sharp. A 1.849-pixel tile radius
matches the authored camera displacement despite a three-frame predecessor;
360 degrees doubles the radius to 3.697 pixels.

Odd 257×193 output, portable TAA, spatial scaling at 2/3 and MetalFX temporal
at .8 pass. A serial Metal API-validation resize passes at 514×386 and restores
1026×770 without API errors. Report SHA256:
`c29ad4469bb7bd173a7956d599ac4c6d65904063712db2f8895741020c96a381`.
The odd editor capture combines motion blur, DoF and bloom, report SHA256:
`5a3a6d9139b741b056b0fdf93fc59de287aef876bfa792cd490408eead88d03a`.
The dedicated Release editor build passes with the surface-diffusion integration:
`env -u VKR_DISPLAY_OUTPUT -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS ./build_editor.sh Release`.
Its log is `.scratch/renderer-improvements-post-effects-editor.log`.

Commands: `./build_release.sh`; `python3 tools/checks/check_motion_blur.py
.scratch/motion-blur-native-runs.json`; and `./build_release/tools/vkr_harness
snapshot --case tools/cases/local/motion_blur_180_local.case.json --profile
tools/profiles/local-brdf-display-validation.json`, with graphics validation
variables unset. The resize selects `motion_blur_resize_local.case.json`,
`local-metal-windowed-validation-serial.json` and `MTL_DEBUG_LAYER=1` with shader
validation unset. Twelve runs and their payloads are retained under
`.scratch/renderer-evidence/motion-blur`; original snapshots remain intact.

This screen-space approximation cannot recover hidden background or motion
from deformation. It deliberately preserves transparent-covered pixels.
No performance claim is made. Native Vulkan execution is unavailable on this
Mac; the feature remains UNALIGNED under
[ADR-044](044-shader-cross-backend-contract.md).
