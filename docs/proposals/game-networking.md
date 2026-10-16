---
status: proposed
updated: 2026-10-16
authority: proposal
---
# Game networking

A layered architecture for multiplayer games: snapshots with
interpolation, client-side prediction, interest management and
authoritative servers. One design serves a first-person shooter session of
10 to 20 players and a persistent world of 10,000 or more players with 200
to 400 in one place, and a game may run both at the same time. The model is
Star Citizen's server meshing: a stateful replication layer between clients
and the simulation servers, an authority for every entity that can move
between servers, and persistent entity state.

The owner deferred this work on 2026-10-16. It starts after the
[network protocol](network-protocol.md) phases for editor collaboration,
asset streaming and view streaming, and builds on the transport of
[ADR-105](../adr/105-network-transport-and-asset-depot.md). Names of types
and functions are provisional until the phase that adds them.

## Current baseline

- **Transport (ADR-105).** Encrypted UDP connections with five delivery
  classes on up to 64 channels, congestion control, pacing and path
  validation. A connection ID holds a 4-bit shard,
  12 random bits and a 16-bit slot. The bit stream and the exact-schema wire
  codec already have `NETID`, `QFLOAT` and quantized vector kinds
  ([vkr_wire.h](../../net/src/vkr_wire.h)). Sessions open versioned
  services. Servers run on Windows and macOS (owner decision of
  2026-10-09); a macOS server sends and receives one datagram per system
  call.
- **Typed values.** Type descriptors
  ([vkr_type_desc.h](../../runtime/src/core/vkr_type_desc.h),
  [ADR-076](../adr/076-project-object-model.md)) describe every component:
  at most 64 properties and 1 KiB per value. The collaborative session
  sends descriptor-typed values with exact floats
  ([vkr_net_type.h](../../runtime/src/net/vkr_net_type.h),
  [ADR-106](../adr/106-collaborative-editing-session.md)); nothing
  quantizes or delta-encodes components yet.
