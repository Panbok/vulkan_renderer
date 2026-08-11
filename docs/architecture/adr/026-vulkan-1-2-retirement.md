---
status: implemented
updated: 2026-08-11
authority: adr
---

# ADR-026: Vulkan 1.2 Retirement Sequencing and the Bindless-Only End State

## Status

**Accepted and implemented.** The owner explicitly authorized full V7 retirement
on 2026-08-11, superseding this ADR's earlier B2 observation-period condition.
All six deletion steps are complete: public `vulkan` selects the bindless
implementation; legacy selectors, shaders, manifests, build rules, frontend
subsystems, Vulkan 1.2 backend/adaptor, 87-operation interface, wrappers, and
render-pass/render-target graph residue are absent. Metal remains the macOS
default and bindless Vulkan is the Windows Vulkan implementation.

The authorization changed the risk decision; it did not manufacture missing
evidence. The complete CPU suite and macOS Metal gates pass after deletion, but
the resulting Windows executable cannot be run on the current macOS host and
MoltenVK still lacks `VK_EXT_descriptor_buffer`. Fresh native Windows runtime
and validation evidence therefore remains unavailable and is a named residual
risk, not an acceptance claim. Historical B1/B2 rationale below is retained to
explain the safer sequence that the owner consciously waived.

## Context

[ADR-021](021-metal-first-bindless-backend.md) defines three retirement gates.
Gate A makes Metal the default macOS renderer; Gate A2 removes the MoltenVK
path; Gate B authorizes removing `VkrRendererBackendInterface`, the Vulkan 1.2
implementation, and the descriptor-set-only shaders.

Two things about that structure need refining now that the bindless Vulkan
backend has a concrete design.

**First, Gate B is a single gate covering three different risks.** As written it
authorizes, off one evidence bundle, flipping the Windows default *and* deleting
the only renderer Windows has ever had. Those are not the same decision. A
default flip is reversible by a flag; a deletion is reversible only by revert,
and only until the surrounding code moves on. The difference between "Windows has
a renderer" and "Windows had a renderer" is worth a separate gate.

**Second, Gate A2's dependency is implied rather than named.** ADR-021 requires
that "the modern Vulkan path has native Windows validation coverage" before
MoltenVK is removed. With the staged plan in the
[bindless Vulkan backend specification](../bindless-vulkan-backend-spec.md) §11,
that phrase now has a concrete meaning, and leaving it vague invites removing the
project's only Vulkan observation path too early.

Two facts sharpen both points:

- The macOS development environment cannot run the bindless Vulkan backend:
  with SDK 1.4.357.0 the Apple M1 Pro reports Vulkan 1.4.334 and MoltenVK 1.4.1
  but still does not expose required `VK_EXT_descriptor_buffer`.
  Native Windows hardware now exercises the completed RX 6700 XT V3 offscreen
  and windowed walking renderer, V4 publisher, and V5 authored graph. The full
  graph is synchronization-validation clean offscreen and in a hidden native
  window, including asynchronous five-channel capture and analytical IBL. The
  required post-extraction Metal witnesses remain unavailable on Windows. The
  split development loop makes an observation period more valuable, not less.
- Gate A is currently **open**. The previously accepted Metal Bistro generation
  was invalidated by a cross-backend audit that found retained IBL, sampler,
  transparency, and presentation defects. Those are corrected, but replacement
  pixels have not been owner-reviewed. Nothing downstream should be described as
  imminent while the first gate is unresolved.
- Gate B1 is **complete**. A Windows Bistro visual audit found that the
  bindless path canceled its queued global-HDR bake and rejected the authored
  normalized local-probe cubemap. The implementation defects are corrected and
  the owner visually accepted the corrected Bistro/text result. Performance is
  not a B1 acceptance gate for this implementation-only stage. The Windows
  default therefore flips to bindless without deleting the legacy path.

