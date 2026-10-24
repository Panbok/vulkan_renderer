---
status: partial
updated: 2026-10-24
authority: adr
---

# ADR-133: Depot repositories, remotes and merge

## Status

Accepted (partial). A depot store is a repository with local branches,
remotes and tracked remote branches. Commits are local. Fetch, push and pull
exchange branches with named remotes, and branches that both sides moved are
merged per file, with scene units sent to Bakery's merge operations
(`merge_scene_unit`, `merge_project_document`, from the Windows track). The
editor's Source Control works on this model
([ADR-110](110-project-sharing-in-the-editor.md)). A scene whose edit
overlay both sides changed stays one conflict until merge per entity (D5 of
[the proposal](../proposals/depot-git-model.md)); self-contained projects
(D4) remain too.

## Context

Moving the Level Design Test between the Mac and Windows on 2026-10-23
needed two agent sessions, a hand-made copy between unrelated stores, and a
mirror that only fast-forwards. The owner asked for a model shaped like git
(owner decisions of 2026-10-24 in the proposal): a repository on each
machine, both machines as remotes of each other, local commits, push to any
branch, branch sync, and conflicts settled between remotes.

## Decision

### The store is the repository

[vkr_depot_repository.h](../../tools/depot/vkr_depot_repository.h) owns the
repository files in a store:

- `refs/<branch>`: the local branches, which the store also serves.
- `repository`: a 16-byte ID made on first use.
- `remotes`: one text line per remote, `name address key repository
  interval`. A remote name has 1 to 32 characters from `a-z`, `0-9`, `-`
  and `_`.
- `tracking/<remote>/<branch>`: each remote branch at the last fetch or
  push.

A branch moves only by compare-and-swap under the store's named process lock
(`vkr_platform_process_lock_acquire`, named from the case-folded absolute
store path). The server's branch moves go through the same swap, so a
working copy that shares its server's store (`clone --store`) commits into
the served branch safely. Remote and tracking writes take the same lock.

### Working copies adopt the model on open

The state file keeps its form: branch, base, index and the sparse flag, plus
the server it used to sync with, which the editor still reads.
Opening or saving a working copy gives its store what the older form kept
in the state alone (`workspace_adopt_repository` in
[vkr_depot_workspace.c](../../tools/depot/vkr_depot_workspace.c)):

- the state's server becomes the remote `origin`, unless it is the store's
  own server, which makes the working copy that repository itself;
- `refs/<branch>` is set to the base when missing;
- `tracking/origin/<branch>` is set to the base when missing.

Checkout and the older push (`vkr_depot_fetch_checkout`,
`vkr_depot_commit_push`) move `refs/<branch>`, and origin's tracking branch
when origin is the working copy's server.

### Commit, fetch, push and pull

[vkr_depot_sync.h](../../tools/depot/vkr_depot_sync.h):

- `vkr_depot_commit_local` commits on top of the base and moves the branch
  with no network. It refuses when the branch in the store is not at the
  base (another working copy of the store moved it).
- `vkr_depot_fetch` copies each remote branch (cooked side branches excluded)
  into the store and records its tracking branch. A sparse working copy
  takes trees only. The history behind each branch arrives as commits and
  trees without their files, so ancestry and logs work.
- `vkr_depot_push` refuses a push that is not a fast-forward of the remote's
  branch, uploads every commit the remote lacks oldest first (each against
  its first parent), then moves the branch by the server's compare-and-swap.
- `vkr_depot_pull` fetches the working copy's branch, then fast-forwards,
  reports AHEAD, or reports DIVERGED.
- Every connection to a version 2 service sends HELLO with the remote's
  repository ID, zero for the service's default, and the outbound-only flag
  (`$VKR_DEPOT_OUTBOUND_ONLY=1`, or a file `outbound-only` beside the
  identity key). A remote added by address learns its repository's ID from
  the first reply that carries one. A version 1 service gets no HELLO.

### Merge

[vkr_depot_merge.h](../../tools/depot/vkr_depot_merge.h):

- The merge base is the nearest common ancestor, found breadth first from
  the other side. Each path takes the side that changed it. A path both
  sides changed to different content is a conflict. A merge commit has both
  commits as parents.
