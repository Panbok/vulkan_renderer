---
name: vkr-docs
description: Write or update docs, ADRs, implementation status, and documentation indexes.
---

# VKR documentation

Start discovery at `docs/README.md`. Read only the documents needed for the
current decision. `vkr-task-workflow` owns the single local note at
`.scratch/<task-slug>.md` when the work needs one.

## Authority and decisions

Code defines implemented behavior. `docs/architecture/renderer-architecture-spec.md`
defines recorded status. ADRs define decision rationale. A proposal or an unused
module does not establish shipped behavior; inspect production callers.

Ask the user as soon as an unresolved architecture choice affects the work.
Give the concrete tradeoff and a recommendation. Do not replace a needed answer
with a proposal document or defer questions to a final list.

Record durable decisions that constrain future work in an ADR. Local reversible
choices do not need one. Document ownership, allocator lifetime, synchronization,
and backend behavior where those facts are not clear from the code. Performance
claims require configuration, command, and measured results.

## Document metadata

Every Markdown file under `docs/`, except `README.md`, begins with:

```yaml
---
status: implemented | partial | proposed | superseded | investigation
updated: YYYY-MM-DD
authority: spec | adr | design | progress | investigation
---
```

Select one value per field. `authority` identifies the kind of claim the document
owns; it does not make a design claim override implementation evidence.

| Status | Required evidence or content |
|---|---|
| `implemented` | Current production code matches the description |
| `partial` | Implemented behavior and its exact remaining gap |
| `proposed` | Design without complete production integration |
| `superseded` | Link to its replacement |
| `investigation` | Diagnosis or postmortem with the conclusion at the top |

Non-Markdown artifacts use the owning index for status and purpose. Remove
editor and OS metadata from the docs tree.

## ADRs

Use the sections Status, Context, Decision, Consequences, Alternatives considered,
and Revisit when. Use these ADR status labels:

- `Accepted`: implemented decision in force.
- `Accepted (partial)`: decision in force with an explicitly described integration gap.
- `Proposed`: recommendation awaiting implementation or decision.
- `Superseded by ADR-NNN`: replaced decision.

Number new ADRs sequentially as `NNN-kebab-title.md`. Preserve existing numbers
and add the new row to `docs/architecture/adr/README.md` in the same change.

## Update and verify

1. Write intent and non-obvious invariants. Reference stable symbols and paths;
   avoid cached line numbers and prose that repeats headers or code.
2. When a proposal ships, update its metadata, the architecture spec feature
   table, the docs index, and the owning ADR status together.
3. When superseded, add the replacement link, set `status: superseded`, move the
   document to `docs/archive/`, and update links and indexes. Archive a completed
   investigation when the underlying issue is closed.
4. `docs/README.md` owns active documents and non-Markdown artifacts.
   `docs/archive/README.md` owns the archive recursively. A legacy collection may
   remain one indexed unit if its own historical index survives. Register each
   added or moved artifact with path, status, and a one-line purpose.
5. Check the changed documents and indexes with the command below. Inspect index
   coverage separately. The checker validates local target existence; it does
   not validate heading anchors, resolve reference labels, or contact websites.

```sh
python3 .codex/skills/vkr-docs/scripts/check_links.py \
  docs/README.md docs/architecture/adr/README.md docs/archive/README.md
```

Pass additional changed Markdown files, or `docs` to scan the full tree. The
command exits nonzero for missing targets. For a docs-only change, these checks
and a diff review are sufficient; renderer builds and unit tests add no evidence.
