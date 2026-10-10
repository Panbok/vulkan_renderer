---
status: proposed
updated: 2026-10-10
authority: proposal
---
# Network protocol

The work that remains of one binary protocol over UDP for every networked
feature of the engine and the editor. The transport, the binary data format,
services over sessions and the asset depot (phases 0 to 3) are implemented;
[ADR-105](../adr/105-network-transport-and-asset-depot.md) records them and
their evidence. This proposal keeps the services that build on them:

1. Asset streaming: assets fetched from a depot as the scene needs them and
   uploaded into GPU memory.
2. Game sessions: lobbies of 10 to 20 players first, later persistent worlds
   with 10,000 or more players and 200 to 400 in one place.
3. Editor collaboration: several users in one editor session whose edits,
   drags and clicks reach every participant.
4. Agent federation: agents on several machines that work on one level
   through their own editors, with work divided between them.
5. View streaming: frames rendered on another machine and streamed back.

It also keeps the gaps of phases 0 to 3. Names of types and functions are
provisional until the phase that adds them.

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
- **Simulation.** The scene ticks at a fixed 60 Hz. The FPS module orders
  input commands by tick and sequence without a wire encoding
  ([ADR-073](../adr/073-native-gameplay-foundation.md)). The
  [entity behavior proposal](entity-behavior-system.md#preserving-a-multiplayer-path)
  asks for network identities mapped to local handles and no speculative
  replication descriptors on every field.
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
| Game sessions | Small messages, 20 to 60 Hz | Unreliable state, reliable events | Latency, bytes per client, clients per server |
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

Non-goals: text formats on the wire, browser clients, lockstep or
cross-platform deterministic simulation, peer-to-peer game meshes, voice,
and a TCP fallback.

## Owner decisions of 2026-10-09

| Decision | Choice | Consequence |
|---|---|---|
| Crypto library | A pinned libsodium release, vendored: X25519, Ed25519, BLAKE2b, ChaCha20-Poly1305 and AES-256-GCM | Built by `vkr_sodium` ([vendor/libsodium.md](../../vendor/libsodium.md)). The transport refuses to start without the AES-GCM hardware path, which every CPU of ADR-083 has. Monocypher (no AES hardware path) and AEGIS-128L (32 B tag in libsodium) were rejected. |
| Transport | The custom transport of [ADR-105](../adr/105-network-transport-and-asset-depot.md), not a QUIC library | The project owns loss recovery, congestion control and the handshake, so the security review and the simulated-link tests are required gates. |
| Depot versioning | Own commits, trees, refs and locks | Git LFS is excluded because its batch API is JSON over HTTPS. `vkr_depot` is the command (ADR-105); editor integration remains. |
| Server platforms | Windows and macOS only; Linux later | Every server, including the phase 8 scale test, runs on Windows or macOS. Windows servers use Registered I/O in phase 8. A macOS server sends and receives one datagram per system call. A Linux server build is future work. |

## Gaps of phases 0 to 3

| Gap | Work | Evidence needed |
|---|---|---|
| macOS | Build and run `vkr_net`, `vkr_depot` and the tests on Apple silicon | `--suite net`, `--suite depot` and `check_depot_e2e.py` on a Mac |
| Cross-machine numbers | `vkr_net_bench` and a depot push and pull between a Mac and a Windows machine on 1 GbE | Raw UDP and transport RTT and throughput per direction; depot MB/s and cores per side against the 110 MB/s target |
| Background mode | A simulator bottleneck shared by two links | A background connection yields to an interactive one; two bulk connections converge to equal shares |
| Threaded host | A net thread with bounded rings to application threads, woken by `vkr_net_host_wake` | Editor frame time unchanged while a depot transfer runs |
| Windows server I/O | Registered I/O per worker in phase 8 | Datagrams per second per core against the one-call baseline |
| Sparse working copy | Files fetched when opened, through the remote content mount of asset streaming | A Bistro scene opens from a sparse working copy |
| Shared Bakery cache | Action keys to output hashes in a depot; a cook downloads instead of re-encoding | A second machine's cook of Bistro hits the cache |
| Security | Fuzzers for the frame parser, handshake, wire decoder and depot objects under ASan and UBSan; an independent review | No fuzzer findings; review closed before use outside a trusted network |
| Privacy | Header protection of packet numbers; NAT traversal with a rendezvous and relay | Decided by the owner before internet deployment |

## Components on the wire

Replication encodes components through their existing `VkrTypeDesc`, so a
component is described once. A replicated type registers a
`VkrNetComponentDesc` that names the type and, only where needed, a
quantization per property. Every other property takes a default encoding
from its kind:

| Property kind | Default wire encoding |
|---|---|
| `BOOL` | 1 bit |
| `I32`, `U32` | `VARUINT` (zigzag for `I32`) |
| `F32`, `ANGLE` | `F32`, or `QFLOAT` when the property has a range and the net descriptor gives bits |
| `VEC2`–`VEC4`, `COLOR`, `DIRECTION` | `F32` per component, or `QFLOAT` as above |
| `QUAT` | `QUAT(15)` |
| `ENUM` | `ENUM(count of names)` |
| `STRING` | `UTF8(capacity)` |
| `ENTITY` | `NETID` inside a session; `ENTITY_REF` in edit batches |

`TRANSIENT` properties never travel. A component's change mask has one
bit per property, which is at most 64 bits because `VKR_TYPE_PROPERTY_MAX`
is 64. Opt-in registration keeps unreplicated types free of network
descriptors, which the entity behavior proposal requires.

## Services

### Asset streaming (use case 4b)

Asset streaming loads a scene from a remote depot or a streaming server
without a full download.

- **Remote content mount.** A new VFS mount kind resolves an identity to a
  hash and size through a depot tree, and fetches missing chunks into the
  local store. VFS mounts are fixed at startup today, so this mount needs
  thread-safe lookups of fetch state. Phase 1 blocks a loader worker until
  the bytes arrive; loaders already prepare resources on workers
  ([ADR-045](../adr/045-resource-prepare-and-render-thread-finalize.md)).
- **Platform variants.** The client states its platform when it opens the
  service. The server serves the host-native cooked variant (ASTC for
  Metal, BC for x86-64, [ADR-012](../adr/012-texture-compression-pipeline.md)),
  or refuses an identity without one. A server with Bakery can cook the
  missing variant first.
- **Progressive order.** Phase 2 requests texture mips from the smallest
  level upward, and mesh LOD ranges from the coarsest level
  ([ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md)).
  Priority comes from the streaming sources and the screen size. A request
  for an asset that leaves interest is cancelled with CANCEL.
- **GPU placement.** Fragments decrypt into staging memory supplied by the
  receiver. The resource then goes through the ordered publication queue
  with confirm-before-use. Staging memory returns to its pool after GPU
  completion, not after a frame count. The
  [dedicated transfer queue](dedicated-transfer-queue.md) proposal owns
  any Vulkan transfer-queue path.
- **Integrity.** The client checks the SHA-256 of each chunk before it
  publishes the resource.

### Game sessions (use case 2)

#### Topology

The topology grows in two steps:

```text
Step A (10 to 20 players):
  client ---- server (dedicated or listen)

Step B (10,000+ players):
  client ---- gateway ---- cell servers
  client ---/        \---- cell servers
```

Step A is the target of this proposal. Step B is a research program that
builds on the same transport. Its requirements on the transport are listed
under [Step B](#step-b-persistent-worlds).

#### Identity and state

- **Network IDs.** The server assigns each replicated entity a session
  `NetId`: a 20-bit index and a 12-bit generation. Each peer maps a
  `NetId` to its local `VkrEntityId`. An entity from a level document is
  named once by its `VkrEntityRef` in its spawn record, because both peers
  load the same level.
- **Replicated entities** carry a `net_replicated` component: owner,
  priority, relevance radius and update-rate tiers. Only types with a
  `VkrNetComponentDesc` travel.
- **Server tick.** The server simulates at the existing 60 Hz and sends
  snapshots at a per-session rate from 20 to 60 Hz.

#### Snapshots and delta compression

The server keeps a ring of quantized states for each replicated entity
over the last 64 ticks. It keeps, per client and entity, the last tick
that the client acknowledged. An update for a client is the difference
between the current state and the acknowledged state:

```text
update = NetId
       + component change mask
       + per component: property change mask, changed values
```

- Clients that acknowledged the same tick receive the same bytes. The
  server caches each encoded difference by entity, baseline tick and
  current tick, so most updates in a crowded area are copies.
- A baseline that left the ring forces a full state.
- Snapshots travel on an `UNRELIABLE` channel. Each packet decodes on its
  own, so one lost packet loses only its entities. The packet-acknowledged
  hook advances the client's baselines; the client sends no extra
  acknowledgment.
- Spawns and despawns ride in snapshot packets and repeat until a packet
  that carried them is acknowledged.
- Gameplay events that need order (damage confirmations, inventory
  changes) use a `RELIABLE_ORDERED` channel.

#### Interest and bandwidth budget

The server divides the world into a uniform grid, for example 32 m. A
client's interest set is the cells near its streaming sources plus entities
marked always relevant. Each candidate entity accumulates priority from
distance, view direction, its own priority and the time since its last
update. Each snapshot sends the highest-priority updates that fit the
client's byte budget. The budget is the lower of a configured cap and a
share of the bandwidth estimate.

Estimates for planning, which phase 5 and phase 8 must measure. Each
assumes 10 B per entity update and 51 B of packet overhead over IPv4.

| Case | Updates per second per client | Down per client | Server egress |
|---|---|---|---|
| 20 players, all relevant, 30 Hz | 20 × 30 = 600 | about 60 kbps | about 1.2 Mbps |
| 400 in one place: 32 at 30 Hz, 96 at 10 Hz, 272 at 4 Hz | 3,008 | about 250 kbps | about 100 Mbps |
| Input upstream: 60 packets/s, four commands each | — | about 36 kbps up | — |

#### Input, prediction and time

- The client sends input on an `UNRELIABLE` channel with `IMMEDIATE`. Each
  packet repeats the last four to eight FpsInput commands with their tick
  and sequence, so a burst of lost packets loses no command.
- The server holds one to two ticks of input in a jitter buffer and
  reports the buffer's depth. The client changes its simulation rate by up
  to ±5% to keep the depth at its target.
- The client predicts its own player. Each snapshot carries the last input
  sequence that the server applied, and the client replays later inputs
  from the corrected state. This needs the separation of authoritative and
  presentation state that the entity behavior proposal describes.
- The client draws remote entities two snapshot intervals in the past and
  interpolates between snapshots.
- Hitscan lag compensation keeps one second of hitbox history on the
  server. It belongs to a later gameplay change.

#### Step B: persistent worlds

10,000 players over a world, with 200 to 400 in one place, need
simulation across several processes. The transport supports this through:

- connection IDs with shard bits, so a load balancer or a socket per
  worker can route packets without state;
- gateways that terminate client connections (crypto, acknowledgment,
  pacing) and forward compact per-client payloads from cell servers;
- server-to-server connections that multiplex many client lanes, with a
  32-bit lane ID in the service header rather than one transport channel
  per client;
- snapshot updates that encode per entity, so updates from several cell
  servers combine into one client packet;
- connection migration between gateways with a fresh token.

Entity authority handoff across cell boundaries, cell-server placement and
persistence are outside this proposal.

### Editor collaboration (use case 3)

One editor is the session host. A headless editor on a server can also be
the host. The host owns the authoritative scenes, the journal and a
session-wide edit sequence. Other editors are participants.

#### Joining

1. The participant pulls the project from the depot, or confirms that its
   working copy matches the host's commit.
2. The host sends the session state as schema messages: per container, its
   journal revision and the entities changed since the commit, encoded
   through their type descriptors.
3. The participant applies the state and reports ready. From then on,
   journal groups arrive in session order.

#### Edits

A participant's editor runs an operation locally, as it does today, up to
the `VkrSampleEditBatchItem` list. It sends the list to the host instead
of submitting it to its own runtime.

```text
participant                          host
  | EDIT_BATCH: items, base revisions -> |
  |                                      | validate, check bases,
  |                                      | apply as journal group,
  |                                      | assign session sequence
  | <- EDIT_RESULT: ok or code + index   |
  | <- JOURNAL_GROUP (to every peer)     |
```

- `EDIT_BATCH` names entities by `ENTITY_REF` or by a `$k` index into the
  batch, never by a local `VkrEntityId`. Component values travel through
  the component's wire encoding and include only the fields the request
  sets. Each item that reads an entity carries that entity's revision.
- The host refuses an item whose base revision is out of date. The
  participant then runs the operation again on the current state. Edits
  to different entities never conflict.
- `JOURNAL_GROUP` carries the author, the group ID and each entry's
  resulting state: entity values, structure changes, and the after-state
  rectangles of terrain samples. Participants apply results and do not
  execute the operations again. Brush geometry and terrain strokes use
  floating-point math that can differ between ARM and x86, so applying
  results keeps all peers identical.
- A participant waits one round trip for its structure edits in phase 4.
  On a local network that wait is below one frame.
- Undo and redo are requests to the host. Author-scoped undo already
  refuses another author's step (`VKR-AGENT-0009`).

#### Gestures and presence

- A drag, gizmo move, slider scrub or terrain stroke sends `GESTURE`
  messages on a `SEQUENCED` channel at the display rate: gesture ID,
  entity and quantized value. Other participants draw a preview. The end
  of the gesture is one `EDIT_BATCH`, which matches the current `gesture`
  merge into one undo entry.
- `PRESENCE` messages on a `SEQUENCED` channel at 10 Hz carry each
  participant's camera, selection, cursor ray and tool. Editors draw
  remote participants from them.
- Claims (ADR-084) belong to the host and apply to every remote author.
  The change feed is the host's feed.
- A file created during the session, such as an import or a material
  graph, is pushed to the depot. The host broadcasts `ASSET_PUBLISHED` with
  the identity and hash, and participants fetch it when they need it.
  Each platform cooks its own variant or takes it from the shared Bakery
  cache.
- Play stays local to each participant in this proposal.

### Agent federation (use case 5)

Agent federation is editor collaboration with agents as authors.

- An agent talks JSON to its own editor over the local socket, unchanged.
  The editor is a session participant. Its agents' batches travel as
  `EDIT_BATCH` with the author `(participant, agent name)`.
- `changes.feed`, `claims.*` and the Changes window read the host's
  session-wide state. The claim rule then holds between agents on
  different machines.
- **Task board.** The host keeps a list of tasks: ID, kind (layout,
  material, lighting and so on), region claim, assignee and state.
  Participants advertise agent slots and capabilities, such as the tiled
  pipeline or a lightmap bake host. The host assigns tasks, and an agent
  takes its next task through a local `task.next` operation. This divides
  work between level design and material agents across machines.
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
- **Input.** Pointer and key input travels like game input: unreliable,
  repeated, `IMMEDIATE`. Text entry travels on a reliable ordered channel.
- **Client.** The client presents the newest decoded frame without a
  jitter buffer. A one-frame buffer is an option for paths with jitter.

Target for phase 7: glass-to-glass latency at or below 50 ms p95 on a
local network at 2560×1440 and 60 Hz, at 20 to 40 Mbps.

## Implementation

| Owner | Path | Contents |
|---|---|---|
| `vkr_runtime` | `runtime/src/net/` | `world`, `stream` and `view` services, the remote content mount |
| Editor | `editor/src/editor_session.c` | `collab` service, presence, task board, Session window |
| Renderer | Encoder and decoder hooks | Separate design and ADR in phase 7 |
| `vkr_net` | `net/src/` | Threaded host; replication helpers on the bit stream |

- **Threads.** The editor and games pump a threaded host so a frame never
  waits on the network. Snapshot encoding runs per client on the job
  system and hands encoded payloads to the net workers.
- **Memory.** Each service bounds its queues by the transport's channel
  windows; staging memory for streamed assets returns to its pool after GPU
  completion, not after a frame count.

## Phases

| Phase | Scope | Exit evidence |
|---|---|---|
| 4. Editor collaboration and agents | Host and join, edit batches, journal groups, gestures, presence, session claims and feed, task board | Two editors and two agent groups edit Bistro; both saved scenes match byte for byte |
| 5. Game sessions | `world` service, opt-in components, input, prediction for the FPS module | 20 clients (bots) on a Bistro session with measured bytes per client and correction counts under 2% loss |
| 6. Asset streaming | Remote content mount, then progressive mips and LOD ranges | Bistro streamed from a depot: time to first frame and to full detail; captures equal to a local load |
| 7. View streaming | Encoder and decoder hooks, view channel with FEC | Glass-to-glass latency on Bistro, with a camera-flash or frame-counter method |
| 8. Scale | Headless server executable for Windows and macOS that simulates without a renderer, server workers, Registered I/O on Windows, 400 co-located bots | Server tick cost, system-call CPU and egress at 400 clients on each host, against the estimate table |

Phase 4 comes first because the editor uses it first and the depot it needs
exists.

## Performance targets

These are targets for the evidence of each phase, not results.

| Target | Value | Phase |
|---|---|---|
| Depot throughput on 1 GbE between Mac and Windows | at least 110 MB/s, at most 1.5 cores per side | gap |
| Transport CPU per 1,200 B packet, sent and received, without system calls | at most 2 µs on M1 Pro and Ryzen 5 2600 | gap |
| Collaborative edit round trip | RTT plus at most one frame | 4 |
| 20-player session bandwidth | at most 64 kbps down per client | 5 |
| Glass-to-glass latency on a local network | at most 50 ms p95 at 2560×1440, 60 Hz | 7 |
| 400 co-located clients | at most 300 kbps down per client; snapshot encode within the server tick on a Ryzen 5 2600 and on an M1 Pro | 8 |

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
  may limit bulk transfers above 1 GbE and servers beyond the 400-client
  case. Step B needs measured server capacity before it chooses between
  more Windows workers and a Linux server build.
- **Prediction needs gameplay changes.** Prediction and replay need the
  state separation of the entity behavior proposal before phase 5 can
  meet its exit evidence.
- **Scope of step B.** Persistent worlds need distributed simulation that
  this proposal does not specify.

## Related documents

- [ADR-105](../adr/105-network-transport-and-asset-depot.md): the
  transport, data format, sessions and asset depot.
- [ADR-073](../adr/073-native-gameplay-foundation.md): fixed ticks and
  ordered input.
- [ADR-076](../adr/076-project-object-model.md): type descriptors and
  entity references.
- [ADR-077](../adr/077-asset-build-system.md): content hashes, `.vkpak`,
  the Bakery cache.
- [ADR-082](../adr/082-renderer-owned-render-thread.md): publication
  queue.
- [ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md): agent
  channel, authors, claims and the change feed.
- [ADR-086](../adr/086-world-partition.md): streaming sources and cells.
- [Entity behavior proposal](entity-behavior-system.md): the multiplayer
  boundaries.
- [Dedicated transfer queue](dedicated-transfer-queue.md): Vulkan upload
  path.
