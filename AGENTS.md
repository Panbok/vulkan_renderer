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

**Branchless by construction.** Validate and normalize at cold boundaries, then
trust internal data. Hot paths contain no validation, recovery, null-guard, or
assertion branches. Shape or partition data, or compute cheap derived values, so
optional cases disappear; never rely on the compiler to remove redundant guards.

## Project

Renderer and engine framework in C11 with selected Metal 4 and Vulkan 1.4
bindless packet strategies.
The frontend (`lib/src/renderer/renderer_frontend.c`) owns the subsystems and
selects one coarse `VkrRendererImpl` strategy at initialization. Vulkan types
stay behind `lib/src/renderer/vulkan/`; the Vulkan 1.2 adaptor and
generic backend interface were removed by ADR-026.
Frame orchestration is a JSON-authored render graph
(`assets/render_graphs/main.rendergraph.json`) realized and executed inside the
selected implementation, driven by packet submission:
`vkr_renderer_prepare_frame()` then `vkr_renderer_submit_packet()`.

There is no view/layer system. `VkrViewSystem` and `VkrLayer` were removed; any
document or comment describing them is historical.

## Start here

For any renderer task, read in this order:

1. `docs/architecture/renderer-architecture-spec.md` — **status authority**.
   §4 is the feature table, §8 is the prioritized known-issues list. Check §8
   before "fixing" anything; most surprising behaviour is already logged there.
2. `docs/architecture/adr/README.md` — **rationale authority**, 26 ADRs covering
   the backend seam, render graph, packet API, reflection, allocators, GPU
   memory, synchronization, ECS, Vulkan baseline, texture compression, and draw
   submission, plus proposed present-target and metrics-registry seams.
3. `docs/README.md` — complete index of the doc tree.

Conflict rule: **code is the implementation authority**, the architecture spec
is the status authority, and ADRs are the rationale authority. A design
document's existence is not evidence that its feature ships.

## Skills

Skills live in `.codex/skills/` and are mirrored into `.claude/skills/` as
tracked regular-file copies, so both agents read the same content. Editing a
skill means writing both trees and leaving them byte-identical; `diff -rq
.codex/skills .claude/skills` is the check. This is the only place they are
enumerated.

| Task | Skill |
|---|---|
| Multi-step research, diagnosis, implementation, subagent delegation, task notes | `.codex/skills/vkr-task-workflow/SKILL.md` |
| Named renderer agents, root-only Sol routing, cold subagent context, delegation order | `.codex/skills/vkr-agent-team/SKILL.md` |
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
task notes under `.scratch/`, which is local and untracked. When it selects
delegation, load `vkr-agent-team` for the repository's named roles, strict model
routing, minimal child context, and orchestration order; task lifecycle and
evidence remain with `vkr-task-workflow`.

## Build, test, validate

```sh
./build.sh [Debug|Release]     # Debug -> build_debug; Release -> build_release
./build_run.sh [Debug|Release] # build and launch
./build_release.sh             # optimized build -> build_release/app/vulkan_renderer
build_release/tools/vkr_harness profile --case <case> --profile <profile>
./build_test.sh                # build + run the CPU suite
./build_test_batch.sh          # 50 runs; confirm or refute a flake
build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sponza_orbit.case.json \
  --profile tools/profiles/performance-windowed.json # Release performance evidence
tools/pack_vkt_textures.sh                       # explicit shared KTX2/UASTC packing
# Windows: tools\pack_vkt_textures.bat
```

Build configurations use persistent, non-overlapping output trees. Normal
builds never pack textures. Run `tools/pack_vkt_textures.sh` (or its `.bat`
equivalent) when source textures change or before producing a packed asset set;
the packer uses the shared `build_vkt_packer` tree and skips up-to-date `.vkt`
outputs unless `VKR_VKT_PACK_FORCE=1` is set.

