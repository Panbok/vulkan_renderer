---
status: implemented
updated: 2026-09-09
authority: adr
---

# ADR-040: MetalFX temporal reconstruction and completed-GPU scale control

## Status

Accepted.

## Context

A fixed spatial scale cannot reconstruct temporal detail or adapt to varying
Scene cost. MetalFX owns private history and needs motion to the exact previous
encode, which differs from selecting the newest completed portable history.

## Decision

Select spatial or MetalFX temporal mode before renderer initialization. Reject
unsupported devices/backends and explicit MetalFX under incompatible Metal
validation wrappers; do not silently substitute another workload. Zero-initialized
API callers use spatial mode. The macOS sample selects MetalFX and dynamic scale,
with an explicit portable diagnostic configuration under validation.

Stage scene-linear HDR, non-reversed depth and normalized `previous_uv - current_uv`
motion into private output-sized textures with an active internal content rectangle.
Pass jitter separately and convert motion to active-content pixel displacement.
MetalFX writes Scene-output-resolution HDR before exposure/bloom/tonemap. Paneled
UI stays native and composes after the reconstructed Scene; portable resolve is
omitted in this mode.

Follow the SDK with `MetalFX.Stabilize`, an output-resolution compute pass that
accumulates eligible stationary samples. The shared CPU scene signature and
native radiance, publication and graph revisions must match the exact preceding
submitted output/transform producer. Camera, scene, material, light, resource or
viewport changes, missing history and scaler resets immediately preserve current
MetalFX RGB and write sample age zero. Pending publication, text and active
upload/IBL writers remain ineligible. With SSR enabled, 128 consecutive matching
submitted frames first retain current MetalFX output; the following 128 eligible
samples form a finite mean. Completed eligible pixels copy that mean. This adds
no reprojection of its own and leaves the SDK's private history active.

The pass maps each canonical output center into the active source grid with
current raster jitter and reads four integer `temporal_validity` texels. Any
missing-motion marker or transmission/blend marker bypasses accumulation, even
when authored transparency reactivity is zero. Only reliable opaque/background
footprints accumulate. This uses existing validity, not an added MetalFX reactive
mask or FSR's optical-composition policy. The ceiling is six texture reads and
one write per output pixel: current color, four validity reads and previous color.
Global settling/reset bypasses need one current-color read and one write.

Retain `metalfx_output_color` in the existing completion-gated history pool.
With three frame slots, its instance count changes from three to five. The two
extra RGBA16F outputs add 14.0625 MiB of pixel storage at 1280×720; native
allocation rounding and temporary old/new resize allocations are excluded.
Existing per-image metadata owns the scene signature and settling count, and
publishes only after successful submission. Input selection and combined
last-use tracking preserve the common pool's completion proof. Alpha stores
private per-pixel sample age from 0 to 128. Scene presentation/export restores
opaque alpha; raw HDR diagnostics retain the versioned age contract described
in [ADR-044](044-shader-cross-backend-contract.md).

Previous transforms and matrices must identify the exact preceding scaler encode.
A missing predecessor resets history. An in-flight predecessor is ordered with a
GPU shared event and retained through consumption. Untracked graph textures use
the scaler's public fence. Output resize proves completion before replacing the
scaler, fence and textures; live dock drags defer that recreation until completion.

Dynamic resolution consumes completed GPU submission intervals tagged with scale.
Ignore duplicate and stale-tier samples. The allocation-free controller uses an
EMA, asymmetric over/under-budget thresholds, bounded 0.05 tiers and a cooldown;
retain the exact minimum endpoint and reset temporal state on transitions. The
sample starts at 0.8 within `[0.334,1.0]` targeting 13.333333 ms of GPU work.
Before an upward step, the controller records the lower tier's filtered cost.
If the higher tier exceeds budget and returns to that lower tier, it retains the
measured higher/lower cost ratio. Another upward attempt requires the lower
cost times that ratio to stay below the existing 82% headroom threshold for the
existing 45 qualifying samples. Unchanged work cannot repeatedly probe the same
known failure. Sustained headroom at the higher tier clears the failed boundary;
there is no timer expiry that would restart oscillation.

