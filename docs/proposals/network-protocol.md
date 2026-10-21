---
status: proposed
updated: 2026-10-21
authority: proposal
---
# Network protocol

The work that remains of one binary protocol over UDP for every networked
feature of the engine and the editor. The transport, the binary data format,
services over sessions and the asset depot (phases 0 to 3) are implemented;
[ADR-105](../adr/105-network-transport-and-asset-depot.md) records them and
their evidence. The core of editor collaboration is implemented too
([ADR-106](../adr/106-collaborative-editing-session.md)). This proposal keeps
the services that build on them:

1. Asset streaming: assets fetched from a depot as the scene needs them and
   uploaded into GPU memory.
2. Editor collaboration: the rest of several users in one editor session
   whose edits, drags and clicks reach every participant.
3. Agent federation: agents on several machines that work on one level
   through their own editors, with work divided between them.
4. View streaming: frames rendered on another machine and streamed back.

It also keeps the gaps of phases 0 to 3. Names of types and functions are
provisional until the phase that adds them.

Game networking (multiplayer sessions, persistent worlds and their server
scale) has its own proposal, [Game networking](game-networking.md), which
will be tackled separately after the phases here. The owner moved it out
on 2026-10-16.

## Current baseline

- **Network stack (ADR-105).** `vkr_net` provides the sans-I/O transport
  core with the Noise IK handshake, AES-256-GCM packets, five delivery
  classes on up to 64 channels, flow control, BBR-style congestion control,
  PMTU discovery and path validation; the UDP host; the deterministic
  simulator; the bit stream and exact-schema wire codec; and sessions that
  open versioned services. `vkr_depot` serves and syncs a content-addressed,
  versioned asset store with locks and access roles.
- **Typed values.** Type descriptors
  ([vkr_type_desc.h](../../runtime/src/core/vkr_type_desc.h),
  [ADR-076](../adr/076-project-object-model.md)) describe every component:
  at most 64 properties and 1 KiB per value. Entities have document-stable
  UUIDs ([vkr_entity_ref.h](../../lib/src/core/vkr_entity_ref.h)).
- **Edits.** Editor edits are `VkrSampleEditBatchItem` lists of at most
  2,048 items, applied by the runtime as one journal group with authors,
  claims and a change feed
  ([vkr_scene_edit.h](../../runtime/src/renderer/systems/vkr_scene_edit.h),
  [vkr_sample_runtime.h](../../runtime/src/vkr_sample_runtime.h)). Journal
  sequences are process-wide, and requests refer to local `VkrEntityId`
  values.
- **Streaming inputs.** World partition supplies streaming sources and
  128 m cells ([ADR-086](../adr/086-world-partition.md)). Resource
  publication is an ordered queue with confirm-before-use
  ([ADR-082](../adr/082-renderer-owned-render-thread.md)). VFS mounts
  ([vkr_vfs.h](../../lib/src/filesystem/vkr_vfs.h)) are fixed at startup.
  Offscreen targets share frame submission
  ([ADR-014](../adr/014-offscreen-present-target.md)). No video encoder or
  decoder exists.

## Requirements

The use cases put different demands on one transport.

| Use case | Traffic shape | Delivery needed | Metric that matters |
|---|---|---|---|
| Asset depot | Bulk, MB to GB per transfer | Reliable, any order, resumable | Throughput per core |
| Editor collaboration | Low-rate edits, 60 Hz gestures | Reliable ordered edits, latest-value gestures | Edit round trip |
| View streaming | 10 to 50 Mbps video | Retransmit only before a deadline | Glass-to-glass latency |
| Asset streaming | Bulk with priorities | Reliable, any order, cancellable | Time to first visible mip |
| Agent federation | As editor collaboration, plus tasks | Reliable ordered | Edit round trip |

Requirements that follow from the table:

- Delivery is a property of a channel, not of the protocol. One
  connection carries channels with different delivery classes.
- A use case that is not active costs nothing. Services, channels, FEC,
  compression and large MTUs are negotiated, so an unused feature adds no
  bytes and no work.
- Small messages cost a few bytes of framing, and many messages share one
  packet.
- Bulk transfers do not raise the latency of interactive channels on the
  same path.
- A peer cannot make another peer allocate memory beyond fixed caps.
- Every byte after the handshake is authenticated and encrypted.

