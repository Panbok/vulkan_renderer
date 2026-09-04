---
status: implemented
updated: 2026-09-05
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
Device factor limits and rounded endpoint extents are checked before encoding.

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
