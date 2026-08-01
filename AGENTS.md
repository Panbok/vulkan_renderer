# AGENTS

Single source of truth for both Claude and Codex. `CLAUDE.md` only points here.

## Guiding principle

**Performance is correctness.** This is a renderer. A frame that misses its
budget is a failed frame, and a per-draw heap allocation, blocking wait, string
construction, or lock is a defect — not a style preference.

The counterweight that keeps this honest: **an unmeasured performance claim is
not a result.** Ownership, lifetime, and GPU-completion invariants are what make
a measurement mean anything; a faster frame that reuses a resource before the
GPU is done with it has measured nothing. Never trade an invariant for a number.

## Project

Vulkan 1.2 renderer and engine framework in C11 — ~212 source files, ~115k LOC.
The frontend (`lib/src/renderer/renderer_frontend.c`) owns the subsystems and
reaches the Vulkan backend only through the `VkrRendererBackendInterface`
function-pointer table; Vulkan types stay behind `lib/src/renderer/vulkan/`.
Frame orchestration is a JSON-authored render graph
(`assets/render_graphs/main.rendergraph.json`) with named pass executors in
`lib/src/renderer/passes/`, driven by packet submission:
`vkr_renderer_prepare_frame()` then `vkr_renderer_submit_packet()`.

There is no view/layer system. `VkrViewSystem` and `VkrLayer` were removed; any
document or comment describing them is historical.

## Start here

For any renderer task, read in this order:

1. `docs/architecture/renderer-architecture-spec.md` — **status authority**.
   §4 is the feature table, §8 is the prioritized known-issues list. Check §8
   before "fixing" anything; most surprising behaviour is already logged there.
2. `docs/architecture/adr/README.md` — **rationale authority**, 15 ADRs covering
   the backend seam, render graph, packet API, reflection, allocators, GPU
   memory, synchronization, ECS, Vulkan baseline, texture compression, and draw
   submission, plus proposed present-target and metrics-registry seams.
3. `docs/README.md` — complete index of the doc tree.

Conflict rule: **code is the implementation authority**, the architecture spec
is the status authority, and ADRs are the rationale authority. A design
document's existence is not evidence that its feature ships.

## Skills

Skills live in `.codex/skills/` and are linked into `.claude/skills/` by tracked
symlinks, so both agents read the same files. This is the only place they are
enumerated.

| Task | Skill |
|---|---|
| Multi-step research, diagnosis, implementation, subagent delegation, task notes | `.codex/skills/vkr-task-workflow/SKILL.md` |
| Renderer architecture, refactors, backend/graph/pass changes, hot paths, API design | `.codex/skills/vkr-renderer-design/SKILL.md` |
| Auditing for LOC reduction, file-by-file compression plans, consolidation passes | `.codex/skills/compress-codebase/SKILL.md` |
| Frame-time or hitch investigation, optimization, benchmarks, any speed claim | `.codex/skills/vkr-performance/SKILL.md` |
| Structured renderer cases, profiles, reports, and deterministic repetitions | `.codex/skills/vkr-harness/SKILL.md` |
| Allocator choice, ownership, lifetime, hash keys, scene reload growth, leaks | `.codex/skills/vkr-memory/SKILL.md` |
| Deciding on and running tests, validation layers, pipeline-cache and backend matrices | `.codex/skills/vkr-validation/SKILL.md` |
| Writing or updating docs and ADRs, marking a proposal shipped, archiving | `.codex/skills/vkr-docs/SKILL.md` |

`vkr-renderer-design` carries the compression, N+1, and hot-path rules in
`PRINCIPLES.md`, this repository's real backend seams and its currently-known
gaps in `VULKAN_PATTERNS.md`, and audit sequencing in `WORKFLOW.md`.

`vkr-task-workflow` is the entry point for multi-step work. Classify every
request first: answer a focused question, a brief explanation, or a status check
directly. It routes to the skills above rather than restating them, and owns
task notes under `.scratch/`, which is local and untracked.

