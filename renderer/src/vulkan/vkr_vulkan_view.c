/* View streaming on the desktop pipeline (ADR-131). A frame with a view
 * export resamples its finished Scene image into an NV12 picture
 * (shaders/vulkan/slang/editor/view_export.slang) after its graph, and a
 * Vulkan Video H.264 session encodes that picture on the device's encode
 * queue once the frame's submission signals the renderer's timeline. A
 * thread of the encoder's waits for each encode in submission order, hands
 * its Annex B packet to the export's `on_packet` and reports the export.
 *
 * Ownership: the renderer owns the view state for its lifetime. A session
 * (video session and parameters, DPB, pictures, bitstream buffers, feedback
 * queries, encode command buffers, timeline and thread) is made by the first
 * export of an extent and frame rate on the rendering thread, and destroyed
 * there once every slot has come back: when the extent or frame rate
 * changes, after VKR_VK_VIEW_IDLE_SECONDS without an export, and at renderer
 * destroy after the device went idle. A slot belongs to the rendering thread
 * from its choice until its encode is submitted, then to the encoder thread
 * until its export is reported; `busy` hands it back. Video images and
 * buffers take memory outside the renderer's pools, because each one chains
 * the session's profile list. */

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_threads.h"
#include "math/vkr_math.h"
#include "memory/vkr_heap.h"
#include "platform/vkr_platform.h"
#include "vkr_gpu_slot_table.h"
#include "vulkan/vkr_vulkan_internal.h"

/* Encodes in flight: the editor hands out four exports at most. */
#define VKR_VK_VIEW_SLOTS 4u
/* The reconstructed picture and the one it references. */
#define VKR_VK_VIEW_DPB_SLOTS 2u
#define VKR_VK_VIEW_SESSION_BINDINGS 8u
#define VKR_VK_VIEW_FORMATS 8u
/* One thread writes a 2x2 block: four luma samples and one chroma pair. */
#define VKR_VK_VIEW_GROUP_SIZE 8u
#define VKR_VK_VIEW_PICTURE_FORMAT VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
/* A session no export used this long gives its memory back. */
#define VKR_VK_VIEW_IDLE_SECONDS 2.0
/* frame_num counts to 2^(4 + 12) before it wraps. */
#define VKR_VK_VIEW_LOG2_MAX_FRAME_NUM_MINUS4 12u
/* The encoder thread's wait for one submission, in slices, before it
   reports the export unwritten: the renderer itself gives up on its
   timeline after five seconds. */
#define VKR_VK_VIEW_WAIT_SLICE_NS 250000000ull
#define VKR_VK_VIEW_WAIT_SLICES 20u
/* Bytes beyond a raw 4:2:0 picture that a bitstream buffer keeps for slice
   headers. */
#define VKR_VK_VIEW_BITSTREAM_SLACK 4096u

/* Mirrors VkrVkViewExportRoot in
   shaders/vulkan/slang/editor/view_export.slang. */
typedef struct VkrVulkanViewExportRoot {
  uint32_t source_texture;
  uint32_t source_sampler;
  uint32_t luma_image;
  uint32_t chroma_image;
  uint32_t extent[2];
  uint32_t coded_extent[2];
  /* 1 when the Scene image stores display-encoded values. */
  uint32_t source_encoded;
  uint32_t reserved[3];
} VkrVulkanViewExportRoot;

_Static_assert(sizeof(VkrVulkanViewExportRoot) == 48u,
               "Vulkan view export root must remain 48 bytes");

typedef struct VkrVkViewSlot {
  /* The NV12 picture the export writes and the encode reads; both queue
     families use it concurrently. */
  VkImage picture;
  VkDeviceMemory picture_memory;
  VkImageView picture_view;
  VkImageView luma_view;
  VkImageView chroma_view;
  VkrGpuSlotHandle luma_slot;
  VkrGpuSlotHandle chroma_slot;
  VkBuffer bitstream;
  VkDeviceMemory bitstream_memory;
  const uint8_t *bitstream_mapped;
  VkDeviceSize bitstream_size;
  bool8_t bitstream_coherent;
  VkCommandBuffer command;
  /* Nonzero from the slot's choice until its export is reported. */
  VkrAtomicUint32 busy;
  /* Written by the rendering thread before the slot joins the encoder
     thread's queue under its mutex. */
  VkrViewExport export;
  uint64_t frame_value;
  uint64_t encode_value;
  bool8_t key_frame;
} VkrVkViewSlot;

/* What one encode writes: its picture's place in the stream, chosen on the
   rendering thread and committed once the encode is submitted. */
typedef struct VkrVkViewPicture {
  bool8_t key_frame;
  uint32_t setup_dpb;
  /* Pictures since the last IDR picture, which is picture 0. */
  uint64_t index;
  uint16_t idr_pic_id;
  uint32_t bitrate;
  StdVideoEncodeH264ReferenceInfo reference;
} VkrVkViewPicture;

typedef struct VkrVkViewSession {
  struct s_VkrVulkanView *view;
  uint32_t width;
  uint32_t height;
  uint32_t frame_rate;
  uint32_t coded_width;
  uint32_t coded_height;
  VkVideoSessionKHR session;
  VkDeviceMemory session_memory[VKR_VK_VIEW_SESSION_BINDINGS];
  uint32_t session_memory_count;
  VkVideoSessionParametersKHR parameters;
  /* The encoded SPS and PPS, Annex B, which every key frame carries. */
  uint8_t *parameter_sets;
  uint32_t parameter_set_size;
  VkImage dpb;
  VkDeviceMemory dpb_memory;
  VkImageView dpb_view;
  VkQueryPool feedback;
  VkCommandPool command_pool;
  /* Signalled by each encode with its slot's `encode_value`. */
  VkSemaphore encoded;
  VkrVkViewSlot slots[VKR_VK_VIEW_SLOTS];
  uint32_t next_slot;

  /* Coding state; rendering thread. */
  uint64_t encode_value;
  bool8_t coding_started;
  bool8_t dpb_initialized;
  uint32_t rate_bitrate;
  bool8_t reference_valid;
  uint32_t reference_dpb;
  bool8_t dpb_active[VKR_VK_VIEW_DPB_SLOTS];
  StdVideoEncodeH264ReferenceInfo dpb_reference[VKR_VK_VIEW_DPB_SLOTS];
  uint64_t picture_index;
  uint16_t idr_pic_id;
  float64_t last_export_time;

  /* The encoder thread and its queue of submitted slots, in submission
     order; `mutex` guards the queue and `quit`. */
  VkrThread thread;
  bool8_t thread_running;
  VkrMutex mutex;
  VkrCondVar cond;
  uint32_t queue[VKR_VK_VIEW_SLOTS];
  uint32_t queue_first;
  uint32_t queue_count;
  bool8_t quit;
  /* Encoder thread only: a key frame's parameter sets and slice data, and
     whether the encoder writes start codes before its slices. */
  uint8_t *packet;
  uint64_t packet_capacity;
  bool8_t start_code_checked;
  bool8_t start_code_missing;
} VkrVkViewSession;

struct s_VkrVulkanView {
  VkrVulkanRenderer *renderer;
  bool8_t supported;
  /* The one H.264 profile every video object chains; stable addresses. */
  VkVideoEncodeUsageInfoKHR usage;
  VkVideoEncodeH264ProfileInfoKHR h264_profile;
  VkVideoProfileInfoKHR profile;
  VkVideoProfileListInfoKHR profile_list;
  VkVideoCapabilitiesKHR capabilities;
  VkVideoEncodeCapabilitiesKHR encode_capabilities;
  VkVideoEncodeH264CapabilitiesKHR h264_capabilities;
  VkVideoEncodeRateControlModeFlagBitsKHR rate_control_mode;
  VkrVkViewSession *session;
  /* The extent and frame rate whose session could not be made, so the
     next export of them does not try again. */
  uint32_t failed[3];
  /* The reasons an export went unwritten that were logged, one bit each,
     so each is logged once, and whether any export arrived. */
  uint32_t logged_reasons;
  bool8_t export_seen;

  /* The frame's export, taken when its submission starts, and what the
     frame prepared and recorded for it; rendering thread. */
  bool8_t has_pending;
  VkrViewExport pending;
  bool8_t prepared;
  bool8_t recorded;
  uint32_t slot_index;
  uint64_t root_address;
  uint32_t groups[2];
  VkImage scene_image;
  VkImageLayout scene_layout;
};

// =============================================================================
// Helpers
// =============================================================================

vkr_internal void vkr_vk_view_report(const VkrViewExport *export,
                                     bool8_t written) {
  if (export->done) {
    export->done(export->context, export->tag, written);
  }
}

/* Why an export goes unwritten before its encode, each logged once. */
typedef enum VkrVkViewReason {
  VKR_VK_VIEW_REASON_NO_SCENE = 0,
  VKR_VK_VIEW_REASON_NO_SESSION,
  VKR_VK_VIEW_REASON_SLOTS_BUSY,
  VKR_VK_VIEW_REASON_NO_ROOT,
  VKR_VK_VIEW_REASON_NOT_RECORDED,
} VkrVkViewReason;

