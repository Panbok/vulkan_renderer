#include "renderer/vkr_draw_batch.h"

#include "core/logger.h"

vkr_internal int vkr_draw_key_compare(const VkrDrawKey *a,
                                      const VkrDrawKey *b) {
  if (a->pipeline_id != b->pipeline_id)
    return (a->pipeline_id < b->pipeline_id) ? -1 : 1;
  if (a->material_id != b->material_id)
    return (a->material_id < b->material_id) ? -1 : 1;
  if (a->geometry_id != b->geometry_id)
    return (a->geometry_id < b->geometry_id) ? -1 : 1;
  if (a->range_id != b->range_id)
    return (a->range_id < b->range_id) ? -1 : 1;
  return 0;
}

vkr_internal int vkr_draw_command_key_compare(const void *lhs,
                                              const void *rhs) {
  const VkrDrawCommand *a = (const VkrDrawCommand *)lhs;
  const VkrDrawCommand *b = (const VkrDrawCommand *)rhs;
  return vkr_draw_key_compare(&a->key, &b->key);
}

vkr_internal int vkr_draw_command_distance_compare(const void *lhs,
                                                   const void *rhs) {
  const VkrDrawCommand *a = (const VkrDrawCommand *)lhs;
  const VkrDrawCommand *b = (const VkrDrawCommand *)rhs;
  if (a->camera_distance > b->camera_distance)
    return -1;
  if (a->camera_distance < b->camera_distance)
    return 1;
  return vkr_draw_key_compare(&a->key, &b->key);
}

vkr_internal void vkr_draw_batcher_build_batches(
    const Vector_VkrDrawCommand *commands, Vector_VkrDrawBatch *batches,
    uint32_t command_base, uint32_t *draws_merged, uint32_t *batches_created) {
  vector_clear_VkrDrawBatch(batches);

  if (commands->length == 0) {
    return;
  }

  assert_log(commands->length <= UINT32_MAX, "Command count overflow");
  uint32_t batch_start = 0;
  VkrDrawKey current_key = commands->data[0].key;

  for (uint32_t i = 1; i < (uint32_t)commands->length; ++i) {
    if (vkr_draw_key_compare(&current_key, &commands->data[i].key) == 0) {
      continue;
    }

    uint32_t batch_count = i - batch_start;
    vector_push_VkrDrawBatch(
        batches, (VkrDrawBatch){.key = current_key,
                                .first_command = command_base + batch_start,
                                .command_count = batch_count,
                                .first_instance = 0});
    *batches_created += 1;
    if (batch_count > 0) {
      *draws_merged += batch_count - 1;
    }

    batch_start = i;
    current_key = commands->data[i].key;
  }

  uint32_t final_count = (uint32_t)commands->length - batch_start;
  vector_push_VkrDrawBatch(
      batches, (VkrDrawBatch){.key = current_key,
                              .first_command = command_base + batch_start,
                              .command_count = final_count,
                              .first_instance = 0});
  *batches_created += 1;
  if (final_count > 0) {
    *draws_merged += final_count - 1;
  }
}

bool8_t vkr_draw_batcher_init(VkrDrawBatcher *batcher, VkrAllocator *allocator,
                              uint32_t initial_capacity) {
  assert_log(batcher != NULL, "Batcher is NULL");
  assert_log(allocator != NULL, "Allocator is NULL");

  MemZero(batcher, sizeof(*batcher));

  uint32_t capacity = initial_capacity > 0 ? initial_capacity : 1;
  batcher->opaque_commands =
      vector_create_VkrDrawCommand_with_capacity(allocator, capacity);
  batcher->transparent_commands =
      vector_create_VkrDrawCommand_with_capacity(allocator, capacity);
  batcher->opaque_batches =
      vector_create_VkrDrawBatch_with_capacity(allocator, capacity);
  batcher->transparent_batches =
      vector_create_VkrDrawBatch_with_capacity(allocator, capacity);

  if (batcher->opaque_commands.data && batcher->transparent_commands.data &&
      batcher->opaque_batches.data && batcher->transparent_batches.data) {
    return true_v;
  }

  vkr_draw_batcher_shutdown(batcher);
  return false_v;
}

