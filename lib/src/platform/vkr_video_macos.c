/* Hardware video coding on macOS (vkr_video.h): VideoToolbox's H.264
   encoder and decoder over IOSurface-backed CVPixelBuffers, which Metal
   wraps as textures without a copy. VideoToolbox hands out length-prefixed
   NAL units and keeps the parameter sets in a format description; the
   stream here is Annex B with the parameter sets in every key frame, so the
   other end needs nothing else to join it. */
#include "vkr_video.h"

#if defined(PLATFORM_APPLE)
#include "core/logger.h"
#include "core/vkr_atomic.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdlib.h>
#include <string.h>

/* Bytes of the length VideoToolbox writes before each NAL unit. */
#define VKR_VIDEO_NAL_LENGTH_BYTES 4u
#define VKR_VIDEO_H264_SPS 7u
#define VKR_VIDEO_H264_PPS 8u
/* Parameter sets one H.264 stream may carry in a key frame. */
#define VKR_VIDEO_PARAMETER_SETS_MAX 8u

/* Holds one reference to its pixel buffer, which the codecs may retain
   on their own while they read it. */
struct VkrVideoSurface {
  CVPixelBufferRef buffer;
  VkrAtomicUint32 references;
};

static const uint8_t vkr_video_start_code[4] = {0u, 0u, 0u, 1u};

static CFNumberRef vkr_video_number(int32_t value) {
  return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
}

static void vkr_video_set_number(VTSessionRef session, CFStringRef key,
                                 int32_t value) {
  CFNumberRef number = vkr_video_number(value);
  (void)VTSessionSetProperty(session, key, number);
  CFRelease(number);
}

/* The attributes of a BGRA buffer that Metal and the codecs share. */
static CFDictionaryRef vkr_video_buffer_attributes(uint32_t width,
                                                   uint32_t height) {
  CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFDictionaryRef iosurface = CFDictionaryCreate(
      kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFNumberRef format = vkr_video_number((int32_t)kCVPixelFormatType_32BGRA);
  CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey, format);
  CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey,
                       iosurface);
  CFDictionarySetValue(attributes, kCVPixelBufferMetalCompatibilityKey,
                       kCFBooleanTrue);
  if (width && height) {
    CFNumberRef w = vkr_video_number((int32_t)width);
    CFNumberRef h = vkr_video_number((int32_t)height);
    CFDictionarySetValue(attributes, kCVPixelBufferWidthKey, w);
    CFDictionarySetValue(attributes, kCVPixelBufferHeightKey, h);
    CFRelease(w);
    CFRelease(h);
  }
  CFRelease(format);
  CFRelease(iosurface);
  return attributes;
}

/* BT.709 for the encoder's conversion to YCbCr and the decoder's back, so a
   round trip keeps the colours. */
static void vkr_video_tag_buffer(CVPixelBufferRef buffer) {
  CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey,
                        kCVImageBufferColorPrimaries_ITU_R_709_2,
                        kCVAttachmentMode_ShouldPropagate);
  CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey,
                        kCVImageBufferTransferFunction_ITU_R_709_2,
                        kCVAttachmentMode_ShouldPropagate);
  CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey,
                        kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                        kCVAttachmentMode_ShouldPropagate);
}

// =============================================================================
// Surfaces
// =============================================================================

bool8_t vkr_video_supported(VkrVideoCodec codec) {
  if (codec != VKR_VIDEO_CODEC_H264) {
    return false_v;
  }
  return VTIsHardwareDecodeSupported(kCMVideoCodecType_H264) ? true_v : false_v;
}

static VkrVideoSurface *vkr_video_surface_wrap(CVPixelBufferRef buffer) {
  VkrVideoSurface *surface = calloc(1u, sizeof(VkrVideoSurface));
  if (!surface) {
    CVPixelBufferRelease(buffer);
    return NULL;
  }
  surface->buffer = buffer;
  surface->references = 1u;
  return surface;
}