## Build, test, validate

```sh
./build.sh [Debug|Release]     # compile shaders, configure, build, copy assets
./build_run.sh [Debug|Release] # build and launch
./build_release.sh             # optimized build -> build_release/app/vulkan_renderer
build_release/tools/vkr_harness profile --case <case> --profile <profile>
./build_test.sh                # build + run the CPU suite
./build_test_batch.sh          # 50 runs; confirm or refute a flake
./validate_pipeline_cache.sh   # cold/warm pipeline cache behaviour
tools/validate_multithreaded_backend_matrix.sh   # backend threading matrix
tools/benchmark_multithreaded_backend.sh         # Release perf harness -> summary.csv
tools/pack_vkt_textures.sh                       # offline KTX2/UASTC packing
```

The core build/run/test wrappers have `.bat` equivalents. The pipeline-cache,
backend-matrix, benchmark, and texture-packing utilities are currently POSIX
shell only. Use the repository scripts rather than invoking `cmake` directly —
they own shader compilation and asset copying.

A green CPU suite is not evidence that Vulkan usage is correct. Anything that
records commands or transitions resources needs a validation-layer run.

## Code conventions

- C11. Custom scalars: `bool8_t`, `bool32_t`, `uint32_t`, `float32_t`,
  `float64_t`. `String8` is length-prefixed and **not** null-terminated
  internally.
- `Array(T)` / `Vector(T)` macros generate `Array_T` with `.data`, `.count`,
  `.capacity`.
- Prefer `MemSet` / `MemCopy` / `MemZero` / `MemCompare` from `defines.h`.
- Naming: `snake_case` functions and locals, `Vkr*` types, `vkr_*` public APIs,
  `s_` prefix for internal structs behind opaque handles.
- Errors: return `bool8_t` or an enum error code. Prefer one cleanup path
  (`goto cleanup;`) over duplicated teardown.
- Format with `clang-format` (LLVM base, `.clang-format`); `clang-tidy` is
  configured via `.clangd`.
- Document intent, invariants, ownership/lifetime, ordering, and non-obvious
  Vulkan synchronization. Do not restate the code — prefer a better name over a
  comment.
- Shaders are Slang (`.slang` → `.spv`), compiled by the build scripts.

## Logging

`lib/src/core/logger.h` — `log_fatal`, `log_error`, `log_warn`, `log_info`,
`log_debug`, `log_trace`.

- `fatal` — the path will crash or abort.
- `error` — a recoverable error returned to a caller that can handle it.
- `warn` — unexpected but not critical to function.
- `info` — user-relevant runtime information: device info, startup phases.
- `debug` / `trace` — creation and initialization tracing, **non-hot paths
  only**. Logging in a per-draw path is a performance defect.

## Events

`lib/src/core/event.c/h` is a thread-based event system used for cross-system
communication. **Callbacks run on a separate thread**: any external data a
callback references must be copied into event-owned storage or protected by
synchronization primitives. See `lib/src/application.h` for usage.

Prefer events for cross-system communication unless strictly synchronous
communication is required.

## Agent context

`AGENTS.md`, `CLAUDE.md`, `docs/`, `.codex/skills/`, and the
`.claude/skills/` symlinks are committed repository context. Keep them in the
same change when an architectural decision, status claim, or skill rule moves.
Start documentation discovery at `docs/README.md`.

Task notes under `.scratch/` are local and untracked; move anything that must
survive into `docs/`.

## Commits and PRs

- Conventional Commits with scope: `feat(renderer): ...`, `fix(fonts): ...`,
  `ref(renderer): ...`.
- PRs: clear summary, the exact commands run, and linked issues.
- For rendering or UI changes, include a screenshot or short clip and note any
  shader or asset updates.

## Configuration

Required: CMake 3.27+, Vulkan SDK, `slangc`. Optional: set `VCPKG_ROOT` to use
the vcpkg toolchain. Build scripts prefer Ninja and clang when available.