- **Simulation.** The scene ticks at a fixed 60 Hz. The FPS module orders
  input commands by tick and sequence without a wire encoding
  ([ADR-073](../adr/073-native-gameplay-foundation.md)). The
  [entity behavior proposal](entity-behavior-system.md#preserving-a-multiplayer-path)
  asks for serializable input, authoritative state kept apart from
  presentation, network identities mapped to local handles, and no
  replication descriptors on every field.
- **Streaming.** World partition supplies streaming sources and 128 m
  cells ([ADR-086](../adr/086-world-partition.md)).
- **Not present.** Replication, snapshots, prediction, interest
  management, entity authority, a headless server executable and
  persistence.

## Model: Star Citizen's server meshing

Cloud Imperium Games described the architecture in public developer
posts. This proposal takes the structure, not the implementation.

| Concept | What it does | Layer here |
|---|---|---|
| Replication layer | A separate, stateful service that holds the replicated state of every entity and streams it to clients and servers. Clients connect to it, not to game servers. | L2 |
| Entity authority | Exactly one simulation server may write an entity. Authority can move between servers. | L3 |
| Server meshing | Many simulation servers, each with authority over an area. A static mesh with fixed areas comes first; a dynamic mesh assigns areas by load. | L3 |
| Object container streaming | The world is a tree of containers (zones, ships, rooms) loaded around streaming sources, on clients and on servers. | L2, L3 |
| Persistent streaming (entity graph) | A graph database stores the state of every replicated entity, so servers are replaceable. | L4 |

Sources: the
[Server Meshing and Persistent Streaming Q&A](https://star-citizen.wiki/Comm-Link:18397/en),
the [Roadmap Roundup of 2023-10-04](https://star-citizen.wiki/Comm-Link:19505/en)
and the
[Letter from the Chairman](https://api.star-citizen.wiki/rsi/comm-links/19848).
CIG describes the static mesh as limited in scale and the dynamic mesh as
later work. This proposal does not depend on their current state.

## Requirements

| Configuration | Players | Metric that matters |
|---|---|---|
| FPS session | 10 to 20, all relevant to each other | Latency, hit registration, bytes per client |
| Persistent world | 10,000+ in total, 200 to 400 in one place | Bytes per client, clients per server, server tick cost |
| Both | An FPS arena inside a persistent world, or a crowded fight in it | The same game code and client in both |

Requirements that follow:

- Game code uses one API in every configuration. It writes the entities it
  has authority over and reads every other entity.
- An FPS session adds no network hop and no work for layers it does not
  use. A layer that is not deployed costs nothing.
- Per-client bandwidth stays within a budget however many entities are
  near the client.
- A simulation server can stop without disconnecting its clients in the
  persistent configuration.

Non-goals: lockstep or cross-platform deterministic simulation,
peer-to-peer game meshes, voice, browser clients.

## Layers

```text
L4  Persistence            entity graph store                  (persistent world)
L3  Authority and meshing  which simulation server writes which entity
L2  Replication, interest  state store, per-client priority, delta snapshots
L1  Prediction             input, replay, interpolation        (client)
L0  Transport              ADR-105 (implemented)
```

Each layer depends only on the layers below it. L1 runs on clients only;
L2 to L4 run on servers.

### L0: transport

Implemented in ADR-105. This proposal needs from it:

- shard bits in the connection ID, so a load balancer or a socket per
  worker can route packets without state;
- server-to-server connections that multiplex many client lanes, with a
  32-bit lane ID in the service header rather than one transport channel
  per client;
- connection migration between replication nodes with a fresh token;
- Registered I/O per worker on Windows servers, measured against the
  one-call-per-datagram baseline.

### L1: prediction and interpolation

- The client sends input on an `UNRELIABLE` channel with `IMMEDIATE`. Each
  packet repeats the last four to eight FpsInput commands with their tick
  and sequence, so a burst of lost packets loses no command.
- The authority holds one to two ticks of input in a jitter buffer and
  reports the buffer's depth. The client changes its simulation rate by up
  to ±5% to keep the depth at its target.
- The client predicts its own player. Each snapshot carries the last input
  sequence that the authority applied, and the client replays later inputs
  from the corrected state. This needs the separation of authoritative and
  presentation state that the entity behavior proposal describes.
- The client draws remote entities two snapshot intervals in the past and
  interpolates between snapshots.
- Hitscan lag compensation keeps one second of hitbox history. In a mesh,
  the replication layer keeps that history, so an authority can check a
  shot at an entity that another server owns.

L1 does not know the topology. A client predicts the same way whether one
process or a mesh serves it.

### L2: replication and interest

The replication layer owns the **entity state store**: the latest
quantized state of every replicated entity and a ring of past states.

- **Network IDs.** The store assigns each replicated entity a session
  `NetId`: a 20-bit index and a 12-bit generation. Each peer maps a
  `NetId` to its local `VkrEntityId`. An entity from a level document is
  named once by its `VkrEntityRef` in its spawn record, because every peer
  loads the same level.
- **Replicated components.** A replicated entity carries a
  `net_replicated` component: priority, relevance radius and update-rate
  tiers. Only types that register a `VkrNetComponentDesc` travel. The
  descriptor names the type and, only where needed, a quantization per
  property; every other property takes a default encoding from its kind:

  | Property kind | Wire encoding |
  |---|---|
  | `BOOL` | 1 bit |
  | `I32`, `U32` | `VARUINT` (zigzag for `I32`) |
  | `F32`, `ANGLE` | `F32`, or `QFLOAT` when the property has a range and the descriptor gives bits |
  | `VEC2`–`VEC4`, `COLOR`, `DIRECTION` | `F32` per component, or `QFLOAT` as above |
  | `QUAT` | `QUAT(15)` |
  | `ENUM` | `ENUM(count of names)` |
  | `STRING` | `UTF8(capacity)` |
  | `ENTITY` | `NETID` |

  `TRANSIENT` properties never travel. A component's change mask has one
  bit per property, at most 64 bits.
- **Ticks.** Simulation runs at the existing 60 Hz. The store sends
  snapshots at a per-session rate from 20 to 60 Hz.
- **Delta snapshots.** The store keeps quantized states for the last 64
  ticks and, per client and entity, the last tick the client
  acknowledged. An update is the difference from the acknowledged state:

  ```text
  update = NetId
         + component change mask
         + per component: property change mask, changed values
  ```

  Clients that acknowledged the same tick receive the same bytes, so the
  store caches each encoded difference by entity, baseline tick and current
  tick. A baseline that left the ring forces a full state. Snapshots travel
  on an `UNRELIABLE` channel and each packet decodes on its own. The
  packet-acknowledged hook advances baselines; the client sends no extra
  acknowledgment. Spawns and despawns repeat until a packet that carried
  them is acknowledged. Gameplay events that need order (damage
  confirmations, inventory changes) use `RELIABLE_ORDERED`.
- **Interest.** Containers are the world partition's cells first, and
  nested containers (vehicle and building interiors) later. A client's
  interest set is the containers near its streaming sources plus entities
  marked always relevant. Each candidate accumulates priority from
  distance, view direction, its own priority and the time since its last
  update. Each snapshot sends the highest-priority updates that fit the
  client's byte budget: the lower of a configured cap and a share of the
  bandwidth estimate.
- **Encoding per entity.** Updates encode per entity, so updates that come
  from several authorities combine into one client packet.

### L3: authority and meshing

- **Authority record.** Each entity in the store has an authority: a
  server ID and an epoch. The store accepts a write only from the current
  authority with the current epoch.
- **Transfer.** The store moves authority at a tick boundary. The old
  authority sends its final state and stops writing; the store raises the
  epoch and names the new authority, which starts from that state. A write
  with an old epoch is refused, so no state is lost or applied twice.
- **Interaction across authorities.** A request to change an entity that
  another server owns (damage, a pickup, a door) goes through the store to
  the owner, which applies or refuses it. Physics contacts across a border
  use an overlap band that both servers load: the owner simulates the
  entity and the neighbour treats it as a kinematic proxy.
- **Server-side streaming.** A simulation server loads only the containers
  of the entities it owns and those within their interest. It runs no
  renderer.
- **Static mesh first.** A configuration file assigns areas to servers. A
  dynamic mesh that splits and merges areas by load comes later and uses
  the same transfer.

### L4: persistence

The entity graph stores every persistent entity's state by its
`VkrEntityRef`, with its parent container. The replication layer writes
changes to it and loads from it, so a restarted simulation server or
replication node resumes from stored state. The storage format is an open
decision.

## Configurations

```text
FPS session (one process):
  client ---- [ L2 + L3: one authority ]

Persistent world:
  client ---- replication node ---- simulation servers (static mesh)
  client ---/        |        \---- simulation servers
                     +---- entity graph (L4)

Both:
  an FPS arena runs as a one-process session; the persistent world's
  replication layer hands its clients over by connection migration
```

The one-process configuration has no extra hop: L2 and L3 run in the same
process and exchange state in memory. The persistent world adds one hop
between client and authority, which suits a persistent world but not a
competitive shooter.

## Phases

| Phase | Scope | Exit evidence |
|---|---|---|
| G1. FPS session | L1, L2 and one in-process authority; a `world` service; opt-in components; prediction for the FPS module | 20 clients (bots) on a Bistro session with measured bytes per client and correction counts under 2% loss |
| G2. Separate replication layer | The store as its own process with one simulation server | The G1 session through a separate replication node: added latency measured; a simulation server restarts without a client disconnect |
| G3. Static mesh | Several simulation servers with fixed areas; authority transfer; requests across authorities | Bots crossing a Bistro area border with no lost or duplicated state; a shot across the border is registered once |
| G4. Scale | Headless server executable for Windows and macOS that simulates without a renderer; server workers; Registered I/O on Windows; 400 co-located bots | Server tick cost, system-call CPU and egress at 400 clients on each host, against the estimate table |
| G5. Persistence | The entity graph | A Bistro world restarts every server and resumes every entity's state |
| G6. Dynamic mesh | Areas split and merged by load | Bots crowd one area; the mesh splits it with no lost state, within the tick budget |

G1 alone is a complete FPS session. Each later phase keeps G1's client and
game code unchanged.

## Performance targets

These are targets for the evidence of each phase, not results.

| Target | Value | Phase |
|---|---|---|
| 20-player session bandwidth | at most 64 kbps down per client | G1 |
| Latency added by a separate replication node | measured; target set in G2 | G2 |
| 400 co-located clients | at most 300 kbps down per client; snapshot encode within the server tick on a Ryzen 5 2600 and on an M1 Pro | G4 |

Estimates for planning, which G1 and G4 must measure. Each assumes 10 B per
entity update and 51 B of packet overhead over IPv4.

| Case | Updates per second per client | Down per client | Server egress |
|---|---|---|---|
| 20 players, all relevant, 30 Hz | 20 × 30 = 600 | about 60 kbps | about 1.2 Mbps |
| 400 in one place: 32 at 30 Hz, 96 at 10 Hz, 272 at 4 Hz | 3,008 | about 250 kbps | about 100 Mbps |
| Input upstream: 60 packets/s, four commands each | — | about 36 kbps up | — |

## Verification

- Each layer gets simulator tests that name their failure, as the
  transport has (ADR-105): lost and reordered snapshots, stale baselines,
  transfers with in-flight writes, refused old-epoch writes.
- Every phase ends with a check over real sockets with bots on Bistro.
- Performance claims use matched Release builds with graphics validation
  off, on Windows and macOS servers.

## Open decisions

Each decision has a recommendation. The owner can change it before the
phase that depends on it.

| Decision | Recommendation | Tradeoff |
|---|---|---|
| Replication layer | Stateful, as in Star Citizen | A stateless gateway that forwards packets is simpler, but a server crash loses state and per-client encoding repeats on every server. |
| Authority unit | Per entity, defaulting to its container's server | Authority per area only is simpler, but vehicles and players crossing borders need per-entity transfer anyway. |
| Physics across borders | Overlap band with kinematic proxies | Running physics in the replication layer removes borders but turns it into a simulation server. |
| Persistence store | Decide in G5; prefer an append log keyed by `VkrEntityRef` over an external database | An external graph database is ready-made but adds a server dependency. |
| Server platforms | Windows and macOS (2026-10-09); Linux later | Linux has `sendmmsg`, `recvmmsg` and cheaper hosting; it needs a server build. |

## Alternatives considered

- **Gateways and cell servers that forward packets.** The earlier plan in
  the network protocol proposal. Rejected as the base: no state survives a
  cell server, and every cell server encodes for every client it serves.
- **Zones with handoff screens.** Classic MMO sharding: simple, but players
  see the seams and cannot interact across a zone border.
- **Hosted spatial simulation platforms.** Similar structure, but an
  external service owns the core of the game.
- **Lockstep or peer-to-peer.** Non-goals: they need cross-platform
  determinism or trust between players.

## Risks

- **The extra hop.** A separate replication node adds latency. The
  one-process configuration keeps competitive FPS sessions free of it.
- **Interaction across authorities.** Shots, contacts and transactions
  across a border are the hardest part of a mesh, and CIG still works on
  them. G3 must prove them before G4 scales.
- **Replication layer capacity.** One node for a crowded area is a
  bottleneck and a single point of failure until G2 and G5 make it
  restartable.
- **Gameplay prerequisites.** Prediction and replay need the state
  separation of the entity behavior proposal before G1 can meet its exit
  evidence.
- **Server throughput on Windows and macOS.** One system call per
  datagram may limit servers beyond the 400-client case. G4 measures it
  before choosing between more Windows workers and a Linux server build.

## Related documents

- [Network protocol](network-protocol.md): the protocol work this
  proposal follows.
- [ADR-105](../adr/105-network-transport-and-asset-depot.md): the
  transport, data format and sessions.
- [ADR-106](../adr/106-collaborative-editing-session.md): descriptor-typed
  values on the wire.
- [ADR-073](../adr/073-native-gameplay-foundation.md): fixed ticks and
  ordered input.
- [ADR-076](../adr/076-project-object-model.md): type descriptors and
  entity references.
- [ADR-086](../adr/086-world-partition.md): streaming sources and cells.
- [Entity behavior proposal](entity-behavior-system.md): the multiplayer
  boundaries.