VkrVideoSurface *vkr_video_surface_create(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u) {
    return NULL;
  }
  CFDictionaryRef attributes = vkr_video_buffer_attributes(width, height);
  CVPixelBufferRef buffer = NULL;
  const CVReturn result =
      CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                          kCVPixelFormatType_32BGRA, attributes, &buffer);
  CFRelease(attributes);
  if (result != kCVReturnSuccess || !buffer) {
    return NULL;
  }
  vkr_video_tag_buffer(buffer);
  return vkr_video_surface_wrap(buffer);
}

void vkr_video_surface_retain(VkrVideoSurface *surface) {
  if (surface) {
    (void)vkr_atomic_uint32_fetch_add(&surface->references, 1u,
                                      VKR_MEMORY_ORDER_RELAXED);
  }
}

void vkr_video_surface_release(VkrVideoSurface *surface) {
  if (surface && vkr_atomic_uint32_fetch_sub(&surface->references, 1u,
                                             VKR_MEMORY_ORDER_ACQ_REL) == 1u) {
    CVPixelBufferRelease(surface->buffer);
    free(surface);
  }
}

uint32_t vkr_video_surface_width(const VkrVideoSurface *surface) {
  return surface ? (uint32_t)CVPixelBufferGetWidth(surface->buffer) : 0u;
}

uint32_t vkr_video_surface_height(const VkrVideoSurface *surface) {
  return surface ? (uint32_t)CVPixelBufferGetHeight(surface->buffer) : 0u;
}

void *vkr_video_surface_platform(const VkrVideoSurface *surface) {
  return surface ? (void *)CVPixelBufferGetIOSurface(surface->buffer) : NULL;
}

/* Copies rows between the surface and `pixels`; `to_surface` picks the
   direction. */
static bool8_t vkr_video_surface_copy(CVPixelBufferRef buffer, uint8_t *pixels,
                                      uint64_t row_bytes, bool8_t to_surface) {
  const CVPixelBufferLockFlags flags =
      to_surface ? 0u : kCVPixelBufferLock_ReadOnly;
  if (CVPixelBufferLockBaseAddress(buffer, flags) != kCVReturnSuccess) {
    return false_v;
  }
  uint8_t *base = CVPixelBufferGetBaseAddress(buffer);
  const uint64_t stride = (uint64_t)CVPixelBufferGetBytesPerRow(buffer);
  const uint64_t width_bytes = (uint64_t)CVPixelBufferGetWidth(buffer) * 4u;
  const uint64_t height = (uint64_t)CVPixelBufferGetHeight(buffer);
  const bool8_t ok = base && row_bytes >= width_bytes;
  for (uint64_t y = 0u; ok && y < height; ++y) {
    if (to_surface) {
      MemCopy(base + y * stride, pixels + y * row_bytes, width_bytes);
    } else {
      MemCopy(pixels + y * row_bytes, base + y * stride, width_bytes);
    }
  }
  (void)CVPixelBufferUnlockBaseAddress(buffer, flags);
  return ok;
}

bool8_t vkr_video_surface_write(VkrVideoSurface *surface, const uint8_t *pixels,
                                uint64_t row_bytes) {
  return surface && pixels &&
         vkr_video_surface_copy(surface->buffer, (uint8_t *)pixels, row_bytes,
                                true_v);
}

bool8_t vkr_video_surface_read(const VkrVideoSurface *surface, uint8_t *pixels,
                               uint64_t row_bytes) {
  return surface && pixels &&
         vkr_video_surface_copy(surface->buffer, pixels, row_bytes, false_v);
}

// =============================================================================
// Encoder
// =============================================================================

struct VkrVideoEncoder {
  VTCompressionSessionRef session;
  VkrVideoPacketFn on_packet;
  void *context;
  uint32_t width;
  uint32_t height;
  /* The Annex B form of the packet being reported; VideoToolbox calls the
     output callback of one session in order, one at a time. */
  uint8_t *stream;
  uint64_t stream_capacity;
};

static bool8_t vkr_video_stream_reserve(VkrVideoEncoder *encoder,
                                        uint64_t size) {
  if (size <= encoder->stream_capacity) {
    return true_v;
  }
  const uint64_t capacity = Max(size, encoder->stream_capacity * 2u);
  uint8_t *stream = realloc(encoder->stream, (size_t)capacity);
  if (!stream) {
    return false_v;
  }
  encoder->stream = stream;
  encoder->stream_capacity = capacity;
  return true_v;
}