vkr_internal void vkr_vk_view_note(VkrVulkanView *view,
                                   VkrVkViewReason reason) {
  vkr_local_persist const char *const reasons[] = {
      "the frame rendered no sampled Scene image",
      "no encoder session for its extent and frame rate",
      "every encode slot was still busy",
      "the frame's upload ring was full",
      "the frame recorded no export",
  };
  const uint32_t bit = 1u << (uint32_t)reason;
  if (view->logged_reasons & bit) {
    return;
  }
  view->logged_reasons |= bit;
  log_info("Vulkan view streaming: an export went unwritten: %s (logged "
           "once)",
           reasons[reason]);
}

/* Reports the frame's export unwritten and gives its slot back. */
vkr_internal void vkr_vk_view_reject(VkrVulkanView *view) {
  if (!view->has_pending) {
    return;
  }
  if (view->prepared && view->session) {
    vkr_atomic_uint32_store(&view->session->slots[view->slot_index].busy, 0u,
                            VKR_MEMORY_ORDER_RELEASE);
  }
  view->has_pending = false_v;
  view->prepared = false_v;
  view->recorded = false_v;
  const VkrViewExport export = view->pending;
  view->pending = (VkrViewExport){0};
  vkr_vk_view_report(&export, false_v);
}

/* A memory type with `required`, preferring one that also has
   `preferred`; UINT32_MAX without one. */
vkr_internal uint32_t vkr_vk_view_memory_type(const VkrVulkanRenderer *renderer,
                                              uint32_t type_bits,
                                              VkMemoryPropertyFlags required,
                                              VkMemoryPropertyFlags preferred) {
  const VkPhysicalDeviceMemoryProperties *memory =
      vkr_vulkan_device_memory_properties(renderer->device);
  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0u; i < memory->memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags flags = memory->memoryTypes[i].propertyFlags;
    if (!(type_bits & (1u << i)) || (flags & required) != required) {
      continue;
    }
    if ((flags & preferred) == preferred) {
      return i;
    }
    if (fallback == UINT32_MAX) {
      fallback = i;
    }
  }
  return fallback;
}

vkr_internal bool8_t vkr_vk_view_allocate(
    const VkrVulkanRenderer *renderer, const VkMemoryRequirements *requirements,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
    VkDeviceMemory *out_memory, VkMemoryPropertyFlags *out_flags) {
  *out_memory = VK_NULL_HANDLE;
  const uint32_t type = vkr_vk_view_memory_type(
      renderer, requirements->memoryTypeBits, required, preferred);
  if (type == UINT32_MAX) {
    return false_v;
  }
  const VkMemoryAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements->size,
      .memoryTypeIndex = type,
  };
  if (vkAllocateMemory(vkr_vk_renderer_device(renderer), &info, NULL,
                       out_memory) != VK_SUCCESS) {
    *out_memory = VK_NULL_HANDLE;
    return false_v;
  }
  if (out_flags) {
    *out_flags = vkr_vulkan_device_memory_properties(renderer->device)
                     ->memoryTypes[type]
                     .propertyFlags;
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_view_create_image(const VkrVulkanRenderer *renderer,
                                              const VkImageCreateInfo *info,
                                              VkImage *out_image,
                                              VkDeviceMemory *out_memory) {
  const VkDevice device = vkr_vk_renderer_device(renderer);
  if (vkCreateImage(device, info, NULL, out_image) != VK_SUCCESS) {
    *out_image = VK_NULL_HANDLE;
    return false_v;
  }
  VkMemoryRequirements requirements;
  vkGetImageMemoryRequirements(device, *out_image, &requirements);
  return vkr_vk_view_allocate(renderer, &requirements, 0u,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out_memory,
                              NULL) &&
         vkBindImageMemory(device, *out_image, *out_memory, 0u) == VK_SUCCESS;
}

vkr_internal bool8_t vkr_vk_view_create_view(
    const VkrVulkanRenderer *renderer, VkImage image, VkImageViewType type,
    VkFormat format, VkImageAspectFlags aspect, uint32_t layer_count,
    VkImageUsageFlags usage, VkImageView *out_view) {
  const VkImageViewUsageCreateInfo usage_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
      .usage = usage,
  };
  const VkImageViewCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = &usage_info,
      .image = image,
      .viewType = type,
      .format = format,
      .subresourceRange = {.aspectMask = aspect,
                           .levelCount = 1u,
                           .layerCount = layer_count},
  };
  if (vkCreateImageView(vkr_vk_renderer_device(renderer), &info, NULL,
                        out_view) != VK_SUCCESS) {
    *out_view = VK_NULL_HANDLE;
    return false_v;
  }
  return true_v;
}

/* Whether the profile takes NV12 pictures with `usage`, from an image
   created with at least `flags`. */
vkr_internal bool8_t vkr_vk_view_format_supported(
    const VkrVulkanView *view, const VkrVulkanVideoEncodeFunctions *video,
    VkImageUsageFlags usage, VkImageCreateFlags flags) {
  const VkPhysicalDeviceVideoFormatInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
      .pNext = &view->profile_list,
      .imageUsage = usage,
  };
  const VkPhysicalDevice physical =
      vkr_vulkan_device_physical(view->renderer->device);
  VkVideoFormatPropertiesKHR formats[VKR_VK_VIEW_FORMATS];
  for (uint32_t i = 0u; i < ArrayCount(formats); ++i) {
    formats[i] = (VkVideoFormatPropertiesKHR){
        .sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR,
    };
  }
  uint32_t count = ArrayCount(formats);
  const VkResult result =
      video->get_format_properties(physical, &info, &count, formats);
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    return false_v;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (formats[i].format == VKR_VK_VIEW_PICTURE_FORMAT &&
        formats[i].imageTiling == VK_IMAGE_TILING_OPTIMAL &&
        (formats[i].imageCreateFlags & flags) == flags) {
      return true_v;
    }
  }
  return false_v;
}

/* The lowest level from 4.1 that holds `macroblocks` per picture at
   `frame_rate`, within the device's maximum. Every level from 4.1 allows
   High-profile bit rates above the editor's 40 Mbit/s cap. */
vkr_internal StdVideoH264LevelIdc vkr_vk_view_level(const VkrVulkanView *view,
                                                    uint32_t macroblocks,
                                                    uint32_t frame_rate) {
  vkr_local_persist const struct {
    StdVideoH264LevelIdc level;
    uint32_t max_frame_macroblocks;
    uint32_t max_macroblocks_per_second;
  } levels[] = {
      {STD_VIDEO_H264_LEVEL_IDC_4_1, 8192u, 245760u},
      {STD_VIDEO_H264_LEVEL_IDC_4_2, 8704u, 522240u},
      {STD_VIDEO_H264_LEVEL_IDC_5_0, 22080u, 589824u},
      {STD_VIDEO_H264_LEVEL_IDC_5_1, 36864u, 983040u},
      {STD_VIDEO_H264_LEVEL_IDC_5_2, 36864u, 2073600u},
  };
  const uint64_t rate = (uint64_t)macroblocks * frame_rate;
  StdVideoH264LevelIdc level = levels[ArrayCount(levels) - 1u].level;
  for (uint32_t i = 0u; i < ArrayCount(levels); ++i) {
    if (macroblocks <= levels[i].max_frame_macroblocks &&
        rate <= levels[i].max_macroblocks_per_second) {
      level = levels[i].level;
      break;
    }
  }
  return Min(level, view->h264_capabilities.maxLevelIdc);
}

