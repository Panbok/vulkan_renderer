---
status: implemented
updated: 2026-09-05
authority: adr
---
# ADR-015: Bounded typed metrics and pinned snapshots

## Status

Accepted. The application, renderer, resource workers, sample UI, and harness
use the registry. This record describes storage and publication; it makes no
claim that instrumentation has zero cost.

## Context

Metrics have different writers, lifetimes, and availability. A worker can finish
after the frame that started it. A consumer can retain a snapshot while the
renderer publishes another. Treating unavailable GPU timing as zero, or reading
a resetting counter twice, would corrupt measurements.

## Decision

`Application` owns an arena-backed `VkrMetrics`, initializes it before its
producers, and seals the catalog after renderer/device registration. Slots use
typed descriptors and generation/index IDs. Registration copies names and
rejects duplicates, invalid descriptors, and exhaustion. Required fixed
registration failures propagate through initialization; optional device rows
stop at available capacity and log the omitted coverage.

Render-thread writers update frame-local values. Concurrent writers update
cumulative atomics; finalization derives interval deltas, so workers never
retain a reusable frame buffer. Concurrent durations provide interval sum/count,
without claiming interval extrema derived from cumulative extrema.

The render thread claims a non-current, unpinned buffer from three snapshot
buffers before writing it. Readers acquire/release ownership atomically. If
neither candidate can be claimed, publication drops without waiting and the
concurrent interval is discarded explicitly. Bounded events copy subjects into
an MPSC ring and report dropped events and truncated subjects.

The renderer adapter owns collection of graph/backend aggregates and the
resetting upload/command-slot wait counters. Availability and reason codes
survive into reports. GPU timings retain source frame and submit serials;
collection time does not establish which frame performed the work.

Metal initializes referenced geometry rows during candidate preparation.
`cpu.packet_candidate_pack` includes those writes; the removed standalone
`cpu.packet_geometry_table_build` scope reports zero. Earlier Metal geometry
timings also included candidate packing, so adding those two historical rows
double-counts work. `packet.geometry_row_bytes` still reports the full indexed
upload span, not the number of rows written.

## Consequences

Writers use indexed storage after initialization. Consumer overlap and event
volume have fixed bounds. Reports must expose missing samples and overflow.
`VKR_METRICS_ENABLED=0` removes writer calls; the harness requires metrics enabled.
The overhead of a chosen instrumentation profile requires matched measurements.

## Alternatives considered

String-keyed sampling adds lookup work. Worker writes into frame-indexed arrays
make reuse unsafe. A global always-atomic policy imposes concurrent-writer costs
on render-thread slots. Log scraping loses types, validity, and ownership.

## Revisit when

A real catalog exceeds capacity, readers need longer retention, or sub-frame
tracing cannot fit the bounded event contract.

## Implementation

- [Registry and publication](../../lib/src/core/vkr_metrics.c),
  [types and inline writers](../../lib/src/core/vkr_metrics.h).
- [Application ownership](../../lib/src/application.h),
  [renderer collector](../../lib/src/renderer/vkr_renderer_metrics.c).
- [Harness sampling](../../tools/harness/vkr_harness_child.c).