/* The sample's NAL units as Annex B, after its parameter sets when it is a
   key frame; the size, or 0 when it does not convert. */
static uint64_t vkr_video_annex_b(VkrVideoEncoder *encoder,
                                  CMSampleBufferRef sample, bool8_t key_frame) {
  CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
  const uint64_t length =
      block ? (uint64_t)CMBlockBufferGetDataLength(block) : 0u;
  if (length == 0u) {
    return 0u;
  }
  uint64_t size = 0u;
  if (key_frame) {
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    size_t count = 0u;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            format, 0, NULL, NULL, &count, NULL) != noErr) {
      return 0u;
    }
    for (size_t i = 0u; i < count && i < VKR_VIDEO_PARAMETER_SETS_MAX; ++i) {
      const uint8_t *set = NULL;
      size_t set_size = 0u;
      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
              format, i, &set, &set_size, NULL, NULL) != noErr ||
          !vkr_video_stream_reserve(encoder, size + 4u + set_size)) {
        return 0u;
      }
      MemCopy(encoder->stream + size, vkr_video_start_code, 4u);
      MemCopy(encoder->stream + size + 4u, set, set_size);
      size += 4u + set_size;
    }
  }
  /* Length-prefixed units become start-code-prefixed ones of the same
     size, so the frame copies in place after the parameter sets. */
  if (!vkr_video_stream_reserve(encoder, size + length) ||
      CMBlockBufferCopyDataBytes(block, 0, (size_t)length,
                                 encoder->stream + size) !=
          kCMBlockBufferNoErr) {
    return 0u;
  }
  uint64_t offset = size;
  const uint64_t end = size + length;
  while (offset + VKR_VIDEO_NAL_LENGTH_BYTES <= end) {
    const uint8_t *prefix = encoder->stream + offset;
    const uint64_t unit = ((uint64_t)prefix[0] << 24) |
                          ((uint64_t)prefix[1] << 16) |
                          ((uint64_t)prefix[2] << 8) | (uint64_t)prefix[3];
    if (unit == 0u || unit > end - offset - VKR_VIDEO_NAL_LENGTH_BYTES) {
      return 0u;
    }
    MemCopy(encoder->stream + offset, vkr_video_start_code, 4u);
    offset += VKR_VIDEO_NAL_LENGTH_BYTES + unit;
  }
  return offset == end ? end : 0u;
}

static void vkr_video_encoded(void *output_context, void *frame_context,
                              OSStatus status, VTEncodeInfoFlags flags,
                              CMSampleBufferRef sample) {
  VkrVideoEncoder *encoder = output_context;
  VkrVideoPacket packet = {.tag = (uint64_t)(uintptr_t)frame_context};
  if (status == noErr && sample && !(flags & kVTEncodeInfo_FrameDropped)) {
    packet.timestamp_us = (uint64_t)Max(
        0, CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample),
                              1000000, kCMTimeRoundingMethod_Default)
               .value);
    CFArrayRef attachments =
        CMSampleBufferGetSampleAttachmentsArray(sample, false);
    const CFDictionaryRef first =
        attachments && CFArrayGetCount(attachments) > 0
            ? CFArrayGetValueAtIndex(attachments, 0)
            : NULL;
    packet.key_frame = !first || !CFDictionaryContainsKey(
                                     first, kCMSampleAttachmentKey_NotSync);
    packet.size = vkr_video_annex_b(encoder, sample, packet.key_frame);
    packet.data = packet.size ? encoder->stream : NULL;
  }
  if (!packet.data) {
    packet.size = 0u;
    packet.key_frame = false_v;
  }
  encoder->on_packet(encoder->context, &packet);
}

