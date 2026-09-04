---
name: vkr-task-workflow
description: Coordinate multi-step VKR investigation or implementation, bounded subagents, evidence and resumption. Skip task notes for focused questions and one-step edits.
---

# VKR task workflow

## Start

Answer a focused question or perform a one-step edit directly. For multi-step
work, inspect `.scratch/` for the same objective and create or reuse one note.
Use [references/task-notes.md](references/task-notes.md) for its compact format.
Record scope, branch/HEAD, existing changes, acceptance conditions and the next
step before implementation. A review authorizes findings; an implementation or
fix request authorizes the in-scope edits needed to deliver it.

Load `AGENTS.md` and only the domain skills needed for the current step. Search
for symbols or sections before reading large files. Keep logs and inventories
on disk; carry decisive facts and paths in context. Reuse results from the same
revision and configuration instead of repeating discovery after each step.

## Resolve architecture before dependent code

Apply the architecture decision rule in `AGENTS.md`. Investigate implementation
facts first. Ask immediately when an unsettled owner, contract, lifetime,
portability choice or budget changes the implementation. State the choice,
recommendation and cost. Continue independent work while waiting; do not choose
silently or park the question in a plan's unresolved-items section.

The user's request and earlier answers remain authorization. Do not ask again
for routine implementation choices within that scope. Ask before expanding the
outcome or mutating evidence/baselines outside the request. Record decisions
when answered and update the scope only when the user changes it.

## Delegate when it reduces work

Use a subagent for an independent bounded investigation, implementation slice,
or review that can proceed alongside useful local work. Work locally when the
next action depends on the answer, the task is small, or transferring context
would cost more than doing it. Do not spawn a duplicate investigation.

Give each child the objective, exact owned paths, relevant constraints, evidence
required and a stopping condition. Prefer a short handoff over a full-history
fork. Assign one writer per overlapping file and one owner for GPU execution;
serialize builds sharing an output tree and all measured/validation runs.
Children return findings, changed paths, exact checks and unresolved decisions.
The parent inspects their diff and evidence before integrating the result.

Select a model from the active tool's advertised models. This is a project
routing preference, not a permanent capability or pricing claim:

| Subtask | Selection |
| --- | --- |
| Bounded lookup, inventory, mechanical edits with deterministic checks | Small/fast model; `gpt-5.6-luna` when available |
| Multi-file implementation or independent review with established architecture | Balanced coding model; `gpt-5.6-terra` when available |
| Ownership, synchronization, shader math, architecture or unexplained performance | Parent's strongest available reasoning model; inherit when appropriate |

Use the lowest reasoning effort that covers the uncertainty. Escalate when the
child fails its evidence check or cannot resolve a concrete dependency, carrying
its findings forward. Respect a user-selected model. If overrides or named
models are unavailable, use the available default; do not install roles or
change persistent configuration to satisfy this table. Keep the root's model
unchanged unless the user requests it.

## Implement and verify

1. Select the smallest check that can falsify each changed invariant.
   `vkr-validation` owns test selection, `vkr-harness` owns renderer cases and
   artifacts, and `vkr-performance` owns speed claims. For prose or scripts use
   their structural or functional checks, not an unrelated renderer suite.
2. Verify referenced tools, flags and inputs exist. Reproduce a failure or
   retain a relevant baseline before changing the behavior being compared.
3. Make one coherent change. Inspect its diff, ownership and scope before an
   expensive run. Preserve existing edits, including staged changes.
4. Run the selected check and inspect its actual evidence. On failure, locate
   the cause, fix the smallest responsible change and rerun the affected check.
   Broaden only for a specific uncovered invariant. A unit test needs the
   justification required by `vkr-validation` before it is added or run.
5. Record decisive numbers, report digest, exact command, configuration and
   result. An unavailable check needs a concrete reason and uncovered claim.
   Ask immediately if missing evidence requires the user's machine or decision;
   continue work that does not depend on it.

Do not spend GPU runs or write documentation merely to satisfy a task-note
section. Tools that fail on supported inputs are defects to diagnose. Repair
in-scope tooling and validate the repaired path; distinguish missing host
capabilities from tool defects.

## Resume and finish

Update the note after a decision, meaningful result, scope change or handoff.
On resume, read it first, compare branch/HEAD and dirty paths, and check retained
artifacts still exist. Reconcile drift before editing. Keep accepted decisions;
supersede one only with new evidence or a user answer.

Finish when the acceptance conditions have evidence and no required decision
is silently deferred. Summarize the result, meaningful checks and limitations.
Update affected durable status/rationale through `vkr-docs`. Copy results out
of run trees, preserve required cross-machine payloads under `vkr-harness`,
and remove this task's disposable artifacts. Mark the note complete only when
its conditions are met; otherwise state the exact blocking dependency.
