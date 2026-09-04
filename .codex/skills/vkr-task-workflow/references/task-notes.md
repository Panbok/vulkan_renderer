# Task notes

Use one `.scratch/<task-slug>.md` for multi-step work. Reuse a note only for the
same objective. `.scratch/` is local and untracked. Durable decisions belong in
the owning document; the note links to them rather than copying their policy.

Keep enough state to resume without repeating the investigation:

```markdown
# <task>
State: active | waiting | complete
Updated: YYYY-MM-DD

## Objective and baseline
User outcome, in-scope paths, branch/HEAD, pre-existing changes.
Observable acceptance conditions and planned evidence.

## Decisions
Accepted choices, their reasons and affected constraints.
Pending user question only after asking it immediately in conversation.

## Evidence
Exact meaningful command/tool action, configuration, exit/status,
decisive values, report digest, and artifact path if still needed.
For an unavailable check: reason and the claim it leaves unverified.

## Next action
One executable step, or the exact external dependency while waiting.
```

Omit empty sections and routine search history. Update after a material result,
decision, scope change or handoff, rather than appending a second summary.
Store bulk logs separately. Never store secrets. Copy decisive values out of
`build/` before cleanup; a path to deleted output is not evidence. On resume,
compare the workspace with the recorded baseline and reconcile drift before
editing. Mark complete only after the acceptance conditions are satisfied.
