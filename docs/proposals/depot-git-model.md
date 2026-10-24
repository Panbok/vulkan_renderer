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

The Windows session reviewed the first draft (git `c11bac93`) on 2026-10-24.
This revision takes its corrections: the causes in the table, merging scene
folders as units, lock and cooked-branch rules after a merge, the Windows
service form and installed path, and the added evidence.

## Why: the Level Design Test transfer, 2026-10-23 to 2026-10-24

Moving one scene from the Mac to Windows took two agent sessions and one
night. Each failure points at a part of the model:

| What happened | Cause in today's model |
| --- | --- |
| Windows could not see the Mac's shared project | Only the sharing editor serves, and the Mac drops inbound traffic. Nothing tells the user that a machine cannot serve. |
| The Mac's share (`~/VKR Projects/Testbed 2`) had no history in common with the Windows depot copy | Share makes a new store each time. Two copies of one project can have separate histories. |
| The Windows server had been stopped for 5 days without notice, and stopped 3 times in 24 hours | The server is the editor, a `vkr_depot serve` process or a launchd job, and no surface shows whether any of them runs. A server started from a shell stops with that shell, and nothing restarts one after a reboot. |
| The transfer needed a hand-made file copy into another working copy | No editor action moves changes between two stores. `mirror` only fast-forwards and stops at diverged branches. |
| 53 materials, a graph and 43 textures arrived missing | The editor falls back to the repository's `assets/` (ADR-077 overlay), so a project looks complete on the machine that made it. Bakery's `share_project` did not follow surface themes. The commit went straight into a working copy and never ran `share_project` at all, so only a check at commit time would have caught it. |
| Stale material records of rebuilt models blocked the cook on Windows | A model rebuild or reimport kept the import's old material records live in the inventory. `recook_textures` stopped at the first build it could not cook, and Get latest still reported success. |
| Get latest said "some textures did not cook", but the scene did not open | A pull reports that the transfer finished, not whether each scene prepares and opens. |

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
- `pull [remote] [branch]` runs fetch, then merge, then `recook_textures`
  and `prepare_scene` for each scene the pull changed. It reports per scene
  whether the scene prepares, not only that the transfer finished.
- A remote that cannot be reached shows why: no answer, or a refused key.
  A machine cannot tell a dropped packet from a powered-off peer, so the
  "outbound only" reason comes from the peer itself: a service configured
  as outbound only says so whenever it connects. Windows then shows the Mac
  as "reachable only when the Mac connects". The Mac pushes to Windows and
  fetches from it. No tunnel or bypass is added.
- Cooked side branches follow their branch on fetch and push, as today.
  Each machine publishes the side branch of its own platform for every
  commit it checks out, merge commits included.

### Merge

- Phase 1 merges per file against the merge base, the nearest common
  ancestor of the two commits. A path changed on one side only is taken. A
  path changed on both sides to different content is a conflict. Each
  conflict is settled with Mine or Theirs, and a commit with two parents
  records the result.
- **A scene folder merges as one unit, and so does `project.json`.** Each
  Bakery job writes a new `inventory/<uuid>.json` and moves the
  `inventory` field of `scene.json`, and each edit writes a new
  `edits/<uuid>.json` the same way. A per-file merge after a job on both
  machines sees two added inventories with no conflict and one
  `scene.json` conflict. Mine or Theirs would then drop the other side's
  imports without a warning. So a conflict on `scene.json`, its live
  inventory or its live overlay lists the whole scene. Phase 1 also merges
  the live inventories' record lists by record `id`, and keeps a record
  that both sides changed as a conflict.
- Locks stay the tool for binary assets that cannot be merged. A commit
  checks locks against the last fetched lock list and warns, because it
  can run offline. A push checks them on the server and refuses, as today.
- Phase 2 merges scene edit overlays per entity. Overlays already store
  changes per entity, so two machines that edit different parts of a level
  merge with no conflict. Fields changed on both sides remain conflicts.

### Service

- One `vkr_depot` service per machine runs as a launchd agent on macOS and
  as a per-user Task Scheduler task on Windows ("At log on", restart on
  failure). It runs without an editor and survives the sessions that
  started it. A Windows service (SCM) is not used: it runs in session 0
  under another account, without the user's identity key or home folder.
