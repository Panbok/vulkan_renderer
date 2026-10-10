#include "editor_session.h"

#include "editor_internal.h"

#include "core/logger.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"
#include "filesystem/filesystem.h"
#include "net/vkr_net_scene_edit.h"
#include "renderer/systems/vkr_scene_terrain.h"
#include "vkr_bitstream.h"
#include "vkr_net_crypto.h"
#include "vkr_net_host.h"
#include "vkr_net_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The collaboration service, version 1 (docs/proposals/network-protocol.md,
 * "Editor collaboration"). The schema hash names this file's message layout;
 * a layout change takes a new version and hash.
 *
 * Channel 0, reliable and ordered, carries the session: HELLO and WELCOME,
 * EDIT from a participant, RESULT for a refused one, APPLIED in session
 * order, PEER_LEFT, and for agents ASK, ANSWER, SHARED (the host's claims
 * and tasks) and REVIEWED (a batch accepted on some editor). EDIT and
 * APPLIED also name the new terrains a batch creates, which every editor
 * stages before applying it. Channel 1,
 * sequenced, carries PRESENCE. Every message starts with its 4-bit type. EDIT
 * and APPLIED name the batch's agent author and label; an edit's body travels
 * as aligned bytes after its header, so the host relays a participant's body
 * unchanged:
 *
 * APPLIED then carries the terrain samples the edit changed on the host
 * (session_terrain_results), which other editors write instead of running
 * the terrain ops again.
 *
 *   SINGLE  one edit (vkr_net_scene_edit.h)
 *   BATCH   container 16, count varuint, then each edit
 *   UNDO, REDO  empty
 *   REVERT  container 16, the session sequence of the batch it reverts */
#define SESSION_SERVICE_VERSION 1u
#define SESSION_SCHEMA_HASH 0x434f4c4c4142000bull
#define SESSION_EDIT_CHANNEL 0u
#define SESSION_PRESENCE_CHANNEL 1u
#define SESSION_MESSAGE_MAX (8u << 20)
#define SESSION_PRESENCE_US 100000ull
/* Presence while this editor drags a gizmo, so others follow the drag. */
#define SESSION_PRESENCE_DRAG_US 33000ull
/* Applied edits the host keeps for editors that join later; past it, the
   ones every participant received leave and joining is refused. */
#define SESSION_LOG_BYTES_MAX (64ull << 20)
/* Gizmo drags a participant applied ahead of the host. */
#define SESSION_SPECULATIVE_MAX 16u
/* Tokens of the batches this editor applies for others, and of the edits
   it forwards without a requester's token. */
#define SESSION_TOKEN_APPLY 0x8000000000000000ull
#define SESSION_TOKEN_FORWARD 0x4000000000000000ull
/* Agent requests waiting on the host, answers waiting on a participant,
   batch notes of one build and applied batches waiting for the feed. */
#define SESSION_ASK_MAX 64u
#define SESSION_ANSWER_MAX 16u
#define SESSION_NOTE_MAX 8u
#define SESSION_APPLIED_MAX 16u
#define SESSION_ACCEPTED_MAX 64u
/* New terrains one batch stages on every editor. */
#define SESSION_STAGE_MAX 16u
/* Bodies one physics batch of the session holds, as a ragdoll's. */
#define SESSION_PHYSICS_CHANGE_MAX 128u

typedef enum SessionMessage {
  SESSION_MSG_HELLO = 1,
  SESSION_MSG_WELCOME,
  SESSION_MSG_EDIT,
  SESSION_MSG_RESULT,
  SESSION_MSG_APPLIED,
  SESSION_MSG_PRESENCE,
  SESSION_MSG_PEER_LEFT,
  SESSION_MSG_ASK,
  SESSION_MSG_ANSWER,
  SESSION_MSG_SHARED,
  SESSION_MSG_REVIEWED,
} SessionMessage;

typedef enum SessionEditKind {
  SESSION_EDIT_SINGLE = 0,
  SESSION_EDIT_BATCH,
  SESSION_EDIT_UNDO,
  SESSION_EDIT_REDO,
  SESSION_EDIT_REVERT,
  SESSION_EDIT_WIRE_COUNT,
  /* Local only: answers a batch this editor forwarded and the host refused. */
  SESSION_EDIT_REFUSED = SESSION_EDIT_WIRE_COUNT,
} SessionEditKind;

typedef enum SessionWelcome {
  SESSION_WELCOME_OK = 0,
  /* The joining editor's scene is not the session's base. */
  SESSION_WELCOME_SCENE,
  SESSION_WELCOME_FULL,
  /* The session's history no longer fits for a new editor. */
  SESSION_WELCOME_HISTORY,
} SessionWelcome;

typedef enum SessionRefusal {
  SESSION_REFUSED_FAILED = 1,
  /* An undo or redo past the edits of the session. */
  SESSION_REFUSED_HISTORY,
  /* A revert of a batch the session did not make. */
  SESSION_REFUSED_GROUP,
} SessionRefusal;

/* A new flat terrain to stage (vkr_editor_session_note_terrain). */
typedef struct SessionStage {
  char path[SCENE_TERRAIN_PATH_CAPACITY];
  uint32_t cells;
  float32_t spacing;
  float32_t height_min;
  float32_t height_max;
  float32_t height;
} SessionStage;

/* An edit waiting to apply here. The host's queue holds this editor's and
 * the participants' edits in arrival order; a participant's holds the host's
 * APPLIED edits in session order. `payload` is owned. */
typedef struct SessionEdit {
  uint8_t kind;
  uint8_t author;
  /* Host: made in this editor, so no RESULT travels for it. */
  bool8_t local;
  uint8_t refusal;
  uint64_t token;
  uint64_t seq;
  uint8_t *payload;
  uint32_t size;
  /* A batch's agent author, as the session knows it, and label; empty for
     the designer's edits. */
  char agent[VKR_EDITOR_AUTHOR_CAPACITY];
  char label[96];
  /* An agent batch that waits for the designer's review. */
  bool8_t review;
  /* APPLIED: the terrain samples the edit changed on the host; owned. */
  uint8_t *results;
  uint32_t results_size;
  /* New terrains to stage before the edit applies; owned. */
  SessionStage *stages;
  uint32_t stage_count;
} SessionEdit;

/* A terrain edit the host injected: the edit's index in its batch (zero for
   a single edit), the terrain and the samples it may change. */
typedef struct SessionTerrain {
  uint32_t index;
  VkrEntityId entity;
  VkrHeightfieldRect rect;
} SessionTerrain;

/* The author and label ops gave the batch it submits under `token`. */
typedef struct SessionNote {
  uint64_t token;
  char author[VKR_EDITOR_AUTHOR_CAPACITY];
  char label[96];
  bool8_t review;
  SessionStage stages[SESSION_STAGE_MAX];
  uint32_t stage_count;
} SessionNote;

/* A batch some editor accepted, by its journal group here. */
typedef struct SessionAccepted {
  uint16_t container;
  uint64_t group;
} SessionAccepted;

typedef struct SessionAnswerSlot {
  bool8_t used;
  VkrEditorSessionAnswer answer;
} SessionAnswerSlot;

/* The journal group a session batch made here, for a later revert, and
   the agent that made it, for the feed. */
typedef struct SessionGroup {
  uint64_t seq;
  uint64_t group;
  uint16_t container;
  char agent[VKR_EDITOR_AUTHOR_CAPACITY];
  char label[96];
} SessionGroup;

/* A participant's connection, on the host. */
typedef struct SessionLink {
  bool8_t live;
  bool8_t welcomed;
  VkrNetConnectionId connection;
  uint8_t id;
  /* Log entries sent to it. */
  uint32_t log_sent;
} SessionLink;

typedef struct SessionBytes {
  uint8_t *data;
  uint32_t size;
} SessionBytes;

typedef struct SessionBuffer {
  uint8_t *data;
  uint32_t capacity;
} SessionBuffer;

/* What the loaded journals hold, to tell whether an edit applied. */
typedef struct SessionSignature {
  uint64_t revision;
  uint64_t cursor;
  uint64_t newest;
} SessionSignature;

/* A gizmo drag a participant applied before the host ordered it. */
typedef struct SessionSpeculative {
  uint64_t token;
  uint64_t group;
  uint16_t container;
} SessionSpeculative;

/* The edit injected into the request slots during the last build. */
typedef struct SessionInflight {
  bool8_t active;
  /* A local undo of a speculative or untravelled edit; it never travels. */
  bool8_t rollback;
  /* A single edit applied as a batch of one (session_place). */
  bool8_t as_batch;
  SessionEdit edit;
  uint64_t slot_token;
  uint16_t container;
  /* Edits of an injected batch, for the change feed. */
  uint32_t item_count;
  /* Host: its terrain edits, whose results APPLIED carries. */
  uint32_t terrain_count;
  SessionSignature before;
} SessionInflight;

struct VkrEditorSession {
  VkrAllocator *allocator;
  uint8_t mode;
  VkrNetHost *host;
  VkrNetSession *net;
  char name[VKR_EDITOR_SESSION_NAME_MAX];
  char address[64];
  char key[65];
  char error[192];
  uint64_t scene_generation;
  uint64_t now;
  /* Set from a network callback; the update ends the session after it. */
  bool8_t leave_pending;

  /* Participant. */
  VkrNetConnectionId connection;
  bool8_t welcomed;
  uint8_t self_id;
  uint8_t digest[32];
  /* Edits the host's history held when it welcomed this editor. */
  uint64_t welcome_log;
  uint64_t receive_seq;

  /* Host. */
  SessionLink links[VKR_EDITOR_SESSION_PEERS_MAX];
  SessionBytes *log;
  uint32_t log_count;
  uint32_t log_capacity;
  uint32_t log_first;
  uint64_t log_bytes;
  /* Steps of the session an undo or redo may reach. */
  uint32_t undo_depth;
  uint32_t redo_depth;

  /* Both: the next session sequence this editor applies (the host gives
     it), the queue, the batches' groups and the edit in flight. */
  uint64_t apply_seq;
  SessionEdit *queue;
  uint32_t queue_head;
  uint32_t queue_count;
  uint32_t queue_capacity;
  SessionGroup *groups;
  uint32_t group_count;
  uint32_t group_capacity;
  SessionSpeculative speculative[SESSION_SPECULATIVE_MAX];
  uint32_t speculative_count;
  uint32_t rollbacks;
  SessionInflight inflight;
  uint64_t next_token;
  VkrSampleEditBatchItem *items;
  uint32_t item_capacity;
  SessionTerrain *terrain;
  uint32_t terrain_capacity;
  SessionBuffer results;
  /* What an injected single edit's request points to: collision layers or
     a physics batch. */
  VkrSceneCollisionLayers edit_layers;
  VkrScenePhysicsChange *edit_physics;
  VkrSampleEditBatchResult refused;
  VkrSceneEditRequest scratch;
  SessionBuffer payload;
  SessionBuffer message;

  VkrEditorSessionPeer peers[VKR_EDITOR_SESSION_PEERS_MAX];
  uint32_t peer_count;
  uint64_t presence_at;

  /* The Session window's fields; they survive a reset. */
  char form_name[VKR_EDITOR_SESSION_NAME_MAX];
  char tool[24];
  char form_bind[64];
  char form_address[128];
  char form_key[72];

  /* Agent federation. `epoch` survives a reset. */
  uint64_t epoch;
  SessionNote notes[SESSION_NOTE_MAX];
  uint32_t note_next;
  /* Terrains noted this build, for the batch noted next. */
  SessionStage stages[SESSION_STAGE_MAX];
  uint32_t stage_count;
  VkrEditorSessionApplied applied[SESSION_APPLIED_MAX];
  uint32_t applied_head;
  uint32_t applied_count;
  SessionAccepted accepted[SESSION_ACCEPTED_MAX];
  uint32_t accepted_head;
  uint32_t accepted_count;
  /* Host: participants' requests, and the claims and tasks as sent. */
  VkrEditorSessionAsk asks[SESSION_ASK_MAX];
  uint32_t ask_head;
  uint32_t ask_count;
  uint8_t *shared_bytes;
  uint32_t shared_size;
  /* Participant: answers by token, and the host's claims and tasks. */
  SessionAnswerSlot answers[SESSION_ANSWER_MAX];
  uint64_t shared_revision;
  VkrEditorClaim shared_claims[VKR_EDITOR_CLAIM_MAX];
  uint32_t shared_claim_count;
  VkrEditorTask shared_tasks[VKR_EDITOR_TASK_MAX];
  uint32_t shared_task_count;
};

// =============================================================================
// Small helpers
// =============================================================================

static void session_write_float(VkrBitWriter *writer, float32_t value) {
  uint32_t bits = 0u;
  MemCopy(&bits, &value, sizeof(bits));
  vkr_bit_write(writer, bits, 32u);
}

