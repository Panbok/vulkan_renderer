# VKR agent contract

## Engineering priorities

**Performance is correctness.** VKR is a C11 renderer with Metal 4 and Vulkan
1.4 packet implementations. A missed frame budget is a defect. A speed claim
requires matched Release measurements; preserving output, ownership and GPU
completion is a prerequisite for comparing those measurements.

- Write simple C with explicit data flow, typed records and one owner per fact.
  Prefer a local function or table over a forwarding layer, generic interface
  or flag-driven helper. An abstraction needs concrete callers with the same
  policy. Remove the replaced representation in the same change.
- Validate and normalize at input, creation and state-transition boundaries.
  Per-draw, per-element and command-emission loops consume proven data without
  defensive validation, recovery, null guards or assertions. Partition optional
  work before those loops. Preserve real capacity, generation and completion
  proofs at their owning boundaries.
- Keep heap allocation, string construction, locks, pipeline creation and
  blocking waits out of per-draw work. Frame-slot reuse must still prove GPU
  completion. Do not remove synchronization to improve a timing number.
- Choose memory by lifetime and access pattern before writing the allocation.
  Reuse owned capacity or bounded stack storage when the lifetime permits.
  Use arenas for one bulk lifetime, DMemory for independently freed objects,
  and pools for fixed-size churn. State the owner, release point, pointer
  stability and GPU last use. Pre-size hot-path storage; measure growth and
  retained memory. `vkr-memory` owns the concrete allocator rules.
- Metal and Vulkan share portable rendering semantics, data contracts and
  feature behavior. Backend-specific mechanisms or performance features need
  an explicit capability boundary and measured justification. Preserve accepted
  exceptions in the ADRs. `vkr-shaders` owns shader efficiency, ABI and native
  parity evidence; a one-backend build never proves full compatibility.

## Architecture decisions

Before implementation, identify the owner, data lifetime, affected contract
and smallest verification that can falsify the change. Follow the existing
accepted architecture when it answers those questions.

If a choice changes ownership, a public or shader contract, graph scheduling,
resource lifetime, portability, or a quality/performance budget and the request
or accepted design does not settle it, ask the user immediately. Give the
concrete choice, your recommendation and its tradeoff in a few sentences.
Pause only the dependent work. Resolve facts from code and tools yourself.
Do not defer user decisions to a document footer, TODO list or final report.
Record the answer after it is made; a design document cannot substitute for it.

## Load only the needed context

For renderer changes or diagnosis, consult these in order, reading the relevant
sections rather than the entire tree:

1. `docs/ARCHITECTURE.md`: current frame flow, subsystem ownership, capability
   boundaries, and known implementation gaps. Read only affected sections.
2. `docs/INDEX.md`: find the ADRs that constrain this task and read those decisions.
3. `docs/CONTEXT.md`: resolve unfamiliar project terms. `docs/proposals/` contains
   future work and does not establish implemented behavior.

Code defines current implementation, `docs/ARCHITECTURE.md` defines recorded
status, and ADRs define accepted rationale. Investigate conflicts before editing;
proposal text alone does not prove a feature exists. For tooling or prose-only
work, inspect its owner directly without loading unrelated renderer documents.

## Skills and task flow

Skill paths below are relative to `.codex/skills/`. Load only matching skills
and only the references needed for the current step. Apply `unslop` to writing.

| Task | Skill entry point |
| --- | --- |
| Multi-step investigation or implementation, delegation, resume | `vkr-task-workflow/SKILL.md` |
| Renderer architecture, graph, backend, hot paths | `vkr-renderer-design/SKILL.md` |
| Shader source or shader-visible host contract, either backend | `vkr-shaders/SKILL.md` |
| Allocation, ownership, lifetime, reload or growth | `vkr-memory/SKILL.md` |
| Optimization, timings or performance claims | `vkr-performance/SKILL.md` |
| Harness cases, profiles, captures, reports or baselines | `vkr-harness/SKILL.md` |
| Choosing or running verification | `vkr-validation/SKILL.md` |
| File-by-file source compression | `compress-codebase/SKILL.md` |
| Documents, ADRs or their indexes | `vkr-docs/SKILL.md` |
| Skills or agent instructions | `writing-for-agents/SKILL.md` |
| Requested decision stress test | `grilling/SKILL.md` |
| Written responses and edited prose | `unslop/SKILL.md` |

