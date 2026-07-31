---
status: partial
updated: 2026-07-31
authority: adr
---
# ADR-008: Lifetime-Tiered CPU↔GPU Data Paths

**Status:** Accepted (partial)

## Context

Per-frame instances, uniform values, descriptor bindings, load-time assets, and
GPU readback have different size, lifetime, and synchronization needs. One
transfer strategy would either over-synchronize frequent data or make bulk
device-local resources inefficient.

## Decision

Use separate paths for each lifetime class.

### 1. Persistently mapped frame streams

`VkrInstanceBufferPool` owns three fixed-capacity mapped buffers (65,536
instances each). `begin_frame` resets a cursor, `alloc` returns a mapped range,
and `flush_range` handles non-coherent visibility. Overflow logs/fails and can
cause the pass to omit work.

`VkrIndirectDrawSystem` mirrors this for 16,384 layout-asserted indirect
commands, but no production pass currently calls `vkr_indirect_draw_alloc()`.

Both stream rings are reset with the frame-in-flight slot exposed by the
backend. Reuse is therefore protected by the same fence that `begin_frame`
waited before resetting the cursor, independently of swapchain image count.

### 2. Uniforms and descriptors

Global UBO regions and global descriptor sets are indexed by swapchain image.
Descriptor writes are cached using generation plus the concrete image view,
sampler, and buffer payload. Instance descriptor-state release is deferred by a
monotonic submit serial.

`.shadercfg` supplies named uniform staging offsets; those member offsets are
not yet cross-validated against SPIR-V (ADR-005).

### 3. Resource preparation and bulk upload

`VkrResourceLoader` splits CPU-only worker `prepare_async` from render-thread
`finalize_async`. The pump uses request/op/byte budgets and allows the first
oversized request so progress cannot deadlock.

Bulk image/buffer uploads use staging and a dedicated transfer queue when a
separate transfer family is available, including queue-family ownership work.
However, upload helpers wait indefinitely on their submit fences before
returning, including a graphics upload phase where needed. GPU finalization is
therefore synchronous at the call boundary; the queue is dedicated, but upload
completion is not asynchronous.

Per-worker transfer and graphics-upload command pools exist behind experimental
parallel runtime controls. They require both `VKR_PARALLEL_UPLOAD` and
`VKR_PARALLEL_UPLOAD_UNSAFE=1`; this is not the normal default path.

### 4. GPU readback

Picking copies one pixel into a three-slot host-visible ring. Polling normally
checks the associated frame fence without blocking and returns results later.
When a new request wraps onto a pending slot, request recording waits
indefinitely for that slot's frame fence. Readback is deferred in the common
case, not guaranteed non-blocking.

## Consequences

**Positive**

- Frequent frame data avoids staging copies.
- Asset CPU preparation can run in parallel without Vulkan access.
- Compressed texture payloads can upload explicit mip/layer regions.
- Descriptor churn is reduced and descriptor-state destruction is delayed past
  GPU use.
- Static assertions pin GPU-facing frame data layouts.

**Negative / risks**

- The implementation has several lifetime paths and fixed capacities.
- A pump byte/op budget does not guarantee a frame-time budget while each GPU
  finalization can block on fences.
- Readback ring pressure can stall the recording thread.
- The CPU-only prepare contract and experimental parallel-upload safety are
  primarily convention/configuration based.

## Alternatives Considered

- **Stage every update.** Uniform but wasteful for small frequent host-visible
  data. Rejected.
- **Use the graphics queue only.** Simpler ownership, but gives up a selected
  transfer queue. Still a reasonable fallback on devices without a dedicated
  family.
- **Truly asynchronous upload retirement.** Preferred evolution: signal a
  semaphore/timeline value, retain staging objects until completion, and let
  consuming work wait without blocking the CPU.
- **Synchronous readback.** Simpler but stalls on every pick. Rejected.

## Revisit When

- Remove per-upload waits and define staging retirement/consumer dependencies.
- Replace the unsafe parallel opt-in with validated queue/pool ownership or
  remove the dormant path.
- Add adaptive finalization based on measured frame time.
- Grow or make fixed stream limits configurable when telemetry shows pressure.
