---
status: implemented
updated: 2026-07-31
authority: spec
---
# P2 Throughput — Measured Findings

> **Headline.** Frustum culling and separate shadow visibility ship. Instancing
> and multi-draw-indirect do **not**, because the measurement shows they would
> collapse exactly zero draws on the shipped content. Frame time is unchanged:
> 19.671 ms → 19.637 ms average, which is noise.

## Why this document exists

`docs/architecture/renderer-architecture-spec.md` §8 P2 assumes the world path's
per-draw loop is a throughput problem worth attacking with culling, instancing,
and MDI. Per AGENTS.md an unmeasured performance claim is not a result, so each
item was measured before and after rather than assumed.

Two of the four assumptions did not survive contact with the content.

## Method

Release build, headless, fixed camera (no input), Sponza:

```sh
./build_release.sh
VKR_AUTOLOAD_SCENE=1 VKR_BENCHMARK_LOG=1 VKR_BENCHMARK_LABEL=<label> \
  VKR_AUTOCLOSE_SECONDS=60 ./build_release/app/vulkan_renderer
```

`BENCHMARK_SAMPLE` now carries the visibility and merge counters added for this
work: `vis_tested`, `vis_cull_cam`, `vis_cull_shadow`, `mergeable`,
`distinct_keys`, `max_run`.

Both runs use the same camera, so the comparison is like-for-like. A moving
camera would make culling numbers vary per frame and is the obvious follow-up.

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

## Finding 2 — culling cannot reject anything on this content

Culling was implemented twice. At object granularity it tested 6 objects and
rejected 0 — unsurprising, since two of them are Sponza-sized and contain the
camera. At **submesh** granularity it tests 36 and still rejects 0.

The reason is material merging. Measured submesh bounding radii for the curtains
model are 6.5–9.6 against a whole-model radius of 10.2: every submesh spans
essentially the entire model, because its geometry is scattered across the whole
building. There is no spatial locality left to cull against.

Culling ships anyway, because it is correct, costs 36 sphere tests per frame,
and is a prerequisite for Finding 3. But on this content it is a no-op, and
saying otherwise would be inventing a result.

**What would make culling pay:** per-primitive submeshes rather than
material-merged ones, or splitting merged submeshes spatially, or moving to
per-meshlet GPU culling. All three are asset-pipeline or architecture changes
well beyond P2.

## Finding 3 — separate shadow visibility is a correctness fix, not a speedup

The shadow payload previously aliased the world payload's arrays outright. The
moment culling rejects anything, every camera-culled object silently stops
casting a shadow.

Camera and light visibility are now classified independently in one traversal,
and the shadow caster list is built from the widest cascade's volume (cascades
are nested, so the widest bounds every caster any cascade needs). This is
unobservable today precisely because nothing is culled — which is exactly why it
needed to land *with* culling rather than after it.

`tests/src/draw_merge_test.c` pins the property directly: an object outside the
camera volume but inside the light volume must be marked
`VKR_VISIBLE_SHADOW` and not `VKR_VISIBLE_CAMERA`.

## Finding 4 — instancing and MDI have nothing to merge

Two draws can be merged only if they are the same submesh of the same geometry
with the same material — one asset drawn more than once. Measured on Sponza:

- 35 opaque draws
- **35 distinct merge keys**
- **largest mergeable run: 1**

Every draw is unique. The scene is two different models, each submesh a distinct
material; nothing repeats. Instancing would collapse 0 draws, and MDI — which
additionally requires shared descriptors and vertex/index buffers — has no
groups of size greater than one to work with even before those constraints.

Neither is shipped. Implementing instancing requires reordering instance-record
emission so a merged run is contiguous, which is a real refactor of a working
draw path with, on this content, a provably zero payoff. That trade is not worth
making on a guess.

The merge measurement itself is tested against synthetic inputs with known
answers (`test_repeated_asset_is_mergeable` expects `mergeable=2`,
`max_run=3`), so the zero is a measurement, not a broken counter.

**What would make instancing pay:** content that instances one asset many times
— foliage, props, modular kits. The engine already supports it: `VkrMeshAsset`
is shared across `VkrMeshInstance`s, so N instances of one asset would produce
runs of length N. The metric is now in the benchmark line, so the moment such a
scene exists the payoff is visible without new instrumentation.

## Incidental finding — the projection and view matrices disagree on handedness

`mat4_perspective` builds a **+Z-forward (left-handed)** projection
(`m22 = far/(far-near)`, `m32 = +1`), while `mat4_look_at` builds a
**−Z-forward (right-handed)** view. Verified with the engine's own functions: a
point at world −10 Z, with a camera at the origin and
`VKR_DEFAULT_CAMERA_FORWARD = (0,0,-1)`, lands at clip `w = -10` — behind the
eye — while a point at +50 Z is inside the clip volume.

Rendering is unaffected: the shader uses `mul(projection, mul(view, world_pos))`
and is self-consistent, and the frustum is extracted from that same product, so
culling agrees with what is actually rasterized. `tests/src/draw_merge_test.c`
pins that agreement over an 860-point sample: anything inside the clip volume
survives the frustum test.

What *is* wrong is the documented meaning of the camera's `forward` vector: the
effective view direction is its negation. Nothing depends on that today, but it
is a trap for any future code that reasons about camera direction — picking rays,
audio, AI visibility. Worth reconciling deliberately rather than discovering it
again.

## Status against spec §8 P2

| Item | Outcome |
|---|---|
| 11. Cull before materializing world payloads | Shipped; rejects 0 on this content, root cause documented |
| 12. Use real instancing first | **Not shipped** — 0 mergeable draws measured |
| 13. Use MDI only for meaningful groups | **Not shipped** — largest binding-compatible run is 1 |
| 14. Keep camera and shadow visibility separate | Shipped; correctness fix, pinned by test |
