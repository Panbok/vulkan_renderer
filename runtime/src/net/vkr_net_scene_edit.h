#pragma once

#include "renderer/systems/vkr_scene_edit.h"
#include "vkr_bitstream.h"

/* Scene edits on the wire for collaborative editing (ADR-106). An edit
 * names entities by their document-stable id (VkrEntityRef), or by the index
 * of the edit of the same batch that creates them, never by a local
 * VkrEntityId, which differs between editors. Values travel through their
 * type descriptors (vkr_net_type.h).
 *
 * Only edits every peer reproduces exactly travel: APPLY (transform, name,
 * visibility, lights and one world component; not physics), CREATE with a
 * document id chosen ahead, DUPLICATE with the seed its copies' ids derive
 * from, DELETE, REPARENT, component add, remove and replace, and terrain
 * ops. A terrain op's result can differ between machines (floating point),
 * so the session host sends the samples it changed beside the op, and other
 * editors write those (VKR_HEIGHTFIELD_OP_SAMPLES). Physics, settings or
 * partition edits do not travel yet; the writer refuses them.
 *
 * A stamp's image and a road's points travel as raw arrays in place, 16-byte
 * aligned from the start of the encoded bytes and in the little-endian
 * order of every supported host. A reader whose bytes start 16-byte aligned
 * borrows them, so the request lives no longer than those bytes. */

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

/* Writes `size` bytes as a raw array, padded to a 16-byte boundary from the
   start of the writer's buffer; and reads one in place, NULL when the
   reader's bytes are not aligned so or are too short. */
void vkr_net_write_array(VkrBitWriter *writer, const void *bytes,
                         uint32_t size);
const void *vkr_net_read_array(VkrBitReader *reader, uint32_t size);

/* Reads one edit and resolves its entities in this editor's scenes. False,
   with a message, on a malformed edit or an entity this editor lacks. */
bool8_t vkr_net_scene_edit_read(VkrBitReader *reader,
                                const VkrNetSceneEditScenes *scenes,
                                VkrSceneEditRequest *out_request,
                                int32_t *out_entity_ref,
                                int32_t *out_parent_ref, char *error,
                                uint32_t capacity);
