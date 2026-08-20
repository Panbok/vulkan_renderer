---
name: vkr-agent-team
description: Routes bounded VKR renderer work to project-scoped researcher, renderer_debugger, coder, and reviewer agents while reserving Sol for the root coordinator, enforcing minimal-context spawns, and preserving write and GPU-evidence boundaries. Use together with vkr-task-workflow when the user requests subagents, delegation, or parallel agent work, or when a substantive VKR task has independent branches worth delegating. Do not use for single-agent work, simple questions, or generic Codex configuration outside this repository.
---

# VKR Agent Team

Use this skill only as the delegation companion to `vkr-task-workflow`.
`vkr-task-workflow` owns task state, authorization, acceptance criteria, write
reservations, evidence, and completion. This skill owns agent roles, models,
spawn context, and routing order. Domain skills still own renderer rules and
gates.

## Keep the root as lead

Keep planning, architecture, ambiguous debugging, task partitioning, delegated
evidence verification, integration, and final decisions with the root agent.
Children investigate, execute, or review bounded briefs; they do not replace the
root's judgment.

Reserve `gpt-5.6-sol` and its `gpt-5.6` alias exclusively for the root unless
the user explicitly supersedes this rule for one named task:

- Without a current task-local user override, never create or spawn a child with
  `gpt-5.6-sol`, `gpt-5.6`, or an inherited root model.
- For an ad hoc or generic child, pass `model: "gpt-5.6-luna"` or
  `model: "gpt-5.6-terra"` explicitly. Never allow it to inherit the root model.
- Use Luna by default. Use Terra only for the configured reviewer or when a
  bounded branch genuinely needs stronger reasoning.
- If Terra and root-owned synthesis cannot cover a branch that appears to need
  a Sol child, stop and ask for explicit, per-task user approval. Until approval,
  do not spawn it. If approved, record the single task-local override and exact
  spawn in the task note; never change project or personal defaults and never
  reuse that approval.

Project configuration sets `agents.default_subagent_model` to Luna as a safety
net for otherwise unspecified children. It deliberately does not set the root
model and does not override a model pinned by a named agent file.

Before every named-agent spawn, inspect its resolved TOML and require the model
to be exactly the role's Luna or Terra value below. A named agent's file takes
precedence over an explicit spawn model. If the file cannot be inspected or its
model is outside the allowlist, do not use that named agent; use an ad hoc child
with an explicit allowed model or keep the work at the root. Record the selected
role and effective model in the delegation ledger.

## Start every child cold

Pass `fork_turns: "none"` on every spawn. This is a spawn-time tool argument,
not a repository config key. Do not invent `fork_turns` TOML.

Because the child receives no parent conversation, give it a self-contained
brief containing only:

- objective and observable deliverable;
- task-note path and the decisions or acceptance criteria it needs;
- exact read paths, symbols, cases, reports, or diff range;
- the relevant project skills it must use;
- write reservation and must-not-write paths;
- effective permission state and whether the write boundary is enforced or
  behavioral;
- allowed commands, evidence requirements, and stop conditions;
- prohibition on spawning, delegating to, or coordinating child agents;
- required return shape: identity/status, files inspected or changed,
  commands/artifacts, conclusions, and remaining uncertainty.

Do not paste the full conversation, unrelated diffs, secrets, or speculative
root reasoning. Keep nesting at depth one; children never spawn children.

Treat `sandbox_mode` in a custom agent file as a default, not proof of the
effective child sandbox: live parent/session overrides are reapplied to spawned
children. Before launching `researcher` or `reviewer`, inspect the effective
permission state. When the active tool cannot enforce read-only execution,
record that the boundary is behavioral, snapshot the focused status/diff before
launch, require no writes in the brief, and compare status/diff immediately
after return. Keep the work at the root if that residual risk is unacceptable.

## Choose one role

Use the project agents under `.codex/agents/`. If the active spawn tool cannot
select a custom agent type, use the corresponding explicit model and include
that agent's role contract in the cold brief.

| Role | Model | Boundary | Use for |
|---|---|---|---|
| `researcher` | `gpt-5.6-luna` | No writes; read-only sandbox default | Code-path and call-site inventory, status/rationale extraction from the architecture spec and ADRs, test-coverage inventory, and official Vulkan/Metal/Slang documentation checks. |
| `renderer_debugger` | `gpt-5.6-luna` | Evidence writes only; never application code | Focused harness reproduction, renderer logs/reports/captures, deterministic repetitions, and separately authorized API-validation diagnostics. |
| `coder` | `gpt-5.6-luna` | One reserved write region | Implement an accepted root plan, format touched code, and run the assigned smallest complete gates. |
| `reviewer` | `gpt-5.6-terra` | No writes; read-only sandbox default | Actual-diff correctness, behavior regression, GPU lifetime/synchronization, branchless hot-path, performance-evidence, edge-case, security, test, and maintainability risks. |

Do not route architecture ownership or final decisions to any child. Do not use
`renderer_debugger` as a browser agent: VKR's evidence surfaces are the harness,
renderer reports, captures, logs, and validation layers.

## Route by task state

Use only the roles that reduce uncertainty or wall-clock time:

1. Have the root create or resume the task note and select applicable domain
   skills through `vkr-task-workflow`.
2. Use `researcher` for a bounded evidence inventory and
   `renderer_debugger` for a focused runtime reproduction when needed. These
   roles report evidence; the root diagnoses and decides.
3. Have the root write the plan, acceptance criteria, reservations, and gates.
4. Use one `coder` only after its write region is reserved. The coder implements
   the plan; it reports plan defects instead of redesigning the architecture.
5. Use `reviewer` only after an actual diff exists. Give it the diff range,
   invariants, and acceptance criteria; require concrete findings with file and
   line evidence, not style commentary.
6. Have `coder` fix only root-verified findings within the existing reservation.
7. Use `renderer_debugger` to verify a runtime fix only when its focused evidence
   is part of the selected gates.
8. Have the root inspect the diff and decisive evidence, reconcile the task note,
   stop or close every child, and make the final decision.

Skip any role whose work is sequential, trivial, already known, or more costly
to brief and verify than to perform at the root.

## Preserve renderer evidence

Load and obey the domain owner named in the delegation:

- `vkr-renderer-design` for renderer seams, packets, graph/pass work, hot paths,
  Vulkan lifetime, and architectural refactors;
- `vkr-memory` for allocation, ownership, and lifetime;
- `vkr-harness` for structured cases, profiles, reports, and repetitions;
- `vkr-validation` for CPU, backend, validation-layer, and pipeline-cache gates;
- `vkr-performance` for any timing run or speed claim;
- `vkr-docs` for documentation and ADR changes.

Never launch parallel GPU diagnostic children. Run exactly one Metal validation
renderer process at a time, keep shader validation to the smallest reproduction,
and keep diagnostic validation separate from Release baseline or performance
runs. A researcher or reviewer may inspect existing GPU artifacts in parallel;
only the process that creates GPU evidence must be serialized.

## Integrate returns

Update the task note's delegation and evidence ledgers after each return. Verify
cited files, commands, reports, and material claims before acting on them.
Independently re-derive GPU lifetime, synchronization, destructive, and other
high-risk conclusions. An agent return is evidence to assess, not a decision.

Before finishing, ensure no child remains running or able to edit, no overlapping
write reservation was used, every accepted finding is resolved or recorded as
risk, and every reported gate has its truthful status.
