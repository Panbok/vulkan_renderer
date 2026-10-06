---
status: proposed
updated: 2026-10-06
authority: proposal
---
# Codebase audit remediation: remaining work

The 2026-09-23 audit plan was implemented on the local branch
`audit-remediation` (baseline `abeedc19`). Each resolved item's commit
records its change and verification, so `git log abeedc19..audit-remediation`
is the implementation record. On 2026-09-24 the owner accepted the
recommended option for every open decision; the list below maps each one to
its commit. This document keeps only what is still open, work deferred with
evidence, and checks this macOS host cannot run.

The [repository contract](../../AGENTS.md) applies to every item.

## Owner decisions applied

| Decision | Outcome |
| --- | --- |
| Scene loader publication paths | `e66953c3`: synchronous loads run the staged prepare/finalize path; `scene_load_json_owned` is gone. The commit lists the synchronous API's behavior changes. |
| Instance-buffer metrics | `c97f789b`: the three rows are retired, with their assertions in six cases and their entries in six profiles. |
| Tracked Bistro text baselines | `7c34698f` lets a snapshot replace an unreadable baseline; `a4b51639` re-accepts the Metal baseline. The Vulkan baseline is open below. |
| `vec2_equal` epsilon boundary | `588f6229`: the bound is inclusive, as in `vec3_equal` and `vec4_equal`. The Bistro re-cook is open below. |
| Metal packet renderer translation unit (plan D11) | Kept as one unit. A whole rebuild takes 1.8-2.9 s, and Objective-C gets no ThinLTO, so a split would export about 128 helpers that could no longer be inlined. |
| Ungated checks | `f6e7253c`: `build_test.sh` runs the 300-line function check and `tools/checks/check_metrics_disabled.py`. |
| Versioned capture records | The summary layout table records each version's capture and artifact record sizes; a stored layout that differs from the current structs is rejected until it has a converter, and the record pin forces a version bump when a record grows. |

## Open

### Bistro Vulkan text baseline

The tracked `smoke.bistro.vulkan.text.snapshot` baseline, accepted on
2026-08-07, is a version 2 summary. It was unreadable because the reader's
version 2 layout was reconstructed from version 3: the real writer's case had
no resize controls, a 76-byte renderer config and a provenance without
`world_renderer`, so its header is 77,792 bytes, not 77,904. Its capture
records are the current 2,072 bytes; the earlier "2,064-byte records"
diagnosis divided the wrong header size. The reader now uses the exact
version 2 layout, and a CPU test decodes this tracked file. Whether the old
baseline still matches today's Vulkan output needs a Vulkan machine: run the
snapshot there and, if the output changed, review and accept a new
generation.

### Bistro re-cook after the `vec2_equal` change

Only mesh-cook deduplication calls `vec2_equal`, so existing cooked assets are
unchanged until the next cook. A re-cook of `falcon.obj` in an isolated
workspace was byte-identical under both comparators. Bistro was not
re-cooked: a cook regenerates its 2.9 GB spec-gloss texture cache, the data
disk had under 5 GB free, and the mesh cooker commits several GB of memory on
large sources. The change can only merge vertex pairs whose texcoords differ
by exactly 2^-23, which the cooked format's UV quantization already stores
identically, so a re-cook can lower the vertex count but cannot change
rendering.

## Deferred with evidence

### Allocation exceptions kept on the C heap (plan D9)

These stay malloc-owned:
- Texture upload payloads: `VkrTexturePreparedLoad`, decode results and
  transcode cache records. Both publishers copy them before publication
  returns, decode workers allocate them in parallel, and `VkrDMemory` never
  decommits freed pages.
- The window platform state and the Windows UTF-8 argv shim.

The Windows window-creation failure paths repeat `free(state)`. Converting
them to one cleanup path needs a Windows build.

### Render-graph JSON read and write builders

`vkr_rg_json_add_pass_reads` and `vkr_rg_json_add_pass_writes` in
`renderer/src/vkr_rg_json.c` differ only in the use list they read and three
pass-builder calls. The repository contract disfavors a flag-driven merge, and
a table of builder functions would put indirect calls into the per-frame
graph build. They stay as they are unless that path is measured.

### Performance evidence

The authoritative `performance-windowed` profiles refuse a dirty worktree.
This tree carries uncommitted user edits and the configure-patched KTX
submodule, so every timing in these commits is a local, non-authoritative
observation (`local-windowed`, `bistro_shadow_orbit`).

The local Bistro orbit is GPU-bound at about 40.4 ms
(`frame.command_slot_waits` is 1 per frame). `Lighting.Deferred.Fullscreen`
takes 17.4 ms of the 40 ms GPU time, a larger target than any CPU item here;
see [renderer features and performance](renderer-features-perf/renderer-features-perf.md).

The Metal upload and submission splits each had four alternating A/B runs
per side:

| Split | `cpu.render_submit` p50 median | Untouched `cpu.graph_build` |
| --- | --- | --- |
| Upload | 1.94 -> 2.13 ms | 0.276 -> 0.296 ms |
| Submission | 2.04 -> 2.12 ms | 0.281 -> 0.299 ms |

Normalized by `cpu.graph_build`, the upload split is within 2% and the
submission split is not slower. Rerun C4, C6 and both splits on a clean
checkout before quoting any of them as a change.

## Unavailable on this host

- Native Vulkan execution and validation for every renderer change. Vulkan compiles on macOS with
  warnings as errors, and the non-Apple branches were syntax-checked. The
  Vulkan IBL commit fix (`0bffcb18`) and the debug-messenger fix (`3680d390`)
  were verified by compilation and reading only.
- Windows builds, including the MSVC `__forceinline` removal and the Windows
  window code.
- A fresh configure without `VULKAN_SDK` finds the Homebrew Vulkan headers
  (1.4.328) before the SDK (1.4.357), and `vkr_vulkan_wsi.c` then fails on
  `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`. The existing build directories
  cache the SDK path.
- `renderer/src/vkr_sheen_lut_data.inc` was generated on Windows. The macOS
  `vkr_sheen_cooker` output differs from it on 600 lines both before and
  after the D7 split, so the split was checked byte-identical against macOS
  output instead.
- Failure paths without a fixture: the mesh-manager merged-geometry release
  (`e96d585c`), the Metal pipeline-creation library release (`44ff8c75`) and
  the scene loader's destruction of an untracked non-node mesh instance
  (`e66953c3`) were checked by reading.

## Declined

F3 (routing test fixtures through a test allocator): no named failure.
