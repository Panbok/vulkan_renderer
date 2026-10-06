---
status: implemented
updated: 2026-10-06
authority: adr
---

# ADR-091: Growable Array and probe-bounded hash table

## Status

Accepted. Implemented in
[`array.h`](../../lib/src/containers/array.h),
[`vkr_hashtable.h`](../../lib/src/containers/vkr_hashtable.h),
[`queue.h`](../../lib/src/containers/queue.h),
[`bitset.c`](../../lib/src/containers/bitset.c) and
[`vkr_freelist.c`](../../lib/src/containers/vkr_freelist.c), with the legacy
glyph index in [`vkr_text.c`](../../runtime/src/core/vkr_text.c).

## Context

An audit of `lib/src/containers/` at `526665c8` found these defects:

- The hash table failed an insert after 128 probes without growing. A
  simulation with the real growth rule needed more than 127 probes in four
  of five trials at 100,000 keys.
- Removes left tombstones that only growth cleared. `request_by_key` in
  [`vkr_resource_system.c`](../../runtime/src/renderer/systems/vkr_resource_system.c)
  inserts and removes one unique key per request. After about 500 requests
  it had no empty slot, so each lookup walked 128 slots under the
  resource-system mutex.
- `hash % capacity` read the low bits of FNV-1a, which depend only on the low
  bits of each byte. It also divided on every probe.
- The legacy font path formatted each codepoint as decimal text to find a
  glyph.

The audit also found two containers with one layout. `Array` had a fixed
length and `AlignOf(type)` storage, and `Vector` grew with the allocator's
default alignment. Five element types had both.

## Decision

**Array.** One type, `Array_T`, with fields allocator, capacity, length and
data, replaces `Vector`:

- `array_create_T(allocator, capacity)` reserves with length 0. Capacity 0
  returns an empty record that keeps the allocator.
- `array_create_filled_T(allocator, length)` allocates and zeroes `length`
  elements.
- `array_push_T` doubles the capacity when full, starting at
  `VKR_ARRAY_FIRST_GROWTH_CAPACITY`. `array_reserve_T` grows to an exact
  capacity. Both preserve the contents on failure.
- Storage uses `AlignOf(type)`. Growth allocates new storage, copies `length`
  elements and frees the old block.
- A successful push, reserve or grow can move the storage. Slot tables in the
  texture, material, geometry, mesh, camera, font and world-text owners are
  sized once at init and never grow. Each owner's header states this rule.

**Hash table.** Capacity is zero or a power of two. Each entry stores its
64-bit hash. The index is the FNV-1a hash after the MurmurHash3 fmix64
finalizer, masked by `capacity - 1`. A lookup compares the stored hash before
the key bytes. The table counts tombstones and keeps live keys plus
tombstones at or below three quarters of the capacity, so every probe
sequence ends at an empty slot and no probe limit exists. When an insert
needs room, the table rehashes at the same capacity if the live keys then
fill at most half of that limit; otherwise it doubles. A remove clears the
tombstones that an empty slot follows. An entry keeps the key pointer from
the insert that created it.

**Legacy glyphs.** `vkr_text_font_index_glyphs` builds `VkrFont::glyph_index`,
a codepoint-sorted array, and lookups use a binary search. Of repeated
codepoints, the first glyph wins.

**Other fixes.** Queue indices wrap without division. `bitset8_create` has a
`(void)` prototype, and the header documents single-bit flags.
`vkr_freelist_resize` takes the node buffer size.

## Consequences

- An insert of a new key can rehash and invalidate value pointers, as growth
  did before. A remove never moves entries.
- Capacity requests round up to a power of two, which can double a table's
  memory.
- Former `Array` records grow by 8 bytes for `capacity`.
- Slot-table pointer stability is an owner rule, not a type property.
- Verified on Windows with Vulkan: the Debug CPU suite (94 suites), the
  Debug and Release builds of every target except the editor, and a Release
  Bistro run of `tools/cases/smoke/bistro_shading_diagnostics.case.json` with
  `tools/profiles/local-offscreen.json` (pass, non-authoritative). The new
  tests cover a 200-key probe run, request-style churn at capacity 128, low
  hash bits, 64-byte alignment through growth and the legacy glyph index.
- Not verified: the editor build and a Bistro load in the editor (another
  session's unfinished editor edits did not compile on 2026-10-06), the
  macOS Metal build and CPU suite, and a matched Release before/after timing
  of resource request submission. No speed claim is made.

## Alternatives considered

- **Keep `Array` and `Vector` separate.** This keeps stability as a type
  guarantee for slot tables. It also keeps two APIs for one layout.
- **A fixed flag on the merged type.** This adds a branch to every growth
  call. The repository contract prefers typed tables over flag-driven helpers.
- **Inline small-buffer storage.** Constructors return records by value, so a
  record holding its own buffer would leave `data` pointing into the old copy.
- **An integer-key hash table for glyphs.** A sorted array needs no tombstones
  or growth, and fonts do not change after load.

## Revisit when

A hot path needs bounded stack storage that can grow; a caller-provided
initial buffer is the candidate. Also revisit when a hash-table workload
needs keys other than null-terminated strings.
