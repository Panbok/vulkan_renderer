---
status: proposed
updated: 2026-10-09
authority: proposal
---
# Network protocol

One binary protocol over UDP for every networked feature of the engine and
the editor:

1. An asset depot: a versioned asset server on a remote machine, with push,
   pull and on-demand download.
2. Game sessions: lobbies of 10 to 20 players first, later persistent worlds
   with 10,000 or more players and 200 to 400 in one place.
3. Editor collaboration: several users in one editor session whose edits,
   drags and clicks reach every participant.
4. Streaming: frames rendered on another machine and streamed back, and
   assets streamed into GPU memory as the scene needs them.
5. Agent federation: agents on several machines that work on one level
   through their own editors, with work divided between them.

This document specifies the transport, the data format, the session layer
and one service per use case, then the code owners, phases, open decisions
and the evidence each phase needs. Names of types and functions are
provisional until the phase that adds them.

## Current baseline

- **No network transport exists.** The only sockets are local AF_UNIX
  stream sockets
  ([vkr_local_socket.h](../../lib/src/platform/vkr_local_socket.h)) for
  the agent channel, `vkr_mcp` and the `vkr_bakery serve` daemon, which
  speak newline-delimited JSON
  ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)).
- **Content identity.** SHA-256 runs on the ARMv8 and x86 SHA instructions
  ([vkr_hash.h](../../lib/src/core/vkr_hash.h)). `.vkpak` chunks are named
  by the SHA-256 of their decoded bytes and are optionally one zstd frame
  ([vkr_vfs.h](../../lib/src/filesystem/vkr_vfs.h),
  [ADR-077](../adr/077-asset-build-system.md)). VFS mounts are fixed at
  startup.
- **Typed values.** Type descriptors
  ([vkr_type_desc.h](../../runtime/src/core/vkr_type_desc.h),
  [ADR-076](../adr/076-project-object-model.md)) describe every component:
  at most 64 properties and 1 KiB per value, with kinds, ranges and
  validation. Entities have document-stable UUIDs
  ([vkr_entity_ref.h](../../lib/src/core/vkr_entity_ref.h)).
- **Edits.** Editor edits are `VkrSampleEditBatchItem` lists of at most
  2,048 items, applied by the runtime as one journal group with authors,
  claims and a change feed
  ([vkr_scene_edit.h](../../runtime/src/renderer/systems/vkr_scene_edit.h),
  [vkr_sample_runtime.h](../../runtime/src/vkr_sample_runtime.h)). Journal
  sequences are process-wide, and requests refer to local `VkrEntityId`
  values.
