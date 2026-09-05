---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-025: Procedural renderer with build-selected native operations

## Status

Accepted.

## Context

Metal 4 runs on macOS and capability-gated Vulkan 1.4 runs on Windows. This
platform selection does not require a runtime operations table or untyped state
pointer. GPU-addressed data and bindless resources already support direct native
recording without a generic per-draw RHI.

## Decision

Use ordinary C functions with typed arguments. Native lifecycle, frame rendering,
cancellation, targets, completion, capture and metrics select their implementation
at the platform build boundary. `VkrRendererImpl` retains implementation properties;
it contains no operations table or `void *state`. Initialization rejects an
unsupported requested backend before creating its native resources.

Use established rendering nouns and verbs. Function names distinguish creating
resources, preparing inputs, recording commands, submitting work, presenting and
observing completion. A function named `submit` must not conceal scene extraction
or UI construction. The public `render_frame` name describes its full preparation,
recording, submission and presentation responsibility. Parameters expose the
operation's inputs; private native resource types may remain opaque. Plain
functions do not require public native object layouts or global renderer state.

The native implementation owns pipelines, resources, graph realization, barrier
lowering, command recording, submission and timestamps. Shared C owns frame-input
validation, derived portable frame facts, graph semantics and the lifetime cores
in ADR-024. Assets publish through `VkrAssetPublisher`; this separate publication
contract retains its existing callbacks and generation rules.

Handle recoverable failure at creation, allocation, publication, preparation,
acquisition and native submission/completion boundaries. Validate references and
reserve capacity before draw/dispatch emission. Prepared command emitters use
`void` where they consume proven inputs and cannot encounter recoverable allocation
or lookup failures. A native `void` recording command does not prove valid use,
successful submission or GPU completion. Both native implementations prepare
all enabled pass families before emission. Native object and encoder creation,
command-buffer begin/end and native lifecycle operations remain fallible. `DEVICE_ERROR` terminates the current
renderer lifetime: callers stop rendering and destroy it. Native Vulkan terminal
failures propagate through begin/render/cancel. A Metal failure after queue commit
also reports `DEVICE_ERROR`; already-submitted GPU work and native history cannot
be rolled back as though preparation had failed.

Preserve GPU-pointer roots, bindless texture/sampler tables and the authored
render graph. GPU ranges still need native backing for indexed/indirect commands;
image barriers still need layouts and subresource identities. Backend resource
identifiers and root layouts remain native contracts under ADR-044. Aaltonen's
[No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api) informs
the small procedural vocabulary, not removal of these ownership or synchronization
requirements.

The application and editor share `renderer_lib` and a neutral sample runtime.
Backend-specific quality modes retain explicit capability boundaries, as in
ADR-039/040. No generic command RHI, legacy Vulkan 1.2 fallback, frontend pipeline
registry or per-draw runtime dispatch is introduced.

## Consequences

Native calls have explicit types and compile-time selection. The application owns
scene-facing systems, event subscriptions, resize delivery and frame scratch.
`VkrRenderAssets` owns asset systems, loader registration, persistent text and load
scratch. Assets borrow the renderer's publisher; shutdown joins workers and drains
GPU use before releasing those owners. Renderer initialization takes native target
and device configuration without an event manager or application frame-rate state.
Unused renderer owner/scratch arenas and their capability sizes are removed;
native graph DMemory retains its backend owner. Backend allocator queries expose
that native allocator rather than an unused shared arena.
API-specific code remains substantial, and shared semantics still require native
evidence from both backends. There is no Linux or
D3D12 implementation promise.

## Alternatives considered

The former coarse runtime operations table selected behavior the platform build
already determines. A generic draw RHI would add a second command vocabulary
without a demonstrated shared recording policy. Exposing all native fields would
spread resource and synchronization ownership into callers.

## Revisit when

A concrete platform requirement needs runtime backend selection, or measured
repeated native recording policy justifies a shared command operation.

## Implementation

[`vkr_renderer.h`](../../lib/src/renderer/vkr_renderer.h),
[`vkr_renderer_impl.h`](../../lib/src/renderer/vkr_renderer_impl.h),
[`vkr_renderer_impl.c`](../../lib/src/renderer/vkr_renderer_impl.c), and
[`vkr_renderer.c`](../../lib/src/renderer/vkr_renderer.c),
[`vkr_renderer_internal.h`](../../lib/src/renderer/vkr_renderer_internal.h),
[`vkr_render_assets.h`](../../lib/src/renderer/systems/vkr_render_assets.h), and
[`application.h`](../../lib/src/application.h).
This record incorporates former ADR-020/021/022/026.
