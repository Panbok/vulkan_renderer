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
between servers, and persistent entity state. The parts that run per client
and per entity come from shooters and MMOs that shipped them; the
[sources](#other-systems-this-design-takes-from) name each one.

The owner deferred this work on 2026-10-16. It starts after the
[network protocol](network-protocol.md) phases for editor collaboration,
asset streaming and view streaming, and builds on the transport of
[ADR-105](../adr/105-network-transport-and-asset-depot.md). Names of types
and functions are provisional until the phase that adds them.

## Current baseline

An audit of the code on 2026-10-16 found the transport ready for this work
and the gameplay, physics and streaming code not ready. Phase G0 closes the
gaps listed here.

- **Transport (ADR-105).**
  - Encrypted UDP connections with five delivery classes (`UNRELIABLE`,
    `SEQUENCED`, `RELIABLE_ORDERED`, `RELIABLE_UNORDERED`, `DEADLINE`) on
    up to 64 channels. A session service gets at most 16 channels
    ([vkr_net_session.h](../../net/src/vkr_net_session.h)).
  - Unreliable and sequenced messages must fit one datagram: 1,200 B,
    or up to 1,452 B after PMTU discovery
    ([vkr_net_core.h](../../net/src/vkr_net_core.h)).
  - `VKR_NET_SEND_NOTIFY` reports `VKR_NET_EVENT_ACKED` or
    `VKR_NET_EVENT_LOST` for the packet that carried a message, with a
    64-bit tag. A packet declared lost and acknowledged later raises no
    `ACKED` event.
  - The bandwidth estimate, pacing rate and congestion window are readable
    per path.
  - A connection ID holds a 4-bit shard, 12 random bits and a 16-bit slot.
    One host has one socket and one single-threaded core. Nothing routes
    datagrams to a core by shard, and a connection cannot move to another
    process.
  - Each connection tracks `sent_packet_capacity` packets (default 4,096),
    about 768 KiB per connection before channel buffers.
  - Servers run on Windows and macOS (owner decision of 2026-10-09). Each
    system call sends or receives one datagram
    ([vkr_udp_socket.h](../../lib/src/platform/vkr_udp_socket.h)).
- **Wire codec.** The bit stream and the exact-schema codec have `VARUINT`,
  zigzag `INT`, `NETID`, `QFLOAT`, `VEC3Q`, `QUAT` (smallest three) and
  `ENUM` kinds ([vkr_wire.h](../../net/src/vkr_wire.h)). `NETID` is a plain
  varuint. The codec writes every field in order: it has no change masks,
  no delta against a baseline and no quantized `VEC2` or `VEC4`. A 64-bit
  hash of the schema selects the version.
- **Typed values.**
  - Type descriptors
    ([vkr_type_desc.h](../../runtime/src/core/vkr_type_desc.h),
    [ADR-076](../adr/076-project-object-model.md)) describe components: at
    most 64 properties and 1 KiB per value, with an optional range per
    property.
  - `VKR_PROPERTY_FLAG_TRANSIENT` marks runtime state that is never
    serialized, read, undone or edited. Most state that a game must
    replicate is of this kind.
  - Script types with `VKR_TYPE_FLAG_TOLERANT` change layout on hot reload
    ([ADR-079](../adr/079-c-script-modules.md)).
  - The collaborative session sends descriptor-typed values with exact
    floats and no type hash
    ([vkr_net_type.h](../../runtime/src/net/vkr_net_type.h),
    [ADR-106](../adr/106-collaborative-editing-session.md)). It sends
    nothing while the simulation runs.
- **Simulation and gameplay** ([ADR-073](../adr/073-native-gameplay-foundation.md)).
  - The scene ticks at a fixed 60 Hz, at most 8 ticks per update. Every
    tick steps the whole scene: animation, physics and callbacks
    ([vkr_scene_simulation.h](../../runtime/src/renderer/systems/vkr_scene_simulation.h)).
  - Nothing saves and restores scene, ECS or physics state. A character
    can be created, destroyed, stepped once per tick and read, but not set
    to a corrected state ([vkr_physics.h](../../runtime/src/physics/vkr_physics.h)).
  - A physics world holds at most 64 characters (`VKR_PHYSICS_MAX_CHARACTERS`).
  - Rays do not hit characters, and damageable character proxies do not
    exist. A character cannot be a kinematic body.
  - The FPS module queues `FpsCommand` transitions (`tick`, `sequence`,
    `action`, `pressed`, `x`, `y`) without a controller identity. A late
    or reordered command faults the stream
    ([fps_input.h](../../scripts/fps/src/fps_input.h)).
  - FPS player and door state live in untyped runtime state
    (`vkr_state_add`, size and alignment only), which no descriptor
    describes.
  - The [entity behavior proposal](entity-behavior-system.md#preserving-a-multiplayer-path)
    asks for commands with controller identity, an explicit random seed,
    no wall-clock reads, network IDs mapped to local handles, rollback
    state, and no replication descriptors on every field.
- **Entities.** `VkrEntityId` is `[world:16 | generation:16 | index:32]`
  ([vkr_entity.h](../../runtime/src/core/vkr_entity.h)); `VkrEntityRef` is
  a 16-byte document UUID. Nothing tracks component changes except a
  transform dirty flag. Scripts and behaviors write components through raw
  mutable pointers (`vkr_component_get_mut` in [sdk.h](../../sdk/sdk.h)),
  so no setter can check authority or mark changes.
- **Streaming** ([ADR-086](../adr/086-world-partition.md)). 128 m cells,
  one loaded cell set per scene, at most 4 streaming sources
  (`VKR_SCENE_STREAM_SOURCES_MAX`) and a default budget of 256 loaded
  cells. Entities spawned
  at runtime belong to the persistent layer and never stream. During Play
  the origin rebases to follow one camera.
- **Headless.** `vkr_runtime` links the renderer. Level loading, streaming
  and the script host run in the renderer-owned sample runtime. A headless
  editor still renders offscreen on the GPU. CPU tests build scenes with
  physics and no GPU, so a GPU-free server is possible but does not exist.
- **Not present.** Replication, snapshots, prediction, interest
  management, entity authority, a GPU-free server executable and
  persistence.

## Model: Star Citizen's server meshing

Cloud Imperium Games (CIG) described the architecture in public developer
posts. This proposal takes the structure, not the implementation.

| Concept | What it does | Layer here |
|---|---|---|
| Replication layer | A separate, stateful service that holds the replicated state of every entity and streams it to clients and servers. Clients connect to a gateway in front of it, not to game servers. | L2 |
| Entity authority | Exactly one simulation server may write an entity. Authority can move between servers. | L3 |
| Server meshing | Many simulation servers, each with authority over an area. A static mesh with fixed areas comes first; a dynamic mesh assigns areas by load. | L3 |
| Object container streaming | The world is a tree of containers (zones, ships, rooms) loaded around streaming sources, on clients and on servers. The replication layer tells each client which containers to load. | L2, L3 |
| Persistent streaming (entity graph) | A graph database stores the state of every replicated entity, so servers are replaceable. | L4 |

The 2021 Q&A splits the replication layer into services. CIG first ran
them combined as one process, the Hybrid, "to reduce the number of moving
parts". Every live shard still ran one Hybrid at the time of the
research.

| CIG service | Role | Restart |
|---|---|---|
| Gateway | Holds client connections and forwards packets. A client can be served by several replicants. Holds no game state. | Seconds |
| Replicant | Streams entity state; runs no game logic, animation or physics. | Rebuilt from the entity graph, with visible snapping |
| Atlas | Named only; read as the service that assigns authority. | Not published |
| Scribe | Named only; read as the writer of ordered, durable mutation queues to the entity graph. | Not published |

Sources: the
[Server Meshing and Persistent Streaming Q&A](https://star-citizen.wiki/Comm-Link:18397/en)
of 2021-11-10, the
[Roadmap Roundup of 2023-10-04](https://star-citizen.wiki/Comm-Link:19505/en),
the [Alpha 3.18 postmortem](https://star-citizen.wiki/Comm-Link:19471/en),
the after-action reports of meshing tests
[D](https://robertsspaceindustries.com/spectrum/community/SC/forum/3/thread/server-meshing-test-d-after-action-report)
and
[E](https://robertsspaceindustries.com/spectrum/community/SC/forum/3/thread/server-meshing-test-e-after-action-report)
(October 2024), the [Alpha 4.0 notes](https://star-citizen.wiki/Comm-Link:20360/en),
the [Alpha 4.0.1 notes](https://starcitizen.tools/Star_Citizen_Alpha_4.0.1)
and the
[Letter from the Chairman](https://api.star-citizen.wiki/rsi/comm-links/19848).
This proposal does not depend on the current state of CIG's services.

### What Star Citizen learned in practice

CIG's own reports show where the model failed. Each failure gives a rule
that this design applies from its first phase.

| What happened | Source | Rule here |
|---|---|---|
| One Hybrid process per shard: a Hybrid crash disconnected every client of the shard. | Q&A; tests C and D | The store is restartable without disconnecting clients: a gateway keeps the connections, and authorities refill the store ([process roles](#process-roles)). |
| In test E, players who crossed a server border on overloaded servers never reached the new authority and were lost. | Test E report | A transfer has a deadline and aborts back to the old authority; an entity always has exactly one authority ([transfer](#transfer)). |
| One network message queue carried state, binds and remote calls; binds and messages grew as shards aged, and the queue had to be replaced in 3.24. | Test reports of 2024 | Separate bounded queues per traffic kind, with backpressure; queue depth and entity count are soak-test metrics. |
| Large streaming batches stalled fan-out and teleported players. | Test D report | Spawns and container loads share the per-client byte budget; no batch blocks state updates. |
| Transit, elevators, missions and social systems assumed one server; transit "was not designed for Server Meshing" and was rebuilt in 2026. | 4.0 known issues; 4.0.1 notes; 2026 tech talk | Game code asks "do I have authority over this entity", never "am I the server", and cross-authority requests exist from G1. |
| Server recovery in 3.24 abandoned all missions. | 3.24.1 notes | Missions and inventory are persistent state that survives a server restart (L4). |
| In 3.18 the backend saw more than 20 times the expected load; a deadlock on one database server stalled the shard cluster; global inventory locks halted requests; unknown entity limits forced shard reseeding. | 3.18 postmortem | Persistence is per shard with no global locks; entity counts have budgets and lifetimes; bot load tests run before any release. |
| A static mesh does not help a crowd: one territory server "runs slower and slower" under crowding (August 2026). | Letter from the Chairman, 2026-08-27 ([mirror](https://galacticcitadel.com/2026/08/27/letter-from-the-chairman/)) | A crowd has an explicit overload policy ([overload](#overload)). |
| Movement became server-authoritative with prediction only in 3.17.2, as a retrofit. | Community reading of the 3.17.2 notes | Server-authoritative movement with prediction from G1. |

Comparable systems point the same way. SpatialOS put authority per
component and interest queries in a hosted service; its games depended on
that service's pricing and hosting. EVE Online slows a whole node with time
dilation, down to 10% of real time, and asks players to book large fights
so that CCP can move the system to a stronger node. New World suffered
exploits from a server that waited on client input. These sources are
listed in the [alternatives](#alternatives-considered).

## Other systems this design takes from

| Technique | Taken from | Source |
|---|---|---|
| Input redundancy, a server input buffer, client tick-rate adjustment of about 5% | Overwatch; Unity Netcode for Entities | [Overwatch summary](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback), [Unity time sync](https://docs.unity3d.com/Packages/com.unity.netcode@1.0/manual/time-synchronization.html) |
| Lag compensation with a capped rewind | Source; Counter-Strike (`sv_maxunlag` 0.2 s); Valorant | [Valorant netcode](https://www.riotgames.com/en/news/peeking-valorants-netcode) |
| Delta against an acknowledged baseline, tracked per object | Quake 3; Fiedler | [Snapshot compression](https://gafferongames.com/post/snapshot_compression/), [per-object baselines](https://gafferongames.com/post/networked_physics_in_virtual_reality/) |
| Priority accumulator | Fiedler; Unreal Iris; Halo: Reach | [State synchronization](https://gafferongames.com/post/state_synchronization/), [Iris prioritization](https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-prioritization-in-unreal-engine) |
| Filtering before prioritization; quantize once and share work across connections | Unreal Iris; Replication Graph | [Iris components](https://dev.epicgames.com/documentation/en-us/unreal-engine/components-of-iris-in-unreal-engine), [Replication Graph](https://dev.epicgames.com/documentation/unreal-engine/replication-graph-in-unreal-engine) |
| Interest hysteresis: subscribe on an inner area, unsubscribe on an outer one | Albion Online | [Albion, part 2](https://davidsalz.de/secrets-of-albion-online-part-2/) |
| State, events and control as separate replication kinds | Halo: Reach; Tribes | [Tribes model](https://www.gamedevs.org/uploads/tribes-networking-model.pdf) |
| Re-flag lost properties instead of keeping baselines | Tribes, Torque | [Tribes model](https://www.gamedevs.org/uploads/tribes-networking-model.pdf) |
| Visibility filtering of the snapshot stream against wallhacks | Valorant | [Valorant fog of war](https://www.riotgames.com/en/news/demolishing-wallhacks-valorants-fog-war) |
| Time dilation under overload | EVE Online | [Time dilation](https://wiki.eveuniversity.org/Time_dilation) |
| Guaranteed minimum rates for nearby characters | Guild Wars 2 removed distance culling that hid nearby players | [End of culling](https://www.guildwars2.com/en/news/world-vs-worldthe-end-of-culling/) |

## Requirements

| Configuration | Players | Metric that matters |
|---|---|---|
| FPS session | 10 to 20, all relevant to each other | Latency, hit registration, bytes per client, server tick cost |
| Persistent world | 10,000+ in total, 200 to 400 in one place | Bytes per client, clients per server, server tick cost |
| Both | An FPS arena inside a persistent world, or a crowded fight in it | The same game code and client in both |

Requirements that follow:

- Game code uses one API in every configuration. It writes the entities it
  has authority over, reads every other entity, and sends requests to
  change entities that another authority owns.
- An FPS session adds no network hop and no work for layers it does not
  use. A layer that is not deployed costs nothing.
- Per-client bandwidth stays within a budget however many entities are
  near the client. Nearby combatants keep a minimum update rate inside
  that budget.
- The server never trusts the client's clock, view time or movement.
- Every queue between processes is bounded and reports its depth.
- A simulation server or a replication store can stop without
  disconnecting clients in the persistent configuration.
- Overload degrades update rates and simulation detail in a defined order
  before it slows the simulation.

Non-goals: lockstep or cross-platform deterministic simulation,
peer-to-peer game meshes, voice, browser clients, client-side anti-cheat
software, and the account, login and matchmaking services that issue
connect tokens.

## Decisions G1 must get right

A later phase can change a server process, but not what every client and
every line of game code already depends on. G1 fixes these contracts:

1. **Authority, not role.** Game code asks whether it has authority over
   an entity. Writes to an entity without authority become requests.
2. **Global ticks.** Every state sample, event and input names a 64-bit
   shard tick. The wire sends the low 16 bits relative to a recent
   acknowledged tick.
3. **Typed replicated state.** Every replicated value is a property of a
   typed component with a net descriptor; no replicated state lives in an
   untyped blob.
4. **Container-relative positions.** Positions on the wire are relative to
   the entity's parent container or its 128 m cell, so worlds larger than
   float precision need no wire change.
5. **The `NetId` wire form.** Snapshot updates send indexes in ascending
   order as gaps, and spawn records add the generation. Events and
   requests carry the full `NetId`.
6. **Updates name their baseline.** Each entity update names the tick of
   the state it was encoded against, so a client never applies a
   difference to the wrong state.
7. **Events carry ticks and wait for spawns.** An event never refers to an
   entity that the client cannot resolve.

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

- **A game connection profile.** The ring of tracked packets must cover
  the worst probe timeout plus the reorder window at the connection's
  packet rate. When the ring is full, the core sends nothing. A client in
  a 400-player crowd receives about 90 snapshot packets per second plus
  reliable traffic, so 512 tracked packets cover about 4 s. That costs
  96 KiB per connection instead of 768 KiB, and about 38 MiB at 400
  clients.
- **Snapshot acknowledgment.** Snapshot messages use `VKR_NET_SEND_NOTIFY`
  with the snapshot sequence in the tag. `ACKED` promotes baselines.
  `LOST` re-marks the entities that the packet carried. Because a late
  acknowledgment after `LOST` raises no event, a spawn can arrive twice;
  clients apply spawns idempotently.
- **No fragmented snapshots.** A snapshot that exceeds one datagram splits
  into several packets that each decode on their own.
- **Routing by shard.** A dispatcher that sends datagrams to a worker core
  by the shard bits, and a host with several cores.
- **Server-to-server lanes.** Connections that multiplex many client lanes,
  with a 32-bit lane ID in the service header rather than one transport
  channel per client.
- **Connection handoff.** A connection that moves to another gateway or
  arena process with a fresh connect token. ADR-105 only migrates an
  address inside one core, so this needs its own design.
- **Registered I/O.** Registered I/O per worker on Windows servers,
  measured against the one-call-per-datagram baseline.

### L1: prediction and interpolation

L1 does not know the topology. A client predicts the same way whether one
process or a mesh serves it.

#### Input

- Each tick the client samples one command: a button bitmask, a press
  counter per button, aim angles and the tick. The command is state, not
  a transition, so a lost or repeated command cannot fault the stream.
  The press counters keep a tap that starts and ends between two samples.
  The FPS module derives its `FpsCommand` transitions from consecutive
  commands on each peer.
- Each input packet carries every command that the server has not
  acknowledged, delta-coded against the previous command, up to a byte
  cap. It travels on an `UNRELIABLE` channel with `IMMEDIATE`. A fixed
  window of four to eight commands covers only 67 to 133 ms of burst
  loss.
- The authority admits commands in tick order and drops duplicates by
  tick. Each command carries the controller identity that the entity
  behavior proposal asks for.
- The authority holds one to two ticks of input in a jitter buffer and
  reports the buffer's depth. The client changes its simulation rate by up
  to ±5% to keep the depth at its target, as Overwatch and Unity do.
- When the buffer is empty, the authority repeats the last command. The
  server has no rollback, so it drops a command that arrives for a tick it
  has already simulated, and the client receives a correction.
- The authority consumes at most one command per server tick, plus a
  bounded catch-up debt. A client that runs its clock fast gains nothing
  (speed hack).
- The authority validates each command: aim deltas, fire rate against the
  weapon state, and tick range.

#### Prediction

- The client predicts its own player, the player's weapons and the
  entities that the player spawns. Each snapshot carries the last input
  tick that the authority applied.
- On a correction the client restores its player from the snapshot and
  replays later commands. Replay covers only the predicted entities: the
  scene steps as a whole today, so G0 adds a step for one character and a
  way to set its state.
- The state of predicted entities is quantized with the store's
  quantizers after every tick, on the authority and on the client. Without
  this, every replay from a server state diverges from the original run.
- Replay length follows the RTT. A session sets the highest RTT it admits,
  250 ms (15 ticks at 60 Hz) as a starting value, and warns or refuses
  clients above it. A client whose replay would exceed the cap snaps to
  the server state.
- The drawn position hides corrections: the visual error decays over a few
  frames and never feeds back into the simulation.
- A predicted spawn, such as a projectile, carries a local key. The
  authority's spawn record returns the key, and the client adopts or
  destroys its predicted entity when the record arrives.
- This needs the separation of authoritative and presentation state that
  the entity behavior proposal describes.

#### Interpolation

- The client draws remote entities in the past and interpolates each
  entity between its own received samples, by the shard tick of each
  sample. A low-priority entity can update every 5 to 20 snapshots, so a
  fixed "two snapshot intervals" delay is wrong for it.
- The delay adapts to measured jitter and loss: about three send
  intervals for the competitive tier, longer for slow rate tiers.
  Extrapolation is clamped to 250 ms.
- Every millisecond of delay adds to the peeker's advantage, so the delay
  is measured and reported, not fixed generously.

#### Lag compensation

- The client sends, with each shot, the tick and the fraction that it
  drew. The server sets the range of interpolation delay that a client may
  use. The authority checks the view time against the measured RTT and
  that range, and refuses a shot outside the bound.
- The authority keeps one second of hitbox history and rewinds at most
  200 ms, starting values that G1 measures (Counter-Strike's
  `sv_maxunlag` is 0.2 s). Beyond the cap the shooter is no longer
  favored.
- A swept-volume broad phase over the history selects candidates; only
  those rewind fully.
- The history stores hitbox poses at a reduced rate and interpolates
  between them, as Valorant does with animation every fourth frame.
- In a mesh, the store keeps the hitbox history of every entity. Each
  authority keeps a local copy for the entities in its border band, so it
  checks a shot at an entity that another server owns without waiting.
- Hit registration needs rays that hit characters and damageable
  character proxies (G0).

### L2: replication and interest

The replication layer owns the **entity state store**: the latest
quantized state of every replicated entity, the per-client baselines, and
the interest sets.

#### Network IDs

- The store assigns each replicated entity a session `NetId`: a 20-bit
  index and a 12-bit generation. Each peer maps a `NetId` to its local
  `VkrEntityId`.
- A snapshot lists entities in ascending index order and encodes each
  index as the gap from the previous one. The generation travels in the
  spawn record. Events and requests travel on other channels and can
  arrive late, so they carry the full `NetId` with its generation.
- An index returns to the free list only after every client that held the
  entity has acknowledged its despawn or disconnected.
- Spawn and despawn records carry their tick. A client keeps the tick of
  the last record per index and ignores an older record, so a delayed
  despawn cannot remove a newer spawn.
- The directory allocates `NetId` ranges per shard, so every store and
  authority of the shard uses one name for an entity.
- An entity from a level document is named once by its `VkrEntityRef` in
  its spawn record, because every peer loads the same level. A client
  checks the content version (the depot commit) when it joins and is
  refused on a mismatch.
- A persistent entity that no client or server observes needs no `NetId`.
  The store assigns one when the entity enters its live set, so the 20-bit
  index limits live entities, not stored ones. An entity spawned at runtime
  receives a new `VkrEntityRef` when it first persists.

#### Replicated state

- A replicated entity carries a `net_replicated` component: priority,
  relevance radii and update-rate tiers.
- Only types that register a `VkrNetComponentDesc` travel. The descriptor
  names the type, the properties that travel, and a quantization where
  the default is wrong. By default every property except a `TRANSIENT` one
  travels. A `TRANSIENT` property travels only when the descriptor names
  it: it keeps its editor meaning (not saved, not edited) and is still game
  state.
- FPS player and door state move from untyped runtime state into typed
  components (G0).
- Default encodings follow the property kind:

  | Property kind | Wire encoding |
  |---|---|
  | `BOOL` | 1 bit |
  | `I32`, `U32` | `VARUINT` (zigzag for `I32`) |
  | `F32`, `ANGLE` | `F32`, or `QFLOAT` when the property has a range and the descriptor gives bits |
  | `VEC2`–`VEC4`, `COLOR`, `DIRECTION` | `F32` per component, or `QFLOAT` as above; the codec needs quantized `VEC2` and `VEC4` |
  | Position | Relative to the parent container or the 128 m cell; 17 bits per ground axis give 1 mm in a cell, and height has its own range (23 bits give 1 mm over 8 km) |
  | `QUAT` | `QUAT(15)` |
  | `ENUM` | `ENUM(count of names)` |
  | `STRING` | `UTF8(capacity)` |
  | `ENTITY` | `NETID`, through a table from `VkrEntityRef` to `NetId` |

- A component's change mask has one bit per property, at most 64 bits.
- The world service's schema hash covers every net descriptor. A script
  hot reload that changes a replicated type's layout is refused while a
  session is connected.

#### Change detection

Components are written through raw pointers, so no setter can mark
changes. After each tick the authority compares the raw bytes of each
replicated component of its owned entities with a copy from the previous
tick. It quantizes only the changed components and sets property change
bits from the quantized values. The cost grows with the replicated state
that the authority owns, and G1 measures it.

On a peer without authority, the next applied state overwrites a local
write to a replicated component. Debug builds report such writes.
Predicted entities are the exception: their corrections go through
replay.

#### Ticks and rates

Simulation runs at the existing 60 Hz. The store sends snapshots at a
per-session rate from 20 to 60 Hz, and each entity at the rate of its
tier.

#### Baselines and delta snapshots

- For each pair of client and entity in interest, the store keeps the last
  quantized state that the client acknowledged. An update is the
  difference from it, and names it:

  ```text
  update = NetId gap
         + baseline tick (relative), or none for a full state
         + component presence changes (added with full value, removed)
         + component change mask
         + per component: property change mask, changed values
  ```

- The client keeps the states it received per entity and decodes each
  update against the state that the update names. With about six packets
  in flight at 60 Hz and 100 ms RTT, the newest received state is often
  not the acknowledged one. The client discards states older than the
  newest baseline that the server named. The server never names a
  baseline older than a configured age; it sends a full state instead.
- Each sent packet keeps a record of the entities and states it carried.
  `ACKED` copies them into the client's baselines. `LOST` raises those
  entities' priority so they go out again with current values. A packet
  declared lost and acknowledged later promotes nothing, so the baseline
  only stays older.
- An entity without a baseline for the client travels as a full state
  (its spawn).
- Work shared by all clients runs once per tick per entity: change
  detection, quantization and value encoding. Per-client work is the
  comparison with the baseline and a bit copy.
- The original plan kept quantized states for the last 64 ticks
  (1.07 s) and cached encoded differences by entity, baseline tick and
  current tick. An entity updated every one to two seconds would often
  lose its baseline from such a ring, and clients with different RTTs and
  priorities rarely share baseline ticks. The cache comes back only if
  G1 measures a useful hit rate.
- Memory is clients × entities in interest × quantized size, plus the
  records of states in flight ([memory estimates](#performance-targets)).
- Snapshots travel on an `UNRELIABLE` channel and each packet decodes on
  its own. Spawns and despawns repeat until a packet that carried them is
  acknowledged.

#### Interest

- **Relevance first.** The store keeps a spatial grid of cells finer than
  the world partition's 128 m cells, sized by G1. Relevance radii belong
  to each entity. The query covers the grid cells within the largest
  radius of a size class, and a short list holds entities with long
  radii, such as large ships. Entities marked always relevant (the
  client's own player, its team and objectives) are candidates too. An
  entity enters at its inner radius and leaves at its outer radius, so it
  does not flicker at the edge.
- **Visibility.** A competitive session can also filter candidates by
  visibility, using the rooms and portals of
  [ADR-115](../adr/115-rooms-and-portals.md), so the snapshot stream does
  not carry wallhack data. A hidden entity stays spawned and its updates
  are withheld, so visibility changes cause no spawn churn. The filter
  looks ahead by the client's latency so enemies do not appear late at
  corners. A hidden entity that makes a sound sends a position for audio
  only.
- **Priority.** Each candidate accumulates priority from distance, view
  direction, its own priority and the time since its last update.
  Priority is computed per grid cell where the entities in it share a
  distance, then per entity for the nearest cells.
- **Rate tiers.** Tiers are hard maximum send rates chosen by distance
  and view. The starting tiers of 60, 20 and 5 Hz are values that G1
  measures. Entities that recently hit, damaged or aimed at the client
  keep a minimum rate from a reserved share of the budget. When that
  share is full, the nearest of them keep it.
- **Budget.** Each snapshot sends the highest-priority updates that fit
  the client's byte budget: the lower of a configured cap and a share of
  the bandwidth estimate. Spawns share the budget; a container's spawn
  goes before its children's.
- **Containers.** Containers are the world partition's cells first, and
  nested containers (vehicle and building interiors) later.

#### Events

| Kind | Example | Delivery |
|---|---|---|
| State | Position, health, door open | Snapshots |
| Cosmetic event | Muzzle flash, impact, footstep | `UNRELIABLE`, sent only to clients whose interest holds the entity, dropped after a deadline |
| Gameplay event | Damage confirmation, inventory change, kill | `RELIABLE_UNORDERED` with a sequence per system; the receiver orders each system's events, so one system's loss does not block another, and the 16 channels per service suffice |
| Control | Input commands | L1 input |

Every event carries the tick at which it happened. The client applies an
event when its timeline reaches that tick: the interpolation timeline for
remote entities, the prediction timeline for its own. An event that names
an entity the client has not spawned waits for the spawn for a bounded
time, then counts as dropped. The store sends an event about an entity
only to clients whose interest holds that entity.

#### Hierarchy and coordinates

- The parent link of an attached entity (a weapon in a hand, a player in
  a ship) is replicated state. A child spawns after its parent, and a
  client interpolates the child in its parent's space.
- Servers run with a fixed origin. Positions on the wire are relative to
  the parent container or the cell. Cells are square on the ground plane,
  so the height axis needs its own range: 23 bits give 1 mm over 8 km. A
  change of cell sends the new cell index.

#### Level entities and client streaming

A client streams world partition cells around its own player. The store
cannot send state for a level entity in a cell that the client has not
loaded. The client reports its loaded cells on a reliable channel, and the
store sends level-entity state only for reported cells. When a client
reports a cell unloaded, the store drops that client's baselines for the
cell's entities. Dynamic entities do not depend on the client's cells.

A level entity that the game destroys must stay destroyed when a client
loads its cell later. The store keeps a tombstone per destroyed level
entity in its cell, sends the cell's tombstones when a client reports the
cell loaded, and persists them in L4.

### L3: authority and meshing

#### Shard clock

The directory owns the shard clock, and every authority ticks on it. A
transfer happens at a tick boundary that both servers count the same way.

An authority that misses a tick runs behind the clock and catches up with
up to 8 ticks per update, the scene's existing limit. An authority that
lags more than a configured number of ticks starts the
[overload](#overload) steps.

Bounds stated in ticks (transfer deadlines, replay length, history length)
count shard ticks. Bounds stated in milliseconds (rewind cap, lease
timeout, request timeout) count wall time. This difference matters only
under time dilation.

#### Authority record

Each entity has an authority: a server ID and an epoch. The directory is
the only writer of authority records, epochs and leases. Each entity has
one primary store, which holds a copy of its authority record and accepts
a write only from the current authority with the current epoch.

Each simulation server holds one lease for all its entities and renews it
with a heartbeat. The heartbeat does not depend on writes, so a server
whose entities are idle keeps them. When a lease expires, the directory
gives the server's area to a standby server, which loads the area's cells
and continues from the store's state with a raised epoch. The area's
entities freeze until the standby takes over. A server that comes back
cannot write, because its epoch is old.

#### Transfer

The unit of transfer is an authority group: a root entity and its
attached descendants, such as a ship with its passengers or a player with
its weapons. A group moves as a whole at one tick, so attachments never
span two authorities.

1. The directory names a new authority B and a transfer tick T for a
   group that authority A owns at epoch e.
2. A simulates through T, writes the final state with epoch e, and stops
   writing. The final state includes the last applied input tick, the
   jitter buffer's commands and the IDs of the requests that A has
   applied. A keeps its full state of the group frozen until the transfer
   commits or aborts.
3. From T, the store holds the group's input and requests in a buffer.
4. The store sends the state to B. B loads the group and confirms. The
   directory raises the epoch, and the store releases the buffer to B. B
   resumes from the state at T.
5. If B does not confirm within a deadline of a few ticks, the directory
   aborts the transfer and returns the group to A with a new epoch. A
   resumes from its frozen state and the store releases the buffer to A.
   A group never stays without an authority.

A write with an old epoch is refused, so no state is lost or applied
twice.

#### Interaction across authorities

- A request to change an entity that another server owns (damage, a
  pickup, a door) goes through the store to the owner, which applies or
  refuses it.
- A request carries the sending entity, a sequence and the sender's
  authority epoch. The sequence counter is part of the sending entity's
  state, so it moves with the entity in a transfer. The store refuses a
  request with an old epoch.
- The owner applies each request at most once. The IDs of applied
  requests travel in the final state of a transfer, so the new owner does
  not apply a request again. The store forwards pending requests to the
  new owner.
- Game code never waits for a reply in a tick. A request completes,
  is refused, or has an unknown outcome after a timeout. A retry reuses
  the request's ID, so a retried pickup cannot take an item twice.
- Physics contacts across a border use an overlap band that both servers
  load: the owner simulates the entity and the neighbour treats it as a
  kinematic proxy. Characters cannot be kinematic bodies today, so the
  proxy needs a new physics object kind.
- Lifts, transit, missions and other systems that span areas use the same
  requests from the start. CIG rebuilt its transit system because it
  assumed one server.

#### Overload

When an authority cannot hold its tick, it applies these steps in order
and reports each step:

1. The store lowers rate tiers for distant entities.
2. The authority sheds optional simulation: AI detail, debris, distant
   animation.
3. The area refuses new entries and sends arrivals to another instance.

A fourth step that slows the simulation is an
[open decision](#open-decisions). Shard-wide time dilation, as EVE Online
uses, keeps one clock but slows every player of the shard for one crowd.
Its factor goes beyond the ±5% rate adjustment of L1, so every snapshot
must carry the factor, and the factor needs a floor. Dilation per area
keeps other areas at full speed but needs different tick rates across
borders, which contradicts the single shard clock.

A dynamic mesh cannot split a crowd smaller than its interaction radius.
Two authorities in one dense fight would turn every contact and shot into
a request across authorities, so G6 measures the split against the
overload steps.

#### Server-side streaming

A simulation server loads the cells of its area and of the border band,
with a fixed origin, and runs no renderer. World partition's four
streaming sources and one cell set per scene do not serve this, so G0 adds
area-driven loading.

#### Static mesh first

A configuration file assigns areas to servers. A dynamic mesh that splits
and merges areas by load comes later and uses the same transfer.

### L4: persistence

- The entity graph stores every persistent entity's state by its
  `VkrEntityRef`, with its parent container. The persistence writer
  appends the store's changes to it, and the store loads from it, so a
  restarted simulation server or store resumes from stored state.
- **Economic operations commit first.** A trade, a purchase, a pickup into
  an inventory or any change of an item's owner commits to durable storage
  before clients see it. A crash after clients saw the result therefore
  cannot roll it back.
- **One coordinator per operation.** An operation that spans authorities
  goes to the persistence writer as coordinator. The coordinator holds
  the items in escrow, commits one record with an operation ID, and
  releases them. A replayed record with a known ID does nothing, so a
  crash cannot duplicate or lose an item.
- **One shard per player.** A player record is checked out to one shard
  at a time under a lease, so one account cannot play in two shards.
- Mission state is written when it changes. Transforms are written at an
  interval.
- Each shard has its own store with no locks across shards.
- Each container has an entity budget, and debris has a lifetime.
- The storage format is an open decision.

## Process roles

The persistent world runs the replication layer as separate roles. The
one-process session runs the same roles as modules of one process.

| Role | CIG name | State it holds | When it restarts |
|---|---|---|---|
| Gateway | Gateway | Client connections and keys; routing of each client to the stores that serve its interest | Its clients reconnect with a fresh token |
| Store | Replicant | Replicated state of the entities for which it is the primary store, baselines, interest, hitbox history | Clients keep their connections at the gateway. The gateway sends them a reset: they drop the store's entities and receive them again as spawns within their budgets. Authorities send the state of their entities again; persistence supplies the rest. |
| Directory | Atlas | The only writer of authority records, epochs, leases, `NetId` ranges and the shard clock | Rebuilt from the copies that stores hold; no transfer starts until it is back |
| Persistence writer | Scribe | A durable queue of changes for L4 | Replays its queue |
| Simulation server | Game server | The entities it owns | Its lease expires, and the directory gives its area to a standby server |

The store can be refilled from the authorities. CIG's replicant restarts
from the entity graph instead, and its clients see the world snap.

## Configurations

```text
FPS session (one process):
  client ---- [ L2 + L3: one authority ]

Editor play:
  editor ---- [ L2 + L3 ] + N clients in one process,
              over the transport simulator's links

Persistent world:
  client -- gateway -- store -- simulation servers (static mesh)
  client --/             |  \-- simulation servers
                         +---- directory, persistence writer -- L4

Both:
  an FPS arena runs as a one-process session; the gateway hands its
  clients over to the arena process
```

The one-process configuration has no extra hop: L2 and L3 run in the same
process and exchange state in memory. The persistent world adds one hop
between client and authority, which suits a persistent world but not a
competitive shooter.

Editor play runs a session and several clients in the editor process, over
the deterministic transport simulator with configured latency, jitter and
loss. A game developer tests prediction and corrections without a second
machine.

## Phases

| Phase | Scope | Exit evidence |
|---|---|---|
| G0. Prerequisites | A server executable that links no renderer; typed FPS player and door state; a step for one character and a way to set its state; rays that hit character proxies; input as per-tick state with dedupe; area-driven cell loading with a fixed origin | CPU tests: a character restored and replayed for 15 ticks matches the original run within quantization; the server loads Bistro and ticks at 60 Hz without a GPU device; a ray hits a character proxy; duplicate and reordered commands do not fault the input stream |
| G1. FPS session | L1, L2 and one in-process authority; a `world` service; net descriptors; editor play; a bot swarm (many bot clients in one process) | 20 bots on a Bistro session at 100 ms RTT with jitter and 2% loss: bytes per client, correction rate, server-confirmed hits against client-reported hits, and server tick cost; if the open decision holds, the same session with two in-process authorities split by area |
| G2. Separate replication roles | Gateway, store and directory as their own processes with one simulation server | The G1 session through the roles: added latency measured; the store and the simulation server each restart without a client disconnect |
| G3. Static mesh | Several simulation servers with fixed areas; a standby server; group transfer with deadline and abort; requests across authorities | Bots cross a Bistro area border with no lost or duplicated state; a shot across the border registers once; a door and a pickup across the border work; the new authority is killed during a transfer and the entity stays with the old one |
| G4. Scale | Server workers and the shard dispatcher; Registered I/O on Windows; more than 64 characters per physics world; the overload steps; 400 co-located bots | Server tick cost, system-call CPU and egress at 400 clients on each host, against the estimate table; each overload step triggers and recovers |
| G5. Persistence | The entity graph, the persistence writer and transactions | A Bistro world restarts every server and resumes every entity's state; a server killed during a trade neither duplicates nor loses the item |
| G6. Dynamic mesh | Areas split and merged by load | Bots crowd one area; the mesh splits it with no lost state, within the tick budget, and the split is measured against the overload steps |

G1 alone is a complete FPS session. Each later phase keeps G1's client and
game code unchanged, which the [G1 decisions](#decisions-g1-must-get-right)
make possible.

## Performance targets

These are targets for the evidence of each phase, not results.

| Target | Value | Phase |
|---|---|---|
| Server tick for a 20-player session (simulation, replication and network) | at most 4 ms of one core at 60 Hz, so one host runs several sessions | G1 |
| 20-player session bandwidth | at most 128 kbps down per client at 30 Hz snapshots; at most 256 kbps at 60 Hz | G1 |
| Transport memory per game connection | at most 128 KiB | G1 |
| Latency added by the separate replication roles | measured; target set in G2 | G2 |
| 400 co-located clients | at most 512 kbps down per client, with the tier rates that G1's measured update sizes allow; snapshot encode within the server tick on a Ryzen 5 2600 and on an M1 Pro | G4 |

Estimates for planning, which G1 and G4 must measure. They assume 51 B of
packet overhead over IPv4 and 1,200 B datagrams. A moving character costs
about 14 to 24 B per update: the index gap, the baseline tick, masks, a
57-bit position (17 bits on each ground axis, 23 bits of height), aim, an
animation state and sometimes a velocity. Halo: Reach measured about
1 kbit/s per character per client in combat. The original estimates
assumed 10 B. The estimates count characters only; events, weapons,
projectiles and spawns add to them.

| Case | Updates per second per client | Down per client at 16 B | Down per client at 24 B | Server egress at 24 B |
|---|---|---|---|---|
| 20 players, 30 Hz | 19 × 30 = 570 | about 85 kbps | about 122 kbps | about 2.4 Mbps |
| 20 players, 60 Hz | 19 × 60 = 1,140 | about 170 kbps | about 243 kbps | about 4.9 Mbps |
| 400 in one place: 32 at 30 Hz, 96 at 10 Hz, 272 at 4 Hz | 3,008 | about 410 kbps | about 614 kbps | about 246 Mbps |
| Input upstream: 60 packets/s, about 40 B of commands each | — | about 44 kbps up | — | — |

At 24 B per update, the 400-player tiers exceed the 512 kbps target, and
the slow tiers must drop to fit it.

Memory estimates for 400 co-located clients:

| Item | Size |
|---|---|
| Transport, game profile | 400 × 96 KiB ≈ 38 MiB |
| Baselines: 1,000 entities in interest at 64 B | ≈ 26 MB |
| Records of states in flight: 3,008 updates/s for 0.25 s at 64 B | ≈ 19 MB |
| Hitbox history: 400 characters, 1 s at 30 Hz, 640 B per pose | ≈ 7.7 MB |

## Verification

- Each layer gets simulator tests that name their failure, as the
  transport has (ADR-105):
  - lost and reordered snapshots, and a late acknowledgment after `LOST`;
  - an update decoded while newer unacknowledged packets are in flight,
    against the baseline it names;
  - a delayed despawn that arrives after a newer spawn of the same index;
  - an index reused before every client acknowledged its despawn;
  - an event that arrives before its entity's spawn, and an event about a
    despawned entity whose index was reused;
  - a transfer with in-flight writes and input, a transfer whose new
    authority does not confirm, and a group that transfers with its
    attachments;
  - a refused write with an old epoch, and a server that returns after its
    lease expired;
  - a request applied before a transfer and not applied again after it,
    and a retry after a timeout that does not apply twice;
  - a level entity destroyed while a client had its cell unloaded;
  - a fast client clock, a tap between two input samples, and a shot with
    an out-of-bound view time.
- Every phase ends with a check over real sockets with bots on Bistro.
  The bot swarm runs many clients without a renderer in one process. G4
  runs it on a second machine, so the bots do not share the server's
  cores, and within the memory budget of a 16 GB Mac.
- Restart tests kill one process of each role during a session and check
  that no client disconnects where the role table says none should.
- Soak tests run a session for hours and record queue depths, entity
  counts and memory, because CIG's queues and entity counts grew as shards
  aged.
- Tools that G1 adds: per-client bandwidth by component and entity, a
  replication overlay in the editor (authority, rate tier, last update,
  correction), and the server tick broken down by layer.
- Performance claims use matched Release builds with graphics validation
  off, on Windows and macOS servers.

## Open decisions

Each decision has a recommendation. The owner can change it before the
phase that depends on it.

| Decision | Recommendation | Tradeoff |
|---|---|---|
| Replication layer | Stateful, as in Star Citizen, with a gateway that holds connections | A stateless gateway that forwards packets to simulation servers is simpler, but a server crash loses state and per-client encoding repeats on every server. |
| Authority unit | Per authority group (a root entity and its attachments), defaulting to its container's server | Authority per area only is simpler, but vehicles and players crossing borders need per-group transfer anyway. Authority per component (SpatialOS) is finer but makes every entity a distributed object. |
| Baselines | One acknowledged state per client and entity | Tribes-style re-flagging keeps only change bits per client and entity and shares all encoding, but a lost packet can leave one entity's properties from different ticks. Compare both in G1 if baseline memory is the limit. |
| When to test several authorities | G1 runs its session a second time with two in-process authorities split by area | It adds transfer and requests to G1, but finds code that assumes one server two phases earlier. Leaving it to G3 repeats CIG's retrofit. |
| Tick rate of the persistent world | 60 Hz, as the FPS session | 30 Hz halves simulation cost for 400 players, but needs the fixed step to become a session setting, and clients must predict at the same rate. Decide in G4 from the measured tick cost. |
| Rewind cap | 200 ms default, set per session | A larger cap favors high-ping shooters and lets players exploit fake latency. |
| Overload step after the entry cap | Decide in G4: shard-wide time dilation with a floor and a factor in every snapshot, or none | Dilation keeps everyone in the fight but slows every player of the shard for one crowd. Without it, an overloaded area runs behind the clock and its players see lag. Dilation per area contradicts the single shard clock. |
| Persistence store | Decide in G5; prefer an append log keyed by `VkrEntityRef` over an external database | An external graph database is ready-made but adds a server dependency; CIG's third-party database caused its 3.18 stalls. |
| Server platforms | Windows and macOS (2026-10-09); Linux later | Linux has `sendmmsg`, `recvmmsg` and cheaper hosting; it needs a server build. |

## Alternatives considered

- **Gateways and cell servers that forward packets.** The earlier plan in
  the network protocol proposal. Rejected as the base: no state survives a
  cell server, and every cell server encodes for every client it serves.
- **Zones with handoff screens.** Classic MMO sharding: simple, but players
  see the seams and cannot interact across a zone border.
- **Hosted spatial simulation platforms.** SpatialOS gave per-component
  authority and interest queries as a hosted service. Its games depended on
  one host and its pricing
  ([Worlds Adrift shutdown](https://www.gamedeveloper.com/business/bossa-shutting-down-i-worlds-adrift-i-says-game-is-no-longer-commercially-viable-)).
- **Single-process systems with time dilation.** EVE Online runs one
  solar system in one process and slows it under load. It has no borders
  inside a system, and large fights need a stronger node booked in
  advance. Time dilation is kept here as the last overload step.
- **Client-authoritative movement.** Cheap for the server, but open to
  speed and position cheats. CIG retrofitted server authority in 3.17.2,
  and New World's exploits came from a server that waited on client input
  ([PC Gamer](https://www.pcgamer.com/new-world-is-not-client-authoritative-amazon-says/)).
- **Push-model change tracking.** Iris marks changes through setters.
  Components here are written through raw pointers, so a setter would miss
  writes; the compare-after-tick pass catches every write.
- **Lockstep or peer-to-peer.** Non-goals: they need cross-platform
  determinism or trust between players.

## Risks

- **Scope.** Star Citizen has worked on meshing for more than five years,
  and SpatialOS games failed on the platform's cost. G1 must be useful on
  its own; later phases start only when a game needs them.
- **Prerequisites.** G0 changes physics, gameplay state and streaming. The
  entity behavior proposal's rollback state is broader than G0's
  one-character replay; prediction of anything else waits for it.
- **The extra hop.** The separate replication roles add latency. The
  one-process configuration keeps competitive FPS sessions free of it.
- **Interaction across authorities.** Shots, contacts and transactions
  across a border are the hardest part of a mesh, and CIG still works on
  them. G3 must prove them before G4 scales.
- **Crowds.** A crowd in one place stays on one authority. G4 proves 400
  players on one server and the overload steps; a dynamic mesh does not
  remove this limit.
- **Replication store capacity.** One store for a crowded area is a
  bottleneck until G2 makes it restartable and G4 measures it.
- **Bandwidth per character.** The 400-player target depends on update
  sizes near 16 B. G1 measures the real size per component.
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
- [ADR-073](../adr/073-native-gameplay-foundation.md): fixed ticks,
  ordered input and characters.
- [ADR-076](../adr/076-project-object-model.md): type descriptors and
  entity references.
- [ADR-079](../adr/079-c-script-modules.md): scripts and hot reload.
- [ADR-086](../adr/086-world-partition.md): streaming sources and cells.
- [ADR-115](../adr/115-rooms-and-portals.md): rooms and portals for
  visibility.
- [Entity behavior proposal](entity-behavior-system.md): the multiplayer
  boundaries.
