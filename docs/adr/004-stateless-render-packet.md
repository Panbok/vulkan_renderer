---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-004: Versioned packet submission with ordered preparation

## Status

Accepted.

## Context

The application describes requested frame work independently of native pass
recording. Retained assets and frame acquisition still require state.

## Decision

Use `VkrRenderPacket` version 27 with typed optional world, shadow, skybox,
UI, editor, picking, text and debug payloads. Globals carry exposure, bloom,
GTAO and temporal inputs. Caller-owned arrays remain valid until submit returns.
Generation handles identify retained resources; packets are not standalone
serialized recordings.

`vkr_renderer_prepare_frame()` proves slot reuse and prepares the target before
packet construction. `vkr_renderer_submit_packet()` validates the structural
packet envelope before native lowering. Capacity, required pointers, version,
ranges and publication generations are checked at that boundary. Draw recording
consumes producer-proven rows.

Rejected packets and recording failures use the selected implementation's
cancel path to resolve prepared target/command state. Static candidate residency,
retained graph state and temporal histories commit only after successful submit.
Other retained resource or text changes are not a general transaction.

## Consequences

Frame inputs have one ownership and validation boundary, while the prepare/submit
protocol remains stateful. Invalid packets may still pay acquisition/cancel
work. Public layout changes require packet-version coordination.

## Alternatives considered

Immediate bind/draw APIs couple applications to native ordering. A retained
renderer scene would couple extraction to one scene model. Fully transactional
submission would require a different acquisition and mutation protocol.

## Revisit when

Standalone replay, parallel packet construction or transactional rejection
becomes a concrete requirement.

## Implementation

[`vkr_render_packet.h`](../../lib/src/renderer/vkr_render_packet.h),
[`renderer_frontend.c`](../../lib/src/renderer/renderer_frontend.c),
[`vkr_candidate_residency.h`](../../lib/src/renderer/vkr_candidate_residency.h),
and both selected implementation submit/cancel paths.
