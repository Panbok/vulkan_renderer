---
status: implemented
updated: 2026-09-06
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

## Alternatives considered

Newest-completed portable history is the wrong motion source for private scaler
history. CPU waiting for the previous encode would unnecessarily serialize frames.
A fixed scale sacrifices detail throughout lighter intervals.

## Revisit when

Framework support changes, accepted moving-image evidence exposes reconstruction
faults, or another backend gains an authorized temporal upscaler.

## Implementation

[`vkr_dynamic_resolution.c`](../../lib/src/renderer/vkr_dynamic_resolution.c),
[`vkr_metal_packet_setup.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_setup.inc),
[`vkr_metal_packet_commands.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_commands.inc), and
[`vkr_metal_packet_frame.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_frame.inc).