static float32_t session_read_float(VkrBitReader *reader) {
  const uint32_t bits = (uint32_t)vkr_bit_read(reader, 32u);
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

static void session_write_text(VkrBitWriter *writer, const char *text) {
  const uint32_t length = (uint32_t)strlen(text);
  vkr_bit_write_varuint(writer, length);
  vkr_bit_write_bytes(writer, text, length);
}

static bool8_t session_read_text(VkrBitReader *reader, char *out,
                                 uint32_t capacity) {
  const uint64_t length = vkr_bit_read_varuint(reader);
  if (length >= capacity) {
    return false_v;
  }
  const uint8_t *bytes = vkr_bit_read_bytes(reader, (uint32_t)length);
  if (!bytes) {
    return false_v;
  }
  MemCopy(out, bytes, length);
  out[length] = '\0';
  return true_v;
}

static void session_write_ref(VkrBitWriter *writer, const VkrEntityRef *ref) {
  vkr_bit_write(writer, vkr_load_le_u64(ref->bytes), 64u);
  vkr_bit_write(writer, vkr_load_le_u64(ref->bytes + 8), 64u);
}

static void session_read_ref(VkrBitReader *reader, VkrEntityRef *out) {
  vkr_store_le_u64(out->bytes, vkr_bit_read(reader, 64u));
  vkr_store_le_u64(out->bytes + 8, vkr_bit_read(reader, 64u));
}

static void session_hex(const uint8_t *bytes, uint32_t size, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (uint32_t i = 0u; i < size; ++i) {
    out[2u * i] = digits[bytes[i] >> 4];
    out[2u * i + 1u] = digits[bytes[i] & 15u];
  }
  out[2u * size] = '\0';
}

static bool8_t session_parse_hex(const char *text, uint8_t *out,
                                 uint32_t size) {
  if (!text || strlen(text) != 2u * size) {
    return false_v;
  }
  for (uint32_t i = 0u; i < 2u * size; ++i) {
    const char c = text[i];
    uint32_t digit = 0u;
    if (c >= '0' && c <= '9') {
      digit = (uint32_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = (uint32_t)(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
      digit = (uint32_t)(c - 'A') + 10u;
    } else {
      return false_v;
    }
    out[i / 2u] = (uint8_t)((i & 1u) ? (out[i / 2u] | digit) : (digit << 4));
  }
  return true_v;
}

/* A numeric address or a host name, with an optional port. */
static bool8_t session_address(const char *text, VkrNetAddress *out) {
  if (vkr_net_address_parse(text, VKR_EDITOR_SESSION_PORT, out)) {
    return true_v;
  }
  char host[256];
  const char *colon = strrchr(text, ':');
  uint32_t port = VKR_EDITOR_SESSION_PORT;
  size_t length = strlen(text);
  if (colon) {
    port = (uint32_t)strtoul(colon + 1, NULL, 10);
    length = (size_t)(colon - text);
  }
  if (length == 0u || length >= sizeof(host) || port == 0u || port > 65535u) {
    return false_v;
  }
  MemCopy(host, text, length);
  host[length] = '\0';
  return vkr_net_address_resolve(host, (uint16_t)port, true_v, out);
}

static bool8_t session_buffer_grow(SessionBuffer *buffer) {
  const uint32_t capacity = buffer->capacity ? buffer->capacity * 2u : 4096u;
  if (capacity > SESSION_MESSAGE_MAX) {
    return false_v;
  }
  uint8_t *grown = realloc(buffer->data, capacity);
  if (!grown) {
    return false_v;
  }
  buffer->data = grown;
  buffer->capacity = capacity;
  return true_v;
}

// =============================================================================
// Scenes, journals and the digest
// =============================================================================

static VkrNetSceneEditScenes session_scenes(const VkrSampleUiFrame *frame) {
  VkrNetSceneEditScenes scenes = {.world = frame->world};
  scenes.containers[0] = frame->scene;
  for (uint32_t i = 0u; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    scenes.containers[1u + i] = frame->additive[i];
  }
  return scenes;
}

static const VkrScene *session_container(const VkrSampleUiFrame *frame,
                                         uint32_t world) {
  if (world == VKR_SCENE_WORLD_ROOT_ID) {
    return frame->world;
  }
  if (world == 0u) {
    return frame->scene;
  }
  return world <= VKR_SCENE_ADDITIVE_MAX ? frame->additive[world - 1u] : NULL;
}

static SessionSignature session_signature(const VkrSampleUiFrame *frame) {
  const VkrSceneEditState *journals[2u + VKR_SCENE_ADDITIVE_MAX] = {
      frame->scene ? frame->edits : NULL,
      frame->world ? frame->world_edits : NULL};
  for (uint32_t i = 0u; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    journals[2u + i] = frame->additive[i] ? frame->additive_edits[i] : NULL;
  }
  SessionSignature signature = {0};
  for (uint32_t i = 0u; i < ArrayCount(journals); ++i) {
    const VkrSceneEditState *journal = journals[i];
    if (!journal) {
      continue;
    }
    signature.revision += journal->revision;
    signature.cursor += journal->undo_cursor;
    if (journal->undo_count) {
      signature.newest = Max(signature.newest,
                             journal->undo[journal->undo_count - 1u].sequence);
    }
  }
  return signature;
}

typedef struct SessionDigestRecord {
  uint16_t container;
  uint8_t ref[16];
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
} SessionDigestRecord;

static int session_digest_compare(const void *a, const void *b) {
  const SessionDigestRecord *left = a;
  const SessionDigestRecord *right = b;
  if (left->container != right->container) {
    return left->container < right->container ? -1 : 1;
  }
  return memcmp(left->ref, right->ref, sizeof(left->ref));
}

/* One entity's identity, name, pose and parent. */
static bool8_t session_digest_entity(const VkrScene *scene, VkrEntityId entity,
                                     uint16_t container,
                                     SessionDigestRecord *out) {
  VkrEntityRef ref;
  VkrSceneEditValues values;
  if (!vkr_scene_entity_ref(scene, entity, &ref) ||
      vkr_entity_ref_empty(&ref) ||
      !vkr_scene_edit_read(scene, entity, &values)) {
    return false_v;
  }
  out->container = container;
  MemCopy(out->ref, ref.bytes, sizeof(out->ref));
  const float32_t pose[10] = {values.position.x, values.position.y,
                              values.position.z, values.rotation.x,
                              values.rotation.y, values.rotation.z,
                              values.rotation.w, values.scale.x,
                              values.scale.y,    values.scale.z};
  uint8_t bytes[sizeof(pose)];
  for (uint32_t i = 0u; i < ArrayCount(pose); ++i) {
    uint32_t bits = 0u;
    MemCopy(&bits, &pose[i], sizeof(bits));
    vkr_store_le_u32(bytes + 4u * i, bits);
  }
  VkrEntityRef parent_ref = {0};
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  if (transform && transform->parent.u64) {
    (void)vkr_scene_entity_ref(scene, transform->parent, &parent_ref);
  }
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  vkr_sha256_update(&hash, values.name, strlen(values.name) + 1u);
  vkr_sha256_update(&hash, bytes, sizeof(bytes));
  vkr_sha256_update(&hash, parent_ref.bytes, sizeof(parent_ref.bytes));
  vkr_sha256_final(&hash, out->digest);
  return true_v;
}

void vkr_editor_session_digest(const VkrSampleUiFrame *frame,
                               uint8_t out_digest[32]) {
  static const uint16_t worlds[2u + VKR_SCENE_ADDITIVE_MAX] = {
      0u, 1u, 2u, 3u, 4u, 5u, 6u, VKR_SCENE_WORLD_ROOT_ID};
  uint64_t capacity = 0u;
  for (uint32_t w = 0u; w < ArrayCount(worlds); ++w) {
    const VkrScene *scene = session_container(frame, worlds[w]);
    capacity += scene ? scene->world->dir.living : 0u;
  }
  SessionDigestRecord *records =
      capacity ? malloc(capacity * sizeof(*records)) : NULL;
  uint64_t count = 0u;
  for (uint32_t w = 0u; records && w < ArrayCount(worlds); ++w) {
    const VkrScene *scene = session_container(frame, worlds[w]);
    for (uint32_t i = 0u; scene && i < scene->world->dir.living; ++i) {
      const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
      if (vkr_scene_entity_alive(scene, entity) &&
          session_digest_entity(scene, entity, worlds[w], &records[count])) {
        count += 1u;
      }
    }
  }
  if (count) {
    qsort(records, count, sizeof(*records), session_digest_compare);
  }
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  vkr_sha256_update(&hash, "vkr.collab.digest.1", 19u);
  for (uint64_t i = 0u; i < count; ++i) {
    vkr_sha256_update(&hash, &records[i].container,
                      sizeof(records[i].container));
    vkr_sha256_update(&hash, records[i].ref, sizeof(records[i].ref));
    vkr_sha256_update(&hash, records[i].digest, sizeof(records[i].digest));
  }
  vkr_sha256_final(&hash, out_digest);
  free(records);
}

// =============================================================================
// Edits: payloads, the queue and journal groups
// =============================================================================

/* What an edit payload encodes. */
typedef struct SessionPayload {
  uint8_t kind;
  const VkrNetSceneEditScenes *scenes;
  const VkrSceneEditRequest *request;
  const VkrSampleEditBatchItem *items;
  uint32_t count;
  uint16_t container;
  uint64_t revert_seq;
  char error[192];
} SessionPayload;

/* A creation needs its document id, and a duplicate the seed of its
   copies' ids, before it travels: every editor then gives the new entities
   the same ids. */
static const VkrSceneEditRequest *
session_with_ref(VkrEditorSession *session,
                 const VkrSceneEditRequest *request) {
  if ((request->action != VKR_SCENE_EDIT_CREATE &&
       request->action != VKR_SCENE_EDIT_DUPLICATE) ||
      !vkr_entity_ref_empty(&request->values.ref)) {
    return request;
  }
  session->scratch = *request;
  vkr_scene_entity_ref_generate(&session->scratch.values.ref);
  return &session->scratch;
}

static bool8_t session_put_payload(VkrEditorSession *session,
                                   VkrBitWriter *writer,
                                   SessionPayload *payload) {
  switch (payload->kind) {
  case SESSION_EDIT_SINGLE:
    return vkr_net_scene_edit_write(
        writer, payload->scenes, session_with_ref(session, payload->request),
        -1, -1, payload->error, sizeof(payload->error));
  case SESSION_EDIT_BATCH:
    vkr_bit_write(writer, payload->container, 16u);
    vkr_bit_write_varuint(writer, payload->count);
    for (uint32_t i = 0u; i < payload->count; ++i) {
      const VkrSampleEditBatchItem *item = &payload->items[i];
      if (!vkr_net_scene_edit_write(writer, payload->scenes,
                                    session_with_ref(session, &item->request),
                                    item->entity_ref, item->parent_ref,
                                    payload->error, sizeof(payload->error))) {
        return false_v;
      }
    }
    return true_v;
  case SESSION_EDIT_REVERT:
    vkr_bit_write(writer, payload->container, 16u);
    vkr_bit_write_varuint(writer, payload->revert_seq);
    return true_v;
  default:
    return true_v;
  }
}

/* Encodes `payload` into an owned copy. A CREATE given no document id gets
   one here, so the bytes, not the request, are the edit from now on. */
static bool8_t session_encode_payload(VkrEditorSession *session,
                                      SessionPayload *payload,
                                      uint8_t **out_bytes, uint32_t *out_size) {
  *out_bytes = NULL;
  *out_size = 0u;
  for (;;) {
    VkrBitWriter writer;
    vkr_bit_writer_init(&writer, session->payload.data,
                        session->payload.capacity);
    if (session->payload.capacity) {
      if (!session_put_payload(session, &writer, payload)) {
        return false_v;
      }
      const uint32_t size = vkr_bit_writer_finish(&writer);
      if (size || (!writer.overflow && writer.bytes == 0u)) {
        *out_size = size;
        *out_bytes = malloc(Max(size, 1u));
        if (!*out_bytes) {
          snprintf(payload->error, sizeof(payload->error), "Out of memory.");
          return false_v;
        }
        if (size) {
          MemCopy(*out_bytes, session->payload.data, size);
        }
        return true_v;
      }
    }
    if (!session_buffer_grow(&session->payload)) {
      snprintf(payload->error, sizeof(payload->error),
               "The edit is larger than a session message.");
      return false_v;
    }
  }
}

static void session_edit_release(SessionEdit *edit) {
  free(edit->payload);
  free(edit->results);
  free(edit->stages);
  MemZero(edit, sizeof(*edit));
}

static bool8_t session_queue_push(VkrEditorSession *session,
                                  const SessionEdit *edit) {
  if (session->queue_head + session->queue_count == session->queue_capacity) {
    if (session->queue_head) {
      memmove(session->queue, session->queue + session->queue_head,
              session->queue_count * sizeof(*session->queue));
      session->queue_head = 0u;
    } else {
      const uint32_t capacity =
          session->queue_capacity ? session->queue_capacity * 2u : 64u;
      SessionEdit *grown =
          realloc(session->queue, capacity * sizeof(*session->queue));
      if (!grown) {
        return false_v;
      }
      session->queue = grown;
      session->queue_capacity = capacity;
    }
  }
  session->queue[session->queue_head + session->queue_count++] = *edit;
  return true_v;
}

static SessionEdit session_queue_pop(VkrEditorSession *session) {
  const SessionEdit edit = session->queue[session->queue_head++];
  if (--session->queue_count == 0u) {
    session->queue_head = 0u;
  }
  return edit;
}

static void session_group_add(VkrEditorSession *session, uint64_t seq,
                              uint16_t container, uint64_t group,
                              const SessionEdit *edit) {
  if (!group) {
    return;
  }
  if (session->group_count == session->group_capacity) {
    const uint32_t capacity =
        session->group_capacity ? session->group_capacity * 2u : 64u;
    SessionGroup *grown =
        realloc(session->groups, capacity * sizeof(*session->groups));
    if (!grown) {
      return;
    }
    session->groups = grown;
    session->group_capacity = capacity;
  }
  SessionGroup *entry = &session->groups[session->group_count++];
  *entry = (SessionGroup){.seq = seq, .group = group, .container = container};
  if (edit) {
    snprintf(entry->agent, sizeof(entry->agent), "%s", edit->agent);
    snprintf(entry->label, sizeof(entry->label), "%s", edit->label);
  }
}

static uint64_t session_group_of_seq(const VkrEditorSession *session,
                                     uint16_t container, uint64_t seq) {
  for (uint32_t i = 0u; i < session->group_count; ++i) {
    if (session->groups[i].seq == seq &&
        session->groups[i].container == container) {
      return session->groups[i].group;
    }
  }
  return 0u;
}

static uint64_t session_seq_of_group(const VkrEditorSession *session,
                                     uint16_t container, uint64_t group) {
  for (uint32_t i = 0u; i < session->group_count; ++i) {
    if (session->groups[i].group == group &&
        session->groups[i].container == container) {
      return session->groups[i].seq;
    }
  }
  return 0u;
}

static bool8_t session_reserve_items(VkrEditorSession *session,
                                     uint32_t count) {
  if (count <= session->item_capacity) {
    return true_v;
  }
  VkrSampleEditBatchItem *grown =
      realloc(session->items, count * sizeof(*grown));
  if (!grown) {
    return false_v;
  }
  session->items = grown;
  session->item_capacity = count;
  return true_v;
}

/* A gesture of one author stays one gesture on every editor, and never
   meets another author's. */
static uint64_t session_gesture(uint8_t author, uint64_t gesture) {
  return gesture ? ((uint64_t)(author + 1u) << 56) |
                       (gesture & 0x00ffffffffffffffull)
                 : 0u;
}

// =============================================================================
// Messages
// =============================================================================

static bool8_t session_send(VkrEditorSession *session,
                            VkrNetConnectionId connection, uint8_t channel,
                            const uint8_t *data, uint32_t size) {
  const VkrNetSendOptions options = {0};
  return vkr_net_session_send(session->net, connection, VKR_NET_SERVICE_COLLAB,
                              channel, data, size, &options,
                              session->now) == VKR_NET_SEND_OK;
}

/* EDIT and APPLIED: a header, then the payload's bytes. */
static uint32_t session_encode_edit(VkrEditorSession *session, uint8_t type,
                                    const SessionEdit *edit) {
  for (;;) {
    VkrBitWriter writer;
    vkr_bit_writer_init(&writer, session->message.data,
                        session->message.capacity);
    vkr_bit_write(&writer, type, 4u);
    if (type == SESSION_MSG_APPLIED) {
      vkr_bit_write_varuint(&writer, edit->seq);
      vkr_bit_write(&writer, edit->author, 8u);
    }
    vkr_bit_write(&writer, edit->token, 64u);
    vkr_bit_write(&writer, edit->kind, 4u);
    session_write_text(&writer, edit->agent);
    session_write_text(&writer, edit->label);
    vkr_bit_write(&writer, edit->review, 1u);
    vkr_bit_write_varuint(&writer, edit->stage_count);
    for (uint32_t i = 0u; i < edit->stage_count; ++i) {
      const SessionStage *stage = &edit->stages[i];
      session_write_text(&writer, stage->path);
      vkr_bit_write_varuint(&writer, stage->cells);
      session_write_float(&writer, stage->spacing);
      session_write_float(&writer, stage->height_min);
      session_write_float(&writer, stage->height_max);
      session_write_float(&writer, stage->height);
    }
    vkr_bit_write_varuint(&writer, edit->size);
    vkr_bit_write_bytes(&writer, edit->payload, edit->size);
    if (type == SESSION_MSG_APPLIED) {
      vkr_bit_write_varuint(&writer, edit->results_size);
      vkr_bit_write_bytes(&writer, edit->results, edit->results_size);
    }
    const uint32_t size =
        session->message.capacity ? vkr_bit_writer_finish(&writer) : 0u;
    if (size) {
      return size;
    }
    if (!session_buffer_grow(&session->message)) {
      return 0u;
    }
  }
}

/* Reads an EDIT or APPLIED after its type into `out`, owning a payload
   copy. */
static bool8_t session_read_edit(VkrBitReader *reader, uint8_t type,
                                 SessionEdit *out) {
  MemZero(out, sizeof(*out));
  if (type == SESSION_MSG_APPLIED) {
    out->seq = vkr_bit_read_varuint(reader);
    out->author = (uint8_t)vkr_bit_read(reader, 8u);
  }
  out->token = vkr_bit_read(reader, 64u);
  out->kind = (uint8_t)vkr_bit_read(reader, 4u);
  if (!session_read_text(reader, out->agent, sizeof(out->agent)) ||
      !session_read_text(reader, out->label, sizeof(out->label))) {
    return false_v;
  }
  out->review = (bool8_t)vkr_bit_read(reader, 1u);
  const uint64_t stages = vkr_bit_read_varuint(reader);
  if (stages > SESSION_STAGE_MAX) {
    return false_v;
  }
  if (stages) {
    out->stages = calloc(stages, sizeof(*out->stages));
    if (!out->stages) {
      return false_v;
    }
    out->stage_count = (uint32_t)stages;
  }
  for (uint32_t i = 0u; i < out->stage_count; ++i) {
    SessionStage *stage = &out->stages[i];
    if (!session_read_text(reader, stage->path, sizeof(stage->path))) {
      session_edit_release(out);
      return false_v;
    }
    stage->cells = (uint32_t)vkr_bit_read_varuint(reader);
    stage->spacing = session_read_float(reader);
    stage->height_min = session_read_float(reader);
    stage->height_max = session_read_float(reader);
    stage->height = session_read_float(reader);
  }
  const uint64_t size = vkr_bit_read_varuint(reader);
  if (out->kind >= SESSION_EDIT_WIRE_COUNT || size > SESSION_MESSAGE_MAX) {
    session_edit_release(out);
    return false_v;
  }
  const uint8_t *bytes = vkr_bit_read_bytes(reader, (uint32_t)size);
  if (!bytes) {
    session_edit_release(out);
    return false_v;
  }
  out->payload = malloc(Max((uint32_t)size, 1u));
  if (!out->payload) {
    return false_v;
  }
  MemCopy(out->payload, bytes, size);
  out->size = (uint32_t)size;
  if (type == SESSION_MSG_APPLIED) {
    const uint64_t results = vkr_bit_read_varuint(reader);
    const uint8_t *result_bytes =
        results <= SESSION_MESSAGE_MAX
            ? vkr_bit_read_bytes(reader, (uint32_t)results)
            : NULL;
    if (!result_bytes) {
      session_edit_release(out);
      return false_v;
    }
    /* Copied apart, so its raw arrays start aligned (vkr_net_read_array). */
    if (results) {
      out->results = malloc(results);
      if (!out->results) {
        session_edit_release(out);
        return false_v;
      }
      MemCopy(out->results, result_bytes, results);
      out->results_size = (uint32_t)results;
    }
  }
  if (!vkr_bit_reader_at_end(reader)) {
    session_edit_release(out);
    return false_v;
  }
  return true_v;
}

typedef void (*SessionPut)(VkrBitWriter *writer, const void *context);

/* Builds a message into the growable message buffer. */
static uint32_t session_build(VkrEditorSession *session, SessionPut put,
                              const void *context) {
  for (;;) {
    VkrBitWriter writer;
    vkr_bit_writer_init(&writer, session->message.data,
                        session->message.capacity);
    put(&writer, context);
    const uint32_t size =
        session->message.capacity ? vkr_bit_writer_finish(&writer) : 0u;
    if (size) {
      return size;
    }
    if (!session_buffer_grow(&session->message)) {
      return 0u;
    }
  }
}

static void session_write_vec3(VkrBitWriter *writer, Vec3 value) {
  session_write_float(writer, value.x);
  session_write_float(writer, value.y);
  session_write_float(writer, value.z);
}

static Vec3 session_read_vec3(VkrBitReader *reader) {
  const float32_t x = session_read_float(reader);
  const float32_t y = session_read_float(reader);
  const float32_t z = session_read_float(reader);
  return vec3_new(x, y, z);
}

static void session_write_claim(VkrBitWriter *writer,
                                const VkrEditorClaim *claim) {
  vkr_bit_write_varuint(writer, claim->id);
  vkr_bit_write(writer, claim->container, 16u);
  session_write_text(writer, claim->author);
  session_write_text(writer, claim->name);
  session_write_vec3(writer, claim->min);
  session_write_vec3(writer, claim->max);
}

static bool8_t session_read_claim(VkrBitReader *reader, VkrEditorClaim *out) {
  MemZero(out, sizeof(*out));
  out->id = (uint32_t)vkr_bit_read_varuint(reader);
  out->container = (uint16_t)vkr_bit_read(reader, 16u);
  if (!session_read_text(reader, out->author, sizeof(out->author)) ||
      !session_read_text(reader, out->name, sizeof(out->name))) {
    return false_v;
  }
  out->min = session_read_vec3(reader);
  out->max = session_read_vec3(reader);
  return !reader->overflow;
}

static void session_write_task(VkrBitWriter *writer,
                               const VkrEditorTask *task) {
  vkr_bit_write_varuint(writer, task->id);
  vkr_bit_write(writer, task->state, 2u);
  vkr_bit_write(writer, task->has_region, 1u);
  if (task->has_region) {
    vkr_bit_write(writer, task->container, 16u);
    session_write_vec3(writer, task->min);
    session_write_vec3(writer, task->max);
  }
  session_write_text(writer, task->kind);
  session_write_text(writer, task->title);
  session_write_text(writer, task->requires);
  session_write_text(writer, task->assignee);
  session_write_text(writer, task->note);
}

static bool8_t session_read_task(VkrBitReader *reader, VkrEditorTask *out) {
  MemZero(out, sizeof(*out));
  out->id = (uint32_t)vkr_bit_read_varuint(reader);
  out->state = (uint8_t)vkr_bit_read(reader, 2u);
  out->has_region = (bool8_t)vkr_bit_read(reader, 1u);
  if (out->has_region) {
    out->container = (uint16_t)vkr_bit_read(reader, 16u);
    out->min = session_read_vec3(reader);
    out->max = session_read_vec3(reader);
  }
  return session_read_text(reader, out->kind, sizeof(out->kind)) &&
         session_read_text(reader, out->title, sizeof(out->title)) &&
         session_read_text(reader, out->requires, sizeof(out->requires)) &&
         session_read_text(reader, out->assignee, sizeof(out->assignee)) &&
         session_read_text(reader, out->note, sizeof(out->note));
}

/* ASK: token, kind, author, then every field an ask may use. */
static void session_put_ask(VkrBitWriter *writer, const void *context) {
  const VkrEditorSessionAsk *ask = context;
  vkr_bit_write(writer, SESSION_MSG_ASK, 4u);
  vkr_bit_write(writer, ask->token, 64u);
  vkr_bit_write(writer, ask->kind, 4u);
  session_write_text(writer, ask->author);
  session_write_claim(writer, &ask->claim);
  vkr_bit_write(writer, ask->all, 1u);
  session_write_task(writer, &ask->task);
  session_write_text(writer, ask->kinds);
  session_write_text(writer, ask->capabilities);
  vkr_bit_write_varuint(writer, ask->slots);
}

static bool8_t session_read_ask(VkrBitReader *reader,
                                VkrEditorSessionAsk *out) {
  MemZero(out, sizeof(*out));
  out->token = vkr_bit_read(reader, 64u);
  out->kind = (uint8_t)vkr_bit_read(reader, 4u);
  if (!session_read_text(reader, out->author, sizeof(out->author)) ||
      !session_read_claim(reader, &out->claim)) {
    return false_v;
  }
  out->all = (bool8_t)vkr_bit_read(reader, 1u);
  return session_read_task(reader, &out->task) &&
         session_read_text(reader, out->kinds, sizeof(out->kinds)) &&
         session_read_text(reader, out->capabilities,
                           sizeof(out->capabilities)) &&
         (out->slots = (uint32_t)vkr_bit_read_varuint(reader), true_v) &&
         vkr_bit_reader_at_end(reader) &&
         out->kind >= VKR_EDITOR_ASK_CLAIM_SET &&
         out->kind <= VKR_EDITOR_ASK_TASK_SLOTS;
}

static void session_put_answer(VkrBitWriter *writer, const void *context) {
  const VkrEditorSessionAnswer *answer = context;
  vkr_bit_write(writer, SESSION_MSG_ANSWER, 4u);
  vkr_bit_write(writer, answer->token, 64u);
  vkr_bit_write(writer, answer->ok, 1u);
  session_write_text(writer, answer->code);
  session_write_text(writer, answer->error);
  session_write_claim(writer, &answer->claim);
  vkr_bit_write(writer, answer->has_task, 1u);
  if (answer->has_task) {
    session_write_task(writer, &answer->task);
  }
  vkr_bit_write_varuint(writer, answer->released_count);
  for (uint32_t i = 0u; i < answer->released_count; ++i) {
    vkr_bit_write_varuint(writer, answer->released[i]);
  }
}

static bool8_t session_read_answer(VkrBitReader *reader,
                                   VkrEditorSessionAnswer *out) {
  MemZero(out, sizeof(*out));
  out->token = vkr_bit_read(reader, 64u);
  out->ok = (bool8_t)vkr_bit_read(reader, 1u);
  if (!session_read_text(reader, out->code, sizeof(out->code)) ||
      !session_read_text(reader, out->error, sizeof(out->error)) ||
      !session_read_claim(reader, &out->claim)) {
    return false_v;
  }
  out->has_task = (bool8_t)vkr_bit_read(reader, 1u);
  if (out->has_task && !session_read_task(reader, &out->task)) {
    return false_v;
  }
  const uint64_t released = vkr_bit_read_varuint(reader);
  if (released > VKR_EDITOR_CLAIM_MAX) {
    return false_v;
  }
  out->released_count = (uint32_t)released;
  for (uint32_t i = 0u; i < out->released_count; ++i) {
    out->released[i] = (uint32_t)vkr_bit_read_varuint(reader);
  }
  return vkr_bit_reader_at_end(reader);
}

typedef struct SessionShared {
  const VkrEditorClaim *claims;
  uint32_t claim_count;
  const VkrEditorTask *tasks;
  uint32_t task_count;
} SessionShared;

static void session_put_shared(VkrBitWriter *writer, const void *context) {
  const SessionShared *shared = context;
  vkr_bit_write(writer, SESSION_MSG_SHARED, 4u);
  vkr_bit_write_varuint(writer, shared->claim_count);
  for (uint32_t i = 0u; i < shared->claim_count; ++i) {
    session_write_claim(writer, &shared->claims[i]);
  }
  vkr_bit_write_varuint(writer, shared->task_count);
  for (uint32_t i = 0u; i < shared->task_count; ++i) {
    session_write_task(writer, &shared->tasks[i]);
  }
}

static uint32_t session_encode_presence(const VkrEditorSessionPeer *peer,
                                        uint8_t *out, uint32_t capacity) {
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, out, capacity);
  vkr_bit_write(&writer, SESSION_MSG_PRESENCE, 4u);
  vkr_bit_write(&writer, peer->id, 8u);
  session_write_text(&writer, peer->name);
  session_write_text(&writer, peer->tool);
  vkr_bit_write(&writer, peer->has_camera, 1u);
  if (peer->has_camera) {
    session_write_float(&writer, peer->camera_position.x);
    session_write_float(&writer, peer->camera_position.y);
    session_write_float(&writer, peer->camera_position.z);
    session_write_float(&writer, peer->camera_yaw);
    session_write_float(&writer, peer->camera_pitch);
  }
  vkr_bit_write(&writer, peer->has_selection, 1u);
  if (peer->has_selection) {
    vkr_bit_write(&writer, peer->selection_container, 16u);
    session_write_ref(&writer, &peer->selection);
  }
  vkr_bit_write(&writer, peer->dragging, 1u);
  if (peer->dragging) {
    vkr_bit_write(&writer, peer->drag_container, 16u);
    session_write_ref(&writer, &peer->drag);
    for (uint32_t i = 0u; i < 16u; ++i) {
      session_write_float(&writer, peer->drag_world.elements[i]);
    }
  }
  return vkr_bit_writer_finish(&writer);
}

static bool8_t session_read_presence(VkrBitReader *reader,
                                     VkrEditorSessionPeer *out) {
  MemZero(out, sizeof(*out));
  out->id = (uint8_t)vkr_bit_read(reader, 8u);
  if (!session_read_text(reader, out->name, sizeof(out->name)) ||
      !session_read_text(reader, out->tool, sizeof(out->tool))) {
    return false_v;
  }
  out->has_camera = (bool8_t)vkr_bit_read(reader, 1u);
  if (out->has_camera) {
    const float32_t x = session_read_float(reader);
    const float32_t y = session_read_float(reader);
    const float32_t z = session_read_float(reader);
    out->camera_position = vec3_new(x, y, z);
    out->camera_yaw = session_read_float(reader);
    out->camera_pitch = session_read_float(reader);
  }
  out->has_selection = (bool8_t)vkr_bit_read(reader, 1u);
  if (out->has_selection) {
    out->selection_container = (uint16_t)vkr_bit_read(reader, 16u);
    session_read_ref(reader, &out->selection);
  }
  out->dragging = (bool8_t)vkr_bit_read(reader, 1u);
  if (out->dragging) {
    out->drag_container = (uint16_t)vkr_bit_read(reader, 16u);
    session_read_ref(reader, &out->drag);
    for (uint32_t i = 0u; i < 16u; ++i) {
      out->drag_world.elements[i] = session_read_float(reader);
    }
  }
  return vkr_bit_reader_at_end(reader);
}

static void session_peer_set(VkrEditorSession *session,
                             const VkrEditorSessionPeer *peer) {
  for (uint32_t i = 0u; i < session->peer_count; ++i) {
    if (session->peers[i].id == peer->id) {
      session->peers[i] = *peer;
      return;
    }
  }
  if (session->peer_count < VKR_EDITOR_SESSION_PEERS_MAX) {
    session->peers[session->peer_count++] = *peer;
  }
}

static void session_peer_remove(VkrEditorSession *session, uint8_t id) {
  for (uint32_t i = 0u; i < session->peer_count; ++i) {
    if (session->peers[i].id == id) {
      log_info("%s left the session", session->peers[i].name);
      session->peers[i] = session->peers[--session->peer_count];
      return;
    }
  }
}

// =============================================================================
// Host
// =============================================================================

static SessionLink *session_link(VkrEditorSession *session,
                                 VkrNetConnectionId connection) {
  for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    if (session->links[i].live && session->links[i].connection == connection) {
      return &session->links[i];
    }
  }
  return NULL;
}

static SessionLink *session_link_by_id(VkrEditorSession *session, uint8_t id) {
  for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    if (session->links[i].live && session->links[i].welcomed &&
        session->links[i].id == id) {
      return &session->links[i];
    }
  }
  return NULL;
}