vkr_internal bool8_t vkr_vk_view_slots_free(const VkrVkViewSession *session) {
  for (uint32_t i = 0u; i < VKR_VK_VIEW_SLOTS; ++i) {
    if (vkr_atomic_uint32_load(&session->slots[i].busy,
                               VKR_MEMORY_ORDER_ACQUIRE)) {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_view_starts_with_start_code(const uint8_t *data,
                                                        uint64_t size) {
  return (size >= 3u && data[0] == 0u && data[1] == 0u && data[2] == 1u) ||
         (size >= 4u && data[0] == 0u && data[1] == 0u && data[2] == 0u &&
          data[3] == 1u);
}

// =============================================================================
// Encoder thread
// =============================================================================

/* Waits for `value` in slices; VK_TIMEOUT after the last one. */
vkr_internal VkResult vkr_vk_view_wait(VkDevice device, VkSemaphore semaphore,
                                       uint64_t value) {
  const VkSemaphoreWaitInfo info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1u,
      .pSemaphores = &semaphore,
      .pValues = &value,
  };
  VkResult result = VK_TIMEOUT;
  for (uint32_t i = 0u; i < VKR_VK_VIEW_WAIT_SLICES && result == VK_TIMEOUT;
       ++i) {
    result = vkWaitSemaphores(device, &info, VKR_VK_VIEW_WAIT_SLICE_NS);
  }
  return result;
}

/* Hands a finished encode's packet to the export: the slice data as the
   encoder wrote it, after the parameter sets on a key frame. False when the
   feedback reports no bytes. */
vkr_internal bool8_t vkr_vk_view_send_packet(VkrVkViewSession *session,
                                             VkrVkViewSlot *slot,
                                             uint32_t slot_index,
                                             uint64_t written_us) {
  const VkDevice device = vkr_vk_renderer_device(session->view->renderer);
  /* Written in the order of the feedback flags: offset, then bytes. */
  uint32_t feedback[2] = {0u, 0u};
  if (vkGetQueryPoolResults(device, session->feedback, slot_index, 1u,
                            sizeof(feedback), feedback, sizeof(feedback),
                            0u) != VK_SUCCESS) {
    return false_v;
  }
  const uint64_t offset = feedback[0];
  const uint64_t size = feedback[1];
  if (!size || offset > slot->bitstream_size ||
      size > slot->bitstream_size - offset) {
    return false_v;
  }
  if (!slot->bitstream_coherent) {
    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = slot->bitstream_memory,
        .size = VK_WHOLE_SIZE,
    };
    if (vkInvalidateMappedMemoryRanges(device, 1u, &range) != VK_SUCCESS) {
      return false_v;
    }
  }
  const uint8_t *data = slot->bitstream_mapped + offset;
  if (!session->start_code_checked) {
    session->start_code_checked = true_v;
    session->start_code_missing =
        !vkr_vk_view_starts_with_start_code(data, size);
    if (session->start_code_missing) {
      log_warn("Vulkan view streaming: the encoder writes slices without "
               "start codes; adding them");
    }
  }
  vkr_local_persist const uint8_t start_code[] = {0u, 0u, 0u, 1u};
  const uint64_t prefix = session->start_code_missing ? sizeof(start_code) : 0u;
  VkrVideoPacket packet = {
      .data = data,
      .size = size,
      .timestamp_us = written_us,
      .tag = slot->export.tag,
      .key_frame = slot->key_frame,
  };
  if (slot->key_frame || prefix) {
    const uint64_t parameters =
        slot->key_frame ? session->parameter_set_size : 0u;
    const uint64_t total = parameters + prefix + size;
    if (total > session->packet_capacity) {
      return false_v;
    }
    MemCopy(session->packet, session->parameter_sets, parameters);
    MemCopy(session->packet + parameters, start_code, prefix);
    MemCopy(session->packet + parameters + prefix, data, size);
    packet.data = session->packet;
    packet.size = total;
  }
  slot->export.on_packet(slot->export.context, &packet);
  return true_v;
}

/* Waits for the slot's frame and encode, then delivers its packet and
   reports the export. */
vkr_internal void vkr_vk_view_deliver(VkrVkViewSession *session,
                                      uint32_t slot_index) {
  VkrVulkanRenderer *renderer = session->view->renderer;
  VkrVkViewSlot *slot = &session->slots[slot_index];
  const VkDevice device = vkr_vk_renderer_device(renderer);
  bool8_t written = false_v;
  const VkResult frame_result =
      vkr_vk_view_wait(device, renderer->timeline, slot->frame_value);
  if (frame_result == VK_SUCCESS) {
    const uint64_t written_us =
        (uint64_t)(vkr_platform_get_absolute_time() * 1.0e6);
    const VkResult encode_result =
        vkr_vk_view_wait(device, session->encoded, slot->encode_value);
    written = encode_result == VK_SUCCESS &&
              vkr_vk_view_send_packet(session, slot, slot_index, written_us);
    if (encode_result != VK_SUCCESS) {
      log_error("Vulkan view streaming: an encode did not finish (result=%d)",
                (int)encode_result);
    }
  } else {
    log_error("Vulkan view streaming: an exported frame did not finish "
              "(result=%d)",
              (int)frame_result);
  }
  vkr_vk_view_report(&slot->export, written);
}

vkr_internal void *vkr_vk_view_encoder_main(void *argument) {
  VkrVkViewSession *session = argument;
  (void)vkr_thread_set_name("vkr.view_encode");
  for (;;) {
    vkr_mutex_lock(session->mutex);
    while (!session->quit && session->queue_count == 0u) {
      vkr_cond_wait(session->cond, session->mutex);
    }
    /* A stop still delivers every submitted slot first. */
    if (session->queue_count == 0u) {
      vkr_mutex_unlock(session->mutex);
      return NULL;
    }
    const uint32_t slot_index = session->queue[session->queue_first];
    session->queue_first = (session->queue_first + 1u) % VKR_VK_VIEW_SLOTS;
    session->queue_count -= 1u;
    vkr_mutex_unlock(session->mutex);
    vkr_vk_view_deliver(session, slot_index);
    vkr_atomic_uint32_store(&session->slots[slot_index].busy, 0u,
                            VKR_MEMORY_ORDER_RELEASE);
  }
}

// =============================================================================
// Session
// =============================================================================

/* Every submitted slot must have been delivered or be deliverable: the
   thread drains its queue before it stops. */
vkr_internal void vkr_vk_view_session_destroy(VkrVulkanView *view,
                                              VkrVkViewSession *session) {
  if (!session) {
    return;
  }
  VkrVulkanRenderer *renderer = view->renderer;
  const VkDevice device = vkr_vk_renderer_device(renderer);
  const VkrVulkanVideoEncodeFunctions *video =
      vkr_vulkan_device_video_encode(renderer->device);
  if (session->thread_running) {
    vkr_mutex_lock(session->mutex);
    session->quit = true_v;
    vkr_cond_signal(session->cond);
    vkr_mutex_unlock(session->mutex);
    (void)vkr_thread_join(session->thread);
    (void)vkr_thread_destroy(renderer->allocator, &session->thread);
  }
  if (session->cond) {
    (void)vkr_cond_destroy(renderer->allocator, &session->cond);
  }
  if (session->mutex) {
    (void)vkr_mutex_destroy(renderer->allocator, &session->mutex);
  }
  for (uint32_t i = 0u; i < VKR_VK_VIEW_SLOTS; ++i) {
    VkrVkViewSlot *slot = &session->slots[i];
    const VkrGpuSlotHandle handles[] = {slot->luma_slot, slot->chroma_slot};
    for (uint32_t h = 0u; h < ArrayCount(handles); ++h) {
      if (handles[h].generation && renderer->storage_image_slots &&
          vkr_gpu_slot_table_retire(renderer->storage_image_slots, handles[h],
                                    renderer->submit_value) !=
              VKR_GPU_SLOT_STATUS_OK) {
        log_error("Vulkan view streaming failed to retire a picture "
                  "descriptor");
      }
    }
    if (slot->luma_view) {
      vkDestroyImageView(device, slot->luma_view, NULL);
    }
    if (slot->chroma_view) {
      vkDestroyImageView(device, slot->chroma_view, NULL);
    }
    if (slot->picture_view) {
      vkDestroyImageView(device, slot->picture_view, NULL);
    }
    if (slot->picture) {
      vkDestroyImage(device, slot->picture, NULL);
    }
    if (slot->picture_memory) {
      vkFreeMemory(device, slot->picture_memory, NULL);
    }
    if (slot->bitstream) {
      vkDestroyBuffer(device, slot->bitstream, NULL);
    }
    if (slot->bitstream_memory) {
      vkFreeMemory(device, slot->bitstream_memory, NULL);
    }
  }
  if (session->command_pool) {
    vkDestroyCommandPool(device, session->command_pool, NULL);
  }
  if (session->encoded) {
    vkDestroySemaphore(device, session->encoded, NULL);
  }
  if (session->feedback) {
    vkDestroyQueryPool(device, session->feedback, NULL);
  }
  if (session->dpb_view) {
    vkDestroyImageView(device, session->dpb_view, NULL);
  }
  if (session->dpb) {
    vkDestroyImage(device, session->dpb, NULL);
  }
  if (session->dpb_memory) {
    vkFreeMemory(device, session->dpb_memory, NULL);
  }
  if (session->parameters) {
    video->destroy_parameters(device, session->parameters, NULL);
  }
  if (session->session) {
    video->destroy_session(device, session->session, NULL);
  }
  for (uint32_t i = 0u; i < session->session_memory_count; ++i) {
    vkFreeMemory(device, session->session_memory[i], NULL);
  }
  vkr_heap_free(session->parameter_sets);
  vkr_heap_free(session->packet);
  vkr_heap_free(session);
}

vkr_internal bool8_t vkr_vk_view_create_session_object(
    VkrVulkanView *view, VkrVkViewSession *session) {
  const VkrVulkanRenderer *renderer = view->renderer;
  const VkDevice device = vkr_vk_renderer_device(renderer);
  const VkrVulkanVideoEncodeFunctions *video =
      vkr_vulkan_device_video_encode(renderer->device);
  const VkVideoSessionCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
      .queueFamilyIndex = video->queue_family_index,
      .pVideoProfile = &view->profile,
      .pictureFormat = VKR_VK_VIEW_PICTURE_FORMAT,
      .maxCodedExtent = {session->coded_width, session->coded_height},
      .referencePictureFormat = VKR_VK_VIEW_PICTURE_FORMAT,
      .maxDpbSlots = VKR_VK_VIEW_DPB_SLOTS,
      .maxActiveReferencePictures = 1u,
      .pStdHeaderVersion = &view->capabilities.stdHeaderVersion,
  };
  if (video->create_session(device, &info, NULL, &session->session) !=
      VK_SUCCESS) {
    session->session = VK_NULL_HANDLE;
    return false_v;
  }
  uint32_t count = 0u;
  if (video->get_session_memory_requirements(device, session->session, &count,
                                             NULL) != VK_SUCCESS ||
      count > VKR_VK_VIEW_SESSION_BINDINGS) {
    return false_v;
  }
  VkVideoSessionMemoryRequirementsKHR
      requirements[VKR_VK_VIEW_SESSION_BINDINGS];
  VkBindVideoSessionMemoryInfoKHR binds[VKR_VK_VIEW_SESSION_BINDINGS];
  for (uint32_t i = 0u; i < count; ++i) {
    requirements[i] = (VkVideoSessionMemoryRequirementsKHR){
        .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR,
    };
  }
  if (count &&
      video->get_session_memory_requirements(device, session->session, &count,
                                             requirements) != VK_SUCCESS) {
    return false_v;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (!vkr_vk_view_allocate(renderer, &requirements[i].memoryRequirements, 0u,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              &session->session_memory[i], NULL)) {
      return false_v;
    }
    session->session_memory_count = i + 1u;
    binds[i] = (VkBindVideoSessionMemoryInfoKHR){
        .sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,
        .memoryBindIndex = requirements[i].memoryBindIndex,
        .memory = session->session_memory[i],
        .memorySize = requirements[i].memoryRequirements.size,
    };
  }
  return !count || video->bind_session_memory(device, session->session, count,
                                              binds) == VK_SUCCESS;
}

