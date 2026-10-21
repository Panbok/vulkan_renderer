#pragma once

#include "defines.h"

/* Hardware video coding of BGRA frames over shared GPU surfaces, for view
   streaming (docs/adr/131-view-streaming.md). On macOS a surface is an
   IOSurface-backed CVPixelBuffer and the codecs are VideoToolbox's; on other
   platforms every call reports the service unavailable. */

typedef enum VkrVideoCodec {
  VKR_VIDEO_CODEC_H264 = 1,
} VkrVideoCodec;

/* A BGRA8 image the GPU and the codecs share. Reference counted: a creator
   or decoder hands out one reference, which vkr_video_surface_release
   drops. */
typedef struct VkrVideoSurface VkrVideoSurface;

/* Whether this machine encodes and decodes `codec` in hardware. */
bool8_t vkr_video_supported(VkrVideoCodec codec);

/* A `width` x `height` surface that Metal can wrap as a texture; NULL where
   the service is unavailable or the allocation failed. */
VkrVideoSurface *vkr_video_surface_create(uint32_t width, uint32_t height);
void vkr_video_surface_retain(VkrVideoSurface *surface);
void vkr_video_surface_release(VkrVideoSurface *surface);
uint32_t vkr_video_surface_width(const VkrVideoSurface *surface);
uint32_t vkr_video_surface_height(const VkrVideoSurface *surface);
/* The platform image behind the surface (an IOSurfaceRef on macOS), for a
   renderer to wrap; valid while the caller holds a reference. */
void *vkr_video_surface_platform(const VkrVideoSurface *surface);
/* Copies `height` rows of `width` BGRA pixels between the surface and
   `pixels`, `row_bytes` apart, on the CPU: for tests and for a renderer
   without shared textures. */
bool8_t vkr_video_surface_write(VkrVideoSurface *surface, const uint8_t *pixels,
                                uint64_t row_bytes);
bool8_t vkr_video_surface_read(const VkrVideoSurface *surface, uint8_t *pixels,
                               uint64_t row_bytes);

typedef struct VkrVideoEncoderConfig {
  VkrVideoCodec codec;
  uint32_t width;
  uint32_t height;
  uint32_t frame_rate;
  uint32_t bitrate_bps;
} VkrVideoEncoderConfig;

/* One encoded frame: an Annex B byte stream (start codes before each NAL
   unit) whose key frames carry their parameter sets, so a decoder joins
   at any key frame. `data` is NULL and `size` 0 when the encoder dropped
   the frame. Valid during the callback only. */
typedef struct VkrVideoPacket {
  const uint8_t *data;
  uint64_t size;
  uint64_t timestamp_us;
  uint64_t tag;
  bool8_t key_frame;
} VkrVideoPacket;

/* Called once per encoded or dropped frame, in submission order, on a
   thread of the encoder's; the frame's surface may be written again once
   it returns. */
typedef void (*VkrVideoPacketFn)(void *context, const VkrVideoPacket *packet);

typedef struct VkrVideoEncoder VkrVideoEncoder;

/* A low-latency real-time encoder: no frame reordering, rate control
   within about a frame, a key frame only on request. */
VkrVideoEncoder *vkr_video_encoder_create(const VkrVideoEncoderConfig *config,
                                          VkrVideoPacketFn on_packet,
                                          void *context);
/* Queues `surface`, which must match the configured size, and returns at
   once; `tag` comes back with its packet. False when the encoder refused
   it, and then no packet follows. */
bool8_t vkr_video_encoder_encode(VkrVideoEncoder *encoder,
                                 VkrVideoSurface *surface,
                                 uint64_t timestamp_us, bool8_t key_frame,
                                 uint64_t tag);
void vkr_video_encoder_set_bitrate(VkrVideoEncoder *encoder,
                                   uint32_t bitrate_bps);
/* Waits until every queued frame has produced its packet. */
void vkr_video_encoder_flush(VkrVideoEncoder *encoder);
/* Flushes, then frees the encoder. */
void vkr_video_encoder_destroy(VkrVideoEncoder *encoder);

typedef struct VkrVideoDecoder VkrVideoDecoder;

VkrVideoDecoder *vkr_video_decoder_create(VkrVideoCodec codec);
/* Decodes one Annex B frame on the caller's thread. A frame before the
   first key frame, or one the decoder rejects, gives false: the stream
   needs a key frame. On success `*out_surface` holds one reference to the
   decoded image. */
bool8_t vkr_video_decoder_decode(VkrVideoDecoder *decoder, const uint8_t *data,
                                 uint64_t size, VkrVideoSurface **out_surface);
void vkr_video_decoder_destroy(VkrVideoDecoder *decoder);
