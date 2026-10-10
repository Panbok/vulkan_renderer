#pragma once

#include "editor_ops.h"
#include "vkr_sample_runtime.h"

/* Collaborative editing (docs/proposals/network-protocol.md, "Editor
 * collaboration"). One editor hosts a session over the network transport;
 * others join it. The host is authoritative: every journal-changing edit,
 * a participant's or its own, applies on the host first, in one session
 * order, and the host broadcasts the applied, value-level edit. Every peer
 * applies the broadcast through the same request slots its own edits use,
 * so every journal mirrors the host's and a shared undo stays consistent.
 *
 * The session runs once per UI build, after every other part of the editor
 * has made its requests: it forwards a participant's edits instead of
 * applying them, records the host's own, and injects queued edits into the
 * request slots that are free. A participant must start from the host's
 * session base: the same scene with no unsaved edits, which a scene digest
 * checks when it joins; edits made since are replayed to it in order.
 *
 * Edits that cannot travel yet (vkr_net_scene_edit.h: duplicate, terrain,
 * physics, settings, partition, scene loads) are refused while a session
 * runs, with a Console message. */

#define VKR_EDITOR_SESSION_PORT 7330u
#define VKR_EDITOR_SESSION_PEERS_MAX 32u
#define VKR_EDITOR_SESSION_NAME_MAX 48u

typedef enum VkrEditorSessionMode {
  VKR_EDITOR_SESSION_NONE = 0,
  VKR_EDITOR_SESSION_HOST,
  VKR_EDITOR_SESSION_PARTICIPANT,
} VkrEditorSessionMode;

typedef struct VkrEditorSessionPeer {
  uint8_t id;
  char name[VKR_EDITOR_SESSION_NAME_MAX];
  bool8_t has_camera;
  Vec3 camera_position;
  float32_t camera_yaw;
  float32_t camera_pitch;
  bool8_t has_selection;
  uint16_t selection_container;
  VkrEntityRef selection;
} VkrEditorSessionPeer;

typedef struct VkrEditorSessionStatus {
  uint8_t mode; /**< VkrEditorSessionMode. */
  /* Participant: joined and replaying or live. */
  bool8_t joined;
  uint8_t self_id;
  /* The next session sequence number this editor applies. */
  uint64_t sequence;
  /* Edits waiting to apply here. */
  uint32_t pending;
  char address[64];
  char key[65];
  char error[192];
  VkrEditorSessionPeer peers[VKR_EDITOR_SESSION_PEERS_MAX];
  uint32_t peer_count;
} VkrEditorSessionStatus;

typedef struct VkrEditorSession VkrEditorSession;

VkrEditorSession *vkr_editor_session_create(VkrAllocator *allocator);
void vkr_editor_session_destroy(VkrEditorSession *session);

/* Hosts on `bind` (NULL: every interface, VKR_EDITOR_SESSION_PORT) with a
   fresh key; the session base is the current scene state. */
bool8_t vkr_editor_session_host(VkrEditorSession *session,
                                const VkrSampleUiFrame *frame, const char *bind,
                                const char *name, char *error,
                                uint32_t capacity);

/* Joins the host at `address` whose key is `key_hex`. */
bool8_t vkr_editor_session_join(VkrEditorSession *session,
                                const VkrSampleUiFrame *frame,
                                const char *address, const char *key_hex,
                                const char *name, char *error,
                                uint32_t capacity);

void vkr_editor_session_leave(VkrEditorSession *session);

/* Runs the session for one UI build; call after everything else that may
   edit the scene. */
void vkr_editor_session_update(VkrEditorSession *session,
                               const VkrSampleUiFrame *frame);

void vkr_editor_session_status(const VkrEditorSession *session,
                               VkrEditorSessionStatus *out_status);

/* The digest a participant must match to join: every entity with a document
   id in the loaded containers, by id, with its name, pose and parent. */
void vkr_editor_session_digest(const VkrSampleUiFrame *frame,
                               uint8_t out_digest[32]);

// =============================================================================
// Agent federation: authors, the change feed, claims and the task board
// =============================================================================

/* `local` as the session knows it: `<local>@<this editor's name>` while a
   session runs, else `local` unchanged. `out` may be `local`. */
void vkr_editor_session_author(const VkrEditorSession *session,
                               const char *local, char *out, uint32_t capacity);

/* Names the author and label of the batch this build submits under
   `token`; the session carries them with the batch to every editor. */