- The service runs from an installed copy at a fixed path:
  `%LOCALAPPDATA%\VKR\depot\` on Windows and
  `~/Library/Application Support/VKR/depot/` on macOS.
  `vkr_depot service install` copies the binary there and restarts the
  task. Builds never touch the running service. Windows locks a running
  exe, and each new exe path gets its own firewall prompt and rule (8 such
  rules exist on the Windows desktop today). The firewall rule binds to the
  installed path and covers UDP 7320 and the 7319 discovery port. The
  installer creates it for the active network profile (Public on the
  Windows desktop), or asks. The task needs no administrator rights, but
  the firewall rule does: `service install` asks once through UAC, and
  when that is declined it prints the exact `netsh` command.
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
  repository's `assets/`. Only engine content still comes from there:
  paths that git tracks under `assets/` plus the editor bundle, for example
  the greybox role materials. A theme that names engine content is not
  flagged as untracked.
- Commit prunes builds, overlays and inventories that no live file names,
  with the rules of Bakery's `collect_garbage`: a record's `previous_build`
  stays (ADR-088 revisions), and recent files get a grace period. Pruned
  files stay in history.
- Projects live under a configurable projects root. The default is under
  the user's profile, which is a non-ASCII path on the Windows desktop
  (`C:\Users\Пользователь`).

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

## Contracts between the tracks (C0)

The Mac session implements the depot core and the editor, and the Windows
session the service and Bakery. The three contracts below are where their
work meets.

### Repository layout

A store is the repository. The working copies of one repository can share
its store, as `clone --store` does today, and the service serves that same
store.

```
<store>/objects/                   chunks and objects (unchanged)
<store>/refs/<branch>              local branches: the ones the store serves
<store>/repository                 repository id (16 random bytes) and name
<store>/remotes                    per remote: name, address, service key,
                                   repository id, fetch-and-push interval