/* One SPS and one PPS: High profile without B slices (Constrained High, as
   the macOS encoder writes), 4:2:0 8-bit, frame_num and POC type 2 so
   pictures need no reordering, one reference frame, the coded extent
   cropped to the picture, and BT.709 video-range colour in the VUI, which
   the view export's conversion uses. */
vkr_internal bool8_t vkr_vk_view_create_parameters(VkrVulkanView *view,
                                                   VkrVkViewSession *session) {
  const VkrVulkanRenderer *renderer = view->renderer;
  const VkDevice device = vkr_vk_renderer_device(renderer);
  const VkrVulkanVideoEncodeFunctions *video =
      vkr_vulkan_device_video_encode(renderer->device);
  const uint32_t mb_width = session->coded_width / 16u;
  const uint32_t mb_height = session->coded_height / 16u;
  const bool8_t cropped = session->coded_width != session->width ||
                          session->coded_height != session->height;
  const StdVideoH264SequenceParameterSetVui vui = {
      .flags = {.video_signal_type_present_flag = 1u,
                .color_description_present_flag = 1u,
                .bitstream_restriction_flag = 1u},
      .aspect_ratio_idc = STD_VIDEO_H264_ASPECT_RATIO_IDC_UNSPECIFIED,
      /* Unspecified video format; BT.709 primaries, transfer and matrix. */
      .video_format = 5u,
      .colour_primaries = 1u,
      .transfer_characteristics = 1u,
      .matrix_coefficients = 1u,
      .max_num_reorder_frames = 0u,
      .max_dec_frame_buffering = 1u,
  };
  const StdVideoH264SequenceParameterSet sps = {
      .flags = {.constraint_set4_flag = 1u,
                .constraint_set5_flag = 1u,
                .direct_8x8_inference_flag = 1u,
                .frame_mbs_only_flag = 1u,
                .frame_cropping_flag = cropped ? 1u : 0u,
                .vui_parameters_present_flag = 1u},
      .profile_idc = STD_VIDEO_H264_PROFILE_IDC_HIGH,
      .level_idc =
          vkr_vk_view_level(view, mb_width * mb_height, session->frame_rate),
      .chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420,
      .log2_max_frame_num_minus4 = VKR_VK_VIEW_LOG2_MAX_FRAME_NUM_MINUS4,
      .pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2,
      .max_num_ref_frames = 1u,
      .pic_width_in_mbs_minus1 = mb_width - 1u,
      .pic_height_in_map_units_minus1 = mb_height - 1u,
      /* Crop offsets count 4:2:0 chroma samples, two luma samples each. */
      .frame_crop_right_offset = (session->coded_width - session->width) / 2u,
      .frame_crop_bottom_offset =
          (session->coded_height - session->height) / 2u,
      .pSequenceParameterSetVui = &vui,
  };
  /* CABAC and the 8x8 transform where the encoder writes them. */
  const VkVideoEncodeH264StdFlagsKHR syntax =
      view->h264_capabilities.stdSyntaxFlags;
  const bool8_t transform_8x8 =
      (syntax & VK_VIDEO_ENCODE_H264_STD_TRANSFORM_8X8_MODE_FLAG_SET_BIT_KHR) !=
      0u;
  const bool8_t cabac =
      (syntax &
       VK_VIDEO_ENCODE_H264_STD_ENTROPY_CODING_MODE_FLAG_SET_BIT_KHR) != 0u;
  const StdVideoH264PictureParameterSet pps = {
      .flags = {.transform_8x8_mode_flag = transform_8x8 ? 1u : 0u,
                .entropy_coding_mode_flag = cabac ? 1u : 0u},
      .weighted_bipred_idc = STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_DEFAULT,
  };
  const VkVideoEncodeH264SessionParametersAddInfoKHR add = {
      .sType =
          VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR,
      .stdSPSCount = 1u,
      .pStdSPSs = &sps,
      .stdPPSCount = 1u,
      .pStdPPSs = &pps,
  };
  const VkVideoEncodeH264SessionParametersCreateInfoKHR h264 = {
      .sType =
          VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR,
      .maxStdSPSCount = 1u,
      .maxStdPPSCount = 1u,
      .pParametersAddInfo = &add,
  };
  const VkVideoSessionParametersCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
      .pNext = &h264,
      .videoSession = session->session,
  };
  if (video->create_parameters(device, &info, NULL, &session->parameters) !=
      VK_SUCCESS) {
    session->parameters = VK_NULL_HANDLE;
    return false_v;
  }
  /* The parameter sets as the implementation encodes them, overrides
     included. */
  const VkVideoEncodeH264SessionParametersGetInfoKHR h264_get = {
      .sType =
          VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR,
      .writeStdSPS = VK_TRUE,
      .writeStdPPS = VK_TRUE,
  };
  const VkVideoEncodeSessionParametersGetInfoKHR get = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
      .pNext = &h264_get,
      .videoSessionParameters = session->parameters,
  };
  VkVideoEncodeSessionParametersFeedbackInfoKHR feedback = {
      .sType =
          VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,
  };
  size_t size = 0u;
  if (video->get_encoded_parameters(device, &get, &feedback, &size, NULL) !=
          VK_SUCCESS ||
      size == 0u || size > UINT32_MAX) {
    return false_v;
  }
  session->parameter_sets = vkr_heap_alloc(size);
  if (!session->parameter_sets ||
      video->get_encoded_parameters(device, &get, &feedback, &size,
                                    session->parameter_sets) != VK_SUCCESS) {
    return false_v;
  }
  session->parameter_set_size = (uint32_t)size;
  if (!vkr_vk_view_starts_with_start_code(session->parameter_sets, size)) {
    log_error("Vulkan view streaming: the encoded parameter sets have no "
              "start code");
    return false_v;
  }
  if (feedback.hasOverrides) {
    log_info("Vulkan view streaming: the encoder overrode parameter set "
             "fields");
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_view_create_dpb(VkrVulkanView *view,
                                            VkrVkViewSession *session) {
  const VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &view->profile_list,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VKR_VK_VIEW_PICTURE_FORMAT,
      .extent = {session->coded_width, session->coded_height, 1u},
      .mipLevels = 1u,
      .arrayLayers = VKR_VK_VIEW_DPB_SLOTS,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  return vkr_vk_view_create_image(view->renderer, &info, &session->dpb,
                                  &session->dpb_memory) &&
         vkr_vk_view_create_view(
             view->renderer, session->dpb, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
             VKR_VK_VIEW_PICTURE_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT,
             VKR_VK_VIEW_DPB_SLOTS, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
             &session->dpb_view);
}

/* The feedback queries, the encode command pool and buffers, and the
   timeline the encodes signal. */
vkr_internal bool8_t vkr_vk_view_create_submission(VkrVulkanView *view,
                                                   VkrVkViewSession *session) {
  const VkrVulkanRenderer *renderer = view->renderer;
  const VkDevice device = vkr_vk_renderer_device(renderer);
  const VkrVulkanVideoEncodeFunctions *video =
      vkr_vulkan_device_video_encode(renderer->device);
  const VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedback_info = {
      .sType =
          VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR,
      .pNext = &view->profile,
      .encodeFeedbackFlags =
          VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
          VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR,
  };
  const VkQueryPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .pNext = &feedback_info,
      .queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR,
      .queryCount = VKR_VK_VIEW_SLOTS,
  };
  if (vkCreateQueryPool(device, &pool_info, NULL, &session->feedback) !=
      VK_SUCCESS) {
    session->feedback = VK_NULL_HANDLE;
    return false_v;
  }
  const VkCommandPoolCreateInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = video->queue_family_index,
  };
  if (vkCreateCommandPool(device, &command_info, NULL,
                          &session->command_pool) != VK_SUCCESS) {
    session->command_pool = VK_NULL_HANDLE;
    return false_v;
  }
  VkCommandBuffer commands[VKR_VK_VIEW_SLOTS];
  const VkCommandBufferAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = session->command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = VKR_VK_VIEW_SLOTS,
  };
  if (vkAllocateCommandBuffers(device, &allocate_info, commands) !=
      VK_SUCCESS) {
    return false_v;
  }
  for (uint32_t i = 0u; i < VKR_VK_VIEW_SLOTS; ++i) {
    session->slots[i].command = commands[i];
  }
  const VkSemaphoreTypeCreateInfo timeline_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
  };
  const VkSemaphoreCreateInfo semaphore_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &timeline_info,
  };
  if (vkCreateSemaphore(device, &semaphore_info, NULL, &session->encoded) !=
      VK_SUCCESS) {
    session->encoded = VK_NULL_HANDLE;
    return false_v;
  }
  return true_v;
}