VkrVideoEncoder *vkr_video_encoder_create(const VkrVideoEncoderConfig *config,
                                          VkrVideoPacketFn on_packet,
                                          void *context) {
  if (!config || !on_packet || config->codec != VKR_VIDEO_CODEC_H264 ||
      config->width == 0u || config->height == 0u) {
    return NULL;
  }
  VkrVideoEncoder *encoder = calloc(1u, sizeof(VkrVideoEncoder));
  if (!encoder) {
    return NULL;
  }
  encoder->on_packet = on_packet;
  encoder->context = context;
  encoder->width = config->width;
  encoder->height = config->height;
  CFMutableDictionaryRef specification = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(specification,
                       kVTVideoEncoderSpecification_EnableLowLatencyRateControl,
                       kCFBooleanTrue);
  CFDictionarySetValue(
      specification,
      kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
      kCFBooleanTrue);
  const OSStatus created = VTCompressionSessionCreate(
      kCFAllocatorDefault, (int32_t)config->width, (int32_t)config->height,
      kCMVideoCodecType_H264, specification, NULL, kCFAllocatorDefault,
      vkr_video_encoded, encoder, &encoder->session);
  CFRelease(specification);
  if (created != noErr || !encoder->session) {
    log_warn("Video: no hardware H.264 encoder for %ux%u (status %d)",
             config->width, config->height, (int)created);
    free(encoder);
    return NULL;
  }
  VTSessionRef session = encoder->session;
  (void)VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime,
                             kCFBooleanTrue);
  (void)VTSessionSetProperty(
      session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
  (void)VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_H264_ConstrainedHigh_AutoLevel);
  vkr_video_set_number(session, kVTCompressionPropertyKey_ExpectedFrameRate,
                       (int32_t)Max(config->frame_rate, 1u));
  vkr_video_set_number(session, kVTCompressionPropertyKey_AverageBitRate,
                       (int32_t)config->bitrate_bps);
  /* Key frames come on request (a joining or recovering client); the
     interval only bounds how long a stream goes without one. */
  vkr_video_set_number(session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
                       (int32_t)(Max(config->frame_rate, 1u) * 60u));
  (void)VTSessionSetProperty(session, kVTCompressionPropertyKey_ColorPrimaries,
                             kCVImageBufferColorPrimaries_ITU_R_709_2);
  (void)VTSessionSetProperty(session,
                             kVTCompressionPropertyKey_TransferFunction,
                             kCVImageBufferTransferFunction_ITU_R_709_2);
  (void)VTSessionSetProperty(session, kVTCompressionPropertyKey_YCbCrMatrix,
                             kCVImageBufferYCbCrMatrix_ITU_R_709_2);
  (void)VTCompressionSessionPrepareToEncodeFrames(encoder->session);
  return encoder;
}

bool8_t vkr_video_encoder_encode(VkrVideoEncoder *encoder,
                                 VkrVideoSurface *surface,
                                 uint64_t timestamp_us, bool8_t key_frame,
                                 uint64_t tag) {
  if (!encoder || !surface ||
      vkr_video_surface_width(surface) != encoder->width ||
      vkr_video_surface_height(surface) != encoder->height) {
    return false_v;
  }
  CFDictionaryRef options = NULL;
  if (key_frame) {
    const void *keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
    const void *values[] = {kCFBooleanTrue};
    options = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                 &kCFTypeDictionaryKeyCallBacks,
                                 &kCFTypeDictionaryValueCallBacks);
  }
  const OSStatus status = VTCompressionSessionEncodeFrame(
      encoder->session, surface->buffer,
      CMTimeMake((int64_t)timestamp_us, 1000000), kCMTimeInvalid, options,
      (void *)(uintptr_t)tag, NULL);
  if (options) {
    CFRelease(options);
  }
  return status == noErr;
}

void vkr_video_encoder_set_bitrate(VkrVideoEncoder *encoder,
                                   uint32_t bitrate_bps) {
  if (encoder && bitrate_bps > 0u) {
    vkr_video_set_number(encoder->session,
                         kVTCompressionPropertyKey_AverageBitRate,
                         (int32_t)Min(bitrate_bps, (uint32_t)INT32_MAX));
  }
}

void vkr_video_encoder_flush(VkrVideoEncoder *encoder) {
  if (encoder) {
    (void)VTCompressionSessionCompleteFrames(encoder->session, kCMTimeInvalid);
  }
}

void vkr_video_encoder_destroy(VkrVideoEncoder *encoder) {
  if (!encoder) {
    return;
  }
  vkr_video_encoder_flush(encoder);
  VTCompressionSessionInvalidate(encoder->session);
  CFRelease(encoder->session);
  free(encoder->stream);
  free(encoder);
}

