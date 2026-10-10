#pragma once

#include "renderer/systems/vkr_scene_edit.h"
#include "vkr_bitstream.h"

/* Scene edits on the wire for collaborative editing
 * (docs/proposals/network-protocol.md, "Editor collaboration"). An edit names
 * entities by their document-stable id (VkrEntityRef), or by the index of the
 * edit of the same batch that creates them, never by a local VkrEntityId,
 * which differs between editors. Values travel through their type
 * descriptors (vkr_net_type.h).
 *
 * Only edits every peer reproduces exactly travel: APPLY (transform, name,
 * visibility, lights and one world component; not physics), CREATE with a
 * document id chosen ahead, DELETE, REPARENT and component add, remove and
 * replace. DUPLICATE (fresh ids on each peer), terrain strokes (floating
 * point each host computes) and physics, settings or partition edits do not
 * travel yet; the writer refuses them. */

typedef struct VkrNetSceneEditScenes {
  /* The loaded containers by world id: 0 the primary scene, 1 to
     VKR_SCENE_ADDITIVE_MAX added scenes; `world` the root World. NULL where
     none is loaded. */
  const VkrScene *containers[1u + VKR_SCENE_ADDITIVE_MAX];
  const VkrScene *world;
} VkrNetSceneEditScenes;

/* Whether the edit is one that can travel. */
bool8_t vkr_net_scene_edit_supported(const VkrSceneEditRequest *request);

/* Writes one edit. `entity_ref` and `parent_ref` are the batch indices of
   VkrSampleEditBatchItem, or negative. False, with a message, when the edit
   cannot travel or an entity has no document id. */
bool8_t vkr_net_scene_edit_write(VkrBitWriter *writer,
                                 const VkrNetSceneEditScenes *scenes,
                                 const VkrSceneEditRequest *request,
                                 int32_t entity_ref, int32_t parent_ref,
                                 char *error, uint32_t capacity);

/* Reads one edit and resolves its entities in this editor's scenes. False,
   with a message, on a malformed edit or an entity this editor lacks. */
bool8_t vkr_net_scene_edit_read(VkrBitReader *reader,
                                const VkrNetSceneEditScenes *scenes,
                                VkrSceneEditRequest *out_request,
                                int32_t *out_entity_ref,
                                int32_t *out_parent_ref, char *error,
                                uint32_t capacity);
