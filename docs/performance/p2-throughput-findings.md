---
status: investigation
updated: 2026-08-20
authority: investigation
---
# P2 Throughput — Measured Findings

> **Historical instrument note.** The measurements below remain the evidence
> for the shipped P2 decision, but their `BENCHMARK_SAMPLE` command was retired
> in harness Phase 2b. New measurements use `vkr_harness profile` and the
> authoritative workflow in `.codex/skills/vkr-performance/SKILL.md`; do not use
> the command below as current guidance.
>
> The `VKR_DISABLE_MDI` A/B switch described below was also retired with the
> Vulkan 1.2 frontend submission systems in V7. It remains here only to explain
> how the historical measurements were obtained.

> **Headline.** All four P2 items ship. Culling rejects ~37% of submeshes on
> San Miguel (0% on Sponza). Instancing and multi-draw-indirect are implemented
> and MDI demonstrably fires — the shadow pass collapses **1124 draw calls into
> 8 indirect calls** every frame. Frame time does not move: 20.57 ms with MDI
> vs 20.91 ms without, across three interleaved runs whose own spread is larger
> than the gap. Draw submission is ~2% of a ~20 ms frame, so removing it cannot
> show up.

## Why this document exists

`docs/architecture/renderer-architecture-spec.md` §8 P2 assumes the world path's
per-draw loop is a throughput problem worth attacking with culling, instancing,
and MDI. Per AGENTS.md an unmeasured performance claim is not a result, so each
item was measured before and after rather than assumed.

Two of the four assumptions did not survive contact with the content, and the
two remaining ones shipped with numbers that say where the frame time actually
goes.

## Method (historical)

Release build, headless, no input. Two scenes: Sponza (36 draws) for Findings
1-3, San Miguel (282 submeshes) for Finding 4.

```sh
./build_release.sh
VKR_AUTOLOAD_SCENE=1 VKR_BENCHMARK_LOG=1 VKR_BENCHMARK_LABEL=<label> \
  VKR_AUTOCLOSE_SECONDS=60 ./build_release/app/vulkan_renderer
```

`BENCHMARK_SAMPLE` carries visibility and merge counters plus separate logical
command/API-call counters: `vis_tested`, `vis_cull_cam`, `vis_cull_shadow`,
`opaque_before`, `opaque_after`, `mergeable`, `max_run`, `geoms`,
`geom_mat_pairs`, `world_commands`, `world_calls`,
`world_indirect_commands`/`world_indirect_calls`, and the corresponding shadow
counters. The original capture used one ambiguous `indirect` counter; the P2
review split it after confirming that it mixed shadow commands into world
metrics.

At the time of the capture, `VKR_DISABLE_MDI=1` turned the indirect path off at
runtime, so the A/B in Finding 4 used the same binary against the same content.

Culling counts vary frame to frame on San Miguel because the camera moves, which
is why Finding 4 compares 40-second means over three interleaved runs rather
than single frames.

## Results

| | Before | After |
|---|---|---|
| avg frame time | 19.671 ms | 19.637 ms |
| min frame time | 8.328 ms | 8.263 ms |
| world draws | 36 | 36 |
| world draw calls | 36 | 36 |
| objects tested | — | 36 |
| culled (camera) | — | **0** |
| culled (shadow) | — | **0** |
| mergeable opaque draws | — | **0** |
| distinct merge keys | — | 35 |
| largest mergeable run | — | **1** |

## Finding 1 — the scene is 36 draws, not hundreds

The two glTF models contain **447 primitives** across 132 mesh nodes, but the
renderer collects **36 draws** from **2 instances / 45 submeshes**. The importer
merges primitives by material, so a "submesh" is all geometry in a model sharing
one material.

The per-draw world loop the spec flags as a weakness is therefore already
issuing about one draw per material for the whole scene. Whatever costs 19 ms
per frame here, it is not draw-call submission: the render graph's whole CPU
time is 0.15–0.28 ms.

## Finding 2 — culling rejects nothing on Sponza, ~37% on San Miguel

Culling was implemented twice. At object granularity it tested 6 objects and
rejected 0 — unsurprising, since two of them are Sponza-sized and contain the
camera. At **submesh** granularity it tests 36 and still rejects 0.