/* A slot's NV12 picture, which the export's compute writes through two
   plane views and the encode reads, and its host-visible bitstream. */
vkr_internal bool8_t vkr_vk_view_create_slot(VkrVulkanView *view,
                                             VkrVkViewSession *session,
                                             VkrVkViewSlot *slot) {
  VkrVulkanRenderer *renderer = view->renderer;
  const VkDevice device = vkr_vk_renderer_device(renderer);
  const VkrVulkanVideoEncodeFunctions *video =
      vkr_vulkan_device_video_encode(renderer->device);
  const uint32_t families[] = {vkr_vulkan_device_queue_family(renderer->device),
                               video->queue_family_index};
  const VkImageCreateInfo picture_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &view->profile_list,
      .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
               VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VKR_VK_VIEW_PICTURE_FORMAT,
      .extent = {session->coded_width, session->coded_height, 1u},
      .mipLevels = 1u,
      .arrayLayers = 1u,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage =
          VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_STORAGE_BIT,
      .sharingMode = VK_SHARING_MODE_CONCURRENT,
      .queueFamilyIndexCount = ArrayCount(families),
      .pQueueFamilyIndices = families,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  if (!vkr_vk_view_create_image(renderer, &picture_info, &slot->picture,
                                &slot->picture_memory) ||
      !vkr_vk_view_create_view(
          renderer, slot->picture, VK_IMAGE_VIEW_TYPE_2D,
          VKR_VK_VIEW_PICTURE_FORMAT, VK_IMAGE_ASPECT_COLOR_BIT, 1u,
          VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, &slot->picture_view) ||
      !vkr_vk_view_create_view(renderer, slot->picture, VK_IMAGE_VIEW_TYPE_2D,
                               VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_PLANE_0_BIT,
                               1u, VK_IMAGE_USAGE_STORAGE_BIT,
                               &slot->luma_view) ||
      !vkr_vk_view_create_view(
          renderer, slot->picture, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8_UNORM,
          VK_IMAGE_ASPECT_PLANE_1_BIT, 1u, VK_IMAGE_USAGE_STORAGE_BIT,
          &slot->chroma_view) ||
      !vkr_vk_publish_storage_view(renderer, slot->luma_view,
                                   &slot->luma_slot) ||
      !vkr_vk_publish_storage_view(renderer, slot->chroma_view,
                                   &slot->chroma_slot)) {
    return false_v;
  }
  /* A raw 4:2:0 picture bounds a frame the rate control keeps near one
     frame interval of bits. */
  slot->bitstream_size = vkr_align_up_u64(
      (uint64_t)session->coded_width * session->coded_height * 3u / 2u +
          VKR_VK_VIEW_BITSTREAM_SLACK,
      Max(view->capabilities.minBitstreamBufferSizeAlignment, 1u));
  const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = &view->profile_list,
      .size = slot->bitstream_size,
      .usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (vkCreateBuffer(device, &buffer_info, NULL, &slot->bitstream) !=
      VK_SUCCESS) {
    slot->bitstream = VK_NULL_HANDLE;
    return false_v;
  }
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(device, slot->bitstream, &requirements);
  VkMemoryPropertyFlags flags = 0u;
  void *mapped = NULL;
  if (!vkr_vk_view_allocate(renderer, &requirements,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                            &slot->bitstream_memory, &flags) ||
      vkBindBufferMemory(device, slot->bitstream, slot->bitstream_memory, 0u) !=
          VK_SUCCESS ||
      vkMapMemory(device, slot->bitstream_memory, 0u, VK_WHOLE_SIZE, 0u,
                  &mapped) != VK_SUCCESS) {
    return false_v;
  }
  slot->bitstream_mapped = mapped;
  slot->bitstream_coherent =
      (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;
  return true_v;
}

vkr_internal bool8_t vkr_vk_view_create_session(VkrVulkanView *view,
                                                uint32_t width, uint32_t height,
                                                uint32_t frame_rate) {
  VkrVulkanRenderer *renderer = view->renderer;
  const VkExtent2D granularity = view->capabilities.pictureAccessGranularity;
  const uint32_t coded_width =
      Max(vkr_align_up_u32(width, Max(granularity.width, 16u)),
          view->capabilities.minCodedExtent.width);
  const uint32_t coded_height =
      Max(vkr_align_up_u32(height, Max(granularity.height, 16u)),
          view->capabilities.minCodedExtent.height);
  if (coded_width > view->capabilities.maxCodedExtent.width ||
      coded_height > view->capabilities.maxCodedExtent.height) {
    log_warn("Vulkan view streaming: %ux%u is larger than the encoder's "
             "%ux%u",
             width, height, view->capabilities.maxCodedExtent.width,
             view->capabilities.maxCodedExtent.height);
    return false_v;
  }
  VkrVkViewSession *session = vkr_heap_alloc_zeroed(1u, sizeof(*session));
  if (!session) {
    return false_v;
  }
  session->view = view;
  session->width = width;
  session->height = height;
  session->frame_rate = frame_rate;
  session->coded_width = coded_width;
  session->coded_height = coded_height;
  bool8_t created = vkr_vk_view_create_session_object(view, session) &&
                    vkr_vk_view_create_parameters(view, session) &&
                    vkr_vk_view_create_dpb(view, session) &&
                    vkr_vk_view_create_submission(view, session);
  for (uint32_t i = 0u; created && i < VKR_VK_VIEW_SLOTS; ++i) {
    created = vkr_vk_view_create_slot(view, session, &session->slots[i]);
  }
  if (created) {
    session->packet_capacity =
        session->parameter_set_size + 4u + session->slots[0].bitstream_size;
    session->packet = vkr_heap_alloc(session->packet_capacity);
    created = session->packet &&
              vkr_mutex_create(renderer->allocator, &session->mutex) &&
              vkr_cond_create(renderer->allocator, &session->cond);
  }
  if (created) {
    session->thread_running =
        vkr_thread_create(renderer->allocator, &session->thread,
                          vkr_vk_view_encoder_main, session);
    created = session->thread_running;
  }
  if (!created) {
    log_warn("Vulkan view streaming could not make a %ux%u H.264 encoder",
             width, height);
    vkr_vk_view_session_destroy(view, session);
    return false_v;
  }
  view->session = session;
  log_info("Vulkan view streaming: H.264 encoder %ux%u (coded %ux%u) at "
           "%u Hz, %u bytes of parameter sets",
           width, height, coded_width, coded_height, frame_rate,
           session->parameter_set_size);
  return true_v;
}

/* The session for the export's extent and frame rate. A different one is
   replaced once every slot of it has come back; until then, and when it
   cannot be made, the export goes unwritten. */
vkr_internal bool8_t vkr_vk_view_session_ready(VkrVulkanView *view,
                                               const VkrViewExport *export) {
  VkrVkViewSession *session = view->session;
  if (session && session->width == export->width &&
      session->height == export->height &&
      session->frame_rate == export->frame_rate) {
    return true_v;
  }
  if (view->failed[0] == export->width && view->failed[1] == export->height &&
      view->failed[2] == export->frame_rate) {
    return false_v;
  }
  if (session) {
    if (!vkr_vk_view_slots_free(session)) {
      return false_v;
    }
    vkr_vk_view_session_destroy(view, session);
    view->session = NULL;
  }
  if (!vkr_vk_view_create_session(view, export->width, export->height,
                                  export->frame_rate)) {
    view->failed[0] = export->width;
    view->failed[1] = export->height;
    view->failed[2] = export->frame_rate;
    return false_v;
  }
  return true_v;
}

// =============================================================================
// Encoding
// =============================================================================

/* One rate-control state and the structures it chains; filled in place and
   not moved after, because its members point at each other. */
typedef struct VkrVkViewRateControl {
  VkVideoEncodeH264RateControlLayerInfoKHR h264_layer;
  VkVideoEncodeRateControlLayerInfoKHR layer;
  VkVideoEncodeH264RateControlInfoKHR h264;
  VkVideoEncodeRateControlInfoKHR info;
} VkrVkViewRateControl;

vkr_internal void vkr_vk_view_rate_control(const VkrVulkanView *view,
                                           const VkrVkViewSession *session,
                                           uint32_t bitrate,
                                           VkrVkViewRateControl *out) {
  /* One frame interval of buffering, as the network proposal sets it: a
     frame's bits leave within about a frame. */
  const uint32_t buffer_ms = Max(1000u / Max(session->frame_rate, 1u), 2u);
  /* The device's whole QP range. Without one, the AMD driver (Adrenalin
     26.10.1) encodes every picture at one QP whatever the bitrate: about
     50 KB a frame of Bistro at 1280x704 for both 10 and 20 Mbit/s. */
  const VkVideoEncodeH264CapabilitiesKHR *caps = &view->h264_capabilities;
  out->h264_layer = (VkVideoEncodeH264RateControlLayerInfoKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR,
      .useMinQp = VK_TRUE,
      .minQp = {.qpI = caps->minQp, .qpP = caps->minQp, .qpB = caps->minQp},
      .useMaxQp = VK_TRUE,
      .maxQp = {.qpI = caps->maxQp, .qpP = caps->maxQp, .qpB = caps->maxQp},
  };
  out->layer = (VkVideoEncodeRateControlLayerInfoKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR,
      .pNext = &out->h264_layer,
      .averageBitrate = bitrate,
      .maxBitrate = bitrate,
      .frameRateNumerator = session->frame_rate,
      .frameRateDenominator = 1u,
  };
  /* An open GOP of P pictures that each reference the one before; an IDR
     picture only on request. The GOP is not regular, so no reference
     pattern flag applies. */
  out->h264 = (VkVideoEncodeH264RateControlInfoKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR,
      .gopFrameCount = UINT32_MAX,
      .idrPeriod = UINT32_MAX,
      .consecutiveBFrameCount = 0u,
      .temporalLayerCount = 1u,
  };
  out->info = (VkVideoEncodeRateControlInfoKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,
      .pNext = &out->h264,
      .rateControlMode = view->rate_control_mode,
      .layerCount = 1u,
      .pLayers = &out->layer,
      .virtualBufferSizeInMs = buffer_ms,
      .initialVirtualBufferSizeInMs = buffer_ms / 2u,
  };
}

/* Records the slot's encode: acquire the picture, keep the DPB's writes
   ordered, reset rate control on the session's first encode or set a new
   bitrate, encode with the feedback query, and release the bitstream to
   the host. */
vkr_internal bool8_t vkr_vk_view_record_encode(
    VkrVulkanView *view, VkrVkViewSession *session, uint32_t slot_index,
    const VkrVkViewPicture *picture) {
  const VkrVulkanVideoEncodeFunctions *video =
      vkr_vulkan_device_video_encode(view->renderer->device);
  VkrVkViewSlot *slot = &session->slots[slot_index];
  const VkCommandBuffer command = slot->command;
  if (vkResetCommandBuffer(command, 0u) != VK_SUCCESS) {
    return false_v;
  }
  const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (vkr_vk_begin_command_buffer(command, &begin_info) != VK_SUCCESS) {
    return false_v;
  }
  /* The picture's compute writes reach this queue through the timeline
     wait at the encode stage; the barrier chains to that wait. */
  const VkImageMemoryBarrier2 barriers[] = {
      {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
          .dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
          .dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = slot->picture,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1u,
                               .layerCount = 1u},
      },
      {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = session->dpb_initialized
                              ? VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR
                              : VK_PIPELINE_STAGE_2_NONE,
          .srcAccessMask = session->dpb_initialized
                               ? VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR
                               : VK_ACCESS_2_NONE,
          .dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
          .dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR |
                           VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
          .oldLayout = session->dpb_initialized
                           ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR
                           : VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = session->dpb,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1u,
                               .layerCount = VKR_VK_VIEW_DPB_SLOTS},
      },
  };
  const VkDependencyInfo acquire = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = ArrayCount(barriers),
      .pImageMemoryBarriers = barriers,
  };
  vkr_vk_cmd_pipeline_barrier2(command, &acquire);
  vkCmdResetQueryPool(command, session->feedback, slot_index, 1u);

  const bool8_t rate_control =
      view->rate_control_mode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
  VkrVkViewRateControl current;
  vkr_vk_view_rate_control(view, session, session->rate_bitrate, &current);
  VkrVkViewRateControl next;
  vkr_vk_view_rate_control(view, session, picture->bitrate, &next);

  VkVideoPictureResourceInfoKHR dpb_pictures[VKR_VK_VIEW_DPB_SLOTS];
  VkVideoReferenceSlotInfoKHR bound[VKR_VK_VIEW_DPB_SLOTS];
  for (uint32_t i = 0u; i < VKR_VK_VIEW_DPB_SLOTS; ++i) {
    dpb_pictures[i] = (VkVideoPictureResourceInfoKHR){
        .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
        .codedExtent = {session->coded_width, session->coded_height},
        .baseArrayLayer = i,
        .imageViewBinding = session->dpb_view,
    };
    /* An inactive slot's picture is bound for the setup only. */
    bound[i] = (VkVideoReferenceSlotInfoKHR){
        .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
        .slotIndex = session->dpb_active[i] ? (int32_t)i : -1,
        .pPictureResource = &dpb_pictures[i],
    };
  }
  const VkVideoBeginCodingInfoKHR begin = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
      .pNext = rate_control && session->coding_started ? &current.info : NULL,
      .videoSession = session->session,
      .videoSessionParameters = session->parameters,
      .referenceSlotCount = VKR_VK_VIEW_DPB_SLOTS,
      .pReferenceSlots = bound,
  };
  video->cmd_begin_coding(command, &begin);
  VkVideoCodingControlFlagsKHR control_flags = 0u;
  if (!session->coding_started) {
    control_flags |= VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
  }
  if (rate_control &&
      (!session->coding_started || picture->bitrate != session->rate_bitrate)) {
    control_flags |= VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
  }
  if (control_flags) {
    const VkVideoCodingControlInfoKHR control = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
        .pNext = (control_flags &
                  VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR)
                     ? &next.info
                     : NULL,
        .flags = control_flags,
    };
    video->cmd_control_coding(command, &control);
  }

  StdVideoEncodeH264ReferenceListsInfo lists = {0};
  MemSet(lists.RefPicList0, STD_VIDEO_H264_NO_REFERENCE_PICTURE,
         sizeof(lists.RefPicList0));
  MemSet(lists.RefPicList1, STD_VIDEO_H264_NO_REFERENCE_PICTURE,
         sizeof(lists.RefPicList1));
  const uint32_t reference_dpb = session->reference_dpb;
  if (!picture->key_frame) {
    lists.RefPicList0[0] = (uint8_t)reference_dpb;
  }
  const StdVideoEncodeH264PictureInfo std_picture = {
      .flags = {.IdrPicFlag = picture->key_frame ? 1u : 0u, .is_reference = 1u},
      .idr_pic_id = picture->idr_pic_id,
      .primary_pic_type = picture->reference.primary_pic_type,
      .frame_num = picture->reference.FrameNum,
      .PicOrderCnt = picture->reference.PicOrderCnt,
      .pRefLists = &lists,
  };
  const StdVideoEncodeH264SliceHeader slice_header = {
      .slice_type = picture->key_frame ? STD_VIDEO_H264_SLICE_TYPE_I
                                       : STD_VIDEO_H264_SLICE_TYPE_P,
      .cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0,
      .disable_deblocking_filter_idc =
          STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED,
  };
  const VkVideoEncodeH264NaluSliceInfoKHR slice = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR,
      .pStdSliceHeader = &slice_header,
  };
  const VkVideoEncodeH264PictureInfoKHR h264_picture = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR,
      .naluSliceEntryCount = 1u,
      .pNaluSliceEntries = &slice,
      .pStdPictureInfo = &std_picture,
  };
  const VkVideoEncodeH264DpbSlotInfoKHR setup_info = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR,
      .pStdReferenceInfo = &picture->reference,
  };
  const VkVideoReferenceSlotInfoKHR setup = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
      .pNext = &setup_info,
      .slotIndex = (int32_t)picture->setup_dpb,
      .pPictureResource = &dpb_pictures[picture->setup_dpb],
  };
  const VkVideoEncodeH264DpbSlotInfoKHR reference_info = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR,
      .pStdReferenceInfo = &session->dpb_reference[reference_dpb],
  };
  const VkVideoReferenceSlotInfoKHR reference = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
      .pNext = &reference_info,
      .slotIndex = (int32_t)reference_dpb,
      .pPictureResource = &dpb_pictures[reference_dpb],
  };
  const VkVideoEncodeInfoKHR encode = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR,
      .pNext = &h264_picture,
      .dstBuffer = slot->bitstream,
      .dstBufferOffset = 0u,
      .dstBufferRange = slot->bitstream_size,
      .srcPictureResource =
          {
              .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
              .codedExtent = {session->coded_width, session->coded_height},
              .imageViewBinding = slot->picture_view,
          },
      .pSetupReferenceSlot = &setup,
      .referenceSlotCount = picture->key_frame ? 0u : 1u,
      .pReferenceSlots = picture->key_frame ? NULL : &reference,
  };
  vkCmdBeginQuery(command, session->feedback, slot_index, 0u);
  video->cmd_encode(command, &encode);
  vkCmdEndQuery(command, session->feedback, slot_index);
  const VkVideoEndCodingInfoKHR end = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
  };
  video->cmd_end_coding(command, &end);
  const VkBufferMemoryBarrier2 release = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
      .srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = slot->bitstream,
      .size = VK_WHOLE_SIZE,
  };
  const VkDependencyInfo release_dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1u,
      .pBufferMemoryBarriers = &release,
  };
  vkr_vk_cmd_pipeline_barrier2(command, &release_dependency);
  return vkEndCommandBuffer(command) == VK_SUCCESS;
}

