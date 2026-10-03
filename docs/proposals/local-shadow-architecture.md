---
status: proposed
updated: 2026-10-02
authority: proposal
---
# Local shadow architecture

Local-light shadows that never switch on or off for a visible static light,
built in layers that later phases extend without replacing. The implemented
system is [ADR-019](../adr/019-bounded-forward-spatial-lighting.md); this
proposal changes what decides which lights are shadowed.

## Problem

Bistro has 72 point lights, all with a 7.5 m range except one at 15 m. glTF
import makes every one a shadow caster. The renderer shadows at most 5 (High)
or 10 (Ultra) of them per frame and re-selects every frame, so lights cross the
budget while the camera moves and their shadows crossfade over 0.25 s.

Indoors this matters most. Six lamps sit inside the café around the indoor
probe at (-7.5, 2.2, 9.0), yet the ranges of 7 to 21 lamps contain each of the
five owner-supplied indoor cameras (`bistro_light_leak_camera_1` to `_4` and
`bistro_light_leak_owner_interior`); most are exterior lamps whose unshadowed
light passes through walls. Within 15 m of those cameras are 40 to 51 lamps.

## Current baseline

Since 2026-10-02 the phase 1 cache is implemented per ADR-019: every
shadow-casting light is resident in one shared multi-layer 4096² D16 atlas,
with face sizes fixed by range; invalid and stale faces redraw by importance
within the preset's face budget per frame, and a transmission pool of one layer
per budgeted face serves the most important lights. Importance is measured
visible contribution on both backends, distance until a sample arrives.
Shadows fade out by camera distance. The `Shadow.LocalMask` pass writes
per-pixel overlap slots that deferred lighting reads, with inline filtering
past eight lights or for forward and transmission shading. Phase 1 still lacks
its evidence gates and the measured format choice; the distance fade did not
lower the cost of the indoor and street views, where nearby lights dominate
(ADR-019).

The measurements below predate the cache: one 4096² D32 atlas per physical
target image holding up to 64 faces, selected per frame.

Measured on the Windows host (AMD Radeon RX 6700 XT, RDNA2, driver 26.6.3,
Ryzen 5 2600), Vulkan Release, 1280x720, non-authoritative local profiles,
recorded with commands and reports in ADR-019:

| Case | Configuration | Result |
|---|---|---|
| `local_shadow_bistro_vulkan_street` | High, steady, all faces reused | `Shadow.LocalMask` 1.51 ms, `Lighting.Deferred` 1.35 ms, frame 5.27 ms (medians) |
| `local_shadow_bistro_vulkan_street_ultra` | Ultra | +0.025 ms mask, +0.06 ms lighting, +0.13 ms frame over High |
| `local_shadow_bistro_vulkan_walk` | High, distance / contribution ranking | unshadowed visible local light 43% / 26%; crossfading 1.4% / 2.0% mean, 10.4% / 8.6% p95 |
| `local_shadow_bistro_vulkan_walk_ultra` | Ultra, distance / contribution ranking | unshadowed 23% / 13%; crossfading 0.6% / 0.9% mean, 8.0% / 3.9% p95 |

Measured on the Mac host (Apple M1 Pro), Metal Release, before the mask slots
and the larger budget ([lighting efficiency](lighting-efficiency.md)): local
shadows cost about 4.9 ms of a 20.9 ms Bistro street-view frame at 1280x720,
3.7 ms of it per-pixel filtering; two more full-filter lights added about
3 ms of mask time.

No per-frame budget makes the walk metrics reach zero: in 9 of the 14
`bistro_snapshot` viewpoints more than 10 lights' ranges reach the view
frustum, and a budget must leave some of them unshadowed.

## Target hardware

These are the architectures the system is built and measured for. Phases name
which of them a capability may require.

