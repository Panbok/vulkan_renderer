---
status: proposed
updated: 2026-09-23
authority: proposal
---
# Codebase audit remediation plan

Baseline: `main` at `abeedc19` (2026-09-23), macOS host, Metal only. This plan
records the verified findings of a repository-wide audit for correctness,
performance, readability and maintainability, and the ordered work needed to
resolve them. It is written for an implementing agent; every item names its
evidence, the change, the surviving owner and the check that proves it. The
[repository contract](../../AGENTS.md) applies to every item: preserve output,
ownership, GPU completion and the frame budget; use Bistro for scene evidence;
a speed claim needs matched Release measurements.

## Evidence collected

All commands run from the repository root against `build_release` (Release,
Apple clang 21, `compile_commands.json` exported by `./build_release.sh`). Scope is
first-party code under `lib/`, `renderer/`, `runtime/`, `editor/`, `app/`,
`tools/` and `tests/`; vendored and generated payloads are excluded.

| Check | Command or method | Result |
|---|---|---|
| CPU tests | `./build_test.sh` | exit 0, 87 registered suites, all pass |
| Compiler warnings | every first-party TU from `compile_commands.json` re-run with `-fsyntax-only -Wall -Wextra -Wshadow -Wdouble-promotion -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-unused-function` | 30,166 diagnostics in 171 files: 22,848 `-Wmissing-braces` (22,104 in `lib/src/math/vec.h`), 4,356 `-Wdouble-promotion`, 2,825 `-Wunused-variable`, 117 `-Wunused-but-set-variable`, 10 `-Wuninitialized`, 9 `-Wshadow`, 1 `-Woverloaded-virtual` |
| Static analysis | same TUs with `clang --analyze -Xclang -analyzer-output=text` | 2,359 diagnostics; 55 non-dead-store findings outside `tests/`, listed in [Correctness](#b-correctness) |
| Format drift | `clang-format --dry-run -Werror` per first-party `.c`/`.h` | 170 of 634 files differ (54 renderer, 43 runtime, 28 tests, 18 tools, 13 lib, 11 editor, 2 app, 1 examples) |
| Function length | brace-matching scan of `.c/.h/.m/.cpp/.inc` | 3,039 functions; 42 exceed 150 lines, 13 exceed 300, 1 exceeds 500 |
| Verbatim duplication | normalized 10-line window hashing across files, merged into contiguous runs | 92 cross-file pairs; largest listed in [Maintainability](#d-maintainability-and-duplication) |
| Hazard greps | `strcpy`/`strcat`/`sprintf`/`atoi` family; raw `malloc`/`free` outside `lib/src/memory`; `TODO`/`FIXME` | 23 unsafe-copy sites (13 outside tests); 181 raw allocation sites (110 outside tests); 3 real TODO markers |
| Build flags | `grep` of CMake for warning and architecture flags | no `-Wall`/`-Wextra`/`/W4` on any VKR target; `-Werror=implicit-function-declaration` only; `-march=native` on every non-MSVC target |

Not collected: Release harness timings, Metal validation runs, native Vulkan
execution, and a manual read of every per-draw loop. Items that depend on those
gates say so explicitly. The scanner scripts used above live in the untracked
`.scratch/codebase-audit-plan/` directory of the audit host; item A6 makes the
useful ones tracked.

## Execution rules for the implementer

1. One workstream item per commit, scoped Conventional Commit message, and the
   item's verification recorded in the commit body. Do not batch formatting
   with behavior changes.
2. Run `./build_test.sh` after every item and `./build_release.sh` plus
   `./build_editor.sh Release` after any item touching `renderer/`, `runtime/`
   or `editor/`. Inspect diagnostics, not only exit codes.
3. Any change on a per-frame path (workstream C, items D5, D6, D11, E2) needs a
   matched before/after `vkr_harness profile` on a Bistro performance case with
   validation variables unset, as required by `vkr-performance`. Attach the
   report digests to the commit. Never publish a baseline.
4. Shader edits (D6) follow `vkr-shaders`: edit both native roots or the shared
   kernel, rebuild both compilers, and run the existing Bistro snapshot case on
   Metal; record native Vulkan as unavailable on macOS.
5. Preserve the three pre-existing uncommitted tool edits present at the audit
   baseline unless the user has since committed or reverted them.
6. Ask before changing a public contract, an accepted budget, or an ADR-owned
   decision. Everything below stays inside existing contracts unless marked
   "decision".

Order of work: A1 to A4, then B, then D1, D2, D11, then the remaining D items,
then C, then E and F. Effort: S under one hour, M half a day, L multiple days.

## A. Build hygiene and static checks

### A1. Enable compiler warnings on VKR targets (M)

Evidence: no VKR target sets a warning level; `CMakeLists.txt:59-61` only adds
`/arch:AVX2` or `-march=native`, and `vkr_require_declared_c_functions` adds a
single error flag.

Change: add `vkr_apply_warnings(target)` next to
`vkr_require_declared_c_functions` in `CMakeLists.txt` and call it from
`vkr_configure_library` in `cmake/vkr_library_target.cmake` and
`vkr_configure_application_target` in `cmake/vkr_application_target.cmake` so
every VKR-owned target receives it. Flags: `-Wall -Wextra -Wshadow
-Wunused-but-set-variable -Wno-unused-parameter -Wno-missing-field-initializers
-Wno-sign-compare -Wno-missing-braces` (MSVC: `/W4 /wd4100 /wd4127 /wd4201`).
`-Wmissing-braces` is excluded by decision: the 22,104 hits come from
`(Vec2){x, y}`-style initializers of the anonymous-union vector types in
`lib/src/math/vec.h:194-202`, which are well-defined brace elision, and
rewriting them would harm every math call site. Vendored subdirectories and
the `stb_*`/`cgltf` implementation files keep their own settings (the existing
`vkr_apply_build_policy` already distinguishes them).

Fix the real findings the flags expose before enabling them:

| Warning | Location | Note |
|---|---|---|
| `-Wshadow` | `renderer/src/vkr_rg_json.c:3019,3020,3092,3093` | inner `resolved_name`/`owned_name` shadow the outer pair in the repeated-use loops |
| `-Wshadow` | `runtime/src/assets/vkr_mesh_cooked_decode.c:891`, `runtime/src/renderer/resources/loaders/material_loader.c:1468`, `runtime/src/vkr_sample_runtime.c:1603`, `tools/assets/vkr_mesh_encode.c:1177`, `tools/vkr_font_cooker.cpp:1200` | rename the inner variable |
| `-Wunused-but-set-variable` | `lib/src/memory/vkr_allocator.c:836` (`bytes_released`), `runtime/src/core/vkr_entity.c:549` (`end`, only consumed by compiled-out `assert_log`), `runtime/src/renderer/resources/loaders/material_loader.c:2220`, `runtime/src/renderer/systems/vkr_mesh_manager.c:2223-2224,3516`, `runtime/src/vkr_sample_runtime.c:1661` | delete the counter or feed it to metrics |
| `-Wunused-but-set-variable` | `renderer/src/vulkan/vkr_vulkan_draws.c:1020-1021` | counters are read only by a `log_warn` that Release strips (`LOG_LEVEL=1`); wrap the counters in the same condition or route them into the existing work-volume metrics |
| `-Wunused-variable` (non-test) | `runtime/src/vkr_sample_runtime.c` (6), `scene_loader.c` (4), `renderer/src/vulkan/vkr_vulkan_memory.c` (4), `material_loader.c` (3), `lib/src/math/mat.h` (3), plus 12 single hits | delete |
| `-Woverloaded-virtual` | `runtime/src/physics/vkr_physics.cpp:318` | `s_CharacterFilter::ShouldCollide(ObjectLayer)` hides `JPH::BodyFilter::ShouldCollide(const BodyID &)`; add `using JPH::BodyFilter::ShouldCollide;` so the intended default stays reachable and reviewers can see it |

Verification: Release, editor and test builds are warning-free for VKR targets;
then add `-Werror` for the same list under a `VKR_WARNINGS_AS_ERRORS` option
that the wrappers enable.

### A2. Triage the static-analyzer findings (M)

Evidence: 55 findings outside tests. Most are assert-guarded preconditions that
disappear in Release because `ASSERT_LOG=0` removes `assert_log`; those are
accepted by the "internal helpers consume proven data" rule and need no code
change. The remaining ones are listed under B with fixes. Add
`tools/checks/run_clang_checks.py` (item A6) so the analyzer count becomes a
tracked number, and record the accepted-precondition set in that script's
suppression list with the file and reason.

### A3. Add a format gate (S)

Evidence: 170 of 634 first-party C files drift from `.clang-format`.

Change: one formatting-only commit produced by `clang-format -i` over
first-party `.c`/`.h` files (not `.m`, `.metal`, `.slang`, `.inc` shader
sources; `.m` may be formatted only after a manual diff review). Then add
`tools/checks/check_format.py` invoked by `build_test.sh` and `build_test.bat`
before the path checks, failing on drift. Verification: the check passes, the
CPU suite still passes, `git diff --stat` shows only whitespace and line
breaks.

### A4. Replace `-march=native` with an explicit baseline (S, decision)

Evidence: `CMakeLists.txt:61`, `renderer/CMakeLists.txt:198`,
`tests/CMakeLists.txt:25` compile every non-MSVC target for the build host's
CPU. A Release binary built on one machine may execute illegal instructions on
another; `docs/ARCHITECTURE.md` build policy does not state this.

Change: introduce `VKR_TARGET_ARCH` (default `native` for developer builds,
`x86-64-v3` or `armv8.2-a` for distributable builds) and document it in the
build policy section. Verification: configure both values, build Release, run
the CPU suite.

### A5. Header guard consistency (S)

Evidence: 320 headers use `#pragma once`, one uses an `#ifndef` guard. Convert
the one to `#pragma once`.

### A6. Track the audit scanners (S)

Add `tools/checks/run_clang_checks.py` (warning and analyzer runner over
`compile_commands.json`, summary by kind and file), `tools/checks/check_format.py`
(A3) and `tools/checks/report_long_functions.py` (function-length report). Run
the two checks from the test wrappers; the reports are diagnostics only.

## B. Correctness

Verified defects and analyzer findings that survive review. Assert-only
precondition reports were excluded (see A2).

### B1. Uninitialized manifest limit tokens (S)

`tools/harness/vkr_harness_manifest.c:1122-1130`: `limit_tokens[3]` is
uninitialized, `vkr_harness_manifest_field` is called with its result
discarded, then `limit_tokens[i] >= 0` is read. A field that is absent leaves
garbage, which can select a random operator or dereference an invalid token.
Fix: initialize to `{-1, -1, -1}` and check the call result. Verification:
extend `tests/src/harness_test.c` assertion parsing with a manifest missing all
three limit keys; it must fail with the "exactly one limit" error.

### B2. Uninitialized `version` view on early parse failure (S)

`editor/src/editor_project_store.c:1742-1744`: `String8 version;` is
uninitialized; the analyzer reports garbage reads at `:1736/:1744` when the
member lookup path returns without assigning. Initialize `version = {0}` and
make `vkr_editor_project_json_member` assign its output on every return.
Verification: existing `editor_project_store_test` plus one malformed-manifest
case.

### B3. Null dereference on disabled-but-fired UI buttons (S)

`editor/src/editor_bakery.c:1013-1017` dereferences `selected` after a button
whose `config.disabled` depends on `!selected`; `editor/src/editor_scene_panels.c:1245-1247`
does the same with `asset->node_count` (also a division by zero when
`node_count == 0`). Whether `vkr_ui_button` can return true while disabled is
an implementation detail of the UI system; add explicit `if (!selected)` and
`if (!asset || !asset->node_count)` guards so the panel does not depend on it.
Also `editor/src/editor_animation.c:641` divides by `count` that the caller
may pass as zero; guard it. Verification: CPU suite; one UI-layout test that
calls the panel with no selection.

### B4. Bounded string copies (M)

Evidence: 13 non-test `strcpy` sites. `editor/src/editor_project_store.c:798,948,1088,1184,1209,1353,1452,1707,2006`,
`editor/src/editor_scene_panels.c:2304`, `editor/src/editor_projects.c:1649`,
`runtime/src/vkr_sample_runtime.c:4276`, and `lib/src/containers/str.c:473`
(`string_copy` forwards to `strcpy`). Each site was checked; most have a length
check a few lines earlier, but the check and the copy are separate facts and
two (`:1184`, `:1452`) rely on the source having been bounded by an earlier
function. Fix: add `vkr_string_copy_bounded(char *dst, uint64_t dst_size, const char *src)`
returning `bool8_t` to `lib/src/containers/str.h`, use it at every site, delete
the `strcpy` forwarding in `str.c`. Verification: `string_test` gains an
overflow case; editor project-store and scene-panel tests still pass.

### B5. Objective-C retain leaks in diagnostics tools (S)

`tools/metal/vkr_metal_texture_array_diagnostic.m:38-67` and
`tools/metal/vkr_metal_timestamp_diagnostic.m:1606` leak Metal objects per the
analyzer. Confirm whether the tools target compiles with ARC (`-fobjc-arc`);
if not, either enable ARC for `tools/metal` or release the objects. The
timestamp tool also has 32 raw `malloc`/`free` sites; route them through one
arena created at startup. Verification: build the tools, run each diagnostic
once, analyzer count for the two files reaches zero.

### B6. Analyzer findings to confirm or annotate (S each)

Check and either fix or record the reason in A6's suppression list:
`runtime/src/core/vkr_entity.c:401` and `:1324` (uninitialized argument /
assignment on the archetype insertion path), `runtime/src/platform/vkr_window_macos.m:42`
(null passed to a nonnull parameter), `tools/vkr_diffuse_baker.cpp:151` (same),
`tools/assets/mesh_loader_gltf.c:2873`, `tools/assets/vkr_collision_import.c:102`
(`node->mesh` when `node` may be null), `runtime/src/assets/vkr_mesh_cooked_decode.c:1467-1472`
(`mapped[i]` when `ranges.length` exceeds the mapped capacity),
`renderer/src/vulkan/vkr_vulkan_draws.c:521` (`binding` when
`lighting->subsurface` is unset), `runtime/src/renderer/systems/vkr_scene_frame.c:116,331,375,492`
(scratch arrays allocated only when a count is nonzero; make the zero-count
path skip the loop explicitly), `runtime/src/renderer/systems/vkr_scene_physics.c:1865,2141`
(`scene->physics` null when physics was never created; `physics_body` returns
null in that case, so add the invariant comment or an explicit check).

### B7. Prior audit debt still open (M)

The 2026-09-05 audit recorded but did not fix: path-allocation failure paths in
`lib/src/platform/vkr_filesystem_mac.c` and `vkr_filesystem_windows.c`, and a
non-atomic no-overwrite rename. Implement no-overwrite rename with
`renamex_np(RENAME_EXCL)` on macOS, `renameat2(RENAME_NOREPLACE)` where
available, and `MoveFileExW` without `MOVEFILE_REPLACE_EXISTING` on Windows;
make every path-allocation failure return `FILE_ERROR_OUT_OF_MEMORY` and
release partial work. Verification: `filesystem_test` gains an existing-target
rename case and an allocation-failure case using the test allocator.

## C. Performance

No measured regression was found; this audit did not run Release timings.
Items here are either verified cold-path facts or measurement tasks that must
precede any hot-path edit. Every item needs the matched profile from execution
rule 3.

### C1. Establish the current Bistro baseline record (M)

Run `tools/cases/performance/bistro_shadow_orbit.case.json` with
`tools/profiles/performance-windowed.json` and the `-gpu` profile, five
children each, and store the report digests in the task note. All later C, D5,
D6, D11 and E2 commits compare against these digests.

### C2. Per-frame input validation cost (M, measure first)

`renderer/src/vkr_renderer.c:2276` validates the whole `VkrFrameInput` every
frame through `vkr_frame_input_validate` (`renderer/src/vkr_frame_input.c:201`),
which walks every candidate and payload. The contract requires validation at
the input boundary, so the work is legitimate; measure its CPU scope on Bistro
and, if it exceeds one percent of `frame.wall`, move per-candidate checks that
extraction already proves (handle generation, submesh range) behind a
producer-proven flag on the packet.

### C3. UI system per-frame allocations (S, measure first)

`runtime/src/renderer/systems/vkr_ui_system.c` performs 29 frame-allocator
allocations per frame and rescans all 2,048 retained buckets
(`VKR_UI_RETAINED_BUCKET_CAPACITY`) for damage at `:2551`. Both are bounded and
arena-backed; record the UI CPU scope on the editor Bistro case and only act if
it is visible.

### C4. Graph-build name lookups (S)

`renderer/src/vkr_rg_json.c:2529-2560` searches images and buffers linearly by
name with a `TODO: use hash map`. All callers run at graph realization
(startup and resize), not per frame. Either implement the lookup through the
existing `VkrHashTable(String8)` or delete the TODO and record the cold-path
justification in a comment.

### C5. Float to double promotion in per-frame code (S)

`-Wdouble-promotion` fires 4,356 times; most are intentional (`mat4_inverse`
accumulates in `float64_t`, the animation graph uses doubles for orientation
tests, harness and cooker code is cold). Triage only per-frame files:
`runtime/src/renderer/systems/vkr_scene_animation.c` (26),
`renderer/src/vkr_froxel_fog.c` (15), `renderer/src/vkr_dof.c` (9),
`runtime/src/animation/vkr_animation_player.c` (8),
`runtime/src/renderer/systems/vkr_lighting_system.c`. Replace accidental double
literals and `fabs`/`fmod` calls with the `f` variants where precision is not
required; keep the deliberate double accumulations and mark them with a
comment.

### C6. Force-inline policy (M, decision, measure)

`lib/src/defines.h` defines `INLINE` as `always_inline` in Release and it is
applied to 497 functions, including 92 in `vec.h` and 48 in `mat.h`. The
repository contract says not to force-inline by habit. Change `INLINE` to plain
`static inline` and introduce `VKR_FORCE_INLINE` for the few call sites that a
Bistro profile shows to regress. Verification: matched Release profile (C1)
before and after; binary size delta recorded.

## D. Maintainability and duplication

### D1. One owner for cooked-format codec primitives (L)

Evidence, contiguous verbatim runs: `runtime/src/assets/vkr_mesh_cooked_decode.c`
and `tools/assets/vkr_mesh_encode.c` share about 760 lines (runs at
`decode:576/encode:584` 110 lines, `decode:68/encode:76` 97 lines,
`decode:517/encode:525` 67 lines, and eight more); `runtime/src/assets/vkr_font_cooked_decode.c`
and `tools/assets/vkr_font_encode.c` share about 406 lines
(`decode:123/encode:302` 114 lines, `decode:293/encode:472` 107 lines). The
copies contain the format constants, SHA-256, CRC32, little-endian
reader/writer helpers and checked `add_u64`/`mul_u64`. SHA-256 exists five
times (`vkr_mesh_cooked_decode.c`, `vkr_mesh_encode.c`, `tools/harness/vkr_harness_sha256.c`,
`tools/vkr_font_cooker.cpp`, `tools/bake/vkr_bake_material.cpp`) and CRC32 six
times (add `vkr_diffuse_volume.c`, `vkr_texture_transcode_cache.c`,
`vkr_font_cooked_decode.c`, `vkr_font_encode.c`). Checked arithmetic helpers
are also duplicated in `vkr_texture_transcode_cache.c`.

Change: add `lib/src/core/vkr_hash.c/.h` (SHA-256 streaming API and CRC32) and
`lib/src/core/vkr_byte_io.h` (bounded little-endian reader/writer, checked
add/mul). Move the mesh and font format constants and record layouts into the
existing `runtime/src/assets/vkr_mesh_cooked.h` and `vkr_font_cooked.h`, which
the tools already reach through `vkr_asset_formats` (`cmake/vkr_asset_targets.cmake:2-13`).
Delete every copy, including the harness and cooker SHA-256 implementations.
Verification: `mesh_cooked_tests`, `font_cooked_tests`, `harness_test` digest
cases, `texture_vkt_tests`; re-cook one fixture of each kind with the rebuilt
cookers and confirm the artifact SHA-256 is unchanged.

### D2. Harness legacy schema copies (M)

`tools/harness/vkr_harness_capture.c:27-470` and `tests/src/harness_test.c:1706-2140`
each carry verbatim copies of earlier case and capture-summary struct layouts
(`VkrHarnessCaseV8` and siblings) taken from previous revisions of
`vkr_harness.h:27-460`, so three files now define the same fields and any
schema change must be mirrored by hand; `vkr_harness_capture.c:880-1390` also
repeats one 10-line conversion block per field group. Keep a legacy layout only
if a checked-in baseline still uses that schema (`tools/baselines/*/capture-summary.bin`);
put the retained layouts in one `vkr_harness_legacy.h` owned by the harness
library and have the tests include it instead of copying. Collapse the
per-field conversion blocks into a field table. Verification: `harness_test`
legacy compatibility suite; `autotest` on the tracked Bistro snapshot
baselines.

### D3. Platform duplication (M)

Contiguous verbatim runs: `lib/src/platform/vkr_filesystem_mac.c:532` and
`vkr_filesystem_windows.c:674` (54 lines) plus six more runs totaling about 158
lines; `runtime/src/platform/vkr_window_macos.m:917`/`vkr_window_windows.c:452`
(30 lines) and four more runs (about 98 lines); `vkr_gamepad_macos.m:7`/`vkr_gamepad_windows.c:16`
(36 lines); `vkr_threads_mac.c:47`/`vkr_threads_windows.c:44` (27 lines).
`translate_keycode` is a 233-line switch on macOS and a 214-line switch on
Windows. Change: move platform-independent logic into `lib/src/platform/vkr_filesystem_common.c`,
`runtime/src/platform/vkr_window_common.c` and `vkr_gamepad_common.c`; replace
both keycode switches with `static const Keys` lookup tables indexed by native
keycode. Verification: `filesystem_test`, `input_test`, `event_test`, `threads_test`;
manual editor launch on macOS; Windows build remains a separate gate.

### D4. Font loader duplication (M)

`runtime/src/renderer/resources/loaders/mtsdf_font_loader.c:57` and
`system_font_loader.c:78` share 47 lines; `bitmap_font_loader.c:460` and
`system_font_loader.c:1117` share 32 lines; `mtsdf_font_loader.h` and
`system_font_loader.h` are identical apart from names. Extract the shared face
registration and atlas publication into `font_loader_common.c` owned by the
font system and keep one loader per source format. Verification: `text_test`,
`font_cooked_tests`, editor startup with system fonts.

### D5. Backend-shared blocks in renderer cores (M, measure)

Verbatim runs between backends: `renderer/src/metal/internal/vkr_metal_packet_graph.inc:99`
and `renderer/src/vulkan/vkr_vulkan_graph.c:124` (44 lines);
`vkr_metal_packet_resources.inc:690` and `vkr_vulkan_publisher.c:15` (26);
`vkr_metal_packet_lifecycle.inc:267` and `vkr_vulkan_renderer.c:1599` (24);
`vkr_metal_packet_renderer.m:1002` and `vkr_vulkan_renderer.c:1797` (17);
`vkr_metal_memory.c:60` and `vkr_vulkan_memory.c:128` (19). Each is portable
policy (graph instance selection, publication record validation, teardown
ordering, memory class accounting) that belongs in `renderer/src/vkr_gpu_*.c`
or `vkr_render_graph_frame.c`. Hoist each into the shared core and call it from
both backends. Verification: `render_graph_barrier_test`, `vulkan_test`,
`metal_memory_test`, Bistro snapshot on Metal, matched profile.

### D6. Shader code that should be shared kernels (M, `vkr-shaders`)

Within Metal, `renderer/src/shaders/metal/msl/world/default.metal:246` and
`gpu_draws.metal:1362` share 28 lines and about 255 lines in total across runs;
between backends `gpu_draws.metal:1696` and `vulkan/slang/world/deferred.slang:1893`
share 29 lines and about 117 lines in total; post passes (`subsurface`, `dof`,
`motion_blur`, `froxel_fog`, `ssgi`, `ssr`) each carry 12 to 19 identical lines
in both roots; `shared/ssgi_kernel.slangh:46` and `shared/ssr_kernel.slangh:43`
share 17 lines. Move each into the owning `shared/*_kernel.slangh` per
[ADR-044](../adr/044-shader-cross-backend-contract.md). Verification: both
shader compilers, Bistro snapshot on Metal with unchanged final-color digest,
native Vulkan recorded as unavailable.

### D7. Long functions (M)

42 functions exceed 150 lines. Production ones to restructure, in order:
`renderer/src/vulkan/vkr_vulkan_pipelines.c:1270` `vkr_vk_validate_deferred_root_abi`
(631 lines; the per-root field tables already exist, so drive them from one
`{root name, fields, count}` table and a single loop, and do the same for
`vkr_vk_validate_packet_root_abi` at `:193`, 373 lines, and
`vkr_vk_validate_transmission_root_abi` at `:776`);
`renderer/src/vulkan/vkr_vulkan_resources.c:1892` `vkr_vk_publish_sentinel_descriptors`
(321) and `:1703` `vkr_vk_create_descriptor_slot_tables` (188) into per-table
helpers; `renderer/src/vulkan/vkr_vulkan_renderer.c:1893` `vkr_vulkan_renderer_destroy`
(256) and `renderer/src/metal/internal/vkr_metal_packet_lifecycle.inc:680`
`vkr_metal_packet_renderer_destroy` (215) into ordered per-owner teardown
helpers that mirror creation; `editor/src/editor_viewport.c:580`
`vkr_editor_grid_build` (251); `renderer/src/vkr_rg_compile.c:1068`
`vkr_rg_ensure_barrier_state` (245), `:1786` `vkr_rg_compile_schedule` (192),
`:1599` `vkr_rg_generate_barriers` (186); `runtime/src/renderer/resources/ui/vkr_ui_text.c:98`
`vkr_ui_text_generate_geometry` (244); `renderer/src/vulkan/vkr_vulkan_device.c:299`
`vkr_vk_create_instance` (167); `runtime/src/renderer/systems/vkr_scene_system.c:1210`
`vkr_scene_shutdown` (157). Each split keeps the helper `static` in the same
file; no new files. Verification: the owning CPU suite for each file and the
Release builds.

### D8. Oversized translation units and mixed responsibilities (M, decision)

File length alone does not justify a split, so only split where an independent
responsibility exists: `runtime/src/vkr_sample_runtime.c` (5,055 lines, 89
functions) mixes command-line parsing and configuration (`:4200-4310`, the only
`fprintf` sites) with frame control; move the CLI/config into
`vkr_sample_runtime_config.c`. `runtime/src/renderer/resources/loaders/scene_loader.c`
(5,209) mixes JSON parsing with scene publication; evaluate a parse/publish
boundary. `renderer/src/vulkan/vkr_vulkan_internal.h` (3,353) is the single
header for every Vulkan owner; leave it unless D5 moves shared state out.
Ask before splitting anything else.

### D9. Raw allocations outside the allocator contract (M)

110 non-test raw `malloc`/`free` sites, concentrated in
`runtime/src/renderer/systems/vkr_texture_system.c` (26: upload data and
regions at `:2374,2552,2876,3437`, query-less keys at `:1587,1784`),
`vkr_texture_transcode_cache.c` (10), `runtime/src/platform/vkr_window_windows.c`
(7), `scene_loader.c` (4), `lib/src/platform/vkr_entry.h` (4). Only the
`stbi_image_free` pairing at `:1122` is justified. Upload data outlives the
request until GPU completion, so it belongs in `VkrDMemory` with the existing
retirement path; keys and regions belong in the owning system's allocator.
Verification: `texture_lifetime_test`, `resource_async_state_tests`, allocator
tag totals reconcile after a Bistro load/unload cycle.

### D10. TODO markers (S)

`renderer/src/vkr_rg_json.c:2529,2546` (resolved by C4) and
`runtime/src/core/input.h:351` ("multi-device input design"): either open a
proposal section in [editor-ui-extensions](editor-ui-extensions.md) or delete
the marker.

### D11. Metal packet renderer as real translation units (M, measure build time)

`renderer/src/metal/vkr_metal_packet_renderer.m:960-966` includes seven `.inc`
files into one 14,000-line translation unit; every edit recompiles all of it,
and file-local `static` helpers are shared across the `.inc` files without a
declared interface. The parts have independent responsibilities (setup,
resources, graph, commands, frame, lifecycle, animation preview), which
satisfies the repository's rule for separate files. Convert each `.inc` into a
`.m` file with an internal header `vkr_metal_packet_internal.h` declaring the
cross-file helpers. Verification: Release editor build, Bistro Metal snapshot
digest unchanged, incremental rebuild time recorded before and after.

### D12. Public surface of large headers (S, review)

`renderer/src/vkr_renderer.h` (1,326 lines), `runtime/src/renderer/systems/vkr_scene_system.h`
(964), `renderer/src/vkr_frame_input.h` (574), `vkr_shadow_system.h` (618),
`vkr_mesh_manager.h` (591): list every exported function with no caller outside
its own translation unit and make it `vkr_internal`. Use the compiler with A1's
flags plus `-Wmissing-prototypes` to find them.

## E. Readability

### E1. Formatting (see A3).

### E2. Math header prose and inline policy (M)

`lib/src/math/mat.h` (1,800 lines), `vec.h` (1,281), `vkr_simd.h` (1,329) and
`vkr_quat.h` (506) carry Doxygen blocks that restate the signature (for
example `vec2_new` at `vec.h:264-269`). Remove comments that restate code;
keep units, ranges, handedness and singularity notes. Apply C6's inline policy
in the same pass. Verification: `vec_test`, `mat_test`, `quat_test`,
`simd_test`; matched Release profile from C1.

### E3. Logging versus stderr (S)

`fprintf(stderr, ...)` is used only for command-line usage errors in
`runtime/src/vkr_sample_runtime.c:4205-4359` and platform bootstrap messages;
this is acceptable for CLI diagnostics. Record that rule in one comment at the
first site and route anything that is not a CLI usage error through the logger.

## F. Tests

### F1. Suite timing and isolation (S)

87 suite registrations run in one process. Add per-suite wall time to
`tests/src/test_main.c` output so slow suites are visible, and make each
`mkdtemp` fixture (`tests/src/harness_test.c:988,1325,1526,2197,2705,3170,3376`,
`tests/src/metal_diagnostics_test.c:62`) remove its directory on assertion
failure through one cleanup helper.

### F2. Named-failure tests for items in this plan

Only add the tests named in B1, B2, B3, B4, B7 and the re-cook check in D1.
Do not add tests that mirror the implementation.

### F3. Raw allocations in tests (S)

`tests/src/freelist_test.c` (34 raw sites) and `tests/src/harness_test.c` (18)
allocate with `malloc`; convert to the existing `container_test_allocator.h`
so allocator accounting assertions cover them.

## Coverage and limits

This audit read the evidence listed above and the code around every cited
line. It did not read every function in the 300,000-line first-party tree, run
Metal API validation, run Release timings, or execute native Vulkan. Known
architecture limits in [ARCHITECTURE.md](../ARCHITECTURE.md#remaining-implementation-and-evidence-boundaries)
(three-image Metal target, AGX panics, backend edge-policy differences,
transmission redesign) are not restated here. GPU lifetime and per-draw loop
review for the Vulkan and Metal recorders remains a targeted follow-up once
D5 and D11 have made those paths smaller.

## Acceptance

The plan is complete when: A1 warnings are errors on all VKR targets; A3 and A6
checks run from both test wrappers; every B item has its named test; D1 leaves
one SHA-256 and one CRC32 implementation in the tree; D11 leaves no `.inc`
under `renderer/src/metal/internal/`; the function-length report shows no
production function above 300 lines; and every measured item carries its
matched Bistro report digests. Update [ARCHITECTURE.md](../ARCHITECTURE.md)
build policy for A4, then remove this proposal.