The scale of what Gate B eventually deletes is worth stating plainly: the Vulkan
1.2 backend is 40 files and roughly 24,600 lines, of which `vulkan_backend.c`
alone is 10,400. On top of that sit the 87-entry backend interface, its frontend
wrappers, fifteen descriptor-set shader programs plus six shared shader headers
with their build rules and runtime manifests, and several frontend subsystems
that exist only to serve the descriptor-set model.

## Decision

### 1. Split Gate B into B1 and B2

| Gate | Required evidence | What it authorizes |
|---|---|---|
| **B1 — Windows default flip, no deletion** | Backend specification stage V6 complete: ADR-021's Gate-B functional checklist on Windows; native validation clean including synchronization validation; owner-accepted Windows Bistro/text output; pipeline-cache cold and warm behaviour; asset load and unload; and metrics parity. Performance optimization is outside this implementation-only selection gate | Bindless Vulkan becomes the default Windows renderer. Vulkan 1.2 becomes a non-default diagnostic path, still selectable and still building |
| **B2 — deletion** | Gate A2 is complete; the observation contract recorded before B1 has elapsed and its minimum successful sessions/harness repetitions have passed with no unresolved bindless-only correctness or lifetime regression on Windows; and equivalent evidence exists for any other platform claimed to use Vulkan | The deletion sequence in §3 |

The B2 observation contract starts with the 2026-08-11 Windows default flip. It
requires at least 14 calendar days and 20 successful default-path Windows
Bistro/text sessions or harness repetitions, including at least two focused
lifecycle/validation runs, with no unresolved bindless-only correctness or
lifetime regression. B2 cannot shorten that contract in hindsight. Gate A2 is
a hard dependency because steps 2 and 4 remove assets and code the MoltenVK path
still needs. Performance does not gate this implementation-only selection
decision.

Splitting the gate costs one flag and one observation period. It buys the
ability to fall back without a revert during the window when unknown defects are
most likely to surface — which, for a backend developed remotely against
driver-version-specific extension behaviour, is exactly when they will.

### 2. Name Gate A2's dependency

Gate A2's requirement that "the modern Vulkan path has native Windows validation
coverage" means **stage V5**: the full authored graph runs under synchronization
validation in both the windowed and offscreen targets. V3 is valuable walking
evidence, but it does not cover the pass, barrier, capture, and IBL behavior that
MoltenVK currently observes. Removing MoltenVK before V5 trades the project's
only local Vulkan observation path for an incomplete replacement.

Note the ordering consequence: Gate A2 deletes the macOS Vulkan platform
implementation, and Gate B2 deletes the Windows one. Between them the bindless
platform seam in the backend specification §10.1 has already superseded both, so
these are removals of superseded code rather than of load-bearing code.

### 3. Deletion order under B2

Each step is independently buildable and testable, ordered by blast radius
rather than by size.

**Step 1 — make legacy Vulkan unreachable.** Rebind the public `vulkan`
command-line/harness selector to the bindless implementation, remove any
temporary public bindless-Vulkan spelling, and remove public selection of the
legacy implementation. The legacy adaptor and tree may still compile for the
next deletion steps, but no application or harness path can initialize them.
Evidence: selector tests, the CPU suite, and fresh Metal plus bindless-Vulkan
snapshots. This step must precede asset deletion; otherwise a nominally
selectable renderer would remain while its shaders disappear.

**Step 2 — descriptor-set shaders and their build rules.** The fifteen Slang
sources and six shared headers under the legacy shader tree, the CMake function
that compiles them and its fifteen invocations, the fifteen SPIR-V outputs, the
`.shadercfg` manifests naming them, and the legacy-only reflection fixtures.
Evidence: no manifest or runtime reference remains, and bindless SPIR-V is the
only SPIR-V the application loads.

