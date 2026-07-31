---
status: investigation
updated: 2026-07-31
authority: investigation
---
# GPU Device-Memory Baseline

> **Measurement boundary.** Captured on one machine, one scene, one **Debug**
> build. Debug load time is not a release figure and must not be quoted as one.
> The unload sample is missing (see *Gaps*), so release symmetry is unverified.

## Why this exists

`docs/architecture/renderer-architecture-spec.md` §8 P1 item 10 asks for device
memory pooling *and* budget telemetry, with an explicit ordering:

> Measure allocation count, heap use, and load time before selecting block sizes
> or VMA.

The renderer makes one `vkAllocateMemory` per buffer, image, and readback
buffer. Whether that is a problem, and what block size a pool should use, is a
question about the *distribution* of those allocations — how many, how large,
and in which memory types. Choosing a block size before measuring that
distribution would be a guess.

This document is the measurement half. No pooling allocator has been written.

## What is instrumented

`VkrDeviceMemoryStats` (`lib/src/renderer/vkr_renderer.h`), reported through
`vkr_renderer_get_device_memory_stats()`:

| Field | Meaning |
|---|---|
| `live/peak/total_allocation_count` | Device allocations outstanding, high-water mark, and cumulative |
| `live/peak_bytes` | Device bytes outstanding and high-water mark |
| `live_totals_exact` | False if the tracking table overflowed; live figures are then inexact |
| `live_bytes_by_type`, `live_count_by_type` | Per memory-type distribution — the input to a block-size decision |
| `heap_index_by_type`, `property_flags_by_type` | Which heap each type draws from, and its property bits |
| `heap_size_bytes` | Heap capacity from `vkGetPhysicalDeviceMemoryProperties` |
| `heap_usage_bytes`, `heap_budget_bytes` | Driver-reported usage/headroom, including other processes |
| `heap_usage_valid` | False when `VK_EXT_memory_budget` is unavailable |

**How counts stay honest.** Every `VkDeviceMemory` the renderer owns is
allocated through `vulkan_backend_allocate_device_memory` and released through
`vulkan_backend_free_device_memory`. Allocations are recorded in an
open-addressed table keyed by the handle, so a free needs only the handle and
the counters stay exact while the table has capacity. If the table saturates,
tracking sets `live_totals_exact = false` and logs, rather than presenting the
remaining live totals as authoritative.

`VK_EXT_memory_budget` is enabled when the device exposes it. It is telemetry
only: without it, heap sizes and the renderer's own totals are still reported,
just not how much headroom the driver thinks is left.

## How to capture

Run the app and read the `GPU_MEM` lines. They are emitted at three points:

- `startup` — the renderer's own resident allocations, before any scene
- `load-ready` — after an async scene load completes
- `unload` — after a scene is torn down, for checking release symmetry

The capture is headless and reproducible — no human needs to drive the app:

| Environment variable | Effect |
|---|---|
| `VKR_AUTOLOAD_SCENE=1` | Loads `SCENE_PATH` at startup instead of waiting for the `L` key |
| `VKR_METRICS_INTERVAL_SECONDS=<n>` | Emits a `GPU_MEM`/`CPU_MEM` sample every `n` seconds |
| `VKR_AUTOCLOSE_SECONDS=<n>` | Quits after `n` seconds |

All three are opt-in; unset, the app behaves exactly as before.

```sh
./build.sh Debug
VKR_AUTOLOAD_SCENE=1 VKR_METRICS_INTERVAL_SECONDS=8 VKR_AUTOCLOSE_SECONDS=48 \
  ./build/app/vulkan_renderer 2>&1 |
  sed 's/\x1b\[[0-9;]*m//g' | grep -E "GPU_MEM|CPU_MEM|SCENE_LOAD_TIME"
```

Samples are labelled `startup`, `tickN@<elapsed>`, `load-ready`, and `unload`,
so a capture reads as a series. That matters: allocation counts that settle
after a load look different from counts that keep climbing, and only the series
tells them apart.

