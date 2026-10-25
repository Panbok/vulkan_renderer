---
status: proposed
updated: 2026-10-25
authority: proposal
---

# Depot repositories, remotes and merge

Every project becomes a depot repository with a local store, commits are
local, named remotes take fetch, push and pull, diverged branches merge, one
background service per machine serves the repositories, and the editor has a
Source Control window instead of Share and Get. The repository model, the
merge and the editor's Source Control are implemented and recorded in
[ADR-133](../adr/133-depot-repositories-remotes-and-merge.md) and
[ADR-110](../adr/110-project-sharing-in-the-editor.md), and the editor
shares repositories through the per-machine service
([ADR-134](../adr/134-per-machine-depot-service.md)). This proposal keeps
the work that remains: the evidence between the two machines.

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

On 2026-10-25, after trying Source Control, the owner added:

4. **No copies.** A project is one folder per computer; Add version
   control makes that folder the repository and copies nothing. No list
   of repositories, no Sharing tab, no store apart from the project's own;
   one Clone from another computer puts the project on a second computer.
5. **The Changes list follows the work** and names scenes and project
   parts, not internal files.
6. **One project on each machine.** Copies made for a test are deleted
   before handing over. The depot test copy, which shares its history
   with both machines, became the one Testbed.

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
| D3, service and editor | Editor Source Control implemented, with Bakery's pull report after each pull. The per-machine service runs on the Mac as a launchd agent, follows `service.conf` while it runs, publishes its peers, has `add`, `remove` and `status` in the library, and syncs each served store with remotes that have an interval. Add version control works in the project's own folder, a computer that takes connections serves each project's repository by itself, and the editor answers the access requests the service publishes; the hub clones each repository a service lists, or opens the folder this computer already has; a remote of an outbound-only machine says it is reachable only when it connects; the working copy's state no longer names its server. Left: the evidence between the two machines | ADR-110, ADR-133, ADR-134, below |
| D4, self-contained projects | Implemented: a repository reads only its own assets and engine content (git `e18d74d2`), Bakery's `untracked_dependencies` lists and adopts the rest (`c4f425d5`), the editor's Commit stops on them with Copy in and commit, and Commit first prunes what no live file names with Bakery's `collect_garbage` (ADR-110). Projects live in their own folders under the projects folder, set in the Hub (ADR-069). New projects start from `assets/templates`. The Windows desktop's repository workspace moved to `D:\VKR Projects\Testbed`. Left: the evidence between the two machines | ADR-069, ADR-110, below |
| D5, merge per entity | Implemented: overlays both sides changed merge per entity in Bakery's `merge_scene_unit`, checked on Windows with two clones on loopback. Left: the evidence between the two machines on Bistro | ADR-133, below |

## Remaining work

### The rest of the service and editor (D3)

- Done 2026-10-24: the Mac's served store syncs with the remote `windows`
  every 30 s (`remote add ... --interval`) once the owner allowed VKR Depot
  under Local Network, and the launchd `com.vkr.depot.mirror` and
  `com.vkr.depot.serve` jobs are retired.
- Done 2026-10-24: a remote shows "reachable only when it connects" for an
  outbound-only peer that connected from its host (ADR-110), and the working
  copy's state (version 3) no longer names its server; `origin` holds it
  (ADR-133).

### Self-contained projects (D4)

Implemented: untracked dependencies with Copy in and commit, the end of
the asset fallback for repository projects except engine content, pruning
on commit, and projects in their own folders under a projects folder
(ADR-069). Left: the evidence below. The Mac's repository workspace moves
to its projects folder when that session is ready.

### Merge per entity (D5)

Implemented (ADR-133). Left: the evidence below between the two machines.

## Phases

1. **D3, the rest:** implemented; the evidence below.
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

Results so far, 2026-10-24 to 2026-10-25, Release on both machines:

- **D5 passed.** The Mac moved `titlecard1` (0:7:1) and committed
  `8558a377de75` without pushing. Windows moved `titlecard2` (0:10:1) and
  committed `5acc5ef59fd5` into windows-d. The Mac's `depot.sync` made
  `b5ae9450f793` "Merge origin/main" (two parents) with no conflict and
  pushed it, and the Mac's service passed it on to windows-d. On both
  machines Bistro then has `titlecard1` at world y 14.5 and `titlecard2` at
  13.1288.
- **D1, the outbound-only label, passed** on Windows: a scratch clone with
  the Mac as remote `mac` showed "reachable only when it connects" after a
  pull failed. After `69f73d01` the label shows before any try, which
  Windows confirmed, and the status check no longer tries such a remote.