**Step 3 — legacy-only frontend subsystems.** The pipeline registry, shader
system, instance buffer, and indirect-draw modules, plus the descriptor-instance
-state fields carried by submeshes, UI text, world text, and world resources. The
capability boolean from
[ADR-025](025-selected-renderer-implementation-strategy.md) makes each of these a
mechanical deletion of a now-unreachable branch. The draw-batch module is
audited separately — it may be genuinely shared. Evidence: CPU suite green and
fresh snapshots from the two surviving backends: Metal and bindless Vulkan.

**Step 4 — the Vulkan 1.2 backend tree and its adaptor.** Audit every file rather
than deleting the directory wholesale:

- **reused and relocated:** the SPIR-V reflection wrapper required by the
  bindless ABI cross-check; the debug-messenger module; the Vulkan host
  `VkAllocationCallbacks` module, which is distinct from GPU device-memory
  placement; and the format/enum conversion utilities still used by the new
  backend;
- **already superseded at V3:** the Vulkan platform seam and its per-OS
  implementations, replaced by the parameterized bindless platform seam;
- **deleted outright:** the legacy dependency lowerer, replaced by the
  synchronization2 lowerer; render passes and framebuffers, replaced by dynamic
  rendering; descriptor-set/device-resource code with no bindless caller; and
  the remainder.

Remove the internal legacy implementation tag/adaptor and its source registration
in the same step, so no forwarding entry points at deleted code. Leave the now
unused `VkrRendererBackendInterface` definition and frontend wrappers until the
next step; removing them first would leave the legacy tree unable to satisfy its
own compile-time contract. Evidence: full suite, both surviving backends'
baselines, and a Release profile against the immediately preceding bindless
build confirming the removal itself caused no material regression.

**Step 5 — the backend interface and frontend wrappers.** Remove the now-unused
`VkrRendererBackendInterface` definition and every frontend wrapper that called
through its 87-entry table. This is where the frontend loses most of its bulk.
Evidence: compile, full suite, and both surviving backends' snapshots. In the
same change ADR-001 becomes `Superseded by ADR-025`, and the ADR/status indexes
are updated.

**Step 6 — graph legacy fields and migration residue.** Remove the graph's
render-pass handle, render-target array and count, render-target cache, and
render-pass hash map. Imported and final layout fields survive unless unified
image layouts have landed. Remove any remaining temporary selector aliases or
migration-only tests; `VKR_RENDERER_BACKEND_TYPE_VULKAN` now unambiguously means
the bindless implementation, while Metal remains unchanged. The unused future
DX12 enumerator is outside this ADR and is neither evidence for nor against the
retirement. Evidence: render-graph barrier tests, selector tests, and both
surviving backends' snapshots.

### 4. The end state is recorded, not discovered

After B2 the project has Metal on macOS and bindless Vulkan on Windows, and
**no portable diagnostic path at all**. A Windows machine without
`VK_EXT_descriptor_buffer` will have no renderer. There is no software fallback,
no descriptor-set path, and no MoltenVK.

That is the intended outcome of the owner's decision to go fully bindless, and
it is written here so it is a decision rather than a discovery.

### 5. Implementation record

V7 completed on 2026-08-11 in the prescribed dependency order. Shared graph
schedule, culling, condition, and barrier planning remain backend-neutral;
selected implementations own GPU resource realization, dynamic rendering,
commands, and timing. Imported and final image-state declarations survive.
The shared asset systems now publish generation-safe geometry, textures,
samplers, and materials through `VkrAssetPublisher` and no longer maintain a
parallel descriptor-instance or pipeline model.

The former observation contract would not have elapsed before 2026-08-25. The
owner's explicit instruction to complete V7 immediately is the authority for
proceeding before that date and replaces, rather than satisfies, that condition.

## Consequences

**Positive**

- Roughly 24,600 lines of backend, an 87-entry interface, its frontend wrappers,
  fifteen shader programs, and four frontend subsystems leave the tree. What
  remains is one renderer model rather than two.
- Every remaining renderer path is bindless, so a feature no longer has to be
  designed twice or gated on which model the platform uses.
