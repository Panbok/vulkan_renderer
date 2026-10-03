/**
 * @file vkr_publication_queue.h
 * @brief Ordered asset-publication commands and their completions.
 *
 * The frame-loop thread records publications through the renderer's
 * VkrAssetPublisher. The thread that renders the next frame runs them in
 * order, inside that frame when it was acquired, before preparing the frame's
 * input; results return as completions the frame-loop thread drains after the
 * frame completes (ADR-082).
 */
#pragma once

#include "memory/arena.h"
#include "vkr_asset_publisher.h"
#include "vkr_geometry_upload.h"
#include "vkr_texture_upload.h"

typedef struct VkrRenderer VkrRenderer;

typedef struct VkrPublicationCommand VkrPublicationCommand;
typedef struct VkrPublicationBlock VkrPublicationBlock;

/** Commands recorded between two frames, with copies of their payloads.
 * Command records and small copies live in `arena`; vertex, index and texture
 * bytes are heap blocks freed once the batch has run, so a loading burst does
 * not stay committed in the arena. */
typedef struct VkrPublicationBatch {
  Arena *arena;
  VkrPublicationCommand *head;
  VkrPublicationCommand *tail;
  VkrPublicationBlock *blocks;
  uint32_t count;
} VkrPublicationBatch;

/** Completions one batch produced, in command order. */
typedef struct VkrPublicationCompletionList {
  Arena *arena;
  VkrPublicationCompletion *items;
  uint32_t count;
  uint32_t capacity;
  /* Next completion vkr_publication_queue_poll() returns. */
  uint32_t cursor;
} VkrPublicationCompletionList;

/** A bake source the render thread reports on until it is unpublished. */
typedef struct VkrPublicationSource {
  VkrTextureHandle source;
  VkrAtmosphereBakeStatus atmosphere_status;
  uint32_t sh_slot;
  bool8_t atmosphere;
} VkrPublicationSource;

#define VKR_PUBLICATION_SOURCE_MAX 128u

/** Renderer state asset code reads each frame without waiting: what the
 * thread that rendered the last frame observed. */
typedef struct VkrPublicationSnapshot {
  bool8_t idle;
  uint64_t generation;
  VkrPublicationSource sources[VKR_PUBLICATION_SOURCE_MAX];
  uint32_t source_count;
} VkrPublicationSnapshot;

/**
 * Two batches alternate: the frame-loop thread records into `recording`
 * while the render thread runs the other. They swap when a frame is
 * submitted, while no frame is rendering. Completions and the snapshot
 * likewise have a render-side and a frame-loop-side copy, exchanged when the
 * frame-loop thread collects a frame.
 */
typedef struct VkrPublicationQueue {
  VkrPublicationBatch batches[2];
  VkrPublicationBatch *recording;
  VkrPublicationCompletionList completions[2];
  /* Written while a batch runs; read by the frame-loop thread. */
  VkrPublicationCompletionList *produced;
  VkrPublicationCompletionList *delivered;
  VkrPublicationSnapshot render_snapshot;
  VkrPublicationSnapshot snapshot;
  /* An open texture upload batch records its begin marker with its first
     texture, so an empty batch records nothing. */
  bool8_t texture_batch_open;
  bool8_t texture_batch_recorded;
} VkrPublicationQueue;

bool8_t vkr_publication_queue_create(VkrPublicationQueue *queue);
void vkr_publication_queue_destroy(VkrPublicationQueue *queue);

/** Fills `publisher` with functions that record into `renderer`'s queue. */
void vkr_publication_queue_publisher(VkrRenderer *renderer,
                                     VkrAssetPublisher *publisher);

/** Takes the recorded batch for the next frame's work and starts a new one.
 * Call while no frame is rendering. Returns NULL when nothing was recorded. */
VkrPublicationBatch *vkr_publication_queue_take(VkrPublicationQueue *queue);

/** Runs `batch` in order through the renderer's native publisher, inside the
 * acquired frame when there is one, and appends its completions. Runs on the
 * thread rendering the frame. */
void vkr_publication_queue_run(VkrRenderer *renderer,
                               VkrPublicationBatch *batch);

/** Refreshes the render-side snapshot after a frame. */
void vkr_publication_queue_observe(VkrRenderer *renderer);

/** Hands produced completions and the render-side snapshot to the
 * frame-loop thread. Call while no frame is rendering. */
void vkr_publication_queue_deliver(VkrPublicationQueue *queue);
