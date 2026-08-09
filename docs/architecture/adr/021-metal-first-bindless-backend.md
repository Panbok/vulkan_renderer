---
status: partial
updated: 2026-08-09
authority: adr
---

# ADR-021: Metal 4 First; Modern Vulkan for Windows and Linux

## Status

**Accepted (partial)** — Metal 4 is implemented as an application-selectable
macOS renderer. The Bistro bootstrap allocator race is fixed. A later
cross-backend review invalidated the accepted Metal baseline as current Gate A
evidence because it captured broken retained IBL, sampler, transparency, and
presentation behavior. Those defects are corrected, but Gate A pixel acceptance
is open pending owner review of a replacement. Vulkan 1.2/MoltenVK remains the
default. Modern Vulkan remains Stage 6
work for native Windows and any later claimed Linux target. Historical
milestone fixtures and focused backend validators have been retired. CPU tests
own deterministic subsystem/ABI checks, and backend-pinned harness snapshots
consume the domain-organized production shader library through the application
renderer.
Bounded command-slot reuse is externally visible as
`frame.command_slot_waits`, distinct from its upload-only subset.

## Context

[ADR-011](011-vulkan-1-2-baseline.md) serves Windows and macOS through one
Vulkan backend, with MoltenVK on macOS. That baseline deliberately does not use
descriptor indexing, buffer device addresses, dynamic rendering, synchronization2,
or timeline-based internal retirement.

The [bindless GPU-address design](../bindless-gpu-pointer-renderer-spec.md)
requires a first native implementation. The development machine is Apple M1 Pro
on macOS 26.5 with the macOS 26.5 SDK. Its headers expose Metal 4 buffer GPU
addresses, argument tables, resource IDs, transient command allocators,
stage-scoped barriers, queue events, placement heaps, and residency sets. The
standalone Stage 0 executable proved the initial capability/ABI path through
`supportsFamily:MTLGPUFamilyMetal4`, object-creation results, Metal API and
shader validation, and deterministic render/readback. Every production path and
additional record still needs equivalent focused evidence.

The former Metal-private Stage 1 walking renderer subsequently proved precreated
indexed vertex pulling, textured color, exact identifier output, bounded frame-
slot reuse, offscreen resize/readback, and real `CAMetalLayer` acquire/present
under Metal API and GPU validation. It remained outside the application renderer
and was retired after the production packet renderer subsumed that evidence.

The focused Stage 2 subsystem subsequently proved private placement-heap
buffers and textures, distinct upload/readback address-pair rings, residency,
generation invalidation, and submit-value native-object/range retirement over
64 validated GPU copy/readback cycles. Its deterministic CPU suite covers the
failure and stale-lifetime cases that Metal validation cannot prove. It also
remains outside the application renderer, so Gate A is unaffected.

The focused Stage 3 subsystem then proved explicit 64-bit texture-reference
rows, transactional immutable replacement while an earlier frame was
deterministically pending, bounded overflow reporting, and exact two-material
capture without per-material texture binding. Slang 2025.7.1 cannot currently
lower a texture-resource-bearing `StructuredBuffer` row and aborts internally,
so this slice uses a Slang vertex plus a bounded MSL fragment control. That is
valid Metal evidence, not general Slang material ABI acceptance, and Gate A is
still unaffected.

Stage 4 then added canonical access, execution-stage, and visibility records to
the shared graph barriers, with separate legacy Vulkan 1.2 and Metal 4
lowerers. Existing declarations retain their former conservative Vulkan stage
masks; an explicit compute-to-fragment one-layer case proves the narrower path.
The shipping Debug Vulkan application remained validation-clean, while 32
Metal split/intra-encoder dependency cases produced exact readback under Metal
API and GPU validation. The Metal lowerer still has no application graph
executor, so Gate A remains unaffected.