- `.vkrdepotmerge` at the root names units: `unit <driver> <anchor pattern>
  [members]`, with the patterns of `.vkrdepotignore`. A unit goes to its
  driver when each side changed any of its paths. The driver gets each
  side's members in `base/`, `mine/` and `theirs/`, and answers merged,
  conflict or failed. A merged unit's other paths keep their own decision,
  and the driver's files then replace those it wrote, so both sides' new
  inventories stay beside a merged `scene.json`. A unit no driver merged is
  one conflict, listed by its anchor and settled whole.
- A driver name is a key into the caller's drivers. The Bakery driver runs
  `vkr_bakery project --request --result` for `scene` and `project`, and
  nothing for any other name, so a pulled file cannot run a command. Bakery
  is found through `--bakery`, `$VKR_BAKERY`, beside the program, or in the
  build tree's `tools/bakery`.
- A merge refuses local changes and sparse working copies. Under FAIL it
  changes nothing and lists the conflicts. `vkr_depot_pull_merge` merges a
  pull that diverged, fetching merge-base files the store lacks.

### Command

`vkr_depot commit` is local; `commit --push` commits and pushes, or puts the
branch, base and index back when the push is refused. `push`, `fetch`,
`pull`, `merge`, `remote`, `branch` and `switch` follow git's verbs
([vkr_depot_main.c](../../tools/depot/vkr_depot_main.c)).

## Consequences

- The Mac, whose endpoint security drops inbound traffic, adds the Windows
  service as a remote and starts every exchange. Windows sees it only while
  it connects.
- `mirror` still works between stores and only fast-forwards. Remotes
  replace it for working copies.
- A merge writes the result into the working directory and commits it. A
  failure while writing leaves part of the result, which `switch` back to
  the branch restores.
- History commits arrive without their files, so checking out an old commit
  needs its files fetched first.
- Ancestry walks are bounded at 4,096 commits (`VKR_DEPOT_HISTORY_MAX`).

## Alternatives considered

- **Keep the remote in the working copy's state:** two copies of one
  project could not share remotes, and the served repository would name
  itself.
- **A merge driver named by a command in `.vkrdepotmerge`:** a pulled file
  would run arbitrary programs.
- **Per-file merge of scene documents:** Mine or Theirs on `scene.json`
  drops the other side's imports without a warning, because each job adds an
  inventory under a new name.
- **Fetch every history commit's files:** git's model, but it multiplies the
  transfer of asset-heavy projects for a history that is rarely checked
  out.

## Evidence

Mac M1, 2026-10-24:

- `build_debug/tests/vulkan_renderer_tester --suite depot`: 26 tests pass
  (with the Windows service work), among them `test_depot_repository_refs`
  (200 moves from 4 threads through two spellings of a store path; it fails
  with the lock removed), `test_depot_repositories` (local commits, history
  push, fast-forward pull, divergence refused, pull with merge then push,
  branches, older working copies adopted, the served repository, HELLO ID
  learned and a wrong ID refused) and `test_depot_merge` (per-file merge,
  FAIL, Theirs and Mine, unit to driver, unit settled whole, unknown driver
  runs nothing, local changes refused; it fails when a merged unit drops the
  other side's inventory).
- `python3 tools/checks/check_depot_e2e.py build_debug/tools/depot/vkr_depot`
  passes.
- Between machines: `vkr_depot fetch --remote windows` from the Mac's
  `~/vkr-depot/work` against the Windows service (version 2, HELLO) and
  against the Mac's own version 1 server.
- The Release editor on an FPS Arena repository on loopback merged a
  commit made from another clone into a local commit (`Merge origin/main`,
  two parents), pushed it, and the other clone pulled both changes.
- `python3 tools/checks/check_bakery_merge.py --bakery
  build_release/tools/bakery/vkr_bakery` passes on the Mac. Two editors
  that each added an entity to one scene and committed: the second one's
  Commit & Sync stopped with the scene as one conflict, logged as "both
  sides edited the scene; their edits merge per entity in a later depot
  version", kept through status checks; Keep mine merged and Sync pushed.

Unverified: a scene merge in which one side changed only inventories
(Bakery's check covers it, a depot run does not); merge of a sparse working
copy.

## Revisit when

Repositories need history rewriting (reset, revert, stash), the 4,096-commit
walk bound is reached, or scene merges need conflicts per entity (D5).