// =============================================================================
// Decoder
// =============================================================================

struct VkrVideoDecoder {
  VTDecompressionSessionRef session;
  CMVideoFormatDescriptionRef format;
  /* The parameter sets the session was made for, to notice a change. */
  uint8_t *parameters;
  uint64_t parameters_size;
  uint8_t *sample;
  uint64_t sample_capacity;
};

typedef struct VkrVideoDecodeResult {
  CVPixelBufferRef buffer;
} VkrVideoDecodeResult;

static void vkr_video_decoded(void *output_context, void *frame_context,
                              OSStatus status, VTDecodeInfoFlags flags,
                              CVImageBufferRef image, CMTime timestamp,
                              CMTime duration) {
  (void)output_context;
  (void)flags;
  (void)timestamp;
  (void)duration;
  VkrVideoDecodeResult *result = frame_context;
  if (status == noErr && image && !result->buffer) {
    result->buffer = CVPixelBufferRetain(image);
  }
}

VkrVideoDecoder *vkr_video_decoder_create(VkrVideoCodec codec) {
  if (codec != VKR_VIDEO_CODEC_H264) {
    return NULL;
  }
  return calloc(1u, sizeof(VkrVideoDecoder));
}

static void vkr_video_decoder_reset(VkrVideoDecoder *decoder) {
  if (decoder->session) {
    VTDecompressionSessionInvalidate(decoder->session);
    CFRelease(decoder->session);
    decoder->session = NULL;
  }
  if (decoder->format) {
    CFRelease(decoder->format);
    decoder->format = NULL;
  }
  free(decoder->parameters);
  decoder->parameters = NULL;
  decoder->parameters_size = 0u;
}

/* Makes the session for the parameter sets of a key frame, unless the
   current one was made for the same sets. */
static bool8_t vkr_video_decoder_prepare(VkrVideoDecoder *decoder,
                                         const uint8_t *const *sets,
                                         const size_t *set_sizes,
                                         uint32_t set_count) {
  uint64_t size = 0u;
  for (uint32_t i = 0u; i < set_count; ++i) {
    size += set_sizes[i];
  }
  uint8_t *joined = malloc((size_t)Max(size, 1u));
  if (!joined) {
    return false_v;
  }
  uint64_t offset = 0u;
  for (uint32_t i = 0u; i < set_count; ++i) {
    MemCopy(joined + offset, sets[i], set_sizes[i]);
    offset += set_sizes[i];
  }
  if (decoder->session && decoder->parameters_size == size &&
      MemCompare(decoder->parameters, joined, size) == 0) {
    free(joined);
    return true_v;
  }
  vkr_video_decoder_reset(decoder);
  decoder->parameters = joined;
  decoder->parameters_size = size;
  if (CMVideoFormatDescriptionCreateFromH264ParameterSets(
          kCFAllocatorDefault, set_count, sets, set_sizes,
          (int)VKR_VIDEO_NAL_LENGTH_BYTES, &decoder->format) != noErr) {
    vkr_video_decoder_reset(decoder);
    return false_v;
  }
  CFDictionaryRef attributes = vkr_video_buffer_attributes(0u, 0u);
  const VTDecompressionOutputCallbackRecord callback = {
      .decompressionOutputCallback = vkr_video_decoded,
      .decompressionOutputRefCon = decoder};
  const OSStatus created =
      VTDecompressionSessionCreate(kCFAllocatorDefault, decoder->format, NULL,
                                   attributes, &callback, &decoder->session);
  CFRelease(attributes);
  if (created != noErr) {
    vkr_video_decoder_reset(decoder);
    return false_v;
  }
  return true_v;
}

/* The next NAL unit at or after `*offset`: its start and size, past its
   start code; false at the end. */
