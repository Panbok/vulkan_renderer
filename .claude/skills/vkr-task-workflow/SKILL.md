---
name: vkr-task-workflow
description: Routes substantive VKR work through resumable local task notes, explicit scope and authorization, bounded optional subagent coordination, and owner-selected evidence gates. Use for multi-step renderer research, diagnosis, design or refactor planning, performance investigation, feature implementation, parallel subagent coordination, or resuming after context compaction. Do not use for focused questions, brief explanations, status checks, or ordinary conversation.
---

# VKR Task Workflow

This skill owns task lifecycle, coordination, and handoff, not domain rules.
Every domain fact it needs already has an owner; route to that owner instead of
creating a second authority here.

## Classify the request

| Tier | Shape | What to do |
|---|---|---|
| Simple | A focused question, brief explanation, status check, or ordinary conversation | Answer directly and stop. No task note, subagent, or gate. |
| Research | Exploration, diagnosis, design, planning, or a performance investigation with no implementation edits | Open a task note. Stay read-only. |
| Implementation | Changes to code, shaders, build scripts, assets, docs, or agent context | Open a task note. Acceptance criteria before edits. |

Start at the simplest tier that fits and escalate the moment the work turns
multi-step — a question that grows into a three-file investigation is research,
and the note starts then rather than in hindsight.

Tier describes the work, not permission. A review or diagnosis remains
read-only except for its local task note unless the user also requested changes.
If the requested outcome changes, update the objective and acceptance criteria,
and obtain authorization before expanding implementation scope.

## Where the rules live

| You need | Authority |
|---|---|
| Ordering of concerns, conventions, build and test scripts | `AGENTS.md` |
| Named agent roles, model routing, cold spawn context, orchestration order | `vkr-agent-team` — load only when delegation is selected |
| Renderer runtime gates and test conventions | `vkr-validation` — "Choose the cheapest gate that completely covers the invariant" |
| What counts as a performance result, and the report shape | `vkr-performance` — "Evidence policy", "Reporting template" |
| Renderer architecture, hot-path rules, packet and graph contracts | `vkr-renderer-design` |
| Audit sequencing, refactor order, verification ladder | `vkr-renderer-design/WORKFLOW.md` |
| Allocator choice, ownership, lifetime, hash-table keys | `vkr-memory` |
| Whether a finding belongs in `docs/`, and its front-matter | `vkr-docs` — "Keeping the tree accurate" |
| A file-by-file compression map | `compress-codebase` |

Load only the owners relevant to the task. Cite an owner in the note or brief
instead of copying its policy. For docs, skills, assets, build metadata, and
other artifacts outside `vkr-validation`'s renderer matrix, use the owning
format/tool validation plus a focused structural check; do not invent a
renderer gate.

## Keep resumable task notes

Inspect `.scratch/` for the same task, then create or reuse one
`.scratch/<task-slug>.md` before deep work. Read
[references/task-notes.md](references/task-notes.md) for the schema. Before any
implementation edit, record the branch, HEAD, pre-existing dirty paths,
in-scope paths, and ownership of overlapping work. A dirty tree is context to
preserve, not a reason to erase or normalize user changes.

Refresh it after a material finding, a decision, a returned delegation, and a
gate run, whenever scope changes, and before any handoff or likely context
compaction. Keep its state, last-updated field, and single next step current.

`.scratch/` is local and untracked — resumable working state, not shared
authority. Anything that must survive outside this worktree leaves through
`vkr-docs` or the user-facing handoff. The note is never a shadow doc tree.

## Delegate only bounded, independent work

Stay the coordinator and integrator. Delegate only when the active agent
environment permits it and a concrete, independent, bounded branch can make
progress in parallel. Keep nesting at depth one. When delegation is selected,
load `vkr-agent-team`; it owns named roles, model routing, cold spawn context,
and orchestration order while this skill continues to own scope, reservations,
evidence, and completion.

- Assign an explicit scope, deliverable, read paths, write reservation, and
  must-not-write paths before launch. Keep one writer per overlapping region.
- Choose agents through `vkr-agent-team` when it is available. If that companion
  is unavailable, choose the cheapest capable read-only/search agent for
  deterministic inventory and a stronger general agent for ambiguous code-path
  analysis or review without inventing unsupported platform configuration.
- Require returns with agent identity and status, files inspected or changed,
  commands/evidence, conclusions, and remaining uncertainty.
- Verify the cited evidence for every material claim; independently re-derive
  high-risk or irreversible conclusions. Keep other unverified claims labeled
  and do not use them as the sole basis for a change.
- Do not spawn an agent to satisfy the pattern. When delegation costs more than
  it saves, stay single-threaded. Record a rejected fan-out only when it was a
  material scheduling decision.
- Before handoff or finish, wait for, stop, or explicitly transfer every live
  delegation. No subagent may remain able to edit after the task is reported
  complete.

