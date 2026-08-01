---
status: implemented
updated: 2026-07-31
authority: adr
---
# ADR-010: Archetype ECS as Authoritative Scene State

**Status:** Accepted

## Context

Scenes need stable entity identity, heterogeneous components, transform
hierarchy, JSON loading, editor/picking integration, and efficient component
queries. A node hierarchy alone makes orthogonal capabilities and bulk
component iteration awkward.

The renderer uses a packet boundary for one frame, but also has retained mesh,
material, text, and resource systems. The scene-to-renderer relationship must
therefore define which state is authoritative and how it is mirrored.

## Decision

Use the archetype ECS in `core/vkr_entity.*` as authoritative entity/component
storage and layer scene behavior in `renderer/systems/vkr_scene_system.*`.

The ECS provides component registration with size/alignment validation, entity
generation and world-ID checks, archetype migration/storage, queries, and
compiled query caching.

Entity components currently include:

| Component | Purpose |
|---|---|
| `SceneName` | Owned scene name |
| `SceneTransform` | TRS, cached local/world matrices, parent link, dirty flags |
| `SceneMeshRenderer` | Handle to a mesh-manager instance |
| `SceneVisibility` | Local visibility and optional parent inheritance |
| `SceneRenderId` | Stable entity-lifetime picking ID |
| `SceneText3D` | World-text ID and dimensions/dirty state |
| `SceneShape` | Procedural shape and generated mesh slot |
| `SceneDirectionalLight` | Directional light data |
| `ScenePointLight` | Point-light data |

Environment IBL and up to `VKR_SCENE_REFLECTION_PROBE_MAX` box probes are
scene-level arrays/state, not ECS components. Probes use center/extents,
blend distance, intensity controls, texture handles, and bake state.

Transform update and render synchronization are separate calls. The scene uses
compiled queries and a parent→children index/topological order for hierarchy
updates. Dirty render entities are mirrored by the scene render bridge into
`VkrMeshManager`, with a full-sync fallback.

Application draw-packet construction then scans mesh-manager slots/submeshes;
it does not currently extract draw arrays directly from ECS archetype queries.
This makes the mesh manager a retained render mirror and duplicates some scene
state, but keeps renderer asset/instance handles out of the ECS core. Extraction
classifies conservative submesh spheres against the camera and every shadow
cascade before final arrays are populated, then sorts/merges compatible opaque
draws.

Scene JSON loading has CPU async prepare/dependency states followed by
render-thread finalization. Scene-owned mesh instances, mesh slots, environment
textures, and probe textures are released on destroy/reload paths.

## Consequences

**Positive**

- Entity/component state is composable and queryable without a rigid object
  hierarchy.
- Layout and handle validation catch several misuse classes.
- Transform hierarchy logic and dirty propagation are explicit.
- Stable render IDs support picking/editor selection.
- A scene arena provides efficient bulk lifetime, with global-accounting
  reconciliation on destroy.

**Negative / risks**

- Structural component changes require archetype migration.
- Parent-before-child transform evaluation needs auxiliary ordering/index data.
- Component pointers can be invalidated by structural changes.
- Scene state is mirrored into mesh-manager state, so dirty/full synchronization
  must remain correct and cleanup symmetric.
- Packet construction scans the render mirror for capacity, count/visibility,
  and population, so it still does not realize the ECS locality benefit for
  draw extraction.
- Frustum culling is linear over every live submesh; there is no BVH/grid to
  avoid the classification scan on large scenes.
- Some scene operations directly use renderer systems, so the scene layer is
  not renderer-independent despite the update/sync split.

## Alternatives Considered

- **Object-oriented/node scene graph.** Natural hierarchy, weaker orthogonal
  composition and bulk iteration. Rejected.
- **Flat tagged-union object array.** Simple and local, but every new capability
  expands a central type. Rejected.
- **Sparse-set ECS.** Cheaper add/remove for some workloads, weaker archetype
  locality. Rejected for the current workload assumptions.
- **Third-party ECS.** Additional dependency and allocator integration; not
  selected.

## Revisit When

- Decide whether packet extraction should query ECS directly or keep the render
  mirror now that culling/merge work lives in extraction.
- Add a BVH/grid only after measuring collection/culling cost and scene churn.
- Validate repeated partial scene load/unload and renderer-handle symmetry.
- Reconsider storage when component churn or transform hierarchy becomes a
  measured bottleneck.
