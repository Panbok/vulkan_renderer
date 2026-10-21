/* Hardware video coding is not implemented on Windows yet (vkr_video.h):
   every call reports it unavailable. Media Foundation or Vulkan Video
   encode is the later slice of docs/adr/131-view-streaming.md. */
#include "vkr_video.h"

#if !defined(PLATFORM_APPLE)

bool8_t vkr_video_supported(VkrVideoCodec codec) {
  (void)codec;
  return false_v;
}

VkrVideoSurface *vkr_video_surface_create(uint32_t width, uint32_t height) {
  (void)width;
  (void)height;
  return NULL;
}

void vkr_video_surface_retain(VkrVideoSurface *surface) { (void)surface; }

void vkr_video_surface_release(VkrVideoSurface *surface) { (void)surface; }

uint32_t vkr_video_surface_width(const VkrVideoSurface *surface) {
  (void)surface;
  return 0u;
}

uint32_t vkr_video_surface_height(const VkrVideoSurface *surface) {
  (void)surface;
  return 0u;
}

void *vkr_video_surface_platform(const VkrVideoSurface *surface) {
  (void)surface;
  return NULL;
}

bool8_t vkr_video_surface_write(VkrVideoSurface *surface, const uint8_t *pixels,
                                uint64_t row_bytes) {
  (void)surface;
  (void)pixels;
  (void)row_bytes;
  return false_v;
}

bool8_t vkr_video_surface_read(const VkrVideoSurface *surface, uint8_t *pixels,
                               uint64_t row_bytes) {
  (void)surface;
  (void)pixels;
  (void)row_bytes;
  return false_v;
}

VkrVideoEncoder *vkr_video_encoder_create(const VkrVideoEncoderConfig *config,
                                          VkrVideoPacketFn on_packet,
                                          void *context) {
  (void)config;
  (void)on_packet;
  (void)context;
  return NULL;
}

bool8_t vkr_video_encoder_encode(VkrVideoEncoder *encoder,
                                 VkrVideoSurface *surface,
                                 uint64_t timestamp_us, bool8_t key_frame,
                                 uint64_t tag) {
  (void)encoder;
  (void)surface;
  (void)timestamp_us;
  (void)key_frame;
  (void)tag;
  return false_v;
}

void vkr_video_encoder_set_bitrate(VkrVideoEncoder *encoder,
                                   uint32_t bitrate_bps) {
  (void)encoder;
  (void)bitrate_bps;
}

void vkr_video_encoder_flush(VkrVideoEncoder *encoder) { (void)encoder; }

void vkr_video_encoder_destroy(VkrVideoEncoder *encoder) { (void)encoder; }

VkrVideoDecoder *vkr_video_decoder_create(VkrVideoCodec codec) {
  (void)codec;
  return NULL;
}

bool8_t vkr_video_decoder_decode(VkrVideoDecoder *decoder, const uint8_t *data,
                                 uint64_t size, VkrVideoSurface **out_surface) {
  (void)decoder;
  (void)data;
  (void)size;
  if (out_surface) {
    *out_surface = NULL;
  }
  return false_v;
}

void vkr_video_decoder_destroy(VkrVideoDecoder *decoder) { (void)decoder; }

#endif