Answer focused questions directly. Multi-step work uses one compact local note
in `.scratch/` with scope, decisions, evidence and next action. Use subagents
for independent bounded work when parallel progress pays for the handoff;
`vkr-task-workflow` owns model selection and write coordination. Do not create
persistent agent roles or configuration merely to delegate a task.

## Verification loop

1. Name the behavior, failure or cost being checked and select the smallest
   existing tool/case that can reveal it. Verify the executable and options
   exist before relying on a command from prose.
2. Establish the relevant failing case or baseline, make the change, then rerun
   that check. Read the report, capture or diagnostic, not just the exit code.
3. Add a check only when a changed invariant remains uncovered. Stop when the
   selected checks pass; rerun or broaden them only for a new edit, failure or
   unresolved concern.

Unit tests are not a default deliverable. Add or run one only after naming the
specific failure it detects, an independent expected result, and why a build,
harness case or existing check is insufficient or more expensive. Tests that
mirror implementation, mock the renderer into success or assert source wording
provide no acceptance evidence. `vkr-validation` owns the selection rules.

Use the repository build wrappers; they own shader compilation and asset cooking.
`./build_release.sh` builds the Release app and `vkr_harness`.
`./build.sh Debug` and `./build_editor.sh Release` select app/editor builds.
`./build_test.sh` builds and runs the CPU suite only when justified;
`./build_test_batch.sh` repeats it for a concrete intermittent failure.
Core wrappers have `.bat` counterparts; POSIX execution does not validate them.

Use normal Release with graphics validation variables unset for snapshots,
baselines and performance. Debug, sanitizers and API/GPU validation diagnose a
specific issue in a separate focused run. Metal validation is opt-in through
`MTL_DEBUG_LAYER` and `MTL_SHADER_VALIDATION` before device creation. Run one
Metal validation process at a time; never run a broad shader-validation capture
suite. A prior broad run preceded a watchdog panic. CPU tests do not validate
Vulkan API use, and Metal execution does not validate native Vulkan.

## C conventions

- C11; `bool8_t`, `bool32_t`, `uint32_t`, `float32_t`, `float64_t`.
- `String8` carries a length and is not internally null-terminated.
- `Array(T)` and `Vector(T)` use `.data`, `.count`, `.capacity`; growth may
  invalidate borrowed pointers.
- Use `MemSet`, `MemCopy`, `MemZero`, `MemCompare` from `defines.h`.
- Names: `snake_case` functions/locals, `Vkr*` types, `vkr_*` public functions,
  `s_` internal structs behind opaque handles.
- Return `bool8_t` or an error enum. Use `goto cleanup;` for shared teardown
  after partial initialization. Preserve fallible API error propagation.
- Follow `.clang-format`. Comments explain ownership, units, ordering or
  non-obvious synchronization; names and code express ordinary operations.
- Production shaders include Slang and native Metal sources. Use the build
  scripts to compile the affected production entry points.

## Preserve context and evidence

Preserve pre-existing work. Keep `.codex/skills/` and `.claude/skills/` as
byte-identical regular-file copies, including scripts and metadata. Check with
`diff -rq .codex/skills .claude/skills`. `CLAUDE.md` points here. Update affected
status/rationale documents in the same change when their claims move. Keep
`docs/` limited to `CONTEXT.md`, `INDEX.md`, `ARCHITECTURE.md`, `adr/`, and
`proposals/`. Merge superseded rationale into its current ADR and delete obsolete
documents; Git preserves history. `docs/INDEX.md` indexes every retained document.

Copy decisive results, exact commands and report digests out of regenerable
run trees. For captures another machine must consume, follow `vkr-harness`'s
guarded publication rules before deleting the only payload. Preserve a pending
payload when publication needs a user decision, and ask immediately. Delete
only this task's disposable runs and traces after recording their results.

Commits use a scoped Conventional Commit, such as `fix(renderer): ...`. PRs
state the behavior change, exact verification commands and relevant issues.
Rendering/UI changes include a screenshot or clip and identify shader/asset
changes. Report unrun native checks as unavailable; never imply they passed.
