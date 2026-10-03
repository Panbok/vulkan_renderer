#include "vkr_publication_queue.h"

#include "core/logger.h"
#include "vkr_ibl_math.h"
#include "vkr_render_resources.h"
#include "vkr_renderer_internal.h"

#include <stdlib.h>

struct VkrPublicationCommand {
  VkrPublicationCommand *next;
  VkrPublicationKind kind;
  /* Geometry, texture or material handle; a bake's source. */
  uint32_t id;
  uint32_t generation;
  union {
    VkrGeometryConfig geometry;
    VkrGeometryUpload mesh;
    VkrTexturePreparedLoad texture;
    VkrTextureDescription description;
    struct {
      VkrAtmosphereGpuParams params;
      VkrTextureHandle prefilter;
      VkrTextureHandle transmittance;
      VkrTextureHandle multiple_scattering;
      float32_t sh_deringing;
    } bake;
    VkrMaterial material;
  };
  /* Texture upload batches bracket ordinary texture publications. */
  bool8_t begin_texture_batch;
  bool8_t end_texture_batch;
};

struct VkrPublicationBlock {
  VkrPublicationBlock *next;
  void *bytes;
};

/* Batches are reset after their frame, so a reservation holds a frame's
   copied geometry without committing it. */
#define VKR_PUBLICATION_ARENA_RESERVE MB(256)
#define VKR_PUBLICATION_ARENA_COMMIT KB(256)

bool8_t vkr_publication_queue_create(VkrPublicationQueue *queue) {
  MemZero(queue, sizeof(*queue));
  for (uint32_t i = 0u; i < ArrayCount(queue->batches); ++i) {
    queue->batches[i].arena =
        arena_create(VKR_PUBLICATION_ARENA_RESERVE, VKR_PUBLICATION_ARENA_COMMIT);
    queue->completions[i].arena =
        arena_create(VKR_PUBLICATION_ARENA_RESERVE, VKR_PUBLICATION_ARENA_COMMIT);
    if (!queue->batches[i].arena || !queue->completions[i].arena) {
      vkr_publication_queue_destroy(queue);
      return false_v;
    }
  }
  queue->recording = &queue->batches[0];
  queue->produced = &queue->completions[0];
  queue->delivered = &queue->completions[1];
  queue->snapshot = (VkrPublicationSnapshot){.idle = true_v, .generation = 1u};
  queue->render_snapshot = queue->snapshot;
  return true_v;
}

/* Frees the batch's heap copies and empties it for recording. */
vkr_internal void vkr_publication_batch_reset(VkrPublicationBatch *batch) {
  for (VkrPublicationBlock *block = batch->blocks; block; block = block->next) {
    free(block->bytes);
  }
  if (batch->arena) {
    arena_clear(batch->arena, ARENA_MEMORY_TAG_RENDERER);
  }
  batch->head = NULL;
  batch->tail = NULL;
  batch->blocks = NULL;
  batch->count = 0u;
}

void vkr_publication_queue_destroy(VkrPublicationQueue *queue) {
  for (uint32_t i = 0u; i < ArrayCount(queue->batches); ++i) {
    vkr_publication_batch_reset(&queue->batches[i]);
    if (queue->batches[i].arena) {
      arena_destroy(queue->batches[i].arena);
    }
    if (queue->completions[i].arena) {
      arena_destroy(queue->completions[i].arena);
    }
  }
  MemZero(queue, sizeof(*queue));
}

// =============================================================================
// Recording, on the frame-loop thread
// =============================================================================

vkr_internal VkrPublicationCommand *
vkr_publication_record(VkrRenderer *renderer, VkrPublicationKind kind,
                       uint32_t id, uint32_t generation) {
  VkrPublicationBatch *batch = renderer->publications.recording;
  VkrPublicationCommand *command = arena_alloc(
      batch->arena, sizeof(*command), ARENA_MEMORY_TAG_RENDERER);
  if (!command) {
    log_error("Publication queue is out of memory");
    return NULL;
  }
  MemZero(command, sizeof(*command));
  command->kind = kind;
  command->id = id;
  command->generation = generation;
  if (batch->tail) {
    batch->tail->next = command;
  } else {
    batch->head = command;
  }
  batch->tail = command;
  batch->count++;
  return command;
}