/* The next picture: an IDR picture on request, at a session's start or
   after a lost reference, else a P picture referencing the last one. Its
   reconstruction takes the DPB slot the reference does not hold. */
vkr_internal VkrVkViewPicture vkr_vk_view_next_picture(
    const VkrVkViewSession *session, const VkrViewExport *export) {
  const bool8_t key_frame = export->key_frame || !session->reference_valid;
  const uint64_t index = key_frame ? 0u : session->picture_index + 1u;
  const uint64_t max_frame_num =
      1ull << (VKR_VK_VIEW_LOG2_MAX_FRAME_NUM_MINUS4 + 4u);
  const StdVideoH264PictureType type = key_frame
                                           ? STD_VIDEO_H264_PICTURE_TYPE_IDR
                                           : STD_VIDEO_H264_PICTURE_TYPE_P;
  return (VkrVkViewPicture){
      .key_frame = key_frame,
      .setup_dpb = session->reference_valid
                       ? (session->reference_dpb + 1u) % VKR_VK_VIEW_DPB_SLOTS
                       : 0u,
      .index = index,
      .idr_pic_id = key_frame ? (uint16_t)(session->idr_pic_id + 1u)
                              : session->idr_pic_id,
      .bitrate = (uint32_t)Min((uint64_t)export->bitrate_bps,
                               session->view->encode_capabilities.maxBitrate),
      /* POC type 2: twice the pictures since the IDR picture. */
      .reference = {.primary_pic_type = type,
                    .FrameNum = (uint32_t)(index % max_frame_num),
                    .PicOrderCnt = (int32_t)((index * 2u) & INT32_MAX)},
  };
}