static void session_host_accept(void *context, const VkrNetAddress *address,
                                const uint8_t peer_key[32],
                                const uint8_t *credential,
                                uint32_t credential_size,
                                VkrNetAcceptResult *out_result) {
  (void)address;
  (void)peer_key;
  (void)credential;
  (void)credential_size;
  VkrEditorSession *session = context;
  uint32_t live = 0u;
  for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    live += session->links[i].live;
  }
  /* Id zero is the host's own. */
  if (live + 1u >= VKR_EDITOR_SESSION_PEERS_MAX) {
    out_result->code = VKR_NET_CLOSE_SERVER_FULL;
  }
}

static void session_host_send_small(VkrEditorSession *session,
                                    VkrNetConnectionId connection,
                                    const uint8_t *data, uint32_t size) {
  if (size) {
    (void)session_send(session, connection, SESSION_EDIT_CHANNEL, data, size);
  }
}

static void session_host_hello(VkrEditorSession *session, SessionLink *link,
                               VkrBitReader *reader) {
  char name[VKR_EDITOR_SESSION_NAME_MAX];
  if (!session_read_text(reader, name, sizeof(name))) {
    vkr_net_core_close(vkr_net_host_core(session->host), link->connection,
                       VKR_NET_CLOSE_SCHEMA_VIOLATION, session->now);
    return;
  }
  const uint8_t *digest = vkr_bit_read_bytes(reader, 32u);
  if (!digest || !vkr_bit_reader_at_end(reader) || link->welcomed) {
    vkr_net_core_close(vkr_net_host_core(session->host), link->connection,
                       VKR_NET_CLOSE_SCHEMA_VIOLATION, session->now);
    return;
  }
  uint8_t status = SESSION_WELCOME_OK;
  uint8_t id = 0u;
  if (MemCompare(digest, session->digest, 32u) != 0) {
    status = SESSION_WELCOME_SCENE;
  } else if (session->log_first > 0u) {
    status = SESSION_WELCOME_HISTORY;
  } else {
    for (uint8_t candidate = 1u;
         candidate < VKR_EDITOR_SESSION_PEERS_MAX && !id; ++candidate) {
      if (!session_link_by_id(session, candidate)) {
        id = candidate;
      }
    }
    status = id ? SESSION_WELCOME_OK : SESSION_WELCOME_FULL;
  }
  uint8_t bytes[32];
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  vkr_bit_write(&writer, SESSION_MSG_WELCOME, 4u);
  vkr_bit_write(&writer, status, 4u);
  vkr_bit_write(&writer, id, 8u);
  vkr_bit_write_varuint(&writer, session->log_count);
  session_host_send_small(session, link->connection, bytes,
                          vkr_bit_writer_finish(&writer));
  if (status != SESSION_WELCOME_OK) {
    log_warn("Refused %s: %s", name,
             status == SESSION_WELCOME_SCENE
                 ? "its scene is not the session's"
                 : (status == SESSION_WELCOME_HISTORY
                        ? "the session's history is too long to replay"
                        : "the session is full"));
    return;
  }
  link->welcomed = true_v;
  link->id = id;
  link->log_sent = 0u;
  /* The claims and tasks come before the history replays. */
  if (session->shared_size) {
    (void)session_send(session, link->connection, SESSION_EDIT_CHANNEL,
                       session->shared_bytes, session->shared_size);
  }
  VkrEditorSessionPeer peer = {.id = id};
  snprintf(peer.name, sizeof(peer.name), "%s", name);
  session_peer_set(session, &peer);
  log_info("%s joined the session", name);
}

static void session_host_presence(VkrEditorSession *session, SessionLink *link,
                                  VkrBitReader *reader) {
  VkrEditorSessionPeer peer;
  if (!link->welcomed || !session_read_presence(reader, &peer)) {
    return;
  }
  /* The connection says who speaks, not the message. */
  peer.id = link->id;
  session_peer_set(session, &peer);
  uint8_t bytes[384];
  const uint32_t size = session_encode_presence(&peer, bytes, sizeof(bytes));
  for (uint32_t i = 0u; size && i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    const SessionLink *other = &session->links[i];
    if (other->live && other->welcomed && other != link) {
      (void)session_send(session, other->connection, SESSION_PRESENCE_CHANNEL,
                         bytes, size);
    }
  }
}

static void session_review_accepted(VkrEditorSession *session, uint64_t seq,
                                    const SessionLink *from);

static void session_host_message(VkrEditorSession *session, SessionLink *link,
                                 uint8_t channel, VkrBitReader *reader) {
  const uint8_t type = (uint8_t)vkr_bit_read(reader, 4u);
  if (channel == SESSION_PRESENCE_CHANNEL) {
    if (type == SESSION_MSG_PRESENCE) {
      session_host_presence(session, link, reader);
    }
    return;
  }
  if (type == SESSION_MSG_HELLO) {
    session_host_hello(session, link, reader);
    return;
  }
  if (type == SESSION_MSG_REVIEWED) {
    const uint64_t seq = vkr_bit_read_varuint(reader);
    if (!link->welcomed || !vkr_bit_reader_at_end(reader)) {
      vkr_net_core_close(vkr_net_host_core(session->host), link->connection,
                         VKR_NET_CLOSE_SCHEMA_VIOLATION, session->now);
      return;
    }
    session_review_accepted(session, seq, link);
    return;
  }
  if (type == SESSION_MSG_ASK) {
    VkrEditorSessionAsk ask;
    if (!link->welcomed || !session_read_ask(reader, &ask)) {
      vkr_net_core_close(vkr_net_host_core(session->host), link->connection,
                         VKR_NET_CLOSE_SCHEMA_VIOLATION, session->now);
      return;
    }
    ask.peer = link->id;
    if (session->ask_count == SESSION_ASK_MAX) {
      const VkrEditorSessionAnswer busy = {
          .token = ask.token,
          .code = "VKR-AGENT-0006",
          .error = "The session host has too many agent requests waiting"};
      vkr_editor_session_reply(session, &ask, &busy);
      return;
    }
    session
        ->asks[(session->ask_head + session->ask_count++) % SESSION_ASK_MAX] =
        ask;
    return;
  }
  SessionEdit edit;
  if (type != SESSION_MSG_EDIT || !link->welcomed ||
      !session_read_edit(reader, type, &edit)) {
    vkr_net_core_close(vkr_net_host_core(session->host), link->connection,
                       VKR_NET_CLOSE_SCHEMA_VIOLATION, session->now);
    return;
  }
  edit.author = link->id;
  if (!session_queue_push(session, &edit)) {
    session_edit_release(&edit);
  }
}

static void session_host_refuse(VkrEditorSession *session,
                                const SessionEdit *edit, uint8_t refusal) {
  static const char *const reasons[] = {"", "it did not apply",
                                        "it reaches past the session's start",
                                        "the session did not make that batch"};
  if (edit->local) {
    log_warn("The session did not take your edit: %s", reasons[refusal]);
    return;
  }
  SessionLink *link = session_link_by_id(session, edit->author);
  if (!link) {
    return;
  }
  uint8_t bytes[32];
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  vkr_bit_write(&writer, SESSION_MSG_RESULT, 4u);
  vkr_bit_write(&writer, edit->token, 64u);
  vkr_bit_write(&writer, edit->kind, 4u);
  vkr_bit_write(&writer, refusal, 4u);
  session_host_send_small(session, link->connection, bytes,
                          vkr_bit_writer_finish(&writer));
}

