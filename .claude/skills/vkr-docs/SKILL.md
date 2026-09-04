---
name: vkr-docs
description: Write or update current architecture, vocabulary, ADRs, proposals, and the documentation index.
---

# VKR documentation

Start at `docs/INDEX.md`. Load only documents needed for the task.
`vkr-task-workflow` owns the single local note for multi-step work.

## Authority and structure

Code and production callers define implemented behavior. `docs/ARCHITECTURE.md`
records the current architecture, ownership, capabilities, and known gaps.
`docs/CONTEXT.md` defines project vocabulary; ADRs record decisions and rationale.
A declaration, unused module, or proposal does not prove production integration.

Keep `docs/` limited to:

- `CONTEXT.md`: terms used in code and documentation, with owner pointers.
- `INDEX.md`: the single inventory of all retained documents and their purposes.
- `ARCHITECTURE.md`: current system structure and behavior, without phase logs.
- `adr/NNN-title.md`: concise decisions in force, including explicit partial
  integration or deliberate rejection where it constrains future work.
- `proposals/title.md`: unimplemented work, its current baseline, open choices,
  and the evidence needed to accept it.

Do not create archives, progress documents, subsystem indexes, or duplicate
specifications. Remove obsolete documents after merging still-valid rationale
into its current owner; Git preserves the old text. Preserve ADR numbers and
never reuse a removed number. Use the next number above the repository's highest
allocated number, checking history when necessary.

## Audit and write

1. Inspect the owning code and production callers before assigning status.
   Separate implemented behavior, integration gaps, and unavailable native
   evidence. A source audit does not establish pixel parity or performance.
2. Keep one owner per claim. Move future-only portions into a proposal; merge
   repeated implemented constraints into the owning ADR. Do not retain old APIs,
   retired commands, estimates presented as results, or historical stage tables.
3. Record durable decisions with Status, Context, Decision, Consequences,
   Alternatives considered, and Revisit when. Add stable code links sufficient
   to check the contract. Keep operational procedures in their owning skills.
4. Ask immediately when an unsettled architecture choice changes implementation,
   following `AGENTS.md`. Existing user authorization remains sufficient for
   in-scope edits; recording current code is not a new architecture decision.
5. Update the architecture, glossary if terms changed, and `INDEX.md` together.
   When a proposal ships, merge the accepted decision into an ADR and remove the
   proposal or narrow it to its remaining unimplemented scope.

Every document except `INDEX.md` begins with:

```yaml
---
status: implemented | partial | proposed | declined
updated: YYYY-MM-DD
authority: architecture | context | adr | proposal
---
```

Choose one value per field. `implemented` describes current production code;
`partial` names its integration gap. An unrun platform check is an evidence limit,
not proof that the feature is absent. Proposals use `status: proposed` and
`authority: proposal`. ADR Status is Accepted, Accepted (partial), or Declined.
Keep measured claims only with their configuration, exact command, result, and
limits; never generalize an old backend's results to the current implementation.

## Verify

Run the local link checker, then inspect heading anchors, index coverage, code
symbols, inline paths, and the diff. The checker checks file targets, not anchors
or factual accuracy:

```sh
python3 .codex/skills/vkr-docs/scripts/check_links.py docs AGENTS.md .codex/skills
```

Search for references to every removed or moved path in tracked text, including
agent instructions and source-adjacent READMEs. Keep `.claude/skills/` byte-identical
to `.codex/skills/` and check with `diff -rq .codex/skills .claude/skills`.
Documentation-only changes need these structural and source checks; renderer
builds or CPU suites do not validate prose.
