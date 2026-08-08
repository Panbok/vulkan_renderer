---
status: proposed
updated: 2026-08-08
authority: adr
---

# ADR-026: Vulkan 1.2 Retirement Sequencing and the Bindless-Only End State

## Status

**Proposed** — nothing is deleted, and no gate below has been passed. This ADR
refines [ADR-021](021-metal-first-bindless-backend.md)'s Gate B; it does not
supersede that ADR, which remains the authority for Gates A and A2.

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

- The bindless Vulkan backend **cannot run on the development machine**. Local
  MoltenVK reports `apiVersion 1.2.296` without `VK_EXT_descriptor_buffer`. All
  of its evidence comes from remote Windows hardware, which makes an
  observation period more valuable, not less.
- Gate A is currently **open**. The previously accepted Metal Bistro generation
  was invalidated by a cross-backend audit that found retained IBL, sampler,
  transparency, and presentation defects. Those are corrected, but replacement
  pixels have not been owner-reviewed. Nothing downstream should be described as
  imminent while the first gate is unresolved.

The scale of what Gate B eventually deletes is worth stating plainly: the Vulkan
1.2 backend is 40 files and roughly 24,600 lines, of which `vulkan_backend.c`
alone is 10,400. On top of that sit the 86-entry backend interface, its frontend
wrappers, fifteen descriptor-set shader programs plus six shared shader headers
with their build rules and runtime manifests, and several frontend subsystems
that exist only to serve the descriptor-set model.

## Decision

### 1. Split Gate B into B1 and B2

| Gate | Required evidence | What it authorizes |
|---|---|---|
| **B1 — Windows default flip, no deletion** | Backend specification stage V6 complete: ADR-021's Gate-B functional checklist on Windows; native validation clean including synchronization validation; an owner-accepted Windows baseline bootstrapped under the umbrella specification's seven-step policy and passing twice fresh; pipeline-cache cold and warm behaviour; asset load and unload; metrics parity; and an authoritative Release performance profile against legacy Windows Vulkan 1.2 on identical cases | Bindless Vulkan becomes the default Windows renderer. Vulkan 1.2 becomes a non-default diagnostic path, still selectable and still building |
| **B2 — deletion** | A defined observation period after B1 with no unresolved bindless-only correctness or lifetime regression on Windows, plus equivalent evidence for any other platform claimed to use Vulkan | The deletion sequence in §3 |

Splitting the gate costs one flag and one observation period. It buys the
ability to fall back without a revert during the window when unknown defects are
most likely to surface — which, for a backend developed remotely against
driver-version-specific extension behaviour, is exactly when they will.

### 2. Name Gate A2's dependency

Gate A2's requirement that "the modern Vulkan path has native Windows validation
coverage" means **at minimum stage V3** — a walking bindless renderer producing
validation-clean windowed and offscreen frames on Windows — and realistically
**stage V5**, where the full authored graph runs under synchronization
validation. Removing MoltenVK before that trades the project's only local Vulkan
observation path for nothing.

Note the ordering consequence: Gate A2 deletes the macOS Vulkan platform
implementation, and Gate B2 deletes the Windows one. Between them the bindless
platform seam in the backend specification §10.1 has already superseded both, so
these are removals of superseded code rather than of load-bearing code.

### 3. Deletion order under B2

Each step is independently buildable and testable, ordered by blast radius
rather than by size.

**Step 1 — descriptor-set shaders and their build rules.** The fifteen Slang
sources and six shared headers under the legacy shader tree, the CMake function
that compiles them and its fifteen invocations, the fifteen SPIR-V outputs, and
the `.shadercfg` manifests naming them. Evidence: no manifest reference remains,
and bindless SPIR-V is the only SPIR-V the application loads.

This is the largest removal with the smallest blast radius, and it goes first
for a second reason: it proves nothing in the bindless path depends on the
legacy assets, which is cheaper to discover here than after the backend is gone.

**Step 2 — legacy-only frontend subsystems.** The pipeline registry, shader
system, instance buffer, and indirect-draw modules, plus the descriptor-instance
-state fields carried by submeshes, UI text, world text, and world resources. The
capability boolean from
[ADR-025](025-selected-renderer-implementation-strategy.md) makes each of these a
mechanical deletion of the branch that is now unreachable. The draw-batch module
is audited separately — it may be genuinely shared. Evidence: CPU suite green;
both backends' snapshots byte-identical.

**Step 3 — the backend interface and its adaptor.** `VkrRendererBackendInterface`,
the legacy implementation adaptor from ADR-025, and the frontend wrappers that
call through the table. This is where the frontend loses most of its bulk.
Evidence: compile, full suite, both backends' snapshots.

**Step 4 — the Vulkan 1.2 backend tree.** Audited per file rather than deleted
wholesale:

- **reused and relocated:** the SPIR-V reflection wrapper, which the bindless ABI
  cross-check depends on; the debug-messenger module; parts of the format and
  enum conversion utilities;
- **already superseded at V3:** the Vulkan platform seam and its per-OS
  implementations, replaced by the parameterized bindless platform seam;
- **deleted outright:** the legacy dependency lowerer, replaced by the
  synchronization2 lowerer; render passes and framebuffers, replaced by dynamic
  rendering; the host allocation-callback module, replaced by the shared core;
  and the remainder.

Evidence: full suite; both backends' baselines; a Release profile confirming the
removal itself caused no regression.

**Step 5 — graph legacy fields.** The render-pass handle, render-target array and
count, render-target cache, and render-pass hash map on the graph. The imported
and final layout fields survive unless unified image layouts have landed.
Evidence: the render-graph barrier test plus both backends' snapshots.

**Step 6 — enum and selector meaning, last.** Both backend-type enumerators
survive; the Vulkan one now means the bindless backend. The harness string
mapping and its test are updated here. Doing this last means pinned harness case
files and the command-line selector keep working through every preceding step.

### 4. The end state is recorded, not discovered

After B2 the project has Metal on macOS and bindless Vulkan on Windows, and
**no portable diagnostic path at all**. A Windows machine without
`VK_EXT_descriptor_buffer` will have no renderer. There is no software fallback,
no descriptor-set path, and no MoltenVK.

That is the intended outcome of the owner's decision to go fully bindless, and
it is written here so it is a decision rather than a discovery.

## Consequences

**Positive**

- Roughly 24,600 lines of backend, an 86-entry interface, its frontend wrappers,
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
- Steps 2 through 5 touch shared code — systems, the frontend, and the graph —
  so a defect there affects Metal, which is the shipping macOS renderer by then.
  Both backends' snapshots are required evidence at every one of those steps for
  exactly that reason.
- Relocating the SPIR-V reflection wrapper out of a tree being deleted is the
  step most likely to be done carelessly, because it looks like deletion and is
  actually a move.
- Once step 3 lands, ADR-001's decision is no longer in force and its status must
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
  — it would keep the 86-entry interface, the descriptor-set shaders, and the
  legacy frontend subsystems alive to serve a path with no users and no evidence
  budget. If the loss of a diagnostic path proves painful, the honest response is
  to reopen this decision explicitly rather than to have quietly kept a rotting
  path.
- **Delete the whole Vulkan tree in one commit.** Rejected: it merges a shader
  removal, a subsystem removal, an interface removal, a backend removal, and a
  graph change into one change whose failure is not diagnosable.
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