Game sessions put their own demands on the transport; the game networking
proposal lists them.

Non-goals: text formats on the wire, browser clients, voice, and a TCP
fallback.

## Owner decisions of 2026-10-09

| Decision | Choice | Consequence |
|---|---|---|
| Crypto library | A pinned libsodium release, vendored: X25519, Ed25519, BLAKE2b, ChaCha20-Poly1305 and AES-256-GCM | Built by `vkr_sodium` ([vendor/libsodium.md](../../vendor/libsodium.md)). The transport refuses to start without the AES-GCM hardware path, which every CPU of ADR-083 has. Monocypher (no AES hardware path) and AEGIS-128L (32 B tag in libsodium) were rejected. |
| Transport | The custom transport of [ADR-105](../adr/105-network-transport-and-asset-depot.md), not a QUIC library | The project owns loss recovery, congestion control and the handshake, so the security review and the simulated-link tests are required gates. |
| Depot versioning | Own commits, trees, refs and locks | Git LFS is excluded because its batch API is JSON over HTTPS. `vkr_depot` is the command (ADR-105); the collaborative session commits, publishes and checks out through it (ADR-106). |
| Server platforms | Windows and macOS only; Linux later | Every server runs on Windows or macOS. A macOS server sends and receives one datagram per system call. Registered I/O for Windows game servers is in the game networking proposal. A Linux server build is future work. |

## Gaps of phases 0 to 3

| Gap | Work | Evidence needed |
|---|---|---|
| Cross-machine numbers | `vkr_net_bench` and a depot push and pull between a Mac and a Windows machine on 1 GbE; the depot already moves a 1.46 GB scene both ways over Wi-Fi (ADR-105), which measures neither | Raw UDP and transport RTT and throughput per direction; depot MB/s and cores per side against the 110 MB/s target |
| Threaded host | The host thread runs the depot server inside the sharing editor; serving 2.71 GB moved the Bistro editor's median frame time from 23.8 to 25.1 ms on one Mac shared with the receiver (ADR-110). The collaborative session pumps on the host thread too (ADR-106) | The frame time of both measured with the other editor on another machine |
| Sparse project sharing | The editor's Get makes a sparse working copy with **Fetch files when opened** (ADR-123): Bakery processes read the copy themselves, the project and its World arrive when read, cooked textures come from the side branch, and a Submit carries only what changed; checked on one Mac over loopback with Bistro. Not run between two computers or on Windows | A shared project opens from a sparse Get on another computer, Windows included, and a Submit carries only what changed |
| Security | An independent review; the fuzzers run under ASan and UBSan (ADR-105) | Review closed before use outside a trusted network |
| Privacy | NAT traversal with a rendezvous and relay; packet numbers are already masked (ADR-105) | Decided by the owner before internet deployment |

## Services

### Asset streaming (use case 4b)

Asset streaming loads a scene from a remote depot or a streaming server
without a full download.
[ADR-123](../adr/123-asset-streaming-from-a-depot.md) implements steps 1
and 2: a remote content directory that fetches each file a loader asks for,
through a VFS remote, with cooked platform files on a side branch per GPU
family; reads of parts of files that fetch only the chunks holding them; and
progressive mips, where a material's large texture first draws from the
smallest levels at the start of its file and its whole file downloads once
a frame draws it. Every received chunk is checked against its SHA-256
before it enters the store (ADR-105). The rest:

- **Mesh LOD ranges.** Meshes stream from the coarsest LOD range upward
  ([ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md)); the
  mesh format must place the coarse ranges first, as KTX2 places small
  mips first.
- **Priority.** Whole-file downloads follow the order in which drawn
  previews reach their reload (ADR-012), and a queued download whose
  texture no frame drew for 120 draw epochs is cancelled (ADR-123). Mip
  priority will come from the required mips that
  [texture streaming](texture-streaming.md) computes, which then replace
  the fixed 256 KiB preview; this service orders the remote fetches.
  Priority from the streaming sources and the screen size for other assets
  remains.
- **GPU placement.** Fragments decrypt into staging memory supplied by the
  receiver. The resource then goes through the ordered publication queue
  with confirm-before-use. Staging memory returns to its pool after GPU
  completion, not after a frame count. The
  [dedicated transfer queue](dedicated-transfer-queue.md) proposal owns
  any Vulkan transfer-queue path.