The focused Stage 5 packet renderer now executes the authored main graph for
offscreen and hidden-window targets, including placement-backed graph images,
real `CAMetalLayer` drawable acquisition/presentation, resize, feedback copies,
dependency encoding, and full retirement drain. Its asset-backed vertical uses
private placement mesh/cubemap resources, immutable explicit-ID material rows,
Slang-generated vertex pulling, and fourteen precreated render/compute pipelines.
Real pass bodies cover shadow, skybox, opaque, transmission, blend, picking,
tonemap, editor/UI lists, feedback copy, and placement-backed production-size
IBL convolution: 64-square irradiance faces, all nine mips of a 256-square GGX
prefilter cube, and a 128-square split-sum BRDF LUT. Ten evidence packets and
124 pass instances produced exact color, shadow depth, geometry-based picking,
IBL, directional-light, tangent-space normal, alpha-cutout, and per-pass
timestamp readbacks under Metal API/GPU
validation. The first bake uses 11 compute
dispatches; later packets reuse the generation-keyed result until the source
cubemap is replaced. The asset boundary now accepts the shared decoder's 2:1
RGBA16F payload, converts it to a mipmapped cube on GPU, and validates a focused
PBR consumer through the declared HDR scene-color capture.
A declared five-channel batch additionally returns aligned final/HDR color,
depth, shadow-layer, and picking-ID data with exact producer and subresource
metadata;
explicit mesh/material/cubemap unload/reload rejected stale handles and
collected old resources after their last submit. A separate cold-capture/warm-
lookup run proves the fourteen-pipeline Metal 4 archive under API validation. GPU
Validation rejects the same archive on this macOS stack, so shader validation
runs with lookup disabled and no archive speed claim is made. The fixed-capacity
Metal-private capture ring now implements monotonic request IDs, nonblocking
submission, completion polling, durable result ownership, explicit release,
and pending-request abandonment. The shared packet now carries directional and
IBL frame controls plus the bounded point-light table/grid. Immutable Metal
material records retain the production PBR scalars, and focused captures prove
IBL, IBL-disabled directional shading, a grid-selected punctual light from
GPU-addressed frame data, directional sampling from the graph-owned shadow
array, and IOR/thickness attenuation from the graph-declared pre-transmission
copy. Four immutable texture references additionally prove base, normal,
ORM, and emissive sampling plus alpha cutoff. Opt-in Metal 4 counter heaps return one named finite GPU
interval per executed pass and publish it into the bounded shared pass table.
Version-10 packets also carry up to 16 ready reflection probes, lowered once per
frame into native irradiance/prefilter references and box-influence records. A
focused capture proves normalized local influence, box-projected reflection, and
global-environment fallback. Asynchronous completion retains timestamp intervals
under their submit value for later application publication. Version-10 prepared
text records lower retained shaped geometry and logical atlas handles through
dedicated world/UI/picking pipelines; exact HDR/UI pixels and both text IDs pass
in the declared capture. A backend-neutral text harness fixture additionally
proves byte-identical final RGBA output across legacy Vulkan and Metal for
system-font UI, bitmap UI, MTSDF UI, and MTSDF world text. That comparison found
and fixed legacy Vulkan UI culling and alpha blending plus Metal UI clip-Y
lowering. Picking remains exact and deterministic within each backend; the
legacy Vulkan picking shader writes full glyph quads while Metal alpha-tests
glyph coverage, so those picking masks are not cross-backend color-parity
authority. Application selection, production asset publication, and harness
selection are now implemented. The graph/resource allocator race
found during asynchronous Bistro bootstrap is fixed by isolating Metal graph
ownership in render-graph memory and using frame-scoped schedule scratch.
Twelve fresh validation-enabled processes pass. The final backend-pinned
fourteen-view Bistro-plus-text Release run `20260807T091943.686Z-012428` then
passed every child and became accepted generation
`sha256:3db4f4d2294e5fdbc3618e64c4b2baf03bf66051dee0c4ff452e341d20cae51d`.
Fresh snapshot `20260807T092542.337Z-0144b6` and explicit compare
`20260807T092754.437Z-015047` pass all fourteen rows with zero pixels over
policy. This generation is retained as historical same-backend evidence only:
the later parity audit exposed the visual defects described above, and the
corrected renderer intentionally no longer compares equal to it.

Metal 4 is close to Aaltonen's proposal, not identical to it. Direct texture
IDs are opaque 64-bit `MTLResourceID` values rather than entries in an implicit,
process-wide global heap. Metal 4 also exposes bounded application-managed
`MTLTextureViewPool` slots whose IDs are contiguous from `baseResourceID`.
Argument tables are separate bounded root binding tables rather than a
substitute for either texture-reference representation.

The installed Slang 2025.7.1 Metal target accepts all 15 existing shader source
files. Stage 0 additionally proved one focused `ParameterBlock` root layout,
root-address slot mapping, direct and pool-backed texture IDs in a GPU record,
Metal library/pipeline creation, and exact pixels. General reflection/layout
validation is now implemented for every production shader record. Stage 5
requests Metal buffer-type reflection and validates each address-referenced root
and nested element against a durable host manifest. The CPU suite independently
checks manifest completeness and host `sizeof`, `_Alignof`, and `offsetof`
values; pipeline creation rejects a mismatch before encoding. This gate found
and corrected a tonemap-root padding error.