/* Gives an applied edit its place in the session and the history every
   participant receives. */
static void session_host_commit(VkrEditorSession *session, SessionEdit *edit,
                                bool8_t new_step, uint16_t container,
                                uint64_t group) {
  edit->seq = session->apply_seq++;
  switch (edit->kind) {
  case SESSION_EDIT_SINGLE:
  case SESSION_EDIT_BATCH:
    if (new_step) {
      session->undo_depth += 1u;
      session->redo_depth = 0u;
    }
    break;
  case SESSION_EDIT_UNDO:
    session->undo_depth -= 1u;
    session->redo_depth += 1u;
    break;
  case SESSION_EDIT_REDO:
    session->undo_depth += 1u;
    session->redo_depth -= 1u;
    break;
  case SESSION_EDIT_REVERT:
    session->undo_depth = session->undo_depth ? session->undo_depth - 1u : 0u;
    session->redo_depth = 0u;
    break;
  default:
    break;
  }
  session_group_add(session, edit->seq, container, group, edit);
  const uint32_t size = session_encode_edit(session, SESSION_MSG_APPLIED, edit);
  uint8_t *bytes = size ? malloc(size) : NULL;
  if (session->log_count == session->log_capacity) {
    const uint32_t capacity =
        session->log_capacity ? session->log_capacity * 2u : 256u;
    SessionBytes *grown = realloc(session->log, capacity * sizeof(*grown));
    if (grown) {
      session->log = grown;
      session->log_capacity = capacity;
    }
  }
  if (!bytes || session->log_count == session->log_capacity) {
    /* The participants can no longer follow the session. */
    free(bytes);
    log_error("The session could not record edit %llu; it ends",
              (unsigned long long)edit->seq);
    snprintf(session->error, sizeof(session->error),
             "Out of memory for the session's history.");
    session->leave_pending = true_v;
    return;
  }
  MemCopy(bytes, session->message.data, size);
  session->log[session->log_count++] = (SessionBytes){bytes, size};
  session->log_bytes += size;
}

/* Sends each participant the history it has not received, and past the
   history budget drops what every participant has. */
static void session_host_send_log(VkrEditorSession *session) {
  uint32_t lowest = session->log_count;
  for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    SessionLink *link = &session->links[i];
    if (!link->live || !link->welcomed) {
      continue;
    }
    while (link->log_sent < session->log_count) {
      const SessionBytes *entry = &session->log[link->log_sent];
      const VkrNetSendStatus status = vkr_net_session_send(
          session->net, link->connection, VKR_NET_SERVICE_COLLAB,
          SESSION_EDIT_CHANNEL, entry->data, entry->size,
          &(VkrNetSendOptions){0}, session->now);
      if (status == VKR_NET_SEND_QUEUE_FULL) {
        break;
      }
      link->log_sent += 1u;
    }
    lowest = Min(lowest, link->log_sent);
  }
  while (session->log_bytes > SESSION_LOG_BYTES_MAX &&
         session->log_first < lowest) {
    SessionBytes *entry = &session->log[session->log_first++];
    session->log_bytes -= entry->size;
    free(entry->data);
    entry->data = NULL;
  }
}

// =============================================================================
// Reviews
// =============================================================================

static uint32_t session_put_reviewed(uint8_t *out, uint32_t capacity,
                                     uint64_t seq) {
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, out, capacity);
  vkr_bit_write(&writer, SESSION_MSG_REVIEWED, 4u);
  vkr_bit_write_varuint(&writer, seq);
  return vkr_bit_writer_finish(&writer);
}

/* The batch of session sequence `seq` was accepted on another editor: this
   editor drops it from review, and the host tells every participant but
   `from`. */
static void session_review_accepted(VkrEditorSession *session, uint64_t seq,
                                    const SessionLink *from) {
  for (uint32_t i = 0u; i < session->group_count; ++i) {
    const SessionGroup *group = &session->groups[i];
    if (group->seq == seq && session->accepted_count < SESSION_ACCEPTED_MAX) {
      session->accepted[(session->accepted_head + session->accepted_count++) %
                        SESSION_ACCEPTED_MAX] = (SessionAccepted){
          .container = group->container, .group = group->group};
    }
  }
  if (session->mode != VKR_EDITOR_SESSION_HOST) {
    return;
  }
  uint8_t bytes[16];
  const uint32_t size = session_put_reviewed(bytes, sizeof(bytes), seq);
  for (uint32_t i = 0u; size && i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    const SessionLink *link = &session->links[i];
    if (link->live && link->welcomed && link != from) {
      session_host_send_small(session, link->connection, bytes, size);
    }
  }
}

// =============================================================================
// Participant
// =============================================================================

static void session_fail(VkrEditorSession *session, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(session->error, sizeof(session->error), format, arguments);
  va_end(arguments);
  log_error("Collaborative session: %s", session->error);
  session->leave_pending = true_v;
}

static void session_rollback_speculative(VkrEditorSession *session) {
  session->rollbacks += session->speculative_count;
  session->speculative_count = 0u;
}

static void session_participant_message(VkrEditorSession *session,
                                        uint8_t channel, VkrBitReader *reader) {
  const uint8_t type = (uint8_t)vkr_bit_read(reader, 4u);
  if (channel == SESSION_PRESENCE_CHANNEL) {
    VkrEditorSessionPeer peer;
    if (type == SESSION_MSG_PRESENCE && session_read_presence(reader, &peer) &&
        peer.id != session->self_id) {
      session_peer_set(session, &peer);
    }
    return;
  }
  switch (type) {
  case SESSION_MSG_WELCOME: {
    const uint8_t status = (uint8_t)vkr_bit_read(reader, 4u);
    const uint8_t id = (uint8_t)vkr_bit_read(reader, 8u);
    const uint64_t log = vkr_bit_read_varuint(reader);
    if (!vkr_bit_reader_at_end(reader) || session->welcomed) {
      session_fail(session, "The host sent a malformed welcome.");
    } else if (status == SESSION_WELCOME_SCENE) {
      session_fail(session, "This scene is not the one the session started "
                            "from; open the host's saved scene and join "
                            "again.");
    } else if (status == SESSION_WELCOME_HISTORY) {
      session_fail(session, "The session's history is too long to replay; "
                            "the host must start a new session.");
    } else if (status != SESSION_WELCOME_OK) {
      session_fail(session, "The session is full.");
    } else {
      session->welcomed = true_v;
      session->self_id = id;
      session->welcome_log = log;
      log_info("Joined the session as participant %u; replaying %llu edits", id,
               (unsigned long long)log);
    }
    return;
  }
  case SESSION_MSG_APPLIED: {
    SessionEdit edit;
    if (!session->welcomed || !session_read_edit(reader, type, &edit)) {
      session_fail(session, "The host sent a malformed edit.");
      return;
    }
    if (edit.seq != session->receive_seq) {
      session_edit_release(&edit);
      session_fail(session, "The host's edits arrived out of order.");
      return;
    }
    session->receive_seq += 1u;
    if (!session_queue_push(session, &edit)) {
      session_edit_release(&edit);
      session_fail(session, "Out of memory for the session's edits.");
    }
    return;
  }
  case SESSION_MSG_RESULT: {
    const uint64_t token = vkr_bit_read(reader, 64u);
    const uint8_t kind = (uint8_t)vkr_bit_read(reader, 4u);
    const uint8_t refusal = (uint8_t)vkr_bit_read(reader, 4u);
    if (!vkr_bit_reader_at_end(reader)) {
      session_fail(session, "The host sent a malformed result.");
      return;
    }
    for (uint32_t i = 0u; i < session->speculative_count; ++i) {
      if (session->speculative[i].token == token) {
        session_rollback_speculative(session);
        break;
      }
    }
    log_warn("The session host refused an edit (reason %u)", refusal);
    if (kind == SESSION_EDIT_BATCH || kind == SESSION_EDIT_REVERT) {
      /* Its requester waits for an answer under its token. */
      const SessionEdit answer = {
          .kind = SESSION_EDIT_REFUSED, .token = token, .refusal = refusal};
      (void)session_queue_push(session, &answer);
    }
    return;
  }
  case SESSION_MSG_PEER_LEFT: {
    const uint8_t id = (uint8_t)vkr_bit_read(reader, 8u);
    session_peer_remove(session, id);
    return;
  }
  case SESSION_MSG_REVIEWED: {
    const uint64_t seq = vkr_bit_read_varuint(reader);
    if (!vkr_bit_reader_at_end(reader)) {
      session_fail(session, "The host sent a malformed review.");
      return;
    }
    session_review_accepted(session, seq, NULL);
    return;
  }
  case SESSION_MSG_ANSWER: {
    VkrEditorSessionAnswer answer;
    if (!session_read_answer(reader, &answer)) {
      session_fail(session, "The host sent a malformed answer.");
      return;
    }
    /* The oldest slot gives way when every one is taken: its asker timed
       out long ago. */
    SessionAnswerSlot *slot = &session->answers[0];
    for (uint32_t i = 0u; i < SESSION_ANSWER_MAX; ++i) {
      if (!session->answers[i].used) {
        slot = &session->answers[i];
        break;
      }
    }
    *slot = (SessionAnswerSlot){.used = true_v, .answer = answer};
    return;
  }
  case SESSION_MSG_SHARED: {
    const uint64_t claims = vkr_bit_read_varuint(reader);
    bool8_t ok = claims <= VKR_EDITOR_CLAIM_MAX;
    for (uint32_t i = 0u; ok && i < claims; ++i) {
      ok = session_read_claim(reader, &session->shared_claims[i]);
    }
    const uint64_t tasks = ok ? vkr_bit_read_varuint(reader) : 0u;
    ok = ok && tasks <= VKR_EDITOR_TASK_MAX;
    for (uint32_t i = 0u; ok && i < tasks; ++i) {
      ok = session_read_task(reader, &session->shared_tasks[i]);
    }
    if (!ok || !vkr_bit_reader_at_end(reader)) {
      session_fail(session, "The host sent malformed claims or tasks.");
      return;
    }
    session->shared_claim_count = (uint32_t)claims;
    session->shared_task_count = (uint32_t)tasks;
    session->shared_revision += 1u;
    return;
  }
  default:
    session_fail(session, "The host sent an unknown message.");
    return;
  }
}

// =============================================================================
// The network
// =============================================================================

static uint16_t session_service_accept(void *context,
                                       VkrNetConnectionId connection,
                                       uint16_t version) {
  (void)version;
  VkrEditorSession *session = context;
  return session->mode == VKR_EDITOR_SESSION_HOST &&
                 session_link(session, connection)
             ? 0u
             : (uint16_t)VKR_NET_CLOSE_SERVICE_REFUSED;
}

static void session_service_opened(void *context, VkrNetConnectionId connection,
                                   uint16_t version) {
  (void)version;
  VkrEditorSession *session = context;
  if (session->mode != VKR_EDITOR_SESSION_PARTICIPANT ||
      connection != session->connection) {
    return;
  }
  uint8_t bytes[128];
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  vkr_bit_write(&writer, SESSION_MSG_HELLO, 4u);
  session_write_text(&writer, session->name);
  vkr_bit_write_bytes(&writer, session->digest, 32u);
  const uint32_t size = vkr_bit_writer_finish(&writer);
  if (!size ||
      !session_send(session, connection, SESSION_EDIT_CHANNEL, bytes, size)) {
    session_fail(session, "Could not greet the session host.");
  }
}

static void session_service_event(void *context, VkrNetConnectionId connection,
                                  uint8_t channel, const VkrNetEvent *event,
                                  uint64_t now_us) {
  VkrEditorSession *session = context;
  session->now = now_us;
  if (event->type != VKR_NET_EVENT_MESSAGE || session->leave_pending) {
    return;
  }
  VkrBitReader reader;
  vkr_bit_reader_init(&reader, event->data, event->size);
  if (session->mode == VKR_EDITOR_SESSION_HOST) {
    SessionLink *link = session_link(session, connection);
    if (link) {
      session_host_message(session, link, channel, &reader);
    }
  } else if (connection == session->connection) {
    session_participant_message(session, channel, &reader);
  }
}

static void session_service_closed(void *context, VkrNetConnectionId connection,
                                   uint16_t code) {
  VkrEditorSession *session = context;
  if (session->mode == VKR_EDITOR_SESSION_PARTICIPANT &&
      connection == session->connection) {
    session_fail(session, "The host closed the session (code %u).", code);
  }
}

static bool8_t session_start_network(VkrEditorSession *session,
                                     const VkrNetHostConfig *config) {
  if (!vkr_net_crypto_init()) {
    return false_v;
  }
  session->host = vkr_net_host_create(session->allocator, config);
  session->net = session->host
                     ? vkr_net_session_create(session->allocator,
                                              vkr_net_host_core(session->host))
                     : NULL;
  if (!session->net) {
    return false_v;
  }
  VkrNetService service = {
      .id = VKR_NET_SERVICE_COLLAB,
      .versions = {{SESSION_SERVICE_VERSION, SESSION_SCHEMA_HASH}},
      .version_count = 1u,
      .channels =
          {
              {.delivery = VKR_NET_RELIABLE_ORDERED,
               .priority = 1u,
               .weight = 1u,
               .max_message_size = SESSION_MESSAGE_MAX,
               .receive_window = 2u * SESSION_MESSAGE_MAX,
               .send_queue = 4096u},
              {.delivery = VKR_NET_SEQUENCED,
               .priority = 2u,
               .weight = 1u,
               .max_message_size = 384u},
          },
      .channel_count = 2u,
      .context = session,
      .accept = session_service_accept,
      .opened = session_service_opened,
      .event = session_service_event,
      .closed = session_service_closed,
  };
  return vkr_net_session_register(session->net, &service);
}

static void session_host_closed(VkrEditorSession *session, SessionLink *link) {
  const bool8_t welcomed = link->welcomed;
  const uint8_t id = link->id;
  MemZero(link, sizeof(*link));
  if (!welcomed) {
    return;
  }
  session_peer_remove(session, id);
  uint8_t bytes[8];
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  vkr_bit_write(&writer, SESSION_MSG_PEER_LEFT, 4u);
  vkr_bit_write(&writer, id, 8u);
  const uint32_t size = vkr_bit_writer_finish(&writer);
  for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    if (session->links[i].live && session->links[i].welcomed) {
      session_host_send_small(session, session->links[i].connection, bytes,
                              size);
    }
  }
}

static void session_pump(VkrEditorSession *session) {
  if (!vkr_net_host_pump(session->host, 0)) {
    session_fail(session, "The network socket failed.");
    return;
  }
  VkrNetCore *core = vkr_net_host_core(session->host);
  session->now = vkr_net_host_now(session->host);
  VkrNetEvent event;
  while (vkr_net_core_poll(core, &event)) {
    if (vkr_net_session_dispatch(session->net, &event, session->now)) {
      continue;
    }
    if (session->mode == VKR_EDITOR_SESSION_HOST) {
      if (event.type == VKR_NET_EVENT_CONNECTED) {
        for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
          if (!session->links[i].live) {
            session->links[i] =
                (SessionLink){.live = true_v, .connection = event.connection};
            break;
          }
        }
      } else if (event.type == VKR_NET_EVENT_CLOSED) {
        SessionLink *link = session_link(session, event.connection);
        if (link) {
          session_host_closed(session, link);
        }
      }
    } else if (event.connection == session->connection) {
      if (event.type == VKR_NET_EVENT_CONNECTED) {
        if (!vkr_net_session_open(session->net, session->connection,
                                  VKR_NET_SERVICE_COLLAB, session->now)) {
          session_fail(session, "Could not open the session service.");
        }
      } else if (event.type == VKR_NET_EVENT_CLOSED) {
        session_fail(session,
                     session->welcomed
                         ? "The connection to the host closed (code %u)."
                         : "Could not join the host (code %u).",
                     event.code);
      }
    }
  }
}

// =============================================================================
// Local edits
// =============================================================================

static const char *session_action_name(VkrSceneEditAction action) {
  switch (action) {
  case VKR_SCENE_EDIT_TERRAIN:
    return "Terrain sample";
  case VKR_SCENE_EDIT_PARTITION:
    return "World partition";
  default:
    return "Scene load";
  }
}

/* Answers a batch request under its token without applying it. */
static void session_answer_batch(VkrEditorSession *session,
                                 const VkrSampleUiFrame *frame, uint64_t token,
                                 const char *message) {
  MemZero(&session->refused, sizeof(session->refused));
  session->refused.failed_index = UINT32_MAX;
  snprintf(session->refused.message, sizeof(session->refused.message), "%s",
           message);
  *frame->edit_batch = (VkrSampleEditBatchRequest){
      .token = token, .forced_result = &session->refused};
}

/* Sends or queues one edit this editor made; false with a message when it
   cannot go. */
