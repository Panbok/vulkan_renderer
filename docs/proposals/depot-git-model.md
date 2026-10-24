---
status: proposed
updated: 2026-10-24
authority: proposal
---

# Depot repositories, remotes and merge

Every project becomes a depot repository with a local store. Commits are
local; named remotes take fetch, push and pull. Branches that both machines
moved are merged. One background service per machine serves the
repositories. The editor gets a Source Control panel, and the two kinds of
project (shared and not shared) go away. This replaces the workflow in
[ADR-110](../adr/110-project-sharing-in-the-editor.md) and the `mirror`
command of [ADR-105](../adr/105-network-transport-and-asset-depot.md#asset-depot).
It keeps the object format, transport, access keys, locks and cooked side
branches
([ADR-123](../adr/123-asset-streaming-from-a-depot.md#project-sharing)).

## Owner decisions, 2026-10-24

The owner asked for a workflow shaped like git: a repository on each
machine, both machines as remotes of each other, local commits, push to any
branch, branch sync, and conflicts settled between remotes. The owner chose
the recommended option for each open choice:

1. **Projects live in their own folders**, outside the git repository. The
   repository's Testbed project becomes a template that a new project is
   created from.
2. **Merge per file first.** Merge per scene entity is a later phase.
3. **The server is a separate service per machine.** The editor shows its
   state and never hosts it.

## Why: the Level Design Test transfer, 2026-10-23 to 2026-10-24

Moving one scene from the Mac to Windows took two agent sessions and one
night. Each failure points at a part of the model:

| What happened | Cause in today's model |
| --- | --- |
| Windows could not see the Mac's shared project | Only the sharing editor serves, and the Mac drops inbound traffic. Nothing tells the user that a machine cannot serve. |
| The Mac's share (`~/VKR Projects/Testbed 2`) had no history in common with the Windows depot copy | Share makes a new store each time. Two copies of one project can have separate histories. |
| The Windows server had been stopped for 5 days without notice | The server is the editor, a `vkr_depot serve` process or a launchd job, and no surface shows whether any of them runs. |
| The transfer needed a hand-made file copy into another working copy | No editor action moves changes between two stores. `mirror` only fast-forwards and stops at diverged branches. |
| 53 materials, a graph and 43 textures arrived missing | The editor falls back to the repository's `assets/` (ADR-077 overlay), so a project looks complete on the machine that made it. Bakery's `share_project` did not follow surface themes either. |
| Stale material records of rebuilt models blocked the cook on Windows | Scene folders keep every old build and overlay, and the depot sends them all. |

## Current baseline

The parts below already exist and stay:

- Content-addressed objects: chunks, blob manifests, trees, and commits with
  up to `VKR_DEPOT_PARENTS_MAX` parents
  ([vkr_depot_object.h](../../tools/depot/vkr_depot_object.h)). The format
  can already store a merge commit.
- A store that tolerates concurrent writers, and branch moves by
  compare-and-swap that check the objects and locks
  ([vkr_depot_store.h](../../tools/depot/vkr_depot_store.h)).
- Transport, identity keys, access roles and handshake refusal with
  `user@computer` names
  ([vkr_depot_server.h](../../tools/depot/vkr_depot_server.h)).
- Working copies with ignore rules, snapshot, checkout that keeps local
  changes, and per-path conflicts settled by FAIL, TAKE_THEIRS or KEEP_MINE
  ([vkr_depot_workspace.h](../../tools/depot/vkr_depot_workspace.h)).
- Cooked side branches (`<branch>.cooked.astc`, `<branch>.cooked.bc`) and
  sparse working copies (ADR-123).

The parts below change:

- `commit` snapshots, uploads and moves the server's branch in one step. A
  working copy has one remote, in its state file.
- `mirror` keeps two stores equal and only fast-forwards.
- The editor's Share copies the project into `~/VKR Projects/<name>` and
  serves it while that editor runs. Get clones from the other editor. There
  are two kinds of project, and the project hub lists them apart.

## Model

### Repository

A project folder with `.vkrdepot/` is a repository. `.vkrdepot/` holds the
local store, the branches, the remotes and the working-copy index. A new
project is a repository from the start. `vkr_depot init` or the editor's
Initialize makes an existing project one. A repository never contains the
git repository or a build folder.

### Commit, branch, switch

- `commit` snapshots the working copy into the local store and moves the
  current branch. It needs no network.
- `branch` creates or deletes a branch. `switch` checks one out; the editor
  saves the open scene first, then reloads it.
- `log` and `status` read the local store only.

### Remotes

- `remote add <name> <address> <server key>` records a remote. The service's
  discovery list fills both fields in the editor. Remote branches are
  tracked as `<remote>/<branch>`.
- `fetch [remote]` copies the remote's new objects and moves the tracking
  branches.
- `push [remote] [branch]` uploads and moves the remote's branch by
  compare-and-swap from the tracked commit. A push that is not a
  fast-forward is refused, and the user pulls first.
- `pull [remote] [branch]` runs fetch, then merge.
- A remote that cannot be reached shows why: no answer, refused key, or
  "this machine drops inbound traffic". The Mac then pushes to Windows and
  fetches from it. Windows keeps the Mac as a remote that it does not
  reach. The connection is still the same rule as git with one shared
  server, and no tunnel or bypass is added.
- Cooked side branches follow their branch on fetch and push, as today.

### Merge

- Phase 1 merges per file against the merge base, the nearest common
  ancestor of the two commits. A path changed on one side only is taken. A
  path changed on both sides to different content is a conflict. Each
  conflict is settled with Mine or Theirs, and a commit with two parents
  records the result.
- Locks stay the tool for binary assets that cannot be merged.
- Phase 2 merges scene edit overlays per entity. Overlays already store
  changes per entity, so two machines that edit different parts of a level
  merge with no conflict. Fields changed on both sides remain conflicts.

### Service

- One `vkr_depot` service per machine runs as a launchd agent on macOS and
  as a per-user startup task on Windows. It runs without an editor.
- It serves every repository that the user marks "Allow others to fetch and
  push", each with its own access list, on one UDP port. Discovery answers
  with the machine name and the list of served repositories.
- The editor starts the service if it does not run, and shows its state:
  running, address, served repositories, connected peers, last fetch and
  push per remote.
- `mirror` goes away. An optional "fetch and push every N seconds" setting
  per remote replaces it.

### A project holds what it reads

- `status` lists every asset the editor or Bakery read from outside the
  project during the session, as *untracked dependencies*. Commit offers to
  copy them into the project.
- For a repository project, the editor stops falling back to the git
  repository's `assets/`. Only the engine's own content still comes from
  there.
- Commit prunes builds, overlays and inventories that no live file names.
  They stay in history.

### Editor

- The top bar shows the branch picker, the commits ahead and behind
  (↑2 ↓1) and Sync.
- The Source Control panel shows:
  - changes grouped by scene and asset, with entity counts;
  - a message box and Commit;
  - outgoing and incoming commits;
  - remotes with their reachability;
  - conflicts with Mine and Theirs.
- The project hub offers Open, New and Clone from…, with discovered
  services listed under Clone. Each project row shows its branch and how
  far it is ahead or behind.
- Share, Get, Submit and Get latest go away. The `depot.*` agent operations
  take the CLI's verbs: `depot.status`, `depot.commit`, `depot.push`,
  `depot.fetch`, `depot.pull`, `depot.merge`, `depot.branch`,
  `depot.switch` and `depot.remote`.

### Command line

`vkr_depot init | clone | status | commit | push | fetch | pull | merge |
branch | switch | remote | log | lock | unlock | locks | service`. The
`keygen` and `access` commands stay. `serve` becomes a mode of `service`.

## Migration

- A working copy made by today's `clone` becomes a repository whose remote
  `origin` is its old server. `~/vkr-depot/work` on the Mac keeps its
  history.
- A workspace made by Share becomes a repository with no remote. The user
  can then add one.
- The editor keeps today's `.vkrdepot/state` reader for one release and
  writes the new form on first open.
- The launchd `com.vkr.depot.serve` and `com.vkr.depot.mirror` jobs are
  replaced by the service job.

## Phases

1. **D1, local repositories and remotes (CLI).** Local store per repository,
   local commit, remotes, tracking branches, fetch and push with
   fast-forward refusal, and the migration of working copies.
2. **D2, merge per file.** Merge base search, conflicts, Mine and Theirs,
   merge commits, and `pull`. `mirror` is retired.
3. **D3, service and editor.** The per-machine service and its editor state,
   the Source Control panel, the hub's Open, New and Clone, and the renamed
   `depot.*` operations. Share and Get are removed.
4. **D4, self-contained projects.** Projects move outside the git
   repository, Testbed becomes a template, untracked dependencies show in
   status, the asset fallback ends for repository projects, and commit
   prunes dead files.
5. **D5, merge per entity.** Scene overlay merge per entity, and its
   conflict view.

## Evidence for each phase

All scene evidence uses the Testbed project's Bistro scene, between the
Mac M1 and the Windows desktop.

- **D1:**
  - A commit on each machine with the network down.
  - The Mac pushes to Windows; Windows fetches from the Mac and gets the
    "drops inbound traffic" reason, not a timeout.
  - A clone on Windows matches the Mac's tree hash.
- **D2:**
  - The two machines change different files on `main` and both merges
    succeed.
  - The two machines change one file and the conflict is listed. Mine and
    Theirs each give the expected tree, and the merge commit has two
    parents.
- **D3:**
  - With no editor running, the Mac fetches from the Windows service.
  - An editor round trip (commit, push, then pull on the other machine)
    reloads the open scene.
  - The panel shows ahead and behind counts that match `vkr_depot status`.
- **D4:**
  - A Bistro edit that uses a material outside the project shows it as
    untracked.
  - After commit, a clone on the other machine opens with no missing-asset
    warnings.
- **D5:** two machines each move different Bistro entities in one scene,
  and pull merges both with no conflict.

## Open questions

- Should a repository's store be shared between clones on one machine, as
  `clone --store` does today?
- Which history operations are worth adding: reset, revert, stash?
- How should the harness and the agent skills find projects once they
  leave the git repository?