## Results — captured 2026-07-31

### Environment

| | |
|---|---|
| GPU / driver | Apple M1 Pro / MoltenVK |
| `VK_EXT_memory_budget` | Available |
| `maxMemoryAllocationCount` | 1,073,741,824 |
| Scene | `assets/scenes/sponza.scene.json` |
| Build | Debug |

### Allocation counts

| Point | Live allocs | Peak allocs | Total allocs | Live bytes | Peak bytes | Exact |
|---|---|---|---|---|---|---|
| startup | 87 | 88 | 136 | 198.7 MB | 279.4 MB | yes |
| load-ready | 197 | 199 | 342 | 2275.2 MB | 2443.0 MB | yes |
| tick@48s (steady) | 206 | 206 | 351 | 2281.5 MB | 2443.0 MB | yes |
| unload | _not captured_ | | | | | |

Live count and bytes are identical at 32 s, 40 s, and 48 s, so the renderer
reaches a steady state after the load rather than growing.

### Per memory type at steady state

| Type | Heap | Property flags | Live count | Live bytes | Mean alloc |
|---|---|---|---|---|---|
| 0 | 0 | `0x1` DEVICE_LOCAL | 156 | 2258.7 MB | **14.5 MB** |
| 1 | 0 | `0xf` DEVICE_LOCAL \| HOST_VISIBLE \| HOST_COHERENT \| HOST_CACHED | 50 | 22.8 MB | **0.47 MB** |

### Heaps at steady state

| Heap | Size | Driver usage | Driver budget |
|---|---|---|---|
| 0 | 16384.0 MB | 2311.9 MB | 12124.2 MB |

Unified memory, so one heap; driver usage exceeds the renderer's own total
because it counts the whole process.

### Scene load time

| Scene | Seconds (Debug) |
|---|---|
| sponza | 28.2 – 29.6 across two runs |

## What the numbers say

**Pooling is not urgent on this hardware, and the spec's framing overstated the
risk.** Three readings:

1. **Allocation count is nowhere near the limit.** Peak 206 against a
   `maxMemoryAllocationCount` of ~1.07 billion. The architecture spec's earlier
   concern that large scenes "approach a particular allocation limit" does not
   hold here — per-resource allocation is a *performance* question on this
   device, not a correctness one. A desktop driver reporting the common 4096
   limit would still leave 206 comfortably clear.

2. **The distribution is strongly bimodal, so a single pool would be wrong.**
   DEVICE_LOCAL allocations average 14.5 MB while host-visible ones average
   0.47 MB — a 30× spread. One block size cannot serve both without either
   fragmenting the large allocations or wasting most of each block on the small
   ones. Any future pool should be at least two, split on memory type.

3. **The large allocations are already pool-sized.** At a 14.5 MB mean, the
   DEVICE_LOCAL allocations are the size a block allocator would hand out
   anyway. Sub-allocating them would mostly move bookkeeping around rather than
   reduce allocation count meaningfully. The 50 host-visible allocations
   averaging under half a megabyte are the only population where pooling
   clearly pays, and they total 22.8 MB — roughly 1% of device memory in use.

**Conclusion: do not write a pooling allocator on the strength of these
numbers.** Revisit if a target device reports a low `maxMemoryAllocationCount`,
or if a scene pushes the small-allocation population by an order of magnitude.
That is a measured decision rather than a deferred one.

## Gaps

- **No `unload` sample.** Release symmetry — whether live count returns to the
  startup figure — is unverified. Unload is bound to the `U` key and the
  headless capture cannot press it. Either capture it interactively, or add an
  auto-unload knob if it needs to be part of a regression gate.
- **Debug build only.** The load time is a Debug figure and says nothing about
  Release. Allocation counts and sizes should not differ.
- **One device, one scene.** MoltenVK's effectively-unlimited allocation count
  is the most device-specific number here; a discrete-GPU capture would test
  reading 1 above.