| Tier | Architecture | API | Relevant capability | Host in this environment |
|---|---|---|---|---|
| Baseline | Apple M1 family | Metal 4 | Unified memory (M1 Pro 200 GB/s); Metal ray tracing runs on shader cores, without hardware acceleration | M1 Pro (Mac host) |
| Baseline | AMD RDNA2 | Vulkan 1.4 | 384 GB/s plus 96 MB Infinity Cache (RX 6700 XT); `VK_KHR_ray_query` with hardware intersection and shader-driven traversal; no opacity micromaps | RX 6700 XT (Windows host) |
| Baseline | NVIDIA Ampere | Vulkan 1.4 | Hardware ray traversal; `VK_KHR_ray_query`; no opacity micromaps | None recorded |
| High | Apple M4 | Metal 4 | Hardware-accelerated ray tracing (since M3) | None recorded |
| High | AMD RDNA3, RDNA4 | Vulkan 1.4 | Faster ray intersection per compute unit than RDNA2; traversal remains largely shader-driven | None recorded |
| High | NVIDIA Ada, Blackwell | Vulkan 1.4 | Hardware traversal, opacity micromaps (`VK_EXT_opacity_micromap`) for alpha-tested geometry, shader execution reordering | None recorded |

A gate for an architecture without a host stays open until one runs it; no
result transfers between architectures.

## Proposed architecture

Four layers with fixed contracts between them. Each phase replaces the
implementation behind a contract, never the contract a consumer reads.

1. **Visibility contract.** Deferred lighting reads per-light, per-pixel RGB
   visibility, strength applied, from `Shadow.LocalMask` slots; forward and
   transmission shading call the inline receiver. This is implemented and does
   not change. Any producer (cached maps, ray queries) must write the same
   values for the same pixel and light.
2. **Shadow-map cache.** Replaces per-frame selection for static lights. A
   persistent pool, keyed by light render id, face, face resolution and the
   static-world and publication generations that ADR-019's face history
   already checks. A face renders when its key is new or invalid and otherwise
   stays. Faces allocate from fixed 128² cells and receivers reach them through
   the per-face `atlas_rect` and `vkr_local_shadow_atlas_uv`; that per-face
   indirection is the seam through which a later phase allocates partial faces.
   The pool is shared by all frames in flight: static contents are written
   only when invalid and read by every frame, so they need no per-image copy.
   A face rewritten while an earlier frame may still sample it is ordered
   behind that frame by the graph's retained state on the one queue, so it
   needs neither a completion wait nor a second square.
3. **Moving lights and moving casters.** The implemented per-frame selection,
   crossfade and redraw become the budget for lights that move and for static
   lights whose faces a moving caster touches. Contribution ranking orders
   that budget.
4. **Priority.** Measured visible contribution and distance order cache fills
   and evictions, choose face resolution, and drive a continuous fade by
   camera distance for lights beyond the cache's reach. A visible static light
   inside the reach never loses its shadow; it can only render at a lower
   resolution.

## Phases

**Phase 1: indoor, baseline tier.** Every shadow-casting static light of the
loaded scene is resident in the cache. Moving casters redraw the faces they
overlap through layer 3. Face size is fixed per light by range, so camera
motion never redraws a face. The mask pass filters every in-range light; the
nearest lights take the nine-tap filter and the rest the single tap, and a
distance fade bounds how many lights a pixel filters. Contribution measurement
is ported to Metal so both backends rank and fade by the same input. All three
baseline architectures run the same shadow-map path; none requires ray tracing.

**Phase 2: open world, baseline tier.** The cache holds lights within a
residency radius of the camera instead of the whole scene. Fills are amortized
over frames, ordered by priority, and prefetched beyond the fade distance so a
light's shadow is resident before it becomes visible. Residency follows world
streaming cells.

**Phase 3: high tier, indoor and open world.** Two producers become available
behind the same contracts. On M4, RDNA3, RDNA4, Ada and Blackwell, ray-query
visibility replaces map redraws for moving lights and lights beyond the cache,
writing layer 1 directly; Ada and Blackwell use opacity micromaps for
alpha-tested casters. The cache may allocate partial faces at a resolution
chosen per region of the face, the virtual-shadow-map refinement, through the
layer 2 indirection.

A stochastic many-light integrator, which samples a few lights per pixel
instead of looping over all of them, is not part of this architecture: it
replaces deferred lighting's punctual loop rather than a shadow producer and
needs its own decision.

## Carried forward