static bool8_t session_submit(VkrEditorSession *session,
                              SessionPayload *payload, uint64_t token,
                              const SessionNote *note) {
  SessionEdit edit = {.kind = payload->kind,
                      .author = session->self_id,
                      .local = true_v,
                      .token = token};
  if (note) {
    snprintf(edit.agent, sizeof(edit.agent), "%s", note->author);
    snprintf(edit.label, sizeof(edit.label), "%s", note->label);
    edit.review = note->review;
    if (note->stage_count) {
      edit.stages = malloc(note->stage_count * sizeof(*edit.stages));
      if (!edit.stages) {
        snprintf(payload->error, sizeof(payload->error), "Out of memory.");
        return false_v;
      }
      MemCopy(edit.stages, note->stages,
              note->stage_count * sizeof(*edit.stages));
      edit.stage_count = note->stage_count;
    }
  }
  if (!session_encode_payload(session, payload, &edit.payload, &edit.size)) {
    return false_v;
  }
  if (session->mode == VKR_EDITOR_SESSION_HOST) {
    if (!session_queue_push(session, &edit)) {
      session_edit_release(&edit);
      snprintf(payload->error, sizeof(payload->error), "Out of memory.");
      return false_v;
    }
    return true_v;
  }
  const uint32_t size = session_encode_edit(session, SESSION_MSG_EDIT, &edit);
  const bool8_t sent =
      size && session_send(session, session->connection, SESSION_EDIT_CHANNEL,
                           session->message.data, size);
  session_edit_release(&edit);
  if (!sent) {
    snprintf(payload->error, sizeof(payload->error),
             "The edit could not be sent to the host.");
  }
  return sent;
}

static uint64_t session_forward_token(VkrEditorSession *session) {
  return SESSION_TOKEN_FORWARD | ++session->next_token;
}

/* A participant edits only once the history has replayed. */
static bool8_t session_ready(const VkrEditorSession *session) {
  return session->mode == VKR_EDITOR_SESSION_HOST ||
         (session->welcomed && session->apply_seq > session->welcome_log);
}

/* Takes this build's edit requests: journal edits enter the session instead
   of applying here, and requests the session cannot carry are refused. */
static void session_intercept(VkrEditorSession *session,
                              const VkrSampleUiFrame *frame) {
  VkrSampleSceneRequest *scene_request = frame->scene_request;
  if (scene_request && (scene_request->select || scene_request->unload ||
                        scene_request->add || scene_request->remove)) {
    MemZero(scene_request, sizeof(*scene_request));
    log_warn("Leave the collaborative session before opening, closing, "
             "adding or removing scenes");
  }
  VkrSampleWorldRequest *world_request = frame->world_request;
  if (world_request && (world_request->load || world_request->unload)) {
    world_request->load = false_v;
    world_request->unload = false_v;
    log_warn("Leave the collaborative session before loading the World");
  }
  const VkrNetSceneEditScenes scenes = session_scenes(frame);
  VkrSceneEditRequest *edit = frame->scene_edit;
  if (edit && edit->action != VKR_SCENE_EDIT_NONE &&
      edit->action != VKR_SCENE_EDIT_SELECT &&
      edit->action != VKR_SCENE_EDIT_FRAME &&
      edit->action != VKR_SCENE_EDIT_SAVE) {
    SessionPayload payload = {.scenes = &scenes, .request = edit};
    if (edit->action == VKR_SCENE_EDIT_UNDO) {
      payload.kind = SESSION_EDIT_UNDO;
    } else if (edit->action == VKR_SCENE_EDIT_REDO) {
      payload.kind = SESSION_EDIT_REDO;
    } else {
      payload.kind = SESSION_EDIT_SINGLE;
    }
    if (payload.kind == SESSION_EDIT_SINGLE &&
        !vkr_net_scene_edit_supported(edit)) {
      log_warn("%s edits do not reach a collaborative session yet; it was "
               "not applied",
               session_action_name(edit->action));
    } else if (!session_ready(session)) {
      log_warn("Wait until the session has joined to edit");
    } else if (!session_submit(session, &payload,
                               session->mode == VKR_EDITOR_SESSION_HOST
                                   ? 0u
                                   : session_forward_token(session),
                               NULL)) {
      log_warn("The edit did not reach the session: %s", payload.error);
    }
    edit->action = VKR_SCENE_EDIT_NONE;
  }
  VkrSampleEditBatchRequest *batch = frame->edit_batch;
  if (!batch || !batch->token || batch->forced_result) {
    return;
  }
  const uint64_t token = batch->token;
  SessionPayload payload = {.scenes = &scenes,
                            .items = batch->items,
                            .count = batch->count,
                            .container = batch->container};
  if (batch->revert_group) {
    payload.kind = SESSION_EDIT_REVERT;
    payload.revert_seq =
        session_seq_of_group(session, batch->container, batch->revert_group);
  } else {
    payload.kind = SESSION_EDIT_BATCH;
  }
  *batch = (VkrSampleEditBatchRequest){0};
  if (!session_ready(session)) {
    session_answer_batch(session, frame, token,
                         "Wait until the session has joined to edit.");
  } else if (payload.kind == SESSION_EDIT_REVERT && !payload.revert_seq) {
    session_answer_batch(session, frame, token,
                         "That change was made before the session started; "
                         "the session cannot revert it.");
  } else if (payload.kind == SESSION_EDIT_BATCH &&
             (payload.count > VKR_SAMPLE_EDIT_BATCH_MAX || !payload.items)) {
    session_answer_batch(session, frame, token, "Malformed batch.");
  } else {
    for (uint32_t i = 0u;
         payload.kind == SESSION_EDIT_BATCH && i < payload.count; ++i) {
      const VkrSceneEditAction action = payload.items[i].request.action;
      if (action == VKR_SCENE_EDIT_APPLY_COLLISION_LAYERS ||
          action == VKR_SCENE_EDIT_APPLY_PHYSICS_BATCH) {
        char message[192];
        snprintf(message, sizeof(message),
                 "Edit %u: collision layers and physics batches travel in a "
                 "collaborative session only as edits of their own.",
                 i);
        session_answer_batch(session, frame, token, message);
        return;
      }
      if (!vkr_net_scene_edit_supported(&payload.items[i].request)) {
        char message[192];
        snprintf(message, sizeof(message),
                 "Edit %u: %s edits do not reach a collaborative session "
                 "yet.",
                 i, session_action_name(payload.items[i].request.action));
        session_answer_batch(session, frame, token, message);
        return;
      }
    }
    const SessionNote *note = NULL;
    for (uint32_t i = 0u; i < SESSION_NOTE_MAX; ++i) {
      if (session->notes[i].token == token) {
        note = &session->notes[i];
      }
    }
    if (!session_submit(session, &payload, token, note)) {
      session_answer_batch(session, frame, token, payload.error);
    }
  }
}

/* A gizmo drag the runtime recorded itself: the host orders it now; a
   participant forwards it and keeps it ahead of the host until the host's
   order confirms or replaces it. */
static void session_gizmo(VkrEditorSession *session,
                          const VkrSampleUiFrame *frame) {
  const VkrSampleGizmoEdit *gizmo = frame->gizmo_edit;
  const VkrScene *scene =
      session_container(frame, gizmo->entities[0].parts.world);
  const bool8_t travels =
      scene && gizmo->count &&
      (gizmo->fields & ~(uint32_t)VKR_SCENE_EDIT_TRANSFORM) == 0u &&
      session_ready(session) &&
      (session->mode == VKR_EDITOR_SESSION_HOST ||
       session->speculative_count < SESSION_SPECULATIVE_MAX);
  if (!travels) {
    session->rollbacks += 1u;
    log_warn("That gizmo edit does not reach the collaborative session; it "
             "was undone");
    return;
  }
  const uint32_t count = gizmo->count;
  if (!session_reserve_items(session, count)) {
    session->rollbacks += 1u;
    return;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    VkrSampleEditBatchItem *item = &session->items[i];
    MemZero(item, sizeof(*item));
    item->entity_ref = -1;
    item->parent_ref = -1;
    item->request.action = VKR_SCENE_EDIT_APPLY;
    item->request.entity = gizmo->entities[i];
    if (!vkr_scene_edit_read(scene, gizmo->entities[i],
                             &item->request.values)) {
      session->rollbacks += 1u;
      return;
    }
    item->request.values.fields =
        i == 0u ? gizmo->fields : (uint32_t)VKR_SCENE_EDIT_TRANSFORM;
  }
  const VkrNetSceneEditScenes scenes = session_scenes(frame);
  const bool8_t grouped = count > 1u || gizmo->group;
  SessionPayload payload = {.kind = grouped ? SESSION_EDIT_BATCH
                                            : SESSION_EDIT_SINGLE,
                            .scenes = &scenes,
                            .request = &session->items[0].request,
                            .items = session->items,
                            .count = count,
                            .container = gizmo->entities[0].parts.world};
  if (session->mode == VKR_EDITOR_SESSION_HOST) {
    SessionEdit edit = {.kind = payload.kind, .local = true_v};
    if (!session_encode_payload(session, &payload, &edit.payload, &edit.size)) {
      session->rollbacks += 1u;
      log_warn("That gizmo edit cannot travel: %s", payload.error);
      return;
    }
    session_host_commit(session, &edit, true_v, payload.container,
                        gizmo->group);
    session_edit_release(&edit);
    return;
  }
  const uint64_t token = session_forward_token(session);
  if (!session_submit(session, &payload, token, NULL)) {
    session->rollbacks += 1u;
    log_warn("That gizmo edit cannot travel: %s", payload.error);
    return;
  }
  session->speculative[session->speculative_count++] = (SessionSpeculative){
      .token = token, .group = gizmo->group, .container = payload.container};
}

// =============================================================================
// Applying the session's edits
// =============================================================================

// -----------------------------------------------------------------------------
// Terrain results
// -----------------------------------------------------------------------------

/* Host: remembers what terrain edit `index` of the injected edit may change,
   so its result can travel. False when the terrain is not loaded here. */
static bool8_t session_terrain_note(VkrEditorSession *session,
                                    const VkrSampleUiFrame *frame,
                                    const VkrSceneEditRequest *request,
                                    uint32_t index) {
  const VkrScene *scene = session_container(frame, request->entity.parts.world);
  VkrHeightfieldRect rect;
  if (!scene || !vkr_scene_edit_terrain_rect(scene, request->entity,
                                             &request->terrain, &rect)) {
    return false_v;
  }
  if (session->inflight.terrain_count == session->terrain_capacity) {
    const uint32_t capacity =
        session->terrain_capacity ? session->terrain_capacity * 2u : 16u;
    SessionTerrain *grown =
        realloc(session->terrain, capacity * sizeof(*grown));
    if (!grown) {
      return false_v;
    }
    session->terrain = grown;
    session->terrain_capacity = capacity;
  }
  session->terrain[session->inflight.terrain_count++] =
      (SessionTerrain){.index = index, .entity = request->entity, .rect = rect};
  return true_v;
}

typedef struct SessionResultsContext {
  const VkrEditorSession *session;
  const VkrSampleUiFrame *frame;
  bool8_t ok;
} SessionResultsContext;

/* count, then per terrain edit its index, rectangle, heights and weights as
   raw arrays. */
static void session_put_results(VkrBitWriter *writer, const void *context) {
  SessionResultsContext *results = (SessionResultsContext *)context;
  const VkrEditorSession *session = results->session;
  const uint32_t count = session->inflight.terrain_count;
  vkr_bit_write_varuint(writer, count);
  results->ok = true_v;
  for (uint32_t i = 0u; i < count; ++i) {
    const SessionTerrain *terrain = &session->terrain[i];
    const VkrScene *scene =
        session_container(results->frame, terrain->entity.parts.world);
    const VkrHeightfield *field =
        scene ? vkr_scene_terrain_field(scene, terrain->entity) : NULL;
    const uint32_t samples = vkr_heightfield_rect_count(terrain->rect);
    uint16_t *heights = field ? malloc(samples * sizeof(uint16_t)) : NULL;
    uint32_t *weights = heights ? malloc(samples * sizeof(uint32_t)) : NULL;
    if (!weights) {
      free(heights);
      results->ok = false_v;
      return;
    }
    vkr_heightfield_read_rect(field, terrain->rect, heights, weights);
    vkr_bit_write_varuint(writer, terrain->index);
    vkr_bit_write_varuint(writer, terrain->rect.x0);
    vkr_bit_write_varuint(writer, terrain->rect.z0);
    vkr_bit_write_varuint(writer, terrain->rect.x1);
    vkr_bit_write_varuint(writer, terrain->rect.z1);
    vkr_net_write_array(writer, heights, samples * (uint32_t)sizeof(uint16_t));
    vkr_net_write_array(writer, weights, samples * (uint32_t)sizeof(uint32_t));
    free(heights);
    free(weights);
  }
}

/* Host: the samples the injected edit's terrain edits changed, into an owned
   copy on `edit`. */
static bool8_t session_terrain_results(VkrEditorSession *session,
                                       const VkrSampleUiFrame *frame,
                                       SessionEdit *edit) {
  if (!session->inflight.terrain_count) {
    return true_v;
  }
  SessionResultsContext context = {.session = session, .frame = frame};
  uint32_t size = 0u;
  for (;;) {
    VkrBitWriter writer;
    vkr_bit_writer_init(&writer, session->results.data,
                        session->results.capacity);
    session_put_results(&writer, &context);
    size = session->results.capacity && context.ok
               ? vkr_bit_writer_finish(&writer)
               : 0u;
    if (size || !context.ok || !session_buffer_grow(&session->results)) {
      break;
    }
  }
  edit->results = size ? malloc(size) : NULL;
  if (!edit->results) {
    return false_v;
  }
  MemCopy(edit->results, session->results.data, size);
  edit->results_size = size;
  return true_v;
}

/* Participant: puts the host's results in place of the terrain ops of the
   decoded edit: each becomes the samples it made on the host. */
static bool8_t session_terrain_apply_results(VkrEditorSession *session,
                                             const VkrSampleUiFrame *frame,
                                             const SessionEdit *edit,
                                             uint32_t item_count) {
  VkrBitReader reader;
  vkr_bit_reader_init(&reader, edit->results, edit->results_size);
  const uint64_t count = edit->results ? vkr_bit_read_varuint(&reader) : 0u;
  for (uint64_t i = 0u; i < count; ++i) {
    const uint64_t index = vkr_bit_read_varuint(&reader);
    VkrHeightfieldRect rect;
    rect.x0 = (uint32_t)vkr_bit_read_varuint(&reader);
    rect.z0 = (uint32_t)vkr_bit_read_varuint(&reader);
    rect.x1 = (uint32_t)vkr_bit_read_varuint(&reader);
    rect.z1 = (uint32_t)vkr_bit_read_varuint(&reader);
    if (rect.x0 > rect.x1 || rect.z0 > rect.z1 || rect.x1 - rect.x0 > 65535u ||
        rect.z1 - rect.z0 > 65535u) {
      return false_v;
    }
    const uint32_t samples = vkr_heightfield_rect_count(rect);
    const uint16_t *heights =
        vkr_net_read_array(&reader, samples * (uint32_t)sizeof(uint16_t));
    const uint32_t *weights =
        vkr_net_read_array(&reader, samples * (uint32_t)sizeof(uint32_t));
    VkrSceneEditRequest *request =
        edit->kind == SESSION_EDIT_SINGLE
            ? (index == 0u ? frame->scene_edit : NULL)
            : (index < item_count ? &session->items[index].request : NULL);
    if (!heights || !weights || !request ||
        request->action != VKR_SCENE_EDIT_TERRAIN) {
      return false_v;
    }
    request->terrain = (VkrHeightfieldOp){.kind = VKR_HEIGHTFIELD_OP_SAMPLES,
                                          .rect = rect,
                                          .heights = heights,
                                          .weights = weights};
  }
  if (!vkr_bit_reader_at_end(&reader) && edit->results) {
    return false_v;
  }
  /* Every terrain op of the edit has its result. */
  for (uint32_t i = 0u; i < Max(item_count, 1u); ++i) {
    const VkrSceneEditRequest *request = edit->kind == SESSION_EDIT_SINGLE
                                             ? frame->scene_edit
                                             : &session->items[i].request;
    if (request->action == VKR_SCENE_EDIT_TERRAIN &&
        request->terrain.kind != VKR_HEIGHTFIELD_OP_SAMPLES) {
      return false_v;
    }
    if (edit->kind == SESSION_EDIT_SINGLE) {
      break;
    }
  }
  return true_v;
}

/* Stages the new terrains a batch creates, unless their files exist, so
   its terrain components load here as on the editor that made them. */
static bool8_t session_stage_terrains(const VkrSampleUiFrame *frame,
                                      const SessionEdit *edit,
                                      uint16_t container, char *error,
                                      uint32_t capacity) {
  const VkrScene *scene = session_container(frame, container);
  for (uint32_t i = 0u; i < edit->stage_count; ++i) {
    const SessionStage *stage = &edit->stages[i];
    char absolute[1100];
    if (!scene || !vkr_scene_terrain_resolve(scene, stage->path, absolute,
                                             sizeof(absolute))) {
      snprintf(error, capacity, "A new terrain has no place here.");
      return false_v;
    }
    const FilePath path = {.path = string8_create_from_cstr(
                               (const uint8_t *)absolute, strlen(absolute)),
                           .type = FILE_PATH_TYPE_ABSOLUTE};
    if (!file_exists(&path) &&
        !vkr_scene_terrain_stage(absolute, stage->cells, stage->spacing,
                                 stage->height_min, stage->height_max,
                                 stage->height)) {
      snprintf(error, capacity, "A new terrain could not be staged.");
      return false_v;
    }
  }
  return true_v;
}

/* The terrain edits of a decoded edit: the host notes what each may change;
   a participant takes the host's results in place of the ops. */
