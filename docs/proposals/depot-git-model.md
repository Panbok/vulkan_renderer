---
status: proposed
updated: 2026-10-24
authority: proposal
---

# Depot repositories, remotes and merge

Every project becomes a depot repository with a local store, commits are
local, named remotes take fetch, push and pull, diverged branches merge, one
background service per machine serves the repositories, and the editor has a
Source Control window instead of Share and Get. The repository model, the
merge and the editor's Source Control are implemented and recorded in
[ADR-133](../adr/133-depot-repositories-remotes-and-merge.md) and
[ADR-110](../adr/110-project-sharing-in-the-editor.md). This proposal keeps
the work that remains: Bakery's merge operations, the editor on the
per-machine service, self-contained projects (D4), merge per entity (D5)
and the evidence between the two machines.

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

## Where it stands

| Phase | State | Record |
| --- | --- | --- |
| D1, repositories and remotes | Implemented: local commits, remotes, tracking branches, fetch, fast-forward push and pull, branches, adoption of older working copies, HELLO from every client | ADR-133 |
| D2, merge per file | Implemented in the depot: merge base, per-file decisions, units sent whole to a driver, merge commits, pull that merges. Missing: Bakery's operations, so a unit both sides changed is always a conflict | ADR-133, below |
| D3, service and editor | Editor Source Control implemented. The per-machine service landed from the Windows track (git `5e8326a8`, `241587ad`) and is recorded by that track; the editor still serves a repository itself | ADR-110, below |
| D4, self-contained projects | Not started | below |
| D5, merge per entity | Not started | below |

## Remaining work

### Bakery merge operations (D2)

`vkr_depot_merge_bakery_driver` already runs `vkr_bakery project --request
<file> --result <file>` (ADR-133). Bakery needs the operations:

- `merge_scene_unit` for the `scene` driver and `merge_project_document` for
  `project`. The request carries `version` 1, `read_only` true,
  `workspace_root`, `anchor` (the anchor's repository path) and the folders
  `base`, `mine`, `theirs` and `output`, each holding the unit's member
  files at their paths relative to the anchor's folder. The scene operation
  merges the live inventories' records by `id` and the scene file and edit
  overlay that name them, and keeps a record both sides changed as a
  conflict. It writes the merged files into `output` and answers
  `{"merged":true,"write":[…],"delete":[…]}` or `{"merged":false,
  "conflicts":[{"path","id","reason"}…]}`, paths relative to the folder.
- `pull_report`: `recook_textures` then `prepare_scene` for each scene a
  pull changed, with a result per scene, so a pull says whether each scene
  opens, not only that the transfer finished.
- `share_project` writes `.vkrdepotmerge` with the rules
  `unit scene .vkreditor/projects/*/scenes/*/scene.json inventory/*.json edits/*.json`
  and `unit project .vkreditor/projects/*/project.json`, so every new
  repository merges its scenes as units.

### The editor on the per-machine service (D3)

- The editor stops hosting the server: Let others fetch and push registers
  the repository with the installed `vkr_depot service`, starts the service
  when it does not run, and shows its state (running, address, served
  repositories, connected peers) from `VkrDepotServiceInfo`. Access
  requests and the access list come from the service.
- A remote's "fetch and push every N seconds" setting (`interval` in the
  remotes file) replaces `mirror`, and the launchd `com.vkr.depot.serve`
  and `com.vkr.depot.mirror` jobs on the Mac give way to the service.
- A remote's state shows "reachable only when it connects" for a peer the
  service recorded as outbound only.
- The working copy's state drops the server it syncs with once nothing
  reads `remote` and `server_key` (`editor_session.c` and the sparse remote
  in `vkr_depot_remote.c` read them today); the remote `origin` holds it.

### Self-contained projects (D4)

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

### Merge per entity (D5)

Scene edit overlays merge per entity: two machines that move different
entities of one scene merge with no conflict, and only fields both sides
changed are conflicts, shown per entity.

## Phases

1. **D2 Bakery operations** (Windows track): the three operations and the
   `.vkrdepotmerge` rules from `share_project`.
2. **D3 editor on the service:** once the service's state is in
   `VkrDepotServiceInfo` on both platforms.
3. **D4, self-contained projects.** Projects move outside the git
   repository under a configurable root, Testbed becomes a template,
   untracked dependencies show in status, the asset fallback ends for
   repository projects except engine content, and commit prunes dead files.
   The `vkr-cross-machine` skill, `tools/checks/check_bakery_share.py` and
   local tools that copy the repository's Testbed workspace move to the
   template path. Harness cases read `assets/scenes`, not projects, and do
   not change.
4. **D5, merge per entity.**

## Evidence for each phase

All scene evidence uses the Testbed project's Bistro scene, between the
Mac M1 and the Windows desktop. One-machine evidence for D1, D2 and the
editor is in ADR-133 and ADR-110; these gates need both machines.

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