[ADR-020](020-bindless-backend-seam.md) keeps the Vulkan 1.2 implementation
intact while the new path is developed and defers any shared low-level seam
until working slices reveal it.

### The modern Vulkan premise is inverted

This ADR chose Metal first partly because "the most uncertain shader/resource
ABI is tested on the machine used for daily iteration." That premise does not
carry over to the Vulkan side of the decision.

The local Vulkan runtime is MoltenVK. With SDK 1.4.357.0 the Apple M1 Pro now
reports Vulkan 1.4.334 and MoltenVK 1.4.1, but still exposes no
`VK_EXT_descriptor_buffer`. [ADR-023](023-vulkan-1-4-bindless-capability-profile.md)
requires descriptor buffers as well as Vulkan 1.4, so **the bindless Vulkan
backend cannot run on the development machine at all** — not in a degraded
mode, not for a smoke test. Every runtime gate for it needs Windows hardware;
the available RX 6700 XT target supplies the completed V3 evidence and the
pre-integration V4 witness. The completed local V4 integration still requires a
fresh run of `tools/validate_v3_v4_windows.ps1` on that target.

This does not change the Metal-first decision, which remains correct for the
reasons recorded below. It does change what the later stage costs, and it makes
the observation periods in [ADR-026](026-vulkan-1-2-retirement.md) more valuable
rather than less. The
[bindless Vulkan backend specification](../bindless-vulkan-backend-spec.md) §1
enumerates the gates that remain executable on macOS; that list is the ceiling
on local iteration for the rest of this programme.

## Decision

Implement the bindless renderer first with Metal 4 on Apple Silicon, targeting
macOS 26 or later. Require a runtime Metal 4 family check and fail initialization
with an explicit capability report when it is unavailable. There is no silent
Metal 3 fallback and no Intel Mac target in this proposal.

Keep Slang as the intended shared authoring language. The Stage 0
root-address/resource-ID ABI spike accepts it for the initial path; bindless
shaders are new programs against that ABI, and each production record requires
generated/reflected host-layout checks. The existing 15 descriptor-set shaders
remain unchanged for the Vulkan 1.2 renderer.

Develop the later native Vulkan renderer for Windows first and add Linux only
with its separate platform/window/build work. Vulkan 1.4 is the version floor,
but acceptance depends on the immutable capability profile in the design spec,
including a proven bindless descriptor model; the core version alone is not
sufficient.

Once Metal becomes the accepted default macOS renderer, remove MoltenVK from the
default macOS build. Keep it available as a non-default diagnostic path until
its explicit retirement condition is met.

### Migration policy

This ADR is accepted in part. The Vulkan 1.2 path is in maintenance mode:
correctness,
security, compatibility, evidence maintenance, and changes required to keep
supported features coherent continue. New GPU-architecture features target the
bindless path unless a supported-platform requirement forces a Vulkan 1.2
implementation as well.

Maintenance mode is not deprecation. Vulkan 1.2 remains the only renderer for
Windows and the evidence base for the current status specification until the
gates below are passed.

### Retirement gates

| Gate | Required evidence | What it authorizes |
|---|---|---|
| **A — default Metal on macOS** | Metal completes every required graph/pass domain; windowed/offscreen, resize/drawable lifecycle, capture, exact ID/picking, readback, asset load/unload, memory/retirement metrics, and pipeline/archive cold/warm behavior; Metal API/shader validation is clean; an owner-reviewed Metal Bistro baseline is accepted under the design spec's policy and passes twice fresh | Metal becomes the default macOS renderer; MoltenVK becomes a non-default diagnostic build path |
| **A2 — retire MoltenVK** | The observation contract recorded before Gate A has elapsed and its minimum successful sessions/harness repetitions have passed with no unresolved Metal-only correctness/lifetime regression; the modern Vulkan path has completed **stage V5 of the [bindless Vulkan backend specification](../bindless-vulkan-backend-spec.md)** with the full authored graph validation-clean in windowed and offscreen targets; documentation and build support no longer claim MoltenVK as a supported renderer | Remove the MoltenVK rendering path and its macOS build wiring |
| **B — retire Vulkan 1.2** | **Split into B1 and B2 by [ADR-026](026-vulkan-1-2-retirement.md).** B1 requires the modern Vulkan backend to pass the same functional checklist, native validation, baseline, pipeline/shader, load/unload, metrics, and performance gates on Windows; B2 additionally requires the predeclared observation contract after B1 to pass with no unresolved bindless-only regression, plus equivalent evidence for every other platform claimed to use Vulkan | B1 flips the Windows default and demotes Vulkan 1.2 to a diagnostic path. B2 authorizes removing `VkrRendererBackendInterface`, the Vulkan 1.2 implementation, and descriptor-set-only shaders, in the order ADR-026 specifies |