void vkr_draw_batcher_shutdown(VkrDrawBatcher *batcher) {
  assert_log(batcher != NULL, "Batcher is NULL");
  vector_destroy_VkrDrawCommand(&batcher->opaque_commands);
  vector_destroy_VkrDrawCommand(&batcher->transparent_commands);
  vector_destroy_VkrDrawBatch(&batcher->opaque_batches);
  vector_destroy_VkrDrawBatch(&batcher->transparent_batches);
  MemZero(batcher, sizeof(*batcher));
}

void vkr_draw_batcher_reset(VkrDrawBatcher *batcher) {
  assert_log(batcher != NULL, "Batcher is NULL");
  vector_clear_VkrDrawCommand(&batcher->opaque_commands);
  vector_clear_VkrDrawCommand(&batcher->transparent_commands);
  vector_clear_VkrDrawBatch(&batcher->opaque_batches);
  vector_clear_VkrDrawBatch(&batcher->transparent_batches);
  batcher->total_draws_collected = 0;
  batcher->batches_created = 0;
  batcher->draws_merged = 0;
}

void vkr_draw_batcher_add_opaque(VkrDrawBatcher *batcher,
                                 const VkrDrawCommand *cmd) {
  assert_log(batcher != NULL, "Batcher is NULL");
  assert_log(cmd != NULL, "Command is NULL");
  vector_push_VkrDrawCommand(&batcher->opaque_commands, *cmd);
  batcher->total_draws_collected += 1;
}

void vkr_draw_batcher_add_transparent(VkrDrawBatcher *batcher,
                                      const VkrDrawCommand *cmd) {
  assert_log(batcher != NULL, "Batcher is NULL");
  assert_log(cmd != NULL, "Command is NULL");
  vector_push_VkrDrawCommand(&batcher->transparent_commands, *cmd);
  batcher->total_draws_collected += 1;
}

void vkr_draw_batcher_finalize(VkrDrawBatcher *batcher) {
  assert_log(batcher != NULL, "Batcher is NULL");

  batcher->batches_created = 0;
  batcher->draws_merged = 0;

  if (batcher->opaque_commands.length > 1) {
    qsort(batcher->opaque_commands.data, batcher->opaque_commands.length,
          sizeof(VkrDrawCommand), vkr_draw_command_key_compare);
  }

  if (batcher->transparent_commands.length > 1) {
    qsort(batcher->transparent_commands.data,
          batcher->transparent_commands.length, sizeof(VkrDrawCommand),
          vkr_draw_command_distance_compare);
  }

  vkr_draw_batcher_build_batches(
      &batcher->opaque_commands, &batcher->opaque_batches, 0,
      &batcher->draws_merged, &batcher->batches_created);
  vkr_draw_batcher_build_batches(
      &batcher->transparent_commands, &batcher->transparent_batches,
      (uint32_t)batcher->opaque_commands.length, &batcher->draws_merged,
      &batcher->batches_created);
}

uint32_t vkr_draw_batcher_opaque_batch_count(const VkrDrawBatcher *batcher) {
  assert_log(batcher != NULL, "Batcher is NULL");
  return (uint32_t)batcher->opaque_batches.length;
}

const VkrDrawBatch *
vkr_draw_batcher_get_opaque_batch(const VkrDrawBatcher *batcher,
                                  uint32_t index) {
  assert_log(batcher != NULL, "Batcher is NULL");
  assert_log(index < batcher->opaque_batches.length,
             "Batch index out of bounds");
  return &batcher->opaque_batches.data[index];
}

const VkrDrawCommand *
vkr_draw_batcher_get_command(const VkrDrawBatcher *batcher, uint32_t index) {
  assert_log(batcher != NULL, "Batcher is NULL");
  assert_log(index < batcher->opaque_commands.length +
                         batcher->transparent_commands.length,
             "Command index out of bounds");
  return (index < batcher->opaque_commands.length)
             ? &batcher->opaque_commands.data[index]
             : &batcher->transparent_commands
                    .data[index - batcher->opaque_commands.length];
}