static bool8_t session_terrain_place(VkrEditorSession *session,
                                     const VkrSampleUiFrame *frame,
                                     const SessionEdit *edit,
                                     uint32_t item_count, char *error,
                                     uint32_t capacity) {
  session->inflight.terrain_count = 0u;
  if (session->mode != VKR_EDITOR_SESSION_HOST) {
    if (!session_terrain_apply_results(session, frame, edit, item_count)) {
      snprintf(error, capacity, "Terrain results do not match the edit.");
      return false_v;
    }
    return true_v;
  }
  for (uint32_t i = 0u; i < Max(item_count, 1u); ++i) {
    const VkrSceneEditRequest *request = edit->kind == SESSION_EDIT_SINGLE
                                             ? frame->scene_edit
                                             : &session->items[i].request;
    if (request->action == VKR_SCENE_EDIT_TERRAIN &&
        (!request->entity.u64 ||
         !session_terrain_note(session, frame, request, i))) {
      snprintf(error, capacity,
               "Terrain edit %u misses a terrain loaded on the host.", i);
      return false_v;
    }
    if (edit->kind == SESSION_EDIT_SINGLE) {
      break;
    }
  }
  return true_v;
}

/* Decodes `edit` into this build's request slots. Another author's single
   creation or duplicate applies as a batch of one, which does not select
   what it makes, so it leaves this editor's selection alone. */
static bool8_t session_place(VkrEditorSession *session,
                             const VkrSampleUiFrame *frame,
                             const SessionEdit *edit, bool8_t own,
                             uint64_t slot_token, uint16_t *out_container,
                             bool8_t *out_as_batch, char *error,
                             uint32_t capacity) {
  const VkrNetSceneEditScenes scenes = session_scenes(frame);
  VkrBitReader reader;
  vkr_bit_reader_init(&reader, edit->payload, edit->size);
  *out_container = 0u;
  *out_as_batch = false_v;
  switch (edit->kind) {
  case SESSION_EDIT_SINGLE: {
    int32_t entity_ref = -1;
    int32_t parent_ref = -1;
    VkrSceneEditRequest *request = frame->scene_edit;
    if (!session->edit_physics) {
      session->edit_physics =
          calloc(SESSION_PHYSICS_CHANGE_MAX, sizeof(*session->edit_physics));
    }
    const VkrNetSceneEditStorage storage = {
        .collision_layers = &session->edit_layers,
        .physics_changes = session->edit_physics,
        .physics_capacity =
            session->edit_physics ? SESSION_PHYSICS_CHANGE_MAX : 0u};
    if (!vkr_net_scene_edit_read(&reader, &scenes, &storage, request,
                                 &entity_ref, &parent_ref, error, capacity) ||
        !vkr_bit_reader_at_end(&reader)) {
      MemZero(request, sizeof(*request));
      return false_v;
    }
    request->gesture = session_gesture(edit->author, request->gesture);
    if (request->action == VKR_SCENE_EDIT_TERRAIN &&
        !session_terrain_place(session, frame, edit, 0u, error, capacity)) {
      MemZero(request, sizeof(*request));
      return false_v;
    }
    if (own || (request->action != VKR_SCENE_EDIT_CREATE &&
                request->action != VKR_SCENE_EDIT_DUPLICATE)) {
      return true_v;
    }
    if (!session_reserve_items(session, 1u)) {
      MemZero(request, sizeof(*request));
      snprintf(error, capacity, "Out of memory.");
      return false_v;
    }
    session->items[0] = (VkrSampleEditBatchItem){
        .request = *request, .entity_ref = -1, .parent_ref = -1};
    *out_container = request->action == VKR_SCENE_EDIT_DUPLICATE
                         ? request->entity.parts.world
                     : request->parent.u64 ? request->parent.parts.world
                                           : request->container;
    *out_as_batch = true_v;
    *frame->edit_batch =
        (VkrSampleEditBatchRequest){.token = slot_token,
                                    .items = session->items,
                                    .count = 1u,
                                    .container = *out_container};
    MemZero(request, sizeof(*request));
    return true_v;
  }
  case SESSION_EDIT_BATCH: {
    const uint16_t container = (uint16_t)vkr_bit_read(&reader, 16u);
    const uint64_t count = vkr_bit_read_varuint(&reader);
    if (!session_stage_terrains(frame, edit, container, error, capacity)) {
      return false_v;
    }
    if (count == 0u || count > VKR_SAMPLE_EDIT_BATCH_MAX) {
      snprintf(error, capacity, "Malformed batch.");
      return false_v;
    }
    if (!session_reserve_items(session, (uint32_t)count)) {
      snprintf(error, capacity, "Out of memory.");
      return false_v;
    }
    for (uint32_t i = 0u; i < count; ++i) {
      VkrSampleEditBatchItem *item = &session->items[i];
      if (!vkr_net_scene_edit_read(&reader, &scenes, NULL, &item->request,
                                   &item->entity_ref, &item->parent_ref, error,
                                   capacity)) {
        return false_v;
      }
      item->request.gesture = 0u;
    }
    if (!vkr_bit_reader_at_end(&reader)) {
      snprintf(error, capacity, "Malformed batch.");
      return false_v;
    }
    if (!session_terrain_place(session, frame, edit, (uint32_t)count, error,
                               capacity)) {
      return false_v;
    }
    *out_container = container;
    session->inflight.item_count = (uint32_t)count;
    *frame->edit_batch = (VkrSampleEditBatchRequest){.token = slot_token,
                                                     .items = session->items,
                                                     .count = (uint32_t)count,
                                                     .container = container};
    return true_v;
  }
  case SESSION_EDIT_UNDO:
  case SESSION_EDIT_REDO:
    MemZero(frame->scene_edit, sizeof(*frame->scene_edit));
    frame->scene_edit->action = edit->kind == SESSION_EDIT_UNDO
                                    ? VKR_SCENE_EDIT_UNDO
                                    : VKR_SCENE_EDIT_REDO;
    return true_v;
  case SESSION_EDIT_REVERT: {
    const uint16_t container = (uint16_t)vkr_bit_read(&reader, 16u);
    const uint64_t seq = vkr_bit_read_varuint(&reader);
    const uint64_t group = session_group_of_seq(session, container, seq);
    if (!vkr_bit_reader_at_end(&reader) || !group) {
      snprintf(error, capacity, "No batch %llu to revert.",
               (unsigned long long)seq);
      return false_v;
    }
    *out_container = container;
    *frame->edit_batch = (VkrSampleEditBatchRequest){
        .token = slot_token, .container = container, .revert_group = group};
    return true_v;
  }
  default:
    snprintf(error, capacity, "Unknown edit.");
    return false_v;
  }
}

/* Puts the next queued edit, or a rollback, into the free request slots. */
static void session_inject(VkrEditorSession *session,
                           const VkrSampleUiFrame *frame) {
  if (session->inflight.active || session->leave_pending ||
      !frame->scene_edit || !frame->edit_batch ||
      frame->scene_edit->action != VKR_SCENE_EDIT_NONE ||
      frame->edit_batch->token || frame->gizmo_edit_pending ||
      frame->simulation_running || frame->scene_loading ||
      frame->additive_loading || frame->world_loading) {
    return;
  }
  SessionInflight *inflight = &session->inflight;
  const SessionSignature before = session_signature(frame);
  const bool8_t host = session->mode == VKR_EDITOR_SESSION_HOST;
  while (!session->rollbacks && session->queue_count) {
    SessionEdit *head = &session->queue[session->queue_head];
    if (!host && head->kind != SESSION_EDIT_REFUSED &&
        session->speculative_count) {
      if (head->author == session->self_id &&
          head->token == session->speculative[0].token) {
        /* The host ordered the oldest drag this editor applied ahead, with
           nothing between: it stays as applied. */
        session_group_add(session, head->seq, session->speculative[0].container,
                          session->speculative[0].group, head);
        session->speculative_count -= 1u;
        memmove(session->speculative, session->speculative + 1,
                session->speculative_count * sizeof(session->speculative[0]));
        session->apply_seq += 1u;
        SessionEdit adopted = session_queue_pop(session);
        session_edit_release(&adopted);
        continue;
      }
      session_rollback_speculative(session);
      break;
    }
    SessionEdit edit = session_queue_pop(session);
    if (edit.kind == SESSION_EDIT_REFUSED) {
      char message[192];
      snprintf(message, sizeof(message),
               "The session host refused this edit (reason %u).", edit.refusal);
      session_answer_batch(session, frame, edit.token, message);
      session_edit_release(&edit);
      return;
    }
    if (host && ((edit.kind == SESSION_EDIT_UNDO && !session->undo_depth) ||
                 (edit.kind == SESSION_EDIT_REDO && !session->redo_depth))) {
      session_host_refuse(session, &edit, SESSION_REFUSED_HISTORY);
      session_edit_release(&edit);
      continue;
    }
    const bool8_t own = host ? edit.local : edit.author == session->self_id;
    const uint64_t slot_token =
        own && edit.token &&
                (edit.kind == SESSION_EDIT_BATCH ||
                 edit.kind == SESSION_EDIT_REVERT)
            ? edit.token
            : SESSION_TOKEN_APPLY | ++session->next_token;
    char error[192] = {0};
    uint16_t container = 0u;
    bool8_t as_batch = false_v;
    if (!session_place(session, frame, &edit, own, slot_token, &container,
                       &as_batch, error, sizeof(error))) {
      if (!host) {
        session_fail(session, "Session edit %llu does not apply here: %s",
                     (unsigned long long)edit.seq, error);
        session_edit_release(&edit);
        return;
      }
      session_host_refuse(session, &edit,
                          edit.kind == SESSION_EDIT_REVERT
                              ? SESSION_REFUSED_GROUP
                              : SESSION_REFUSED_FAILED);
      if (edit.local && (edit.kind == SESSION_EDIT_BATCH ||
                         edit.kind == SESSION_EDIT_REVERT)) {
        session_answer_batch(session, frame, edit.token, error);
        session_edit_release(&edit);
        return;
      }
      if (edit.local) {
        log_warn("Your edit did not apply: %s", error);
      }
      session_edit_release(&edit);
      continue;
    }
    *inflight = (SessionInflight){.active = true_v,
                                  .as_batch = as_batch,
                                  .edit = edit,
                                  .slot_token = slot_token,
                                  .container = container,
                                  .item_count = inflight->item_count,
                                  .terrain_count = inflight->terrain_count,
                                  .before = before};
    return;
  }
  if (session->rollbacks) {
    session->rollbacks -= 1u;
    MemZero(frame->scene_edit, sizeof(*frame->scene_edit));
    frame->scene_edit->action = VKR_SCENE_EDIT_UNDO;
    *inflight = (SessionInflight){.active = true_v,
                                  .rollback = true_v,
                                  .edit = {.kind = SESSION_EDIT_UNDO},
                                  .before = before};
  }
}

/* Queues another editor's agent batch that applied here for the change
   feed: the entities it edited or created. */
static void session_note_applied(VkrEditorSession *session,
                                 const SessionEdit *edit, uint16_t container,
                                 uint32_t item_count,
                                 const VkrSampleEditBatchResult *result) {
  VkrEditorSessionApplied *notice =
      &session->applied[(session->applied_head + session->applied_count) %
                        SESSION_APPLIED_MAX];
  if (session->applied_count == SESSION_APPLIED_MAX) {
    session->applied_head = (session->applied_head + 1u) % SESSION_APPLIED_MAX;
  } else {
    session->applied_count += 1u;
  }
  *notice = (VkrEditorSessionApplied){
      .container = container, .review = edit->review, .group = result->group};
  snprintf(notice->author, sizeof(notice->author), "%s", edit->agent);
  snprintf(notice->label, sizeof(notice->label), "%s", edit->label);
  for (uint32_t i = 0u;
       i < item_count &&
       notice->entity_count < VKR_EDITOR_SESSION_APPLIED_ENTITIES;
       ++i) {
    const VkrSceneEditRequest *request = &session->items[i].request;
    VkrEntityId entity = result->created[i];
    if (!entity.u64 && request->action != VKR_SCENE_EDIT_DELETE) {
      entity = request->entity;
    }
    bool8_t seen = !entity.u64;
    for (uint32_t k = 0u; !seen && k < notice->entity_count; ++k) {
      seen = notice->entities[k].u64 == entity.u64;
    }
    if (!seen) {
      notice->entities[notice->entity_count++] = entity;
    }
  }
}

/* A reverted agent batch of another editor leaves the feed's story with a
   `reverted` event; its own editor told its agent already. */
static void session_note_reverted(VkrEditorSession *session,
                                  const SessionEdit *edit, uint16_t container) {
  VkrBitReader reader;
  vkr_bit_reader_init(&reader, edit->payload, edit->size);
  (void)vkr_bit_read(&reader, 16u);
  const uint64_t seq = vkr_bit_read_varuint(&reader);
  const SessionGroup *reverted = NULL;
  for (uint32_t i = 0u; i < session->group_count; ++i) {
    if (session->groups[i].seq == seq &&
        session->groups[i].container == container) {
      reverted = &session->groups[i];
    }
  }
  const bool8_t others = session->mode == VKR_EDITOR_SESSION_HOST
                             ? !edit->local
                             : edit->author != session->self_id;
  if (!reverted || !reverted->agent[0] || !others) {
    return;
  }
  VkrEditorSessionApplied *notice =
      &session->applied[(session->applied_head + session->applied_count) %
                        SESSION_APPLIED_MAX];
  if (session->applied_count == SESSION_APPLIED_MAX) {
    session->applied_head = (session->applied_head + 1u) % SESSION_APPLIED_MAX;
  } else {
    session->applied_count += 1u;
  }
  *notice = (VkrEditorSessionApplied){
      .container = container, .reverted = true_v, .group = reverted->group};
  snprintf(notice->author, sizeof(notice->author), "%s", reverted->agent);
  snprintf(notice->label, sizeof(notice->label), "%s", reverted->label);
}

/* Checks the edit injected in the last build against the journals, then
   orders it (host) or counts it (participant). */
static void session_verify(VkrEditorSession *session,
                           const VkrSampleUiFrame *frame) {
  SessionInflight *inflight = &session->inflight;
  if (!inflight->active) {
    return;
  }
  inflight->active = false_v;
  const SessionSignature after = session_signature(frame);
  const VkrSampleEditBatchResult *result = frame->edit_batch_result;
  bool8_t ok = false_v;
  uint64_t group = 0u;
  switch (inflight->edit.kind) {
  case SESSION_EDIT_SINGLE:
    ok = inflight->as_batch
             ? result && result->token == inflight->slot_token && result->ok
             : after.revision != inflight->before.revision;
    break;
  case SESSION_EDIT_UNDO:
    ok = after.cursor < inflight->before.cursor;
    break;
  case SESSION_EDIT_REDO:
    ok = after.cursor > inflight->before.cursor;
    break;
  case SESSION_EDIT_BATCH:
  case SESSION_EDIT_REVERT:
    ok = result && result->token == inflight->slot_token && result->ok;
    group =
        ok && inflight->edit.kind == SESSION_EDIT_BATCH ? result->group : 0u;
    break;
  default:
    break;
  }
  SessionEdit edit = inflight->edit;
  MemZero(&inflight->edit, sizeof(inflight->edit));
  const bool8_t others = session->mode == VKR_EDITOR_SESSION_HOST
                             ? !edit.local
                             : edit.author != session->self_id;
  if (ok && others && edit.agent[0] && edit.kind == SESSION_EDIT_BATCH) {
    session_note_applied(session, &edit, inflight->container,
                         inflight->item_count, result);
  }
  if (ok && edit.kind == SESSION_EDIT_REVERT) {
    session_note_reverted(session, &edit, inflight->container);
  }
  if (inflight->rollback) {
    if (!ok) {
      session_fail(session, "A local edit could not be undone to follow the "
                            "session.");
    }
    return;
  }
  if (session->mode == VKR_EDITOR_SESSION_HOST) {
    /* The edit applied here; participants cannot follow without its
       terrain samples. */
    if (ok && !session_terrain_results(session, frame, &edit)) {
      session_edit_release(&edit);
      session_fail(session, "The terrain samples of an edit could not be "
                            "read; the session ends.");
      return;
    }
    if (ok) {
      session_host_commit(session, &edit,
                          edit.kind == SESSION_EDIT_BATCH ||
                              after.newest != inflight->before.newest,
                          inflight->container, group);
    } else {
      session_host_refuse(session, &edit, SESSION_REFUSED_FAILED);
    }
  } else if (ok) {
    session_group_add(session, edit.seq, inflight->container, group, &edit);
    session->apply_seq += 1u;
  } else {
    session_fail(session, "Session edit %llu did not apply here; join again",
                 (unsigned long long)edit.seq);
  }
  session_edit_release(&edit);
}

static void session_presence(VkrEditorSession *session,
                             const VkrSampleUiFrame *frame) {
  if (session->now < session->presence_at ||
      (session->mode == VKR_EDITOR_SESSION_PARTICIPANT && !session->welcomed)) {
    return;
  }
  session->presence_at =
      session->now + (frame->gizmo_edit_pending ? SESSION_PRESENCE_DRAG_US
                                                : SESSION_PRESENCE_US);
  VkrEditorSessionPeer self = {.id = session->self_id};
  snprintf(self.name, sizeof(self.name), "%s", session->name);
  snprintf(self.tool, sizeof(self.tool), "%s", session->tool);
  self.has_camera = frame->scene_recall.camera_valid;
  self.camera_position = frame->scene_recall.position;
  self.camera_yaw = frame->scene_recall.yaw;
  self.camera_pitch = frame->scene_recall.pitch;
  const VkrEntityId selected = frame->selected_entity;
  const VkrScene *scene =
      selected.u64 ? session_container(frame, selected.parts.world) : NULL;
  if (scene && vkr_scene_entity_ref(scene, selected, &self.selection) &&
      !vkr_entity_ref_empty(&self.selection)) {
    self.has_selection = true_v;
    self.selection_container = selected.parts.world;
  }
  /* The gizmo moves the selection live during a drag. */
  const SceneTransform *transform =
      self.has_selection && frame->gizmo_edit_pending
          ? vkr_entity_get_component(scene->world, selected,
                                     scene->comp_transform)
          : NULL;
  if (transform) {
    self.dragging = true_v;
    self.drag_container = self.selection_container;
    self.drag = self.selection;
    self.drag_world = transform->world;
  }
  uint8_t bytes[384];
  const uint32_t size = session_encode_presence(&self, bytes, sizeof(bytes));
  if (!size) {
    return;
  }
  if (session->mode == VKR_EDITOR_SESSION_PARTICIPANT) {
    (void)session_send(session, session->connection, SESSION_PRESENCE_CHANNEL,
                       bytes, size);
    return;
  }
  for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    if (session->links[i].live && session->links[i].welcomed) {
      (void)session_send(session, session->links[i].connection,
                         SESSION_PRESENCE_CHANNEL, bytes, size);
    }
  }
}