| Implemented in ADR-019 | Role here |
|---|---|
| Atlas, 128² cells, `atlas_rect`, face history and its validity rules | Layer 2 pool, allocation and cache keys |
| `Shadow.LocalMask` overlap slots and inline fallback | Layer 1 contract |
| Selection, knapsack, crossfade, incumbent preference | Layer 3 budget |
| Measured contribution sample and its readback | Layer 4 input, ported to Metal |
| Reused-face culling skip, 64-bit view masks, bounded transmission pool | Unchanged |

## Not chosen for the baseline tier

- **Virtual shadow maps.** Page marking adds a per-pixel, per-light pass and
  page rendering needs fine-grained culling. On the M1 Pro, finer culling cost
  more than it saved ([meshlet cluster culling](meshlet-cluster-culling.md)),
  and the M1 cost that dominates, per-pixel filtering, is unchanged by VSM.
  Phase 3 can refine the cache toward it through the layer 2 seam.
- **Ray-traced visibility.** M1 has no ray-tracing hardware, RDNA2 traverses in
  shaders and has no opacity micromaps for Bistro's alpha-tested foliage, and
  the baseline must run one path on all three architectures.

## Decisions

- **Target.** 60 FPS (16.7 ms per frame) in the indoor cases with output at
  1280x720 and the High preset: rendered at full resolution on RDNA2 and
  Ampere, and at a 0.75 render scale (960x540, spatially upscaled) on the M1
  family. The M1 Pro missed 60 FPS at full resolution with every measured
  choice, 19.1 ms median with every lamp shadowed, and its 95th percentile
  was 18.2 ms even without local shadows; at 0.75 it holds 12.6 ms median and 16.7 ms at the 95th percentile
  (ADR-019). Everything else is measured against that target rather than fixed
  in advance.
- **Choices settled by measurement.** For each choice below, the
  highest-quality option that holds 60 FPS on every baseline host is taken;
  the recommendation applies when options tie:
  1. Face size. All 72 Bistro lights have 432 faces: 216 MiB at 512² and
     54 MiB at 256² in D16. The depth format is settled: the atlas is D16
     (ADR-019).
  2. Transmission in the cache. Refractive casters add transmission arrays per
     face; 432 faces at 128² add about 189 MiB. Alternatives are a smaller
     resident set for transmission or opaque glass for cached lights.
  3. Moving casters. Redraw the touched faces whole (implemented machinery,
     raster cost per touched face) or keep a separate dynamic layer sampled
     alongside the static one (a second lookup per tap). Recommendation:
     redraw.
- **Order.** Phase 1 starts on the M1 Pro, the slowest baseline host, then
  runs on the RX 6700 XT; Ampere gates wait for a host.

## Evidence required

Each gate runs on every baseline architecture with a host; an architecture
without a host stays open. Bistro cases only. Correctness gates use the
existing contribution-weighted metrics.

1. **No static pop-in.** Vulkan and Metal copies of the five indoor cameras and
   an indoor walk through them: with only static casters,
   `lighting.local_shadow.fading_ratio` is zero and
   `lighting.local_shadow.unshadowed_ratio` counts only lights beyond the fade
   distance. Final-color captures show no exterior lamp light inside the café.
2. **60 FPS.** Frame time on the indoor walk with every in-range light
   shadowed holds 16.7 ms at the target's render scale:
   `local_shadow_cache_bistro_metal_indoor_walk` (0.75) on the M1 Pro and
   `local_shadow_cache_bistro_vulkan_indoor_walk` (1.0) on the RX 6700 XT, with
   `Shadow.LocalMask` and `Lighting.Deferred` recorded per option; matched
   Release reports per `vkr-performance`.
3. **Memory.** Live cache bytes from `memory.gpu.*` and `rendergraph.*` rows
   for each measured option on each host.
4. **Fill.** Frames from scene load until every resident face is valid, and the
   frame-time spike while filling.
5. **Moving casters.** A scoped Bistro case with moving casters inside the café
   measuring redraw cost per frame against the 60 FPS target.
6. **Native validation.** One Debug Vulkan validation run and one Metal API
   validation run of the indoor walk, each with zero messages.

Phase 2 adds a Bistro streaming case, and phase 3 a ray-query parity check
against the shadow-map producer, when those phases start.
