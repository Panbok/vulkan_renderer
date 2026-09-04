---
name: compress-codebase
description: Audit source for redundant state, code, checks, and abstractions; map files and implement scoped compression passes.
---

# Compress codebase

Remove redundant representations and work. Preserve behavior, ownership,
concurrency, GPU lifetime, and measured performance. Keep conventional C layout
and format touched C code with `clang-format` using the repository
`.clang-format` configuration. Formatting changes do not count as compression.

## Scope and evidence

1. Use `vkr-task-workflow` for scope and local task state. Inspect the dirty tree.
2. Choose the requested mode: map only, implement, or resume an existing map.
   For resume, reconcile the map with the current tree before editing.
3. Define included paths and exclusions. Record the commit and initial inventory.
4. Name the behavior, ownership, lifetime, and performance invariants affected.
   Use `vkr-validation` to choose the smallest evidence that checks those
   invariants. A CPU suite is not a mandatory baseline. Add a unit test only
   when a specific failure and its detection value justify it.
5. Ask the user immediately if the surviving owner, public contract, allocator
   lifetime, or backend behavior requires an unresolved architecture decision.
   State the concrete choice and recommendation. Continue independent work.

## Map

Read [the ledger schema](references/map-schema.md) for a broad or resumable
pass. A focused change can use a short file list in the task note. Replace
`compression-audit` below with the current task slug.

```sh
python3 .codex/skills/compress-codebase/scripts/inventory.py lib/src app/src \
  --output .scratch/compression-audit/compression-before.tsv
```

The script inventories known source extensions, including `.metal`, `.metalh`,
`.slang`, `.slangh`, and `.inc`.
Explicit files are included regardless of extension. It excludes root `build*/`,
`vendor/`, `lib/src/vendor/`, and generated SPIR-V by default. Use `--exclude`
for additional repository-relative globs; `--include-excluded` removes only the
default exclusions. Directory symlinks are not traversed. Counts are lexical
proxies, not parsed code facts. Inspect callers before claiming dead code or
repetition. Deduplication requires two uses with the same policy.

Every included file needs a disposition. Keep inventory data under
`.scratch/<task-slug>/`; use the existing workflow note for decisions and the
compression plan. Keep a durable architecture decision in `docs/` only when
needed, following `vkr-docs`. Generate the final inventory at a new path so the
baseline and completed ledger survive.

## Implement

Read [compression rules](references/compression-rules.md) when selecting edits.
For each independently verifiable change:

1. Name exact files, the representation being removed, its surviving owner,
   required invariants, and the verification command.
2. Migrate the fact end to end and delete the old representation and adapters.
3. Validate external input at its boundary. Give internal hot paths normalized
   records with no validation or assertion branches.
4. Run the affected evidence, inspect the formatted diff, and recount the full
   scope including new files. Repair regressions before continuing.

Use `vkr-renderer-design` for renderer changes, `vkr-memory` for allocation or
lifetime changes, `vkr-shaders` for shaders or their host contracts, and
`vkr-performance` for hot-path changes or speed claims. Shader changes preserve
Metal/Vulkan behavior; backend-specific optimizations need measured evidence.

## Complete

Reconcile all original and new files. Report net LOC, removed representations,
verification commands and results, and any unavailable evidence. A map-only
result states that source and runtime behavior were not changed or validated.
Architecture questions must be asked when discovered, not left in this report.