// =============================================================================
// Lifetime
// =============================================================================

VkrEditorSession *vkr_editor_session_create(VkrAllocator *allocator) {
  VkrEditorSession *session = calloc(1u, sizeof(VkrEditorSession));
  if (session) {
    session->allocator = allocator;
    snprintf(session->form_name, sizeof(session->form_name), "editor");
    snprintf(session->form_bind, sizeof(session->form_bind), "[::]:%u",
             VKR_EDITOR_SESSION_PORT);
  }
  return session;
}

static void session_reset(VkrEditorSession *session) {
  if (session->host && session->mode == VKR_EDITOR_SESSION_PARTICIPANT &&
      session->connection != VKR_NET_CONNECTION_NONE) {
    vkr_net_core_close(vkr_net_host_core(session->host), session->connection,
                       VKR_NET_CLOSE_APPLICATION, session->now);
  }
  for (uint32_t i = 0u; session->host && i < VKR_EDITOR_SESSION_PEERS_MAX;
       ++i) {
    if (session->links[i].live) {
      vkr_net_core_close(vkr_net_host_core(session->host),
                         session->links[i].connection,
                         VKR_NET_CLOSE_APPLICATION, session->now);
    }
  }
  if (session->host) {
    /* Lets the close reach the peers. */
    vkr_net_core_flush(vkr_net_host_core(session->host));
  }
  vkr_net_session_destroy(session->net);
  vkr_net_host_destroy(session->host);
  for (uint32_t i = session->log_first; i < session->log_count; ++i) {
    free(session->log[i].data);
  }
  free(session->log);
  for (uint32_t i = 0u; i < session->queue_count; ++i) {
    session_edit_release(&session->queue[session->queue_head + i]);
  }
  free(session->queue);
  free(session->groups);
  free(session->items);
  free(session->terrain);
  free(session->results.data);
  free(session->edit_physics);
  free(session->payload.data);
  free(session->message.data);
  session_edit_release(&session->inflight.edit);
  free(session->shared_bytes);
  VkrAllocator *allocator = session->allocator;
  const uint64_t epoch = session->epoch;
  char error[sizeof(session->error)];
  MemCopy(error, session->error, sizeof(error));
  char form[4][128];
  snprintf(form[0], sizeof(form[0]), "%s", session->form_name);
  snprintf(form[1], sizeof(form[1]), "%s", session->form_bind);
  snprintf(form[2], sizeof(form[2]), "%s", session->form_address);
  snprintf(form[3], sizeof(form[3]), "%s", session->form_key);
  MemZero(session, sizeof(*session));
  session->allocator = allocator;
  session->epoch = epoch;
  MemCopy(session->error, error, sizeof(error));
  snprintf(session->form_name, sizeof(session->form_name), "%s", form[0]);
  snprintf(session->form_bind, sizeof(session->form_bind), "%s", form[1]);
  snprintf(session->form_address, sizeof(session->form_address), "%s", form[2]);
  snprintf(session->form_key, sizeof(session->form_key), "%s", form[3]);
}

void vkr_editor_session_destroy(VkrEditorSession *session) {
  if (!session) {
    return;
  }
  session_reset(session);
  free(session);
}

void vkr_editor_session_leave(VkrEditorSession *session) {
  if (session && session->mode != VKR_EDITOR_SESSION_NONE) {
    log_info("Left the collaborative session");
    session_reset(session);
  }
}

static bool8_t session_begin(VkrEditorSession *session,
                             const VkrSampleUiFrame *frame, const char *name,
                             const char *fallback, char *error,
                             uint32_t capacity) {
  if (session->mode != VKR_EDITOR_SESSION_NONE) {
    snprintf(error, capacity, "Leave the current session first.");
    return false_v;
  }
  if (!frame->scene || frame->scene_loading || frame->additive_loading ||
      frame->world_loading) {
    snprintf(error, capacity, "Open a scene and let it load first.");
    return false_v;
  }
  session->error[0] = '\0';
  snprintf(session->name, sizeof(session->name), "%s",
           name && name[0] ? name : fallback);
  session->scene_generation = frame->scene_generation;
  session->epoch += 1u;
  session->apply_seq = 1u;
  session->receive_seq = 1u;
  vkr_editor_session_digest(frame, session->digest);
  return true_v;
}

bool8_t vkr_editor_session_host(VkrEditorSession *session,
                                const VkrSampleUiFrame *frame, const char *bind,
                                const char *name, char *error,
                                uint32_t capacity) {
  if (!session_begin(session, frame, name, "host", error, capacity)) {
    return false_v;
  }
  VkrNetHostConfig config = {
      .socket = {.receive_buffer = 4u << 20, .send_buffer = 4u << 20}};
  if (!session_address(bind && bind[0] ? bind : "[::]", &config.bind)) {
    snprintf(error, capacity, "Invalid address %s.", bind);
    return false_v;
  }
  if (!vkr_net_crypto_init()) {
    snprintf(error, capacity, "The network cryptography did not start.");
    return false_v;
  }
  vkr_net_keypair_generate(&config.core.static_keys);
  config.core.accept_incoming = true_v;
  config.core.accept = session_host_accept;
  config.core.accept_context = session;
  config.core.max_connections = VKR_EDITOR_SESSION_PEERS_MAX;
  session->mode = VKR_EDITOR_SESSION_HOST;
  if (!session_start_network(session, &config)) {
    session_reset(session);
    snprintf(error, capacity, "Could not listen on %s.",
             bind && bind[0] ? bind : "every interface");
    return false_v;
  }
  VkrNetAddress local;
  (void)vkr_net_host_local_address(session->host, &local);
  vkr_net_address_format(&local, session->address, sizeof(session->address));
  session_hex(config.core.static_keys.public_key, 32u, session->key);
  vkr_net_random(config.core.static_keys.secret_key,
                 sizeof(config.core.static_keys.secret_key));
  log_info("Hosting a collaborative session on %s, key %s", session->address,
           session->key);
  return true_v;
}

bool8_t vkr_editor_session_join(VkrEditorSession *session,
                                const VkrSampleUiFrame *frame,
                                const char *address, const char *key_hex,
                                const char *name, char *error,
                                uint32_t capacity) {
  uint8_t server_key[32];
  VkrNetAddress server;
  if (!address || !session_address(address, &server)) {
    snprintf(error, capacity, "Invalid address.");
    return false_v;
  }
  if (!session_parse_hex(key_hex, server_key, sizeof(server_key))) {
    snprintf(error, capacity, "The key must be 64 hexadecimal digits.");
    return false_v;
  }
  if (!session_begin(session, frame, name, "guest", error, capacity)) {
    return false_v;
  }
  VkrNetHostConfig config = {
      .socket = {.receive_buffer = 4u << 20, .send_buffer = 4u << 20}};
  config.bind = vkr_net_address_any((VkrNetAddressFamily)server.family, 0u);
  if (!vkr_net_crypto_init()) {
    snprintf(error, capacity, "The network cryptography did not start.");
    return false_v;
  }
  vkr_net_keypair_generate(&config.core.static_keys);
  session->mode = VKR_EDITOR_SESSION_PARTICIPANT;
  if (!session_start_network(session, &config) ||
      !vkr_net_core_connect(
          vkr_net_host_core(session->host), &server, server_key, NULL, 0u,
          vkr_net_host_now(session->host), &session->connection)) {
    session_reset(session);
    snprintf(error, capacity, "Could not start the connection.");
    return false_v;
  }
  vkr_net_random(config.core.static_keys.secret_key,
                 sizeof(config.core.static_keys.secret_key));
  snprintf(session->address, sizeof(session->address), "%s", address);
  session_hex(server_key, 32u, session->key);
  log_info("Joining the collaborative session at %s", address);
  return true_v;
}

void vkr_editor_session_update(VkrEditorSession *session,
                               const VkrSampleUiFrame *frame) {
  if (!session || session->mode == VKR_EDITOR_SESSION_NONE) {
    return;
  }
  if (frame->scene_generation != session->scene_generation) {
    snprintf(session->error, sizeof(session->error),
             "The scene was replaced; the session ended.");
    log_warn("The scene was replaced; the collaborative session ended");
    session_reset(session);
    return;
  }
  session_pump(session);
  if (!session->leave_pending) {
    session_verify(session, frame);
  }
  if (!session->leave_pending && frame->gizmo_edit) {
    session_gizmo(session, frame);
  }
  if (!session->leave_pending) {
    session_intercept(session, frame);
    session_inject(session, frame);
    session_presence(session, frame);
  }
  if (session->mode == VKR_EDITOR_SESSION_HOST && !session->leave_pending) {
    session_host_send_log(session);
  }
  if (session->leave_pending) {
    session_reset(session);
    return;
  }
  vkr_net_core_flush(vkr_net_host_core(session->host));
}

void vkr_editor_session_status(const VkrEditorSession *session,
                               VkrEditorSessionStatus *out_status) {
  MemZero(out_status, sizeof(*out_status));
  if (!session) {
    return;
  }
  out_status->mode = session->mode;
  out_status->joined =
      session->mode == VKR_EDITOR_SESSION_PARTICIPANT && session->welcomed;
  out_status->self_id = session->self_id;
  out_status->sequence = session->apply_seq;
  out_status->pending =
      session->queue_count + (session->inflight.active ? 1u : 0u);
  snprintf(out_status->address, sizeof(out_status->address), "%s",
           session->address);
  snprintf(out_status->key, sizeof(out_status->key), "%s", session->key);
  snprintf(out_status->error, sizeof(out_status->error), "%s", session->error);
  out_status->peer_count = session->peer_count;
  MemCopy(out_status->peers, session->peers,
          session->peer_count * sizeof(session->peers[0]));
}

// =============================================================================
// Agent federation
// =============================================================================

void vkr_editor_session_author(const VkrEditorSession *session,
                               const char *local, char *out,
                               uint32_t capacity) {
  char copy[VKR_EDITOR_AUTHOR_CAPACITY];
  snprintf(copy, sizeof(copy), "%s", local);
  if (session && session->mode != VKR_EDITOR_SESSION_NONE && copy[0]) {
    snprintf(out, capacity, "%s@%s", copy, session->name);
  } else {
    snprintf(out, capacity, "%s", copy);
  }
}

void vkr_editor_session_note_batch(VkrEditorSession *session, uint64_t token,
                                   const char *author, const char *label,
                                   bool8_t review) {
  if (!session || session->mode == VKR_EDITOR_SESSION_NONE || !token) {
    return;
  }
  SessionNote *note = &session->notes[session->note_next++ % SESSION_NOTE_MAX];
  note->token = token;
  snprintf(note->author, sizeof(note->author), "%s", author ? author : "");
  snprintf(note->label, sizeof(note->label), "%s", label ? label : "");
  note->review = review;
  /* The terrains noted this build belong to this batch. */
  MemCopy(note->stages, session->stages,
          session->stage_count * sizeof(session->stages[0]));
  note->stage_count = session->stage_count;
  session->stage_count = 0u;
}

void vkr_editor_session_note_terrain(VkrEditorSession *session,
                                     const char *relative, uint32_t cells,
                                     float32_t spacing, float32_t height_min,
                                     float32_t height_max, float32_t height) {
  if (!session || session->mode == VKR_EDITOR_SESSION_NONE ||
      session->stage_count == SESSION_STAGE_MAX) {
    return;
  }
  SessionStage *stage = &session->stages[session->stage_count++];
  *stage = (SessionStage){.cells = cells,
                          .spacing = spacing,
                          .height_min = height_min,
                          .height_max = height_max,
                          .height = height};
  snprintf(stage->path, sizeof(stage->path), "%s", relative);
}

bool8_t vkr_editor_session_take_applied(VkrEditorSession *session,
                                        VkrEditorSessionApplied *out) {
  if (!session || !session->applied_count) {
    return false_v;
  }
  *out = session->applied[session->applied_head];
  session->applied_head = (session->applied_head + 1u) % SESSION_APPLIED_MAX;
  session->applied_count -= 1u;
  return true_v;
}

const char *vkr_editor_session_name(const VkrEditorSession *session) {
  return session && session->mode != VKR_EDITOR_SESSION_NONE ? session->name
                                                             : "";
}

bool8_t vkr_editor_session_forwards(const VkrEditorSession *session) {
  return session && session->mode == VKR_EDITOR_SESSION_PARTICIPANT &&
         session->welcomed && !session->leave_pending;
}

bool8_t vkr_editor_session_hosting(const VkrEditorSession *session) {
  return session && session->mode == VKR_EDITOR_SESSION_HOST;
}

uint64_t vkr_editor_session_ask(VkrEditorSession *session,
                                const VkrEditorSessionAsk *ask) {
  if (!vkr_editor_session_forwards(session)) {
    return 0u;
  }
  VkrEditorSessionAsk sent = *ask;
  sent.token = session_forward_token(session);
  const uint32_t size = session_build(session, session_put_ask, &sent);
  return size && session_send(session, session->connection,
                              SESSION_EDIT_CHANNEL, session->message.data, size)
             ? sent.token
             : 0u;
}

