---
status: partial
updated: 2026-10-10
authority: adr
---

# ADR-105: Network transport, data format and asset depot

## Status

Accepted (partial). Phases 0 to 3 of the
[network protocol proposal](../proposals/network-protocol.md) are
implemented: the encrypted UDP transport, the binary data format, services
over sessions, and the asset depot with its server and command. Every check
ran on Windows (Ryzen 5 2600, clang); macOS execution and every
cross-machine measurement are unverified. The proposal keeps editor
collaboration, agent federation, game sessions, asset and view streaming,
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
from the caller's thread.

- **Packets.** A short header is a flags byte (bit 6 set, bits 1–0 the
  packet-number length), the receiver's 32-bit connection ID and 1 to 4
  bytes of the packet number, all authenticated as AES-256-GCM associated
  data; the nonce is a per-direction IV XOR the 64-bit packet number. A
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
  it received to an unvalidated address. There is no 0-RTT data.
- **Delivery.** Up to 64 channels per connection; channel 0 is the reliable
  ordered session channel. Classes: unreliable, sequenced, reliable ordered,
  reliable unordered and deadline (dropped and cancelled after its time).
  Reliable messages up to 64 MiB split into fixed fragments at send time.
  Each channel has priority 0–7, weight, send queue and receive window;
  WINDOW frames raise the sender's limit as the application consumes.
  Data for a channel this side closed is dropped.
- **Recovery.** RFC 9002 loss detection with packet and time thresholds that
  grow when an acknowledgment proves a declared loss spurious, probe
  timeouts, ACK ranges pruned once acknowledged, and a 2,048-packet replay
  window. Frames, not packets, are retransmitted.
- **Congestion control** ([vkr_net_cc.h](../../net/src/vkr_net_cc.h)): a
  BBR-style model with a 10-round bandwidth filter and 10-second minimum
  RTT, STARTUP/DRAIN/PROBE_BW/PROBE_RTT, a loss cap above 2% per round,
  pacing with 10-packet bursts, and a background mode that scales pacing as
  queueing delay approaches 25 ms. PROBE_RTT samples count as app-limited.
- **Path.** PMTU discovery probes 1,452 bytes and then the configured
  maximum (8,952 for jumbo frames); lost probes do not count as congestion.
  A new peer address is validated with PATH_CHALLENGE.
- **Lifetime.** Idle timeout with keep-alives at a third of it, CLOSE with a
  numeric code, and a drain period of three probe timeouts.

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
  bytes, in a canonical binary form whose parser refuses unsorted, invalid
  or case-colliding names.
- **Store** ([vkr_depot_store.h](../../tools/depot/vkr_depot_store.h)): a
  content-addressed directory; each file is a 5-byte header (raw or zstd,
  decoded size) and the payload, kept compressed when that saves a tenth.
  Every write checks the hash and renames a temporary. Files split into
  64 KiB–1 MiB chunks by FastCDC with a fixed gear table.
- **Working copies**
  ([vkr_depot_workspace.h](../../tools/depot/vkr_depot_workspace.h)):
  `.vkrdepot/` holds the state (remote, server key, branch, base commit,
  index). A snapshot hashes only files whose size or time changed.
- **Service** ([vkr_depot_protocol.h](../../tools/depot/vkr_depot_protocol.h)):
  schema `vkr.depot` version 1 on a reliable ordered control channel; stored
  representations as raw messages on a reliable unordered priority-7
  channel tagged `(request << 8) | index`.
- **Server** ([vkr_depot_server.h](../../tools/depot/vkr_depot_server.h)):
  refs move only by compare-and-swap after the server checks the commit's
  objects are present and that it changes no path another key has locked.
  Keys need a role (read, write, admin) in the store's access list.
- **Client** ([vkr_depot_client.h](../../tools/depot/vkr_depot_client.h))
  fetches a commit's missing objects with pipelined GETs and uploads the
  objects a commit adds over its base with HAVE and PUT.
- **Command** ([vkr_depot_main.c](../../tools/depot/vkr_depot_main.c)):
  `keygen`, `init`, `serve`, `access`, `clone`, `status`, `commit` (snapshot,
  upload, move the branch), `pull` (keeps local changes, refuses
  conflicts), `refs`, `log`, `lock`, `unlock`, `locks`.

## Consequences

- Every networked feature builds on one handshake, one congestion
  controller and one data format; a feature adds a service, not a protocol.
- Nothing text-based crosses the wire. JSON stays on the local agent socket.
- The project owns the transport's security and must keep the fuzzing and
  review gates.
- On Windows and macOS each datagram costs one system call; on this host the
  transport moves about as many packets as raw UDP does.

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
  A 10-byte edit inside a 3.5 MB file uploaded 7 objects (0.3 MB).
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

## Gaps

- macOS build and execution, and every Mac–Windows measurement.
- A shared-bottleneck simulator test for background mode and fairness.
- The threaded host, Registered I/O, header protection and NAT traversal.
- The depot's sparse working copy and remote content mount (proposal
  phase 6) and the shared Bakery cache.
- An independent security review and a fuzzer run under ASan/UBSan.

## Revisit when

A Mac–Windows measurement misses a target, the security review asks for
changes, or a service needs a delivery class or frame the transport lacks.
