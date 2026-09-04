---
name: vkr-shaders
description: Specify and verify Metal/Vulkan shader semantics, efficiency, bindings, dispatch, reflection, and host ABI. Required for every production shader or shader-visible host-contract task, including one-backend requests.
---

# VKR shaders

## Locate the contract

Read the state definitions and affected rows in
`docs/adr/044-shader-cross-backend-contract.md`. Use its source inventory to
find the shared helper, Metal entry/root, Vulkan entry/root, host lowering, and
reflection. Inspect all affected counterparts before editing. A shared source
file alone does not prove both production builds execute it.

Keep portable math shared when Slang and MSL support the same semantics. Keep
address spaces, bindings, resource references, and native sampling operations
backend-owned. Update host fields, shader fields, assertions, reflection, and
any affected CPU oracle together when the ABI or algorithm changes.

## Compatibility and efficiency

Both backends must implement the portable feature's semantic contract, including
units, coordinate conventions, ranges, edge behavior, and output meaning. Native
root sizes may differ: Metal resource references and Vulkan bindless indices
have different representations. Pin each native layout independently.

Select data shape, workgroup size, resource access, and algorithm from the work
actually executed. Avoid repeated loads, redundant math, excess live values,
and unnecessary synchronization. Hoist draw/pass constants into their owning
producer when that reduces measured cost. Reduce bandwidth and register pressure
without weakening numerical precision or lifetime requirements.

Validate shader inputs and select optional feature variants before dispatch
where practical. Keep guards required by real work distribution, such as edge
threads from rounded dispatches. Remove a guard only after data or dispatch
shape proves its accesses valid. Do not replace divergent branches with more
ALU or memory traffic without measuring the affected GPU path.

Backend-specific intrinsics, dispatch shapes, or algorithms need a concrete
capability or measured performance reason and must preserve the portable
contract. A change to supported behavior, quality, or compatibility is an
architecture decision: ask the user immediately with the tradeoff and
recommendation. ADR-039/040 already authorize Metal scene scaling and MetalFX;
validate those modes separately from bilateral portable TAA parity.

## Verification loop

For a prose-only edit that changes no executable contract, check the statement
against the owning code and review the diff. No shader build or GPU run is needed.

Use `vkr-validation` to select native diagnostics and `vkr-harness` to execute the
smallest non-degenerate case that exposes the changed output or invariant:

1. Compile affected production shader paths and check host layouts plus compiled
   reflection where ABI or bindings changed.
2. Run the focused native diagnostic on each changed backend. Metal diagnostics
   run serially in the minimal case; follow the validation skill's MetalFX limits.
3. Run the same focused Release case on Metal and Vulkan with validation unset.
   Compare numeric payloads or pixels against the contract's existing tolerance.
   A screenshot alone does not prove parity. A CPU reference is useful only when
   it independently exposes a named arithmetic failure; add one only when that
   advantage is demonstrated.
4. For an efficiency claim, use matched Release measurements through
   `vkr-performance`. Compare work volume and quality as well as time. A different
   resolution or visual algorithm cannot establish equivalent-work speedup.
5. Iterate on failed assertions, diagnostics, and output differences. Record case,
   configuration, devices, digest, compared values, and tolerance in the ledger.

When native runs happen on separate machines, use the guarded `vkr-harness`
baseline publication and `snapshot --cross-backend` workflow. Preserve the first
portable witness before deleting its local run tree.

## Evidence state

An affected entry becomes **UNALIGNED** when either implementation differs or
required source, ABI, or native comparison evidence is missing. Update its row
and document-level state in the same change. Name the missing side and exact
gate when the gap appears; do not save a required decision for a closing list.

Mark **ALIGNED** only when the ledger's applicable bilateral gates pass.
Cross-compilation is not native execution. An authorized backend-specific mode
retains its documented exception and evidence state; do not claim algorithmic
parity for MetalFX. If the other backend is unavailable, complete the available
checks and report the implementation and parity evidence separately.
