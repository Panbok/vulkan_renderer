---
status: partial
updated: 2026-10-24
authority: adr
---

# ADR-134: The per-machine depot service

## Status

Accepted (partial). One background `vkr_depot service` per user and
machine serves every store the user lets others fetch from and push to, as
repositories that a version 2 connection selects with HELLO
([ADR-133](133-depot-repositories-remotes-and-merge.md)). Discovery
version 2 lists the served repositories and says when the machine is
outbound only. The service follows its settings file while it runs,
publishes its state for other processes, and syncs each served store with
each of its remotes that has an interval. The editor's Let others fetch and
push adds a repository to the service and starts the service when it does
not run ([ADR-110](110-project-sharing-in-the-editor.md)); the editor serves
nothing itself.

## Context

Before this decision a store was served by the editor that shared it, a
`vkr_depot serve` process, or a launchd job on the Mac. During the Level
Design Test transfer of 2026-10-23 the Windows server had been stopped for
five days without notice and stopped three times in one day, because a
server started from a shell stops with it and nothing restarts one after a
reboot. Each store needed its own port and key, and no surface showed
whether a server ran. The owner chose a separate service per machine that
the editor shows and never hosts (owner decision 3 of 2026-10-24 in the
proposal).

## Decision

### One server, several repositories

[vkr_depot_server.h](../../tools/depot/vkr_depot_server.h) serves up to
`VKR_DEPOT_SERVER_REPOSITORIES_MAX` (16) stores. Each repository keeps its
own access list, locks and object loader. The handshake accepts a key that
has a role on any repository; HELLO then selects one by its 16-byte ID and
checks the key's role there (`VKR_DEPOT_HELLO_OK`, `_UNKNOWN_REPOSITORY`,
`_NO_ROLE`, `_SELECT_REQUIRED`). A server of one store gives it to every
connection, so version 1 clients, which send no HELLO, keep working; a
server of several closes a request sent before a selection with
`VKR_DEPOT_CLOSE_SELECT_REPOSITORY`.

The protocol offers version 1 (the first 14 messages, with its schema hash
unchanged) beside version 2 (HELLO and HELLO_REPLY), so an older build still
connects ([vkr_depot_protocol.h](../../tools/depot/vkr_depot_protocol.h)).

Repositories change while the server runs:
`vkr_depot_server_add_repository` appends one, and
`vkr_depot_server_remove_repository` closes its connections with
`VKR_DEPOT_CLOSE_REPOSITORY_REMOVED`, stops its loader and moves the later
ones down one index, with the connections and known peers that refer to
them. Each repository is a separate allocation because its loader holds the
store's address.

The server remembers the last 64 keys that connected, with their display
name, live connections, selected repository, the outbound-only flag of their
HELLO and when they were last seen (`vkr_depot_server_peers`).

### Discovery version 2

The browser sends a version 1 and a version 2 probe, and a responder answers
each in its own version
([vkr_depot_discovery.h](../../tools/depot/vkr_depot_discovery.h)). The
version 2 answer adds a flags byte (bit 0: outbound only) and up to 8
repositories with ID, name and default branch, in at most 1400 bytes. A
version 1 reader still parses the version 1 answer. The service refreshes
the listed repositories every 5 seconds and at once after a change.

### The machine's service