### Editor collaboration (use case 3)

[ADR-106](../adr/106-collaborative-editing-session.md) implements the core:
host and join, host-ordered edit requests with mirrored journals, replay
from the session base, shared undo, gizmo moves and their previews,
seeded duplicates, terrain as results, the Session window, other
editors' cameras, names, selections and pointer rays in the Scene, joining
and publishing files through the depot, newer depot bases for long
histories, access by allowed identity keys, edits that load the
partition cells they need, publishing files on their own and the host
thread. The rest:

- A file published during a session reaches other platforms as authored;
  each cooks its own variant, or fetches it from the cooked side branch
  (ADR-123), which a Get or Get latest takes.
- Play stays local to each participant in this proposal.

### Agent federation (use case 5)

[ADR-106](../adr/106-collaborative-editing-session.md#agent-federation)
implements authors across editors, the host's claims and task board with
capability matching and slots, the session-wide change feed with
reverted batches, and reviews across editors. The rest:

- Each participant renders its own captures on its own GPU. A Mac draws
  the tiled pipeline and a Windows machine draws the desktop pipeline
  ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md)). Agents that
  compare captures across machines must account for that difference.

### View streaming (use case 4a)

A server renders a view and streams encoded frames to a client, which
sends input back. For example, a Mac editor can show the desktop pipeline
rendered on a Windows machine.

The path of one frame:

```text
server: render offscreen -> HW encode (slices)
          -> view channel (DEADLINE, FEC)
client: reassemble -> HW decode -> present
          -> input channel back to server
```

- **Encoders.** macOS uses VideoToolbox with low-latency rate control.
  Windows uses Vulkan Video encode (H.264 and H.265 on RDNA 2 and Ampere;
  AV1 on later GPUs), so the image stays on the device. The
  recommendation is Vulkan Video; AMF and NVENC are the alternative. The
  renderer change is its own design and ADR.
- **Settings.** No B-frames, a VBV of one frame, intra refresh instead of
  periodic IDR frames, and slices sent as soon as each one is encoded.
- **Delivery.** Each frame message carries the capture time, the frame
  number and the slice index, and has a deadline of two frame intervals
  after capture. FEC repair packets protect each slice group, and their
  count follows the measured loss; a lossless local network sends none.
  A lost fragment is resent only while time remains. After an
  unrecoverable loss, the client sends `REFRESH` and the encoder starts an
  intra refresh.
- **Rate.** The encoder bitrate is 0.85 × the bandwidth estimate, updated
  every 100 ms.
- **Input.** Pointer and key input travels unreliable and `IMMEDIATE`,
  with each packet repeating the last few events. Text entry travels on a reliable ordered channel.
- **Client.** The client presents the newest decoded frame without a
  jitter buffer. A one-frame buffer is an option for paths with jitter.

Target for phase 7: glass-to-glass latency at or below 50 ms p95 on a
local network at 2560×1440 and 60 Hz, at 20 to 40 Mbps.

## Implementation

| Owner | Path | Contents |
|---|---|---|
| `vkr_runtime` | `runtime/src/net/` | `stream` and `view` services |
| Editor | `editor/src/editor_session.c` | The `collab` service, Session window, Scene overlay, access and agent federation (ADR-106); next partition edits |
| Renderer | Encoder and decoder hooks | Separate design and ADR in phase 7 |
| `vkr_net` | `net/src/` | Threaded host |

- **Threads.** The editor and games pump a threaded host so a frame never
  waits on the network.
- **Memory.** Each service bounds its queues by the transport's channel
  windows; staging memory for streamed assets returns to its pool after GPU
  completion, not after a frame count.

## Phases

| Phase | Scope | Exit evidence |
|---|---|---|
| 4. Editor collaboration and agents | Remaining after ADR-106: the same evidence between two machines | Two editors and two agent groups edit Bistro; both saved scenes match byte for byte |
| 6. Asset streaming | Remote content directory, cooked side branch, progressive mips and cancel (ADR-123); then mesh LOD ranges, priority, and fetched bytes placed for the GPU | Bistro streamed from a depot between two machines: time to first frame and to full detail against a local load; captures equal to a local load |
| 7. View streaming | Encoder and decoder hooks, view channel with FEC | Glass-to-glass latency on Bistro, with a camera-flash or frame-counter method |

