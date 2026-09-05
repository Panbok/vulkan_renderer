---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-004: Explicit frame inputs and acquired frame context

## Status

Accepted.

## Context

Frame inputs must describe requested rendering work without triggering hidden
scene, UI or text edits. Native resource ownership, acquisition and temporal
history still require retained state.

## Decision

Use `VkrFrameInput` version 28 with frame metadata, camera/lighting/settings and
typed optional world, shadow, skybox, UI, editor, picking and debug payloads.
The application builds scene candidates, ordinary blend draws, UI geometry and
world-text draws before rendering. Text edits apply through the text owner and
are absent from the frame input. Supplied draw streams are authoritative;
rendering does not replace them with retained subsystem output.

`VkrPreparedFrame` is private rendering input. It combines the borrowed caller
input with renderer-derived temporal, exposure, bloom and GTAO data. The public
input no longer carries fields callers must zero so the renderer can overwrite
them. CPU arrays remain caller-owned through `vkr_renderer_render_frame()`;
generation identities refer to resources with independent GPU last-use lifetimes.
A frame input is not a standalone serialized command buffer.

`vkr_renderer_begin_frame(renderer, &config, &frame)` receives explicit shadow
map dimensions, proves slot reuse and prepares the target. The returned `VkrFrame`
contains the renderer pointer, acquisition number, target generation, resolved
output/render dimensions and retained-shadow token. The renderer outlives this
context; callers must not copy or modify it. Its number identifies acquisition,
not GPU completion.

`vkr_renderer_render_frame(&frame, &input, ...)` validates the acquired context,
input structure and matching target extent, then prepares derived data, realizes
the graph, records native commands, submits and presents. This is a complete
render operation, not a queue-submit primitive. Rendering or cancellation consumes
the context. `vkr_renderer_cancel_frame(&frame)` resolves acquired resources and
recorded-but-unsubmitted work when input construction fails or a frame is abandoned.
Input rejection cancels the frame; cancellation errors take precedence over the
input error. Reusing a consumed or stale context is rejected.

`vkr_frame_input_validate()` owns structural version, capacity, pointer and range
checks. Native preparation resolves geometry/material generations, submesh ranges
and GPU storage before command emission. Vulkan omits genuinely pending geometry
initialization or material texture publication while preserving ready-draw order;
stale or invalid references fail preparation. Metal exposes no pending native
handle and rejects absent or stale references.

Prepared pass records and root spans belong to acquired frame storage. Both
native implementations prepare all enabled pass families before command emission.
Prepared recorders consume resolved resources, root ranges and dispatch dimensions;
picking and blend use disjoint spans. Cancellation releases unsubmitted upload
storage and speculative last-use marks. Static candidate residency, retained graph
contents and temporal histories commit only after successful submission. Earlier
scene/text edits and resource publication are not a transaction rolled back by
frame cancellation. ADR-024 owns GPU retirement; ADR-044 owns native shader ABI.

## Consequences

The application owns scene-facing systems, frame scratch and `VkrFrameGlobals`.
`VkrRenderAssets` owns persistent assets, text, loaders and load scratch, borrowing
the publisher from the longer-lived renderer. Scene extraction takes concrete
subsystem inputs in `vkr_scene_frame`. The renderer owns acquisition, native
resources and derived rendering state; it does not own scene or UI lifecycle.

The application consumes resize events before acquisition. `target_generation`
lets it refresh UI target state and invalidate shadow fitting even when a recreated
target has unchanged dimensions. Caller-owned scene/asset teardown proves GPU idle
and preserves loader/publisher lifetimes under ADR-024.

Acquisition precedes input validation, so rejection may pay acquisition/cancel
cost. Public layout changes require frame-input version coordination. Allocation,
preparation and native submission remain fallible; prepared emission uses `void`
functions under the boundary rules in ADR-025.

## Alternatives considered

A generic immediate draw API would expose native recording order to scene callers.
Literal statelessness would move histories, publication and completion tracking
into those callers. Transactional scene edits would require a separate mutation
protocol and are not part of frame rendering.

## Revisit when

Standalone replay, parallel frame construction or transactional scene edits
become concrete requirements.

## Implementation

[`vkr_frame_input.h`](../../lib/src/renderer/vkr_frame_input.h),
[`vkr_frame_input.c`](../../lib/src/renderer/vkr_frame_input.c),
[`vkr_prepared_frame.h`](../../lib/src/renderer/vkr_prepared_frame.h),
[`vkr_renderer.h`](../../lib/src/renderer/vkr_renderer.h),
[`vkr_renderer.c`](../../lib/src/renderer/vkr_renderer.c),
[`vkr_scene_frame.c`](../../lib/src/renderer/systems/vkr_scene_frame.c), and
[`application.h`](../../lib/src/application.h).

## Evidence boundary

The complete migration passed `./build_release.sh`, `./build_editor.sh Release`
and `./build_test.sh` on macOS. CPU coverage includes stale-acquisition rejection,
explicit resource completion inputs and moved UI/asset ownership contracts.

Two serial Release Metal API-validation runs passed with `MTL_DEBUG_LAYER=1`
and shader validation unset. The `renderer_api_draws_validation` case passed all
six candidate/residency/overflow assertions with text/UI enabled and three target
images. `text_windowed_resize` passed its native resize round trip under
`local-metal-windowed-validation-serial.json`; logs confirmed validation activation
without API errors in either process.

The `renderer_api_bistro` Release captures preserve baseline depth bytes and all
20 retained draw/candidate/upload work-volume rows. The first full-migration
color capture failed the unchanged maximum-error gate at two pixels (3/255).
An unchanged-binary repeat passed at 2/255. Earlier same-binary observations also
showed color variation; its cause remains unestablished. Both observations are
retained in local task evidence, with exact commands, report digests and comparison
calculations. Manual-exposure telemetry assertions remain unavailable. No baseline
was promoted and no deterministic-output or timing improvement is claimed.

All seven changed Vulkan translation units pass SDK 1.4.357 compile-only checks,
including a Windows preprocessor configuration with temporary Win32 type shims.
This does not validate Windows SDK ABI, linking or Vulkan execution. Native Vulkan,
editor interaction, in-process scene-reload memory plateaus and device-fault
injection remain unrun.
