---
name: vkr-shaders
description: Specify and verify Metal/Vulkan shader semantics, efficiency, bindings, dispatch, reflection, and host ABI. Required for every production shader or shader-visible host-contract task, including one-backend requests.
---

# VKR shaders

## Locate the contract

Read the evidence rules and affected rows in
`docs/adr/044-shader-cross-backend-contract.md`. Use its source inventory to
find the shared helper, each native entry and root that consumes it, host
lowering, and reflection. Metal runs the tiled pipeline and Vulkan the desktop
pipeline (ADR-087): a desktop-only domain has only Vulkan entries, a
tiled-only domain only Metal entries, and a shared kernel can have consumers in
both. Inspect every affected consumer before editing. A shared source file
alone does not prove that a production build executes it.

Keep portable math shared when Slang and MSL support the same semantics. Keep
address spaces, bindings, resource references, and native sampling operations
backend-owned. Update host fields, shader fields, assertions, reflection, and
any affected CPU oracle together when the ABI or algorithm changes.

## Compatibility and efficiency

Every consumer of a shared kernel or host record implements its semantic
contract, including units, coordinate conventions, ranges, edge behavior, and
output meaning. Native root sizes may differ: Metal resource references and
Vulkan bindless indices have different representations. Pin each native layout
independently.

Keep wire-ABI fields packed when the validated layout requires it, but expose
their algorithmic meaning in pass-local names or a narrow internal record. Name
resource roles and units instead of propagating numbered slots or vector lanes
through shader logic. Use pass-specific aliases; do not create a generic texture
framework merely to hide native bindings. Name policy-dependent tolerances and
quality limits, not conventional constants such as texel-center `0.5`.

Select data shape, workgroup size, resource access, and algorithm from the work
actually executed. Avoid repeated loads, redundant math, excess live values,
and unnecessary synchronization. Hoist draw/pass constants into their owning
producer when that reduces measured cost. Reduce bandwidth and register pressure
without weakening numerical precision or lifetime requirements.

Before calling expensive shader work efficient or proposing an optimization,
record its dispatch extent, iteration bounds, nominal texture/buffer accesses,
filtered samples, formats, temporary storage lifetime, and preserved quality
invariant. These source-level counts identify review targets; they are not GPU
timings or cache-miss measurements. Inspect compiled output and matched hardware
measurements before trading an intermediate pass, bandwidth, or storage for
repeated reconstruction.

Validate shader inputs and select optional feature variants before dispatch
where practical. Keep guards required by real work distribution, such as edge
threads from rounded dispatches. Remove a guard only after data or dispatch
shape proves its accesses valid. Do not replace divergent branches with more
ALU or memory traffic without measuring the affected GPU path.

Backend-specific intrinsics, dispatch shapes, or algorithms in a shared kernel
need a concrete capability or measured performance reason and must preserve its
contract. A change to supported behavior, quality, or compatibility is an
architecture decision: ask the user immediately with the tradeoff and
recommendation. ADR-087 already authorizes differences between the classes in
lighting, shadows, anti-aliasing, screen-space effects and quality presets
within its art-level contract; ADR-039 authorizes Metal scene scaling.

## Verification loop

For a prose-only edit that changes no executable contract, check the statement
against the owning code and review the diff. No shader build or GPU run is needed.

Use `vkr-validation` to select native diagnostics and `vkr-harness` to execute the
smallest non-degenerate case that exposes the changed output or invariant:

1. Compile affected production shader paths and check host layouts plus compiled
   reflection where ABI or bindings changed.
2. Run the focused native diagnostic on each changed backend. Metal diagnostics
   run serially in the minimal case.
3. Run a focused Release case with validation unset on each backend that runs
   the change. A case's `renderer.backend` selects its class: `metal` runs the
   tiled pipeline, `vulkan` the desktop pipeline, and an unpinned case the
   host's backend. Compare numeric payloads or pixels against the contract's
   existing tolerance and a same-backend reference, such as the run before the
   change or an accepted baseline. The two classes' images differ by design;
   compare them only for ADR-087's art-level contract. A screenshot alone does
   not prove a contract. A CPU reference is useful only when it independently
   exposes a named arithmetic failure; add one only when that advantage is
   demonstrated.
4. For an efficiency claim, use matched Release measurements through
   `vkr-performance`. Compare work volume and quality as well as time. A different
   resolution or visual algorithm cannot establish equivalent-work speedup.
5. Iterate on failed assertions, diagnostics, and output differences. Record case,
   configuration, devices, digest, compared values, and tolerance in the ledger.

Metal runs on the Mac and Vulkan on the Windows host. A change that both
backends consume needs native runs on both machines; if one is unavailable,
report that backend's native evidence as unavailable. `snapshot --cross-backend`
applies only to a class that both backends implement (`vkr-harness`).

## Evidence state

ADR-044 records, for each domain, the backends that run it and the native
evidence on each. Update the affected rows in the same change. Name the missing
backend and exact gate when a gap appears; do not save a required decision for
a closing list.

Mark **ALIGNED** only within a class that both backends implement, after the
ledger's gates pass on both; no class has two backends today (ADR-087).
Cross-compilation is not native execution. If a consuming backend is
unavailable, complete the available checks and report its missing native
evidence separately.