void vkr_editor_session_note_batch(VkrEditorSession *session, uint64_t token,
                                   const char *author, const char *label);

#define VKR_EDITOR_SESSION_APPLIED_ENTITIES 16u

/* An agent batch of another editor that applied here, for the change
   feed. */
typedef struct VkrEditorSessionApplied {
  uint16_t container;
  char author[VKR_EDITOR_AUTHOR_CAPACITY];
  char label[96];
  uint32_t entity_count;
  VkrEntityId entities[VKR_EDITOR_SESSION_APPLIED_ENTITIES];
} VkrEditorSessionApplied;

/* Takes the oldest such batch; false when none waits. */
bool8_t vkr_editor_session_take_applied(VkrEditorSession *session,
                                        VkrEditorSessionApplied *out);

/* Whether claims and tasks belong to a session host elsewhere: this editor
   joined a session, so its agents ask the host. */
bool8_t vkr_editor_session_forwards(const VkrEditorSession *session);

/* Whether this editor hosts a session; its claims and tasks are the
   session's. */
bool8_t vkr_editor_session_hosting(const VkrEditorSession *session);

typedef enum VkrEditorSessionAskKind {
  VKR_EDITOR_ASK_CLAIM_SET = 1,
  VKR_EDITOR_ASK_CLAIM_RELEASE,
  VKR_EDITOR_ASK_TASK_ADD,
  VKR_EDITOR_ASK_TASK_NEXT,
  VKR_EDITOR_ASK_TASK_DONE,
} VkrEditorSessionAskKind;

/* An agent's request a participant forwards to the host. */
typedef struct VkrEditorSessionAsk {
  uint64_t token;
  uint8_t kind; /**< VkrEditorSessionAskKind. */
  /* Host: the participant that asked. */
  uint8_t peer;
  char author[VKR_EDITOR_AUTHOR_CAPACITY];
  /* CLAIM_SET: id (zero for a new claim), container, name and box;
     CLAIM_RELEASE: id, or every claim of the author with `all`. */
  VkrEditorClaim claim;
  bool8_t all;
  /* TASK_ADD: kind, title and region; TASK_DONE: id, state and note. */
  VkrEditorTask task;
  /* TASK_NEXT: the kinds the agent takes, separated by commas; empty takes
     any. */
  char kinds[128];
} VkrEditorSessionAsk;

typedef struct VkrEditorSessionAnswer {
  uint64_t token;
  bool8_t ok;
  /* The agent channel's error code and message when not ok. */
  char code[24];
  char error[256];
  VkrEditorClaim claim;
  bool8_t has_task;
  VkrEditorTask task;
  uint32_t released_count;
  uint32_t released[VKR_EDITOR_CLAIM_MAX];
} VkrEditorSessionAnswer;

/* Participant: sends `ask` to the host and returns its token, zero when it
   could not go. */
uint64_t vkr_editor_session_ask(VkrEditorSession *session,
                                const VkrEditorSessionAsk *ask);

/* Participant: takes the host's answer to `token`; false while none
   arrived. */
bool8_t vkr_editor_session_take_answer(VkrEditorSession *session,
                                       uint64_t token,
                                       VkrEditorSessionAnswer *out);

/* Host: takes the oldest request a participant's agent sent, and answers
   it. */
bool8_t vkr_editor_session_take_ask(VkrEditorSession *session,
                                    VkrEditorSessionAsk *out);
void vkr_editor_session_reply(VkrEditorSession *session,
                              const VkrEditorSessionAsk *ask,
                              const VkrEditorSessionAnswer *answer);

/* Host: sends the claims and tasks to every participant, now and to each
   that joins later. */
void vkr_editor_session_publish(VkrEditorSession *session,
                                const VkrEditorClaim *claims,
                                uint32_t claim_count,
                                const VkrEditorTask *tasks,
                                uint32_t task_count);

/* Increases each time this editor starts hosting or joins. */
uint64_t vkr_editor_session_epoch(const VkrEditorSession *session);

/* Participant: the host's latest claims and tasks; `revision` grows with
   each copy received, zero before the first. */
uint64_t vkr_editor_session_shared(const VkrEditorSession *session,
                                   const VkrEditorClaim **out_claims,
                                   uint32_t *out_claim_count,
                                   const VkrEditorTask **out_tasks,
                                   uint32_t *out_task_count);