The most recent failed boundary is controller-owned, fixed-size CPU state. Actual
Scene output-size changes clear timing and learned costs while preserving scale,
configuration, submission watermark and transition count. Samples from the old
output extent are ignored. Internal tier changes retain the learned boundary.
This policy can keep a lower tier longer when workload characteristics change
without a corresponding lower-tier cost improvement; the user accepted that
tradeoff. It does not change the target, scale bounds, tier spacing or downshift
thresholds. Deterministic temporal checks cover a failed 15 ms upper tier above a
9 ms lower tier, no repeated probe for unchanged work, and a later retry after
the lower tier improves to 6 ms.
A local Release Bistro observation held scale 0.85 through its 600 measured
frames, with all 517 texture assignments resident; the matched case before this
change made six tier transitions during that window. This is a bounded stability
observation with different internal pixel workloads, not an authoritative speed
or moving-image quality comparison. The two local Release runs used
`tools/cases/local/editor_bistro_drs_headroom.case.json`; their reports passed
eight assertions with 517 resident textures and report SHA-256 values
`fc67990dc648643a6cf79daddade83628a014ddb6407a983838bff623d89f071` (before)
and `7d49cc8e09ef626d8d688dff0c4be3a6f08b2b8a8b162de26f8dfaa091389f6e` (after).
The shared Scene extent owner and MetalFX descriptor endpoints both round scaled
dimensions upward. This prevents a valid continuous minimum such as 0.334 from
rounding below the device's 1/3 input boundary in a small or odd-sized dock.
The requested scale tiers and device factor checks remain unchanged; quantization
adds at most one pixel per axis compared with nearest rounding.

## Consequences

MetalFX is an authorized Metal-only consumer, not bilateral algorithm parity.
GPU-work feedback does not guarantee a whole-frame FPS target. Moving-quality
acceptance and matched performance remain separate from source integration.
Validation of portable mode does not validate native MetalFX.

Stationary accumulation substantially reduces the measured static Bistro bar
variation at 80% scale. The mean per-pixel maximum RGB range in final normalized
sRGB fell from 0.00186788771 to 0.0000385497038, about 97.94%; the corresponding
HDR range fell from 0.0000408191097 to 0.000000644277395.
[The accumulation record](../../assets/verification/renderer-features/metalfx-stationary-accumulation.txt)
owns the exact commands, cases and report digests. Its checkpoints come from
independent child processes, so this is a descriptive variation measurement,
not proof of exact freezing along one continuous history. Moving MetalFX flicker
remains unresolved. There is no matched performance or native Vulkan claim.
The settling window is a policy, not proof that recursive SSR or SDK output has
fully converged; a finite mean can retain sampling bias until its next reset.

The approved scale-dependent jitter trial was reverted after the 80%-scale
Bistro bar comparison worsened. Across 24 matched checkpoints, bar pixels whose
RGB range exceeded three display codes increased from 859 to 1,475; the maximum
range increased from 11 to 13. MetalFX retains eight phases. The trial does not
establish moving-image quality or timing. [The trial record](../../assets/verification/renderer-features/metalfx-jitter-trial.txt)
preserves the cases, measurements and limits.

A controlled replay through `MTL4FXTemporalScaler` found byte-identical output
with black and bright inactive input padding. Fixed exposure values of 1 and 8
did not improve its bar variation. [The replay record](../../assets/verification/renderer-features/metalfx-input-replay.txt)
includes the standalone reproducer and distinguishes frozen captured inputs from
current scene execution. Neither control justifies a production policy change.

## Alternatives considered

Newest-completed portable history is the wrong motion source for private scaler
history. CPU waiting for the previous encode would unnecessarily serialize frames.
A fixed scale sacrifices detail throughout lighter intervals.

Increasing the Halton period with the output/input area ratio follows Apple's
[temporal upscaling guidance](https://developer.apple.com/videos/play/wwdc2022/10103/),
but did not improve this reflective bar. Do not restore that policy as a flicker
fix without new scene evidence.

## Revisit when

Framework support changes, accepted moving-image evidence exposes reconstruction
faults, or another backend gains an authorized temporal upscaler.

## Implementation

[`vkr_dynamic_resolution.c`](../../renderer/src/vkr_dynamic_resolution.c),
[`vkr_metal_packet_setup.inc`](../../renderer/src/metal/internal/vkr_metal_packet_setup.inc),
[`vkr_metal_packet_commands.inc`](../../renderer/src/metal/internal/vkr_metal_packet_commands.inc),
[`vkr_metal_packet_frame.inc`](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc),
[`vkr_metal_packet_graph.inc`](../../renderer/src/metal/internal/vkr_metal_packet_graph.inc), and
[`post/metalfx.metal`](../../renderer/src/shaders/metal/msl/post/metalfx.metal).
