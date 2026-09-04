# Compression rules for C11

## Select a deletion

Inspect callers and data flow. Prefer these changes in order when they preserve
the affected invariants:

1. Remove dead APIs, fields, unreachable branches, and single-value modes.
2. Store each fact once and remove copies and synchronization code.
3. Merge repeated policy only when two concrete uses have the same semantics.
4. Replace parallel fields or switches that differ only in constants with typed
   tables, when traversal and layout fit the access pattern.
5. Build worklists for known states instead of rescanning every record.
6. Remove forwarding wrappers that add no ownership or policy.
7. Normalize input once into records that represent only valid combinations.

Keep distinct policy separate even when structs look alike. A new abstraction
must reduce callers' responsibilities or duplicated policy. Adding a similar
case should change its owning table or registration, without rewriting unrelated
callers or adding per-draw indirection. Ask the user immediately when selecting
the owner requires an unresolved architecture decision.

## Preserve readable C

- Follow `.clang-format`, retain braces, and use one logical statement per line.
  Packed statements and formatting-only deletions do not count as compression.
- Keep helpers `static` until another translation unit needs them. Prefer typed
  structs and functions to macros that hide allocation or control flow.
- Use `goto cleanup;` when multi-step resource creation needs shared teardown.
- Use `MemSet`, `MemCopy`, `MemZero`, and `MemCompare` from `defines.h`.
- Prefer designated initializers when they express the required initialization.
- Preserve known types instead of adding `void *`, unrelated mode flags, or
  compatibility wrappers that retain duplicate owners.
- Review the formatted diff. Names, control flow, and ownership must remain
  understandable without reconstructing a dense expression.

## Locate checks

| Location | Rule |
|---|---|
| File, config, OS, driver, public API | Validate fallible input once and produce normalized data. |
| Creation or compilation | Reject invalid combinations and size storage before use. |
| Internal helper | Consume caller-proven types and state. |
| Per-draw and other hot loops | No validation, recovery, null-guard, or assertion branches. Establish capacity and invariants before entry. |
| Async completion and retirement | Keep generation, last-use serial, fence, and state checks needed to prove reuse is safe. |

Capacity, generation, concurrency, and retirement checks may protect real
invariants. Move their proof to the owning boundary or change the representation
before deleting them. Do not remove a check solely because it looks defensive.

## Preserve storage and GPU lifetime

A borrowed pointer or view requires valid backing storage for every consumer.
Select allocation and reset/free boundaries using `vkr-memory`.

- An arena-backed `String8` ends at arena reset. `String8` is not internally
  null-terminated.
- Hash-table keys survive the entry. Free a removable key after removal finishes
  comparing against the stored pointer.
- Array growth may invalidate pointers. Indices remain usable only while the
  indexed elements retain their position. Reserve before publishing views or
  use storage that remains stable for all consumers.
- Packet payloads remain caller-owned for the renderer's documented consumption
  interval. Verify the submit/copy contract before changing their lifetime.
- Preserve declared graph resource use, acquire/release symmetry, and
  completion-gated physical resource reuse.

Keep allocations, pipeline creation, handle churn, string construction, locks,
and blocking waits out of per-draw work. A smaller diff cannot justify earlier
GPU resource reuse or a longer allocation lifetime without evidence.

## Comments and evidence

Remove comments that restate code. Keep ownership, units, valid ranges,
synchronization, legal attribution, and required tool directives near their
code. Record architecture rationale in an ADR only when it constrains future
work. Bulk comment deletion without a reduction in duplicated facts is not
compression.

Count the entire scope after each change, including new files and helpers.
Moving code between files is not net deletion. Use `vkr-validation` for the
smallest evidence of the changed behavior and `vkr-harness` for renderer cases.
A new unit test needs a specific failure it detects and a reason existing
harness/build evidence is insufficient. Use `vkr-performance` for Release
measurements and `vkr-shaders` for native Metal/Vulkan parity. Repair behavioral,
visual, lifetime, or measured performance regressions before the next change.
