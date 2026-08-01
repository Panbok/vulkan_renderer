# Renderer refactor workflow

Use this for comprehensive audits and multi-slice migrations. For a single
focused change, `SKILL.md` is enough.

## 1. Establish evidence

- Record branch, commit, dirty files, build type, GPU/driver, resolution, scene,
  and swapchain image count. Any later comparison is void without these.
- Read `docs/architecture/renderer-architecture-spec.md` §4 (feature status) and
  §8 (prioritized issues), plus the ADRs that constrain the area.
- Build only through the repository scripts. Do not invoke `cmake` by hand for
  app builds — `./build.sh`, `./build_release.sh`, `./build_test.sh` own the
  shader-compile and asset-copy steps.
- Capture the baseline: `./build_test.sh` green, and a Release measurement per
  `vkr-performance` when a hot path is in scope.

## 2. Audit every file without forcing every file to change

For each file or group, record:

- domain responsibility and actual callers;
- owned versus borrowed state, and the destruction/completion rule;
- allocations, locks, string construction, validation, and handle churn, marking
  which occur per frame, per pass, or per draw;
- duplicated facts and competing authorities;
- interface facts callers must know;
- real variation points versus hypothetical ones;
- the test seam and evidence artifact that would own a regression;
- disposition: keep, deepen, merge, split, make private, or delete.

Coverage means every file has an explicit disposition, including files
deliberately left unchanged. Indiscriminate edits are anti-compression. Use
`compress-codebase` when the audit is the primary deliverable.

## 3. Instrument likely hitches

Before optimizing, measure at least:

- per-pass CPU and GPU milliseconds (`vkr_rg_get_pass_timings`);
- upload wait count and bytes
  (`vkr_renderer_get_and_reset_upload_wait_stats`);
- graph resource live/peak counts (`vkr_rg_get_resource_stats`);
- allocator tag totals across a load/unload cycle
  (`vkr_allocator_print_global_statistics`);
- draw count and instance-stream occupancy per frame;
- pipeline creations after the first frame — any nonzero value is a hitch.

## 4. Refactor order

1. Fix proven correctness and lifetime bugs, with a regression test.
2. Remove frame hitches: pipeline compilation during encoding, blocking upload
   waits, per-draw allocation, and non-reused command/framebuffer objects.
3. Make the render graph the single state authority — picking is declared;
   nested IBL bake work remains the known executor-owned boundary.
4. Centralize handle generation, retirement, and completion proof.
5. Replace invalid-combination records with typed variants rejected before
   lowering.
6. Split backend source by lifecycle where that deepens ownership.
7. Add semantic validation at creation/compile/debug layers.
8. Only then pursue further throughput work from the shipped baseline of
   conservative culling, real instancing, and compatible MDI groups. Measure the
   current visibility/binding-state limit first (architecture spec §8 P2,
   ADR-013).

Each item is a vertical slice with its own tests and compatible before/after
evidence. Do not combine state-authority, lifetime, backend representation, and
pipeline-cache changes into one unreviewable patch.

## 5. Verification ladder

Run the cheapest step that completely covers the invariant you touched.

1. Focused unit or regression test under `tests/src/`.
2. `./build_test.sh` — full CPU suite.
3. `./build_test_batch.sh` when a failure looks intermittent (50 runs).
4. Vulkan validation-layer run of the app across the swapchain image counts and
   queue-family layouts your change can reach.
5. `./validate_pipeline_cache.sh` for shader, pipeline, or reflection changes.
6. `tools/validate_multithreaded_backend_matrix.sh` for backend threading.
7. Same-config Release measurement via `tools/benchmark_multithreaded_backend.sh`
   for anything performance-relevant.

Stop and report rather than asserting a result when: the baseline was already
red, the configuration changed between measurements, required metrics are
missing, GPU timestamps are invalid (`gpu_valid == false`), or GPU completion
cannot be proven. Investigative evidence can guide work; it cannot support an
authoritative claim.

## 6. Report

State what changed, which representation disappeared and which owner survived,
which checks were removed and what still proves the invariant, the measured
before/after with its full configuration, and what you did **not** measure or
cover.
