#include "editor_session.h"

#include "editor_internal.h"

#include "containers/vkr_sort.h"
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "memory/vkr_heap.h"
#include "net/vkr_net_scene_edit.h"
#include "renderer/systems/vkr_scene_terrain.h"
#include "vkr_bakery_os.h"
#include "vkr_bitstream.h"
#include "vkr_depot_sync.h"
#include "vkr_depot_zstd.h"
#include "vkr_net_crypto.h"
#include "vkr_net_host.h"
#include "vkr_net_session.h"

#include <stdarg.h>

/* The collaboration service, version 1 (docs/proposals/network-protocol.md,
 * "Editor collaboration"). The schema hash names this file's message layout;
 * a layout change takes a new version and hash.
 *
 * Channel 0, reliable and ordered, carries the session: HELLO and WELCOME,
 * EDIT from a participant, RESULT for a refused one, APPLIED in session
 * order, PEER_LEFT, for agents ASK, ANSWER, SHARED (the host's claims and
 * tasks) and REVIEWED (a batch accepted on some editor), and for the depot
 * BASE (the session's branch and base commit, before HELLO) and PUBLISHED
 * (a commit of files to fetch). EDIT and
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
#define SESSION_SCHEMA_HASH 0x434f4c4c4142000eull
#define SESSION_EDIT_CHANNEL 0u
#define SESSION_PRESENCE_CHANNEL 1u
#define SESSION_MESSAGE_MAX (8u << 20)
#define SESSION_PRESENCE_US 100000ull
/* Presence while this editor drags a gizmo, so others follow the drag. */
#define SESSION_PRESENCE_DRAG_US 33000ull
/* The farthest cursor hit presence carries, and how far a cursor that
   meets nothing draws. */
#define SESSION_CURSOR_DISTANCE_MAX 10000.0f
#define SESSION_CURSOR_LENGTH 50.0f
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
  SESSION_MSG_BASE,
  SESSION_MSG_PUBLISHED,
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

/* Where a session stands with its depot (ADR-106). Without a working copy
 * around the scene, joining checks only the scene digest (OFF). */
typedef enum SessionDepotPhase {
  SESSION_DEPOT_OFF = 0,
  /* Host: saving the scene, then committing it as the session's base. */
  SESSION_DEPOT_SAVING,
  SESSION_DEPOT_BASING,
  /* Participant: waiting for BASE, checking the base out, reloading. */
  SESSION_DEPOT_WAITING,
  SESSION_DEPOT_CHECKING_OUT,
  SESSION_DEPOT_RELOADING,
  SESSION_DEPOT_READY,
} SessionDepotPhase;

typedef enum SessionDepotJobKind {
  SESSION_DEPOT_COMMIT = 1,
  SESSION_DEPOT_CHECKOUT,
} SessionDepotJobKind;

/* A host's new base for joiners once its history passes the budget: it
   holds edits while the scene saves, then commits the saved scene to the
   session's base branch while edits go on. FAILED stops trying; the history
   then drops what every editor has, as without a depot. */
typedef enum SessionRebase {
  SESSION_REBASE_NONE = 0,
  SESSION_REBASE_SAVING,
  SESSION_REBASE_COMMITTING,
  SESSION_REBASE_FAILED,
} SessionRebase;

/* One depot step on a worker thread, which owns every field but `done`
   until it sets `done`. */
typedef struct SessionDepotJob {
  VkrThread thread;
  bool8_t running;
  VkrAtomicBool done;
  uint8_t kind;
  bool8_t publish;
  /* Commit: on the working copy's base instead of `parent`. */
  bool8_t from_base;
  /* Commit: an automatic publish, which commits only when a file outside
     the open scene's documents changed and has been quiet for a moment;
     `skipped` when it found none. */
  bool8_t automatic;
  bool8_t skipped;
  char directory[1024];
  /* Automatic publish: the open scene's document. */
  char scene[1024];
  char branch[VKR_DEPOT_BRANCH_NAME];
  char message[128];
  VkrDepotId parent;
  VkrDepotId expected;
  /* The commit to check out, or the commit made. */
  VkrDepotId commit;
  bool8_t ok;
  char error[256];
} SessionDepotJob;

#define SESSION_FETCH_MAX 16u
/* Automatic publishing looks for changed files this often, and publishes a
   change once its file has not been written for the quiet time. */
#define SESSION_PUBLISH_POLL_US 3000000ull
#define SESSION_PUBLISH_QUIET_SECONDS 2
/* Builds the host waits for its scene to save before hosting fails. */
#define SESSION_SAVE_FRAMES 600u

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
  /* The service opened and BASE went out. */
  bool8_t opened;
  bool8_t based;
  VkrNetConnectionId connection;
  uint8_t id;
  /* The participant's identity key. */
  uint8_t key[32];
  /* The base BASE sent it: the session sequence its scene holds, from
     which the history replays, and that scene's digest. */
  uint64_t base_seq;
  uint8_t base_digest[32];
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
  /* The UI's: the network thread's and the depot job's handles, made and
     freed on the UI thread. */
  VkrAllocator *allocator;
  uint8_t mode;
  VkrNetHost *host;
  VkrNetSession *net;
  /* The host and the net session allocate from memory of their own: the
     network thread allocates from it while it pumps, and an allocator is
     not thread-safe, so the UI's, which the UI keeps using, would corrupt.
     Only holders of the host lock use it while the thread runs. */
  VkrDMemory net_memory;
  VkrAllocator net_allocator;
  bool8_t net_memory_ready;
  /* Pumps `host`, so acknowledgments, resends and pacing keep their timing
     however long a frame takes. Messages are still read and handled on the
     UI thread: the public calls that reach the network hold its lock
     (session_lock), `lock_depth` deep. NULL when it could not start; the
     UI thread then pumps the host each build. */
  VkrNetHostThread *net_thread;
  uint32_t lock_depth;
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
  /* The newest base: the sequence its scene holds (`digest` is that
     scene's), the history's bytes since and the budget past which a depot
     host makes a new one. A batch at or below `revert_floor` cannot be
     reverted, as an editor that joined from a newer base lacks it. */
  uint64_t base_seq;
  uint64_t base_bytes;
  uint64_t history_budget;
  uint64_t revert_floor;
  uint8_t rebase;
  /* When automatic publishing last looked for changed files. */
  uint64_t publish_polled_at;
  uint32_t rebase_frames;
  uint64_t rebase_seq;
  uint64_t rebase_bytes;
  uint8_t rebase_digest[32];

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
  /* The world partition cell the last placed edit lacked, and the cell this
     editor last loaded for an edit, so a cell that does not load fails the
     edit instead of retrying it every frame. */
  VkrNetSceneEditMissingCell missing;
  VkrNetSceneEditMissingCell cell_requested;
  VkrSampleEditBatchResult refused;
  VkrSceneEditRequest scratch;
  SessionBuffer payload;
  SessionBuffer message;

  VkrEditorSessionPeer peers[VKR_EDITOR_SESSION_PEERS_MAX];
  uint32_t peer_count;
  uint64_t presence_at;
  /* This editor's Scene pointer, which presence carries. */
  bool8_t has_cursor;
  Vec3 cursor_origin;
  Vec3 cursor_direction;
  float32_t cursor_distance;

  /* Depot: the phase, the session branch, the commit this editor holds,
     the working copy's directory, the depot step running and the commits
     other editors published, to fetch in order. */
  uint8_t depot_phase;
  char depot_branch[VKR_DEPOT_BRANCH_NAME];
  /* The commit the session started from, which joiners check out before
     the history replays, and the newest published one. */
  VkrDepotId depot_base;
  VkrDepotId depot_head;
  /* Host: the branch of the newer bases, its tip (zero before the first)
     and the session commit the newest base was made on. */
  char depot_base_branch[VKR_DEPOT_BRANCH_NAME];
  VkrDepotId depot_base_tip;
  VkrDepotId depot_base_parent;
  char depot_directory[1024];
  SessionDepotJob job;
  VkrDepotId fetches[SESSION_FETCH_MAX];
  uint32_t fetch_count;
  uint32_t save_frames;
  bool8_t save_requested;
  bool8_t reload_requested;
  uint64_t reload_generation;

  /* Who joins when this editor hosts (vkr_editor_session_restrict); it
     survives a reset. */
  bool8_t restricted;
  uint8_t allowed[VKR_EDITOR_SESSION_ALLOWED_MAX][32];
  uint32_t allowed_count;
  VkrEditorSessionRefused refusals[VKR_EDITOR_SESSION_REFUSED_MAX];
  uint32_t refused_count;
  /* This editor's identity key in hexadecimal, read once. */
  char identity[65];
  bool8_t identity_read;

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
  const uint32_t length = (uint32_t)string_length(text);
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
  if (!text || string_length(text) != 2u * size) {
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
  const char *colon = string_get_last_char_occurrence(text, ':');
  uint32_t port = VKR_EDITOR_SESSION_PORT;
  size_t length = string_length(text);
  if (colon) {
    port = (uint32_t)string_parse_u64(colon + 1, NULL);
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
  uint8_t *grown = vkr_heap_realloc(buffer->data, capacity);
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
  return MemCompare(left->ref, right->ref, sizeof(left->ref));
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
  vkr_sha256_update(&hash, values.name, string_length(values.name) + 1u);
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
      capacity ? vkr_heap_alloc(capacity * sizeof(*records)) : NULL;
  uint64_t count = 0u;
  for (uint32_t w = 0u; records && w < ArrayCount(worlds); ++w) {
    const VkrScene *scene = session_container(frame, worlds[w]);
    /* Each editor streams world partition cells around its own camera, so
       entities that stream with a cell stay out of the digest. Edits to
       them still verify through the mirrored journals. */
    SceneWorldPartition partition;
    const bool8_t partitioned =
        scene && vkr_scene_partition_settings(scene, &partition);
    for (uint32_t i = 0u; scene && i < scene->world->dir.living; ++i) {
      const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
      VkrScenePartitionCell cell;
      if (vkr_scene_entity_alive(scene, entity) &&
          !(partitioned && vkr_scene_partition_entity_cell(scene, &partition,
                                                           entity, &cell)) &&
          session_digest_entity(scene, entity, worlds[w], &records[count])) {
        count += 1u;
      }
    }
  }
  if (count) {
    vkr_sort(records, count, sizeof(*records), session_digest_compare);
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
  vkr_heap_free(records);
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
        *out_bytes = vkr_heap_alloc(Max(size, 1u));
        if (!*out_bytes) {
          string_format(payload->error, sizeof(payload->error),
                        "Out of memory.");
          return false_v;
        }
        if (size) {
          MemCopy(*out_bytes, session->payload.data, size);
        }
        return true_v;
      }
    }
    if (!session_buffer_grow(&session->payload)) {
      string_format(payload->error, sizeof(payload->error),
                    "The edit is larger than a session message.");
      return false_v;
    }
  }
}