## What to delegate in this repository

Safe to fan out, because these are read-only and disjoint:

- call-site inventory for a symbol across `lib/src`, `app/src`, `tests/src`;
- coverage inventory of `tests/src/` against a named invariant;
- extraction from `docs/` and the ADR index — which document claims what status;
- summarizing harness output under `build/_validation/`.

Treat these as high-collision write regions. Reserve each coupled change to one
writer unless the coordinator first proves and records disjoint ownership:

- a coupled frontend/backend seam change spanning
  `lib/src/renderer/renderer_frontend.c` and
  `lib/src/renderer/vulkan/vulkan_backend.c`;
- a coupled change across `assets/render_graphs/main.rendergraph.json`,
  `lib/src/renderer/vkr_rg_json.c`, and `lib/src/renderer/passes/`;
- edits to the shared suite registration file `tests/src/test_main.c`;
- overlapping edits to shared authorities under `AGENTS.md`, `docs/`, and
  `.codex/skills/`.

## Research tasks

State the question and the evidence that would answer it in the note before
searching. Split only genuinely independent branches; synthesis and every
decision stay with the coordinator.

Do not edit implementation files, and do not spend a Release or GPU gate unless
the research needs a reproduction or a measurement — `vkr-validation` says what
a renderer gate costs before you spend one. If research reveals a fix but the
user requested diagnosis only, report the proposed patch boundary without
implementing it.

Close with conclusions, supporting evidence, remaining uncertainty, and the next
step, in both the note and the reply to the user.

## Implementation tasks

1. Reconcile the workspace baseline and reserve the paths this task may edit.
2. Write observable acceptance criteria and the evidence you intend to produce
   into the note **before** editing. Criteria invented after a green run are not
   criteria.
3. Identify each affected authority and select its smallest complete gate. Use
   `vkr-validation` for renderer runtime changes, not as a universal validator.
4. When a hot path or any speed claim is in scope, `vkr-performance` owns what
   counts as evidence and the shape of the report.
5. When the work is a comprehensive audit or a multi-slice migration,
   `vkr-renderer-design/WORKFLOW.md` owns the sequencing.
6. After editing, inspect the focused diff for scope, accidental user-change
   overlap, and stale references before spending expensive gates.
7. Record every gate in the note as run, failed, or unavailable. An unavailable
   gate gets a concrete reason and the risk it leaves uncovered. Never report an
   unrun gate as passed.

## Use only gates that exist here

Before naming a gate, baseline, manifest, report, or artifact path, verify it in
this tree or its owning skill. Do not import another renderer's replay,
autotest, snapshot, benchmark-baseline, or quality-preset workflow by analogy.
Repository capabilities change; absence is recorded for the task, not frozen as
a permanent fact in this coordination skill.

When a required gate is absent, unavailable, or inapplicable, record the
concrete reason, the evidence used instead, and the residual risk. An
alternative is not equivalent unless the owning skill says it is.

## Preserve user state and evidence

Treat all pre-existing changes as user-owned. Do not overwrite, revert, stage,
or reformat unrelated paths. If an in-scope file already differs, inspect both
the index and worktree versions and layer the task's edit onto the existing
content.

A user request to implement or edit authorizes the corresponding in-scope
changes, including docs and agent context. A research, review, explanation, or
status request does not. Ask before an action that materially expands the
requested scope or mutates baselines, committed generated assets, or other
evidence the request did not clearly place in scope.

Before a clean reconfigure, asset repack, baseline update, or other action that
can invalidate comparison evidence, record or preserve the current evidence and
follow the owning skill's authorization rule.

## Resume an interrupted task

Read the note's state, workspace baseline, decisions, open questions, delegation
ledger, and next step before any further edit. Compare its branch, HEAD, dirty
paths, and write reservations with the current tree; explain and record drift
before adopting a new baseline. Confirm each recorded artifact still exists —
build output is regenerated and may not survive a reconfigure.

Reconcile live or interrupted delegations before spawning replacements. Do not
duplicate work merely because an agent return is missing from compacted context.

Do not silently reinterpret or drop a recorded decision. When a decision no
longer holds, supersede it in the note with the evidence that changed.

## Finish

Reconcile acceptance criteria, delegated results, implementation state, focused
diff, evidence, unresolved risk, and the next step in the note. Mark the note
complete only when its criteria are satisfied; otherwise leave an exact
continuation step and truthful state.

If the task moved a feature's status or the rationale behind a decision, that
belongs in `docs/` in the same change — `vkr-docs` owns the mechanics.

Leave the disk as you found it. Every run this task produced has either had its
result transcribed or, when another machine must consume its captures, been
published through the portable witness workflow. Delete the remaining run
trees and traces; `vkr-harness` owns what is safe to purge.

Give the user an outcome-first summary naming every gate that passed, failed, or
was not run.