/* Copies `bytes` of `source` into the recording batch's arena. */
vkr_internal const void *vkr_publication_copy(VkrRenderer *renderer,
                                              const void *source,
                                              uint64_t bytes) {
  if (!source || bytes == 0u) {
    return source;
  }
  void *copy = arena_alloc(renderer->publications.recording->arena, bytes,
                           ARENA_MEMORY_TAG_RENDERER);
  if (copy) {
    MemCopy(copy, source, bytes);
  }
  return copy;
}

/* Copies bulk `bytes` of `source` into a heap block the batch frees after it
   runs. */
vkr_internal void *vkr_publication_copy_bulk(VkrRenderer *renderer,
                                             const void *source,
                                             uint64_t bytes) {
  if (!source || bytes == 0u) {
    return (void *)source;
  }
  VkrPublicationBatch *batch = renderer->publications.recording;
  VkrPublicationBlock *block = arena_alloc(batch->arena, sizeof(*block),
                                           ARENA_MEMORY_TAG_RENDERER);
  void *copy = block ? malloc(bytes) : NULL;
  if (!copy) {
    log_error("Publication queue could not copy %llu bytes",
              (unsigned long long)bytes);
    return NULL;
  }
  MemCopy(copy, source, bytes);
  block->bytes = copy;
  block->next = batch->blocks;
  batch->blocks = block;
  return copy;
}

vkr_internal String8 vkr_publication_copy_string8(VkrRenderer *renderer,
                                                  String8 source) {
  const uint8_t *copy =
      vkr_publication_copy(renderer, source.str, source.length);
  return (String8){.str = (uint8_t *)copy, .length = copy ? source.length : 0u};
}

vkr_internal const char *vkr_publication_copy_cstr(VkrRenderer *renderer,
                                                   const char *source) {
  return source ? vkr_publication_copy(renderer, source,
                                       string_length(source) + 1u)
                : NULL;
}

vkr_internal bool8_t vkr_publication_record_geometry(
    void *state, VkrGeometryHandle handle, const VkrGeometryConfig *geometry) {
  VkrRenderer *renderer = state;
  VkrPublicationCommand *command = vkr_publication_record(
      renderer, VKR_PUBLICATION_GEOMETRY, handle.id, handle.generation);
  if (!command) {
    return false_v;
  }
  command->geometry = *geometry;
  command->geometry.vertices = vkr_publication_copy_bulk(
      renderer, geometry->vertices,
      (uint64_t)geometry->vertex_size * geometry->vertex_count);
  command->geometry.indices = vkr_publication_copy_bulk(
      renderer, geometry->indices,
      (uint64_t)geometry->index_size * geometry->index_count);
  command->geometry.decodes = vkr_publication_copy(
      renderer, geometry->decodes,
      (uint64_t)geometry->decode_count * sizeof(*geometry->decodes));
  return (command->geometry.vertices || !geometry->vertices) &&
         (command->geometry.indices || !geometry->indices) &&
         (command->geometry.decodes || !geometry->decodes);
}

vkr_internal bool8_t
vkr_publication_record_loaded_mesh(void *state, VkrGeometryHandle handle,
                                   const VkrGeometryUpload *mesh) {
  VkrRenderer *renderer = state;
  VkrPublicationCommand *command = vkr_publication_record(
      renderer, VKR_PUBLICATION_LOADED_MESH, handle.id, handle.generation);
  if (!command) {
    return false_v;
  }
  const VkrGeometryUploadBuffer *buffer = &mesh->buffer;
  command->mesh = *mesh;
  VkrGeometryUploadBuffer *copied = &command->mesh.buffer;
  copied->vertices =
      vkr_publication_copy_bulk(renderer, buffer->vertices,
                           (uint64_t)buffer->vertex_size * buffer->vertex_count);
  copied->indices =
      vkr_publication_copy_bulk(renderer, buffer->indices,
                           (uint64_t)buffer->index_size * buffer->index_count);
  copied->decodes = vkr_publication_copy(
      renderer, buffer->decodes,
      (uint64_t)buffer->decode_count * sizeof(*buffer->decodes));
  VkrGeometryUploadRange *submeshes =
      (VkrGeometryUploadRange *)vkr_publication_copy(
          renderer, mesh->submeshes,
          (uint64_t)mesh->submesh_count * sizeof(*mesh->submeshes));
  if ((buffer->vertices && !copied->vertices) ||
      (buffer->indices && !copied->indices) ||
      (buffer->decodes && !copied->decodes) ||
      (mesh->submeshes && !submeshes)) {
    return false_v;
  }
  for (uint32_t i = 0u; submeshes && i < mesh->submesh_count; ++i) {
    submeshes[i].material_name =
        vkr_publication_copy_string8(renderer, submeshes[i].material_name);
    submeshes[i].shader_override =
        vkr_publication_copy_string8(renderer, submeshes[i].shader_override);
  }
  command->mesh.submeshes = submeshes;
  return true_v;
}