static void session_edit_release(SessionEdit *edit) {
  vkr_heap_free(edit->payload);
  vkr_heap_free(edit->results);
  vkr_heap_free(edit->stages);
  MemZero(edit, sizeof(*edit));
}

static bool8_t session_queue_push(VkrEditorSession *session,
                                  const SessionEdit *edit) {
  if (session->queue_head + session->queue_count == session->queue_capacity) {
    if (session->queue_head) {
      MemCopy(session->queue, session->queue + session->queue_head,
              session->queue_count * sizeof(*session->queue));
      session->queue_head = 0u;
    } else {
      const uint32_t capacity =
          session->queue_capacity ? session->queue_capacity * 2u : 64u;
      SessionEdit *grown =
          vkr_heap_realloc(session->queue, capacity * sizeof(*session->queue));
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

/* Puts the edit just popped back at the head. The pop left room for it. */
static void session_queue_unpop(VkrEditorSession *session,
                                const SessionEdit *edit) {
  if (session->queue_head) {
    session->queue_head -= 1u;
  } else {
    MemCopy(session->queue + 1, session->queue,
            session->queue_count * sizeof(*session->queue));
  }
  session->queue[session->queue_head] = *edit;
  session->queue_count += 1u;
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
        vkr_heap_realloc(session->groups, capacity * sizeof(*session->groups));
    if (!grown) {
      return;
    }
    session->groups = grown;
    session->group_capacity = capacity;
  }
  SessionGroup *entry = &session->groups[session->group_count++];
  *entry = (SessionGroup){.seq = seq, .group = group, .container = container};
  if (edit) {
    string_format(entry->agent, sizeof(entry->agent), "%s", edit->agent);
    string_format(entry->label, sizeof(entry->label), "%s", edit->label);
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
      vkr_heap_realloc(session->items, count * sizeof(*grown));
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
    out->stages = vkr_heap_alloc_zeroed(stages, sizeof(*out->stages));
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
  out->payload = vkr_heap_alloc(Max((uint32_t)size, 1u));
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
      out->results = vkr_heap_alloc(results);
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
  vkr_bit_write(&writer, peer->has_cursor, 1u);
  if (peer->has_cursor) {
    session_write_float(&writer, peer->cursor_origin.x);
    session_write_float(&writer, peer->cursor_origin.y);
    session_write_float(&writer, peer->cursor_origin.z);
    session_write_float(&writer, peer->cursor_direction.x);
    session_write_float(&writer, peer->cursor_direction.y);
    session_write_float(&writer, peer->cursor_direction.z);
    session_write_float(&writer, peer->cursor_distance);
  }
  return vkr_bit_writer_finish(&writer);
}

/* A cursor ray other editors draw: finite, a direction near unit length
   (normalized here) and a distance within SESSION_CURSOR_DISTANCE_MAX. */
static bool8_t session_cursor_valid(Vec3 origin, Vec3 *direction,
                                    float32_t distance) {
  const float32_t values[7] = {origin.x,     origin.y,     origin.z,
                               direction->x, direction->y, direction->z,
                               distance};
  for (uint32_t i = 0u; i < ArrayCount(values); ++i) {
    if (!vkr_is_finite_f32(values[i])) {
      return false_v;
    }
  }
  const float32_t length = vec3_length(*direction);
  if (length < 0.5f || length > 2.0f || distance < 0.0f ||
      distance > SESSION_CURSOR_DISTANCE_MAX) {
    return false_v;
  }
  *direction = vec3_scale(*direction, 1.0f / length);
  return true_v;
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
  out->has_cursor = (bool8_t)vkr_bit_read(reader, 1u);
  if (out->has_cursor) {
    const float32_t ox = session_read_float(reader);
    const float32_t oy = session_read_float(reader);
    const float32_t oz = session_read_float(reader);
    const float32_t dx = session_read_float(reader);
    const float32_t dy = session_read_float(reader);
    const float32_t dz = session_read_float(reader);
    out->cursor_origin = vec3_new(ox, oy, oz);
    out->cursor_direction = vec3_new(dx, dy, dz);
    out->cursor_distance = session_read_float(reader);
    if (!session_cursor_valid(out->cursor_origin, &out->cursor_direction,
                              out->cursor_distance)) {
      return false_v;
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

static bool8_t session_allowed(const VkrEditorSession *session,
                               const uint8_t key[32]) {
  for (uint32_t i = 0u; i < session->allowed_count; ++i) {
    if (MemCompare(session->allowed[i], key, 32u) == 0) {
      return true_v;
    }
  }
  return false_v;
}

/* Lists an editor a restricted host refused, newest last, once per key. The
   name is the participant's credential, kept to printable characters. */
static void session_note_refused(VkrEditorSession *session,
                                 const uint8_t key[32],
                                 const uint8_t *credential,
                                 uint32_t credential_size) {
  VkrEditorSessionRefused refused = {0};
  session_hex(key, 32u, refused.key);
  const uint32_t length =
      Min(credential_size, (uint32_t)sizeof(refused.name) - 1u);
  for (uint32_t i = 0u; i < length; ++i) {
    const uint8_t c = credential[i];
    refused.name[i] = c >= 0x20u && c < 0x7fu ? (char)c : '?';
  }
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < session->refused_count; ++i) {
    if (!string_equals(session->refusals[i].key, refused.key)) {
      session->refusals[kept++] = session->refusals[i];
    }
  }
  if (kept == VKR_EDITOR_SESSION_REFUSED_MAX) {
    MemCopy(&session->refusals[0], &session->refusals[1],
            (kept - 1u) * sizeof(session->refusals[0]));
    kept -= 1u;
  }
  session->refusals[kept++] = refused;
  session->refused_count = kept;
  log_warn("Refused %s (key %s): its key is not allowed", refused.name,
           refused.key);
}

static void session_host_accept(void *context, const VkrNetAddress *address,
                                const uint8_t peer_key[32],
                                const uint8_t *credential,
                                uint32_t credential_size,
                                VkrNetAcceptResult *out_result) {
  (void)address;
  VkrEditorSession *session = context;
  if (session->restricted && !session_allowed(session, peer_key)) {
    session_note_refused(session, peer_key, credential, credential_size);
    out_result->code = VKR_NET_CLOSE_ACCESS_DENIED;
    return;
  }
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
  if (!link->based || MemCompare(digest, link->base_digest, 32u) != 0) {
    status = SESSION_WELCOME_SCENE;
  } else if (session->log_first > link->base_seq) {
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
  vkr_bit_write_varuint(&writer, link->base_seq);
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
  link->log_sent = (uint32_t)link->base_seq;
  /* The claims and tasks come before the history replays. */
  if (session->shared_size) {
    (void)session_send(session, link->connection, SESSION_EDIT_CHANNEL,
                       session->shared_bytes, session->shared_size);
  }
  VkrEditorSessionPeer peer = {.id = id};
  string_format(peer.name, sizeof(peer.name), "%s", name);
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
static void session_depot_base_message(VkrEditorSession *session,
                                       VkrBitReader *reader);
static void session_depot_published(VkrEditorSession *session,
                                    VkrBitReader *reader,
                                    const SessionLink *from);
static void session_host_send_base(VkrEditorSession *session,
                                   SessionLink *link);
static void session_send_hello(VkrEditorSession *session);
static bool8_t session_depot_ready(const VkrEditorSession *session);

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
  if (type == SESSION_MSG_PUBLISHED) {
    if (!link->welcomed) {
      vkr_net_core_close(vkr_net_host_core(session->host), link->connection,
                         VKR_NET_CLOSE_SCHEMA_VIOLATION, session->now);
      return;
    }
    session_depot_published(session, reader, link);
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
  uint8_t *bytes = size ? vkr_heap_alloc(size) : NULL;
  if (session->log_count == session->log_capacity) {
    const uint32_t capacity =
        session->log_capacity ? session->log_capacity * 2u : 256u;
    SessionBytes *grown =
        vkr_heap_realloc(session->log, capacity * sizeof(*grown));
    if (grown) {
      session->log = grown;
      session->log_capacity = capacity;
    }
  }
  if (!bytes || session->log_count == session->log_capacity) {
    /* The participants can no longer follow the session. */
    vkr_heap_free(bytes);
    log_error("The session could not record edit %llu; it ends",
              (unsigned long long)edit->seq);
    string_format(session->error, sizeof(session->error),
                  "Out of memory for the session's history.");
    session->leave_pending = true_v;
    return;
  }
  MemCopy(bytes, session->message.data, size);
  session->log[session->log_count++] = (SessionBytes){bytes, size};
  session->log_bytes += size;
  session->base_bytes += size;
}

static uint64_t session_history_budget(const VkrEditorSession *session) {
  return session->history_budget ? session->history_budget
                                 : SESSION_LOG_BYTES_MAX;
}

/* Sends each participant the history it has not received, and past the
   history budget drops what every participant has. */
static void session_host_send_log(VkrEditorSession *session) {
  uint32_t lowest = session->log_count;
  for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    SessionLink *link = &session->links[i];
    /* An editor between BASE and HELLO replays from its base. */
    if (link->live && link->based && !link->welcomed) {
      lowest = Min(lowest, (uint32_t)link->base_seq);
    }
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
  /* A depot host keeps what joiners replay from its newest base. */
  if (session->depot_phase == SESSION_DEPOT_READY &&
      session->rebase != SESSION_REBASE_FAILED) {
    lowest = Min(lowest, (uint32_t)session->base_seq);
  }
  while (session->log_bytes > session_history_budget(session) &&
         session->log_first < lowest) {
    SessionBytes *entry = &session->log[session->log_first++];
    session->log_bytes -= entry->size;
    vkr_heap_free(entry->data);
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
  string_format_v(session->error, sizeof(session->error), format, arguments);
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
    const uint64_t base = vkr_bit_read_varuint(reader);
    const uint64_t log = vkr_bit_read_varuint(reader);
    if (!vkr_bit_reader_at_end(reader) || session->welcomed || base > log) {
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
      /* The base already holds the edits up to its sequence. */
      session->apply_seq = base + 1u;
      session->receive_seq = base + 1u;
      log_info("Joined the session as participant %u; replaying %llu edits", id,
               (unsigned long long)(log - base));
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
  case SESSION_MSG_BASE:
    session_depot_base_message(session, reader);
    return;
  case SESSION_MSG_PUBLISHED:
    session_depot_published(session, reader, NULL);
    return;
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
  /* The host tells each participant its depot base first; a participant
     greets once it holds that base (session_depot_base_message). */
  if (session->mode == VKR_EDITOR_SESSION_HOST) {
    SessionLink *link = session_link(session, connection);
    if (link) {
      link->opened = true_v;
      session_host_send_base(session, link);
    }
  }
}

static void session_send_hello(VkrEditorSession *session) {
  uint8_t bytes[128];
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  vkr_bit_write(&writer, SESSION_MSG_HELLO, 4u);
  session_write_text(&writer, session->name);
  vkr_bit_write_bytes(&writer, session->digest, 32u);
  const uint32_t size = vkr_bit_writer_finish(&writer);
  if (!size || !session_send(session, session->connection, SESSION_EDIT_CHANNEL,
                             bytes, size)) {
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

/* Takes the host thread's lock for a public call that reaches the
   network; nested calls only count. */
static void session_lock(VkrEditorSession *session) {
  if (session->lock_depth++ == 0u && session->net_thread) {
    vkr_net_host_thread_lock(session->net_thread);
  }
}

static void session_unlock(VkrEditorSession *session) {
  if (session->lock_depth == 0u) {
    return;
  }
  if (--session->lock_depth == 0u && session->net_thread) {
    vkr_net_host_thread_unlock(session->net_thread);
  }
}

/* Starts pumping the host on its own thread, holding its lock at once when
   a public call is under way. */
static void session_start_thread(VkrEditorSession *session) {
  session->net_thread =
      vkr_net_host_thread_start(session->allocator, session->host, NULL, NULL);
  if (!session->net_thread) {
    log_warn("The session's network thread did not start; the editor pumps "
             "the network each frame");
    return;
  }
  if (session->lock_depth) {
    vkr_net_host_thread_lock(session->net_thread);
  }
}

static bool8_t session_start_network(VkrEditorSession *session,
                                     const VkrNetHostConfig *config) {
  if (!vkr_net_crypto_init()) {
    return false_v;
  }
  /* Messages of up to SESSION_MESSAGE_MAX in flight on every link. */
  if (!session->net_memory_ready) {
    if (!vkr_dmemory_create(MB(8), GB(1), &session->net_memory)) {
      return false_v;
    }
    session->net_allocator = (VkrAllocator){.ctx = &session->net_memory};
    vkr_dmemory_allocator_create(&session->net_allocator);
    session->net_memory_ready = true_v;
  }
  session->host = vkr_net_host_create(&session->net_allocator, config);
  session->net = session->host
                     ? vkr_net_session_create(&session->net_allocator,
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
  if (!vkr_net_session_register(session->net, &service)) {
    return false_v;
  }
  session_start_thread(session);
  return true_v;
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
  if (session->net_thread ? vkr_net_host_thread_failed(session->net_thread)
                          : !vkr_net_host_pump(session->host, 0)) {
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
            MemCopy(session->links[i].key, event.peer_key,
                    sizeof(session->links[i].key));
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
      } else if (event.type == VKR_NET_EVENT_CLOSED &&
                 event.code == VKR_NET_CLOSE_ACCESS_DENIED) {
        session_fail(session,
                     "The host does not allow this editor; give the host "
                     "your key %s.",
                     session->identity);
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
  string_format(session->refused.message, sizeof(session->refused.message),
                "%s", message);
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
    string_format(edit.agent, sizeof(edit.agent), "%s", note->author);
    string_format(edit.label, sizeof(edit.label), "%s", note->label);
    edit.review = note->review;
    if (note->stage_count) {
      edit.stages = vkr_heap_alloc(note->stage_count * sizeof(*edit.stages));
      if (!edit.stages) {
        string_format(payload->error, sizeof(payload->error), "Out of memory.");
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
      string_format(payload->error, sizeof(payload->error), "Out of memory.");
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
    string_format(payload->error, sizeof(payload->error),
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
   of applying here, and requests the session cannot carry are refused.
   Loading and pinning world partition cells stays local: an edit carries
   the cells of the entities it names, and an editor loads a cell it lacks
   before applying the edit (session_inject). */
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
      edit->action != VKR_SCENE_EDIT_SAVE &&
      edit->action != VKR_SCENE_EDIT_PARTITION) {
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
        string_format(
            message, sizeof(message),
            "Edit %u: collision layers and physics batches travel in a "
            "collaborative session only as edits of their own.",
            i);
        session_answer_batch(session, frame, token, message);
        return;
      }
      if (!vkr_net_scene_edit_supported(&payload.items[i].request)) {
        char message[192];
        string_format(message, sizeof(message),
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
      session_ready(session) && session_depot_ready(session) &&
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
        vkr_heap_realloc(session->terrain, capacity * sizeof(*grown));
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
    uint16_t *heights =
        field ? vkr_heap_alloc(samples * sizeof(uint16_t)) : NULL;
    uint32_t *weights =
        heights ? vkr_heap_alloc(samples * sizeof(uint32_t)) : NULL;
    if (!weights) {
      vkr_heap_free(heights);
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
    vkr_heap_free(heights);
    vkr_heap_free(weights);
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
  edit->results = size ? vkr_heap_alloc(size) : NULL;
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
      string_format(error, capacity, "A new terrain has no place here.");
      return false_v;
    }
    const FilePath path = {
        .path = string8_create_from_cstr((const uint8_t *)absolute,
                                         string_length(absolute)),
        .type = FILE_PATH_TYPE_ABSOLUTE};
    if (!file_exists(&path) &&
        !vkr_scene_terrain_stage(absolute, stage->cells, stage->spacing,
                                 stage->height_min, stage->height_max,
                                 stage->height)) {
      string_format(error, capacity, "A new terrain could not be staged.");
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
      string_format(error, capacity, "Terrain results do not match the edit.");
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
      string_format(error, capacity,
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
      session->edit_physics = vkr_heap_alloc_zeroed(
          SESSION_PHYSICS_CHANGE_MAX, sizeof(*session->edit_physics));
    }
    const VkrNetSceneEditStorage storage = {
        .collision_layers = &session->edit_layers,
        .physics_changes = session->edit_physics,
        .physics_capacity =
            session->edit_physics ? SESSION_PHYSICS_CHANGE_MAX : 0u,
        .missing = &session->missing};
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
      string_format(error, capacity, "Out of memory.");
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
      string_format(error, capacity, "Malformed batch.");
      return false_v;
    }
    if (!session_reserve_items(session, (uint32_t)count)) {
      string_format(error, capacity, "Out of memory.");
      return false_v;
    }
    const VkrNetSceneEditStorage storage = {.missing = &session->missing};
    for (uint32_t i = 0u; i < count; ++i) {
      VkrSampleEditBatchItem *item = &session->items[i];
      if (!vkr_net_scene_edit_read(&reader, &scenes, &storage, &item->request,
                                   &item->entity_ref, &item->parent_ref, error,
                                   capacity)) {
        return false_v;
      }
      item->request.gesture = 0u;
    }
    if (!vkr_bit_reader_at_end(&reader)) {
      string_format(error, capacity, "Malformed batch.");
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
      string_format(error, capacity, "No batch %llu to revert.",
                    (unsigned long long)seq);
      return false_v;
    }
    *out_container = container;
    *frame->edit_batch = (VkrSampleEditBatchRequest){
        .token = slot_token, .container = container, .revert_group = group};
    return true_v;
  }
  default:
    string_format(error, capacity, "Unknown edit.");
    return false_v;
  }
}

/* The session sequence a REVERT edit reverts. */
static uint64_t session_revert_seq(const SessionEdit *edit) {
  VkrBitReader reader;
  vkr_bit_reader_init(&reader, edit->payload, edit->size);
  (void)vkr_bit_read(&reader, 16u);
  return vkr_bit_read_varuint(&reader);
}

/* Loads and pins the world partition cell the last placed edit lacked, so
   the edit applies on the next build; false when it lacked none or this
   editor already loaded that cell for it and it is still missing. */
static bool8_t session_load_missing_cell(VkrEditorSession *session,
                                         const VkrSampleUiFrame *frame) {
  const VkrNetSceneEditMissingCell missing = session->missing;
  const VkrNetSceneEditMissingCell requested = session->cell_requested;
  if (!missing.found) {
    return false_v;
  }
  if (requested.found && requested.world == missing.world &&
      requested.cell.x == missing.cell.x &&
      requested.cell.z == missing.cell.z) {
    session->cell_requested = (VkrNetSceneEditMissingCell){0};
    return false_v;
  }
  const VkrNetSceneEditScenes scenes = session_scenes(frame);
  if (missing.world != 0u || !scenes.containers[0]) {
    /* Only the primary scene streams cells (ADR-086). */
    return false_v;
  }
  *frame->scene_edit = (VkrSceneEditRequest){
      .action = VKR_SCENE_EDIT_PARTITION,
      .partition_cells = {missing.cell.x, missing.cell.z, missing.cell.x,
                          missing.cell.z}};
  session->cell_requested = missing;
  log_info("Loading cell %d,%d for a session edit", missing.cell.x,
           missing.cell.z);
  return true_v;
}

/* Puts the next queued edit, or a rollback, into the free request slots. */
static void session_inject(VkrEditorSession *session,
                           const VkrSampleUiFrame *frame) {
  if (session->inflight.active || session->leave_pending ||
      !session_depot_ready(session) || !frame->scene_edit ||
      !frame->edit_batch || frame->scene_edit->action != VKR_SCENE_EDIT_NONE ||
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
        MemCopy(session->speculative, session->speculative + 1,
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
      string_format(message, sizeof(message),
                    "The session host refused this edit (reason %u).",
                    edit.refusal);
      session_answer_batch(session, frame, edit.token, message);
      session_edit_release(&edit);
      return;
    }
    if (host && ((edit.kind == SESSION_EDIT_UNDO && !session->undo_depth) ||
                 (edit.kind == SESSION_EDIT_REDO && !session->redo_depth) ||
                 (edit.kind == SESSION_EDIT_REVERT &&
                  session_revert_seq(&edit) <= session->revert_floor))) {
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
    session->missing = (VkrNetSceneEditMissingCell){0};
    if (!session_place(session, frame, &edit, own, slot_token, &container,
                       &as_batch, error, sizeof(error))) {
      if (session_load_missing_cell(session, frame)) {
        session_queue_unpop(session, &edit);
        return;
      }
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
    session->cell_requested = (VkrNetSceneEditMissingCell){0};
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
  string_format(notice->author, sizeof(notice->author), "%s", edit->agent);
  string_format(notice->label, sizeof(notice->label), "%s", edit->label);
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
  string_format(notice->author, sizeof(notice->author), "%s", reverted->agent);
  string_format(notice->label, sizeof(notice->label), "%s", reverted->label);
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
  string_format(self.name, sizeof(self.name), "%s", session->name);
  string_format(self.tool, sizeof(self.tool), "%s", session->tool);
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
  self.has_cursor = session->has_cursor;
  self.cursor_origin = session->cursor_origin;
  self.cursor_direction = session->cursor_direction;
  self.cursor_distance = session->cursor_distance;
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
// Depot (ADR-106)
// =============================================================================

static bool8_t session_depot_ready(const VkrEditorSession *session) {
  return (session->depot_phase == SESSION_DEPOT_OFF ||
          session->depot_phase == SESSION_DEPOT_READY) &&
         session->rebase != SESSION_REBASE_SAVING;
}

/* Whether `path`, relative to the working copy, is one of the open scene's
   documents: the scene, its sidecar or its cell documents, which share the
   scene's name without ".json". Those travel as edits, so changing them
   alone publishes nothing. */
static bool8_t session_scene_document(const char *path, const char *stem,
                                      uint32_t stem_length) {
  return stem_length && string_length(path) >= stem_length &&
         MemCompare(path, stem, stem_length) == 0;
}

/* Automatic publish: whether a file outside the open scene's documents
   changed, and whether every such file has been quiet long enough. */
static bool8_t session_publish_wanted(VkrDepotWorkspace *workspace,
                                      const char *scene, bool8_t *out_quiet,
                                      char *error, uint32_t capacity) {
  *out_quiet = true_v;
  char absolute[1024];
  char stem[1024] = {0};
  uint32_t stem_length = 0u;
  const uint32_t root_length = (uint32_t)string_length(workspace->root);
  if (scene[0] && vkr_bakery_path_absolute(scene, absolute, sizeof(absolute)) &&
      string_length(absolute) > root_length + 1u &&
      MemCompare(absolute, workspace->root, root_length) == 0) {
    string_format(stem, sizeof(stem), "%s", absolute + root_length + 1u);
    for (char *c = stem; *c; ++c) {
      *c = *c == '\\' ? '/' : *c;
    }
    stem_length = (uint32_t)string_length(stem);
    if (stem_length > 5u && string_equals(stem + stem_length - 5u, ".json")) {
      stem_length -= 5u;
    }
  }
  VkrDepotChangeList changes = {0};
  if (!vkr_depot_status(workspace, &changes, error, capacity)) {
    return false_v;
  }
  const int64_t now = vkr_bakery_unix_seconds();
  bool8_t wanted = false_v;
  for (uint32_t i = 0u; i < changes.count; ++i) {
    const VkrDepotChangeEntry *entry = &changes.entries[i];
    if (session_scene_document(entry->path, stem, stem_length)) {
      continue;
    }
    wanted = true_v;
    char file[1024];
    VkrBakeryStat stat;
    if (entry->change != VKR_DEPOT_CHANGE_DELETED &&
        vkr_bakery_path_join(file, sizeof(file), workspace->root,
                             entry->path) &&
        vkr_bakery_stat(file, &stat) && stat.exists &&
        now - stat.mtime_ns / 1000000000 < SESSION_PUBLISH_QUIET_SECONDS) {
      *out_quiet = false_v;
    }
  }
  vkr_depot_change_list_free(&changes);
  return wanted;
}

static void *session_depot_worker(void *argument) {
  SessionDepotJob *job = argument;
  VkrDMemory memory;
  VkrAllocator allocator = {0};
  const bool8_t memory_ok = vkr_dmemory_create(MB(4), MB(256), &memory);
  job->ok = false_v;
  if (memory_ok) {
    allocator.ctx = &memory;
    vkr_dmemory_allocator_create(&allocator);
  }
  VkrDepotWorkspace workspace;
  if (!memory_ok) {
    string_format(job->error, sizeof(job->error), "out of memory");
  } else if (!vkr_depot_workspace_open(&workspace, job->directory,
                                       vkr_depot_zstd_codec())) {
    string_format(job->error, sizeof(job->error), "no depot working copy at %s",
                  job->directory);
  } else {
    /* An automatic publish that finds nothing to publish, or a file still
       being written, ends here; the next poll looks again. */
    bool8_t quiet = true_v;
    job->skipped = job->automatic && job->kind == SESSION_DEPOT_COMMIT &&
                   (!session_publish_wanted(&workspace, job->scene, &quiet,
                                            job->error, sizeof(job->error)) ||
                    !quiet);
    /* A commit snapshots before it connects: hashing a large scene outlasts
       the idle timeout of an open connection. */
    const VkrDepotId parent = job->from_base ? workspace.base : job->parent;
    VkrDepotPendingCommit pending = {0};
    const bool8_t snapshot =
        !job->skipped &&
        (job->kind != SESSION_DEPOT_COMMIT ||
         vkr_depot_commit_snapshot(&workspace, &parent, job->message, &pending,
                                   job->error, sizeof(job->error)));
    const VkrDepotConnectConfig connect = {.address = workspace.remote,
                                           .server_key = workspace.server_key,
                                           .store = &workspace.store};
    VkrDepotConnection *connection =
        snapshot ? vkr_depot_connect(&allocator, &connect, NULL, job->error,
                                     sizeof(job->error))
                 : NULL;
    VkrDepotClient *client = vkr_depot_connection_client(connection);
    if (client && job->kind == SESSION_DEPOT_COMMIT) {
      job->ok = vkr_depot_commit_push(&workspace, client, job->branch, &parent,
                                      &job->expected, &pending, job->error,
                                      sizeof(job->error));
      job->commit = pending.commit;
    } else if (client) {
      uint32_t kept = 0u;
      job->ok = vkr_depot_fetch_checkout(&workspace, client, &job->commit,
                                         VKR_DEPOT_CONFLICT_FAIL, &kept, NULL,
                                         job->error, sizeof(job->error));
    }
    job->ok = job->ok || (job->skipped && !job->error[0]);
    vkr_depot_pending_commit_free(&pending);
    vkr_depot_disconnect(connection);
    vkr_depot_workspace_close(&workspace);
  }
  if (memory_ok) {
    vkr_dmemory_allocator_destroy(&allocator);
  }
  vkr_atomic_bool_store(&job->done, true_v, VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

static bool8_t session_depot_start(VkrEditorSession *session, uint8_t kind);

/* Publishes files this editor wrote without being asked: every few seconds,
   while no other depot step runs or waits, a worker looks for changed files
   outside the open scene's documents and, once they are quiet, commits the
   working copy as session.publish does. */
static void session_publish_poll(VkrEditorSession *session,
                                 const VkrSampleUiFrame *frame) {
  SessionDepotJob *job = &session->job;
  if (job->running || session->fetch_count ||
      session->depot_phase != SESSION_DEPOT_READY ||
      session->rebase != SESSION_REBASE_NONE ||
      (session->mode == VKR_EDITOR_SESSION_PARTICIPANT && !session->welcomed) ||
      session->now - session->publish_polled_at < SESSION_PUBLISH_POLL_US) {
    return;
  }
  session->publish_polled_at = session->now;
  job->from_base = false_v;
  job->automatic = true_v;
  job->publish = true_v;
  job->parent = session->depot_head;
  job->expected = session->depot_head;
  string_format(job->branch, sizeof(job->branch), "%s", session->depot_branch);
  string_format(job->message, sizeof(job->message), "Files from %s",
                session->name);
  string_format(job->scene, sizeof(job->scene), "%.*s",
                (int)frame->scene_path.length,
                (const char *)frame->scene_path.str);
  if (!session_depot_start(session, SESSION_DEPOT_COMMIT)) {
    log_warn("An automatic depot publish could not start");
  }
}

/* Starts the step the job fields describe. */
static bool8_t session_depot_start(VkrEditorSession *session, uint8_t kind) {
  SessionDepotJob *job = &session->job;
  job->kind = kind;
  job->ok = false_v;
  job->error[0] = '\0';
  string_format(job->directory, sizeof(job->directory), "%s",
                session->depot_directory);
  vkr_atomic_bool_store(&job->done, false_v, VKR_MEMORY_ORDER_RELAXED);
  if (!vkr_thread_create(session->allocator, &job->thread, session_depot_worker,
                         job)) {
    return false_v;
  }
  job->running = true_v;
  return true_v;
}

/* Joins a finished step; false while one runs or none ran. */
static bool8_t session_depot_finished(VkrEditorSession *session) {
  SessionDepotJob *job = &session->job;
  if (!job->running ||
      !vkr_atomic_bool_load(&job->done, VKR_MEMORY_ORDER_ACQUIRE)) {
    return false_v;
  }
  (void)vkr_thread_join(job->thread);
  (void)vkr_thread_destroy(session->allocator, &job->thread);
  job->running = false_v;
  return true_v;
}

/* The directory of the open scene when a depot working copy holds it. */
static bool8_t session_depot_find(const VkrSampleUiFrame *frame, char *out,
                                  uint32_t capacity) {
  char directory[1024];
  string_format(directory, sizeof(directory), "%.*s",
                (int)frame->scene_path.length,
                (const char *)frame->scene_path.str);
  char *slash = string_get_last_char_occurrence(directory, '/');
  char *backslash = string_get_last_char_occurrence(directory, '\\');
  if (backslash && (!slash || backslash > slash)) {
    slash = backslash;
  }
  if (slash) {
    *slash = '\0';
  } else {
    string_format(directory, sizeof(directory), ".");
  }
  VkrDepotWorkspace workspace;
  if (!directory[0] || !vkr_depot_workspace_open(&workspace, directory,
                                                 vkr_depot_zstd_codec())) {
    return false_v;
  }
  vkr_depot_workspace_close(&workspace);
  string_format(out, capacity, "%s", directory);
  return true_v;
}

static bool8_t session_journals_saved(const VkrSampleUiFrame *frame) {
  const VkrSceneEditState *journals[2u + VKR_SCENE_ADDITIVE_MAX] = {
      frame->scene ? frame->edits : NULL,
      frame->world ? frame->world_edits : NULL};
  for (uint32_t i = 0u; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    journals[2u + i] = frame->additive[i] ? frame->additive_edits[i] : NULL;
  }
  for (uint32_t i = 0u; i < ArrayCount(journals); ++i) {
    if (journals[i] && journals[i]->revision != journals[i]->saved_revision) {
      return false_v;
    }
  }
  return true_v;
}

/* BASE: whether the session has a depot, then its branch, base commit and
   newest commit. */
static void session_host_send_base(VkrEditorSession *session,
                                   SessionLink *link) {
  if (!link->opened || link->based || !session_depot_ready(session)) {
    return;
  }
  uint8_t bytes[160];
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  vkr_bit_write(&writer, SESSION_MSG_BASE, 4u);
  const bool8_t depot = session->depot_phase == SESSION_DEPOT_READY;
  vkr_bit_write(&writer, depot, 1u);
  if (depot) {
    /* A newer base sits beside the session's commits; the newest commit
       is fetched only when something was published since. */
    const bool8_t published =
        !vkr_depot_id_equal(&session->depot_head, &session->depot_base_parent);
    session_write_text(&writer, session->depot_branch);
    vkr_bit_write_bytes(&writer, session->depot_base.bytes, 32u);
    vkr_bit_write_bytes(
        &writer,
        published ? session->depot_head.bytes : session->depot_base.bytes, 32u);
  }
  session_host_send_small(session, link->connection, bytes,
                          vkr_bit_writer_finish(&writer));
  link->based = true_v;
  link->base_seq = session->base_seq;
  MemCopy(link->base_digest, session->digest, sizeof(link->base_digest));
}

static void session_depot_base_message(VkrEditorSession *session,
                                       VkrBitReader *reader) {
  const bool8_t depot = (bool8_t)vkr_bit_read(reader, 1u);
  if (session->depot_phase != SESSION_DEPOT_WAITING) {
    session_fail(session, "The host sent its base twice.");
    return;
  }
  if (!depot) {
    session->depot_phase = SESSION_DEPOT_OFF;
    session_send_hello(session);
    return;
  }
  const uint8_t *base = NULL;
  const uint8_t *head = NULL;
  if (!session_read_text(reader, session->depot_branch,
                         sizeof(session->depot_branch)) ||
      !(base = vkr_bit_read_bytes(reader, 32u)) ||
      !(head = vkr_bit_read_bytes(reader, 32u)) ||
      !vkr_bit_reader_at_end(reader)) {
    session_fail(session, "The host sent a malformed base.");
    return;
  }
  /* The base holds the scene the history replays onto; files published
     since follow as a fetch once this editor is in. */
  MemCopy(session->depot_base.bytes, base, 32u);
  MemCopy(session->depot_head.bytes, base, 32u);
  VkrDepotId newest;
  MemCopy(newest.bytes, head, 32u);
  if (!vkr_depot_id_equal(&newest, &session->depot_base)) {
    session->fetches[session->fetch_count++] = newest;
  }
  VkrDepotWorkspace workspace;
  if (!session->depot_directory[0] ||
      !vkr_depot_workspace_open(&workspace, session->depot_directory,
                                vkr_depot_zstd_codec())) {
    log_warn("This scene is in no depot working copy; joining by the scene "
             "alone");
    session->depot_phase = SESSION_DEPOT_OFF;
    session_send_hello(session);
    return;
  }
  const bool8_t current =
      vkr_depot_id_equal(&workspace.base, &session->depot_head);
  vkr_depot_workspace_close(&workspace);
  if (current) {
    session->depot_phase = SESSION_DEPOT_READY;
    session_send_hello(session);
    return;
  }
  session->job.commit = session->depot_head;
  if (session->job.running ||
      !session_depot_start(session, SESSION_DEPOT_CHECKOUT)) {
    session_fail(session, "The depot checkout could not start.");
    return;
  }
  session->depot_phase = SESSION_DEPOT_CHECKING_OUT;
  log_info("Checking out the session's base from the depot");
}

static uint32_t session_put_published(uint8_t *out, uint32_t capacity,
                                      const VkrDepotId *commit,
                                      uint8_t author) {
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, out, capacity);
  vkr_bit_write(&writer, SESSION_MSG_PUBLISHED, 4u);
  vkr_bit_write_bytes(&writer, commit->bytes, 32u);
  vkr_bit_write(&writer, author, 8u);
  return vkr_bit_writer_finish(&writer);
}

/* PUBLISHED: a commit to fetch; the host passes it on to the others. */
static void session_depot_published(VkrEditorSession *session,
                                    VkrBitReader *reader,
                                    const SessionLink *from) {
  const uint8_t *bytes = vkr_bit_read_bytes(reader, 32u);
  const uint8_t author = (uint8_t)vkr_bit_read(reader, 8u);
  if (!bytes || !vkr_bit_reader_at_end(reader)) {
    session_fail(session, "A malformed publish arrived.");
    return;
  }
  VkrDepotId commit;
  MemCopy(commit.bytes, bytes, 32u);
  if (session->fetch_count < SESSION_FETCH_MAX) {
    session->fetches[session->fetch_count++] = commit;
  } else {
    log_warn("Too many depot commits wait; pull the session branch later");
  }
  if (session->mode != VKR_EDITOR_SESSION_HOST) {
    return;
  }
  uint8_t message[48];
  const uint32_t size = session_put_published(message, sizeof(message), &commit,
                                              from ? from->id : author);
  for (uint32_t i = 0u; size && i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    const SessionLink *link = &session->links[i];
    if (link->live && link->welcomed && link != from) {
      session_host_send_small(session, link->connection, message, size);
    }
  }
}

static void session_depot_announce(VkrEditorSession *session) {
  uint8_t message[48];
  const uint32_t size = session_put_published(
      message, sizeof(message), &session->depot_head, session->self_id);
  if (!size) {
    return;
  }
  if (session->mode == VKR_EDITOR_SESSION_PARTICIPANT) {
    (void)session_send(session, session->connection, SESSION_EDIT_CHANNEL,
                       message, size);
    return;
  }
  for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    const SessionLink *link = &session->links[i];
    if (link->live && link->welcomed) {
      session_host_send_small(session, link->connection, message, size);
    }
  }
}

/* Past the history budget, a depot host saves the scene with edits held,
   then commits it as a new base for joiners. Undo, redo and revert stop at
   it, since an editor that joins from it lacks what came before. */
static void session_rebase_step(VkrEditorSession *session,
                                const VkrSampleUiFrame *frame) {
  SessionDepotJob *job = &session->job;
  if (session->rebase == SESSION_REBASE_NONE) {
    if (session->base_bytes > session_history_budget(session) &&
        !job->running && !session->leave_pending) {
      session->rebase = SESSION_REBASE_SAVING;
      session->rebase_frames = 0u;
      session->save_requested = false_v;
      log_info("The session's history passed %llu bytes; making a new base",
               (unsigned long long)session_history_budget(session));
    }
    return;
  }
  if (session->rebase != SESSION_REBASE_SAVING) {
    return;
  }
  if (++session->rebase_frames > SESSION_SAVE_FRAMES) {
    session->rebase = SESSION_REBASE_FAILED;
    log_warn("The scene did not save for a new session base; new editors "
             "cannot join once the history drops");
    return;
  }
  /* What applied so far, with no edit in flight or drag unrecorded. */
  if (session->inflight.active || frame->gizmo_edit_pending ||
      frame->scene_loading || frame->additive_loading || frame->world_loading) {
    return;
  }
  if (!session_journals_saved(frame)) {
    if (!session->save_requested && frame->scene_edit &&
        frame->scene_edit->action == VKR_SCENE_EDIT_NONE) {
      frame->scene_edit->action = VKR_SCENE_EDIT_SAVE;
      session->save_requested = true_v;
    }
    return;
  }
  session->rebase_seq = session->apply_seq - 1u;
  session->rebase_bytes = session->base_bytes;
  vkr_editor_session_digest(frame, session->rebase_digest);
  session->undo_depth = 0u;
  session->redo_depth = 0u;
  session->revert_floor = session->rebase_seq;
  job->from_base = false_v;
  job->publish = false_v;
  job->parent = session->depot_head;
  job->expected = session->depot_base_tip;
  string_format(job->branch, sizeof(job->branch), "%s",
                session->depot_base_branch);
  string_format(job->message, sizeof(job->message), "Session base at edit %llu",
                (unsigned long long)session->rebase_seq);
  if (!session_depot_start(session, SESSION_DEPOT_COMMIT)) {
    session->rebase = SESSION_REBASE_FAILED;
    log_warn("The commit of a new session base could not start");
    return;
  }
  session->rebase = SESSION_REBASE_COMMITTING;
}

static void session_rebase_finished(VkrEditorSession *session) {
  const SessionDepotJob *job = &session->job;
  if (!job->ok) {
    session->rebase = SESSION_REBASE_FAILED;
    log_warn("A new session base could not be committed: %s", job->error);
    return;
  }
  session->depot_base = job->commit;
  session->depot_base_tip = job->commit;
  session->depot_base_parent = job->parent;
  session->base_seq = session->rebase_seq;
  session->base_bytes -= session->rebase_bytes;
  MemCopy(session->digest, session->rebase_digest, sizeof(session->digest));
  session->rebase = SESSION_REBASE_NONE;
  char hex[VKR_DEPOT_ID_HEX];
  vkr_depot_id_hex(&session->depot_base, hex);
  log_info("New editors join from base %.12s at edit %llu on %s", hex,
           (unsigned long long)session->base_seq, session->depot_base_branch);
}

/* The depot's share of a build: the host saves and commits its base, a
   participant checks the base out and reloads, and everyone publishes and
   fetches session commits. */
static void session_depot_update(VkrEditorSession *session,
                                 const VkrSampleUiFrame *frame) {
  SessionDepotJob *job = &session->job;
  const bool8_t finished = session_depot_finished(session);
  char hex[VKR_DEPOT_ID_HEX];
  switch (session->depot_phase) {
  case SESSION_DEPOT_SAVING:
    if (session_journals_saved(frame)) {
      job->from_base = true_v;
      job->publish = false_v;
      MemZero(&job->expected, sizeof(job->expected));
      string_format(job->branch, sizeof(job->branch), "%s",
                    session->depot_branch);
      string_format(job->message, sizeof(job->message), "Session base by %s",
                    session->name);
      if (!session_depot_start(session, SESSION_DEPOT_COMMIT)) {
        session_fail(session, "The depot commit could not start.");
        return;
      }
      session->depot_phase = SESSION_DEPOT_BASING;
    } else if (!session->save_requested && frame->scene_edit &&
               frame->scene_edit->action == VKR_SCENE_EDIT_NONE) {
      /* The base holds the scene as saved, so the host saves first. */
      frame->scene_edit->action = VKR_SCENE_EDIT_SAVE;
      session->save_requested = true_v;
    } else if (++session->save_frames > SESSION_SAVE_FRAMES) {
      session_fail(session, "The scene did not save; hosting stopped.");
    }
    return;
  case SESSION_DEPOT_BASING:
    if (!finished) {
      return;
    }
    if (!job->ok) {
      session_fail(session, "The session base could not be committed: %s",
                   job->error);
      return;
    }
    session->depot_base = job->commit;
    session->depot_head = job->commit;
    session->depot_base_parent = job->commit;
    session->depot_phase = SESSION_DEPOT_READY;
    /* Newer bases go to `<branch>.base`, cut so the suffix fits. */
    string_format(session->depot_base_branch,
                  sizeof(session->depot_base_branch), "%.*s.base",
                  (int)(sizeof(session->depot_base_branch) - 6u),
                  session->depot_branch);
    /* Saved now; the scene is what the base holds. */
    vkr_editor_session_digest(frame, session->digest);
    vkr_depot_id_hex(&session->depot_head, hex);
    log_info("The session's base is %.12s on depot branch %s", hex,
             session->depot_branch);
    for (uint32_t i = 0u; i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
      if (session->links[i].live) {
        session_host_send_base(session, &session->links[i]);
      }
    }
    return;
  case SESSION_DEPOT_CHECKING_OUT:
    if (!finished) {
      return;
    }
    if (!job->ok) {
      session_fail(session, "The session's base could not be checked out: %s",
                   job->error);
      return;
    }
    session->depot_phase = SESSION_DEPOT_RELOADING;
    session->reload_requested = true_v;
    session->reload_generation = frame->scene_generation;
    return;
  case SESSION_DEPOT_RELOADING:
    if (session->reload_requested && frame->scene_edit &&
        frame->scene_edit->action == VKR_SCENE_EDIT_NONE) {
      frame->scene_edit->action = VKR_SCENE_EDIT_RELOAD;
      session->reload_requested = false_v;
    } else if (!session->reload_requested &&
               frame->scene_generation != session->reload_generation &&
               frame->scene && !frame->scene_loading && !frame->world_loading &&
               !frame->additive_loading) {
      vkr_editor_session_digest(frame, session->digest);
      session->depot_phase = SESSION_DEPOT_READY;
      session_send_hello(session);
    }
    return;
  case SESSION_DEPOT_READY:
    break;
  default:
    return;
  }
  if (finished && job->kind == SESSION_DEPOT_COMMIT &&
      session->rebase == SESSION_REBASE_COMMITTING) {
    session_rebase_finished(session);
  } else if (finished && job->kind == SESSION_DEPOT_COMMIT && job->automatic &&
             (job->skipped || !job->ok)) {
    /* A publish that lost a race to another editor's waits for that
       commit's fetch, then the next poll commits on top of it. */
    if (!job->ok) {
      log_warn("Automatic publishing to the depot failed: %s", job->error);
    }
  } else if (finished && job->kind == SESSION_DEPOT_COMMIT) {
    if (!job->ok) {
      log_warn("Publishing to the depot failed: %s", job->error);
      string_format(session->error, sizeof(session->error), "Publish: %s",
                    job->error);
    } else {
      session->depot_head = job->commit;
      vkr_depot_id_hex(&session->depot_head, hex);
      log_info("Published %.12s to depot branch %s", hex,
               session->depot_branch);
      session_depot_announce(session);
    }
  } else if (finished) {
    if (!job->ok) {
      log_warn("Fetching from the depot failed: %s", job->error);
      string_format(session->error, sizeof(session->error), "Fetch: %s",
                    job->error);
    } else {
      session->depot_head = job->commit;
      vkr_depot_id_hex(&session->depot_head, hex);
      log_info("Fetched %.12s from depot branch %s", hex,
               session->depot_branch);
    }
  }
  if (finished) {
    job->automatic = false_v;
  }
  if (session->mode == VKR_EDITOR_SESSION_HOST) {
    session_rebase_step(session, frame);
  }
  if (!job->running && session->fetch_count) {
    job->commit = session->fetches[0];
    session->fetch_count -= 1u;
    MemCopy(session->fetches, session->fetches + 1,
            session->fetch_count * sizeof(session->fetches[0]));
    if (!session_depot_start(session, SESSION_DEPOT_CHECKOUT)) {
      log_warn("A depot fetch could not start");
    }
  }
  session_publish_poll(session, frame);
}

bool8_t vkr_editor_session_publish_files(VkrEditorSession *session,
                                         const char *message, char *error,
                                         uint32_t capacity) {
  if (!session || session->depot_phase != SESSION_DEPOT_READY ||
      (session->mode == VKR_EDITOR_SESSION_PARTICIPANT && !session->welcomed)) {
    string_format(error, capacity,
                  "Publishing needs a joined session on a depot working copy.");
    return false_v;
  }
  SessionDepotJob *job = &session->job;
  if (job->running) {
    string_format(error, capacity, "Another depot step is running; try again.");
    return false_v;
  }
  job->from_base = false_v;
  job->automatic = false_v;
  job->publish = true_v;
  job->parent = session->depot_head;
  job->expected = session->depot_head;
  string_format(job->branch, sizeof(job->branch), "%s", session->depot_branch);
  string_format(job->message, sizeof(job->message), "%s",
                message && message[0] ? message : "Session files");
  if (!session_depot_start(session, SESSION_DEPOT_COMMIT)) {
    string_format(error, capacity, "The depot commit could not start.");
    return false_v;
  }
  return true_v;
}

// =============================================================================
// Lifetime
// =============================================================================

VkrEditorSession *vkr_editor_session_create(VkrAllocator *allocator) {
  VkrEditorSession *session =
      vkr_heap_alloc_zeroed(1u, sizeof(VkrEditorSession));
  if (session) {
    session->allocator = allocator;
    string_format(session->form_name, sizeof(session->form_name), "editor");
    string_format(session->form_bind, sizeof(session->form_bind), "[::]:%u",
                  VKR_EDITOR_SESSION_PORT);
  }
  return session;
}

static void session_reset(VkrEditorSession *session) {
  /* The host thread stops first, without its lock; the closes below then
     leave from this thread. */
  if (session->net_thread) {
    if (session->lock_depth) {
      vkr_net_host_thread_unlock(session->net_thread);
    }
    vkr_net_host_thread_stop(session->net_thread);
    session->net_thread = NULL;
  }
  /* A depot step owns the job until it ends. */
  if (session->job.running) {
    (void)vkr_thread_join(session->job.thread);
    (void)vkr_thread_destroy(session->allocator, &session->job.thread);
    session->job.running = false_v;
  }
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
  /* The thread stopped and the host is gone: nothing uses it any more. */
  if (session->net_memory_ready) {
    vkr_dmemory_allocator_destroy(&session->net_allocator);
  }
  for (uint32_t i = session->log_first; i < session->log_count; ++i) {
    vkr_heap_free(session->log[i].data);
  }
  vkr_heap_free(session->log);
  for (uint32_t i = 0u; i < session->queue_count; ++i) {
    session_edit_release(&session->queue[session->queue_head + i]);
  }
  vkr_heap_free(session->queue);
  vkr_heap_free(session->groups);
  vkr_heap_free(session->items);
  vkr_heap_free(session->terrain);
  vkr_heap_free(session->results.data);
  vkr_heap_free(session->edit_physics);
  vkr_heap_free(session->payload.data);
  vkr_heap_free(session->message.data);
  session_edit_release(&session->inflight.edit);
  vkr_heap_free(session->shared_bytes);
  VkrAllocator *allocator = session->allocator;
  const uint64_t epoch = session->epoch;
  char error[sizeof(session->error)];
  MemCopy(error, session->error, sizeof(error));
  const bool8_t restricted = session->restricted;
  const uint32_t allowed_count = session->allowed_count;
  uint8_t allowed[VKR_EDITOR_SESSION_ALLOWED_MAX][32];
  MemCopy(allowed, session->allowed, sizeof(allowed));
  const uint32_t refused_count = session->refused_count;
  VkrEditorSessionRefused refused[VKR_EDITOR_SESSION_REFUSED_MAX];
  MemCopy(refused, session->refusals, sizeof(refused));
  char identity[sizeof(session->identity)];
  MemCopy(identity, session->identity, sizeof(identity));
  const bool8_t identity_read = session->identity_read;
  char form[4][128];
  string_format(form[0], sizeof(form[0]), "%s", session->form_name);
  string_format(form[1], sizeof(form[1]), "%s", session->form_bind);
  string_format(form[2], sizeof(form[2]), "%s", session->form_address);
  string_format(form[3], sizeof(form[3]), "%s", session->form_key);
  MemZero(session, sizeof(*session));
  session->allocator = allocator;
  session->epoch = epoch;
  MemCopy(session->error, error, sizeof(error));
  string_format(session->form_name, sizeof(session->form_name), "%s", form[0]);
  string_format(session->form_bind, sizeof(session->form_bind), "%s", form[1]);
  string_format(session->form_address, sizeof(session->form_address), "%s",
                form[2]);
  string_format(session->form_key, sizeof(session->form_key), "%s", form[3]);
  session->restricted = restricted;
  session->allowed_count = allowed_count;
  MemCopy(session->allowed, allowed, sizeof(allowed));
  session->refused_count = refused_count;
  MemCopy(session->refusals, refused, sizeof(refused));
  MemCopy(session->identity, identity, sizeof(identity));
  session->identity_read = identity_read;
}

/* This editor's identity key pair (the depot identity, made when missing)
   and its public half in hexadecimal; false when it cannot be read or
   made. */
static bool8_t session_identity(VkrEditorSession *session,
                                VkrNetKeyPair *out_keys) {
  VkrNetKeyPair keys;
  if (!vkr_depot_identity(&keys, true_v)) {
    session->identity_read = true_v;
    return false_v;
  }
  session_hex(keys.public_key, 32u, session->identity);
  session->identity_read = true_v;
  if (out_keys) {
    *out_keys = keys;
  }
  vkr_net_random(keys.secret_key, sizeof(keys.secret_key));
  return true_v;
}

void vkr_editor_session_destroy(VkrEditorSession *session) {
  if (!session) {
    return;
  }
  session_reset(session);
  vkr_heap_free(session);
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
    string_format(error, capacity, "Leave the current session first.");
    return false_v;
  }
  if (!frame->scene || frame->scene_loading || frame->additive_loading ||
      frame->world_loading) {
    string_format(error, capacity, "Open a scene and let it load first.");
    return false_v;
  }
  session->error[0] = '\0';
  string_format(session->name, sizeof(session->name), "%s",
                name && name[0] ? name : fallback);
  session->scene_generation = frame->scene_generation;
  session->epoch += 1u;
  session->apply_seq = 1u;
  session->receive_seq = 1u;
  vkr_editor_session_digest(frame, session->digest);
  return true_v;
}

static bool8_t session_host_locked(VkrEditorSession *session,
                                   const VkrSampleUiFrame *frame,
                                   const char *bind, const char *name,
                                   char *error, uint32_t capacity) {
  if (!session_begin(session, frame, name, "host", error, capacity)) {
    return false_v;
  }
  VkrNetHostConfig config = {
      .socket = {.receive_buffer = 4u << 20, .send_buffer = 4u << 20}};
  if (!session_address(bind && bind[0] ? bind : "[::]", &config.bind)) {
    string_format(error, capacity, "Invalid address %s.", bind);
    return false_v;
  }
  if (!vkr_net_crypto_init()) {
    string_format(error, capacity, "The network cryptography did not start.");
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
    string_format(error, capacity, "Could not listen on %s.",
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
  /* A scene in a depot working copy hosts from a commit of it, on a branch
     of the session's own. */
  if (session_depot_find(frame, session->depot_directory,
                         sizeof(session->depot_directory))) {
    /* Branch names take letters, digits, '.', '_' and '-'. */
    string_format(session->depot_branch, sizeof(session->depot_branch),
                  "session-%s-%llu", session->name,
                  (unsigned long long)vkr_bakery_unix_seconds());
    for (char *c = session->depot_branch; *c; ++c) {
      const bool8_t kept =
          (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
          (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-';
      *c = kept ? *c : '_';
    }
    session->depot_phase = SESSION_DEPOT_SAVING;
    log_info("Saving and committing the scene to depot branch %s",
             session->depot_branch);
  }
  return true_v;
}

bool8_t vkr_editor_session_host(VkrEditorSession *session,
                                const VkrSampleUiFrame *frame, const char *bind,
                                const char *name, char *error,
                                uint32_t capacity) {
  if (!session) {
    return false_v;
  }
  session_lock(session);
  const bool8_t ok =
      session_host_locked(session, frame, bind, name, error, capacity);
  session_unlock(session);
  return ok;
}

static bool8_t session_join_locked(VkrEditorSession *session,
                                   const VkrSampleUiFrame *frame,
                                   const char *address, const char *key_hex,
                                   const char *name, char *error,
                                   uint32_t capacity) {
  uint8_t server_key[32];
  VkrNetAddress server;
  if (!address || !session_address(address, &server)) {
    string_format(error, capacity, "Invalid address.");
    return false_v;
  }
  if (!session_parse_hex(key_hex, server_key, sizeof(server_key))) {
    string_format(error, capacity, "The key must be 64 hexadecimal digits.");
    return false_v;
  }
  if (!session_begin(session, frame, name, "guest", error, capacity)) {
    return false_v;
  }
  VkrNetHostConfig config = {
      .socket = {.receive_buffer = 4u << 20, .send_buffer = 4u << 20}};
  config.bind = vkr_net_address_any((VkrNetAddressFamily)server.family, 0u);
  if (!vkr_net_crypto_init()) {
    string_format(error, capacity, "The network cryptography did not start.");
    return false_v;
  }
  /* The identity key is what a restricted host allows; the name goes as
     the credential so the host can tell who it refused. */
  if (!session_identity(session, &config.core.static_keys)) {
    log_warn("No identity key; joining with a fresh key, which a restricted "
             "host refuses");
    vkr_net_keypair_generate(&config.core.static_keys);
  }
  session->mode = VKR_EDITOR_SESSION_PARTICIPANT;
  if (!session_start_network(session, &config) ||
      !vkr_net_core_connect(vkr_net_host_core(session->host), &server,
                            server_key, (const uint8_t *)session->name,
                            (uint32_t)string_length(session->name),
                            vkr_net_host_now(session->host),
                            &session->connection)) {
    session_reset(session);
    string_format(error, capacity, "Could not start the connection.");
    return false_v;
  }
  vkr_net_random(config.core.static_keys.secret_key,
                 sizeof(config.core.static_keys.secret_key));
  string_format(session->address, sizeof(session->address), "%s", address);
  session_hex(server_key, 32u, session->key);
  (void)session_depot_find(frame, session->depot_directory,
                           sizeof(session->depot_directory));
  session->depot_phase = SESSION_DEPOT_WAITING;
  log_info("Joining the collaborative session at %s", address);
  return true_v;
}

bool8_t vkr_editor_session_join(VkrEditorSession *session,
                                const VkrSampleUiFrame *frame,
                                const char *address, const char *key_hex,
                                const char *name, char *error,
                                uint32_t capacity) {
  if (!session) {
    return false_v;
  }
  session_lock(session);
  const bool8_t ok = session_join_locked(session, frame, address, key_hex, name,
                                         error, capacity);
  session_unlock(session);
  return ok;
}

static void session_update_locked(VkrEditorSession *session,
                                  const VkrSampleUiFrame *frame) {
  if (!session || session->mode == VKR_EDITOR_SESSION_NONE) {
    return;
  }
  /* A participant's reload of the session's base replaces the scene. */
  if (frame->scene_generation != session->scene_generation &&
      session->mode == VKR_EDITOR_SESSION_PARTICIPANT &&
      (session->depot_phase == SESSION_DEPOT_CHECKING_OUT ||
       session->depot_phase == SESSION_DEPOT_RELOADING)) {
    session->scene_generation = frame->scene_generation;
  }
  if (frame->scene_generation != session->scene_generation) {
    string_format(session->error, sizeof(session->error),
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
    session_depot_update(session, frame);
  }
  if (!session->leave_pending) {
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

void vkr_editor_session_update(VkrEditorSession *session,
                               const VkrSampleUiFrame *frame) {
  if (!session || session->mode == VKR_EDITOR_SESSION_NONE) {
    return;
  }
  session_lock(session);
  session_update_locked(session, frame);
  session_unlock(session);
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
  string_format(out_status->address, sizeof(out_status->address), "%s",
                session->address);
  string_format(out_status->key, sizeof(out_status->key), "%s", session->key);
  string_format(out_status->error, sizeof(out_status->error), "%s",
                session->error);
  out_status->peer_count = session->peer_count;
  if (session->depot_phase != SESSION_DEPOT_OFF) {
    string_format(out_status->depot_branch, sizeof(out_status->depot_branch),
                  "%s", session->depot_branch);
    vkr_depot_id_hex(&session->depot_head, out_status->depot_head);
  }
  out_status->depot_busy = session->job.running;
  MemCopy(out_status->peers, session->peers,
          session->peer_count * sizeof(session->peers[0]));
  string_format(out_status->identity, sizeof(out_status->identity), "%s",
                session->identity);
  out_status->restricted = session->restricted;
  out_status->allowed_count = session->allowed_count;
  for (uint32_t i = 0u; i < session->allowed_count; ++i) {
    session_hex(session->allowed[i], 32u, out_status->allowed[i]);
  }
  out_status->refused_count = session->refused_count;
  MemCopy(out_status->refused, session->refusals, sizeof(session->refusals));
}

static bool8_t session_allow_key(VkrEditorSession *session,
                                 const uint8_t key[32], bool8_t allowed);

void vkr_editor_session_restrict(VkrEditorSession *session,
                                 bool8_t restricted) {
  if (!session || session->restricted == restricted) {
    return;
  }
  session->restricted = restricted;
  /* The editors already in the session stay. */
  for (uint32_t i = 0u; restricted && i < VKR_EDITOR_SESSION_PEERS_MAX; ++i) {
    if (session->links[i].live) {
      (void)session_allow_key(session, session->links[i].key, true_v);
    }
  }
  log_info("Session joining is %s", restricted ? "restricted to allowed keys"
                                               : "open to the session key");
}

static bool8_t session_allow_key(VkrEditorSession *session,
                                 const uint8_t key[32], bool8_t allowed) {
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < session->allowed_count; ++i) {
    if (MemCompare(session->allowed[i], key, 32u) != 0) {
      MemCopy(session->allowed[kept++], session->allowed[i], 32u);
    }
  }
  session->allowed_count = kept;
  if (allowed) {
    if (kept == VKR_EDITOR_SESSION_ALLOWED_MAX) {
      return false_v;
    }
    MemCopy(session->allowed[session->allowed_count++], key, 32u);
    /* An allowed editor leaves the refused list. */
    char hex[65];
    session_hex(key, 32u, hex);
    uint32_t left = 0u;
    for (uint32_t i = 0u; i < session->refused_count; ++i) {
      if (!string_equals(session->refusals[i].key, hex)) {
        session->refusals[left++] = session->refusals[i];
      }
    }
    session->refused_count = left;
    return true_v;
  }
  for (uint32_t i = 0u; session->host && session->restricted &&
                        session->mode == VKR_EDITOR_SESSION_HOST &&
                        i < VKR_EDITOR_SESSION_PEERS_MAX;
       ++i) {
    if (session->links[i].live &&
        MemCompare(session->links[i].key, key, 32u) == 0) {
      vkr_net_core_close(vkr_net_host_core(session->host),
                         session->links[i].connection,
                         VKR_NET_CLOSE_ACCESS_DENIED, session->now);
    }
  }
  return true_v;
}

static bool8_t session_allow_locked(VkrEditorSession *session,
                                    const char *key_hex, bool8_t allowed) {
  uint8_t key[32];
  return session && key_hex && session_parse_hex(key_hex, key, sizeof(key)) &&
         session_allow_key(session, key, allowed);
}

bool8_t vkr_editor_session_allow(VkrEditorSession *session, const char *key_hex,
                                 bool8_t allowed) {
  if (!session) {
    return false_v;
  }
  session_lock(session);
  const bool8_t ok = session_allow_locked(session, key_hex, allowed);
  session_unlock(session);
  return ok;
}

// =============================================================================
// Agent federation
// =============================================================================

void vkr_editor_session_author(const VkrEditorSession *session,
                               const char *local, char *out,
                               uint32_t capacity) {
  char copy[VKR_EDITOR_AUTHOR_CAPACITY];
  string_format(copy, sizeof(copy), "%s", local);
  if (session && session->mode != VKR_EDITOR_SESSION_NONE && copy[0]) {
    string_format(out, capacity, "%s@%s", copy, session->name);
  } else {
    string_format(out, capacity, "%s", copy);
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
  string_format(note->author, sizeof(note->author), "%s", author ? author : "");
  string_format(note->label, sizeof(note->label), "%s", label ? label : "");
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
  string_format(stage->path, sizeof(stage->path), "%s", relative);
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

static uint64_t session_ask_locked(VkrEditorSession *session,
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

uint64_t vkr_editor_session_ask(VkrEditorSession *session,
                                const VkrEditorSessionAsk *ask) {
  if (!session) {
    return 0u;
  }
  session_lock(session);
  const uint64_t id = session_ask_locked(session, ask);
  session_unlock(session);
  return id;
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

static void session_reply_locked(VkrEditorSession *session,
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

void vkr_editor_session_reply(VkrEditorSession *session,
                              const VkrEditorSessionAsk *ask,
                              const VkrEditorSessionAnswer *answer) {
  if (!session) {
    return;
  }
  session_lock(session);
  session_reply_locked(session, ask, answer);
  session_unlock(session);
}

static void session_publish_locked(VkrEditorSession *session,
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
  uint8_t *bytes = size ? vkr_heap_realloc(session->shared_bytes, size) : NULL;
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

void vkr_editor_session_publish(VkrEditorSession *session,
                                const VkrEditorClaim *claims,
                                uint32_t claim_count,
                                const VkrEditorTask *tasks,
                                uint32_t task_count) {
  if (!session) {
    return;
  }
  session_lock(session);
  session_publish_locked(session, claims, claim_count, tasks, task_count);
  session_unlock(session);
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
    /* The pointer's ray, ending in a cross where it meets something. */
    if (peer->has_cursor) {
      const float32_t length = peer->cursor_distance > 0.0f
                                   ? peer->cursor_distance
                                   : SESSION_CURSOR_LENGTH;
      const Vec3 end = vec3_add(peer->cursor_origin,
                                vec3_scale(peer->cursor_direction, length));
      session_line(&lines, peer->cursor_origin, end, color);
      if (peer->cursor_distance > 0.0f) {
        const float32_t size = Max(0.05f, length * 0.01f);
        for (uint32_t axis = 0u; axis < 3u; ++axis) {
          const Vec3 half =
              vec3_new(axis == 0u ? size : 0.0f, axis == 1u ? size : 0.0f,
                       axis == 2u ? size : 0.0f);
          session_line(&lines, vec3_sub(end, half), vec3_add(end, half), color);
        }
      }
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
                                          string_length(peer->name)),
                 &chip);
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

static String8 session_string(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, string_length(text));
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
  VkrUiTextEditBuffer buffer = {(uint8_t *)text, (uint32_t)string_length(text),
                                capacity};
  char field_id[64];
  string_format(field_id, sizeof(field_id), "%s.field", id);
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

/* Who may join: open or restricted, and the editors refused lately with
   a button that allows each. */
static void session_window_access(VkrEditorUi *editor, VkrUiSystem *ui,
                                  VkrEditorSession *session, float32_t *y,
                                  float32_t width) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[160];
  if (session->restricted) {
    string_format(text, sizeof(text), "Only %u allowed editor%s join",
                  session->allowed_count,
                  session->allowed_count == 1u ? "" : "s");
  } else {
    string_format(text, sizeof(text), "Anyone with the key joins");
  }
  session_label(ui, "access", text, 12, *y, width - 160, theme->text);
  if (session_button(editor, ui, "access.toggle",
                     session->restricted ? "Open joining" : "Restrict",
                     width - 136, *y, 124, false_v)) {
    vkr_editor_session_restrict(session, !session->restricted);
  }
  *y += 34;
  for (uint32_t i = 0u; i < session->refused_count; ++i) {
    const VkrEditorSessionRefused refused = session->refusals[i];
    (void)vkr_ui_push_id_u64(ui, i);
    string_format(text, sizeof(text), "Refused %s (%.12s...)", refused.name,
                  refused.key);
    session_label(ui, "refused", text, 12, *y, width - 160, theme->warning);
    if (session_button(editor, ui, "allow", "Allow", width - 136, *y, 124,
                       false_v)) {
      (void)vkr_editor_session_allow(session, refused.key, true_v);
    }
    (void)vkr_ui_pop_id(ui);
    *y += 30;
  }
}

static void session_window_peers(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrEditorSession *session, float32_t *y,
                                 float32_t width) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  char text[160];
  string_format(text, sizeof(text), "%u other editor%s", session->peer_count,
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
      string_format(text, sizeof(text), "%s  %s  (%.1f, %.1f, %.1f)", peer.name,
                    peer.tool, peer.camera_position.x, peer.camera_position.y,
                    peer.camera_position.z);
    } else {
      string_format(text, sizeof(text), "%s  %s", peer.name, peer.tool);
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

static void session_window_build_locked(VkrEditorUi *editor,
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
      string_format(session->error, sizeof(session->error), "%s", error);
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
      string_format(session->error, sizeof(session->error), "%s", error);
    }
    y += 36;
    /* A restricted host allows this key. */
    if (!session->identity_read) {
      (void)session_identity(session, NULL);
    }
    if (session->identity[0]) {
      string_format(text, sizeof(text), "Your key %.16s...", session->identity);
      session_label(ui, "identity", text, 12, y, width - 160,
                    theme->text_secondary);
      if (session_button(editor, ui, "identity.copy", "Copy your key",
                         width - 136, y, 124, false_v)) {
        (void)vkr_platform_clipboard_write_text(
            (const uint8_t *)session->identity,
            (uint32_t)string_length(session->identity));
      }
      y += 34;
    }
  } else {
    const bool8_t host = session->mode == VKR_EDITOR_SESSION_HOST;
    if (host) {
      string_format(text, sizeof(text), "Hosting on %s as %s", session->address,
                    session->name);
    } else if (session->welcomed) {
      string_format(text, sizeof(text), "Joined %s as %s; edit %llu applied",
                    session->address, session->name,
                    (unsigned long long)(session->apply_seq - 1u));
    } else {
      string_format(text, sizeof(text), "Joining %s...", session->address);
    }
    session_label(ui, "state", text, 12, y, width - 24, theme->text);
    y += 28;
    string_format(text, sizeof(text), "Key %.16s...", session->key);
    session_label(ui, "key", text, 12, y, width - 160, theme->text_secondary);
    if (host && session_button(editor, ui, "copy", "Copy key", width - 136, y,
                               124, false_v)) {
      (void)vkr_platform_clipboard_write_text(
          (const uint8_t *)session->key, (uint32_t)string_length(session->key));
    }
    y += 34;
    if (session->depot_phase != SESSION_DEPOT_OFF) {
      char head[VKR_DEPOT_ID_HEX];
      vkr_depot_id_hex(&session->depot_head, head);
      string_format(text, sizeof(text), "Depot %s at %.12s%s",
                    session->depot_branch, head,
                    session->job.running ? "  (working...)" : "");
      session_label(ui, "depot", text, 12, y, width - 160,
                    theme->text_secondary);
      char publish_error[192];
      if (session->depot_phase == SESSION_DEPOT_READY &&
          session_button(editor, ui, "publish", "Publish files", width - 136, y,
                         124, false_v) &&
          !vkr_editor_session_publish_files(session, NULL, publish_error,
                                            sizeof(publish_error))) {
        string_format(session->error, sizeof(session->error), "%s",
                      publish_error);
      }
      y += 34;
    }
    if (host) {
      session_window_access(editor, ui, session, &y, width);
    }
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

void vkr_editor_session_window_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds) {
  if (!editor->session) {
    session_window_build_locked(editor, frame, bounds);
    return;
  }
  VkrEditorSession *session = editor->session;
  session_lock(session);
  session_window_build_locked(editor, frame, bounds);
  session_unlock(session);
}

static void session_accepted_locked(VkrEditorSession *session,
                                    uint16_t container, uint64_t group) {
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

void vkr_editor_session_accepted(VkrEditorSession *session, uint16_t container,
                                 uint64_t group) {
  if (!session) {
    return;
  }
  session_lock(session);
  session_accepted_locked(session, container, group);
  session_unlock(session);
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
    string_format(session->tool, sizeof(session->tool), "%s", tool ? tool : "");
  }
}

void vkr_editor_session_set_history(VkrEditorSession *session, uint64_t bytes) {
  if (session) {
    session->history_budget = bytes;
  }
}

void vkr_editor_session_set_cursor(VkrEditorSession *session, bool8_t valid,
                                   Vec3 origin, Vec3 direction,
                                   float32_t distance) {
  if (!session) {
    return;
  }
  session->has_cursor =
      valid && session_cursor_valid(origin, &direction, distance);
  session->cursor_origin = origin;
  session->cursor_direction = direction;
  session->cursor_distance = distance;
}
