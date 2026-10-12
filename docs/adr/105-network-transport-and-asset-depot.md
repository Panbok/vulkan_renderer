---
status: partial
updated: 2026-10-12
authority: adr
---

# ADR-105: Network transport, data format and asset depot

## Status

Accepted (partial). Phases 0 to 3 of the
[network protocol proposal](../proposals/network-protocol.md) are
implemented: the encrypted UDP transport, the binary data format, services
over sessions, and the asset depot with its server, command, LAN discovery
and server-to-server mirroring. The checks ran on Windows (Ryzen 5 2600,
clang), and the depot checks also on a Mac M1. On 2026-10-11 the depot moved
a 1.46 GB project scene between the two machines in both directions
([Evidence](#mac-m1-and-windows)). The proposal's cross-machine throughput
and CPU targets are not measured. On 2026-10-12 the transport gained a
host thread, header protection and pacing and background-mode fixes proven
on a shared simulator bottleneck; the depot gained access requests granted
at runtime, a server service thread, non-blocking discovery, library steps
with progress, cancel and conflict policies; four fuzzers ran under ASan and
UBSan. [ADR-106](106-collaborative-editing-session.md) builds editor
collaboration on it, and [ADR-110](110-project-sharing-in-the-editor.md)
project sharing in the editor. The proposal keeps the rest of editor collaboration, agent
federation, game sessions, asset and view streaming,
scale and the open items listed under [Gaps](#gaps).

## Context

The engine had local AF_UNIX sockets only
([ADR-084](084-agent-channel-and-level-design-toolkit.md)). The owner asked
for one UDP protocol for asset transfer, multiplayer, collaborative editing,
streaming and agent federation, with no text format on the wire.

Owner decisions:
- **2026-10-09:** libsodium, vendored; a custom transport instead of a QUIC
  library; the depot's own commits, trees, refs and locks (Git LFS speaks
  JSON); Windows and macOS servers, Linux later.
- **2026-10-09:** no textual format crosses the wire. Asset bytes travel raw;
  everything else is a strict binary schema.

## Decision

### Libraries

| Library | Path | Contents |
|---|---|---|
| `vkr_foundation` | [vkr_udp_socket.h](../../lib/src/platform/vkr_udp_socket.h) | Nonblocking dual-stack UDP with don't-fragment, batch calls and address text; Winsock starts once through [vkr_winsock.h](../../lib/src/platform/vkr_winsock.h) for local and UDP sockets |
| `vkr_sodium` | [vkr_libsodium.cmake](../../cmake/vkr_libsodium.cmake) | libsodium 1.0.22 ([vendor/libsodium.md](../../vendor/libsodium.md)) |
| `vkr_net` | [net/src](../../net/src) | Transport core, crypto, congestion control, simulator, UDP host, bit stream, wire codec, sessions |
| `vkr_bakery_os` | [vkr_bakery_os.h](../../tools/bakery/vkr_bakery_os.h) | Bakery's file-system helpers, now a library the depot links |
| `vkr_depot_lib` | [tools/depot](../../tools/depot) | Objects, store, working copies, depot service, server and client |

`vkr_net` depends only on the foundation and libsodium. The depot takes a
codec instead of linking zstd: `vkr_depot` links its own copy of the zstd
libktx carries, and processes with libktx (the tester) adapt that one
([vkr_depot_zstd.c](../../tools/depot/vkr_depot_zstd.c)).

### Transport

The core ([vkr_net_core.h](../../net/src/vkr_net_core.h)) is sans-I/O: it
takes datagrams and a time and returns datagrams and a deadline. The host
([vkr_net_host.h](../../net/src/vkr_net_host.h)) pumps it on one UDP socket
from the caller's thread, or from a thread of its own
(`vkr_net_host_thread_start`): that thread waits on the socket without the
lock until the core's deadline, a datagram, a wake or 100 ms, then pumps and
runs the caller's step with the lock held; any other thread takes the lock
to touch the core, and its unlock wakes the thread. Waits shorter than a
millisecond use `select` on POSIX; Windows rounds them up to 1 ms.

- **Packets.** A short header is a flags byte (bit 6 set, bits 1–0 the
  packet-number length), the receiver's 32-bit connection ID and 1 to 4
  bytes of the packet number, all authenticated as AES-256-GCM associated
  data; the nonce is a per-direction IV XOR the 64-bit packet number.
  Header protection (RFC 9001 5.4.4) masks the length bits and the
  packet-number bytes with ChaCha20 under a per-direction key from the
  handshake split, sampled from the ciphertext; packets pad to 25 bytes so
  a sample exists, and the connection ID stays clear. libsodium exposes no
  raw AES block, so the AES-based mask was not used. A
  connection ID holds a 4-bit shard, 12 random bits and the 16-bit slot.
- **Frames.** The first byte selects the frame: `0x00`–`0x3f` basic frames
  (PADDING, PING, ACK with ranges, WINDOW, BLOCKED, CANCEL, PATH_CHALLENGE,
  PATH_RESPONSE, CLOSE), `0x40`–`0x7f` MESSAGE with LEN, SEQ, FRAG and TAG
  flags, and `0xc0`–`0xff` extension frames with a length that a receiver
  without them skips. Integers in frames use the QUIC varint.
- **Handshake.** Noise `IK` with X25519, ChaCha20-Poly1305 and BLAKE2b,
  checked against the cacophony vector. The client's first packet is padded
  to 1,200 bytes and carries a credential up to 1,024 bytes; the server's
  accept callback refuses with a close code or accepts with a payload and a
  user value. A server demands a stateless retry cookie when configured or
  when 256 handshakes are half-open, and sends at most three times the bytes
  it received to an unvalidated address. There is no 0-RTT data. The first message carries the client's
  Unix time in microseconds; a server drops one not newer than the last it
  accepted from that static key (1,024 keys remembered), so a captured
  handshake cannot be replayed. A server connection gets its packet tables
  and reaches the application (CONNECTED) only with the client's first
  authenticated packet; the retry threshold defaults to an eighth of the
  connection limit (at least 4). Protocol version 2.
- **Delivery.** Up to 64 channels per connection; channel 0 is the reliable
  ordered session channel. Classes: unreliable, sequenced, reliable ordered,
  reliable unordered and deadline (dropped and cancelled after its time).
  Reliable messages up to 64 MiB split into fixed fragments at send time.
  Each channel has priority 0–7, weight, send queue and receive window;
  WINDOW frames raise the sender's limit as the application consumes.
  Data for a channel this side closed is dropped. Reassembly is capped at
  256 MiB per connection (a channel opens only when its window and largest
  message fit) and 1 GiB per core; past it the connection closes with
  RESOURCES (13). One ACK frame per packet.
- **Recovery.** RFC 9002 loss detection with packet and time thresholds that
  grow when an acknowledgment proves a declared loss spurious, probe
  timeouts, ACK ranges pruned once acknowledged, and a 2,048-packet replay
  window. Frames, not packets, are retransmitted.
- **Congestion control** ([vkr_net_cc.h](../../net/src/vkr_net_cc.h)): a
  BBR-style model with a 10-round bandwidth filter and 10-second minimum
  RTT, STARTUP/DRAIN/PROBE_BW/PROBE_RTT, a loss cap above 2% per round,
  pacing at the set rate with 10-packet bursts (debt is never forgiven, and
  a send needs no whole datagram of tokens), STARTUP gain 2.0, and a
  background mode that scales pacing as queueing delay, measured against
  the path's lifetime minimum RTT plus 5 ms, approaches 25 ms; a background
  flow that yielded restarts STARTUP once the link is free. PROBE_RTT
  samples count as app-limited.
- **Path.** PMTU discovery probes 1,452 bytes and then the configured
  maximum (8,952 for jumbo frames); lost probes do not count as congestion.
  A packet from a new peer address only makes it a candidate; the
  connection moves when that address answers PATH_CHALLENGE.
- **Lifetime.** Idle timeout with keep-alives at a third of it, CLOSE with a
  numeric code, and a drain period of three probe timeouts. A session keeps
  state only for a connection it saw connect and not close.

### Data format and sessions

- [vkr_bitstream.h](../../net/src/vkr_bitstream.h) packs least significant
  bit first and refuses overruns and nonzero padding.
- [vkr_wire.h](../../net/src/vkr_wire.h): C descriptor tables, one
  table-driven codec, fields in order without tags, an exact schema whose
  decoder refuses any bound violation, non-canonical varint, invalid UTF-8,
  NaN or trailing byte, and a 64-bit schema hash over IDs, kinds, widths,
  capacities, ranges and order (not names or offsets).
- [vkr_net_session.h](../../net/src/vkr_net_session.h): SERVICE_OPEN,
  ACCEPT, REFUSE and CLOSE on channel 0, encoded with the wire codec. The
  acceptor picks the highest version both sides list with the same schema
  hash and refuses unknown services, schema mismatches, differing channels
  or its own veto.

### Asset depot

- **Objects** ([vkr_depot_object.h](../../tools/depot/vkr_depot_object.h)):
  chunks, blob manifests, trees and commits, named by the SHA-256 of their
  bytes, in a canonical binary form whose parser, like the encoder, refuses
  unsorted, invalid or case-colliding names. A valid name is not `.vkrdepot`
  or `.git` in any case, holds none of `: < > " | ? *` or a control
  character, is no Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9,
  with any extension), is well-formed UTF-8 without combining marks (NFC),
  and compares by one folding key (ASCII, Latin-1, Latin Extended-A, Greek
  and Cyrillic) that trees, checkout and locks share. A tree is at most 64
  folders deep, 1,000,000 files and 512 GiB; a file at most 4 GiB. The
  server refuses a push that breaks a rule (`VKR_DEPOT_REF_INVALID`).
- **Store** ([vkr_depot_store.h](../../tools/depot/vkr_depot_store.h)): a
  content-addressed directory; each file is a 5-byte header (raw or zstd,
  decoded size) and the payload, kept compressed when that saves a tenth.
  Every write checks the hash and renames a temporary. Object writes are
  not flushed (owner decision, 2026-10-11): a flush per object cost a seek
  each on the Windows store's hard disk, and a 1.46 GB scene cloned in
  4 min 33 s; without it, 1 min 29 s. On Windows the renames also skip
  write-through (`file_rename_unflushed`), and the clone takes 1 min 8 s;
  a working copy sharing the server's store checks the scene out in 16 s. Every read checks the hash instead
  and removes a file a crash left torn, so it counts as missing and is
  fetched again; branch moves stay flushed. Files split into 64 KiB–1 MiB
  chunks by FastCDC with a fixed gear table.
- **Working copies**
  ([vkr_depot_workspace.h](../../tools/depot/vkr_depot_workspace.h)):
  `.vkrdepot/` holds the state (remote, server key, branch, base commit,
  index). A snapshot hashes only files whose size or time changed.
  Checkout writes each file through a temporary and a rename without
  flushing it (owner decision, 2026-10-11): its index entry carries time -1,
  so the next snapshot hashes it once, and a file a crash tore with its size
  and time intact shows as a change instead of passing as current. A later
  checkout leaves such a file alone when its size and blob match, and
  `status` saves the times it verified. Checkout reads `.vkrdepotignore` as
  it stands before it starts and never writes or removes an ignored path,
  so a commit cannot plant per-machine files such as built script
  libraries; ignore rules apply to untracked files only, as in git. It
  removes before it writes, so a rename that changes only case keeps the
  file. Index paths that fail validation when the state loads are dropped.
- **Service** ([vkr_depot_protocol.h](../../tools/depot/vkr_depot_protocol.h)):
  schema `vkr.depot` version 1 on a reliable ordered control channel; stored
  representations as raw messages on a reliable unordered priority-7
  channel tagged `(request << 8) | index`.
- **Server** ([vkr_depot_server.h](../../tools/depot/vkr_depot_server.h)):
  refs move only by compare-and-swap after the server checks the commit's
  objects are present and that it changes no path another key has locked;
  lock paths compare by the folding key. Keys need a role (read, write,
  admin) in the store's access list, looked up again when the transport
  reports the connection. A key without one is refused at the
  handshake and remembered, with the display name it sent as its credential
  (`user@computer`), as one of the last eight access requests; giving it a
  role at runtime (`vkr_depot_server_set_role`) writes the access file and
  lets its next try in, and NONE closes its connections and keeps the key
  out of the requests (256 refused keys until restart). One address holds
  at most two requests. The server reloads an access file changed on disk
  about once a second. Each peer may hold 128 MiB of object bytes ahead of
  their PUT and 8,192 queued GET replies; past them it is closed or told
  the rest is missing. Its server key and the identity key are written
  readable by their owner only.
- **Service thread** ([vkr_depot_service.h](../../tools/depot/vkr_depot_service.h)):
  the store, server, session, UDP host and discovery responder on a host
  thread, safe to call from any thread. It binds `[::]:7320` or the next of
  eight ports, treating a port another socket holds for IPv4 as taken, and
  makes the local identity the admin of a store without an access file
  when asked. `serve` and the sharing editor run it.
- **Client** ([vkr_depot_client.h](../../tools/depot/vkr_depot_client.h))
  fetches a commit's missing objects with pipelined GETs and uploads the
  objects a commit adds over its base with HAVE and PUT.
- **Sync** ([vkr_depot_sync.h](../../tools/depot/vkr_depot_sync.h)): the
  user's identity key, kept in the user data folder
  (`~/Library/Application Support/VKR/depot/identity.key`,
  `%LOCALAPPDATA%\VKR\depot\identity.key`; copied once from the cache
  folder that held it before), a client connection over its own UDP host
  whose status tells an unreachable server from one that has not allowed
  this key yet, clone (resumable), status, log, branch list, commit and push
  to a branch from an expected commit, and fetch and checkout that keeps
  local changes. A path changed on both sides to other content is a
  conflict that FAIL, TAKE_THEIRS or KEEP_MINE settles. Snapshot, upload,
  fetch and checkout report through
  [progress counters](../../tools/depot/vkr_depot_progress.h) another
  thread reads, and stop when it cancels, leaving the working copy valid.
  The command, the collaborative session
  ([ADR-106](106-collaborative-editing-session.md#depot)) and project
  sharing ([ADR-110](110-project-sharing-in-the-editor.md)) use it.
- **Mirror** (`vkr_depot_mirror`): two machines each serve a full store and
  keep their branches equal (owner decision, 2026-10-11). One process
  connects as a client to the local server and to the peer and moves
  objects through the local server's store, which tolerates concurrent
  writers. A branch on one side only is created on the other; a branch one
  side moved past the other's commit fast-forwards there by the server's
  compare-and-swap. A branch both sides moved is reported as diverged and
  left alone, since the depot has no merge.
- **Discovery** ([vkr_depot_discovery.h](../../tools/depot/vkr_depot_discovery.h)):
  `serve` answers binary probes on UDP 7319 with its service port, server
  key, name and host. `discover` broadcasts a probe to every IPv4 subnet
  the machine is on, or sends it to `--to`, and lists the answers. An
  answer is unauthenticated; the transport then proves the key, so the key
  is checked once against the one `serve` prints. A browser
  (`VkrDepotBrowser`) never blocks: it probes the broadcast addresses and a
  list of addresses one by one, and collects answers each frame, since some
  endpoint security drops answers to a broadcast but not to a direct probe.
- **Ignore rules**: `.vkrdepotignore` at a working copy's root keeps paths
  out of snapshots, so an editor workspace commits its project and assets
  but not each machine's jobs, logs, caches and `workspace.json`. A working
  copy beside the server shares its store with `clone --store`. Lightmap
  sets (`.vklm`) travel: each machine bakes and commits the set of its own
  pipeline class, and the scene names both
  ([ADR-088](088-baked-lightmap-sets.md#bakery-and-projects)).
- **Command** ([vkr_depot_main.c](../../tools/depot/vkr_depot_main.c)):
  `keygen`, `init`, `serve`, `discover`, `mirror` (once or `--watch`),
  `access` (takes effect without a restart), `clone`, `status`, `commit`
  (snapshot, upload, move the branch), `pull` (keeps local changes, refuses
  conflicts unless `--take-theirs` or `--keep-mine`), `refs`, `log`,
  `lock`, `unlock`, `locks`. On macOS `vkr_depot` carries an Info.plist with
  a Local Network usage string.
- **Fuzzers** ([tests/fuzz](../../tests/fuzz)): the server core's receive
  path with decrypted frames under the real keys, the wire codec and bit
  stream, sessions, and depot objects, protocol messages and discovery
  datagrams, built with `VKR_BUILD_FUZZERS` and run by
  [run_fuzzers.py](../../tools/checks/run_fuzzers.py) under ASan and UBSan.

## Consequences

- Every networked feature builds on one handshake, one congestion
  controller and one data format; a feature adds a service, not a protocol.
- Nothing text-based crosses the wire. JSON stays on the local agent socket.
- The project owns the transport's security and must keep the fuzzing and
  review gates.
- On Windows and macOS each datagram costs one system call; on this host the
  transport moves about as many packets as raw UDP does.
- A machine whose endpoint security drops unsolicited inbound datagrams
  cannot serve peers or answer discovery; it still reaches other servers
  as a client. Such a machine starts every transfer itself, with `mirror`
  against the other server, and finds that server with `discover --to`,
  since the answer to a broadcast probe is unsolicited too.

## Alternatives considered

QUIC libraries, ENet, GameNetworkingSockets, netcode.io, protobuf-style
schemas and WebRTC; the [proposal](../proposals/network-protocol.md#alternatives-considered)
records why each was not chosen.

## Evidence

Windows 10, Ryzen 5 2600, clang, 2026-10-10.

- CPU tests in `build_debug`: `vulkan_renderer_tester --suite net` runs 26
  transport tests (12 s) and 6 data-format tests;
  `--suite depot` runs 5 depot tests, including a push, fetch, stale
  compare-and-swap, incomplete commit, lock and refused key over a simulated
  10 ms path with 5% loss. All pass.
- End to end over loopback UDP:
  `python3 tools/checks/check_depot_e2e.py build_release/tools/depot/vkr_depot`
  ([check_depot_e2e.py](../../tools/checks/check_depot_e2e.py)) drives
  init, serve, clone, commit, a second clone, an edit, a pull with a kept
  local change, a refused stale push, locks and a refused key; it passes.
  A 10-byte edit inside a 3.5 MB file uploaded 7 objects (0.3 MB). A
  second server then finds the first by `discover --to 127.0.0.1`, and
  `mirror` reports `to-peer main`, then, after a commit on the second
  server, `from-peer main`, whose files the first server's working copy
  pulls byte for byte, then `same main`, and `diverged main` once both
  servers move it; it passes on Windows Debug.
- `build_release`, `vkr_net_bench`:
  - `crypto`: AES-256-GCM 2,189 MB/s on 1,200-byte packets and
    2,531 MB/s on 1 MiB; SHA-256 1,382 MB/s; ChaCha20-Poly1305 652 MB/s.
  - `raw-client` on loopback: RTT p50 48 µs; 66,570 datagrams/s of 1,200
    bytes from one sender (80 MB/s).
  - `client` on loopback: handshake 3 ms; 64-byte unreliable echo p50
    70 µs; bulk 89.1 MB/s without loss at 1,452-byte packets.
  - `sim`: 1,130 MB/s over a 1 GB/s, 1 ms path; 78.8 MB/s over 100 MB/s,
    40 ms RTT; 59.0 MB/s with 1% loss.

These are loopback and simulated results on one host, not the proposal's
cross-machine targets.

### Mac M1 and Windows

2026-10-11. The Windows machine above (wired) and a company-managed Mac M1
(macOS arm64, Wi-Fi, SentinelOne and Netskope active) on one IPv4 subnet.
Each serves a full store; the Windows store is on a hard disk.

- On the Mac, Release `check_depot_e2e.py` passes (3.2 s), and Debug
  `--suite depot`, `--suite filesystem` and `--suite editor_project_store`
  pass.
- The Mac drops every unsolicited inbound datagram, even for a program its
  application firewall allows, so Windows can neither discover nor reach
  the Mac's server. The Mac runs `mirror` against the Windows server, now
  as a login agent with `--watch 30`.
- Mac to Windows: commit `cf90b305` (42 files, 1,462.6 MB, 397 MB in the
  store after zstd) and four later commits, the last `9ba592c4` (39 files,
  1,462.4 MB), which the watching agent pushed on its own. Windows to Mac:
  `e268ab8a`, `from-peer main` in 0.5 s; the Mac pulled it and both files'
  SHA-256 hashes matched. Each repeated `mirror` printed `same`.
- Windows opened the cloned scene in the Release Vulkan editor with
  `--workspace D:\vkr-depot\work`: 54,143 entities, rendered, no error lines.
- A Release clone of `c3df7d0c` on the Windows hard disk over loopback took
  1 min 8 s with its own store and 16.5 s sharing the server's store
  (`clone --store`), which is the checkout alone.

The first mirror, 1.4 MB/s, ran before object writes stopped flushing
(`c13eb11c`); the Mac then measured 27 to 110 MB/s on loopback. Over Wi-Fi
through the endpoint agents, no transfer measures the 1 GbE target.

### Mac M1, 2026-10-12

macOS arm64, Apple clang; the machine was loaded (load average 12 to 23)
during the transport runs, so loopback numbers are noisy.

- Debug `--suite net`: 33 of 33 before the day's changes (34 s), 38 of 38
  after (about 7 s; the pacing fix shortened the bulk tests). New tests:
  the host thread (with its wake disabled the round trip median grows from
  about 330 µs to 75.9 ms and the test fails), fairness on a shared
  bottleneck (shares 0.46 and 0.51; fails outside 35–65% or with over 20%
  unused), background mode (0.01 beside a normal flow at 0.93, 0.96 alone;
  fails above 25%, below 65% or below 70%), header protection (consecutive
  numbers do not count up on the wire; a flipped number or length bit fails
  authentication) and a session event after CLOSED. After the security
  fixes below, 41 of 41, adding a replayed handshake (with the timestamp
  check disabled the test fails), an address spoof that must not move a
  connection, and the reassembly caps.
- Debug `--suite depot` with ASan and UBSan: 15 tests after the security
  fixes (refused names, tree limits, checkout safety with planted state
  paths, ignored paths and a case-only rename, refused pushes, request
  floods), and before them 11 tests, including access
  requests granted at runtime, the three conflict policies, a cancelled
  fetch that a later fetch completes, the identity copy from the cache
  folder, the service's port fallback past an IPv4 holder, discovery by a
  browser, case-colliding tree names and case variants of locked paths.
  `check_depot_e2e.py` passes with `access` taking effect without a
  restart.
- Release `vkr_net_bench` on loopback before the changes: AES-256-GCM
  4,955 MB/s on 1,200-byte packets, raw round trip p50 24 µs and 206 MB/s,
  transport bulk 134 MB/s, `sim` 1,200.8 MB/s over 1 GB/s with 1 ms
  (1.15 µs per packet), 78.8 MB/s over 100 MB/s with 40 ms, 74.0 MB/s with
  1% loss. After: bulk about 145 MB/s, round trip p50 101–144 µs (before
  102–153 µs in the same series; the 320 µs of the first run did not
  repeat), idle server 0.00 s of CPU in 10 s, header mask 112.5 ns, `sim`
  1.68 µs per packet (the simulator's flight list, not the transport, grew).
- Fuzzers, 120 s each (Apple clang ASan and UBSan with LLVM 20's libFuzzer):
  server core 166,251 executions (cov 1361), sessions 113,188 (cov 1189),
  wire codec 24.3 million (cov 299), depot 3.47 million (cov 945), no crash;
  after header protection the core ran 96,035 executions in 60 s. A depot
  tree with case-colliding names, which the parser accepted, is now refused
  and kept as `tests/fuzz/crashes/depot_tree_case_fold`.
- An agent security review on 2026-10-12 (not independent) found that a
  checkout could write into `.vkrdepot/`, that a project's scripts from
  another computer could run without consent, a replayable handshake that
  filled server slots, unbounded trees, impersonated access requests,
  case-only renames that lost files, and smaller limits; each is fixed
  above or in [ADR-110](110-project-sharing-in-the-editor.md), and the
  core fuzzer ran 98,155 executions in 60 s after the handshake change.

## Gaps

- Mac–Windows throughput and CPU per side on 1 GbE, and the transport's
  CPU per packet on an M1 Pro and the Ryzen.
- Windows: the host thread, header protection, the depot service and the
  editor's sharing have not been compiled or run there since 2026-10-11.
  Header protection changes the wire format, so both sides need binaries
  from 2026-10-12 or later.
- Registered I/O and NAT traversal with a rendezvous and relay.
- The depot's sparse working copy and remote content mount (proposal
  phase 6) and the shared Bakery cache.
- An independent security review.

## Revisit when

A Mac–Windows measurement misses a target, the security review asks for
changes, or a service needs a delivery class or frame the transport lacks.