// =============================================================================
// Renderer hooks
// =============================================================================

void vkr_vk_view_create(VkrVulkanRenderer *renderer) {
  renderer->view = NULL;
  const VkrVulkanVideoEncodeFunctions *video =
      vkr_vulkan_device_video_encode(renderer->device);
  if (!video) {
    return;
  }
  VkrVulkanView *view = vkr_heap_alloc_zeroed(1u, sizeof(*view));
  if (!view) {
    return;
  }
  view->renderer = renderer;
  view->usage = (VkVideoEncodeUsageInfoKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR,
      .videoUsageHints = VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR,
      .videoContentHints = VK_VIDEO_ENCODE_CONTENT_RENDERED_BIT_KHR,
      .tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR,
  };
  view->h264_profile = (VkVideoEncodeH264ProfileInfoKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR,
      .pNext = &view->usage,
      .stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH,
  };
  view->profile = (VkVideoProfileInfoKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
      .pNext = &view->h264_profile,
      .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
      .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
      .lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
      .chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
  };
  view->profile_list = (VkVideoProfileListInfoKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
      .profileCount = 1u,
      .pProfiles = &view->profile,
  };
  view->h264_capabilities = (VkVideoEncodeH264CapabilitiesKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR,
  };
  view->encode_capabilities = (VkVideoEncodeCapabilitiesKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR,
      .pNext = &view->h264_capabilities,
  };
  view->capabilities = (VkVideoCapabilitiesKHR){
      .sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
      .pNext = &view->encode_capabilities,
  };
  renderer->view = view;
  const VkResult result =
      video->get_capabilities(vkr_vulkan_device_physical(renderer->device),
                              &view->profile, &view->capabilities);
  const VkVideoEncodeRateControlModeFlagsKHR modes =
      view->encode_capabilities.rateControlModes;
  view->rate_control_mode =
      (modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)
          ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR
      : (modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR)
          ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR
          : VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
  const VkVideoEncodeFeedbackFlagsKHR feedback =
      VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
      VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
  const char *missing =
      result != VK_SUCCESS ? "no High-profile H.264 encode capabilities"
      : view->capabilities.maxDpbSlots < VKR_VK_VIEW_DPB_SLOTS ||
              view->capabilities.maxActiveReferencePictures < 1u ||
              view->h264_capabilities.maxPPictureL0ReferenceCount < 1u
          ? "too few reference pictures"
      : (view->encode_capabilities.supportedEncodeFeedbackFlags & feedback) !=
              feedback
          ? "no bitstream feedback"
      : !vkr_vk_view_format_supported(view, video,
                                      VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
                                          VK_IMAGE_USAGE_STORAGE_BIT,
                                      VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                          VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
          ? "no NV12 picture the export can write"
      : !vkr_vk_view_format_supported(
            view, video, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, 0u)
          ? "no NV12 reference picture"
      : !renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_VIEW_EXPORT]
          ? "no view export pipeline"
          : NULL;
  view->supported = missing == NULL;
  if (!view->supported) {
    log_warn("Vulkan view streaming unavailable: %s", missing);
    return;
  }
  log_info("Vulkan view streaming: H.264 encode on queue family %u, coded "
           "extent %ux%u to %ux%u, level up to %d, rate control 0x%x",
           video->queue_family_index, view->capabilities.minCodedExtent.width,
           view->capabilities.minCodedExtent.height,
           view->capabilities.maxCodedExtent.width,
           view->capabilities.maxCodedExtent.height,
           (int)view->h264_capabilities.maxLevelIdc,
           (uint32_t)view->rate_control_mode);
}

bool8_t
vkr_vulkan_renderer_view_encoding_supported(const VkrVulkanRenderer *renderer) {
  return renderer && renderer->view && renderer->view->supported;
}

/* First in a frame's submission: takes the frame's export, which
   vkr_vk_view_submit reports. An export a failed frame left unreported is
   reported unwritten first. */
void vkr_vk_view_begin_frame(VkrVulkanRenderer *renderer,
                             const VkrPreparedFrame *packet) {
  VkrVulkanView *view = renderer->view;
  const VkrEditorPassPayload *editor = packet->input.editor;
  const VkrViewExport *export = editor ? editor->view_export : NULL;
  if (!view) {
    /* Render validation refuses an export without encoding. */
    if (export) {
      vkr_vk_view_report(export, false_v);
    }
    return;
  }
  vkr_vk_view_reject(view);
  if (export) {
    if (!view->export_seen) {
      view->export_seen = true_v;
      log_info("Vulkan view streaming: the first export arrived, %ux%u at "
               "%u Hz and %u bit/s",
               export->width, export->height, export->frame_rate,
               export->bitrate_bps);
    }
    view->pending = *export;
    view->has_pending = true_v;
  }
}

/* After the frame's graph is prepared and before its uploads flush: the
   session, a free slot and the root of the dispatch that resamples the
   finished Scene image into the slot's picture. Without a rendered Scene
   image, a session or a free slot the export goes unwritten. */
