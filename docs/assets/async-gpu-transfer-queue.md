---
status: proposed
updated: 2026-07-31
authority: design
---
# Async GPU Buffer Upload via Transfer Queue

## Overview

This document describes the implementation of asynchronous GPU buffer/image uploads using a dedicated Vulkan transfer queue. The goal is to improve texture and mesh loading performance by utilizing dedicated DMA hardware when available.

This remains a proposed independent-submission design. The shipped frame path
now records uploads into the active primary command buffer and retires staging
by submit serial without a render-thread wait; bootstrap and other out-of-frame
helpers still use the submit-and-wait flow described below.

## Architecture

### Transfer Queue Concept

Modern GPUs often have dedicated transfer/copy engines separate from the graphics engine. Vulkan exposes these as separate queue families:

```
┌─────────────────────────────────────────────────────────────────┐
│                         GPU Hardware                             │
├─────────────────────┬─────────────────────┬─────────────────────┤
│   Graphics Engine   │   Compute Engine    │   DMA/Copy Engine   │
│   (Queue Family 0)  │   (Queue Family 1)  │   (Queue Family 2)  │
└─────────────────────┴─────────────────────┴─────────────────────┘
```

By using the transfer queue for buffer-to-image copies, we can:
1. Utilize dedicated DMA hardware
2. Potentially overlap transfers with rendering
3. Reduce graphics queue contention

### Two-Phase Upload Pattern

When transfer and graphics queues are in different families, we use a two-phase upload:

```
┌─────────────────────────────────────────────────────────────────┐
│ Phase 1: Transfer Queue                                         │
│  • Transition image → TRANSFER_DST_OPTIMAL                      │
│  • Copy staging buffer → image (base level)                     │
│  • Release ownership barrier                                    │
│  • Submit & wait                                                │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│ Phase 2: Graphics Queue                                         │
│  • Acquire ownership barrier                                    │
│  • Generate mipmaps (if enabled)                                │
│  • Transition → SHADER_READ_ONLY_OPTIMAL                        │
│  • Submit & wait                                                │
└─────────────────────────────────────────────────────────────────┘
```

When both queues share the same family (common on some GPUs), we simplify to a single-phase upload on the graphics queue.

## Implementation Details

### Files Modified

| File | Changes |
|------|---------|
| `vulkan_types.h` | Added `transfer_command_pool` to `VulkanDevice` struct |
| `vulkan_device.c` | Creates/destroys transfer command pool, logs queue info |
| `vulkan_image.h` | Added 3 new upload functions |
| `vulkan_image.c` | Implemented transfer queue upload logic |
| `vulkan_backend.c` | Refactored texture creation to use new upload functions |

### New Data Structures

```c
// In vulkan_types.h
typedef struct VulkanDevice {
  // ... existing fields ...
  VkCommandPool graphics_command_pool;
  VkCommandPool transfer_command_pool;  // NEW: For async uploads

  int32_t graphics_queue_index;
  int32_t present_queue_index;
  int32_t transfer_queue_index;

  VkQueue graphics_queue;
  VkQueue present_queue;
  VkQueue transfer_queue;
  // ...
} VulkanDevice;
```

### New Functions

#### `vulkan_image_upload_with_mipmaps()`

Two-phase upload for regular textures with optional mipmap generation:

```c
bool8_t vulkan_image_upload_with_mipmaps(
    VulkanBackendState *state,
    VulkanImage *image,
    VkBuffer staging_buffer,
    VkFormat image_format,
    bool8_t generate_mipmaps);
```

**Flow:**
1. Allocate command buffer from transfer pool
2. Transition image to `TRANSFER_DST_OPTIMAL`
3. Copy staging buffer to image base level
4. If different queue families: add release barrier
5. Submit to transfer queue and wait
6. If mipmaps or different families:
   - Allocate graphics command buffer
   - Acquire ownership (if needed)
   - Generate mipmaps (uses `vkCmdBlitImage`)
   - Submit to graphics queue and wait

#### `vulkan_image_upload_cube_via_transfer()`

Uploads 6 cube map faces via transfer queue:

```c
bool8_t vulkan_image_upload_cube_via_transfer(
    VulkanBackendState *state,
    VulkanImage *image,
    VkBuffer staging_buffer,
    VkFormat image_format,
    uint64_t face_size);
```

#### `vulkan_image_upload_via_transfer()`

Simple upload without mipmaps (used internally):

```c
bool8_t vulkan_image_upload_via_transfer(
    VulkanBackendState *state,
    VulkanImage *image,
    VkBuffer staging_buffer,
    VkFormat image_format);
```

