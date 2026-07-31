# Semantic compression rules (C11)

## Contents

- [Success measure](#success-measure)
- [Human-readable code](#human-readable-code)
- [Compression order](#compression-order)
- [C-specific patterns](#c-specific-patterns)
- [Bare-minimum checks](#bare-minimum-checks)
- [Borrowed-view lifetime](#borrowed-view-lifetime)
- [Comments](#comments)
- [Architecture tests](#architecture-tests)
- [Reject](#reject)
- [Renderer overlay](#renderer-overlay)

## Success measure

Optimize lifetime maintenance cost: concepts, relationships, states, codepaths,
files, branches, checks, comments, allocations, and total maintained LOC. Prefer
readable procedural locality over terse syntax. Count the full scope after every
slice. Treat formatting-only LOC changes as neutral — semantic compression comes
from removing maintained behavior or representation, never from packing the same
code onto fewer lines.

## Human-readable code

- Follow `.clang-format` (LLVM base) and the established layout.
- Keep one logical statement per line. Do not join declarations, assignments,
  calls, branches, or loop bodies merely to reduce physical LOC.
- Preserve braces and indentation that expose control-flow structure. Do not
  drop braces from single-statement `if`/`for` bodies to save a line.
- Use blank lines to separate coherent phases — validation, setup, core work,
  submission, cleanup — without fragmenting a single phase.
- Keep closely related operations together. Extract a well-named helper only
  when it reduces relationships or makes a genuinely distinct block clearer.
- Prefer readable multi-line initializers and argument lists when a single line
  is hard to scan. Designated initializers on their own lines are cheaper to
  read and to diff than a packed one-liner.
- Review the final formatted diff manually. If a human must mentally unpack a
  dense expression or collapsed block, expand it and do not treat the added
  lines as a regression.

## Compression order

1. Delete dead APIs, unused fields, unreachable branches, one-value modes, and
   translation-unit anchors that exist only to satisfy an old link order.
2. Select one authoritative representation for each fact and delete the mirrors.
3. Merge repeated handle tables, pools, binary cursors, cache I/O, descriptor
   schemas, upload rings, or work records — only after two concrete policies
   actually match.
4. Replace parallel named fields and `switch` clusters with typed indexed
   records and descriptor tables.
5. Replace "scan every record and ask every state question" with state-specific
   worklists and explicit transitions.
6. Collapse forwarding ladders — wrapper functions that only re-call a single
   callee, or a header indirection with one implementation — into a deep
   concrete owner.
7. Make invalid combinations unrepresentable with tagged records, fixed ranges,
   and state-specific operations. Lower once.

Keep unique policy local. Do not unify domains merely because their struct
layouts look similar.

## C-specific patterns

**Prefer**

- `static` helpers inside the `.c` file. Promote to a header only when a second
  translation unit genuinely needs it.
- `static inline` functions and small structs over function-like macros.
- Data tables — arrays of descriptor or config structs iterated in a loop —
  instead of N near-identical blocks or a long `switch` whose arms differ only
  in constants.
- A single cleanup path with `goto cleanup;` for multi-step resource creation.
  This is the project's idiom for acquire/release symmetry; do not replace it
  with duplicated teardown at each early return.
- `MemSet`/`MemCopy`/`MemZero`/`MemCompare` from `defines.h` over raw `<string.h>`
  calls.
- Zero-initialized designated initializers over a `MemZero` followed by field
  assignments.

**Avoid**

- Macros that hide control flow, allocation, or an early return.
- "God helpers" configured by many unrelated flags — that is a granularity
  failure, not compression.
- `void *` parameters that erase a type a caller already knows.
- Indirection that hides who owns memory or which allocator freed it.

## Bare-minimum checks

| Seam | Policy |
| --- | --- |
| Untrusted input: config, file, shader, glTF, KTX2, OS, driver | Validate and normalize once; use one checked reader or table rather than repeated local arithmetic. |
| Public handle or API | One canonical lookup or input validation, then pass pointers/indices internally. |
| Creation / compile boundary | Reject invalid combinations and emit canonical typed records. |
| Internal `static` helper | Accept caller-proven types; remove defensive null/empty/state ladders. |
| Hot loop / lowering | No recoverable validation; at most a debug assertion for a proven invariant. |
| Async completion, resource pump | Keep stale-generation and state checks — reuse and out-of-order completion are real here. |
| Ownership, concurrency, submission | Keep checks that are clocks or proofs: capacity, generation, lock ownership, submit serial, last use, fence completion, retirement. |

Several guards in one internal function usually mean its parameter type is too
broad or it owns several state-machine phases. Change the representation before
deleting guards blindly.

## Borrowed-view lifetime

Never remove count, reserve, fixed-capacity, arena, or retention machinery
merely because it resembles boilerplate. A published pointer, `String8`,
`Array(T)`/`Vector(T)` view, index, or Vulkan handle requires stable backing
storage for the complete consumer lifetime.

Specific hazards in this codebase:

- `String8` is length-prefixed and **not** null-terminated internally. A
  `String8` pointing into an arena dies when that arena is reset or destroyed.
- Hash-table keys must point to stable memory for the lifetime of the entry. If
  entries are removed on unload, allocate keys from a freeable allocator and
  free them **after** the remove call — removal probes compare against the
  stored key pointer.
- `Array(T)`/`Vector(T)` growth reallocates. A pointer or index range taken
  before a push may dangle after it.
- `VkrRenderPacket` payload arrays are caller-owned and must outlive
  `vkr_renderer_submit_packet()`.

Before publishing a view, use one proven policy: exact pre-reservation before
any view is formed, fixed-capacity storage, or an owning arena whose scope
dominates every consumer. Reallocation after publication is a use-after-free
even when every value was valid when created. See `vkr-memory`.

## Comments

Remove narrative comments once the code structure expresses the fact through
domain names, typed states, and explicit data flow. Move lasting rationale and
invariants into `docs/` or an ADR. Keep:

- ownership and lifetime statements (who allocates, who frees, what survives a
  scope);
- valid ranges, sentinel values, and units;
- threading assumptions — which thread calls this, what must be synchronized;
- Vulkan synchronization and ordering constraints that are easy to violate;
- legal attribution and required tool directives.

Bulk comment deletion without structural repair is not compression.

## Architecture tests

- One fact has one owner and one lowering.
- A reusable abstraction has two semantically identical concrete uses.
- A module is deep: small interface, substantial hidden policy, no pass-through
  peer layer.
- Adding N+1 changes one descriptor, table, tagged record, or real registration
  without rewriting unrelated callers or adding hot-loop indirection.
- High-level operations retain usable lower-level granularity.
- New helpers and modules delete more code and relationships than they add.

## Reject

- minification, packed statements, collapsed blocks, or cryptic names;
- macros that hide ordinary control flow only to reduce physical LOC;
- flag-driven mega-functions combining unrelated policy;
- speculative interfaces with one implementation;
- compatibility aliases that keep the old and new authorities alive together;
- moving code between files without net scope reduction;
- deleting error handling at a fallible boundary;
- deleting capacity, generation, synchronization, or retirement proof;
- performance claims without same-configuration measured evidence.

## Renderer overlay

For `lib/src/renderer/`, additionally preserve: visual output; the
prepare/submit ordering contract; graph declaration versus frame payload
ownership; exact declared resource use; handle acquire/release symmetry;
completion-gated physical resource identity. Keep pipeline creation, allocation,
handle churn, string construction, locks, and waits out of per-draw work. Use
focused tests for deterministic ownership, `./build_test.sh` for the CPU suite,
Vulkan validation layers for API correctness, and isolated Release benchmarks
for performance.
