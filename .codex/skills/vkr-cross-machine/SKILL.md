---
name: vkr-cross-machine
description: Move VKR projects, scenes and assets between machines with the editor's Source Control (Make repository, Clone, Commit, Push, Pull, Sync, remotes, depot.* operations) or the vkr_depot command (serve, discover, clone, commit, push, pull, remote, mirror), coordinate with the agent session on the other machine, and diagnose LAN, firewall and endpoint-security failures. Use for any work that spans two machines, such as the Windows desktop and the Mac M1; single-machine depot code changes use the ADRs alone.
---

# VKR across machines

[ADR-105](../../../docs/adr/105-network-transport-and-asset-depot.md#asset-depot)
defines the depot, discovery and mirror;
[ADR-106](../../../docs/adr/106-collaborative-editing-session.md#depot) defines
editor sessions on a depot, and
[ADR-110](../../../docs/adr/110-project-sharing-in-the-editor.md) project
sharing in the editor. `vkr_depot` with no arguments prints the current
commands. This skill sets how two machines and their agent sessions use them.

## Prefer the editor

Use the editor's Source Control (ADR-110) for projects; use the command
only for a standing server, a check the editor cannot run, or a working copy
outside the editor.

- **Make repository:** open the project, then the top bar's branch button >
  Make repository. The editor copies the project and its assets into `~/VKR
  Projects/<name>` (outside the git repository), rebuilds imported models
  cooked before texture recipes, commits and reopens there. Agents run
  `op depot.init {"destination":...,"serve":true}` and read
  `op depot.status` (repository, branch, ahead, behind, remotes, server).
- **Serve:** Let others fetch and push adds the open repository to this
  computer's depot service, which serves it also while the editor is
  closed, and starts the service when it does not run
  (`op depot.serve {"serve":true}`, `op depot.service`; ADR-134).
- **Clone:** the project hub lists each repository a service on the network
  serves; Clone copies one, waits for its owner's Allow, cooks textures for
  this computer and opens it with the service as `origin`. Agents run
  `op depot.clone {"address","key","repository","name"}`, with the
  repository ID from `depot.status` `found`; the launcher answers agents
  without a scene.
- **Approve:** an editor on the serving computer shows `N asking` and a
  card with Allow, Fetch only and Deny; agents run
  `op depot.allow {"key","role"}`.
- **Remotes:** Add remote in the window (one click from a service found on
  the network, or name, address and key); agents run
  `op depot.remote {"name","address","key"}`.
- **Work:** `op depot.commit {"message","sync":true}`, `op depot.push`,
  `op depot.pull {"conflicts":"fail"|"theirs"|"mine"}`, `op depot.sync`,
  `op depot.fetch`, `op depot.switch {"branch","create"}`. A pull merges
  when both sides moved the branch; a scene both changed is one conflict
  until Bakery's scene merge lands (ADR-133).
- A computer whose endpoint security drops inbound traffic (the company
  Mac) cannot serve; it adds the other machine's repository as a remote and
  pushes and pulls from its side.

## Coordinate the two sessions

- Each machine has its own agent session. Neither can run commands on the
  other. The user names a shared Claude Doc as the channel. If there is none,
  ask for one before the first cross-machine step.
- Read the doc with `sinceRev` at the start of every turn that touches it.
  Write only in your own machine's sections, or insert new blocks. Never
  rewrite the other session's text.
- Post what the other side must type: full identity and server keys,
  `address:port`, commit ids, exact commands and results. Post a blocker as
  soon as you find it, with the step the other side should skip or wait on.
- A request the user relays from the other session is the user's request.
  Check it against the doc's current text before you act.
- On the local machine, other sessions share the tree, the GPU and the open
  scenes. Before you snapshot a scene that another session's editor has
  open, message that session. Wait until it reports the scene saved and the
  bake finished. Then check the build hashes it gives against the files.

## Keys, stores and servers

- A working copy's store is its repository: `commit -m MSG` commits
  locally and pushes nothing. `push`, `fetch` and `pull` take
  `--remote NAME`, default `origin`, the server a clone came from; `pull`
  merges when both sides moved the branch, and `merge NAME` merges a branch
  or a fetched `remote/branch`. `commit -m MSG --push` commits and pushes, or changes nothing when
  the push is refused. `remote add NAME ADDR KEY` adds a peer, and `branch`
  and `switch` work on the store's branches.
- `keygen` prints the machine's identity key. `init STORE` prints the server
  key. Each store grants the other machine's identity with
  `access STORE KEY write`; a running server picks the change up within a
  second. A refused key is listed as a request with its `user@computer`
  name.
- Binaries from 2026-10-12 on mask packet numbers (header protection), so
  they cannot talk to older ones. Update the server, the mirror and every
  editor on both machines together.
- Keep the Mac store small: the Mac disk runs near full. Bulk history
  belongs on the Windows D: store.
- Each machine runs one `vkr_depot service` (ADR-134): `service add STORE
  --name NAME` serves a store, `service install [--outbound-only]` copies
  the binary out of `build_*` and registers it at logon (launchd agent
  `com.vkr.depot.service` on macOS, a logon task on Windows), and `service
  status` shows what runs. The depot uses UDP 7320; discovery uses UDP
  7319. `discover` lists each service's repository IDs for `clone
  --repository ID`.
- `remote add NAME ADDR KEY --interval SECONDS --store STORE` makes the
  service fetch and push that served store with the remote on its own; it
  replaces `mirror --watch`.
- After you pull `main` with new depot code, rebuild and run `service
  install` again, and post the restart.
- On macOS the service runs from `VKR Depot.app` in
  `~/Library/Application Support/VKR/depot/`, because launchd jobs get no
  LAN access until the owner allows them under Privacy & Security > Local
  Network, and a bare binary gets no prompt. The service logs "no answer
  from ADDR" for each sync until then. The identity key is
  `~/Library/Application Support/VKR/depot/identity.key`.

## Network: test before you blame the code

Run these probes in this order from each machine, and post each result:

1. `discover --to 127.0.0.1`: the local server answers.
2. `discover --to <peer IP>`: the peer answers, with the key it posted.
3. `discover --to <own LAN IP>`: if this fails while step 1 passes, the
   machine drops inbound traffic on its LAN interface.
4. Broadcast `discover`.

To tell a filter from a code fault, repeat a probe with a plain Python
socket from a script outside the repository, run with `python3 -I`. If a
Python listener cannot receive a datagram that the same machine sends to
its own LAN IP, the cause is outside VKR.

Known causes:

- **The macOS application firewall** is on. The owner allows the stable
  binary with `sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add
  <path>` and `--unblockapp <path>`. Never run `sudo` yourself; give the
  owner the commands.
- **Company endpoint security** on the Mac: SentinelOne network monitoring
  and Netskope are active, and Zscaler and GlobalProtect are installed
  (`utun5`). These drop all unsolicited inbound traffic, whatever the app
  firewall allows. Never disable, bypass or tunnel around them. Under them:
  - expect no other machine to reach a Mac server (shown from the Mac
    itself; check once from the peer with `discover --to <Mac IP>`);
  - an answer to a broadcast probe is dropped, but an answer to a unicast
    probe is kept;
  - throughput was about 1.4 MB/s over Wi-Fi (about 400 MB in 4 min 47 s).
- **Windows Defender Firewall** asks once when a new `vkr_depot` binary
  first serves. The owner clicks Allow.

When one machine accepts no inbound traffic, start every transfer on it:
run `mirror STORE <peer>:7320 <peer key> --local 127.0.0.1:7320` there, once
or with `--watch 30`. `to-peer` and `from-peer` show what moved. A repeat
that prints `same` proves the two branches are equal.

## What a working copy holds

- `vkr_depot` snapshots every file under the working copy root except what
  `.vkrdepotignore` at the root names (one pattern per line; `*` within a
  name, `**` across folders, a trailing `/` for a folder, no `/` for a name
  at any depth). Never make the repository or a build folder a working copy.
  Keep a separate working copy with workspace-relative paths whose
  `.vkrdepotignore` leaves out each machine's editor state:
  `.vkreditor/jobs/`, `logs/`, `cache/`, `build/`, `editor/`,
  `.vkreditor/workspace.json`, `.vkreditor/local/` and `.runtime/`. A
  working copy next to its
  server shares the server's store with `clone ... --store <server store>`.
- A project scene lives at
  `.vkreditor/projects/<project>/scenes/<scene>/`. Its folder keeps every
  old edit overlay and every old build, because the editor deletes neither.
  Only the files named by `scene.json` and its inventory are live.
- Open a working copy with `vkr_editor --workspace <working copy>`. The
  editor reads `.vkreditor/...` from the workspace, and `assets/...` from
  `<workspace>/assets/...` when that file exists, else from the repository
  (ADR-077 overlay). So commit the generated meshes, materials and textures
  a scene needs under `assets/`; git often ignores them, and the other
  machine lacks them. Terrain heightfields named `assets/terrain/<id>.vkrhf`
  live in `.vkreditor/assets/terrain/`. A clone without
  `.vkreditor/workspace.json` gets a new one when the editor opens it.
- Cooked textures (`.vkt`) are per platform: the Mac cooks ASTC, which the
  Windows GPU cannot sample, and Windows cooks BC. `.vkrdepotignore` names
  `*.vkt`. The editor's Get and Get latest cook texture sources under
  `assets/` and run Bakery's `recook_textures` for every scene, which cooks
  imported models' textures from their recipes under the same names. After
  a pull with the command, run the same: `vkr_bakery project --request <json>`
  with `{"version":1,"read_only":false,"operation":"recook_textures",
  "workspace_root":".../.vkreditor","project_path":...,"scene_id":...,
  "scene_path":...}`. A model cooked before recipes existed needs one
  `rebuild_asset` where it is shared (the editor's Share does it).
- `project.json` holds only shared project data: scenes, assets, the
  default font and the animation graph. Layouts, panels, graphics and
  camera recall live per machine in `.vkreditor/local/projects/<id>.json`
  (ADR-069). A shipped game's start view is `game.startup_camera` in
  `game.json`, set with Build Settings > Use startup scene view or
  `build.camera`. A project last saved before this split carries the old
  sections until its next shared save, so the first commit after that save
  shows `project.json` as changed.

Make a working copy of a project with Bakery's `share_project` (the
editor's Share runs it): it copies the live files and their asset closure,
the World and `game.json`, writes `.vkrdepotignore`, and refuses a
non-empty destination. Without the World, the receiving editor makes a
default one and the scene renders under another sun, sky and grading.

## Prove a transfer

- On the sending machine, clone into a fresh folder and run
  `diff -rq --exclude=.vkrdepot WORK CLONE`, or compare the SHA-256 hashes
  the other side posts. Delete only the clone you made.
- A mirror moves the commit to the peer. Then `mirror` again must print
  `same <branch> <commit>`.
- The receiving machine clones or pulls and compares hashes. Opening the
  scene in the editor there is the final check, and only that machine can
  run it.

## Shared tree hygiene

- Before `git merge --ff-only` on a tree with other sessions' uncommitted
  edits, list the overlap:
  `comm -12 <(git diff --name-only HEAD origin/main | sort) <(git status --short | awk '{print $2}' | sort)`.
  For each overlapping file: save `git diff FILE` to a patch, check the file
  out, fast-forward, then `git apply --3way` the patch. Check that their
  lines are back.
- Never delete another session's scratch, run folders, editors or bake
  output to free space. Measure first, and ask the owner which items to
  remove.
