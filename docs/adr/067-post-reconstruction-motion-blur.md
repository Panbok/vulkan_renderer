---
status: implemented
updated: 2026-10-06
authority: adr
---

# ADR-067: Post-reconstruction motion blur

## Status

Accepted. Motion blur belongs to the desktop pipeline, and Vulkan is its only
implementation; the tiled pipeline turns it off
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 7). Production
compilation and compiled shader contracts pass. The native image checks below
ran on the Metal desktop implementation, removed on 2026-10-06; native Vulkan
execution is not yet recorded.

## Decision

The user approved optional camera and rigid-object motion blur, a shutter angle
from 0 to 360 degrees, at most 32 color samples and a 16 output-pixel radius.
Transparent-covered pixels remain sharp because their motion and opaque depth
describe different surfaces. Initialization disables blur and sets a 180-degree
shutter. Zero shutter bypasses all blur resources and passes.

[Frame input](../../renderer/src/vkr_frame_input.h) version 43 adds the controls.
[Preparation](../../renderer/src/vkr_motion_blur.c) produces a 48-byte record
containing shutter fraction, perspective depth range, output-to-raster mapping
and output/tile extents. Frame delta must be finite and nonnegative. The Vulkan
root occupies 80 bytes.

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

The Release wrapper compiles the production shaders. Compiled SPIR-V
reflection and validation pass for all three passes, their root layouts and
dispatch sizes. An independent exposure oracle covers 11,800 shutter/history
cases with maximum radius error 1.81e-6 pixels. Directed depth coverage and
constant-color checks pass. On the Metal desktop implementation, removed on
2026-10-06, native fixtures passed camera and rigid-object motion, static
output, zero shutter and transparent coverage. Disabled and zero-shutter
final/HDR payloads matched byte-for-byte; a static active pass preserved HDR
bytes exactly. The moving red edge's largest step fell from 4.990 to 2.794 at
180 degrees and 1.834 at 360 degrees. The transparent green edge stayed sharp.
A 1.849-pixel tile radius matched the authored camera displacement despite a
three-frame predecessor; 360 degrees doubled the radius to 3.697 pixels.

Commands: `./build_release.sh`; `python3 tools/checks/check_motion_blur.py
.scratch/motion-blur-native-runs.json`; and `./build_release/tools/vkr_harness
snapshot --case tools/cases/local/motion_blur_180_local.case.json --profile
tools/profiles/local-brdf-display-validation.json`, with graphics validation
variables unset. The motion blur cases are now pinned to Vulkan.

This screen-space approximation cannot recover hidden background or motion
from deformation. It deliberately preserves transparent-covered pixels.
No performance claim is made. Native Vulkan execution remains unrecorded.
