---
name: compress-codebase
description: Maps every in-scope source file and executes evidence-driven semantic compression passes that remove redundant checks, branches, comments, wrappers, representations, and shallow abstractions while preserving behavior, ownership, and measured performance. Use when auditing the codebase for LOC reduction, creating a file-by-file compression plan, or implementing staged code-compression and consolidation passes.
---

# Compress Codebase

## Contract

Compress relationships and states, not formatting. Preserve observable behavior,
public contracts, ownership, concurrency, lifetime, and measured hot-path
performance. A smaller file is not success when total maintained code or concept
count grows elsewhere.

Keep the result conventionally formatted and easy to review. Preserve meaningful
line breaks, indentation, and blank lines between distinct phases. Keep one
logical statement per line; do not fold control flow, declarations, or multiple
operations together to manufacture a lower LOC count. Run `clang-format` (LLVM
base, `.clang-format`) on touched code.

Map every file in scope before broad edits. Coverage means every original file
has an explicit disposition, including files deliberately kept unchanged.

## Select the mode

- **Map:** produce an exhaustive implementation handoff without editing source.
- **Execute:** build or refresh the map, then implement it as verified vertical
  slices. Never start a broad compression pass from aggregate LOC alone.
- **Resume:** reconcile the existing ledger against the current tree before more
  edits; do not silently skip or reinterpret mapped rows.

## Establish evidence

1. Read `AGENTS.md`, `docs/architecture/renderer-architecture-spec.md`, the
   relevant ADRs, and the dirty working-tree state.
2. Define scope and exclusions literally. Record commit, file count, physical
   and nonblank LOC, generated code, comments, and branch/check proxies.
3. Define behavioral, interface, ownership, concurrency, lifetime, and
   performance invariants **before** proposing any deletion.
4. Capture the smallest trustworthy baseline owned by each affected invariant:
   `./build_test.sh` at minimum, plus a Release measurement when a hot path is
   in scope.

## Map every file

Read [references/map-schema.md](references/map-schema.md). Run:

```sh
python3 .codex/skills/compress-codebase/scripts/inventory.py lib/src app/src \
  --output .codex/skills/compress-codebase/ledger.tsv
```

The script excludes `build*/`, `vendor/`, and `lib/src/vendor/` by default; pass
`--exclude <glob>` to add more and `--include-excluded` to audit them anyway.

Then inspect concrete callers and find at least two real examples before
claiming repetition. Complete every ledger row and produce a narrative map that
groups cross-file work into ordered vertical slices. Reconcile the inventory to
100% — vendored, generated, and retained files still need an explicit scope
decision.

## Choose compression

Read [references/compression-rules.md](references/compression-rules.md). Prefer,
in order: delete dead paths; merge duplicate facts into one owner; replace
parallel fields and `switch` clusters with typed tables; replace broad state
scans with state-specific worklists; collapse forwarding layers into deep
concrete modules; lower canonical data once.

Do not introduce macros, generic frameworks, flag-driven mega-functions,
compatibility aliases, or speculative seams to reduce local LOC. An abstraction
needs two semantically identical concrete uses and must reduce total
relationships.

## Execute vertical slices

1. Record the exact files, current LOC, repeated cases, surviving owner, checks
   removed or retained, lifetime implications, expected net deletion, and
   evidence owner.
2. Migrate one source of truth end to end. Delete the superseded representation
   and its adapters in the same slice when safe.
3. Keep external and fallible validation at its boundary. Internal code consumes
   caller-proven pointers, `String8` views, indices, or state-specific records.
4. Run the cheapest complete affected gate, recount the whole scope, and record
   both deleted and added lines. Moving code between files is not compression.
5. Stop and repair any behavior, lifetime, visual, or measured performance
   regression before starting the next slice.
6. Review the formatted diff as code, not only as metrics. Reject any slice
   whose LOC reduction depends on packed lines, collapsed blocks, or
   harder-to-scan control flow.

For `lib/src/renderer/` work, also use `vkr-renderer-design`. Add `vkr-memory`
when allocator, ownership, or lifetime is in scope; `vkr-performance` for hot
paths or speed claims; and `vkr-validation` when selecting or running evidence
gates. Preserve packet payload ownership, graph declaration versus frame payload
separation, acquire/release symmetry, and completion-gated resource identity.

## Finish

Re-inventory the final tree and reconcile every original and new file. Report
file and LOC deltas, removed codepaths and checks, surviving authorities,
validation results, exceptions, and unresolved risks. A map-only result must
state clearly that no implementation or runtime validation was performed.
