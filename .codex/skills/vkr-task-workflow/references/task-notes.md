# Task note schema

## Path and lifecycle

One file per task at `.scratch/<task-slug>.md`, kebab-case slug, reused on
resume. Inspect existing notes before choosing a slug. Reuse only a note for the
same objective; never overwrite a semantically unrelated task or open a second
note for the same one. Supersede decisions in place instead of deleting history.

`.scratch/` is untracked and machine-local. It is working state that makes a
long task survivable across handoff and context compaction — it is not a
documentation tree and carries no authority. A finding that must outlive the
task moves into `docs/` under `vkr-docs`, which owns front-matter, ADR format,
and index registration.

Never record secrets, tokens, credentials, or unredacted commands containing
them.

## Required sections

| Section | Required content | Why |
| --- | --- | --- |
| State and checkpoint | `active`, `waiting`, or `complete`; created/updated dates; and one current next step. | A resumed agent can tell whether to continue and where. |
| Objective and scope | The user's actual request, tier, in-scope paths/outcomes, and explicit exclusions. | A compacted context reconstructs intent from here, not from the diff. |
| Workspace baseline | Repository root, branch, HEAD, pre-existing staged/unstaged/untracked paths, and in-scope paths. | Distinguishes user-owned work from task edits and exposes branch drift on resume. |
| Acceptance criteria | Observable conditions that make the task done, and the evidence that will show each. Implementation tasks fill this **before** editing. | Criteria written after a green run are a description of what happened, not a test of it. |
| Constraints and invariants | Applicable user constraints plus behavior, ownership, lifetime, concurrency, or hot-path invariants. Name the owning authority where one exists. | Preserves the conditions under which evidence is meaningful. |
| Decisions | Each decision with the evidence that forced it, newest last. Supersede rather than delete. | Prevents relitigating a settled choice after compaction. |
| Evidence ledger | See below. | Durable record of material actions and results, without shell-history noise. |
| Gates | Every selected gate as pending, passed, failed, unavailable, or inapplicable, with reason and residual risk. | Prevents an unrun gate from becoming an implied pass. |
| Open questions | What is unresolved and what would resolve it. | Distinguishes "unknown" from "unchecked". |
| Next step | The single next action, concrete enough to execute cold; `none` only when complete. | The first thing a resumed session executes. |

## Workspace baseline

Capture the baseline before implementation edits. A dirty worktree is expected:
record it and preserve it. Include staged, unstaged, and untracked paths, but do
not paste unrelated diffs into the note. Mark in-scope paths and paths that must
remain untouched.

On resume, compare the recorded branch, HEAD, dirty paths, and scope with
the current tree. Record a dated checkpoint when they differ and explain how
the drift was reconciled before replacing the working baseline.

## Evidence ledger

Record actions that change state, run a gate or measurement, reproduce a bug,
produce an artifact, or materially support a decision. Routine navigation and
search do not need a row unless their exact result is decisive. Record the
redacted command or tool action precisely enough to repeat, its exit/status,
the artifact, and the decisive result.

**Copy the decisive values out of `build/` into the note.** `build/` is
gitignored and regenerated; `vkr-validation` notes that the CPU suite
reconfigures with `--fresh`. Harness profile runs use unique artifact
directories, but they remain ignored build output rather than durable
repository evidence. A note that points at an artifact the next clean build can
destroy has recorded nothing. Paste the handful of numbers a conclusion rests
on; link or path-reference the bulk logs rather than copying them.

Keep gate status in the Gates section even when its command also appears in the
evidence ledger. Name an unavailable or inapplicable gate, give the concrete
reason, and state the risk left uncovered. The relevant validation or format
owner defines what each gate proves.

## Log policy

Link or path-reference large logs. Do not paste bulk output into the note — it
buries the decisions and defeats the purpose of a durable summary. Quote only
the lines a conclusion rests on, with the path they came from.

## Note skeleton

```markdown
# <task slug>

State: active
Created: YYYY-MM-DD
Updated: YYYY-MM-DD

## Objective and scope

<request>

- Tier: <research | implementation>
- In scope: <outcomes and paths>
- Out of scope: <explicit exclusions>

## Workspace baseline

- Repository: <absolute root>
- Branch: <branch or detached>
- HEAD: <commit>
- Existing changes: <staged / unstaged / untracked paths, or clean>
- In-scope paths: <paths this task may edit>

## Acceptance criteria

- [ ] <observable condition> — evidence: <instrument or gate>

## Constraints and invariants

- <invariant> (owner: <authority>)

## Decisions

- <decision> — because <evidence>

## Evidence ledger

| when | command / tool action | exit / status | artifact | decisive result |
| --- | --- | --- | --- | --- |

## Gates

| gate | pending / passed / failed / unavailable / inapplicable | reason and residual risk |
| --- | --- | --- |

## Open questions

- <question> — resolved by <what>

## Next step

<one concrete action>
```
