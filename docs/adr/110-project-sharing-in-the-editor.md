---
status: partial
updated: 2026-10-24
authority: adr
---

# ADR-110: Source control in the editor

## Status

Accepted (partial). The editor's Source Control makes a project a
repository, commits, makes and switches branches, pushes, pulls with
merges, syncs with remotes, serves a repository to the local network,
approves access and clones repositories other computers serve, with no
command line and no settings to type. It replaced Share, Get, Submit and Get
latest on 2026-10-24 (the depot model is
[ADR-133](133-depot-repositories-remotes-and-merge.md)). This computer's
depot service ([ADR-134](134-per-machine-depot-service.md)) serves the
repositories the user shares, also while the editor is closed; the editor
adds them to it, starts it and shows its state. The Windows build of the
editor part, a run between two computers and the macOS Local Network prompt
for the editor are unverified.

## Context

The depot ([ADR-105](105-network-transport-and-asset-depot.md)) moved
projects between a Mac and a Windows machine, but only through its command:
`keygen`, `init`, `access` with copied keys and a server restart, a
staging script that chose a project's files, `serve` and `mirror` as launchd
agents in an app bundle for Local Network access, and a manual texture cook
after each pull. The owner asked for the same through the editor, without
permissions to troubleshoot, configuration or commands.

Owner decisions:

- **2026-10-11:** Share moves the project into a workspace of its own; the
  sharing editor runs the server while it is open (no background service);
  a computer the server does not know asks, and the sharing editor approves.
