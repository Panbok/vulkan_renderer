---
name: vkr-shaders
description: Maintain Metal/Vulkan shader parity and the verified shader contract whenever work reads, reviews, diagnoses, or changes production shader code, shared shader math, shader entry points, resource bindings, dispatch shapes, shader-visible values, host-to-shader ABI structures, reflection, or pipeline layout. Use for every shader task, even when the requested change names only one backend.
---

# VKR Shaders

## Start from the parity ledger

Read `docs/rendering/shader-cross-backend-contract.md` completely before acting
on shader work. Locate the affected contract or create a narrowly scoped entry.
Code is implementation authority; the ledger records what has been verified.

Inspect all four relevant surfaces before editing:

1. shared shader helpers and constants;
2. the Metal production entry point and resource root;
3. the Vulkan production entry point and resource root; and
4. host lowering, CPU mirrors, static assertions, and runtime reflection.

Do this even when a bug is visible on only one backend. A shared file proves
source reuse, not that both compiled backends include and execute it.

## Preserve semantic and ABI parity

- Keep portable math in one shared helper when Slang and MSL semantics permit.
- Keep entry points, address spaces, bindings, native handles, and API-specific
  sampling in backend-owned files.
- Treat Metal's 64-bit resource references and Vulkan's 32-bit bindless indices
  as native representations. Require semantic field parity, not identical root
  sizes.
- Change the host structure, shader structure, field order, static assertions,
  reflection records, and CPU mirror together when an ABI or algorithm moves.
- Record types, byte sizes, alignments, offsets, constants, dispatch dimensions,
  and algorithms exactly. Do not infer an undocumented value from convention.

## Apply the state rule immediately

An entry is **ALIGNED** only after both production sources and both backend
evidence gates pass. If either side differs or has not been run:

1. mark the entry **UNALIGNED**;
2. change the document-level parity state to **UNALIGNED**;
3. name the missing backend, value, and exact validation gate; and
4. leave it unaligned until the missing evidence exists.

Never hide a one-sided finding by omitting it, retaining an old aligned label,
or treating a successful cross-compilation as runtime evidence. Unreviewed
shader areas stay explicitly outside the reviewed ledger.

## Require executable evidence

Use `vkr-validation` to choose the minimum correctness gates and `vkr-harness`
to run and interpret structured snapshots. For a parity claim, require:

- deterministic CPU reference coverage for portable arithmetic where practical;
- host size/offset assertions and compiled-shader reflection for ABI contracts;
- focused validation-layer runs for each changed native backend;
- the same focused Release case on Metal and Vulkan with validation disabled;
  and
- report digests plus real compared values and tolerances, not screenshots
  alone.

Record the case, configuration, backend/device, report digest, measured values,
and comparison in the shader contract. Transcribe the result, then delete
regenerable artifact trees created by the task.

## Finish the shader change

Before calling shader work complete, confirm that both backends implement the
same semantic contract, native ABI differences are documented, tests pin the
shared math, runtime reports exercise non-degenerate data, and the parity ledger
was updated in the same change. If only one backend could be validated, the
implementation may be handed off, but its contract remains **UNALIGNED**.