- **D3, the service without an editor:** the Mac's service fetched Bistro's
  `aa61c38a6fbd` from windows-d with no editor running. After Windows was
  shut down overnight (Fast Startup, so close to a resume, not a full
  reboot), its service ran again 6 s after logon and the Mac's service
  resumed on its own. The Source Control window's ahead and behind counts
  (1 and 1 before the D5 sync) matched `vkr_depot status`.
- **D4, a clone that opens:** Bistro reached the Mac with its textures
  cooked there only after two fixes: Bistro's builds were rebuilt with
  texture recipes (`b72db602`), and `!probe.vkt` let the baked probe cube
  travel (git `c1dc11b6`). It then opened with 5987 entities.
- **D2, different files, passed.** The Mac committed `5558257ff58b` (two
  new files under `notes/`) and Windows `749574a96259` (a line added to its
  own note). The Mac's `depot.sync` made `fe630f3e06b8` "Merge origin/main"
  (two parents) with no conflict, and Windows fast-forwarded to it with all
  three changes.
- **D2, one file with Mine, passed.** Each machine replaced
  `notes/d2-shared.txt`: the Mac in `c57deaf892f1`, Windows in
  `f1706fc8d1ef`. The Mac's `depot.sync` listed the file as a conflict
  ("1 files changed here and on origin; choose whose to keep") and merged
  nothing. `depot.pull` with `conflicts: mine` made `f1851a370f53` (two
  parents) with the Mac's line, and the push reached windows-d.
- **D2, one file with Theirs, passed.** The same steps with `conflicts:
  theirs` made `efe651189ba1` (parents `59b3a93737e5`, `aa09f3febefe`),
  and both machines hold Windows' line.
- **D2, one import job on each side, passed.** The Mac placed
  `ll_boulder.gltf` in Bistro (`1e240b033b9c`), Windows `ll_shrub.gltf`
  (`2dfa53145719`), each with `model.place`, which adds two builds and an
  inventory. The Mac's `depot.sync` merged them with no conflict into
  `896517125785`. On both machines Bistro then opened with both models at
  their places, the D5 title cards unchanged, 6 inventories and 7 builds
  (3 + 2 + 2); `scene.describe` matched 5991 entities on the Mac.
  `prepare_scene` passed on both of the project's scenes (Bistro on open;
  the Level Design Test as a Bakery job, no warnings).
- **D3, a rebuild while the service runs, passed on the Mac:** building
  `vkr_depot` left the service at the same PID, because it runs from its
  `VKR Depot.app` copy.
- **D3, the editor round trip with the scene open, passed both ways.**
  With Bistro open in a Windows editor, the Mac committed and synced
  `df0cb962e1ec` (a model raised to y 2); Windows' `depot.pull` merged it
  and the open scene reloaded with the new place. Windows then committed
  `1f6145a5ebfa` in the same editor; on the Mac, `depot.sync` in an editor
  with Bistro open pulled it, the scene reloaded by itself (1.3 s) and the
  other model read y 1. The served copy on Windows first could neither pull
  nor commit after a push from another computer, because it has no remote;
  git `e6c04214` makes Pull take the store's branch.
- **D4, untracked materials, passed on the Mac.** A brick brush in Bistro
  whose surface theme binds a material and texture that only the engine
  checkout held, git-ignored, stopped `depot.commit` with both listed;
  `adopt` copied them into the working copy's `assets/` and committed
  `7a5dea7b634a`. A theme naming a git-tracked engine material is not
  untracked, since every install has it. The editor's first commit after
  unreferenced builds passed the 24-hour grace also prunes them (ADR-110),
  which removed six Level Design Test builds in `59b3a93737e5`.
- **D1, a clone matches, and D4, a clone that opens under a non-ASCII
  root, passed on Windows.** An editor `depot.clone` from windows-d into
  `D:\Проекты\vkr-d4` (14.6 GB with its store, 21 minutes on an HDD)
  fetched 690 cooked textures from `main.cooked.bc` and cooked 581, none
  failing, among them the adopted `d4_grid.png`. `vkr_depot status` there
  gave 0 changes at `8c29c8508c50`, the Mac's commit, so the trees match.
  Bistro opened with no missing-asset lines, the brick faces on the copied
  `d4_brick.mt` and the D2 and D3 models in place. The first try failed
  after 268 MB on the transport's 15 s idle timeout, while both ends waited
  on the same disk; git `fdc12ce1` keeps connections through 60 s stalls.
- Left, each needing the owner: D1's commits with the network down; D3
  after a full reboot; D3's service install on a Windows PC with no rule
  yet, which asks UAC for the Defender rule.

## Open questions

- Which history operations are worth adding: reset, revert, stash?