Before Gate A flips the macOS default, its acceptance record defines the A2
observation contract as both a calendar duration and a minimum number of
successful real sessions or harness repetitions. A2 cannot define or shorten
that contract in hindsight.

Bistro color parity alone is insufficient for every gate. It does not cover
resource lifetime, window recreation, exact identifier data, capture/readback,
pipeline behavior, or native Vulkan correctness. macOS Metal evidence never
authorizes deleting the only Windows renderer.

Linux is not currently a supported platform. Gate B does not invent Linux
support, but any release that claims Linux must pass the same modern-Vulkan
evidence before the backend is described as Windows/Linux complete.

## Consequences

**Positive**

- The primary development platform gets a native renderer instead of a
  translation layer.
- Metal directly supports buffer addresses, resource IDs, placement heaps,
  transient command allocation, residency, and list-free barriers.
- The most uncertain shader/resource ABI is tested on the machine used for
  daily iteration.
- The current window layer and Objective-C build support already provide the
  platform entry point.
- Retirement is evidence-gated separately for macOS defaulting, MoltenVK
  deletion, and Windows Vulkan replacement.

**Negative / risks**

- macOS 26 is an aggressive deployment floor.
- Intel Macs are excluded by scope.
- Metal material rows may differ from modern Vulkan rows. Explicit 64-bit IDs
  are a conservative but less compact option; bounded texture-view-pool indices
  are a candidate whose shader ABI, growth, and lifetime behavior remain to be
  proven in Stage 3.
- Metal 4 APIs and Slang's relevant resource-ID ABI have little project-local
  precedent.
- There is no Vulkan validation-layer equivalent for Metal; Metal API/shader
  validation, deterministic readback, and lifetime tests must collectively own
  the evidence.
- Between Gate A and Gate B the project can carry two shipping implementations
  and a third in progress, its maximum maintenance cost.
- Removing MoltenVK reduces the ability to observe Vulkan regressions on the
  primary machine; native Windows automation must exist before A2.
- **The modern Vulkan backend has no macOS development loop at all.** Under
  [ADR-023](023-vulkan-1-4-bindless-capability-profile.md)'s required profile it
  cannot run on MoltenVK, so every runtime defect costs a remote Windows round
  trip and the project depends on Windows hardware being available for the whole
  Stage 6 programme. This is the sharpest cost of the Metal-first ordering: the
  platform with the mature bindless renderer is the one that cannot test the
  other.
- After Gate B2 there is no portable diagnostic renderer on any platform. Metal
  serves macOS, bindless Vulkan serves Windows, and a Windows machine lacking
  descriptor buffers has no renderer. ADR-026 records this as the intended end
  state.

## Alternatives Considered

- **Metal 3 as the baseline.** It has GPU addresses, resource IDs, heaps,
  residency, and older barrier APIs, and would lower the OS floor. Rejected for
  the first implementation because maintaining a second Metal recording/
  synchronization path would dilute the walking slice. Revisit if macOS 26 is
  not distributable; do not claim Metal 3 is technically incapable.
- **Metal 3 core plus opportunistic Metal 4.** Rejected because it creates two
  Metal validation matrices before one native renderer is proven.
- **Modern Vulkan first.** Rejected because the primary development platform
  would remain on MoltenVK, while the first native target and its platform
  iteration loop would be elsewhere.
- **Hand-written MSL as the permanent shader language.** Rejected pending the
  Stage 0 Slang spike. It remains the control/fallback used to distinguish a
  Metal API issue from a Slang code-generation issue.
- **Drop MoltenVK immediately at Gate A.** Rejected because a short diagnostic
  overlap is valuable until native Windows Vulkan automation replaces the lost
  Vulkan observation path.

## Revisit When

- A later Slang root-address/resource-ID record cannot be validated against its
  host layout.
- The Stage 3 compact texture-view-pool representation fails, or explicit
  64-bit fallback rows are materially too large or slow under a same-
  configuration measurement.
- Metal 4 runtime behavior differs from the SDK contract for argument-table
  snapshots, residency, barriers, or command-allocator reset.
- A distribution requirement cannot accept macOS 26 or Apple-Silicon-only
  support.
- Gate A feature parity proves smaller or larger than the checklist above; update
  the gate before changing the default.
- Native Windows automation is insufficient to authorize A2.
- The modern Vulkan target matrix shows that the required descriptor/capability
  profile is not broadly supportable.
- Carrying multiple GPU paths is no longer justified by delivery progress.