Phase 4 comes first because the editor uses it first and the depot it needs
exists. The former phases 5 (game sessions) and 8 (scale) are in the
[game networking proposal](game-networking.md); the other phases keep their
numbers.

## Performance targets

These are targets for the evidence of each phase, not results.

| Target | Value | Phase |
|---|---|---|
| Depot throughput on 1 GbE between Mac and Windows | at least 110 MB/s, at most 1.5 cores per side | gap |
| Transport CPU per 1,200 B packet, sent and received, without system calls | at most 2 µs on M1 Pro and Ryzen 5 2600 | gap |
| Collaborative edit round trip | RTT plus at most one frame | 4 |
| Glass-to-glass latency on a local network | at most 50 ms p95 at 2560×1440, 60 Hz | 7 |

## Verification

- Each service gets simulator tests that name their failure, as the
  transport and depot have (ADR-105), and an end-to-end check over real
  sockets.
- Every scene-based check uses Bistro. Performance claims use matched
  Release builds with graphics validation off.
- Native evidence for each GPU class comes from its backend: Metal for the
  tiled pipeline, Vulkan for the desktop pipeline.

## Open decisions

Each decision has a recommendation. The owner can change it before the
phase that depends on it.

| Decision | Recommendation | Tradeoff |
|---|---|---|
| Collaboration authority | Host-sequenced replication of results | CRDTs remove the host but need merge rules for ECS structure and every component type, and do not match the journal. |
| Windows video encoding | Vulkan Video | AMF and NVENC are mature and per-vendor; Vulkan Video is one API and keeps the image on the device. |
| Local agent socket | Keep JSON | Agents and MCP speak JSON. The socket is a local process boundary, not the network. |
| Internet sessions | Direct addresses and port forwarding first; rendezvous and relay later | Home-to-home sessions behind NAT need hole punching and a relay service. |

## Alternatives considered

- **QUIC (msquic, quiche, ngtcp2).** QUIC delivers reliable streams and
  RFC 9221 datagrams. It has no sequenced or deadline class and no
  per-message priority. Its TLS certificate model does not match connect
  tokens or pinned workspace keys. The owner chose the custom transport on
  2026-10-09. This proposal keeps QUIC's loss recovery, acknowledgment
  ranges, varint and amplification rule.
- **netcode.io and yojimbo.** The connect-token design is adopted. The
  reliability layer has no congestion control and no bulk path.
- **ENet.** No encryption and no congestion control suitable for bulk
  transfers.
- **GameNetworkingSockets.** A complete C++ library with its own threads
  and allocators. It adds no depot or deadline semantics.
- **TCP for the depot.** Kernel offload makes TCP cheap per byte on
  Windows and macOS. A second transport needs a second security path and
  does not share congestion control with interactive sessions. Phase 3's
  measurements decide whether this remains acceptable.
- **Protobuf, FlatBuffers, Cap'n Proto.** Binary, but they spend bytes on
  field tags or offset tables, need a generator, and do not pack bits.
  Snapshot updates need bit-level packing.
- **WebRTC for view streaming.** It negotiates sessions with SDP, a text
  format, and carries a large dependency.

## Risks

- **Security of a custom protocol.** Mitigated by a specified handshake
  (Noise), library primitives, fuzzing and an external review before
  deployment outside trusted networks.
- **Throughput on Windows and macOS servers.** One system call per datagram
  may limit bulk transfers above 1 GbE.

## Related documents

- [ADR-105](../adr/105-network-transport-and-asset-depot.md): the
  transport, data format, sessions and asset depot.
- [ADR-106](../adr/106-collaborative-editing-session.md): the collaborative
  editing session.
- [Game networking](game-networking.md): multiplayer sessions, persistent
  worlds and server scale.
- [ADR-076](../adr/076-project-object-model.md): type descriptors and
  entity references.
- [ADR-077](../adr/077-asset-build-system.md): content hashes, `.vkpak`,
  the Bakery cache.
- [ADR-082](../adr/082-renderer-owned-render-thread.md): publication
  queue.
- [ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md): agent
  channel, authors, claims and the change feed.
- [ADR-086](../adr/086-world-partition.md): streaming sources and cells.
- [Dedicated transfer queue](dedicated-transfer-queue.md): Vulkan upload
  path.