static bool8_t vkr_video_next_unit(const uint8_t *data, uint64_t size,
                                   uint64_t *offset, uint64_t *out_start,
                                   uint64_t *out_size) {
  uint64_t i = *offset;
  while (i + 3u <= size &&
         !(data[i] == 0u && data[i + 1u] == 0u && data[i + 2u] == 1u)) {
    ++i;
  }
  if (i + 3u > size) {
    return false_v;
  }
  const uint64_t start = i + 3u;
  uint64_t end = start;
  while (end + 3u <= size &&
         !(data[end] == 0u && data[end + 1u] == 0u &&
           (data[end + 2u] == 1u || (data[end + 2u] == 0u && end + 3u < size &&
                                     data[end + 3u] == 1u)))) {
    ++end;
  }
  if (end + 3u > size) {
    end = size;
  }
  *out_start = start;
  *out_size = end - start;
  *offset = end;
  return *out_size > 0u;
}

bool8_t vkr_video_decoder_decode(VkrVideoDecoder *decoder, const uint8_t *data,
                                 uint64_t size, VkrVideoSurface **out_surface) {
  if (!decoder || !data || size == 0u || !out_surface) {
    return false_v;
  }
  *out_surface = NULL;
  const uint8_t *sets[VKR_VIDEO_PARAMETER_SETS_MAX];
  size_t set_sizes[VKR_VIDEO_PARAMETER_SETS_MAX];
  uint32_t set_count = 0u;
  uint64_t sample_size = 0u;
  uint64_t offset = 0u;
  uint64_t start = 0u;
  uint64_t unit = 0u;
  while (vkr_video_next_unit(data, size, &offset, &start, &unit)) {
    const uint8_t type = data[start] & 0x1fu;
    if (type == VKR_VIDEO_H264_SPS || type == VKR_VIDEO_H264_PPS) {
      if (set_count < VKR_VIDEO_PARAMETER_SETS_MAX) {
        sets[set_count] = data + start;
        set_sizes[set_count] = (size_t)unit;
        set_count += 1u;
      }
      continue;
    }
    const uint64_t needed = sample_size + VKR_VIDEO_NAL_LENGTH_BYTES + unit;
    if (needed > decoder->sample_capacity) {
      const uint64_t capacity = Max(needed, decoder->sample_capacity * 2u);
      uint8_t *sample = realloc(decoder->sample, (size_t)capacity);
      if (!sample) {
        return false_v;
      }
      decoder->sample = sample;
      decoder->sample_capacity = capacity;
    }
    uint8_t *prefix = decoder->sample + sample_size;
    prefix[0] = (uint8_t)(unit >> 24);
    prefix[1] = (uint8_t)(unit >> 16);
    prefix[2] = (uint8_t)(unit >> 8);
    prefix[3] = (uint8_t)unit;
    MemCopy(prefix + VKR_VIDEO_NAL_LENGTH_BYTES, data + start, unit);
    sample_size = needed;
  }
  if (set_count >= 2u &&
      !vkr_video_decoder_prepare(decoder, sets, set_sizes, set_count)) {
    return false_v;
  }
  if (!decoder->session || sample_size == 0u) {
    return false_v;
  }
  CMBlockBufferRef block = NULL;
  if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, decoder->sample,
                                         (size_t)sample_size, kCFAllocatorNull,
                                         NULL, 0, (size_t)sample_size, 0,
                                         &block) != kCMBlockBufferNoErr) {
    return false_v;
  }
  CMSampleBufferRef sample = NULL;
  const size_t sample_sizes[] = {(size_t)sample_size};
  const OSStatus made =
      CMSampleBufferCreateReady(kCFAllocatorDefault, block, decoder->format, 1,
                                0, NULL, 1, sample_sizes, &sample);
  CFRelease(block);
  if (made != noErr || !sample) {
    return false_v;
  }
  VkrVideoDecodeResult result = {0};
  VTDecodeInfoFlags flags = 0u;
  const OSStatus decoded = VTDecompressionSessionDecodeFrame(
      decoder->session, sample, 0, &result, &flags);
  CFRelease(sample);
  if (decoded != noErr || !result.buffer) {
    if (result.buffer) {
      CVPixelBufferRelease(result.buffer);
    }
    return false_v;
  }
  *out_surface = vkr_video_surface_wrap(result.buffer);
  return *out_surface != NULL;
}

void vkr_video_decoder_destroy(VkrVideoDecoder *decoder) {
  if (!decoder) {
    return;
  }
  vkr_video_decoder_reset(decoder);
  free(decoder->sample);
  free(decoder);
}

#endif
