---
name: vkr-renderer-design
description: Applies VKR's renderer architecture, semantic-compression, N+1, hot-path, GPU-lifetime, and measured-performance rules. Use when reviewing or refactoring lib/src/renderer, changing the Vulkan backend, render-graph state, pass executors, packet contracts, resource ownership, command recording, renderer hot paths, or renderer-facing interfaces.
---

# VKR Renderer Design

## Guiding principle

**Performance is correctness.** This is a renderer: a frame that misses its
budget is a failed frame, and a per-draw heap allocation, blocking wait, string
construction, or lock is a defect rather than a style preference.

The paired constraint is what keeps that honest: **a performance claim without
same-configuration measurement is not a result.** Ownership, lifetime, and
submission-completion invariants are what make a measurement mean anything —
a faster frame that reuses a resource before the GPU is done with it has not
measured anything. Never trade an invariant for a number.

## Quick start

1. Read `docs/architecture/renderer-architecture-spec.md` (status authority) and
   the relevant ADRs in `docs/architecture/adr/`. Check §8 before "fixing"
   anything — the known gaps are already prioritized there.
2. Define the visual, behavioral, lifetime, and performance invariants you must
   preserve, before editing.
3. Baseline: `./build_test.sh`, plus a Release measurement via
   `vkr-performance` when the change touches a hot path.
4. Trace concrete call paths. Find at least two real examples before compressing.
5. Refactor one vertical slice, run the cheapest sufficient gate, measure again.

Read [PRINCIPLES.md](PRINCIPLES.md) for the Muratori/Fleury compression rules and
the project N+1 test. Read [VULKAN_PATTERNS.md](VULKAN_PATTERNS.md) for this
repository's real backend seams and its currently-known architectural gaps. Use
[WORKFLOW.md](WORKFLOW.md) for comprehensive audits and migration sequencing.

## Core rules

- Compress observed repetition; never design reuse from zero or one example.
- Preserve continuous granularity: a high-level helper must have a usable
  lower-level equivalent so exceptional work does not require a rewrite.
- Prefer a few deep modules with small interfaces and strong locality over many
  shallow forwarding layers.
- A seam needs real variation. One adapter is hypothetical; two are real.
  `VkrRendererBackendInterface` has exactly one implementation — do not widen it
  on the theory that a second backend might arrive.
- Apply the N+1 test at module interfaces: adding one pass kind, resource state,
  texture format, light kind, or material slot must extend a table, descriptor,
  tagged record, or registration point without rewriting unrelated callers.
- N+1 is **not** permission to generalize per-draw or per-dispatch hot loops.
- Collapse equivalent cases into shared codepaths; make invalid states
  unrepresentable with typed/tagged records where practical.
- Use one authoritative representation per fact, and lower it once.

## Hot-path rules

Per draw, per dispatch, and per instance:

- no heap allocation, no arena growth that is not a bump into pre-reserved space;
- no blocking fence or semaphore wait;
- no `String8` construction, formatting, or `snprintf`;
- no handle acquire/release churn, no registry lookup by name;
- no mutex acquisition;
- no pipeline creation or descriptor-set allocation.

Prefer contiguous batches, SoA when measurement justifies it, and fixed-capacity
arrays for GPU-bounded collections. `VkrInstanceBufferPool` (65,536 entries) and
`VkrIndirectDrawSystem` (16,384 commands) are the existing fixed-capacity model;
overflow is reported, not silent — keep it that way.

Validate at creation, compile, and debug layers. In a proven hot loop use an
assertion, not a recoverable check.

## Render graph and packet contracts

- The graph declares resources and accesses; `vkr_rg_compile.c` builds
  dependencies, orders passes, culls, and plans barriers; `vkr_rg_execute.c`
  runs them. Work that touches GPU state must be **declared**, not hidden inside
  an executor.
- `VkrRenderPacket` payload arrays are caller-owned and must stay alive until
  `vkr_renderer_submit_packet()` returns. Passes read typed payloads from
  `VkrRgPassContext`.
- Submission is ordered and state-mutating: `vkr_renderer_prepare_frame()`
  acquires the swapchain image before validation runs. Treat packet rejection as
  a path with side effects, not a clean no-op.
- `VKR_RG_RESOURCE_FLAG_TRANSIENT` means graph-owned and reusable, not
  freed-per-frame. Resources survive realizations and are recreated when their
  resolved description changes.

## Resource and GPU lifetime

- Every successful acquire has a matching release on **all** paths, including
  early error exits. Prefer one cleanup path (`goto cleanup;`) over duplicated
  teardown.
- Logical destruction invalidates a handle immediately. Physical destruction and
  slot reuse wait on proven GPU completion — a frame fence or submit serial, not
  an assumed frame lag. Instance descriptor-state retirement already uses submit
  serial; follow that model.
- Descriptor indices, buffer ranges, and readback ring slots stay unavailable for
  reuse until every recorded and submitted use is resolved.
- Vulkan types stay behind `lib/src/renderer/vulkan/`. Public code depends on
  opaque handles and `VkrTextureDescription`-style typed descriptions.
- Treat file byte buffers (SPIR-V, glTF, KTX2) as temporary: allocate from a
  scope and release after the Vulkan object is created. See `vkr-memory`.

## Compression and cleanup

Use `compress-codebase` for a systematic pass. For ordinary feature work:

- extract a `static` helper in the `.c` file before promoting anything to a
  header;
- prefer `static inline` functions and data tables over macros;
- use the rule of three — extract at 3+ occurrences, keep it direct at 1–2;
- after every feature or fix, remove dead paths, unused variables, and defensive
  checks that no longer guard a real case;
- a comment is a semantic abstraction. If it restates the code, delete it; if it
  records intent, ownership, ordering, or a Vulkan synchronization requirement,
  keep it.

## Validation

`vkr-validation` owns the gates. Minimum for renderer-facing change:
`./build_test.sh`, a Vulkan validation-layer run, and — for anything touching a
hot path — a Release measurement per `vkr-performance`. The CPU test suite does
not substitute for a validation-layer run.