[vkr_depot_service.h](../../tools/depot/vkr_depot_service.h) ("The
machine's service") owns the service folder, the user data folder's
`depot/`:

| File | Holds |
| --- | --- |
| `service.conf` | bind (default `0.0.0.0:7320`), discovery name and port, outbound only, and one `store NAME PATH` line per served store |
| `service.key` | the service's secret key; the first store added gives its `server.key`, so remotes that name that key stay valid |
| `service.status` | what the running service last published: address, repositories, known peers and the access requests of keys it refused |
| `bin/` (macOS: `VKR Depot.app/Contents/MacOS/`) | the installed copy of `vkr_depot`, so a rebuild never stops the service or locks its binary |
| `service.log` | what `service run` prints when it runs as the service |

The library functions take no running service: `vkr_depot_service_add`
(checks the store, gives it a repository ID, makes the user's identity its
admin when it has no access list, adopts its key when the service has none,
saves the settings), `vkr_depot_service_remove`, `vkr_depot_service_find`
(a store's entry, with the service's path comparison) and
`vkr_depot_service_status` (settings, key, a loopback discovery answer from
a service with that key, and the published peers and requests). A store's
access list is a file the running service rereads within a second, so
`vkr_depot_access_set` and `vkr_depot_access_list` answer requests without
a connection. The editor uses these instead of running the command.

[vkr_depot_service_cmd.c](../../tools/depot/vkr_depot_service_cmd.c) adds
what only the command does:

- `install` copies the binary into `bin/` and registers it at logon: on
  Windows a Task Scheduler task in the user's session that restarts a
  minute after any exit, plus one UAC prompt for a Defender rule bound to
  the installed binary (skipped with `--outbound-only`); on macOS a launchd
  agent `com.vkr.depot.service` kept alive, whose binary sits in an app
  bundle with an Info.plist that carries `NSLocalNetworkUsageDescription`,
  signed ad hoc with the identifier `com.vkr.depot.service`. macOS refuses
  local network connections of a bare binary that launchd starts, and keeps
  the user's Local Network answer for the signing identifier across
  installs. `--outbound-only` also writes the marker that makes this
  machine's clients say so in their HELLO.
- `run` serves the listed stores (none is allowed: the first `add` serves
  one), rereads `service.conf` each second and applies a change with
  `vkr_depot_service_apply` (stores only; bind, name, port and outbound only
  apply at start), and publishes `service.status` every 5 seconds when it
  changed.
- `run` syncs, on a thread of its own, each served store with each remote
  whose `interval` is set (`vkr_depot_repository_sync` from ADR-133): the
  first round right after start, then each interval. It logs pulled,
  pushed, diverged and failed branches, and a failing remote once until it
  works again. The thread reads the settings and remotes files itself and
  shares only the stores with the server, whose branches move under the
  store's lock.
- `add`, `remove`, `list`, `status` and `uninstall` wrap the library and
  the registration.

The task and the agent run in the user's session, so the store's process
lock (`Local\` on Windows) excludes the service and the user's own commits
alike.

## Consequences

- One port, one key and one discovery entry per machine, whichever stores
  it serves. A remote that names a store's old `server.key` keeps working
  when that store is the first one the service adopts.
- `service add` and `remove` take effect within a second on a running
  service; removing a store closes its connections.
- A process reads the running service's peers and requests from a file,
  not over a connection. `service.status` is at most 5 seconds old and is
  read only when the service answers discovery, so the editor also drops a
  request whose key already has a role in the store's access list.
- On macOS the service syncs with other machines only after the user allows
  VKR Depot under Local Network once.
- A peer's outbound-only flag comes from its HELLO, so a version 1 client
  is never shown as outbound only.
- A sync of a large store blocks only the sync thread; settings, publishing
  and serving go on.
- Display names of a user with a non-ASCII account name show as `?`
  characters (`????????????????????????@DESKTOP-DOVQPAO` on the Windows
  desktop): the credential keeps ASCII only.

## Alternatives considered

- **Keep `serve` per store, with its own port.** Rejected: each store needs
  a port, a key and a process that nothing restarts.
- **Restart the service to apply a settings change.** Rejected: it drops
  the transfers of every other repository.
- **A status request over the depot protocol.** Rejected for now: the
  status is local to the machine, and a file needs no new message or
  access rule.
- **A Windows service (SCM) instead of a logon task.** Rejected: it runs
  outside the user's session, where the user data folder, the identity key
  and the store lock name differ.

## Evidence

- CPU tests, `vulkan_renderer_tester --suite depot`, Release on the Windows
  desktop: 28 passed, among them `test_depot_hello`,
  `test_depot_repositories_served`, `test_depot_repositories_change` (a
  store added to a server of one, a removal that closes its connections and
  moves a later store's connection down one index),
  `test_depot_service_settings` (key adoption, settings round trip, apply
  of an added and a removed store, and a status that names the client's key
  as outbound only, under a temporary home) and
  `test_depot_discovery_versions`.
- Loopback interop between the saved version 1 binary and this build, both
  directions: clone, commit and clone again pass
  (`.scratch/depot-w1-compat.ps1`, local only).
- Windows desktop, 2026-10-24: windows-d runs as `vkr_depot service run`
  from the Release build with the D: store adopted (key `f9239cc6…`,
  unchanged). A scratch store added with `service add` and a remote on
  windows-d with a 5 second interval pulled `main 69ca5d5a4282` and both
  cooked branches without a command, stayed quiet on later rounds, and
  `service remove` stopped serving it within a second.
- Mac M1, 2026-10-24: `service install --outbound-only` runs the launchd
  agent; `service status` shows outbound only and the mac-m1 repository; the
  version 1 launchd mirror still syncs through it, and the CLI's fetch from
  origin and from windows works with HELLO (reported by the Mac session).
- Windows desktop, 2026-10-24, `service install --name windows-d`: the
  logon task registers, starts the installed copy below the non-ASCII
  profile (`C:\Users\Пользователь\AppData\Local\VKR\depot\bin`), and
  `service status` shows it registered and running. Three defects fixed on
  the way:
  - `vkr_platform_process_run` could not open `NUL` for output, so every
    `schtasks` and `netsh` call failed;
  - `run --log` reopened stdout with the CRT's ANSI `freopen`, which
    cannot open a path below that profile (now
    `file_reopen_standard_output`);
  - Defender had added Block rules for the installed binary when its
    "allow access" dialog was dismissed. A Block rule wins over the Allow
    rule, so the elevated step now removes every inbound rule for the
    binary before it adds its own.
- On the same machine the UAC prompt came back declined, and the firewall
  rule is added by hand with the two commands `install` prints.
- Mac M1, 2026-10-24: `vkr_depot` builds with no warnings with
  `file_reopen_standard_output`, and the installed service writes its log
  through it. The bare installed binary's interval sync logged "no answer
  from 192.168.3.2:7320 within 30000 ms" while the same binary run from a
  terminal fetched from windows-d, which led to the app bundle.
- `test_depot_service_settings` also checks that `service add` makes the
  user's identity the store's admin and that a refused key's request comes
  back through `service.status`; each check fails with its code removed.
- The editor on the service: see ADR-110's evidence.
- Mac M1, after the owner allowed VKR Depot under Local Network: the
  service logged "sync mac-m1 with windows works again", `main`,
  `main.cooked.astc` and `main.cooked.bc` equal windows-d's, and the
  launchd mirror and serve jobs were retired.

## Revisit when

- A peer needs the status over the network, or more than 16 repositories
  or 64 known keys per machine.