- The graph sheds its render-pass and framebuffer vocabulary, which exists solely
  for the Vulkan 1.2 lowerer.
- Splitting the gate means the Windows default can be reverted by a flag during
  the period when unknown defects are most likely.
- Ordering by blast radius means each step's failure is diagnosable on its own,
  rather than as one large deletion that either builds or does not.

**Negative / risks**

- **No portable diagnostic renderer remains.** When a defect reproduces on
  Windows only, there is no second backend on that platform to bisect against.
  Cross-backend comparison becomes cross-platform comparison, which confounds
  driver, API, and platform differences at once.
- Devices without descriptor buffers lose support entirely and permanently.
- The observation period between B1 and B2 is a real calendar cost, during which
  three renderer paths still exist.
- Steps 3, 5, and 6 touch shared systems, the frontend, or the graph, so a defect
  there affects Metal, which is the shipping macOS renderer by then. Both
  surviving backends' snapshots are required evidence at those steps for exactly
  that reason.
- Relocating the SPIR-V reflection wrapper out of a tree being deleted is the
  step most likely to be done carelessly, because it looks like deletion and is
  actually a move.
- Once step 5 lands, ADR-001's decision is no longer in force and its status must
  move in the same change.

## Alternatives Considered

- **Keep ADR-021's single Gate B.** Rejected: it authorizes flipping the Windows
  default and deleting the only Windows renderer off one evidence bundle, with no
  window in which to fall back cheaply.
- **Delete the legacy backend as soon as the bindless one boots on Windows.**
  Rejected. It maximizes the period during which Windows has one unproven
  renderer and no fallback, on a backend the developer cannot run locally.
- **Keep Vulkan 1.2 indefinitely as a diagnostic path.** Genuinely attractive
  given that no portable diagnostic path survives B2, and it is what the "risks"
  above argue for. Rejected because it contradicts the owner's decision to go
  fully bindless, and because a diagnostic path that no one ships steadily decays
  — it would keep the 87-entry interface, the descriptor-set shaders, and the
  legacy frontend subsystems alive to serve a path with no users and no evidence
  budget. If the loss of a diagnostic path proves painful, the honest response is
  to reopen this decision explicitly rather than to have quietly kept a rotting
  path.
- **Delete the whole Vulkan tree in one commit.** Rejected: it merges a shader
  removal, a subsystem removal, an interface removal, a backend removal, and a
  graph change into one change whose failure is not diagnosable.
- **Delete legacy shaders before changing public selection.** Rejected because
  it leaves a renderer nominally selectable after removing assets it requires;
  every intermediate step must remain honest and executable.
- **Retire the enum value and rename the selector to `vulkan-bindless`
  permanently.** Rejected as the end state: after retirement there is one Vulkan
  backend, and making every harness case file and script carry a qualifier for a
  distinction that no longer exists is churn. The qualifier is useful only during
  migration.

## Revisit When

- Gate A remains open long enough that the whole sequence is stalled behind it; at
  that point the gates are reordered or the Metal baseline problem is escalated on
  its own terms.
- The bindless Vulkan backend cannot pass B1 on the available Windows hardware,
  which would mean the capability profile in
  [ADR-023](023-vulkan-1-4-bindless-capability-profile.md) needs revisiting
  before any retirement question does.
- The observation period surfaces a bindless-only regression class that recurs,
  which is evidence the period should lengthen rather than that the deletion
  should proceed.
- The loss of a portable diagnostic path proves materially painful in practice,
  at which point the "keep Vulkan 1.2 as a diagnostic path" alternative is
  reopened explicitly with that experience as its evidence.
- A supported platform appears that cannot meet the bindless capability profile,
  which would reintroduce the need for a second renderer model and invalidate the
  end state described in §4.
- Linux is claimed as a supported platform, which per ADR-021 requires the same
  evidence on Linux hardware before the backend is described as complete there.