Supported non-Windows Clang/GCC Debug configurations enable ASan and UBSan by
default. Set `VKR_ENABLE_DEBUG_SANITIZERS=OFF` for an explicit diagnostic
opt-out; `VKR_ENABLE_SANITIZERS=ON` retains the all-configuration opt-in used by
sanitized Release-family builds. Windows Debug defaults to sanitizers off
because Clang ASan is incompatible with the Debug CRT and MSVC does not provide
the requested ASan+UBSan pair.

Debug builds and graphics validation layers are diagnostic configurations, not
baseline configurations. Use them only while reproducing or debugging a
concrete issue, and use the smallest focused case that exercises that issue.
Snapshot/baseline comparisons and performance runs must use the normal Release
configuration with Metal and Vulkan validation environment variables unset.
Run any required diagnostic validation separately; never mix it into a
baseline or performance command.

Metal validation is opt-in on macOS, including in Debug builds. Set
`MTL_DEBUG_LAYER=1` for API validation and `MTL_SHADER_VALIDATION=1` for
shader/GPU validation; set both to enable both modes. The variables must be in
the environment before the first Metal device is created.

**Warning:** Metal validation can tank frame rate, especially shader
validation. One local, non-authoritative Debug observation fell from roughly
30–40 FPS to 6–7 FPS with both modes enabled. Debug timings are not performance
evidence; never benchmark or make a performance claim from a run with either
validation mode enabled. `build_run.sh` warns when it detects either nonzero
variable. Never run Metal shader/GPU validation across a broad multi-capture or
baseline suite: a validation-enabled 14-capture snapshot was immediately
followed by a macOS watchdog kernel panic. This is correlation rather than a
proven root cause. Run exactly one validation-enabled Metal renderer process at
a time and keep shader validation to a minimal issue reproduction; never launch
parallel Metal validation children.

The core build/run/test and texture-packing wrappers have `.bat` equivalents.
The backend-matrix utility is currently POSIX shell only; the C harness is
cross-platform. Use the repository scripts rather than
invoking `cmake` directly — they own shader compilation and asset copying.

A green CPU suite is not evidence that Vulkan usage is correct. When debugging
a concrete command-recording or resource-transition issue, use a separate,
focused validation-layer run; do not turn baseline execution into an implicit
validation run.

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

`AGENTS.md`, `CLAUDE.md`, `docs/`, `.codex/config.toml`, `.codex/agents/`,
`.codex/skills/`, and the `.claude/skills/` mirror are committed repository
context. Keep them in the same change when an architectural decision, status
claim, or skill rule moves. Start documentation discovery at `docs/README.md`.

Task notes under `.scratch/` are local and untracked; move anything that must
survive into `docs/`.

**A retained artifact tree is not a record.** Measurement, testing, snapshot,
and diagnostic runs write regenerable output — `build/_artifacts/`, Instruments
traces, exported XML, scratch captures — and nothing prunes it. Carry the
result out as documented numbers, report digests, and the command that
reproduces them, then delete the tree in the same turn that produced it.
Leaving gigabytes behind is a defect; see `vkr-harness` for what is safe to
purge.

## Commits and PRs

- Conventional Commits with scope: `feat(renderer): ...`, `fix(fonts): ...`,
  `ref(renderer): ...`.
- PRs: clear summary, the exact commands run, and linked issues.
- For rendering or UI changes, include a screenshot or short clip and note any
  shader or asset updates.

## Configuration

Required: CMake 3.27+, Vulkan SDK 1.4.357+, and Slang 2026.13.1+ (`slangc`).
Older local toolchains fail on the current Vulkan headers and shader semantics.
Export `VULKAN_SDK` so CMake resolves the required SDK: with it unset, a
package-manager Vulkan earlier than 1.4.357 wins the search and the build fails
on undeclared symbols in files that merely include a changed Vulkan header.
Optional: set `VCPKG_ROOT` to use the vcpkg toolchain. Build scripts prefer
Ninja and clang when available.