### Queue Family Ownership Transfer

Vulkan requires explicit ownership transfer between queue families:

```c
// RELEASE barrier (on transfer queue)
VkImageMemoryBarrier release_barrier = {
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = 0,
    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,  // Same layout
    .srcQueueFamilyIndex = transfer_queue_index,
    .dstQueueFamilyIndex = graphics_queue_index,
    // ...
};

// ACQUIRE barrier (on graphics queue)
VkImageMemoryBarrier acquire_barrier = {
    .srcAccessMask = 0,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    .srcQueueFamilyIndex = transfer_queue_index,
    .dstQueueFamilyIndex = graphics_queue_index,
    // ...
};
```

**Important:** The release must happen-before the acquire (Vulkan spec requirement).

### Code Simplification

The texture creation code was significantly simplified:

**Before:** ~150 lines of manual command buffer management, layout transitions, mipmap generation, and error handling.

**After:** ~30 lines calling `vulkan_image_upload_with_mipmaps()`.

```c
// NEW simplified texture creation
if (initial_data) {
    bool8_t generate_mipmaps =
        (texture->texture.image.mip_levels > 1) && linear_blit_supported;

    if (!vulkan_image_upload_with_mipmaps(state, &texture->texture.image,
                                          staging_buffer->buffer.handle,
                                          image_format, generate_mipmaps)) {
        log_fatal("Failed to upload texture via transfer queue");
        // ... cleanup ...
    }
}
```

## Performance Characteristics

### Benefits

| Scenario | Benefit |
|----------|---------|
| Discrete GPU (NVIDIA/AMD) | High - dedicated DMA engines, separate VRAM |
| Integrated GPU with dedicated transfer | Medium - reduces graphics queue contention |
| Apple Silicon (M1/M2) | Low - unified memory, no separate DMA |

### Platform-Specific Notes

#### Apple Silicon (M1/M2/M3)

```
Transfer queue: 0x16d47b000 (family 2, dedicated: yes)
```

Even though macOS reports a dedicated transfer queue family, Apple Silicon uses **unified memory architecture**. The GPU and CPU share the same RAM, so there's no actual DMA transfer between memory regions. The main performance gains on Apple Silicon come from:

1. **Job system parallelism** - Multiple workers decoding textures via `stbi_load_from_memory`
2. **Batch texture loading** - Loading multiple textures in parallel
3. **Async file I/O** - Reading files on worker threads

#### Discrete GPUs (NVIDIA/AMD)

On discrete GPUs with dedicated VRAM:
- Transfer queue uses dedicated DMA engines
- Data is physically copied from system RAM to VRAM
- Can overlap with graphics rendering for true async benefit

### Optimization Priorities by Platform

| Platform | Priority Optimizations |
|----------|----------------------|
| Apple Silicon | LZ4 compression, memory-mapped files, prefetching |
| Discrete GPU | Transfer queue (implemented), async staging buffers |
| All | Job system parallelism, batch loading |

## Debugging

### Log Output

The device initialization logs transfer queue info:

```
[DEBUG]: Created Vulkan graphics command pool: 0x30000000003
[DEBUG]: Created Vulkan transfer command pool: 0x30000000004
[DEBUG]: Graphics queue: 0x600000abc000
[DEBUG]: Present queue: 0x600000abc000
[DEBUG]: Transfer queue: 0x600000def000 (family 2, dedicated: yes)
```

### Common Validation Errors

**Error:** "no matching release operation was queued"
```
vkQueueSubmit(): pSubmits[0].pCommandBuffers[0] contains a VkImageMemoryBarrier
that acquires ownership of VkImage 0xa700000000a7 for destination queue family 0,
but no matching release operation was queued for execution from source queue family 2.
```

**Fix:** Ensure release barrier is added on transfer queue before acquire on graphics queue.

## Future Improvements

1. **True Async Uploads** - Use semaphores instead of fences to allow rendering while transfers are in-flight
2. **Staging Buffer Pool** - Reuse staging buffers to reduce allocation overhead
3. **Transfer Batching** - Batch multiple small uploads into single command buffer
4. **Timeline Semaphores** - More flexible synchronization for overlapping uploads

## References

- [Vulkan Spec: Queue Family Ownership Transfer](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#synchronization-queue-transfers)
- [AMD: Async Compute and Transfer](https://gpuopen.com/learn/concurrent-execution-asynchronous-queues/)
- [NVIDIA: Vulkan Memory Management](https://developer.nvidia.com/vulkan-memory-management)
