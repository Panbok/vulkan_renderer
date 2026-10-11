---
name: vkr-cross-machine
description: Move VKR projects, scenes and assets between machines with the vkr_depot server (serve, discover, clone, commit, pull, mirror), coordinate with the agent session on the other machine, and diagnose LAN, firewall and endpoint-security failures. Use for any work that spans two machines, such as the Windows desktop and the Mac M1; single-machine depot code changes use the ADR alone.
---

# VKR across machines

[ADR-105](../../../docs/adr/105-network-transport-and-asset-depot.md#asset-depot)
defines the depot, discovery and mirror;
[ADR-106](../../../docs/adr/106-collaborative-editing-session.md#depot) defines
editor sessions on a depot. `vkr_depot` with no arguments prints the current
commands. This skill sets how two machines and their agent sessions use them.

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

- `keygen` prints the machine's identity key. `init STORE` prints the server
  key. Each store grants the other machine's identity with
  `access STORE KEY write`. The server reads the access list only when it
  starts, so restart a running server after `access`.
- Keep the Mac store small: the Mac disk runs near full. Bulk history
  belongs on the Windows D: store.
- Run the server from a stable copy of the binary, not from `build_*`. On
  macOS, a rebuilt binary has a new ad hoc signature, so a firewall rule for
  the old one may not match it. Example:
  `cp build_release/tools/depot/vkr_depot ~/vkr-depot/bin/` and
  `serve ~/vkr-depot/store --bind 0.0.0.0:7320 --name mac-m1`. The depot
  uses UDP 7320; discovery uses UDP 7319.
- After you pull `main` with new depot code, rebuild, copy and restart the
  server, and post the restart.

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

- `vkr_depot` snapshots every file under the working copy root and has no
  ignore rules. Never make the repository, a build folder or a whole
  `.vkreditor` folder a working copy. Keep a separate working copy with
  workspace-relative paths.
- A project scene lives at
  `.vkreditor/projects/<project>/scenes/<scene>/`. Its folder keeps every
  old edit overlay and every old build, because the editor deletes neither.
  Only the files named by `scene.json` and its inventory are live.
- The editor reads `.vkreditor/...` from `--workspace` and `assets/...`
  from the content root, which is the repository in a development build.
  Terrain heightfields named `assets/terrain/<id>.vkrhf` live in
  `.vkreditor/assets/terrain/`. Generated meshes, materials and textures
  under `assets/` are often untracked or ignored by git, so the other
  machine lacks them. Until the editor also looks for `assets/...` in the
  workspace, copy those folders into the receiving repository after a pull.
- `project.json` stores editor layouts and camera state. Each editor session
  rewrites it, so expect a conflict on it whenever both machines open the
  project. Pull before you open the project, and commit soon after you save.

Stage a scene with [scripts/stage_scene.py](scripts/stage_scene.py). It
copies the live set with its untracked asset closure and exits 1 when a
referenced asset is missing. Run it from the repository root:

```sh
python3 -I .codex/skills/vkr-cross-machine/scripts/stage_scene.py \
    ~/vkr-depot/work --project <id> --scene <id> --dry-run
```

Read the list, run it without `--dry-run`, then `commit WORK -m "<message>"`.

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
