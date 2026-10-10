#pragma once

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