bool8_t vkr_editor_session_take_answer(VkrEditorSession *session,
                                       uint64_t token,
                                       VkrEditorSessionAnswer *out) {
  for (uint32_t i = 0u; session && i < SESSION_ANSWER_MAX; ++i) {
    SessionAnswerSlot *slot = &session->answers[i];
    if (slot->used && slot->answer.token == token) {
      *out = slot->answer;
      slot->used = false_v;
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_editor_session_take_ask(VkrEditorSession *session,
                                    VkrEditorSessionAsk *out) {
  if (!vkr_editor_session_hosting(session) || !session->ask_count) {
    return false_v;
  }
  *out = session->asks[session->ask_head];
  session->ask_head = (session->ask_head + 1u) % SESSION_ASK_MAX;
  session->ask_count -= 1u;
  return true_v;
}

void vkr_editor_session_reply(VkrEditorSession *session,
                              const VkrEditorSessionAsk *ask,
                              const VkrEditorSessionAnswer *answer) {
  SessionLink *link = vkr_editor_session_hosting(session)
                          ? session_link_by_id(session, ask->peer)
                          : NULL;
  if (!link) {
    return;
  }
  VkrEditorSessionAnswer sent = *answer;
  sent.token = ask->token;
  const uint32_t size = session_build(session, session_put_answer, &sent);
  if (size) {
    (void)session_send(session, link->connection, SESSION_EDIT_CHANNEL,
                       session->message.data, size);
  }
}

void vkr_editor_session_publish(VkrEditorSession *session,
                                const VkrEditorClaim *claims,
                                uint32_t claim_count,
                                const VkrEditorTask *tasks,
                                uint32_t task_count) {
  if (!vkr_editor_session_hosting(session)) {
    return;
  }
  const SessionShared shared = {.claims = claims,
                                .claim_count = claim_count,
                                .tasks = tasks,
                                .task_count = task_count};
  const uint32_t size = session_build(session, session_put_shared, &shared);
  uint8_t *bytes = size ? realloc(session->shared_bytes, size) : NULL;
  if (!bytes) {
    log_warn("The session could not send its claims and tasks");
    return;
  }
  MemCopy(bytes, session->message.data, size);
  session->shared_bytes = bytes;
  session->shared_size = size;
  for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    const SessionLink *link = &session->links[i];
    if (link->live && link->welcomed) {
      (void)session_send(session, link->connection, SESSION_EDIT_CHANNEL, bytes,
                         size);
    }
  }
}

uint64_t vkr_editor_session_epoch(const VkrEditorSession *session) {
  return session ? session->epoch : 0u;
}

uint64_t vkr_editor_session_shared(const VkrEditorSession *session,
                                   const VkrEditorClaim **out_claims,
                                   uint32_t *out_claim_count,
                                   const VkrEditorTask **out_tasks,
                                   uint32_t *out_task_count) {
  if (!vkr_editor_session_forwards(session)) {
    *out_claims = NULL;
    *out_claim_count = 0u;
    *out_tasks = NULL;
    *out_task_count = 0u;
    return 0u;
  }
  *out_claims = session->shared_claims;
  *out_claim_count = session->shared_claim_count;
  *out_tasks = session->shared_tasks;
  *out_task_count = session->shared_task_count;
  return session->shared_revision;
}

// =============================================================================
// Session window and Scene overlay
// =============================================================================

Vec4 vkr_editor_session_peer_color(uint8_t id) {
  static const Vec4 colors[] = {
      {0.30f, 0.80f, 1.00f, 1.0f}, {1.00f, 0.55f, 0.25f, 1.0f},
      {0.55f, 0.95f, 0.40f, 1.0f}, {0.95f, 0.40f, 0.85f, 1.0f},
      {1.00f, 0.90f, 0.30f, 1.0f}, {0.60f, 0.55f, 1.00f, 1.0f},
      {0.35f, 1.00f, 0.80f, 1.0f}, {1.00f, 0.40f, 0.40f, 1.0f}};
  return colors[id % ArrayCount(colors)];
}

typedef struct SessionLines {
  VkrEditorBrushGridLine *out;
  uint32_t capacity;
  uint32_t count;
} SessionLines;

static void session_line(SessionLines *lines, Vec3 from, Vec3 to, Vec4 color) {
  if (lines->out && lines->count < lines->capacity) {
    lines->out[lines->count] =
        (VkrEditorBrushGridLine){.from = from, .to = to, .color = color};
  }
  lines->count += 1u;
}

/* A camera as a pyramid from its eye along yaw and pitch (degrees, as
   VkrCamera turns them), half a metre deep. */
static void session_camera_lines(SessionLines *lines,
                                 const VkrEditorSessionPeer *peer, Vec4 color) {
  const float32_t yaw = vkr_to_radians(peer->camera_yaw);
  const float32_t pitch = vkr_to_radians(peer->camera_pitch);
  const Vec3 forward = vec3_normalize(
      vec3_new(vkr_cos_f32(yaw) * vkr_cos_f32(pitch), vkr_sin_f32(pitch),
               vkr_sin_f32(yaw) * vkr_cos_f32(pitch)));
  const Vec3 right =
      vec3_normalize(vec3_cross(forward, vec3_new(0.0f, 1.0f, 0.0f)));
  const Vec3 up = vec3_normalize(vec3_cross(right, forward));
  const Vec3 eye = peer->camera_position;
  const Vec3 center = vec3_add(eye, vec3_scale(forward, 0.5f));
  Vec3 corners[4];
  for (uint32_t i = 0u; i < 4u; ++i) {
    const float32_t sx = (i == 0u || i == 3u) ? -0.3f : 0.3f;
    const float32_t sy = i < 2u ? 0.2f : -0.2f;
    corners[i] =
        vec3_add(center, vec3_add(vec3_scale(right, sx), vec3_scale(up, sy)));
  }
  for (uint32_t i = 0u; i < 4u; ++i) {
    session_line(lines, eye, corners[i], color);
    session_line(lines, corners[i], corners[(i + 1u) % 4u], color);
  }
  /* The roof marker says which way is up. */
  session_line(lines, vec3_add(center, vec3_scale(up, 0.2f)),
               vec3_add(center, vec3_scale(up, 0.32f)), color);
}

/* An entity's local bounds through `world`, or a cross at its position
   when it has none. */
static void session_entity_lines(SessionLines *lines, const VkrScene *scene,
                                 VkrEntityId entity, Mat4 world, Vec4 color) {
  Vec3 lower = {0};
  Vec3 upper = {0};
  if (!vkr_scene_entity_local_bounds(scene, entity, &lower, &upper)) {
    const Vec3 at = mat4_position(world);
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      const Vec3 half =
          vec3_new(axis == 0u ? 0.25f : 0.0f, axis == 1u ? 0.25f : 0.0f,
                   axis == 2u ? 0.25f : 0.0f);
      session_line(lines, vec3_sub(at, half), vec3_add(at, half), color);
    }
    return;
  }
  Vec3 corners[8];
  for (uint32_t i = 0u; i < 8u; ++i) {
    corners[i] = mat4_mul_vec3(world, vec3_new((i & 1u) ? upper.x : lower.x,
                                               (i & 2u) ? upper.y : lower.y,
                                               (i & 4u) ? upper.z : lower.z));
  }
  for (uint32_t i = 0u; i < 8u; ++i) {
    for (uint32_t bit = 1u; bit < 8u; bit <<= 1u) {
      if (!(i & bit)) {
        session_line(lines, corners[i], corners[i | bit], color);
      }
    }
  }
}

uint32_t vkr_editor_session_lines(const VkrEditorSession *session,
                                  const VkrSampleUiFrame *frame,
                                  VkrEditorBrushGridLine *out,
                                  uint32_t capacity) {
  if (!session || session->mode == VKR_EDITOR_SESSION_NONE ||
      frame->scripts_running) {
    return 0u;
  }
  SessionLines lines = {.out = out, .capacity = capacity};
  for (uint32_t i = 0u; i < session->peer_count; ++i) {
    const VkrEditorSessionPeer *peer = &session->peers[i];
    const Vec4 color = vkr_editor_session_peer_color(peer->id);
    if (peer->has_camera) {
      session_camera_lines(&lines, peer, color);
    }
    const VkrScene *scene =
        peer->has_selection
            ? session_container(frame, peer->selection_container)
            : NULL;
    const VkrEntityId entity =
        scene ? vkr_scene_find_entity_ref(scene, &peer->selection)
              : VKR_ENTITY_ID_INVALID;
    const SceneTransform *transform =
        entity.u64 && vkr_scene_entity_alive(scene, entity)
            ? vkr_entity_get_component(scene->world, entity,
                                       scene->comp_transform)
            : NULL;
    if (transform) {
      session_entity_lines(&lines, scene, entity, transform->world, color);
    }
    /* A drag in progress: the entity where the peer holds it now, and a
       line from where it is here. */
    const VkrScene *dragged_scene =
        peer->dragging ? session_container(frame, peer->drag_container) : NULL;
    const VkrEntityId dragged =
        dragged_scene ? vkr_scene_find_entity_ref(dragged_scene, &peer->drag)
                      : VKR_ENTITY_ID_INVALID;
    const SceneTransform *resting =
        dragged.u64 && vkr_scene_entity_alive(dragged_scene, dragged)
            ? vkr_entity_get_component(dragged_scene->world, dragged,
                                       dragged_scene->comp_transform)
            : NULL;
    if (resting) {
      session_entity_lines(&lines, dragged_scene, dragged, peer->drag_world,
                           color);
      session_line(&lines, mat4_position(resting->world),
                   mat4_position(peer->drag_world), color);
    }
  }
  return out ? Min(lines.count, capacity) : lines.count;
}

void vkr_editor_session_names_build(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame) {
  const VkrEditorSession *session = editor->session;
  if (!session || session->mode == VKR_EDITOR_SESSION_NONE ||
      !session->peer_count || !frame->mapping_valid ||
      frame->scene_rendering_stopped || frame->scripts_running) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = ui->content_scale;
  const Vec4 image = frame->mapping.image_rect_px;
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt =
      (VkrUiEdges){image.y / scale, 0, 0, image.x / scale};
  panel.style.min_size_pt = panel.style.max_size_pt =
      (Vec2){image.z / scale, image.w / scale};
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("session.names"), &panel)) {
    return;
  }
  for (uint32_t i = 0u; i < session->peer_count; ++i) {
    const VkrEditorSessionPeer *peer = &session->peers[i];
    Vec2 pixel = {0};
    if (!peer->has_camera ||
        !vkr_editor_viewport_pixel(frame, peer->camera_position, &pixel)) {
      continue;
    }
    const Vec2 text = vkr_editor_text_size(
        ui, VKR_FONT_HANDLE_INVALID, peer->name, vkr_ui_theme()->font_caption);
    const Vec2 size = {text.x + 14.0f, 20.0f};
    const float32_t x = (pixel.x - image.x) / scale - size.x * 0.5f;
    const float32_t y = (pixel.y - image.y) / scale - size.y - 14.0f;
    if (x < 0.0f || y < 0.0f || x + size.x > image.z / scale ||
        y + size.y > image.w / scale) {
      continue;
    }
    VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
    chip.placement = VKR_UI_PLACEMENT_DEFAULT;
    chip.placement.column = chip.placement.row = 0;
    chip.placement.justify = chip.placement.align = VKR_UI_ALIGN_START;
    chip.placement.margin_pt = (VkrUiEdges){y, 0, 0, x};
    chip.style.min_size_pt = chip.style.max_size_pt = size;
    chip.style.padding_pt = (VkrUiEdges){2, 7, 2, 7};
    chip.style.corner_radius_pt = (Vec4){10, 10, 10, 10};
    chip.style.font_size_pt = vkr_ui_theme()->font_caption;
    chip.style.background_color =
        vkr_ui_color_alpha(vkr_editor_session_peer_color(peer->id), 0.9f);
    chip.style.text_color = (Vec4){0.04f, 0.05f, 0.06f, 1.0f};
    (void)vkr_ui_push_id_u64(ui, peer->id);
    vkr_ui_label(ui, string8_lit("name"),
                 string8_create_from_cstr((const uint8_t *)peer->name,
                                          strlen(peer->name)),
                 &chip);
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

static String8 session_string(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, strlen(text));
}

/* A widget at (x, y) points inside the window body, as the Build window
   lays out its rows. */
static VkrUiWidgetConfig session_widget(float32_t x, float32_t y,
                                        float32_t width, float32_t height) {
  VkrUiWidgetConfig c = vkr_ui_widget_config_default();
  c.placement = (VkrUiPlacement){.column_span = 1,
                                 .row_span = 1,
                                 .justify = VKR_UI_ALIGN_START,
                                 .align = VKR_UI_ALIGN_START,
                                 .margin_pt = {y, 0, 0, x}};
  c.style.min_size_pt = (Vec2){Max(1.0f, width), height};
  c.style.max_size_pt = c.style.min_size_pt;
  c.style.font_size_pt = vkr_ui_theme()->font_body;
  c.style.text_color = vkr_ui_theme()->text;
  c.style.padding_pt = (VkrUiEdges){2, 4, 2, 4};
  return c;
}

static void session_label(VkrUiSystem *ui, const char *id, const char *text,
                          float32_t x, float32_t y, float32_t width,
                          Vec4 color) {
  VkrUiWidgetConfig c = session_widget(x, y, width, 24);
  c.style.text_color = color;
  vkr_ui_label(ui, session_string(id), session_string(text), &c);
}

static void session_field(VkrUiSystem *ui, const char *id, const char *label,
                          char *text, uint32_t capacity, float32_t y,
                          float32_t width) {
  session_label(ui, id, label, 12, y, 90, vkr_ui_theme()->text_secondary);
  VkrUiWidgetConfig c = session_widget(102, y, width - 114, 24);
  vkr_editor_field_style(&c);
  VkrUiTextEditBuffer buffer = {(uint8_t *)text, (uint32_t)strlen(text),
                                capacity};
  char field_id[64];
  snprintf(field_id, sizeof(field_id), "%s.field", id);
  (void)vkr_ui_text_field(ui, session_string(field_id), &buffer, &c);
}

static bool8_t session_button(VkrEditorUi *editor, VkrUiSystem *ui,
                              const char *id, const char *text, float32_t x,
                              float32_t y, float32_t width, bool8_t primary) {
  VkrUiWidgetConfig c = session_widget(x, y, width, 26);
  if (primary) {
    vkr_editor_primary_style(&c, editor->heading_font);
  } else {
    vkr_editor_action_style(&c, editor->heading_font);
  }
  return vkr_ui_button(ui, session_string(id), session_string(text), &c);
}

static void session_window_peers(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrEditorSession *session, float32_t *y,
                                 float32_t width) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[160];
  snprintf(text, sizeof(text), "%u other editor%s", session->peer_count,
           session->peer_count == 1u ? "" : "s");
  session_label(ui, "peers.title", text, 12, *y, width - 24, theme->text);
  *y += 28;
  for (uint32_t i = 0u; i < session->peer_count; ++i) {
    const VkrEditorSessionPeer peer = session->peers[i];
    (void)vkr_ui_push_id_u64(ui, peer.id);
    VkrUiWidgetConfig swatch = session_widget(12, *y + 6, 12, 12);
    swatch.style.background_color = vkr_editor_session_peer_color(peer.id);
    swatch.style.corner_radius_pt = (Vec4){6, 6, 6, 6};
    vkr_ui_label(ui, string8_lit("swatch"), (String8){0}, &swatch);
    if (peer.has_camera) {
      snprintf(text, sizeof(text), "%s  %s  (%.1f, %.1f, %.1f)", peer.name,
               peer.tool, peer.camera_position.x, peer.camera_position.y,
               peer.camera_position.z);
    } else {
      snprintf(text, sizeof(text), "%s  %s", peer.name, peer.tool);
    }
    session_label(ui, "name", text, 32, *y, width - 150, theme->text);
    if (peer.has_camera && frame->editor_state_request &&
        session_button(editor, ui, "view", "Go to view", width - 112, *y, 100,
                       false_v)) {
      frame->editor_state_request->move_camera = true_v;
      frame->editor_state_request->camera_position = peer.camera_position;
      frame->editor_state_request->camera_yaw = peer.camera_yaw;
      frame->editor_state_request->camera_pitch = peer.camera_pitch;
    }
    (void)vkr_ui_pop_id(ui);
    *y += 30;
  }
}

void vkr_editor_session_window_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds) {
  VkrEditorSession *session = editor->session;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t width = bounds.width / ui->content_scale;
  if (!session || width < 240) {
    return;
  }
  VkrUiPanelConfig layout = vkr_ui_panel_config_default();
  layout.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("session.layout"), &layout)) {
    return;
  }
  float32_t y = 10;
  char text[256];
  char error[192] = {0};
  if (session->mode == VKR_EDITOR_SESSION_NONE) {
    session_label(ui, "state", "Not in a collaborative session.", 12, y,
                  width - 24, theme->text_secondary);
    y += 30;
    session_field(ui, "name", "Your name", session->form_name,
                  sizeof(session->form_name), y, width);
    y += 38;
    session_field(ui, "bind", "Listen on", session->form_bind,
                  sizeof(session->form_bind), y, width);
    y += 30;
    if (session_button(editor, ui, "host", "Host this scene", 102, y, 160,
                       true_v) &&
        !vkr_editor_session_host(session, frame, session->form_bind,
                                 session->form_name, error, sizeof(error))) {
      snprintf(session->error, sizeof(session->error), "%s", error);
    }
    y += 42;
    session_field(ui, "address", "Address", session->form_address,
                  sizeof(session->form_address), y, width);
    y += 30;
    session_field(ui, "key", "Host key", session->form_key,
                  sizeof(session->form_key), y, width);
    y += 30;
    if (session_button(editor, ui, "join", "Join", 102, y, 160, true_v) &&
        !vkr_editor_session_join(session, frame, session->form_address,
                                 session->form_key, session->form_name, error,
                                 sizeof(error))) {
      snprintf(session->error, sizeof(session->error), "%s", error);
    }
    y += 36;
  } else {
    const bool8_t host = session->mode == VKR_EDITOR_SESSION_HOST;
    if (host) {
      snprintf(text, sizeof(text), "Hosting on %s as %s", session->address,
               session->name);
    } else if (session->welcomed) {
      snprintf(text, sizeof(text), "Joined %s as %s; edit %llu applied",
               session->address, session->name,
               (unsigned long long)(session->apply_seq - 1u));
    } else {
      snprintf(text, sizeof(text), "Joining %s...", session->address);
    }
    session_label(ui, "state", text, 12, y, width - 24, theme->text);
    y += 28;
    snprintf(text, sizeof(text), "Key %.16s...", session->key);
    session_label(ui, "key", text, 12, y, width - 160, theme->text_secondary);
    if (host && session_button(editor, ui, "copy", "Copy key", width - 136, y,
                               124, false_v)) {
      (void)vkr_platform_clipboard_write_text((const uint8_t *)session->key,
                                              (uint32_t)strlen(session->key));
    }
    y += 34;
    session_window_peers(editor, frame, session, &y, width);
    y += 6;
    if (session_button(editor, ui, "leave",
                       host ? "Stop hosting" : "Leave session", 12, y, 160,
                       false_v)) {
      vkr_editor_session_leave(session);
    }
    y += 36;
  }
  if (session->error[0]) {
    session_label(ui, "error", session->error, 12, y, width - 24,
                  theme->warning);
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_session_accepted(VkrEditorSession *session, uint16_t container,
                                 uint64_t group) {
  if (!session || session->mode == VKR_EDITOR_SESSION_NONE) {
    return;
  }
  const uint64_t seq = session_seq_of_group(session, container, group);
  if (!seq) {
    return;
  }
  if (session->mode == VKR_EDITOR_SESSION_HOST) {
    uint8_t bytes[16];
    const uint32_t size = session_put_reviewed(bytes, sizeof(bytes), seq);
    for (uint32_t i = 0u; size && i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
      const SessionLink *link = &session->links[i];
      if (link->live && link->welcomed) {
        session_host_send_small(session, link->connection, bytes, size);
      }
    }
    return;
  }
  uint8_t bytes[16];
  const uint32_t size = session_put_reviewed(bytes, sizeof(bytes), seq);
  if (size && session->welcomed) {
    (void)session_send(session, session->connection, SESSION_EDIT_CHANNEL,
                       bytes, size);
  }
}

bool8_t vkr_editor_session_take_accepted(VkrEditorSession *session,
                                         uint16_t *out_container,
                                         uint64_t *out_group) {
  if (!session || !session->accepted_count) {
    return false_v;
  }
  const SessionAccepted accepted = session->accepted[session->accepted_head];
  session->accepted_head = (session->accepted_head + 1u) % SESSION_ACCEPTED_MAX;
  session->accepted_count -= 1u;
  *out_container = accepted.container;
  *out_group = accepted.group;
  return true_v;
}

void vkr_editor_session_set_tool(VkrEditorSession *session, const char *tool) {
  if (session) {
    snprintf(session->tool, sizeof(session->tool), "%s", tool ? tool : "");
  }
}
