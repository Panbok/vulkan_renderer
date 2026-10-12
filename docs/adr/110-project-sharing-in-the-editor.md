---
status: partial
updated: 2026-10-12
authority: adr
---

# ADR-110: Project sharing in the editor

## Status

Accepted (partial). The editor shares a project on the local network, gets
a project another editor shares, submits, gets the latest changes, settles
conflicts and approves access, with no command line and no settings to
type. Every step ran on one Mac M1 between editors with separate identities
over loopback. The Windows build of the editor part, a run between two
computers and the macOS Local Network prompt for the editor are unverified.

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

## Decision

[editor_depot.c](../../editor/src/editor_depot.c) owns sharing. One worker
thread runs one step at a time (status, submit, get latest, get, share); the
server runs on its own thread
([vkr_depot_service.h](../../tools/depot/vkr_depot_service.h)). The UI
thread reads finished results and progress counters only.

### One workspace per shared project

- **Share** saves nothing itself and refuses while a scene has unsaved
  edits. It runs Bakery's `share_project` into `~/VKR Projects/<name>` (or
  a folder the user names), rebuilds each model `needs_rebuild` lists,
  removes the builds that replaced, makes the folder a depot working copy
  whose store is `.vkrdepot/store` with a server key, and reopens the editor
  there (`vkr_editor_projects_open_workspace`). The first commit, `Shared
  <name>`, follows once the server runs.
- A working copy whose store holds `server.key` is this editor's to serve:
  opening it starts the server again. Any other working copy with a remote is
  a copy of someone else's project. Each shared or copied workspace is
  listed in `<user data>/VKR/depot.json` for the project hub.
- New materials, graphs and themes in a shared workspace are written to its
  own `assets/`, so the project carries them (ADR-077).

### Server and access

- The service binds `[::]:7320` or the next free port, skipping a port
  another socket holds for IPv4, answers discovery on UDP 7319 when that
  port is free with the project's name and the computer's name, and uses
  memory of its own (8 MB, growing to 1 GB). The editor reaches its own
  server over the loopback of the family it bound.
- The store's owner is the editor's identity. A computer the server does
  not know is refused at the handshake and recorded with the name it sent
  (`user@computer`); the top bar shows `N asking`, and a card offers Allow,
  View only and Deny, with the first eight digits of the asking key; the
  waiting computer shows its own, so two people can tell a real request from
  an impostor that sends the same name. The waiting computer retries every
  3 s and continues on its own once allowed. Names of allowed computers are
  kept for the access list.
- The identity key lives in the user data folder, not the cache
  (`vkr_depot_identity_path`).

### Getting a project

- The project hub lists shared workspaces on this computer and projects
  found on the network: a broadcast every 2 s plus a direct probe of every
  server used before, since some endpoint security drops answers to
  broadcasts but not to direct probes. A field takes a computer name or
  address for a server discovery misses.
- Get clones into `~/VKR Projects/<name>`, waits for approval if needed,
  cooks every texture source without a cooked file for this computer,
  re-cooks each scene's build textures and the project's own
  (`recook_textures`), then opens the project.
- A copy whose server moved to another address follows its key through
  discovery and saves the new address.
- Discovery is not authenticated: each found project shows its key's first
  digits and whether it was used before, and an answer with a known
  project's name but another key says it is not that computer.

### Scripts

Scripts are native code. A project copied from another computer does not
build or run its scripts until its user chooses Run scripts (a card, the
Project sharing window or `depot.trust`); a pull that changes a `Scripts/`
folder holds them again until the user trusts the change. The project's
own sharer trusts its scripts. Built libraries never travel: the
workspace's `.vkrdepotignore` names `.vkreditor/scripts/`, and checkout
writes no ignored path (ADR-105).

### Changes

- A status step every 20 s counts local changes (a snapshot against the
  index) and reads the server's branch. The top bar button shows Share,
  `N changes`, `New changes`, `Conflicts`, `Waiting for access`, `Offline`,
  `Shared` or `Synced`, and opens the Project sharing window.
- **Submit** needs saved scenes. It first takes a newer commit from the
  server, keeping local changes, then snapshots, uploads and moves the
  branch by compare-and-swap.
- **Get latest** fetches and checks out, cooks and re-cooks the textures it
  brought, rescans materials when a material changed, and reloads the open
  scene once its edits are saved. A file changed here and there stops with
  the conflicts listed; Use theirs and Keep mine settle all of them.
- History lists the latest commits of the local store with their age.

### Agents and packaging

- `depot.status`, `depot.share`, `depot.submit`, `depot.pull`,
  `depot.clone`, `depot.allow`, `depot.find` and `depot.cancel` join the
  agent table ([editor_ops.c](../../editor/src/editor_ops.c)) and need no
  scene; the launcher serves the agent channel and sharing too.
- On macOS the `vkr_editor` and `vkr_depot` executables carry an Info.plist
  with `NSLocalNetworkUsageDescription` in `__TEXT,__info_plist`
  ([vkr_info_plist.cmake](../../cmake/vkr_info_plist.cmake)), so the Local
  Network prompt names the program and says why. Windows Defender Firewall
  asks once when the editor first serves; the Share text says to choose
  Allow.

## Consequences

- Sharing and collaboration use one working copy; a collaborative session
  ([ADR-106](106-collaborative-editing-session.md)) on a shared project
  bases on its depot.
- A shared project lives outside the repository. Editing the original after
  sharing does not reach the shared copy.
- The project is reachable only while its editor runs. A computer whose
  endpoint security drops unsolicited inbound traffic cannot share; it gets
  and submits through another computer's editor or `vkr_depot serve`.
- Two editors on one computer: the second shares on the next port without
  discovery while the first holds UDP 7319.

## Alternatives considered

- **Track the project inside the current workspace** with include rules:
  every commit would need the closure walk, and pulls would land in the
  repository.
- **A hidden staging copy:** each submit copies and diffs the closure, and
  pulls land where nobody edits.
- **A background login item for the server:** always reachable, but a
  second process and its own Local Network approval; declined 2026-10-11.
- **Open access on the network, or a pairing code:** declined for approval
  on the server.
- **Shipping cooked textures, or rebuilding a model on each receiver:**
  unusable across GPU families, or a new revision on every machine that
  then travels back.

## Evidence

Mac M1, Release, 2026-10-12, headless editors driven through the agent
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
  copy after Get (`scripts_held`) and built only after `depot.trust`.

Unverified: the editor part on Windows; a share between two computers; the
Local Network prompt for the editor binary; scene reload after a pull with
the scene open.

## Revisit when

A share between two computers fails where loopback passes, users need a
project reachable while its editor is closed, or a merge of scene files is
needed instead of Keep mine and Use theirs.