The reason is material merging. Measured submesh bounding radii for the curtains
model are 6.5–9.6 against a whole-model radius of 10.2: every submesh spans
essentially the entire model, because its geometry is scattered across the whole
building. There is no spatial locality left to cull against.

Culling ships anyway, because it is correct, costs 36 sphere tests per frame,
and is a prerequisite for Finding 3. But on Sponza it is a no-op, and saying
otherwise would be inventing a result.

On **San Miguel** it is not a no-op: 282 submeshes tested, 102-107 rejected
depending on camera position — about **37%** of the scene, and the world draw
count tracks it (175-180 draws from 282 submeshes). That scene has the spatial
locality Sponza's material-merged submeshes destroyed, which is the difference
the next paragraph predicts.

**What would make culling pay:** per-primitive submeshes rather than
material-merged ones, or splitting merged submeshes spatially, or moving to
per-meshlet GPU culling. All three are asset-pipeline or architecture changes
well beyond P2.

## Finding 3 — separate shadow visibility is a correctness fix, not a speedup

The shadow payload previously aliased the world payload's arrays outright. The
moment culling rejects anything, every camera-culled object silently stops
casting a shadow.

Camera and light visibility are classified independently in one traversal. The
review corrected the light test to keep the union of every cascade volume:
cascade centers shift with their receiver slices, so the final/widest cascade
does not necessarily contain earlier cascades. Testing only it could drop a
near-only caster.

P21 later removed that CPU classifier and its `draw_merge_test.c` coverage.
The surviving frustum properties live in `tests/src/visibility_test.c`; the
camera-plus-cascade GPU partition is covered by the focused P16/P17 harness
case instead.

## Finding 4 — instancing and MDI ship, and MDI fires only in the shadow pass

Both were implemented after the Sponza measurement, on the owner's call, and
re-measured on **San Miguel** (`assets/scenes/san_miguel.scene.json`) — 282
submeshes rather than Sponza's 36.

### What the content turned out to be

```
vis_tested=282 vis_cull_cam=102 vis_cull_shadow=0
opaque_before=180 opaque_after=180 mergeable=0 max_run=1
geoms=1 mats=180 indirect=1124
```

`geoms=1 mats=180` is the whole story. San Miguel is one `.obj` whose submeshes
**share a single geometry buffer** but carry a **distinct material each**. So:

- **Instancing merges nothing** (`mergeable=0`, `max_run=1`). Merging requires
  the same submesh of the same geometry with the same material; no two draws
  here agree on material.
- **World-pass MDI batches nothing.** A batch may only span draws sharing
  pipeline *and* descriptor set *and* buffers. The per-material descriptor set
  changes on every draw, so every batch closes at size 1 and falls back to a
  direct draw.
- **Shadow-pass MDI batches everything.** Opaque shadow casters are depth-only:
  the pass binds its pipeline and instance state once for the whole list and
  never touches a material. Geometry and index buffer are the only things left
  that can break a batch — and there is exactly one geometry. So all 281 opaque
  casters × 4 cascades collapse into **8 indirect calls** (batches are capped at
  256 commands), which the old `indirect=1124` counter represented. Current
  telemetry reports this as `shadow_indirect_commands=1124` and
  `shadow_indirect_calls=8`.

This asymmetry is the useful result: **MDI's reach is set by how much state a
pass binds per draw, not by how many draws it has.** The depth-only pass was
already binding-uniform and got the full win; the world pass cannot until
materials stop owning a descriptor set each.

### Frame time: no measurable change

The historical A/B used the same binary, scene, and camera path via the now
retired `VKR_DISABLE_MDI=1` switch, with three interleaved runs of 40 s each:

| run | MDI on | MDI off |
|---|---|---|
| 1 | 19.984 ms | 20.578 ms |
| 2 | 21.333 ms | 22.307 ms |
| 3 | 20.391 ms | 19.844 ms |
| **mean** | **20.57 ms** | **20.91 ms** |

Run 3 reverses the ordering and the within-mode spread (1.3–2.5 ms) exceeds the
between-mode gap (0.34 ms). The honest reading is **no measurable difference**.

