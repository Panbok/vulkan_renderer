#pragma once

#include "defines.h"
#include "math/mat.h"
#include "renderer/vkr_renderer.h"

/** Maximum instance slots per frame. */
#define VKR_INSTANCE_BUFFER_MAX_INSTANCES 65536
/** Number of buffered instance streams. */
#define VKR_INSTANCE_BUFFER_FRAMES 3

/**
 * @brief GPU-visible instance data layout for instanced draws.
 *
 * Matches std430 layout requirements with explicit padding.
 */
typedef struct VkrInstanceDataGPU {
  Mat4 model;
  uint32_t object_id;
  uint32_t material_index;
  uint32_t flags;
  uint32_t _padding;
} VkrInstanceDataGPU;

_Static_assert(sizeof(VkrInstanceDataGPU) == 80,
               "VkrInstanceDataGPU must be 80 bytes");
_Static_assert(sizeof(VkrInstanceDataGPU) % 16 == 0,
               "VkrInstanceDataGPU must be 16-byte aligned");

/**
 * @brief Instance buffer for instanced draws.
 *
 * @param buffer The buffer handle.
 * @param mapped_ptr The mapped pointer to the instance data.
 * @param capacity The capacity of the buffer.
 * @param write_offset The current write offset.
 * @param needs_flush Whether the buffer needs to be flushed.
 */
typedef struct VkrInstanceBuffer {
  VkrBufferHandle buffer;
  VkrInstanceDataGPU *mapped_ptr; /**< Mapped pointer to the instance data. */
  uint32_t capacity;              /**< The capacity of the buffer. */
  uint32_t write_offset;          /**< The current write offset. */
  bool8_t needs_flush; /**< Whether the buffer needs to be flushed. */
} VkrInstanceBuffer;

/**
 * @brief Instance buffer pool for instanced draws.
 *
 * @param buffers The buffers.
 * @param renderer The renderer.
 * @param current_frame The current frame.
 * @param max_instances The maximum number of instances.
 * @param initialized Whether the pool is initialized.
 */
typedef struct VkrInstanceBufferPool {
  VkrInstanceBuffer buffers[VKR_INSTANCE_BUFFER_FRAMES]; /**< The buffers. */
  VkrRendererFrontendHandle renderer;                    /**< The renderer. */
  uint32_t current_frame; /**< The current frame. */
  uint32_t max_instances; /**< The maximum number of instances. */
  bool8_t initialized;    /**< Whether the pool is initialized. */
} VkrInstanceBufferPool;

/**
 * @brief Initialize the instance buffer pool.
 *
 * @param pool The pool.
 * @param renderer The renderer.
 * @param max_instances The maximum number of instances.
 * @return true if the pool is initialized, false otherwise.
 */
bool8_t vkr_instance_buffer_pool_init(VkrInstanceBufferPool *pool,
                                      VkrRendererFrontendHandle renderer,
                                      uint32_t max_instances);
/**
 * @brief Shutdown the instance buffer pool.
 *
 * @param pool The pool.
 * @param renderer The renderer.
 */
void vkr_instance_buffer_pool_shutdown(VkrInstanceBufferPool *pool,
                                       VkrRendererFrontendHandle renderer);

/**
 * @brief Begin the instance buffer frame.
 *
 * @param pool The pool.
 * @param frame_index The frame index.
 */
void vkr_instance_buffer_begin_frame(VkrInstanceBufferPool *pool,
                                     uint32_t frame_index);

/**
 * @brief Allocate instance data from the instance buffer.
 *
 * @param pool The pool.
 * @param count The number of instances to allocate.
 * @param out_base_instance The base instance.
 * @param out_ptr The output pointer to the instance data.
 * @return true if the allocation is successful, false otherwise.
 */
bool8_t vkr_instance_buffer_alloc(VkrInstanceBufferPool *pool, uint32_t count,
                                  uint32_t *out_base_instance,
                                  VkrInstanceDataGPU **out_ptr);

/**
 * @brief Flush the instance buffer range.
 *
 * @param pool The pool.
 * @param base_instance The base instance.
 * @param count The number of instances to flush.
 */
void vkr_instance_buffer_flush_range(VkrInstanceBufferPool *pool,
                                     uint32_t base_instance, uint32_t count);

/**
 * @brief Flush the current instance buffer.
 *
 * @param pool The pool.
 */
void vkr_instance_buffer_flush_current(VkrInstanceBufferPool *pool);

/**
 * @brief Get the current instance buffer.
 *
 * @param pool The pool.
 * @return The current instance buffer.
 */
VkrBufferHandle
vkr_instance_buffer_get_current(const VkrInstanceBufferPool *pool);