<store>/tracking/<remote>/<branch> each remote branch at the last fetch or push
<store>/tracking/<remote>.locks    the remote's locks at the last fetch
<store>/locks, access              unchanged
```

- The working copy's `.vkrdepot/state` (version 3) keeps the checked-out
  branch, the base commit, the index and the sparse flag. The remote and
  server key leave it.
- A branch moves by compare-and-swap under the store's named process lock
  (`vkr_platform_process_lock_acquire`). A local commit, a fetch and the
  server use the same lock, so a working copy can share the store its
  service serves.
- Branch names keep `vkr_depot_branch_valid`. A remote name has 1 to 32
  characters from `a-z`, `0-9`, `-` and `_`.
- Migration of a version 1 or 2 state: the remote and key become the remote
  `origin`, `tracking/origin/<branch>` and `refs/<branch>` (when absent)
  get the base, and the store gets a repository id. A state whose server
  key is the store's own `server.key` is the served repository and gets no
  `origin`.

### Merge driver

- `.vkrdepotmerge` at the root names units: groups of paths that merge
  together through a driver. Each line is
  `unit <driver> <anchor pattern> [member patterns relative to the anchor's folder]`:

  ```
  unit scene .vkreditor/projects/*/scenes/*/scene.json inventory/*.json edits/*.json
  unit project .vkreditor/projects/*/project.json
  ```

- A driver name is only a key into drivers built into the caller (`scene`,
  `project`). A line that names any other driver is ignored with a
  warning. A pulled file can never make the depot run a command.
- A merge first decides each path against the merge base. A unit goes to
  its driver when each side changed at least one of its paths, even
  different ones: two jobs add different inventories and both move
  `scene.json`. The driver gets
  the unit's member files of each side, written to `base/`, `mine/` and
  `theirs/` under a temporary folder, and returns one of three results:
  merged (files to write and delete under the anchor's folder), conflicts
  (a list of `{path, id, reason}`), or failed. When the driver is missing
  or fails, the whole unit is a conflict, and Mine or Theirs settles it as
  one unit.
- `tools/depot` links only `vkr_bakery_os` and runs no Bakery job in its
  process. So the depot calls a function that the caller supplies, and
  `vkr_depot` and the editor supply one that runs
  `vkr_bakery project --request <file> --result <file>`. The `scene`
  driver maps to the operation `merge_scene_unit` and `project` to
  `merge_project_document`. The request and result use the existing
  project-job JSON, with the fields `anchor`, `base`, `mine`, `theirs`,
  `output` and per-side `paths`. Bakery writes the merged files into
  `output`. Its result is `{"merged":true,"write":[…],"delete":[…]}` or
  `{"merged":false,"conflicts":[…]}`.
- After a pull, the depot runs the operation `pull_report` for the scenes
  the pull changed. It runs `recook_textures` and then `prepare_scene`, and
  returns a result per scene.
- The depot finds `vkr_bakery` next to its own executable, or through
  `--bakery` or `VKR_BAKERY`. The Mac session owns the depot side and the
  Windows session owns the three operations.

### Service

- One service key per machine. The service adopts an existing served
  store's key, so remotes that name `f9239cc6…` (windows-d) stay valid.
- Discovery answers also list the served repositories (id, name, default
  branch) and say when the machine is outbound only.
- After the handshake, the client selects a repository by id. A server
  that serves one store takes a connection that selects none as that
  store, so older clients keep working until both machines update.
  `VkrDepotConnectConfig` gains the repository id, where zero means none.
- An outbound-only service says so in its hello, and the server records
  it for that key. The remote's status then shows "reachable only when it
  connects".
- `VkrDepotServiceInfo` reports: running, address and port, the served
  repositories (name, id, store), and connected peers (name, key, last
  seen, outbound only). The last fetch and push per remote live in the
  repository.

## Migration

- A working copy made by today's `clone` becomes a repository whose remote
  `origin` is its old server. `~/vkr-depot/work` on the Mac keeps its
  history.
- A working copy that shares its server's store (`clone --store`, such as
  `D:\vkr-depot\work`) becomes the served repository itself and gets no
  `origin`, so it never becomes a remote of itself. A repository's store
  can be a path outside `.vkrdepot/` for this case.
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
   merge commits, scene folders and `project.json` merged as units with
   inventory records merged by `id`, and `pull` with its per-scene prepare
   report. `mirror` is retired.
3. **D3, service and editor.** The per-machine service, `service install`
   with its fixed path and firewall rule, the service's state in the
   editor, the Source Control panel, the hub's Open, New and Clone, and the
   renamed `depot.*` operations. Share and Get are removed.
4. **D4, self-contained projects.** Projects move outside the git
   repository under a configurable root, Testbed becomes a template,
   untracked dependencies show in status, the asset fallback ends for
   repository projects except engine content, and commit prunes dead files.
   The `vkr-cross-machine` skill, `tools/checks/check_bakery_share.py` and
   local tools that copy the repository's Testbed workspace move to the
   template path. Harness cases read `assets/scenes`, not projects, and do
   not change.
5. **D5, merge per entity.** Scene overlay merge per entity, and its
   conflict view.

## Evidence for each phase

All scene evidence uses the Testbed project's Bistro scene, between the
Mac M1 and the Windows desktop.

- **D1:**
  - A commit on each machine with the network down.
  - The Mac pushes to Windows. Windows shows the Mac as reachable only when
    the Mac connects, because the Mac's service announced itself as
    outbound only, not after a timeout.
  - A clone on Windows matches the Mac's tree hash.
- **D2:**
  - The two machines change different files on `main` and both merges
    succeed.
  - The two machines change one file and the conflict is listed. Mine and
    Theirs each give the expected tree, and the merge commit has two
    parents.
  - Both machines run one import job in the same Bistro scene. The merge
    keeps both sides' records, or it lists the whole scene as a conflict;
    it never drops a side.
  - After every pull, `prepare_scene` passes on every scene of the project.
- **D3:**
  - With no editor running, and after a reboot, the Mac fetches from the
    Windows service.
  - A rebuild of `vkr_depot` while the service runs leaves the service and
    its firewall rule untouched.
  - A fresh `service install` on a Windows PC that has no rule yet.
  - An editor round trip (commit, push, then pull on the other machine)
    reloads the open scene, and `prepare_scene` passes on every scene.
  - The panel shows ahead and behind counts that match `vkr_depot status`.
- **D4:**
  - A scoped Bistro case with one brush whose surface theme names
    materials outside the project shows those materials and their textures
    as untracked.
  - After commit, a clone on the other machine opens with no missing-asset
    warnings.
  - A project under a non-ASCII projects root on Windows clones, cooks and
    opens.
- **D5:** two machines each move different Bistro entities in one scene,
  and pull merges both with no conflict.

## Open questions

- Which history operations are worth adding: reset, revert, stash?