The reason is visible in the same line: `rg_cpu_total_ms` is 0.32–0.53 ms of a
~20 ms frame. The entire render graph — every pass, every draw call, every
barrier — is about 2% of frame time. Deleting 1116 draw calls from a 2% slice
cannot move the total. San Miguel at ~20 ms is GPU-bound on shading, not on
submission.

Both features ship regardless, because their work-volume effect is real and
they are prerequisites for content that *is* submission-bound. The original
frame-time A/B is retained as historical evidence; the review fixed correctness
and telemetry defects described below before treating the path as accepted.

**What would make instancing pay:** content that instances one asset many times
— foliage, props, modular kits. `VkrMeshAsset` is already shared across
`VkrMeshInstance`s, so N instances of one asset produce a run of length N.

**What would make world-pass MDI pay:** bindless or material-table descriptors,
so draws stop needing a descriptor change each. That is the prerequisite the
spec already suspected, now measured rather than assumed.

The merge measurement itself is tested against synthetic inputs with known
answers (`test_repeated_asset_is_mergeable` expects `mergeable=2`, `max_run=3`),
so the zero on this content is a measurement, not a broken counter.

## Review corrections

The P2 review found four correctness issues hidden by the measured scenes:

1. Indirect submission always rebound a geometry's default index buffer even
   when range resolution selected its compacted opaque index buffer. Direct and
   indirect capability paths could therefore render different indices. MDI now
   binds the batch's selected index buffer.
2. Cascade culling assumed the last/widest cascade contained every earlier
   cascade. Their centers shift, so shadow visibility now tests their union.
3. World instancing ignored position-dependent local reflection-probe
   descriptors. A probe binding context now prevents merging across positions
   while a local probe is pending or ready; pending matters because the IBL
   pass can make it ready after packet construction.
4. Merge measurement allocated/sorted a duplicate key array before sorting the
   candidates again. The candidate sort now supplies the run metrics directly.
5. With shadows disabled, the conservative classifier still marked every
   camera-culled object shadow-visible, forcing material lookup and candidate
   construction for payloads that could not be submitted. The extraction path
   now clears shadow visibility when no shadow payload was requested.
6. Single-buffer render-graph targets were fixed to serve every swapchain image,
   but the rule had no regression test. A CPU graph-execution test now exercises
   image index 2 against a single target.

The benchmark counters were also corrected to distinguish logical commands
from actual API calls and to keep world and shadow submission separate. The
sample application's benchmark-only San Miguel default was reverted to Sponza.

## Resolved finding — projection and view handedness disagreed

The report was verified. `mat4_perspective` used `m22 = far/(far-near)` and
`m32 = +1` (left-handed) while `mat4_look_at`, camera/shadow extraction,
direction helpers, shaders, and documentation all required right-handed `-Z`
forward. A nominal point at world `-10 Z` produced negative clip W; `+Z` was
raster-visible while movement/shadow code reasoned about `-Z`.

`mat4_perspective` now uses the right-handed Vulkan-ZO signs, and the default
sample camera moved to positive Z so it still starts facing the scene. Tests
assert the declared axis, positive W in front, near/far NDC depth, points behind
the eye, forward controller motion, and the independent clip-volume/frustum
invariant over the sample. The controller's old sign inversion had compensated
for the invalid projection and would have made `W` move backward after the
projection fix, so positive forward input now moves along `camera->forward`.
The review also moved renderer-facing orthographic cameras to the same
Vulkan-ZO, Y-inverted projection and removed a matrix-shape heuristic that
misclassified orthographic Vulkan projections as OpenGL depth.

## Status against spec §8 P2

| Item | Outcome |
|---|---|
| 11. Cull before materializing world payloads | Shipped; rejects 0 on Sponza, ~37% on San Miguel |
| 12. Use real instancing first | Shipped; merge key includes descriptor binding context, and pending/ready local probes conservatively prevent cross-position merging |
| 13. Use MDI only for meaningful groups | Shipped; selected index buffer matches direct fallback, 1,124 shadow commands → 8 calls |
| 14. Keep camera and shadow visibility separate | Shipped; camera list independent and shadow list uses the union of all cascades |
