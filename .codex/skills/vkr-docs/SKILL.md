---
name: vkr-docs
description: Conventions for writing and maintaining documentation and ADRs under docs/ so the tree stays accurate and agent-indexable. Use when adding a design document, writing or updating an ADR, marking a proposal as implemented, archiving a superseded document, or updating the docs index.
---

# VKR Documentation

`docs/` is committed agent and contributor context. Start discovery at
`docs/README.md`.

The tree is large. It stays useful only if every document states what it is and
the active and archived indexes cover their declared scope.

## Authority order

1. **Code** is the implementation authority. If a document disagrees with the
   code, the code is what runs.
2. `docs/architecture/renderer-architecture-spec.md` is the **status** authority
   — what is implemented, partial, or absent.
3. `docs/architecture/adr/` is the **rationale** authority — why a decision was
   made and what would cause it to be revisited.
4. Everything else is a design document, a progress log, or an investigation,
   and must say which.

A design document's existence is **not** evidence that its feature ships.

## Required front-matter

Every Markdown document under `docs/` except `README.md` files begins with:

```yaml
---
status: implemented | partial | proposed | superseded | investigation
updated: YYYY-MM-DD
authority: spec | adr | design | progress | investigation
---
```

| `status` | Meaning |
|---|---|
| `implemented` | Described behaviour matches current code |
| `partial` | Implemented with a stated gap — name the gap in the document |
| `proposed` | Design only; no production code, or no production call site |
| `superseded` | Historical; must name what replaced it |
| `investigation` | A diagnosis or postmortem, not a plan |

Derive `status` from evidence, not intent. A module that exists and has tests
but no production caller is `proposed`, not `implemented` — check for real call
sites before claiming otherwise.

`authority` records what kind of document this is, so an agent knows whether to
trust it over a neighbour: `spec` and `adr` outrank `design`, which outranks
`progress` and `investigation`.

Non-Markdown artifacts such as JSON schemas do not receive YAML front-matter.
List their status and purpose in the owning index. Remove editor/OS metadata
such as `.DS_Store`; it is not documentation.

## ADRs

Write an ADR when a decision **constrains future work**. Purely local or
trivially reversible decisions do not get one.

Format, in order: **Status → Context → Decision → Consequences → Alternatives
Considered → Revisit When**.

ADR status values are narrower than document status:

- **Accepted** — in force and implemented.
- **Accepted (partial)** — decided and implemented with a known unfinished
  integration; state the gap in the ADR itself.
- **Proposed** — recommended, not yet implemented.
- **Superseded by ADR-NNN** — no longer in force.

Number sequentially (`NNN-kebab-title.md`) and add a row to
`docs/architecture/adr/README.md` in the same change. Never renumber an existing
ADR; supersede it instead.

## Writing rules

- **Prefer stable symbol and path references over line numbers.** Line numbers
  are a snapshot and rot silently. `vkr_rg_execute()` in `vkr_rg_execute.c`
  survives a refactor; `vkr_rg_execute.c:412` does not.
- Record ownership, lifetime, synchronization, error propagation, and measured
  evidence for renderer decisions. Those are the facts that are expensive to
  re-derive from code.
- State what was measured and under which configuration. An unqualified
  performance number in a design document will be quoted back as fact.
- Name the gap. "Partial" without a stated boundary is worse than no status.
- Do not restate code. A document that paraphrases a header adds a second thing
  to maintain and no information.

## Keeping the tree accurate

**When a proposal ships**, update all three in the same change:

1. the document's `status` and `updated` front-matter;
2. `docs/architecture/renderer-architecture-spec.md` §4 feature table;
3. `docs/README.md` index row.

Also update the owning ADR's status if the decision moved from Proposed to
Accepted.

**When a document is superseded**, do not delete it. Set `status: superseded`,
add a one-line pointer to its replacement, move it under `docs/archive/`, and
update the index. History is cheap to keep and expensive to reconstruct.

**When you finish an investigation**, set `status: investigation`, record the
conclusion at the top rather than only in a trailing section, and archive it if
the underlying issue is closed.

## Index

`docs/README.md` lists every active document and non-Markdown artifact with its
path, status, and one-line purpose. `docs/archive/README.md` recursively owns
the archive listing; a preserved legacy collection may be one indexed unit when
its own historical index remains intact.

Adding, moving, or archiving a document means editing the owning index in the
same change. Verify links resolve:

```sh
cd docs && for l in $(grep -oE '\]\([^)#][^)]*\)' README.md | tr -d '()]'); do
  [ -e "$l" ] || echo "MISS $l"
done
```
