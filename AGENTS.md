# VKR repository contract

VKR is C11 with Metal 4 and Vulkan 1.4 packet implementations. Preserve output,
ownership, GPU completion, supported behavior, and the frame budget. A speed
claim requires matched Release measurements.

## Scope and discovery

A review produces findings; an implementation or fix request authorizes the
in-scope edits needed to deliver it. Preserve existing work. Inspect the owning
source, callers, and relevant accepted ADR before changing a contract. Use
`docs/INDEX.md` to find decisions and read only affected sections of
`docs/ARCHITECTURE.md`. Proposals do not establish behavior.

Ask only for an unresolved decision that changes scope, ownership, public or
shader behavior, portability, or an accepted budget. Give a recommendation and
tradeoff immediately; continue independent work. Search symbols before opening
large files. Exclude generated payloads, assets, third-party code, and build
output unless they are the subject.

## Readable C and reuse

Use one statement per line, braces for control-flow bodies, a blank line between
functions, and separation between logical stages. Do not join declarations or
shader attributes to a preceding closing brace. Never remove useful whitespace,
comments, or names to save tokens or LOC. Follow `.clang-format` for supported
languages and inspect the diff; do not apply an unverified C formatter to Slang
or Metal source.

Use project types (`bool8_t`, `bool32_t`, `float32_t`, `float64_t`, fixed-width
integers), `true_v`/`false_v`, `snake_case` names, `Vkr*` types, `vkr_*` public
functions, and `s_` private structs behind opaque handles. Keep helpers internal
until another translation unit needs them. Prefer designated initializers and
`goto cleanup;` for partially acquired resources. Preserve fallible API errors.
`String8` is not internally null-terminated; container growth can invalidate
borrowed pointers.

Before adding a utility, inspect `lib/src/defines.h` and the owning library API.
Use `MemZero`, `MemSet`, `MemCopy`, `MemCompare`, `ArrayCount`, and other project
helpers when their semantics fit. `MemCopy` permits overlap; `ArrayCount` needs
an actual array. Evaluate side-effecting inputs before multi-evaluation macros.
Do not replace shader intrinsics with host macros, force-inline by habit, or
duplicate a defective primitive instead of fixing its owner. Aggregate zero
initialization remains valid.

Extend the existing owner by default. A new source/header needs an independent
responsibility, lifetime, reusable contract, or backend/build boundary. File
length alone never justifies a split or merge. Prefer a local function or typed
table over a forwarding layer, generic interface, or flag-driven helper. Remove
a replaced representation and its adapters in the same change.

## Correctness, memory, and shaders

Validate and normalize at input, creation, publication, and state-transition
boundaries. Repeated hot work consumes proven data. Before removing a check,
name its invariant and surviving proof. Keep numerical, capacity, generation,
rounded-dispatch edge, and GPU-completion checks where they own a real invariant.
Partition optional work before per-draw and command-emission loops. Keep heap
allocation, strings, locks, pipeline creation, handle churn, and blocking waits
out of those loops.

Choose memory by lifetime and access pattern before allocation. Prefer bounded
stack storage or reused capacity. State the owner, release point, pointer
stability, and GPU last use. Frame age is not GPU completion. `vkr-memory` owns
allocator, borrowed-view, and retirement rules.

Metal and Vulkan share portable rendering semantics, contracts, and feature
behavior. A backend-specific mechanism needs an explicit capability boundary
and measured justification. Every production shader or shader-visible host
change uses `vkr-shaders` and inspects both native roots, shared helpers, host
layout, bindings, and consumers. A one-backend build, source review, or
cross-compilation does not establish native parity.

## Skills and task flow

Skill paths are relative to `.codex/skills/`. Load only the applicable entry and
conditional references needed for the current step:

- `vkr-task-workflow` for multi-step, resumable, or delegated work.
- `vkr-renderer-design` for architecture, graph, backend, and hot-path changes.
- `vkr-shaders` for shaders and shader-visible host contracts.
- `vkr-memory` for allocation, ownership, lifetime, reload, and growth.
- `vkr-harness` for cases, profiles, captures, reports, and baselines; add
  `vkr-performance` for timing claims or `vkr-validation` for native diagnostics.
- `compress-codebase` for broad simplification, `vkr-docs` for authoritative
  documentation, and `writing-for-agents` for skills or agent instructions.
- `grilling` only when the user asks to be challenged.

Answer focused questions and perform one-step edits directly. Multi-step work
uses one compact `.scratch/` note. Delegate only independent bounded work when
parallel progress repays the handoff; use one writer per overlapping file and
serialize GPU runs. Do not create persistent agent roles to delegate a task.

## Evidence and completion

Use Bistro for every scene-based task. Never select a Sponza scene, case,
capture, baseline, or example for performance, validation, smoke, visual work,
or any other purpose. Small synthetic fixtures are allowed only for isolated
defects. If no compatible Bistro case exists, report the gate unavailable or
add a scoped Bistro case; never weaken the profile or substitute another scene.

Select the cheapest independent check that detects the changed invariant.
Existing deterministic CPU tests are valid when they provide the most direct
oracle. Add a test only when it detects a named failure independently; reject
tests that mirror implementation, mock success, or assert source wording. A
build, CPU test, capture, native diagnostic, and timing report prove different
claims. Inspect reports and diagnostics, not only exit codes.

Use repository build wrappers; they compile shaders and cooker tools but do not
cook or publish assets. Bakery or an explicit cooker wrapper owns generation.
Use normal Release with graphics validation variables unset for snapshots,
baselines, and performance. Debug, API/GPU validation, and traces are focused
diagnostics. Run one Metal validation process at a time and never run a broad
shader-validation capture suite; a prior broad run preceded a watchdog panic.
CPU tests do not validate Vulkan API use, and Metal does not validate Vulkan.

Record exact commands, configuration, report digests, decisive results, and
unavailable gates. Timing claims require compatible capture-free Release
before/after reports, valid samples and spread, equivalent work/output, and
passing authority policy. Never publish a baseline without authorization.
Preserve pending payloads and other tasks' output during cleanup.

Keep `.codex/skills/` and `.claude/skills/` as byte-identical regular-file
copies, including scripts and metadata. `CLAUDE.md` imports this file. Update
affected authoritative docs when claims move; `docs/INDEX.md` inventories every
retained document. Commits use scoped Conventional Commit messages. PRs state
behavior, exact verification, relevant issues, and unavailable native checks;
rendering/UI changes include a screenshot or clip and identify asset changes.