void vkr_vk_view_prepare_export(VkrVulkanRenderer *renderer,
                                VkrVulkanFrameSlot *frame_slot) {
  VkrVulkanView *view = renderer->view;
  if (!view || !view->has_pending) {
    return;
  }
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const VkrRgImageHandle handle =
      vkr_rg_find_image(renderer->graph, string8_lit("editor_scene_image"));
  VkrVulkanGraphImageInstance *scene =
      packet->scene_rendering
          ? vkr_vk_graph_image(renderer, handle,
                               renderer->prepared_frame.image_index)
          : NULL;
  const VkrTextureLayout final_layout =
      scene ? vkr_rg_image_final_subresource_layout(renderer->graph, handle, 0u,
                                                    0u)
            : VKR_TEXTURE_LAYOUT_UNDEFINED;
  if (!view->supported || !scene || !scene->has_sampled_slot ||
      final_layout == VKR_TEXTURE_LAYOUT_UNDEFINED) {
    vkr_vk_view_note(view, VKR_VK_VIEW_REASON_NO_SCENE);
    vkr_vk_view_reject(view);
    return;
  }
  if (!vkr_vk_view_session_ready(view, &view->pending)) {
    vkr_vk_view_note(view, VKR_VK_VIEW_REASON_NO_SESSION);
    vkr_vk_view_reject(view);
    return;
  }
  VkrVkViewSession *session = view->session;
  const uint32_t slot_index = session->next_slot;
  VkrVkViewSlot *slot = &session->slots[slot_index];
  /* All four still encoding: this frame's picture is dropped. */
  if (vkr_atomic_uint32_load(&slot->busy, VKR_MEMORY_ORDER_ACQUIRE)) {
    vkr_vk_view_note(view, VKR_VK_VIEW_REASON_SLOTS_BUSY);
    vkr_vk_view_reject(view);
    return;
  }
  uint64_t root_address = 0u;
  VkrVulkanViewExportRoot *root = vkr_vk_frame_upload_allocate(
      frame_slot, sizeof(*root), _Alignof(VkrVulkanViewExportRoot),
      &root_address, NULL);
  if (!root) {
    vkr_vk_view_note(view, VKR_VK_VIEW_REASON_NO_ROOT);
    vkr_vk_view_reject(view);
    return;
  }
  const VkFormat format = scene->image.format;
  *root = (VkrVulkanViewExportRoot){
      .source_texture = scene->sampled_slot.index,
      .source_sampler = renderer->dfg_sampler_slot.index,
      .luma_image = slot->luma_slot.index,
      .chroma_image = slot->chroma_slot.index,
      .extent = {session->width, session->height},
      .coded_extent = {session->coded_width, session->coded_height},
      /* 8-bit UNORM Scene images hold display-encoded values; sRGB and
         float images sample linear ones. */
      .source_encoded = format == VK_FORMAT_R8G8B8A8_UNORM ||
                        format == VK_FORMAT_B8G8R8A8_UNORM,
  };
  vkr_atomic_uint32_store(&slot->busy, 1u, VKR_MEMORY_ORDER_RELAXED);
  view->prepared = true_v;
  view->slot_index = slot_index;
  view->root_address = root_address;
  view->groups[0] = (session->coded_width / 2u + VKR_VK_VIEW_GROUP_SIZE - 1u) /
                    VKR_VK_VIEW_GROUP_SIZE;
  view->groups[1] = (session->coded_height / 2u + VKR_VK_VIEW_GROUP_SIZE - 1u) /
                    VKR_VK_VIEW_GROUP_SIZE;
  view->scene_image = scene->image.handle;
  view->scene_layout = vkr_vk_texture_layout(final_layout);
  session->last_export_time = vkr_platform_get_absolute_time();
}

/* After the graph, which leaves the Scene image in its final layout: the
   export's dispatch, with the Scene image sampled in SHADER_READ_ONLY and
   returned to its final layout after. */
void vkr_vk_view_record_export(VkrVulkanRenderer *renderer,
                               VkCommandBuffer command) {
  VkrVulkanView *view = renderer->view;
  if (!view || !view->prepared) {
    return;
  }
  VkrVkViewSlot *slot = &view->session->slots[view->slot_index];
  const VkImageLayout sampled = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  const VkImageMemoryBarrier2 acquire[] = {
      {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
          .oldLayout = view->scene_layout,
          .newLayout = sampled,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = view->scene_image,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1u,
                               .layerCount = 1u},
      },
      /* A picture this queue wrote earlier (a frame that failed after its
         submission) is ordered before this write; the encode queue's last
         read of it finished before the slot came back. */
      {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = slot->picture,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1u,
                               .layerCount = 1u},
      },
  };
  const VkDependencyInfo acquire_dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = ArrayCount(acquire),
      .pImageMemoryBarriers = acquire,
  };
  vkr_vk_cmd_pipeline_barrier2(command, &acquire_dependency);
  const VkrVulkanPushConstants push = {.root = view->root_address};
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(push), &push);
  vkr_vk_cmd_bind_pipeline(
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      renderer->deferred_pipelines[VKR_VULKAN_DEFERRED_PIPELINE_VIEW_EXPORT]);
  vkr_vk_cmd_dispatch(command, view->groups[0], view->groups[1], 1u);
  if (view->scene_layout != sampled) {
    VkImageMemoryBarrier2 restore = acquire[0];
    restore.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    restore.srcAccessMask = VK_ACCESS_2_NONE;
    restore.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    restore.dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    restore.oldLayout = sampled;
    restore.newLayout = view->scene_layout;
    const VkDependencyInfo restore_dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1u,
        .pImageMemoryBarriers = &restore,
    };
    vkr_vk_cmd_pipeline_barrier2(command, &restore_dependency);
  }
  view->recorded = true_v;
}

/* After the frame's submission was accepted with `frame_value`: submits the
   slot's encode behind it and hands the slot to the encoder thread. */
void vkr_vk_view_submit(VkrVulkanRenderer *renderer, uint64_t frame_value) {
  VkrVulkanView *view = renderer->view;
  if (!view || !view->has_pending) {
    return;
  }
  if (!view->recorded) {
    vkr_vk_view_note(view, VKR_VK_VIEW_REASON_NOT_RECORDED);
    vkr_vk_view_reject(view);
    return;
  }
  VkrVkViewSession *session = view->session;
  const uint32_t slot_index = view->slot_index;
  VkrVkViewSlot *slot = &session->slots[slot_index];
  const VkrVkViewPicture picture =
      vkr_vk_view_next_picture(session, &view->pending);
  const VkrVulkanVideoEncodeFunctions *video =
      vkr_vulkan_device_video_encode(renderer->device);
  bool8_t submitted =
      vkr_vk_view_record_encode(view, session, slot_index, &picture);
  if (submitted) {
    const VkSemaphoreSubmitInfo wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = renderer->timeline,
        .value = frame_value,
        .stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
    };
    const VkSemaphoreSubmitInfo signal = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = session->encoded,
        .value = session->encode_value + 1u,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    const VkCommandBufferSubmitInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = slot->command,
    };
    const VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1u,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1u,
        .pCommandBufferInfos = &command_info,
        .signalSemaphoreInfoCount = 1u,
        .pSignalSemaphoreInfos = &signal,
    };
    const VkResult result =
        vkr_vk_queue_submit2(video->queue, 1u, &submit, VK_NULL_HANDLE);
    submitted = result == VK_SUCCESS;
    if (!submitted) {
      log_error("Vulkan view streaming: encode submission failed (result=%d)",
                (int)result);
    }
  }
  if (!submitted) {
    /* The session's reference may not hold what it did: the next picture
       starts over with an IDR picture. */
    session->reference_valid = false_v;
    vkr_vk_view_reject(view);
    return;
  }
  session->encode_value += 1u;
  session->coding_started = true_v;
  session->dpb_initialized = true_v;
  session->rate_bitrate = picture.bitrate;
  session->dpb_active[picture.setup_dpb] = true_v;
  session->dpb_reference[picture.setup_dpb] = picture.reference;
  session->reference_dpb = picture.setup_dpb;
  session->reference_valid = true_v;
  session->picture_index = picture.index;
  session->idr_pic_id = picture.idr_pic_id;
  session->next_slot = (slot_index + 1u) % VKR_VK_VIEW_SLOTS;
  slot->export = view->pending;
  slot->frame_value = frame_value;
  slot->encode_value = session->encode_value;
  slot->key_frame = picture.key_frame;
  view->has_pending = false_v;
  view->prepared = false_v;
  view->recorded = false_v;
  view->pending = (VkrViewExport){0};
  vkr_mutex_lock(session->mutex);
  session->queue[(session->queue_first + session->queue_count) %
                 VKR_VK_VIEW_SLOTS] = slot_index;
  session->queue_count += 1u;
  vkr_cond_signal(session->cond);
  vkr_mutex_unlock(session->mutex);
}

/* A frame that ends before its submission reports its export unwritten. */
void vkr_vk_view_cancel(VkrVulkanRenderer *renderer) {
  if (renderer->view) {
    vkr_vk_view_reject(renderer->view);
  }
}

/* At frame preparation: a session no export used for
   VKR_VK_VIEW_IDLE_SECONDS gives its memory back once its slots came
   back. */
void vkr_vk_view_collect(VkrVulkanRenderer *renderer) {
  VkrVulkanView *view = renderer->view;
  VkrVkViewSession *session = view ? view->session : NULL;
  if (!session || view->has_pending ||
      vkr_platform_get_absolute_time() - session->last_export_time <
          VKR_VK_VIEW_IDLE_SECONDS ||
      !vkr_vk_view_slots_free(session)) {
    return;
  }
  vkr_vk_view_session_destroy(view, session);
  view->session = NULL;
  log_info("Vulkan view streaming released its encoder after %.0f s without "
           "an export",
           VKR_VK_VIEW_IDLE_SECONDS);
}

/* After the device went idle: every export the renderer holds is reported
   before the view goes. */
void vkr_vk_view_destroy(VkrVulkanRenderer *renderer) {
  VkrVulkanView *view = renderer->view;
  if (!view) {
    return;
  }
  vkr_vk_view_reject(view);
  vkr_vk_view_session_destroy(view, view->session);
  vkr_heap_free(view);
  renderer->view = NULL;
}