vkr_internal bool8_t vkr_publication_record_unpublish_geometry(
    void *state, VkrGeometryHandle handle) {
  return vkr_publication_record(state, VKR_PUBLICATION_UNPUBLISH_GEOMETRY,
                                handle.id, handle.generation) != NULL;
}

vkr_internal bool8_t vkr_publication_record_begin_texture_batch(void *state) {
  VkrRenderer *renderer = state;
  renderer->publications.texture_batch_open = true_v;
  renderer->publications.texture_batch_recorded = false_v;
  return true_v;
}

vkr_internal bool8_t vkr_publication_record_end_texture_batch(void *state) {
  VkrRenderer *renderer = state;
  VkrPublicationQueue *queue = &renderer->publications;
  const bool8_t recorded = queue->texture_batch_recorded;
  queue->texture_batch_open = false_v;
  queue->texture_batch_recorded = false_v;
  if (!recorded) {
    return true_v;
  }
  VkrPublicationCommand *command =
      vkr_publication_record(renderer, VKR_PUBLICATION_TEXTURE, 0u, 0u);
  if (command) {
    command->end_texture_batch = true_v;
  }
  return command != NULL;
}

vkr_internal VkrRendererError
vkr_publication_record_texture(void *state, VkrTextureHandle handle,
                               const VkrTexturePreparedLoad *texture) {
  VkrRenderer *renderer = state;
  VkrPublicationQueue *queue = &renderer->publications;
  if (queue->texture_batch_open && !queue->texture_batch_recorded) {
    VkrPublicationCommand *begin =
        vkr_publication_record(renderer, VKR_PUBLICATION_TEXTURE, 0u, 0u);
    if (!begin) {
      return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    begin->begin_texture_batch = true_v;
    queue->texture_batch_recorded = true_v;
  }
  VkrPublicationCommand *command = vkr_publication_record(
      renderer, VKR_PUBLICATION_TEXTURE, handle.id, handle.generation);
  if (!command) {
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }
  command->texture = *texture;
  command->texture.upload_data = vkr_publication_copy_bulk(
      renderer, texture->upload_data, texture->upload_data_size);
  command->texture.upload_regions = vkr_publication_copy_bulk(
      renderer, texture->upload_regions,
      (uint64_t)texture->upload_region_count *
          sizeof(*texture->upload_regions));
  if ((texture->upload_data && !command->texture.upload_data) ||
      (texture->upload_regions && !command->texture.upload_regions)) {
    return VKR_RENDERER_ERROR_OUT_OF_MEMORY;
  }
  return VKR_RENDERER_ERROR_NONE;
}

vkr_internal bool8_t vkr_publication_record_writable_texture(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  VkrPublicationCommand *command = vkr_publication_record(
      state, VKR_PUBLICATION_WRITABLE_TEXTURE, handle.id, handle.generation);
  if (command) {
    command->description = *description;
  }
  return command != NULL;
}

vkr_internal bool8_t vkr_publication_record_texture_sampler(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  VkrPublicationCommand *command = vkr_publication_record(
      state, VKR_PUBLICATION_TEXTURE_SAMPLER, handle.id, handle.generation);
  if (command) {
    command->description = *description;
  }
  return command != NULL;
}

vkr_internal bool8_t vkr_publication_record_ibl_bake(void *state,
                                                     VkrTextureHandle source,
                                                     VkrTextureHandle prefilter,
                                                     float32_t sh_deringing) {
  VkrPublicationCommand *command = vkr_publication_record(
      state, VKR_PUBLICATION_IBL_BAKE, source.id, source.generation);
  if (command) {
    command->bake.prefilter = prefilter;
    command->bake.sh_deringing = sh_deringing;
  }
  return command != NULL;
}

vkr_internal bool8_t vkr_publication_record_atmosphere_bake(
    void *state, const VkrAtmosphereGpuParams *params, VkrTextureHandle source,
    VkrTextureHandle prefilter, VkrTextureHandle transmittance,
    VkrTextureHandle multiple_scattering, float32_t sh_deringing) {
  VkrPublicationCommand *command = vkr_publication_record(
      state, VKR_PUBLICATION_ATMOSPHERE_BAKE, source.id, source.generation);
  if (command) {
    command->bake.params = *params;
    command->bake.prefilter = prefilter;
    command->bake.transmittance = transmittance;
    command->bake.multiple_scattering = multiple_scattering;
    command->bake.sh_deringing = sh_deringing;
  }
  return command != NULL;
}

vkr_internal bool8_t vkr_publication_record_unpublish_texture(
    void *state, VkrTextureHandle handle) {
  return vkr_publication_record(state, VKR_PUBLICATION_UNPUBLISH_TEXTURE,
                                handle.id, handle.generation) != NULL;
}

vkr_internal bool8_t vkr_publication_record_material(
    void *state, VkrMaterialHandle handle, const VkrMaterial *material) {
  VkrRenderer *renderer = state;
  VkrPublicationCommand *command = vkr_publication_record(
      renderer, VKR_PUBLICATION_MATERIAL, handle.id, handle.generation);
  if (!command) {
    return false_v;
  }
  command->material = *material;
  command->material.name = vkr_publication_copy_cstr(renderer, material->name);
  command->material.shader_name =
      vkr_publication_copy_cstr(renderer, material->shader_name);
  return (command->material.name || !material->name) &&
         (command->material.shader_name || !material->shader_name);
}

vkr_internal bool8_t vkr_publication_record_unpublish_material(
    void *state, VkrMaterialHandle handle) {
  return vkr_publication_record(state, VKR_PUBLICATION_UNPUBLISH_MATERIAL,
                                handle.id, handle.generation) != NULL;
}

// =============================================================================
// Queries and completions, on the frame-loop thread
// =============================================================================

vkr_internal const VkrPublicationSource *
vkr_publication_find_source(const VkrPublicationSnapshot *snapshot,
                            VkrTextureHandle source) {
  for (uint32_t i = 0u; i < snapshot->source_count; ++i) {
    const VkrPublicationSource *entry = &snapshot->sources[i];
    if (entry->source.id == source.id &&
        entry->source.generation == source.generation) {
      return entry;
    }
  }
  return NULL;
}

vkr_internal bool8_t vkr_publication_publications_idle(void *state) {
  const VkrRenderer *renderer = state;
  return renderer->publications.recording->count == 0u &&
         renderer->publications.snapshot.idle;
}

vkr_internal uint64_t vkr_publication_publication_generation(void *state) {
  const VkrRenderer *renderer = state;
  return renderer->publications.snapshot.generation;
}

/* The render thread reports a full upload ring as a busy completion. */
vkr_internal bool8_t vkr_publication_texture_upload_available(void *state,
                                                              uint64_t bytes) {
  (void)state;
  (void)bytes;
  return true_v;
}

vkr_internal VkrAtmosphereBakeStatus
vkr_publication_atmosphere_bake_status(void *state, VkrTextureHandle source) {
  const VkrRenderer *renderer = state;
  const VkrPublicationSource *entry =
      vkr_publication_find_source(&renderer->publications.snapshot, source);
  return entry ? entry->atmosphere_status : VKR_ATMOSPHERE_BAKE_PENDING;
}

vkr_internal uint32_t vkr_publication_ibl_sh_slot(void *state,
                                                  VkrTextureHandle source) {
  const VkrRenderer *renderer = state;
  const VkrPublicationSource *entry =
      vkr_publication_find_source(&renderer->publications.snapshot, source);
  return entry ? entry->sh_slot : VKR_SH_SLOT_BLACK;
}

vkr_internal bool8_t
vkr_publication_poll_completion(void *state, VkrPublicationCompletion *out) {
  VkrRenderer *renderer = state;
  VkrPublicationCompletionList *list = renderer->publications.delivered;
  if (list->cursor >= list->count) {
    return false_v;
  }
  *out = list->items[list->cursor++];
  return true_v;
}

void vkr_publication_queue_publisher(VkrRenderer *renderer,
                                     VkrAssetPublisher *publisher) {
  *publisher = (VkrAssetPublisher){
      .state = renderer,
      .poll_completion = vkr_publication_poll_completion,
      .publications_idle = vkr_publication_publications_idle,
      .publication_generation = vkr_publication_publication_generation,
      .texture_upload_available = vkr_publication_texture_upload_available,
      .publish_geometry = vkr_publication_record_geometry,
      .publish_loaded_mesh = vkr_publication_record_loaded_mesh,
      .unpublish_geometry = vkr_publication_record_unpublish_geometry,
      .begin_texture_upload_batch = vkr_publication_record_begin_texture_batch,
      .end_texture_upload_batch = vkr_publication_record_end_texture_batch,
      .publish_texture = vkr_publication_record_texture,
      .publish_writable_texture = vkr_publication_record_writable_texture,
      .update_texture_sampler = vkr_publication_record_texture_sampler,
      .bake_ibl_cubemap = vkr_publication_record_ibl_bake,
      .bake_atmosphere = vkr_publication_record_atmosphere_bake,
      .atmosphere_bake_status = vkr_publication_atmosphere_bake_status,
      .ibl_sh_slot = vkr_publication_ibl_sh_slot,
      .unpublish_texture = vkr_publication_record_unpublish_texture,
      .publish_material = vkr_publication_record_material,
      .unpublish_material = vkr_publication_record_unpublish_material,
  };
}

VkrPublicationBatch *vkr_publication_queue_take(VkrPublicationQueue *queue) {
  VkrPublicationBatch *batch = queue->recording;
  if (batch->count == 0u) {
    return NULL;
  }
  queue->recording =
      batch == &queue->batches[0] ? &queue->batches[1] : &queue->batches[0];
  return batch;
}

void vkr_publication_queue_deliver(VkrPublicationQueue *queue) {
  VkrPublicationCompletionList *delivered = queue->delivered;
  queue->delivered = queue->produced;
  queue->produced = delivered;
  arena_clear(delivered->arena, ARENA_MEMORY_TAG_RENDERER);
  delivered->items = NULL;
  delivered->count = 0u;
  delivered->capacity = 0u;
  delivered->cursor = 0u;
  queue->snapshot = queue->render_snapshot;
}

// =============================================================================
// Running, on the thread that renders the frame
// =============================================================================

vkr_internal void vkr_publication_complete(VkrPublicationQueue *queue,
                                           const VkrPublicationCommand *command,
                                           VkrRendererError error) {
  VkrPublicationCompletionList *list = queue->produced;
  if (list->count == list->capacity) {
    const uint32_t capacity = list->capacity ? list->capacity * 2u : 64u;
    VkrPublicationCompletion *items =
        arena_alloc(list->arena, (uint64_t)capacity * sizeof(*items),
                    ARENA_MEMORY_TAG_RENDERER);
    if (!items) {
      log_error("Publication completion list is out of memory");
      return;
    }
    if (list->count) {
      MemCopy(items, list->items, (uint64_t)list->count * sizeof(*items));
    }
    list->items = items;
    list->capacity = capacity;
  }
  list->items[list->count++] = (VkrPublicationCompletion){
      .kind = command->kind,
      .id = command->id,
      .generation = command->generation,
      .error = error,
  };
}

vkr_internal void vkr_publication_track_source(VkrPublicationSnapshot *snapshot,
                                               VkrTextureHandle source,
                                               bool8_t atmosphere) {
  for (uint32_t i = 0u; i < snapshot->source_count; ++i) {
    VkrPublicationSource *entry = &snapshot->sources[i];
    if (entry->source.id == source.id &&
        entry->source.generation == source.generation) {
      entry->atmosphere = atmosphere;
      entry->atmosphere_status = VKR_ATMOSPHERE_BAKE_PENDING;
      return;
    }
  }
  if (snapshot->source_count == ArrayCount(snapshot->sources)) {
    log_error("Publication queue tracks at most %u bake sources",
              VKR_PUBLICATION_SOURCE_MAX);
    return;
  }
  snapshot->sources[snapshot->source_count++] = (VkrPublicationSource){
      .source = source,
      .atmosphere = atmosphere,
      .atmosphere_status = VKR_ATMOSPHERE_BAKE_PENDING,
      .sh_slot = VKR_SH_SLOT_BLACK,
  };
}

vkr_internal void
vkr_publication_untrack_source(VkrPublicationSnapshot *snapshot,
                               VkrTextureHandle source) {
  for (uint32_t i = 0u; i < snapshot->source_count; ++i) {
    if (snapshot->sources[i].source.id == source.id &&
        snapshot->sources[i].source.generation == source.generation) {
      snapshot->sources[i] = snapshot->sources[--snapshot->source_count];
      return;
    }
  }
}

vkr_internal VkrRendererError
vkr_publication_result(bool8_t accepted) {
  return accepted ? VKR_RENDERER_ERROR_NONE
                  : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
}

void vkr_publication_queue_run(VkrRenderer *renderer,
                               VkrPublicationBatch *batch) {
  VkrPublicationQueue *queue = &renderer->publications;
  const VkrAssetPublisher *native = &renderer->native_publisher;
  void *state = native->state;
  for (const VkrPublicationCommand *command = batch->head; command;
       command = command->next) {
    const VkrGeometryHandle geometry = {.id = command->id,
                                        .generation = command->generation};
    const VkrTextureHandle texture = {.id = command->id,
                                      .generation = command->generation};
    const VkrMaterialHandle material = {.id = command->id,
                                        .generation = command->generation};
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    switch (command->kind) {
    case VKR_PUBLICATION_GEOMETRY:
      error = vkr_publication_result(
          native->publish_geometry(state, geometry, &command->geometry));
      break;
    case VKR_PUBLICATION_LOADED_MESH:
      error = vkr_publication_result(
          native->publish_loaded_mesh(state, geometry, &command->mesh));
      break;
    case VKR_PUBLICATION_UNPUBLISH_GEOMETRY:
      error = vkr_publication_result(native->unpublish_geometry(state, geometry));
      break;
    case VKR_PUBLICATION_TEXTURE:
      if (command->begin_texture_batch) {
        if (native->begin_texture_upload_batch &&
            !native->begin_texture_upload_batch(state)) {
          log_error("Asset texture upload batch initialization failed");
        }
        continue;
      }
      if (command->end_texture_batch) {
        if (native->end_texture_upload_batch &&
            !native->end_texture_upload_batch(state)) {
          log_error("Asset texture upload batch submission failed");
        }
        continue;
      }
      error = native->publish_texture(state, texture, &command->texture);
      break;
    case VKR_PUBLICATION_WRITABLE_TEXTURE:
      error = vkr_publication_result(native->publish_writable_texture(
          state, texture, &command->description));
      break;
    case VKR_PUBLICATION_TEXTURE_SAMPLER:
      error = vkr_publication_result(native->update_texture_sampler(
          state, texture, &command->description));
      break;
    case VKR_PUBLICATION_UNPUBLISH_TEXTURE:
      vkr_publication_untrack_source(&queue->render_snapshot, texture);
      error = vkr_publication_result(native->unpublish_texture(state, texture));
      break;
    case VKR_PUBLICATION_IBL_BAKE:
      error = vkr_publication_result(
          native->bake_ibl_cubemap &&
          native->bake_ibl_cubemap(state, texture, command->bake.prefilter,
                                   command->bake.sh_deringing));
      if (error == VKR_RENDERER_ERROR_NONE) {
        vkr_publication_track_source(&queue->render_snapshot, texture, false_v);
      }
      break;
    case VKR_PUBLICATION_ATMOSPHERE_BAKE:
      error = vkr_publication_result(
          native->bake_atmosphere &&
          native->bake_atmosphere(state, &command->bake.params, texture,
                                  command->bake.prefilter,
                                  command->bake.transmittance,
                                  command->bake.multiple_scattering,
                                  command->bake.sh_deringing));
      if (error == VKR_RENDERER_ERROR_NONE) {
        vkr_publication_track_source(&queue->render_snapshot, texture, true_v);
      }
      break;
    case VKR_PUBLICATION_MATERIAL:
      error = vkr_publication_result(
          native->publish_material(state, material, &command->material));
      break;
    case VKR_PUBLICATION_UNPUBLISH_MATERIAL:
      error =
          vkr_publication_result(native->unpublish_material(state, material));
      break;
    }
    vkr_publication_complete(queue, command, error);
  }
  vkr_publication_batch_reset(batch);
}

void vkr_publication_queue_observe(VkrRenderer *renderer) {
  VkrPublicationSnapshot *snapshot = &renderer->publications.render_snapshot;
  const VkrAssetPublisher *native = &renderer->native_publisher;
  snapshot->idle = !native->publications_idle ||
                   native->publications_idle(native->state);
  snapshot->generation = native->publication_generation
                             ? native->publication_generation(native->state)
                             : 1u;
  for (uint32_t i = 0u; i < snapshot->source_count; ++i) {
    VkrPublicationSource *entry = &snapshot->sources[i];
    if (entry->atmosphere && native->atmosphere_bake_status) {
      entry->atmosphere_status =
          native->atmosphere_bake_status(native->state, entry->source);
    }
    if (native->ibl_sh_slot) {
      entry->sh_slot = native->ibl_sh_slot(native->state, entry->source);
    }
  }
}
