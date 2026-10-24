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
the work that remains: the editor on the per-machine service, the rest of
self-contained projects (D4) and the evidence between the two machines.

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
| D2, merge per file | Implemented: merge base, per-file decisions, units sent whole to Bakery's `merge_scene_unit` and `merge_project_document`, merge commits, pull that merges, `.vkrdepotmerge` written by `share_project` | ADR-133 |
| D3, service and editor | Editor Source Control implemented, with Bakery's pull report after each pull. The per-machine service runs on the Mac as a launchd agent, follows `service.conf` while it runs, publishes its peers, has `add`, `remove` and `status` in the library, and syncs each served store with remotes that have an interval. The editor still serves a repository itself | ADR-110, ADR-133, ADR-134, below |
| D4, self-contained projects | Partly implemented: a repository reads only its own assets and engine content (git `e18d74d2`), Bakery's `untracked_dependencies` lists and adopts the rest (`c4f425d5`), the editor's Commit stops on them with Copy in and commit, and Commit first prunes what no live file names with Bakery's `collect_garbage` (ADR-110). Left: projects under a configurable root, Testbed as a template | ADR-110, below |
| D5, merge per entity | Implemented: overlays both sides changed merge per entity in Bakery's `merge_scene_unit`, checked on Windows with two clones on loopback. Left: the evidence between the two machines on Bistro | ADR-133, below |

## Remaining work

### The editor on the per-machine service (D3)

- The editor stops hosting the server: Let others fetch and push registers
  the repository with `vkr_depot_service_add`, starts the service when it
  does not run, and shows its state (running, address, served repositories,
  known peers) from `vkr_depot_service_status` (ADR-134). Access requests
  and the access list come from the service.
- The service syncs remotes that have an `interval` (ADR-134); the launchd
  `com.vkr.depot.mirror` job on the Mac gives way to it once a remote of the
  served store has one.
- A remote's state shows "reachable only when it connects" for a peer the
  service recorded as outbound only.
- The working copy's state drops the server it syncs with once nothing
  reads `remote` and `server_key` (`editor_session.c` and the sparse remote
  in `vkr_depot_remote.c` read them today); the remote `origin` holds it.

### Self-contained projects (D4)

Implemented: untracked dependencies with Copy in and commit, the end of
the asset fallback for repository projects except engine content, and
pruning on commit (see the table). Left:

- Projects live under a configurable projects root. The default is under
  the user's profile, which is a non-ASCII path on the Windows desktop
  (`C:\Users\Пользователь`).
- The repository's Testbed becomes a template that new projects start from.

### Merge per entity (D5)

Implemented (ADR-133). Left: the evidence below between the two machines.

## Phases

1. **D3 editor on the service:** the service's settings and status are in
   the library (ADR-134).
2. **D4, the rest of self-contained projects.** Projects move outside the git
   repository under a configurable root, Testbed becomes a template,
   untracked dependencies show in status, the asset fallback ends for
   repository projects except engine content, and commit prunes dead files.
   The `vkr-cross-machine` skill, `tools/checks/check_bakery_share.py` and
   local tools that copy the repository's Testbed workspace move to the
   template path. Harness cases read `assets/scenes`, not projects, and do
   not change.
3. **D5, merge per entity.**

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
