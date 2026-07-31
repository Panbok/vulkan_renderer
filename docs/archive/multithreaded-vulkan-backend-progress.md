---
status: superseded
updated: 2026-07-31
authority: progress
---

> **Archived.** Superseded by [`../architecture/renderer-architecture-spec.md`](../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Multithreaded Vulkan Backend Implementation Progress

Spec: `docs/rendering/multithreaded-vulkan-backend-spec.md`
Last updated: 2026-02-09

This document tracks phase-by-phase implementation progress for multithreaded
Vulkan backend uploads.
Update this file at the end of each completed work slice with concrete file
changes, validation results, and any deviations from spec.

## Status Board

- Phase A: Parallel runtime foundation + upload `vkQueueWaitIdle` removal - `completed`
- Phase B: Texture + geometry batch APIs on parallel runtime - `completed`
- Phase C: Pass capture + secondary command buffer recording for all graphics passes - `retired`
- Phase D: Upload-path default enablement, validation gates, rollback controls - `completed`

### Scope Adjustment (2026-02-09)

- Parallel render-record/capture implementation was retired from active scope.
- Active production scope is now upload parallelism only
  (`VKR_VULKAN_PARALLEL_UPLOAD` and `VKR_PARALLEL_UPLOAD` runtime toggle).
- Validation/perf runners were simplified to serial vs upload-only cases:
  - `tools/validate_multithreaded_backend_matrix.sh`
  - `tools/benchmark_multithreaded_backend.sh`
- Historical Phase C notes below are retained for traceability.

---

## Phase A: Parallel Runtime Foundation + Upload Wait-Idle Removal
Status: completed

Goal:
- Introduce backend parallel runtime, queue-submit locking discipline, and remove
  upload-path queue idle stalls behind `VKR_VULKAN_PARALLEL_UPLOAD`.

Implementation details (completed):
- Added backend feature flags and runtime scaffolding in Vulkan types:
  - `VKR_VULKAN_PARALLEL_UPLOAD`
  - `VKR_VULKAN_PARALLEL_RENDER_RECORD`
  - `VulkanParallelRuntime` with per-worker command-pool context layout
  - `VulkanQueueSubmitState` with role-based queue submission locks
- Added queue-lock lifecycle and lock-aware wrappers in backend:
  - `vulkan_backend_queue_submit_locked`
  - `vulkan_backend_queue_present_locked`
  - `vulkan_backend_queue_wait_idle_locked`
  - lock create/destroy integrated into backend init/shutdown
- Migrated all Vulkan queue submit/present call sites to wrappers:
  - frame-end submit path
  - swapchain present path
  - upload/transfer helper submits in buffer/image/command modules
- Removed upload-path hard queue-idle waits when
  `VKR_VULKAN_PARALLEL_UPLOAD=1`:
  - `vulkan_command_buffer_end_single_use`
  - `vulkan_buffer_copy_to`
  - transfer/image upload helpers in `vulkan_image.c`
  - fallback queue-idle path preserved under `#if !VKR_VULKAN_PARALLEL_UPLOAD`
- Preserved sequential fallback semantics by leaving runtime disabled by
  default (`parallel_runtime.enabled = false`) until job-system wiring is
  added in Phase B.

Primary file targets:
- `lib/src/renderer/vulkan/vulkan_types.h`
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_backend.h`
- `lib/src/renderer/vulkan/vulkan_device.c`
- `lib/src/renderer/vulkan/vulkan_command.c`
- `lib/src/renderer/vulkan/vulkan_buffer.c`
- `lib/src/renderer/vulkan/vulkan_image.c`

Exit criteria:
- Upload paths use fence/semaphore completion only; no upload-path
  `vkQueueWaitIdle`.
- Queue submissions from frame thread and workers follow shared lock discipline.
- Build succeeds with feature flag both disabled and enabled.

Validation log:
- `./build.sh Debug` - success.
- `./build_test.sh` - success (all tests passed).

Files touched:
- `lib/src/renderer/vulkan/vulkan_types.h`
- `lib/src/renderer/vulkan/vulkan_backend.h`
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_command.c`
- `lib/src/renderer/vulkan/vulkan_buffer.c`
- `lib/src/renderer/vulkan/vulkan_image.c`
- `lib/src/renderer/vulkan/vulkan_swapchain.c`

Notes / Risks:
- Queue-family ownership transfers must remain explicit and paired.
- Any fallback paths must preserve synchronous caller semantics.
- Phase A added runtime scaffolding only; worker command-pool creation and
  job-system attachment are scheduled for Phase B.
- `VKR_VULKAN_PARALLEL_UPLOAD=0` compile path has fallback code but has not
  been separately built in this phase.

---

## Phase B: Batch Texture + Geometry Upload Integration
Status: completed

Goal:
- Add batch upload APIs and integrate texture/geometry systems so GPU uploads can
  run through backend parallel runtime.

Implementation details (completed in this phase slice):
- Added backend/front-end API surface and wiring:
  - `set_job_system` backend interface hook.
  - `buffer_create_batch` and `texture_create_with_payload_batch` in backend
    interface.
  - Frontend wrappers and `count=1` routing in single-resource calls.
- Job system is now attached from renderer systems initialization before
  texture/material/mesh loading starts.
- Texture system batch load path now:
  - keeps decode parallel jobs,
  - builds one backend texture batch upload request set,
  - preserves dedup and handle-registration behavior.
- Geometry/mesh batch path now:
  - adds `vkr_geometry_system_create_batch`,
  - reserves geometry slots first and rolls back failed entries,
  - submits a single backend buffer batch for vertex/index uploads,
  - updates mesh manager to resolve missing subset geometries via one batch
    creation path.
- Vulkan backend parallel runtime is now initialized with per-worker command
  pools when job system is attached:
  - transfer pool,
  - graphics-upload pool,
  - per-frame secondary-record pools.
- Vulkan backend buffer batch uploads now fan out GPU copy work onto job-system
  GPU workers:
  - main thread creates destination buffers and staging buffers,
  - worker jobs record/submit copy commands from immutable payload metadata,
  - submit/wait uses fence-based completion with shared queue lock wrappers,
  - per-item failure destroys corresponding created buffer handle.
- Vulkan backend texture payload batch uploads now use worker GPU jobs:
  - main thread validates payloads, builds immutable copy-region metadata, and
    creates texture/image wrappers plus staging buffers,
  - worker jobs perform upload command recording/submission per texture using
    worker graphics-upload command pools,
  - final sampler creation and handle publication stay on main thread after
    successful upload completion.
- `renderer_vulkan_upload_buffer` now uses shared staging helper logic and no
  longer depends on staging wrapper pool allocation in upload path.

Validation additions completed:
- Added focused renderer/frontend batch tests for per-item error mapping in
  backend batch callback paths.
- Added fallback-path cleanup symmetry test ensuring failed follow-up uploads
  destroy partially created buffers.
- Added geometry batch rollback test ensuring failed per-geometry uploads tear
  down created buffer handles and invalidate failed geometry slots.

Primary file targets:
- `lib/src/renderer/vkr_renderer.h`
- `lib/src/renderer/renderer_frontend.c`
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/systems/vkr_texture_system.c`
- `lib/src/renderer/systems/vkr_geometry_system.h`
- `lib/src/renderer/systems/vkr_geometry_system.c`
- `lib/src/renderer/systems/vkr_mesh_manager.c`

Exit criteria:
- Texture and geometry batch paths execute through new backend batch APIs.
- Backend batch upload path uses worker runtime for both geometry and texture
  upload work.
- Per-item result mapping and cleanup symmetry validated.
- Sequential fallback remains functional when job system is unavailable.

Validation log:
- `./build.sh Debug` - success.
- `./build_test.sh` - success (all tests passed).
- Added/validated tests:
  - `test_renderer_buffer_batch_fallback_cleanup`
  - `test_renderer_buffer_batch_backend_mapping`
  - `test_renderer_texture_batch_backend_mapping`
  - `test_geometry_system_batch_failure_rolls_back_buffers`

Notes / Risks:
- Worker tasks in buffer batch path use only immutable payload arrays and do not
  access `state->temp_scope`.
- Resource wrapper pool ownership remains main-thread-only for buffer batches.
- Texture batch upload worker path is now in place with focused mapping/cleanup
  coverage at frontend and geometry-system boundaries.

---

## Phase C: Parallel Pass Recording With Secondary Command Buffers
Status: completed

Goal:
- Move draw recording to pass capture/replay and parallelize encoding for all
  graphics passes using secondary command buffers behind
  `VKR_VULKAN_PARALLEL_RENDER_RECORD`.

Implementation details (planned):
- Introduce per-pass capture state and immutable `VulkanDrawOp` stream.
- Render pass begin uses `VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS` when
  enabled.
- End-of-pass partitions ops into chunks and schedules worker jobs to encode
  secondary command buffers.
- Execute secondaries in deterministic order on primary command buffer.
- Preserve ordering for transparent/UI/editor/picking.
- Opaque passes may use state-sorted chunking.
- Add per-pass thresholds to avoid low-draw overhead.

Implementation details (completed in this phase slice):
- Added backend-level active command buffer selection primitive:
  `recording_command_buffer_override` in `VulkanBackendState` and
  `vulkan_backend_get_active_graphics_command_buffer(...)`.
- Routed render command entry points through the shared active-command-buffer
  accessor:
  - draw calls (`draw`, `draw_indexed`, `draw_indexed_indirect`)
  - bind/state commands (`bind_pipeline`, `bind_buffer`, viewport/scissor/depth
    bias)
  - render pass begin/end and frame command-buffer use sites
  - frame-active barrier/transition/timing/readback command recording paths
- Updated shader/pipeline state update paths to bind descriptors/push constants
  against the active command buffer, not only the primary frame buffer.
- Added first executable pass replay path behind
  `VKR_VULKAN_PARALLEL_RENDER_RECORD`:
  - render pass begin now uses `VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS`
    when enabled,
  - one pass-local secondary command buffer is allocated, recorded, and executed
    on the primary at pass end,
  - secondary command buffer lifecycle/reset/discard wiring was added for
    begin-frame, swapchain-recreate, and backend shutdown paths.
- Added pass-capture infrastructure for immutable per-pass command streams:
  - `VulkanPassCaptureOp` union and op-type enum for pipeline/state/bind/draw
    commands,
  - per-pass capture storage (`pass_capture_ops`, `pass_capture_blob`) with
    allocator-scope lifetime management,
  - pass capture helpers for begin/append/reserve/flush/abort.
- Hooked render command APIs into capture mode while a captured pass is active:
  - `renderer_vulkan_update_pipeline_state`,
  - `renderer_vulkan_bind_pipeline`,
  - `renderer_vulkan_bind_buffer`,
  - `renderer_vulkan_set_viewport`,
  - `renderer_vulkan_set_scissor`,
  - `renderer_vulkan_set_depth_bias`,
  - `renderer_vulkan_draw`,
  - `renderer_vulkan_draw_indexed`,
  - `renderer_vulkan_draw_indexed_indirect`.
- Integrated capture lifecycle into pass boundaries:
  - begin render pass starts capture when secondary recording is enabled,
  - end render pass flushes captured ops into the active secondary command
    buffer before `vkCmdExecuteCommands`,
  - discard/reset paths abort capture scope to avoid scope leaks.
- Added chunked secondary record infrastructure in pass-capture flush:
  - extracted replay into `vulkan_backend_pass_capture_replay_range(...)`,
  - added worker GPU job path that records one secondary command buffer per
    chunk using per-worker per-frame secondary command pools,
  - executes chunk secondaries on primary command buffer in deterministic chunk
    index order.
- Added safe fallback gating for chunked path:
  - if chunk setup/submission/wait fails, backend falls back to existing
    single-command-buffer replay path for the same pass.
- Added active render target tracking in backend state so pass replay jobs have
  explicit render-pass/framebuffer inheritance metadata.
- Removed temporary `UPDATE_PIPELINE_STATE` exclusion from chunked recording:
  - chunked replay now accepts pipeline update ops,
  - shared descriptor/state mutation inside update replay is protected with a
    dedicated backend mutex so worker jobs remain thread-safe.
- Added pass-class-aware chunk execution ordering policy:
  - strict submission-order execution for order-sensitive domains
    (UI/post/transparent/picking/overlay),
  - deterministic pipeline-key chunk ordering for opaque-oriented domains
    (world/shadow/skybox) to reduce state churn.
- Added per-domain chunking heuristics:
  - per-domain minimum op thresholds for enabling chunked record,
  - per-domain target ops-per-chunk sizing,
  - stricter thresholding for update-heavy and order-sensitive passes.
- Refined opaque-domain chunk planning and ordering:
  - chunk plan builder now aligns opaque chunk boundaries toward pipeline-state
    transition ops while preserving contiguous replay ranges,
  - chunk sort key now uses dominant draw-pipeline key within each chunk
    (instead of first-pipeline-only key) for stronger state-coherent ordering.
- Extracted pass-capture chunk planning into reusable helper module:
  - added `lib/src/renderer/vulkan/vulkan_pass_capture_plan.{h,c}` with pure
    callback-driven chunk planning and dominant-pipeline sort-key logic,
  - backend now adapts captured ops via callbacks and uses the helper for chunk
    planning in parallel pass replay.
- Consolidated deterministic chunk execution ordering into planner helper:
  - added `vulkan_pass_capture_plan_build_execution_order(...)`,
  - replaced backend-local insertion sort with helper-driven execution ordering.
- Moved pass-domain ordering policy and chunk-threshold heuristics into planner
  helper:
  - added `vulkan_pass_capture_plan_domain_preserves_order(...)`,
  - added `vulkan_pass_capture_plan_parallel_min_ops(...)`,
  - added `vulkan_pass_capture_plan_target_ops_per_chunk(...)`,
  - backend now consumes these helper APIs directly.
- Added crash-containment guard for render-record chunk jobs:
  - worker-chunk path is now skipped for passes that include captured
    `UPDATE_PIPELINE_STATE` ops, forcing serial secondary replay for those
    passes while preserving capture/replay semantics.
- Hardened crash-containment guard for render-record:
  - chunked worker secondary recording is temporarily disabled at runtime in
    pass-capture flush, forcing serial secondary replay for all passes while
    investigating secondary lifetime invalidation during benchmark startup.
- Removed pass-chunk setup allocations from backend allocator scope:
  - chunk-plan/job scratch arrays are now fixed-size stack arrays bounded by
    `VKR_VULKAN_PARALLEL_MAX_WORKERS`,
  - avoids allocator-scope rewind interactions with worker-side replay work.
- Fixed backend mutex/allocator lifecycle symmetry:
  - pass-capture replay mutex and queue-submit locks are now destroyed before
    Vulkan allocator teardown,
  - queue-submit locks are torn down if replay-mutex creation fails during init.
- Added render-record stabilization fallback for validation cleanup:
  - render-pass begin/end now route `VKR_VULKAN_PARALLEL_RENDER_RECORD` through
    immediate per-pass secondary command buffer recording/execution
    (`vulkan_backend_pass_secondary_recording_begin/end_and_execute`) instead
    of deferred pass-capture replay,
  - this preserves secondary-command render-record behavior while isolating
    pass-capture replay descriptor/state mismatch bugs.
- Fixed immediate secondary command-buffer lifetime hazards:
  - executed per-pass secondaries are no longer freed immediately after
    `vkCmdExecuteCommands`,
  - secondary-command pool ownership is tracked per pass in backend state so
    discard/error cleanup frees from the correct pool,
  - normal lifetime now ends at frame-slot secondary command-pool reset after
    the in-flight fence wait.
- Added opt-in runtime switches for staged Phase C rollout:
  - `VKR_PARALLEL_RENDER_RECORD_CAPTURE` enables pass-capture-based replay
    path (env rollback still supported),
  - `VKR_PARALLEL_RENDER_RECORD_CHUNKED` enables chunked worker-secondary
    recording inside capture path (env rollback still supported).
- Completed Phase C default-path switch:
  - pass capture/replay is now the default render-record path whenever
    `VKR_PARALLEL_RENDER_RECORD=1`,
  - chunked worker replay defaults on with deterministic ordering and serial
    fallback for any chunk planning/submit/wait failure,
  - removed `VKR_PARALLEL_RENDER_RECORD_CAPTURE_UNSAFE` gating now that replay
    descriptor updates are prepared at capture time and replay is immutable.
- Refactored capture replay to eliminate descriptor writes during replay:
  - capture-time `update_pipeline_state` now executes a prepare-only pipeline
    state update on the main thread (descriptor writes and buffer uploads only),
  - captured update ops now store immutable replay payload
    (pipeline layout, resolved descriptor sets, set indices, push-constant
    bytes),
  - replay no longer calls `vulkan_graphics_pipeline_update_state(...)` and
    instead emits only bind-descriptor/bind-pipeline/push-constant `vkCmd*`
    commands.

Phase C completion notes:
- Integration/perf gate closure and benchmark-driven tuning continue in Phase D
  validation/performance matrix work.

Primary file targets:
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_types.h`
- `lib/src/renderer/vkr_rg_execute.c`
- `lib/src/renderer/systems/views/vkr_view_world.c`
- `lib/src/renderer/systems/views/vkr_view_ui.c`
- `lib/src/renderer/systems/views/vkr_view_editor.c`
- `lib/src/renderer/systems/views/vkr_view_skybox.c`

Exit criteria:
- All graphics passes use capture/replay path when render-record is enabled.
- Image parity and transparent ordering invariance are preserved.
- Low-draw scenes stay within allowed CPU overhead budget.

Validation log:
- `./build_test.sh` - success after active-command-buffer routing refactor.
- `./build_test.sh` - success after enabling pass-local secondary command
  recording path under feature flag.
- `./build_test.sh` - success after pass-capture integration into render
  state/bind/draw API paths.
- `./build_test.sh` - success after chunked pass-capture secondary recording
  infrastructure and fallback gating.
- `./build_test.sh` - success after enabling chunked replay for pipeline-update
  ops with mutex-guarded update replay.
- `./build_test.sh` - success after domain-aware chunk execution ordering policy
  integration.
- `./build_test.sh` - success after per-domain chunking heuristics and mutex
  lifecycle cleanup integration.
- `./build_test.sh` - success after opaque-domain state-aware chunk planning and
  dominant-pipeline chunk sort-key refinement.
- `./build_test.sh` - success after extracting pass-capture chunk planning into
  reusable helper module and adding pass-capture planner unit tests.
- `./build_test.sh` - success after extracting deterministic chunk execution
  order generation into planner helper and adding ordering unit tests.
- `./build_test.sh` - success after extracting domain ordering policy and
  threshold heuristics into planner helper with direct unit coverage.
- `./build.sh Debug` - success after immediate render-record secondary lifetime
  fix.
- `./build_test.sh` - success after immediate render-record secondary lifetime
  fix.
- `./build.sh Debug` - success after adding capture/chunked runtime toggles and
  opt-in capture path routing.
- `./build_test.sh` - success after adding capture/chunked runtime toggles and
  opt-in capture path routing.
- `./build_test.sh` - success after Phase C default-path switch (capture on by
  default with chunked replay default enabled and unsafe gate removed).

Notes / Risks:
- Descriptor/pipeline state must be fully resolved before worker encoding.
- Capture data lifetimes must outlive worker job completion.
- Opaque-domain ordering currently sorts by coarse pipeline chunk key; deeper
  state sorting was strengthened by dominant-pipeline chunk keys and boundary
  snapping; final perf tuning still requires scene-level measurements.
- Added CPU-only planner coverage to de-risk ordering/chunking regressions
  without GPU integration test requirements.
- Render-record chunk jobs are currently conservative for update-heavy passes;
  this is a stability gate until full worker replay lifetime validation is
  closed.

---

## Phase D: Default Enablement + Perf/Validation Gates + Rollback
Status: in progress

Goal:
- Make parallel upload and parallel render recording default after quality and
  performance gates pass; retain runtime rollback controls.

Implementation details (completed in this phase slice):
- Added runtime feature toggles for parallel upload and parallel render-record
  paths in Vulkan backend state:
  - `parallel_upload_enabled`
  - `parallel_render_record_enabled`
- Added backend environment flag parsing and refresh hooks:
  - `VKR_PARALLEL_UPLOAD`
  - `VKR_PARALLEL_RENDER_RECORD`
- Decoupled worker command-pool runtime initialization from
  `VKR_VULKAN_PARALLEL_UPLOAD`; runtime now initializes when either upload or
  render-record parallel features are enabled.
- Explicitly gated upload and pass-capture parallel paths by their respective
  runtime feature toggles to preserve independent rollback behavior.
- Added runtime startup logging for effective parallel feature state
  (upload/render-record/runtime/worker-count) to make validation runs
  self-describing.
- Added matrix validation runner script:
  - `tools/validate_multithreaded_backend_matrix.sh`
  - compile-time matrix covers default/serial/full-parallel macro defaults
  - runtime matrix covers default/serial/upload-only/render-only/full-parallel
    env combinations.
- Added perf benchmark plumbing and runner:
  - app-side env-controlled benchmark telemetry (`BENCHMARK_SAMPLE`,
    `BENCHMARK_SUMMARY`)
  - runtime controls for non-interactive timing capture
    (`VKR_BENCHMARK_LOG`, `VKR_BENCHMARK_LABEL`, `VKR_RG_GPU_TIMING`)
  - `tools/benchmark_multithreaded_backend.sh` for serial/upload/render/full
    runtime perf comparison with watchdog handling.
- Hardened runtime validation gates in automation scripts:
  - benchmark and validation matrix scripts now fail on log markers for Vulkan
    validation or runtime crash signatures (`validation layer`, `VUID-`,
    `AddressSanitizer`, `Abort trap`, `ABORTING`, `Segmentation fault`),
  - prevents false "PASS" states when process exit is zero but runtime output
    contains critical correctness failures.
- Added conservative render-record capture defaults and safety controls for
  large-scene perf stability:
  - `VKR_PARALLEL_RENDER_RECORD_CAPTURE` is now opt-in (default off) so
    `VKR_PARALLEL_RENDER_RECORD=1` uses immediate per-pass secondary recording
    by default,
  - added `VKR_PARALLEL_RENDER_RECORD_CAPTURE_UNSAFE` override to bypass
    conservative capture fallback policy for explicit experimentation.
- Added adaptive pass-level capture fallback for update-heavy streams:
  - capture path now falls back to immediate-secondary recording when prior-pass
    telemetry indicates update-heavy high-op streams (large scene world/shadow
    style passes),
  - chunked worker replay is disabled for update-heavy captures unless unsafe
    override is enabled.
- Added phase-D regression rollback defaults for benchmark stability:
  - `VKR_PARALLEL_UPLOAD=1` now requires explicit
    `VKR_PARALLEL_UPLOAD_UNSAFE=1` before worker upload jobs are used,
  - `VKR_PARALLEL_RENDER_RECORD=1` now keeps inline primary recording unless
    explicitly opted into immediate-secondary mode via
    `VKR_PARALLEL_RENDER_RECORD_IMMEDIATE=1` or capture mode via
    `VKR_PARALLEL_RENDER_RECORD_CAPTURE=1`.
- Hardened benchmark reproducibility controls for perf gate runs:
  - `tools/benchmark_multithreaded_backend.sh` now supports explicit
    `Release|Debug|RelWithDebInfo` selection via `--build-type` and
    `VKR_BENCH_BUILD_TYPE`,
  - benchmark runner now verifies configured CMake build type and rebuilds when
    binary/build-type mismatch is detected (or when `VKR_BENCH_FORCE_BUILD=1`),
  - default benchmark build type is now `Release` to avoid Debug sanitization
    overhead skewing phase-D perf decisions.

Implementation details (planned):
- Run full validation/perf matrix from spec test section.
- Enable both flags by default.
- Keep environment overrides for immediate rollback.
- Final cleanup pass for redundant legacy helpers and duplicate error paths.

Primary file targets:
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_types.h`
- build/config headers or feature toggle location used by backend
- `docs/rendering/multithreaded-vulkan-backend-spec.md` (if finalized deltas)
- `docs/rendering/multithreaded-vulkan-backend-progress.md`

Exit criteria:
- Validation-layer run is clean for thread/queue-family checks.
- Performance gates met:
  - Cached Sponza+Falcon load <= 0.8s on baseline profile.
  - Low-draw frame CPU overhead from parallel recording <= 5%.
  - Draw-heavy pass CPU recording time improved vs baseline.
- Rollback controls documented and verified.

Validation log:
- `./build_test.sh` - success after adding runtime env toggles and decoupling
  worker-runtime init from upload-only compile flags.
- `tools/validate_multithreaded_backend_matrix.sh --smoke` - success
  (`PASS=1`, `FAIL=0`).
- `tools/validate_multithreaded_backend_matrix.sh` - success
  (`PASS=7`, `FAIL=0`).
- `./build_test.sh` - success after adding app benchmark telemetry + perf runner
  script.
- `VKR_BENCH_AUTOCLOSE_SECONDS=2 VKR_BENCH_MAX_WAIT_SECONDS=20
  tools/benchmark_multithreaded_backend.sh --smoke` - timed out in this
  execution environment (`PASS=0`, `FAIL=1`); runner watchdog and failure
  reporting verified.
- `./build_test.sh` - success after capture-default rollback + adaptive
  update-heavy fallback tuning.
- `./build_test.sh` - success after phase-D default rollback toggles for
  upload worker jobs and render-record immediate-secondary mode.
- `sh -n tools/benchmark_multithreaded_backend.sh` - success after build-type
  selection and cache-mismatch rebuild controls were added.

Notes / Risks:
- Regression risk on scene reload lifecycle; include stress load/unload cycles.
- Keep deferred-destroy semantics unchanged.

---

## Cross-Phase Implementation Rules

- Public renderer semantics remain synchronous; returned handles are immediately
  usable.
- No worker access to `state->temp_scope`.
- Queue submission lock order is fixed: transfer -> graphics-upload -> present.
- No implementation step may reintroduce upload-path `vkQueueWaitIdle`.
- Prefer single cleanup paths in multi-step acquire/creation flows.

## Change Log

- 2026-02-08: Created initial phase tracker with status board, phase scopes,
  planned file targets, exit criteria, and validation placeholders.
- 2026-02-08: Completed Phase A. Added queue-submit/present lock wrappers,
  removed upload-path queue-idle stalls behind `VKR_VULKAN_PARALLEL_UPLOAD`,
  and added backend parallel runtime scaffolding.
- 2026-02-08: Phase B started. Added frontend/backend batch API wiring,
  texture-system and geometry/mesh batch integration, per-worker command-pool
  initialization on job-system attach, and worker-fanned-out buffer batch upload
  copy execution.
- 2026-02-08: Extended Phase B with worker-fanned-out texture payload batch
  command recording/submission in Vulkan backend and main-thread sampler/handle
  finalization after upload completion.
- 2026-02-08: Completed Phase B by adding focused renderer batch and geometry
  rollback tests, validating per-item error mapping and cleanup symmetry with
  `./build_test.sh`.
- 2026-02-08: Started Phase C with active-command-buffer routing infrastructure
  in backend/shader/pipeline paths to unblock secondary command buffer replay.
- 2026-02-08: Extended Phase C with pass-local secondary command buffer replay
  under `VKR_VULKAN_PARALLEL_RENDER_RECORD` (single-secondary path) and added
  lifecycle safety hooks for frame start, swapchain recreation, and shutdown.
- 2026-02-08: Refined Phase C opaque replay scheduling by adding a state-aware
  chunk planner and dominant draw-pipeline chunk sort keys, then validated with
  `./build_test.sh`.
- 2026-02-08: Replaced fixed equal-size opaque chunk partitioning with
  `vulkan_backend_pass_capture_build_chunk_plan(...)`, which snaps chunk
  boundaries toward pipeline transition ops and feeds dominant-pipeline sort
  keys into deterministic secondary execution ordering. Validation:
  `./build_test.sh`.
- 2026-02-08: Extracted pass-capture chunk planning into
  `vulkan_pass_capture_plan.{h,c}` and added unit tests:
  `test_pass_capture_plan_build_preserve_order`,
  `test_pass_capture_plan_build_snaps_to_pipeline_boundaries`,
  `test_pass_capture_plan_chunk_sort_prefers_dominant_pipeline`,
  `test_pass_capture_plan_chunk_sort_falls_back_to_first_pipeline`.
- 2026-02-08: Added planner-owned execution-order helper
  (`vulkan_pass_capture_plan_build_execution_order`) and covered preserve-order
  and sorted-order behavior in pass-capture planner tests.
- 2026-02-08: Moved pass-domain preserve-order policy and per-domain chunk
  thresholds into `vulkan_pass_capture_plan.{h,c}` and added tests:
  `test_pass_capture_plan_domain_preserve_order_policy`,
  `test_pass_capture_plan_parallel_threshold_heuristics`.
- 2026-02-08: Started Phase D rollback controls by adding runtime env toggles
  (`VKR_PARALLEL_UPLOAD`, `VKR_PARALLEL_RENDER_RECORD`), decoupling worker
  runtime initialization from upload-only compile gating, and applying
  explicit per-feature runtime gating in upload and pass-capture paths.
- 2026-02-08: Added `tools/validate_multithreaded_backend_matrix.sh` and ran
  smoke + full compile/runtime toggle matrices (`PASS=7`, `FAIL=0`) with logs
  under `build/_validation/multithreaded_backend/logs`.
- 2026-02-08: Added app benchmark telemetry and
  `tools/benchmark_multithreaded_backend.sh` for Phase D perf gates; smoke run
  timed out in this environment, so perf baselines remain pending desktop run.
- 2026-02-08: Added render-record crash containment: skip worker chunk replay
  for passes with pipeline-state update ops and switch chunk setup scratch
  storage to fixed stack arrays in `vulkan_backend_pass_capture_flush(...)`.
  Validation: `./build_test.sh`.
- 2026-02-08: Temporarily disabled chunked worker replay path in
  `vulkan_backend_pass_capture_flush(...)` (serial secondary replay only) to
  isolate `bound VkCommandBuffer was destroyed or rerecorded` validation
  failures in `render_record_only`/`full_parallel` benchmark cases. Validation:
  `./build.sh Debug`, `./build_test.sh`.
- 2026-02-08: Routed render-record pass execution through immediate secondary
  command buffer begin/end path in render-pass boundaries to stabilize
  validation errors from deferred pass-capture replay while preserving
  `VKR_PARALLEL_RENDER_RECORD` runtime behavior. Validation:
  `./build.sh Debug`, `./build_test.sh`.
- 2026-02-08: Fixed immediate render-record secondary command-buffer lifetime
  by removing post-execute free and tracking per-pass secondary command-pool
  ownership for discard/error cleanup, so executed secondaries remain valid
  until fence-safe frame-slot command-pool reset. Validation:
  `./build.sh Debug`, `./build_test.sh`.
- 2026-02-08: Hardened `tools/benchmark_multithreaded_backend.sh` and
  `tools/validate_multithreaded_backend_matrix.sh` to fail on Vulkan
  validation/crash log markers even when process exit code is zero.
- 2026-02-08: Added staged Phase C runtime toggles
  (`VKR_PARALLEL_RENDER_RECORD_CAPTURE`,
  `VKR_PARALLEL_RENDER_RECORD_CHUNKED`) and wired opt-in pass-capture routing
  at render-pass boundaries, while keeping immediate-secondary render-record as
  default stable behavior. Validation: `./build.sh Debug`, `./build_test.sh`.
- 2026-02-08: Added capture safety gate in
  `vulkan_backend_refresh_parallel_feature_toggles(...)`: capture replay now
  requires both `VKR_PARALLEL_RENDER_RECORD_CAPTURE=1` and
  `VKR_PARALLEL_RENDER_RECORD_CAPTURE_UNSAFE=1`; otherwise it is explicitly
  disabled with a startup warning to prevent known descriptor-update validation
  failures in deferred replay. Validation: `./build.sh Debug`,
  `./build_test.sh`.
- 2026-02-08: Removed replay-time descriptor mutations from pass-capture update
  ops. Capture now prepares descriptor state on the main thread via a
  prepare-only command-buffer override (`VK_NULL_HANDLE`), stores resolved
  descriptor sets/pipeline layout/push constants in the captured op payload,
  and replays immutable `vkCmd*` bind/push operations only. Validation:
  `./build_test.sh`.
- 2026-02-08: Added capture/descriptor lifetime hardening after benchmark-log
  validation failures:
  - rollback reserved pass-capture ops when prepare or push-blob append fails,
    so failed capture updates cannot leave stale op payloads in replay streams,
  - guard instance update path against stale/released instance ids by checking
    id bounds and requiring allocated descriptor-set state before descriptor
    writes/binds,
  - clear per-frame descriptor-set handles on immediate instance release to
    prevent accidental reuse of freed handles via stale instance ids.
  Validation: `./build_test.sh`.
- 2026-02-08: Fixed shader/pipeline selection mismatch in
  `vkr_pass_packet_resolve_pipeline(...)` that could leave
  `shader_system.current_shader` out of sync with the actually resolved
  pipeline when material shader lookup fell back to domain pipelines. Pipeline
  resolution now completes first, then shader selection is aligned to the
  resolved handle (or domain fallback), preventing invalid descriptor write
  patterns like world-material bindings being applied to 3-binding layouts in
  capture-mode runs. Validation: `./build_test.sh`.
- 2026-02-08: Completed Phase C default enablement for pass capture/replay in
  `vulkan_backend_refresh_parallel_feature_toggles(...)` and
  `vulkan_backend_pass_capture_flush(...)`:
  - removed `VKR_PARALLEL_RENDER_RECORD_CAPTURE_UNSAFE` safety gate,
  - defaulted `VKR_PARALLEL_RENDER_RECORD_CAPTURE` to on when
    `VKR_PARALLEL_RENDER_RECORD=1`,
  - defaulted `VKR_PARALLEL_RENDER_RECORD_CHUNKED` to on when capture is on,
  - re-enabled chunked worker replay for update-bearing passes with existing
  serial fallback on any chunk replay failure.
  Validation: `./build_test.sh`.
- 2026-02-09: Fixed chunked secondary replay state invalidation seen on large
  scenes (for example San Miguel):
  - worker chunk record jobs now replay a deterministic pre-chunk state prefix
    (latest pre-chunk viewport/scissor/depth-bias, pipeline/descriptor
    bindings, and vertex/index buffer bindings) before replaying chunk-local
    ops,
  - prevents chunk-local draws from executing without valid pipeline/index
    buffer/dynamic state after chunk-domain reordering.
  Validation: `./build_test.sh`.
- 2026-02-09: Added chunk self-sufficiency gate for parallel pass-capture
  replay to prevent invalid draw-state command streams on large scenes:
  - `vulkan_backend_pass_capture_flush(...)` now validates every planned chunk
    can reconstruct required draw state (pipeline, indexed-draw index-buffer,
    viewport, scissor) from prefix + chunk-local ops before worker submission,
  - if any chunk fails this preflight check, the backend skips chunked worker
    replay for that pass and falls back to existing serial secondary replay,
    preserving correctness without changing public render-record toggles.
  Validation: `./build.sh Debug`, `./build_test.sh`.
- 2026-02-09: Phase D perf-stability tuning for large scenes (for example San
  Miguel):
  - switched pass-capture replay default to opt-in
    (`VKR_PARALLEL_RENDER_RECORD_CAPTURE=1`) while keeping
    `VKR_PARALLEL_RENDER_RECORD=1` immediate-secondary path as default,
  - added `VKR_PARALLEL_RENDER_RECORD_CAPTURE_UNSAFE=1` override to bypass
    adaptive safety fallback for targeted experiments,
  - added telemetry-driven fallback from capture to immediate-secondary for
    update-heavy passes and disabled chunked worker replay for update-heavy
    captures unless unsafe override is enabled.
  Validation: `./build_test.sh`.
- 2026-02-09: Phase D regression rollback defaults for benchmark comparability:
  - gated worker upload job fan-out behind
    `VKR_PARALLEL_UPLOAD_UNSAFE=1` to avoid pseudo-parallel overhead on default
    runs while queue-family-safe transfer ownership flow is still pending,
  - gated immediate-secondary render-record path behind
    `VKR_PARALLEL_RENDER_RECORD_IMMEDIATE=1` so
    `VKR_PARALLEL_RENDER_RECORD=1` no longer forces secondary recording when
    capture is disabled.
  Validation: `./build_test.sh`.
- 2026-02-09: Phase D upload fan-out overhead reduction and runtime gating:
  - converted buffer and texture batch upload worker jobs to record-only
    command generation (no per-job queue submit/wait),
  - added caller-thread batch submit path that submits all successfully recorded
    upload command buffers in one `vkQueueSubmit` + one fence wait and then
    frees worker-recorded command buffers with the correct originating worker
    command pool,
  - tightened parallel runtime enablement so worker pools are initialized only
    when an active worker path is enabled:
    `VKR_PARALLEL_UPLOAD_UNSAFE=1` or render-record
    immediate/capture (`VKR_PARALLEL_RENDER_RECORD_IMMEDIATE=1` or
    `VKR_PARALLEL_RENDER_RECORD_CAPTURE=1`).
  Validation: `./build_test.sh`.
- 2026-02-09: Phase D upload-mode frame-loop overhead cleanup:
  - added explicit gating so worker secondary-record command pools are only
    created/reset when render-record immediate/capture is active,
  - avoids per-frame `vkResetCommandPool` work in upload-only runtime mode.
  Validation: `./build_test.sh`.
- 2026-02-09: Phase D adaptive upload fan-out thresholds:
  - added batch-worthiness gating so parallel upload worker fan-out is used
    only when upload batch size is large enough to amortize worker overhead,
  - thresholds are configurable through:
    `VKR_PARALLEL_UPLOAD_MIN_JOBS` (default `8`) and
    `VKR_PARALLEL_UPLOAD_MIN_BYTES` (default `4194304` bytes),
  - small-scene batches now stay on the synchronous path even when
    `VKR_PARALLEL_UPLOAD_UNSAFE=1`.
  Validation: `./build_test.sh`.
- 2026-02-09: Phase D render-record rollout adjustment:
  - `VKR_PARALLEL_RENDER_RECORD_CAPTURE` now defaults to on when
    `VKR_PARALLEL_RENDER_RECORD=1`, so benchmark render-record cases exercise
    pass capture/chunked secondary recording by default,
  - `VKR_PARALLEL_RENDER_RECORD_IMMEDIATE` remains opt-in for explicit
    immediate-secondary A/B runs.
  Validation: `./build_test.sh`.
