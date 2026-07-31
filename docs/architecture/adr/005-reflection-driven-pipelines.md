---
status: partial
updated: 2026-07-31
authority: adr
---
# ADR-005: SPIR-V-Reflected Resource Layouts with Declarative Shader Manifests

**Status:** Accepted (partial)

## Context

Vulkan pipeline construction must agree with compiled shaders on descriptor
sets/bindings/types/counts, push constants, and vertex inputs. Uniform staging
also has to agree on block member offsets and array/matrix layout. Duplicating
those facts manually creates drift risks.

The renderer additionally needs human-readable names and intent for frontend
uniform updates, render-pass selection, stage files, and vertex ABI choice.

## Decision

Use SPIR-V reflection for the binary interface it currently exposes, and use
`.shadercfg` as a separate frontend manifest.

`vulkan_spirv_reflection.c` currently reflects and merges:

- descriptor sets and bindings, including descriptor type/count and block byte
  size;
- push constant ranges;
- vertex inputs, which are matched to explicit supported host ABI profiles;
- semantic frame/draw set and binding roles, with convention fallback.

Pipeline descriptor-set layouts and push-constant layout are constructed from
that reflected data. Runtime descriptor writes are checked against reflected
set/binding/type/count. Host vertex, instance, and indirect command layouts are
pinned independently with `_Static_assert`.

`.shadercfg` declares shader identity, stages/files, render pass, instance use,
`vertex_abi`, and named uniforms. The frontend uses those uniform declarations
to calculate CPU offsets and stage named values.

### Current limit: no uniform-member cross-validation

Although `VkrShaderReflection` has uniform block/member fields,
`vulkan_spirv_shader_reflection_create()` explicitly returns
`uniform_block_count = 0` and `uniform_blocks = NULL`. The implementation does
not compare manifest uniform names, member types, offsets, array strides, or
matrix layout against SPIR-V.

Consequently, the manifest and compiled uniform block can drift while
descriptor layout validation still succeeds. Reflected block byte size catches
some total-size errors but not member-level disagreement. The earlier claim
that uniform offsets were reflected and cross-validated was incorrect.

## Consequences

**Positive**

- Descriptor layouts, push constants, and vertex inputs follow the compiled
  binary instead of hand-written binding tables.
- Descriptor writes fail early on binding/type/count mismatches.
- Explicit vertex ABI profiles and static assertions make host layout reviewable.
- Manifests retain useful semantic names and pipeline intent.

**Negative / risks**

- Uniform layout still has two independently maintained sources without a
  member-level check; wrong rendering can result without a load error.
- Shader authors cannot safely add/reorder manifest uniforms without verifying
  compiled layout.
- Reflection/load complexity and the `spirv_reflect` dependency remain.
- `.shadercfg` is bespoke and long for large global blocks.

## Alternatives Considered

- **Manifest-only descriptor layout.** Restores binary/host drift. Rejected.
- **Reflection-only interface with generated names/code.** Removes duplication
  but requires a generation and frontend binding strategy not yet implemented.
- **Keep manual uniform layout with tests.** Acceptable as an interim safeguard,
  not a complete solution.

## Revisit When

- Reflect uniform block members and validate manifest type, offset, stride, and
  total size against every relevant stage.
- Prefer generating host metadata from reflection if the manifest no longer
  provides independent value.
- Extend reflection rules for descriptor indexing, storage-buffer light data,
  or shader hot reload.
