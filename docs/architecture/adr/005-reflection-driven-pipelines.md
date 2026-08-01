---
status: implemented
updated: 2026-07-31
authority: adr
---
# ADR-005: SPIR-V-Reflected Resource Layouts with Declarative Shader Manifests

**Status:** Accepted

## Context

Vulkan pipeline construction must agree with compiled shaders on descriptor
sets/bindings/types/counts, push constants, and vertex inputs. Uniform staging
also has to agree on block member offsets and array/matrix layout. Duplicating
those facts manually creates drift risks.

The renderer additionally needs human-readable names and intent for frontend
uniform updates, render-pass selection, stage files, and vertex ABI choice.

## Decision

Use SPIR-V reflection for the compiled binary interface, and use `.shadercfg`
as a separate frontend manifest whose uniform declarations are checked against
that interface.

`vulkan_spirv_reflection.c` currently reflects and merges:

- descriptor sets and bindings, including descriptor type/count and block byte
  size;
- push constant ranges;
- vertex inputs, which are matched to explicit supported host ABI profiles;
- semantic frame/draw set and binding roles, with convention fallback;
- uniform blocks and members, including name, offset, size, scalar/vector/
  matrix shape, array count/stride, and matrix stride.

Pipeline descriptor-set layouts and push-constant layout are constructed from
that reflected data. Runtime descriptor writes are checked against reflected
set/binding/type/count. Host vertex, instance, and indirect command layouts are
pinned independently with `_Static_assert`.

`.shadercfg` declares shader identity, stages/files, render pass, instance use,
`vertex_abi`, cull and depth-test/write state, and named uniforms. The frontend
uses those declarations to construct the graphics pipeline and stage named
values. Depth state defaults to test/write enabled; passes such as the skybox
must opt out explicitly when their depth attachment is only cleared for a
later pass. Rasterization keeps CCW authored triangles front-facing; inside-cube
passes use front-face culling instead of reversing shared geometry winding. At
shader creation, each non-sampler frame/draw declaration must
match a member in the corresponding reflected block by name, offset, exact
size, scalar/vector/matrix shape, array count/stride, and matrix stride.
Matching `(set,binding)` blocks reflected from multiple stages must have
identical member layouts.

Slang lowers std140 matrices and arrays into generated wrapper structs. The
reflection layer recognizes those compiler-generated storage wrappers and
normalizes their nested traits before validation. Arbitrary user-authored
nested structs are not flattened into the manifest's flat declaration model.
A reflected member the manifest does not declare is permitted because the host
does not need to write every shader member.

## Consequences

**Positive**

- Descriptor layouts, push constants, and vertex inputs follow the compiled
  binary instead of hand-written binding tables.
- Descriptor writes fail early on binding/type/count mismatches.
- Explicit vertex ABI profiles and static assertions make host layout reviewable.
- Uniform ABI drift fails shader creation instead of silently staging bytes at
  the wrong offset.
- Manifests retain useful semantic names and pipeline intent. The validator
  exposed and fixed the shipped UI/viewport projection/view order mismatch.

**Negative / risks**

- Uniform declarations remain duplicated between manifest and shader and must
  pass validation whenever either side changes.
- The flat manifest cannot describe arbitrary nested user structs.
- Reflection/load complexity and the `spirv_reflect` dependency remain.
- `.shadercfg` is bespoke and long for large global blocks.

## Alternatives Considered

- **Manifest-only descriptor layout.** Restores binary/host drift. Rejected.
- **Reflection-only interface with generated names/code.** Removes duplication
  but requires a generation and frontend binding strategy not yet implemented.
- **Keep manual uniform layout with tests only.** Rejected because tests cannot
  cover every externally supplied shader or stage combination.

## Revisit When

- Prefer generating host metadata from reflection if the manifest no longer
  provides independent value.
- Add a manifest representation for user-authored nested structs if shaders
  require them.
- Extend reflection rules for descriptor indexing, storage-buffer light data,
  or shader hot reload.
