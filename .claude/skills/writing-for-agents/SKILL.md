---
name: writing-for-agents
description: Write or audit skills, AGENTS.md and CLAUDE.md as concrete instructions with scoped context and checkable outcomes.
---

# Writing for agents

Write only instructions that change a decision or prevent a demonstrated error.
For skill packaging, read [SKILL-MECHANICS.md](SKILL-MECHANICS.md).

## Specify behavior

For each rule, identify when it applies, what the agent does, and what proves
completion. Name the actor, owned data, lifetime or tool when ambiguity affects
execution. Define exceptions beside the rule. Use "must" for requirements and
"prefer" for defaults; include the criterion for departing from a default.

Replace "be thorough", "use judgment" or coined shorthand with the decision it
must produce. For example: "Before publishing an arena-backed view, prove the
arena cannot reset until its last consumer finishes." Do not replace several
constraints with a mood word such as "tight".

Separate implementation facts from desired behavior and measured results.
Verify symbols, paths, flags and supported inputs against code or live tool
help. Exercise a command when its behavior matters. A successful parser check
does not prove the workflow works.

## Minimize context

Keep standing priorities and routing in `AGENTS.md`. Put domain rules in one
owning skill. Put a long conditional procedure in a reference only when a
specific branch needs it. Every reference needs a reachable link and a read
condition. Load only the branch needed for the task.

Descriptions state capability and trigger, with exclusions only to prevent a
likely mismatch. Avoid repeating the body in the description, repeating an
index in several files, or copying tool help that is cheap to query. Keep
non-obvious environment constraints and failure handling next to the command.

## Decisions and scope

An instruction must preserve the user's current request and earlier answers.
Ask an unresolved architectural choice immediately, with a recommendation and
tradeoff; do not instruct agents to collect decisions for a final document.
Proceed with authorized independent work. A missing optional preference does
not create an approval requirement.

## Validate the edit

Read the diff for conflicting requirements, missing exceptions, dead references
and unnecessary work. Check skill frontmatter and mirror equality. For changed
scripts, run a meaningful success and failure case. For a workflow rewrite,
walk through realistic requests and check routing, questions, tool choice and
stopping behavior. Use an independent subagent review for complex changes when
it can add evidence without duplicating the edit. Fix demonstrated failures.