- **Simulation.** The scene ticks at a fixed 60 Hz. The FPS module orders
  input commands by tick and sequence, but the commands have no wire
  encoding ([ADR-073](../adr/073-native-gameplay-foundation.md)). The
  [entity behavior proposal](entity-behavior-system.md#preserving-a-multiplayer-path)
  asks for network identities that map to local handles, and for no
  speculative replication descriptors on every field.
- **Streaming inputs.** World partition supplies streaming sources and
  128 m cells ([ADR-086](../adr/086-world-partition.md)). Resource
  publication is an ordered queue with confirm-before-use
  ([ADR-082](../adr/082-renderer-owned-render-thread.md)). Offscreen
  targets share frame submission
  ([ADR-014](../adr/014-offscreen-present-target.md)). No video encoder or
  decoder exists.
- **Platforms.** macOS with Metal 4 and Windows with Vulkan 1.4
  ([ADR-083](../adr/083-supported-hardware-matrix.md)). The repository
  has no Linux build. Every supported CPU has AES and SHA instructions.
- **Libraries.** Vendored code has no cryptography beyond SHA-256. zstd is
  available through the copy that libktx carries.

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

## Design principles

1. **One transport, many services.** A connection between two processes
   multiplexes every service they share. The services share one congestion
   controller and one handshake.
2. **Binary only.** No JSON, XML, YAML or other text format crosses the
   wire. Error and close reasons are numeric codes. See
   [Data format](#data-format).
3. **Sans-I/O core.** The transport is a state machine that takes
   datagrams and a time, and returns datagrams and a next timeout. Socket
   drivers and threads are separate. Tests drive the core through a
   simulated link with a fixed seed and a virtual clock.
4. **No allocation per packet.** Packet buffers, sent-packet records and
   message queues come from bounded pools. Each connection has fixed caps.
5. **Adaptation through one estimate.** The congestion controller exports
   a bandwidth and RTT estimate. The video encoder, the snapshot budget and
   the depot request window read that estimate.
6. **Proven primitives.** The handshake follows the Noise protocol
   framework, and the primitives come from one vetted library. The project
   writes no cryptographic primitive.

## Layers

The layers from the services down to the socket:

```text
+---------------------------------------------------------------+
| services: depot | stream | world | collab | view | bench       |
+---------------------------------------------------------------+
| session: service open/accept, schema hashes, channel table    |
+---------------------------------------------------------------+
| transport core (sans-I/O): handshake, AEAD, packet numbers,   |
| acks, loss recovery, channels, fragmentation, flow control,   |
| scheduler, congestion control, pacing, PMTU discovery         |
+---------------------------------------------------------------+
| driver: net thread(s), timers, wake-up, rings to app threads  |
+---------------------------------------------------------------+
| vkr_udp_socket: nonblocking UDP, dual-stack, batched I/O      |
+---------------------------------------------------------------+
```

## Transport

### Packet header

A datagram holds one packet. Data packets use a short header:

```text
 0       1               5           5+n               end-16  end
+-------+---------------+-----------+------------------+--------+
| flags | DCID (32 bit) | PN (n B)  | encrypted frames | tag    |
+-------+---------------+-----------+------------------+--------+
```

| Field | Size | Meaning |
|---|---|---|
| `flags` | 1 B | Bit 7: long header (0 here). Bit 6: fixed 1, which separates the protocol from STUN on a shared port. Bit 2: key phase (reserved). Bits 1–0: PN length minus one. Other bits are zero. |
| DCID | 4 B | Destination connection ID chosen by the receiver. A server puts its worker shard index in the top bits. |
| PN | 1–4 B | Low bits of a 64-bit packet number per direction. The sender picks the length that covers twice the distance to the oldest unacknowledged packet. |
| tag | 16 B | AEAD tag. |

The header is the AEAD associated data. The nonce is a per-direction IV
XOR the full 64-bit packet number, so a nonce never repeats under one key.
Packet numbers and connection IDs stay visible to observers, and the
packet count is the only information this exposes. The typical overhead is
23 B per packet (1 + 4 + 2 + 16), plus 28 B of IPv4/UDP or 48 B of
IPv6/UDP headers.

Handshake packets use a long header: flags, a 32-bit protocol version, a
type byte, the DCID and the source connection ID. A client's first packet
is padded to 1,200 B.

### Integers and frames

Frame-level integers use the QUIC variable-length encoding. The two high
bits of the first byte give the length (1, 2, 4 or 8 bytes) for values up
to 2^62. Byte-aligned fixed-width fields are little-endian and use the
helpers in [vkr_byte_io.h](../../lib/src/core/vkr_byte_io.h).

The decrypted payload is a sequence of frames:

| Type | Frame | Fields | Retransmitted |
|---|---|---|---|
| `0x00` | PADDING | — | No |
| `0x01` | PING | — | Elicits an ACK |
| `0x02` | ACK | largest PN, ack delay (8 µs units), range count, first range, then gap/range pairs | No (sent fresh) |
| `0x04` | WINDOW | channel, new receive limit in bytes | Yes |
| `0x05` | BLOCKED | channel, limit that blocked the sender | Yes |
| `0x06` | CANCEL | channel, message sequence or tag | Yes |
| `0x08` | PATH_CHALLENGE / `0x09` PATH_RESPONSE | 8 random bytes | Yes |
| `0x0A` | CLOSE | numeric code (u16), frame type that caused it | No |
| `0x0B` | FEC_REPAIR | channel, group, index, count, repair bytes | No |
| `0x40`–`0x7F` | MESSAGE | see below | By channel class |
| `≥ 0x4000` | Extension | length, then bytes | By extension |

The low six bits of a MESSAGE type are flags:

| Bit | Name | Meaning when set |
|---|---|---|
| 0 | `LEN` | A length follows; otherwise the message runs to the end of the packet |
| 1 | `SEQ` | A message sequence follows (ordered and sequenced channels) |
| 2 | `FRAG` | Fragment offset and total size follow |
| 3 | `TAG` | A sender-chosen tag follows (raw channels) |
| 4 | `PRIO` | A 1-byte priority within the channel follows |
| 5 | reserved | Zero |

The frame continues with the channel ID (1 B), the fields its flags
select, and the payload. A small unreliable message costs 3 B of framing,
and a small reliable ordered message costs 4 to 5 B.

A receiver skips an unknown frame in the extension range, which carries its
own length. An unknown frame type below that range closes the connection
with `PROTOCOL_VIOLATION`. New behavior therefore arrives as extension
frames that old peers ignore, or as a new protocol version.

### Acknowledgment and loss recovery

Loss recovery follows RFC 9002, with the frame-level retransmission model
of QUIC:

- The receiver acknowledges every second ack-eliciting packet at once.
  Otherwise it waits up to `max_ack_delay`: 5 ms for interactive
  connections and 25 ms for bulk ones. ACKs ride on outgoing data when
  data exists.
- A packet is lost when three later packets are acknowledged, or when
  9/8 × max(SRTT, latest RTT) passes after a later packet is acknowledged.
  A probe timeout of SRTT + 4 × RTTVAR + `max_ack_delay` covers tail loss.
- The transport retransmits frames, not packets. A lost packet's reliable
  frames join new packets with new packet numbers. Its unreliable frames
  are dropped. Its deadline frames are resent only while
  `now + SRTT / 2 < deadline`.
- The receiver keeps a 2,048-packet replay window and drops duplicates
  before it decrypts them twice.
- Services can ask to hear when a packet that carried their message is
  acknowledged or declared lost. Game snapshots use this hook instead of
  sending their own acknowledgments.

### Channels and delivery classes

A service declares its channels when it opens. A connection has at most
64 channels, and channel 0 is the reliable ordered session channel.

| Class | Behavior | Typical use |
|---|---|---|
| `UNRELIABLE` | At most once, any order | Snapshots, input redundancy |
| `SEQUENCED` | At most once; the receiver drops a message older than the newest delivered one | Presence, gesture previews, latest value |
| `RELIABLE_ORDERED` | Exactly once, in send order | Edits, events, RPCs, session control |
| `RELIABLE_UNORDERED` | Exactly once, in arrival order | Depot chunks, streamed assets |
| `DEADLINE` | As `RELIABLE_UNORDERED` until the message's deadline, then dropped and cancelled | Video frames |

Each channel also declares:

- **Payload kind:** `RAW` or `SCHEMA` (see [Data format](#data-format)).
- **Priority:** 0 (control) to 7 (background), and a weight inside its
  level.
- **Maximum message size:** the default is 1 MiB and the hard cap is
  64 MiB. Larger objects are split by the service.
- **Receive window:** bytes the receiver accepts in flight. The receiver
  raises the window with WINDOW frames as the service consumes data.

Ordering applies inside one channel only. A lost packet on one channel
never blocks delivery on another channel.

### Fragmentation and placement

A message larger than one packet's payload travels as fragments with an
offset and a total size. For a `RELIABLE_UNORDERED` or `DEADLINE` channel,
the receiving service can supply the destination buffer when the first
fragment arrives. It resolves the message's tag to a staging region, and
the transport decrypts each fragment directly into that region at its
offset. Without a destination, fragments reassemble in the channel's
receive window memory. The transport counts the fragments and delivers
the message once every byte is present.

Placement removes the copy between transport and consumer for bulk data.
The remaining passes are the kernel's copy into the receive buffer, the
decryption into the destination, and the hash check by the service.

### Scheduling, pacing and coalescing

A packet is filled in this order:

1. ACK, PATH and CLOSE frames.
2. Messages by channel priority, strictly from level 0 to level 7.
3. Inside one level, deficit round-robin by channel weight, then by
   message priority.

Messages that a thread queues during one tick wait until `vkr_net_flush`,
so one packet carries all of them. A message sent with the `IMMEDIATE`
flag starts a flush at once; input commands use it.

Packets leave at the pacing rate (1.25 × the bandwidth estimate) in bursts
of at most 10 packets or one segmentation-offload batch. Pacing keeps a
large transfer from overflowing shallow router buffers, which would raise
loss and RTT for every channel.

### Congestion control

Each connection has one controller. The recommended controller is
model-based in the manner of BBR: it estimates the bottleneck bandwidth
and the minimum RTT, and it paces to the estimate.

- Services need an explicit bandwidth number for the encoder bitrate, the
  snapshot budget and the depot request window. A model-based controller
  provides one.
- Loss-based controllers such as CUBIC fill router queues until a loss
  occurs. A full queue adds its delay to every interactive channel on the
  same connection.
- A connection opened in `BACKGROUND` mode keeps queueing delay below a
  25 ms target, as LEDBAT does, and yields to other traffic on the path.
  The depot uses this mode by default when a game or editor session
  shares the machine.
- Game snapshots and inputs are application-limited. The controller only
  lowers their budget when the path cannot carry it.

### Path MTU

The first packets use a 1,200 B UDP payload, which fits IPv6's 1,280 B
minimum and common VPN tunnels. Packets set the don't-fragment bit.
Discovery follows DPLPMTUD (RFC 8899): PADDING probes test 1,452 B for
Ethernet and, on a local network, 8,952 B for jumbo frames. A lost probe
does not count as congestion. A bulk transfer on a local network with
jumbo frames sends one eighth of the packets for the same bytes.

### Handshake

The handshake uses the Noise `IK` pattern with X25519, ChaCha20-Poly1305
and BLAKE2b. The client knows the server's static public key in advance:
from a connect token, an invite or a configured depot remote. The client
sends its own static key encrypted in the first message.

The message flow for one connection:

```text
client                                        server
  | Initial: e, es, s, ss, credential  ------> |
  |                 (padded to 1,200 B)        |
  | <------ Retry: cookie (only under load)    |
  | Initial + cookie  ----------------------> |
  | <------ Handshake: e, ee, se, params       |
  | 1-RTT data  <---------------------------> |
```

- The handshake takes one round trip, or two under load.
- The server sends at most three times the bytes it has received from an
  address until the address is validated.
- Under load, the server answers with a stateless Retry. The cookie is a
  MAC over the client address and a time, under a server secret. The
  server allocates connection state only for a valid cookie.
- The server limits handshakes per source address.
- The protocol has no 0-RTT data. A replayed Initial cannot carry an
  action.
- After the handshake, Noise's `Split()` gives one key per direction. The
  data phase uses AES-256-GCM with a 16 B tag, on AES-NI on x86-64 and on
  the ARMv8 crypto extensions on Apple silicon. Phase 0 checks this
  hardware path in the pinned library version.

The credential in the first message depends on the deployment:

| Deployment | Credential | Server check |
|---|---|---|
| Game session | Connect token from a matchmaking backend: client ID, expiry, server addresses and user data, sealed with a key shared by backend and servers (the netcode.io design) | Decrypts the token, checks expiry and address, refuses a token reused within its lifetime |
| Workspace (depot, editor session, agents) | The machine's static key, bound to an Ed25519 user identity | Looks up the key in the depot's or session's access list with a role: read, write or admin |

An invite binds a workspace peer that has not joined before. The invite
holds the server address, the server's public key and a one-time secret.
The joining machine proves the secret in its first message, and the
server adds its key to the access list.

### Connection lifetime

- A connection closes after an idle timeout: 15 s for game and editor
  sessions, 60 s for depot connections. A peer sends PING after one third
  of the timeout without traffic.
- When packets for a known DCID arrive from a new address (a NAT
  rebinding), the receiver sends PATH_CHALLENGE. It sends at most three
  times the received bytes to the new address before the response.
- CLOSE carries a numeric code, such as `PROTOCOL_VIOLATION`,
  `FLOW_CONTROL`, `SCHEMA_MISMATCH`, `ACCESS_DENIED` or
  `SERVICE_REFUSED`. CLOSE carries no reason text.

## Data format

### Rules

1. No text format crosses the wire.
2. Text that is data, such as an entity name, a commit message or an asset
   path, is a bounded UTF-8 field in a binary message. The decoder checks
   its length and its UTF-8 validity.
3. Each channel carries one payload kind, `RAW` or `SCHEMA`.
4. A file transferred as an asset is opaque bytes, whatever its content.
   A scene document in a depot is a blob. Live session state and every
   request about that state are schema messages.
5. Agents keep JSON on the local socket to their own editor, because the
   MCP adapter and the agents speak JSON. The editor converts each request
   to a binary message before the request leaves the machine.

### Raw channels

A raw message is opaque bytes plus an optional sender-chosen tag (a
varint in the MESSAGE frame). The service uses the tag to map bytes to a
request, for example a depot chunk request, without parsing the bytes.

- The transport AEAD provides integrity on the path.
- The service checks content identity, such as the SHA-256 that names a
  depot chunk.
- Compression is decided per message by the service's request and
  response messages. Raw bytes contain no compression header.

### Schema messages

A schema message is a message ID (a bit-varint) followed by the message's
fields in declaration order. Fields are bit-packed, without tags, names
or padding. An optional field costs one presence bit. The transport
frame supplies the message length.

Field kinds and their encodings:

| Kind | Wire form |
|---|---|
| `BOOL` | 1 bit |
| `UINT(n)`, `INT(n)` | n bits; signed values use zigzag |
| `VARUINT` | 2-bit class selecting 8, 16, 32 or 64 bits, then the value |
| `F32` | 32 bits; NaN and infinity are refused unless the field allows them |
| `QFLOAT(min, max, bits)` | Fixed point over the range, at most 32 bits |
| `VEC3Q(min, max, bits)` | Three `QFLOAT` values |
| `QUAT(bits)` | Smallest three: 2-bit index and three components of `bits` each |
| `ENUM(count)` | ceil(log2(count)) bits |
| `BYTES(cap)`, `UTF8(cap)` | `VARUINT` length, then bytes from the next byte boundary |
| `ARRAY(element, cap)` | `VARUINT` count, then the elements |
| `MESSAGE(desc)` | The nested message's fields |
| `NETID` | `VARUINT` session entity ID |
| `ENTITY_REF` | 128 bits |
| `HASH64`, `HASH256` | 64 or 256 bits |

The decoder enforces every bound: ranges, enum counts, array and string
capacities, UTF-8 validity and the end of the frame. A violation closes
the channel with `SCHEMA_VIOLATION`. A decoder never skips an unknown
field. The schema is exact.

Schemas are C descriptor tables, the same pattern as `VkrTypeDesc`. A
`VkrWireMessageDesc` holds the name, the message ID and the field table.
A `VkrWireFieldDesc` holds the kind, the struct offset, the bit width or
capacity and the range. One table-driven codec encodes and decodes them
over a 64-bit bit-stream accumulator.

**Schema identity.** A canonical binary form of a service's descriptors
holds IDs, kinds, widths, ranges and field order. Its SHA-256, truncated
to 64 bits, is the schema hash. Peers exchange the hash when they open
the service. A mismatch selects another version that both peers list, or
refuses the service with `SCHEMA_MISMATCH`. A change in a message is a new
message ID or a new service version, never a silent change in field
meaning.

Per-field tags in the protobuf style cost one or two bytes per field and
need skip logic, and the exact schema needs neither. Game and editor peers
run the same build. The depot server supports a list of versions for older
clients.

### Components on the wire

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

## Session and services

Channel 0 carries session messages, which are schema messages:

| Message | Fields |
|---|---|
| `SERVICE_OPEN` | service ID, versions with schema hashes, requested channels with class, kind, priority and windows |
| `SERVICE_ACCEPT` | chosen version, assigned channel IDs |
| `SERVICE_REFUSE` | numeric code |
| `SERVICE_CLOSE` | service ID, numeric code |

| ID | Service | Use case | Owner |
|---|---|---|---|
| 1 | `depot` | Asset depot | `tools/depot`, client in `vkr_net` |
| 2 | `stream` | Asset streaming | `runtime/src/net` |
| 3 | `world` | Game sessions | `runtime/src/net` |
| 4 | `collab` | Editor collaboration and agent federation | `editor/src` |
| 5 | `view` | View streaming | `runtime/src/net`, encoder hooks in the renderer |
| 6 | `bench` | Throughput and latency tests | `tools/net` |

Two machines in an editor session that also sync assets open `collab`
and `depot` on one connection. The depot's channels run at priority 7, so
edits keep their latency while assets transfer.

### Depot (use case 1)

The depot is a versioned, content-addressed asset store with commit,
branch, push, pull, lock and on-demand fetch.

The object model, from a branch to its bytes:

```text
ref (branch) -> commit -> tree -> blob -> chunks
                  |                 |
              parents         chunk list (SHA-256)
```

- **Chunks** are named by the SHA-256 of their decoded bytes, the same
  identity as `.vkpak` chunks and Bakery outputs. Blobs split by FastCDC
  content-defined chunking (64 KiB minimum, 256 KiB average, 1 MiB
  maximum), so an edit inside a large scene or mesh changes few chunks. A
  file below 64 KiB is one chunk.
- **Trees** are sorted binary entries: a path component, a mode and a
  hash.
- **Commits** are binary records: parents, tree, author key, time and a
  UTF-8 message.
- **Refs** live on the server. A push updates a ref with compare-and-swap
  from the expected old commit, so two concurrent pushes cannot both win.
- **Locks** are exclusive locks on paths. Binary assets cannot merge, so
  a writer locks them before editing.

Operations are schema messages on one reliable ordered channel. Bytes
travel on a raw `RELIABLE_UNORDERED` channel at priority 7.

| Operation | Purpose |
|---|---|
| `REFS` | List branches and their commits |
| `TREE` | Tree entries of a commit below a path prefix |
| `HAVE` | A list of chunk hashes; the reply is a bitmap with one bit per hash that the receiver lacks |
| `GET` | Chunk hashes with tags; the chunks arrive as raw messages with those tags |
| `PUT` | Announce chunks; the bytes follow as raw messages |
| `UPDATE_REF` | Compare-and-swap a branch |
| `LOCK`, `UNLOCK`, `LOCKS` | Path locks |

The transfer path for a pull:

1. The client reads the target tree and lists the chunk hashes it lacks
   in its local store.
2. The client keeps `GET` requests in flight up to a window of bytes. The
   window is the bandwidth estimate × RTT × 2, at least 8 MiB.
3. The server sends each chunk as one zstd frame when compression saves at
   least a tenth, which is the `.vkpak` rule, and raw bytes otherwise.
4. The client decodes the chunk, checks its SHA-256 and writes it to the
   local store by atomic rename. A chunk that fails the check is requested
   again from the start, and three failures stop the pull.
5. An interrupted pull resumes at the chunks that are still missing,
   because chunks are content-addressed.

A push uses `HAVE` and `PUT` in the other direction, then `UPDATE_REF`.

The local store is a per-user directory beside the Bakery cache. A working
copy materializes files from the store with hard links or clones where the
file system allows them, as Bakery already does. A sparse working copy
fetches a file when something opens it. This shares the remote content
mount of [asset streaming](#asset-streaming-use-case-4b).

Bakery's action cache can publish its outputs to a depot. The keys are the
action keys and the values are output hashes. A machine with the same
inputs then downloads ASTC or BC outputs instead of cooking them. ADR-077
listed a shared network cache as a non-goal, so this part needs its own
decision.

Each received byte costs one AES-GCM decryption, one zstd decode for
compressible chunks, one SHA-256 and one disk write. The SHA instructions
and AES hardware put the decryption and the hash above 1 GB/s per core.
One core per side therefore fills 1 GbE. On 10 GbE, the chunk checks run
on the job system's workers.

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

### Code owners

| Owner | Path | Contents |
|---|---|---|
| `vkr_foundation` | `lib/src/platform/vkr_udp_socket.h`, `_posix.c`, `_windows.c` | Nonblocking UDP, dual-stack IPv6, don't-fragment, batched send and receive where the OS has them |
| `vkr_net` (new library) | `net/src/` | Transport core, crypto wrapper, channels, scheduler, congestion control, bit stream, wire codec, session layer, drivers, depot client |
| Vendor | `vendor/libsodium` | Pinned release, built by a project CMake target |
| Tools | `tools/net/vkr_net_bench.c` | Throughput, latency and loss tests over real or simulated links |
| Tools | `tools/depot/` | `vkr_depot` server and CLI: `init`, `serve`, `push`, `pull`, `log`, `lock` |
| `vkr_runtime` | `runtime/src/net/` | `world`, `stream` and `view` services, remote content mount |
| Editor | `editor/src/editor_session.c` | `collab` service, presence, task board, Session window |
| Renderer | Encoder and decoder hooks | Separate design and ADR in phase 7 |
| Tests | `tests/src/net_*_test.c` | Simulated-link tests of the core and the codec |

`vkr_net` depends only on `vkr_foundation` and libsodium, so `vkr_depot`
and a dedicated server can link it without the renderer. It is a new
library because it has an independent responsibility and build boundary.

### Platform I/O

| Host | Receive and send | Batching |
|---|---|---|
| Windows (client and server) | Winsock2 nonblocking UDP, `WSARecvMsg`/`WSASendMsg` | Registered I/O for servers in phase 8; UDP segmentation offload only where Windows provides it |
| macOS (client and server) | BSD sockets with kqueue | None in the public API; one system call per datagram |
| Linux (later) | `recvmmsg`/`sendmmsg` | UDP GSO and GRO, `SO_REUSEPORT` per worker |

Servers run on Windows and macOS (owner decision of 2026-10-09). The
400-client case needs about 11,000 datagrams per second out and 24,000 in
(see [Interest and bandwidth budget](#interest-and-bandwidth-budget)). At
one system call per datagram, that is a fraction of one core on either
host; phase 8 measures it. The `vkr_udp_socket` interface takes arrays of
datagrams, so a Linux backend with batched calls fits later without
changes to the transport. Windows and macOS headers define `near`, `far`,
`small` and similar macros, so the new code avoids those names.

### Threads

- **Clients and editors.** One net thread owns the sockets and the
  transport core. Application threads exchange messages with it through
  bounded single-producer, single-consumer rings. A datagram to the
  thread's own loopback port wakes it.
- **Servers.** N net workers each own the connections whose DCID shard
  bits select that worker. One receiving thread per socket dispatches
  datagrams to the workers by DCID. On Windows, phase 8 gives each worker
  its own Registered I/O queue when one receiving thread limits the
  server.
- **Snapshot encoding** runs per client on the job system, and the
  encoded payloads go to the net workers.
- The net thread never calls into a service. Services poll for received
  messages and events on their own thread.

### Memory

| Data | Storage | Owner and release |
|---|---|---|
| Datagram buffers | Per-thread slabs of 2 KiB and 9 KiB buffers | Net thread; a sent buffer returns after its packet is acknowledged or lost, a received one after dispatch |
| Sent-packet records | Ring per connection, 4,096 by default | Connection; released on acknowledgment or loss |
| Connection control block | Fixed pool, target 4 KiB each | Host; released at close |
| Channel state | 64 slots per connection | Connection |
| Send and receive queues | Shared pools under per-channel and per-connection caps | Channel; WINDOW frames limit the peer |
| Placement buffers | Service staging memory | Service; GPU staging returns after GPU completion |

No path allocates per packet. The [vkr-memory](../../.codex/skills/vkr-memory/SKILL.md)
rules apply to each pool.

### API sketch

The application-facing API (provisional names):

```c
typedef struct VkrNetHost VkrNetHost;
typedef uint32_t VkrNetConnection;

VkrNetHost *vkr_net_host_create(VkrAllocator *allocator,
                                const VkrNetHostConfig *config);
void vkr_net_host_destroy(VkrNetHost *host);

bool8_t vkr_net_connect(VkrNetHost *host, const VkrNetAddress *address,
                        const VkrNetCredential *credential,
                        VkrNetConnection *out_connection);

/* Copies the message into the channel's send queue; false when the
   channel's send cap is full. */
bool8_t vkr_net_send(VkrNetHost *host, VkrNetConnection connection,
                     uint8_t channel, const void *data, uint32_t size,
                     uint32_t flags);

/* Packs every queued message into packets now. */
void vkr_net_flush(VkrNetHost *host);

/* Messages stay valid until the next receive call on this thread. */
uint32_t vkr_net_receive(VkrNetHost *host, VkrNetMessage *out_messages,
                         uint32_t capacity);

VkrNetPathStats vkr_net_path(const VkrNetHost *host,
                             VkrNetConnection connection);
```

The sans-I/O core under it:

```c
void vkr_net_core_on_datagram(VkrNetCore *core,
                              const VkrNetAddress *from, uint8_t *bytes,
                              uint32_t size, uint64_t now_us);
uint32_t vkr_net_core_poll_transmit(VkrNetCore *core,
                                    VkrNetDatagram *out_datagrams,
                                    uint32_t capacity, uint64_t now_us);
uint64_t vkr_net_core_next_timeout_us(const VkrNetCore *core);
```

### Observability

The transport publishes per-connection metrics through the metrics module
([ADR-015](../adr/015-metrics-module.md)): SRTT, RTT variance, bandwidth
estimate, congestion window, bytes and packets per direction, loss, and
queue depth per channel. `vkr_net_bench` prints the same values. The
editor shows them in its Session window.

## Phases

| Phase | Scope | Exit evidence |
|---|---|---|
| 0. Groundwork | Vendor libsodium with its CMake target; `vkr_udp_socket`; raw UDP baseline in `vkr_net_bench` between a Mac and a Windows machine | `crypto_aead_aes256gcm_is_available()` is true on both hosts; AES-256-GCM and SHA-256 bytes per second per core; raw UDP RTT, packets per second and throughput per host, with commands and configuration |
| 1. Transport core | Handshake, AEAD, acknowledgment, the five delivery classes, fragmentation, flow control, scheduler, controller, pacing, PMTU discovery | Simulated-link tests pass; bench results against the phase 0 baseline |
| 2. Data format and sessions | Bit stream, wire descriptors and codec, schema hashes, session messages | Codec round trips, refusal of every bound violation, fuzzing without findings |
| 3. Depot | Object store, push, pull, refs, locks, CLI, sparse working copy | Push and pull of the Bistro asset tree between a Mac and a Windows machine, interrupted and resumed, with hashes equal |
| 4. Editor collaboration and agents | Host and join, edit batches, journal groups, gestures, presence, session claims and feed, task board | Two editors and two agent groups edit Bistro; both saved scenes match byte for byte |
| 5. Game sessions | `world` service, opt-in components, input, prediction for the FPS module | 20 clients (bots) on a Bistro session with measured bytes per client and correction counts under 2% loss |
| 6. Asset streaming | Remote content mount, then progressive mips and LOD ranges | Bistro streamed from a depot: time to first frame and to full detail; captures equal to a local load |
| 7. View streaming | Encoder and decoder hooks, view channel with FEC | Glass-to-glass latency on Bistro, with a camera-flash or frame-counter method |
| 8. Scale | Headless server executable for Windows and macOS that simulates without a renderer, server workers, Registered I/O on Windows, 400 co-located bots | Server tick cost, system-call CPU and egress at 400 clients on each host, against the estimate table |

Phases 3 and 4 come before game sessions because the editor uses them
first, and editor collaboration needs the depot for assets.

## Performance targets

These are targets for the evidence of each phase, not results.

| Target | Value | Phase |
|---|---|---|
| Header overhead per packet | 23 B plus UDP/IP | 1 |
| Framing of a small unreliable message | 3 B | 1 |
| Heap allocations per packet in steady state | 0 | 1 |
| Transport CPU per 1,200 B packet, sent and received, without system calls | at most 2 µs on M1 Pro and Ryzen 5 2600 | 1 |
| Latency above raw UDP on a local network | p99 at most 0.2 ms | 1 |
| Depot throughput on 1 GbE between Mac and Windows | at least 110 MB/s, at most 1.5 cores per side | 3 |
| Collaborative edit round trip | RTT plus at most one frame | 4 |
| 20-player session bandwidth | at most 64 kbps down per client | 5 |
| Glass-to-glass latency on a local network | at most 50 ms p95 at 2560×1440, 60 Hz | 7 |
| 400 co-located clients | at most 300 kbps down per client; snapshot encode within the server tick on a Ryzen 5 2600 and on an M1 Pro | 8 |

On Windows and macOS, user-space UDP without segmentation offload costs
about one system call per datagram. Bulk throughput above 1 GbE on those
hosts depends on jumbo frames and, on Windows, Registered I/O. Phase 3
measures this limit and does not assume it.

## Verification

- **Simulated link.** CPU tests drive two cores through a link model with
  latency, jitter, loss, reordering, duplication, bandwidth limits and MTU
  limits, from a fixed seed and a virtual clock. Each test names its
  failure:
  - Reliable channels deliver every message exactly once at 0% to 30%
    loss, and ordered channels deliver in order.
  - `SEQUENCED` never delivers an older message after a newer one.
  - `DEADLINE` messages stop being resent after their deadline.
  - A peer that ignores WINDOW limits is closed with `FLOW_CONTROL`, and
    the receiver's memory stays within its caps.
  - A server sends at most three times the received bytes to an address
    that is not yet validated.
  - A replayed packet is dropped, and a replayed Initial creates no
    session.
  - A lost PMTU probe does not lower the congestion window.
  - Two bulk connections over one bottleneck converge to equal shares, and
    a `BACKGROUND` connection yields to an interactive one.
- **Fuzzing.** The frame parser, the handshake and the schema decoder run
  under a fuzzer with ASan and UBSan, following
  [vkr-validation](../../.codex/skills/vkr-validation/SKILL.md).
- **Real links.** `vkr_net_bench` runs between a Mac and a Windows
  machine on the local network, and through emulated WAN conditions (the
  `clumsy` tool on Windows, `dnctl` and `pfctl` on macOS). Reports record
  the commands, the configuration and the spread.
- **Scenes.** Every scene-based check uses Bistro: depot transfer of its
  assets, collaborative editing, streaming and view streaming.
  Performance claims use matched Release builds with graphics validation
  off.
- **Security review.** An independent review of the handshake, the
  credential checks and the decoders comes before any deployment outside a
  trusted network.

## Owner decisions of 2026-10-09

| Decision | Choice | Consequence |
|---|---|---|
| Crypto library | A pinned libsodium release, vendored: X25519, Ed25519, BLAKE2b, ChaCha20-Poly1305 and AES-256-GCM | Phase 0 adds a CMake target for its sources, because libsodium ships no CMake build. The transport refuses to start when `crypto_aead_aes256gcm_is_available()` is false; every CPU of ADR-083 has the AES instructions. Monocypher (no AES hardware path) and AEGIS-128L (32 B tag in libsodium) were rejected. |
| Transport | The custom transport this document specifies, not a QUIC library | The project owns loss recovery, congestion control and the handshake, so the security review and the simulated-link tests are required gates. |
| Depot versioning | Own commits, trees, refs and locks | Git LFS is excluded because its batch API is JSON over HTTPS. Depot repositories need their own CLI and editor integration. |
| Server platforms | Windows and macOS only; Linux later | Every server, including the phase 8 scale test, runs on Windows or macOS. Windows servers use Registered I/O in phase 8. A macOS server sends and receives one datagram per system call. A Linux server build is future work. |

## Open decisions

Each decision has a recommendation. Phase 0 can start with the
recommendations, and the owner can change them before the phase that
depends on them.

| Decision | Recommendation | Tradeoff |
|---|---|---|
| Schema codec | Table-driven descriptors | A Bakery-generated C codec is faster per message but adds a code generator. Hot messages (snapshot updates, input) get hand-written encoders on the same bit stream, tested against the table codec. |
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
- **Throughput on Windows and macOS servers.** One system call per
  datagram may limit bulk transfers above 1 GbE and servers beyond the
  400-client case. Phases 3 and 8 measure both. Step B needs measured
  server capacity before it chooses between more Windows workers and a
  Linux server build.
- **Prediction needs gameplay changes.** Prediction and replay need the
  state separation of the entity behavior proposal before phase 5 can
  meet its exit evidence.
- **Scope of step B.** Persistent worlds need distributed simulation that
  this proposal does not specify.

## Related documents

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
