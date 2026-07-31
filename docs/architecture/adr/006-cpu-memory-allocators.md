---
status: implemented
updated: 2026-07-31
authority: adr
---
# ADR-006: Lifetime-Specific CPU Allocators Behind a Common Interface

**Status:** Accepted

## Context

Renderer allocations have different lifetimes: per-call scratch, retained
engine data, individually removable registry entries, scene-lifetime data, and
homogeneous object slots. Arena-only allocation caused high-water growth when
used for reloadable/removable data, while general allocation everywhere would
lose the intended scratch and bulk-lifetime behavior.

## Decision

Expose three backends through `VkrAllocator`:

| Backend | Semantics | Intended use |
|---|---|---|
| `Arena` | Bump allocation, scope/reset/bulk destroy | Scratch or a shared bulk lifetime |
| `VkrDMemory` | Reserved/committed virtual memory and individual free | Reloadable objects, keys, registries |
| `VkrPool` | Fixed-size slots | Homogeneous pooled objects |

`VkrArenaPool` is a separate thread-safe chunk pool used to construct temporary
asset-loader arenas. It is not a `VkrAllocator` backend and should not be counted
as a fourth strategy behind that interface.

Layered behavior includes:

- optional allocator scopes and scope-depth inspection;
- aligned allocation/reallocation;
- per-allocator and atomic global tagged accounting;
- `_ts` entry points that take/use synchronization supplied by the caller;
- `vkr_allocator_release_global_accounting()` before bulk destruction when
  per-allocation frees will not reconcile global totals.

Repository lifetime rules require scratch scopes to end, individually removable
data to use a freeable allocator, owned hash keys to be freed only after table
removal, and every acquired renderer handle to be released on all paths.

The scene runtime owns a scene arena and releases its global accounting before
destroying it. Async scene/mesh preparation has dedicated `VkrDMemory` storage
and mutexes.

## Consequences

**Positive**

- Callers can select allocation behavior by lifetime without changing subsystem
  APIs.
- Scratch paths are cheap and retained/reloadable paths can reclaim memory.
- Tagged statistics provide useful ownership telemetry.
- Scene bulk destruction is explicit and efficient.

**Negative / risks**

- Correct choice is enforced primarily by contributor discipline, not types.
- Allocators are not intrinsically thread-safe; correct `_ts`/mutex use matters.
- Arena high-water behavior is intentional and can be mistaken for a leak.
- Global accounting can drift when bulk reconciliation is omitted.
- These mechanisms reduce known reload-growth risks but do not prove every
  loader or partial-failure path is symmetric.

## Alternatives Considered

- **Arena only.** Cannot reclaim independently owned reloadable entries.
  Rejected.
- **General allocator only.** Simpler but loses scope/bulk behavior. Rejected.
- **Lifetime-specific types in every API.** Stronger enforcement, but invasive
  in C11. Deferred unless misuse remains common.

## Revisit When

- Repeated load/unload measurements expose growth or ownership imbalance.
- Parallel loading shows allocator/mutex contention.
- Debug builds need guard pages, allocation provenance, or leak backtraces.
