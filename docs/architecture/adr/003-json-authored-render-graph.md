---
status: implemented
updated: 2026-07-31
authority: adr
---
# ADR-003: JSON-Authored Render Graph with Named Executors

**Status:** Accepted
**Depends on:** [ADR-002](002-render-graph.md)

## Context

With the render graph in place (ADR-002), frame topology was still built by C
code calling the builder API. That meant:

- Changing the frame structure required a recompile.
- The editor-enabled and editor-disabled frame layouts — which differ in almost
  every pass, because one renders directly to the swapchain and the other
  renders offscreen then composites — had to be expressed as branching in
  imperative setup code.
- The four CSM cascade passes were four near-identical blocks.

## Decision

Move graph topology into a JSON document
(`assets/render_graphs/main.rendergraph.json`) loaded by `vkr_rg_json.c`, and
bind passes to C code through a **name registry** rather than direct function
pointers.

**Executor registry.** A pass declares `"execute": "pass.world"`. During graph
realization the name is resolved through `VkrRgExecutorRegistry` to a
`VkrRgPassExecuteFn` + `user_data` registered by
`vkr_pass_world_register(&registry)`. Topology and implementation are decoupled.

**Conditional resources and passes.** `"condition": "editor_enabled"` or
`"!editor_enabled"` includes or excludes a resource or pass. The current graph
uses this to define `Skybox.Fullscreen`/`Skybox.Editor`,
`World.Fullscreen`/`World.Editor`, `UI.Fullscreen`/`UI.Editor`, and an
editor-only `Editor.Composite`. Conditions select variants; the variants are
not all active in one frame.

**Templated repetition.** A pass may declare
`"repeat": { "count_source": "shadow_cascade_count" }` with `${i}` interpolated
into the pass name and attachment `slice`. All four CSM cascade passes are one
declaration:

```json
{
  "name": "Shadow.Cascade.${i}",
  "repeat": { "count_source": "shadow_cascade_count" },
  "attachments": { "depth": {
    "image": "shadow_map",
    "slice": { "base_layer": "${i}", "layer_count": 1 }
  }},
  "execute": "pass.shadow.cascade"
}
```

**Frame-derived sizing.** `"extent": { "mode": "viewport" }` or
`{ "mode": "square", "size_source": "shadow_map_size" }`, and
`"layers_source": "shadow_cascade_count"`, resolve against
`VkrRenderGraphFrameInfo` rather than being hardcoded.

**Format aliases.** `"SWAPCHAIN"`, `"SWAPCHAIN_DEPTH"`, `"SHADOW_DEPTH"` bind to
the runtime-selected formats instead of pinning a `VkFormat`.

## Consequences

**Positive**

- Frame topology is editable without recompiling, which makes experimenting with
  pass ordering, adding a debug pass, or disabling a pass a text edit.
- The editor/non-editor split is expressed declaratively instead of as branching
  setup code.
- Cascade count is genuinely data-driven — changing it changes the number of
  passes.
- The executor registry makes passes independently registrable, which keeps
  `renderer_frontend.c` from accumulating knowledge of every pass's internals.
- The JSON source is parsed once, while conditions, repeats, dimensions, and
  pass/resource declarations are realized from that model for each submitted
  frame.

**Negative**

- **Errors move from compile time to runtime realization.** A typo in an
  executor name or a reference to an undeclared resource is a runtime failure,
  not a build failure. This trades a strong guarantee for flexibility and needs
  good diagnostics to be acceptable.
- The JSON schema is a second language to learn. A schema exists at
  `docs/rendering/render-graph-schema.json`, but the runtime/build does not make
  schema validation a mandatory authoring step.
- Some resource/name resolution uses linear search. Because the parsed model is
  realized per submitted frame, this is potentially a frame cost; it has not
  yet been shown material by profiling.
- Executor `user_data` lifetime is managed by the caller and not enforced by the
  registry.
- JSON pass type is not a guarantee about queue selection: all pass kinds still
  record on the graphics command buffer. `Picking.Request` is a declared
  graphics pass and `Picking.Readback` declares its transfer read, while the
  `compute`-typed IBL executor still orchestrates nested graphics work outside
  declared graph resources.

## Alternatives Considered

- **C builder API only.** Type-safe and compile-time-checked, but requires a
  recompile for topology changes and forces the editor/non-editor split into
  imperative branching. Rejected — flexibility was the point.
- **Code generation from JSON at build time.** Would recover compile-time
  checking while keeping declarative authoring, but loses runtime editability
  and adds a build step. A reasonable future option if load-time errors become
  painful.
- **Embedded scripting (Lua) for graph construction.** More expressive than
  JSON, but a much larger dependency and runtime for what is fundamentally a
  static declaration. Rejected.

## Revisit When

- Per-frame realization becomes measurable (cache realized topology or replace
  repeated linear lookups where profiling justifies it).
- Runtime graph errors prove to be a recurring source of lost debugging time —
  consider a schema validator or build-time codegen.
- Multiple graphs need to coexist or be hot-swapped at runtime.