- **2026-10-12:** imported models get platform-neutral texture names and are
  re-cooked on the receiving computer
  ([ADR-077](077-asset-build-system.md#decision)).
- **2026-10-24:** a model shaped like git replaces sharing: every project a
  repository, local commits, remotes, merge; the server becomes a service per
  machine that the editor shows and never hosts
  ([proposal](../proposals/depot-git-model.md#owner-decisions-2026-10-24)).

## Decision

[editor_depot.c](../../editor/src/editor_depot.c) owns source control. One
worker thread runs one step at a time (status, commit, push, pull, sync,
fetch, clone, make repository, switch, start the service); the machine's
depot service serves repositories in a process of its own
([vkr_depot_service.h](../../tools/depot/vkr_depot_service.h)). The UI
thread reads finished results and progress counters only.

### Repositories

- A project whose workspace is a depot working copy is a repository; opening
  it adopts a working copy of the older form (ADR-133).
- **Make repository** saves nothing itself and refuses while a scene has
  unsaved edits. It runs Bakery's `share_project` into `~/VKR
  Projects/<name>` (or a folder the user names, outside the git repository),
  rebuilds each model `needs_rebuild` lists, removes the builds that
  replaced, makes the folder a working copy, commits `Created <name>` and
  reopens the editor there (`vkr_editor_projects_open_workspace`).
- **Let others fetch and push** adds the repository's store to the stores
  the machine's service serves (`vkr_depot_service_add`, under the
  project's name in the letters a store name takes), and clearing it
  removes the store; a running service follows within a second. When the
  service does not run, the editor installs and starts it with the
  `vkr_depot` beside it (`vkr_depot service install`, which keeps the
  service's settings), also from a Start button. A store that still holds
  the `server.key` of an editor that served it itself is not served until
  the user turns this on. Each repository the editor made or cloned is
  listed in `<user data>/VKR/depot.json` for the project hub.
- New materials, graphs and themes in a repository are written to its own
  `assets/`, so the project carries them (ADR-077).

### Service and access

- Each status step reads the machine's service on the worker thread
  (`vkr_depot_service_status`, waiting up to 300 ms for its loopback
  discovery answer) and the store's access list. The window shows whether
  the service runs, its computer name, port, key and connected peers, and,
  for an outbound-only computer, that others reach it only while it is
  connected to them. The editor reaches the service over its loopback
  address, selecting the repository by its ID, for the cooked side branch.
- The store's owner is the user's identity (`vkr_depot_service_add` makes
  it the admin). A computer the service does not know is refused at the
  handshake and recorded with the name it sent (`user@computer`); the
  service publishes the requests in `service.status`, the top bar shows `N
  asking`, and a card offers Allow, Fetch only and Deny, with the first
  eight digits of the asking key; the waiting computer shows its own, so
  two people can tell a real request from an impostor that sends the same
  name. Allow and Fetch only write the store's access list, which the
  service rereads within a second; Deny hides the request until the editor
  restarts. The waiting computer retries every 3 s and continues on its own
  once allowed. Names of allowed computers are kept for the access list.
- The identity key lives in the user data folder, not the cache
  (`vkr_depot_identity_path`).

### Cloning

- The project hub lists the repositories on this computer (Open) and those
  found on the network (Clone), one row per repository a service lists in
  its version 2 answer; Clone selects that repository and the remote
  `origin` keeps its ID (ADR-133). Discovery is a broadcast every 2 s plus a
  direct probe of
  every server used before, since some endpoint security drops answers to
  broadcasts but not to direct probes. A field takes a computer name or
  address for a server discovery misses.
- Clone copies into `<projects folder>/<name>` (ADR-069, by default
  `~/VKR Projects`) with the server as `origin`,
  waits for approval if needed, cooks every texture source without a cooked
  file for this computer, re-cooks the build textures of each scene the copy
  holds (a project may list a scene it does not share) and the project's
  own (`recook_textures`), then opens the project. With **Fetch files when
  opened** the copy is a sparse working copy instead: the Clone writes only
  the ignore rules and each file arrives when the editor or a Bakery job
  reads it
  ([ADR-123](123-asset-streaming-from-a-depot.md#sparse-project-sharing)).
- A remote that stops answering follows its key through discovery, and the
  repository saves its new address.
- A remote at a numeric address from which an outbound-only peer last
  connected to this computer's service (`peer-address` in `service.status`)
  shows "reachable only when it connects, last N min ago" instead of a
  failure, unless an exchange with it works; its host is the remote's
  machine, which drops connections from outside.
- Discovery is not authenticated: each found repository shows its key's
  first digits and whether it was used before, and an answer with a known
  name but another key says it is not that computer.

### Scripts

Scripts are native code. A project copied from another computer does not
build or run its scripts until its user chooses Run scripts (a card, the
Source Control window or `depot.trust`); a pull, switch or merge that
changes a `Scripts/` folder holds them again until the user trusts the
change. A repository this computer shared when the editor first listed it
trusts its scripts. Built libraries never travel: the
workspace's `.vkrdepotignore` names `.vkreditor/scripts/`, and checkout
writes no ignored path (ADR-105).

### Source Control

- A status step every 20 s counts local changes (a snapshot against the
  index), fetches the selected remote, and works out the branch's commits to
  push and to pull. The top bar button shows the branch and its most
  pressing state: `conflicts`, `waiting for access`, `N asking`, `N to
  pull`, `N changes` or `N to push`; it opens the Source Control window.
- The window lists, top down: the branch with its counts against the
  remote branch, the branches to switch to and a New branch field; the
  remotes to choose from with the selected one's state and Sync, Pull, Push
  and Fetch; conflicts with Use theirs and Keep mine; the changes with a
  message, Commit and Commit & Sync; the commits to push and to pull; the
  remotes with Add remote (filled by one click from a service found on the
  network) and Remove; Let others fetch and push with the server's state,
  the access requests and the access list; and the history.
- A step the user starts stops a running status check first, so a remote
  that does not answer never holds the user's step behind its timeout.
- **Commit** needs saved scenes and records the changes in the repository.
  A full working copy first runs Bakery's `collect_garbage`, so builds,
  overlays, inventories and scenes no live file names leave the commit
  under that operation's rules (a record's `previous_build` stays,
  unreferenced files stay for 24 hours and unlisted scenes for an hour);
  history keeps them. It then asks Bakery for the files the project reads from outside itself
  (`untracked_dependencies`): with any, it stops and lists them with their
  size, and **Copy in and commit** copies them into the repository's own
  `assets/` and commits them, or **Commit without them** goes on
  (`depot.commit` `adopt` and `ignore_untracked`).
  **Push** pushes the branch when that is a fast-forward and publishes this
  computer's cooked textures to the remote's side branch
  ([ADR-123](123-asset-streaming-from-a-depot.md#project-sharing)).
  **Pull** fetches, fast-forwards or merges through Bakery (ADR-133), takes
  the cooked textures the side branch holds, cooks and re-cooks the rest,
  rescans materials when a material changed, and reloads the open scene
  once its edits are saved. Conflicts stop it with the paths listed; Use
  theirs and Keep mine pull again under that policy. **Sync** pulls, then
  pushes. **Commit & Sync** does both after the commit.
- Switching branches keeps no local change it would overwrite, and reloads
  the scene when the switch changed scene files.

### Agents and packaging

- `depot.status`, `depot.init`, `depot.commit`, `depot.push`, `depot.pull`,
  `depot.sync`, `depot.fetch`, `depot.switch`, `depot.remote`,
  `depot.serve`, `depot.service`, `depot.clone` (with `repository`, an ID
  from `depot.status` `found`), `depot.allow`, `depot.find`, `depot.trust`
  and `depot.cancel` are in the agent table
  ([editor_ops.c](../../editor/src/editor_ops.c)) and need no scene; the
  launcher serves the agent channel and source control too.
- On macOS the `vkr_editor` and `vkr_depot` executables carry an Info.plist
  with `NSLocalNetworkUsageDescription` in `__TEXT,__info_plist`
  ([vkr_info_plist.cmake](../../cmake/vkr_info_plist.cmake)), so the Local
  Network prompt names the program and says why; the service runs from an
  app bundle of its own (ADR-134). On Windows, `service install` asks once
  (UAC) for the firewall rule.

## Consequences

- Source control and collaboration use one working copy; a collaborative
  session ([ADR-106](106-collaborative-editing-session.md)) on a repository
  bases on its depot.
- A repository lives outside the git repository. Editing the original after
  Make repository does not reach the repository.
- A shared repository is reachable while its user is logged in, whether an
  editor runs or not. A computer whose endpoint security drops unsolicited
  inbound traffic cannot serve; it adds another computer's repository as a
  remote and pushes and pulls, and a remote with an interval lets its
  service do that on its own.
- Every editor on one computer shares through the one service; a status
  step costs one loopback discovery round.

## Alternatives considered

- **Track the project inside the current workspace** with include rules:
  every commit would need the closure walk, and pulls would land in the
  repository.
- **A hidden staging copy:** each submit copies and diffs the closure, and
  pulls land where nobody edits.
- **The editor serving the repository while it runs** (2026-10-11 to
  2026-10-24): no second process, but a repository was reachable only
  while its editor ran, and a second editor served on the next port
  without discovery.
- **Open access on the network, or a pairing code:** declined for approval
  on the server.
- **Shipping cooked textures, or rebuilding a model on each receiver:**
  unusable across GPU families, or a new revision on every machine that
  then travels back.

## Evidence

Prune on commit, Windows desktop, Release, 2026-10-24: a scratch copy of
the FPS Arena project made a repository with `depot.init` on `D:/` (which
failed with "Cannot create directory D:" until `vkr_bakery_make_directories`
took a bare drive as its root), an unlisted scene folder two hours old
committed with the `vkr_depot` command, then a headless editor's
`depot.commit` logged "removed 1 unused paths" and committed its removal;
the earlier commit still holds it.

Of the Share, Get and Submit steps this replaced, Mac M1, Release,
2026-10-12, headless editors driven through the agent
channel with separate `HOME` folders, the Testbed project (Bistro and Level
Design Test), and a launchd `vkr_depot serve` holding UDP 7319 and IPv4 7320.

- Share: the copy, the Bistro rebuild and the first commit of 1,507 files
  took about 45 s; the server bound 7321 without discovery.
- Get on a second identity: refused, its request listed with its name,
  allowed with `depot.allow`; 2.71 GB in about 2 min over loopback; 514
  Bistro build textures re-cooked, byte for byte equal to the sharer's; the
  copy opened with no local changes.
- Submit from the copy, New changes and Get latest on the sharer; then both
  changed one file: the sharer's Submit listed the conflict, Keep mine and a
  second Submit went through, and the copy got it.
- Windowed captures of the top bar, the request card, the Project sharing
  window and the hub with a Get waiting for approval.
- The sharing editor, windowed with Bistro open, while a headless editor
  on the same Mac got the project (2.71 GB): frame time median 25.05 ms and
  p95 25.65 ms over 38 samples during the transfer, against 23.82 ms and
  28.03 ms over 14 samples before it; one frame took 93 ms during it. The
  receiving editor's hashing and writes share the machine, so this bounds
  the server thread's cost from above.
- A script module added and submitted on the sharer stayed held on the
  copy after Get (`scripts_held`); its build started only after
  `depot.trust`. That module, named in lower case, then failed to compile
  because `VKR_MODULE` and `VKR_BEHAVIOR` defined the same hook wrapper
  names; the SDK now names the module's apart.

Source Control, Mac M1, Release, 2026-10-24: a headless editor on an FPS
Arena repository whose `origin` was a loopback `vkr_depot serve`, after
another clone pushed a commit. `depot.status` showed 1 to pull;
`depot.commit` with `sync` committed a new cube, merged the other commit
(`Merge origin/main`, two parents) and pushed, leaving 0 to push and 0 to
pull; the other clone pulled both changes; `depot.switch` with `create` made
and switched to `feature`; a window capture showed the top bar button and
the window with its branches, remote state, Sync, Pull, Push, Fetch, the
commit to pull, the remotes and the history. Make repository on a project
whose scene names a missing `source.glb` stopped at Bakery's
`share_project` (VKR-IDX-0001), as intended. With a cube pointing at the
git-ignored `assets/materials/bm/concrete_floor.mt`, Commit stopped and
listed 8 files (the material, its graph and 6 textures, 37.3 MB), and Copy
in and commit copied them into the repository's `assets/` and committed
them. A pull that changed one scene reported "1 of 1 pulled scenes open"
through Bakery's `pull_report`.

The editor on the machine's service, Mac M1, Release, 2026-10-24: a
headless editor under a separate `HOME` on that FPS Arena repository,
against a `vkr_depot service run` of that `HOME` on 127.0.0.1:7362 with
discovery on UDP 7363 (so the installed service stayed untouched).
`depot.serve` added the store to `service.conf` and the service logged
"serving … as arena" within a second; `depot.status` then showed it
running on port 7362 with its key. A second identity's `vkr_depot clone
--repository` was refused 13 times while `depot.status` listed its request
with its name; `depot.allow` gave it write access and its next try cloned
147 files. A third identity allowed as read-only stayed out of the request
list in the next two status steps. `depot.serve` with `serve` false removed
the store and the service stopped serving it. A window capture showed the
serving line with host, port, key and connected peers.

Unverified: the editor part on Windows; Source Control between two
computers; Start (`service install`) from the editor; the Local Network
prompt for the editor binary and for the service's bundle; scene reload
after a pull with the scene open; Make repository end to end on a complete
project.

## Revisit when

Source Control between two computers fails where loopback passes, or a
scene's conflicts need settling per entity in the window (Use
theirs and Keep mine settle a unit whole).
