#include "video_test.h"

#include "platform/vkr_video.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_TEST_WIDTH 640u
#define VIDEO_TEST_HEIGHT 360u
#define VIDEO_TEST_FRAMES 30u
#define VIDEO_TEST_KEY_FRAME 15u

typedef struct VideoTestPacket {
  uint8_t *data;
  uint64_t size;
  uint64_t timestamp_us;
  uint64_t tag;
  bool8_t key_frame;
} VideoTestPacket;

typedef struct VideoTestStream {
  VideoTestPacket packets[VIDEO_TEST_FRAMES];
  uint32_t count;
} VideoTestStream;

static void video_test_on_packet(void *context, const VkrVideoPacket *packet) {
  VideoTestStream *stream = context;
  assert(stream->count < VIDEO_TEST_FRAMES);
  VideoTestPacket *copy = &stream->packets[stream->count++];
  copy->size = packet->size;
  copy->timestamp_us = packet->timestamp_us;
  copy->tag = packet->tag;
  copy->key_frame = packet->key_frame;
  copy->data = packet->size ? malloc((size_t)packet->size) : NULL;
  if (packet->size) {
    assert(copy->data);
    memcpy(copy->data, packet->data, (size_t)packet->size);
  }
}

/* Frame `index`: smooth gradients that drift, a bright square moving
   across them. */
static void video_test_frame(uint32_t index, uint8_t *pixels) {
  for (uint32_t y = 0u; y < VIDEO_TEST_HEIGHT; ++y) {
    for (uint32_t x = 0u; x < VIDEO_TEST_WIDTH; ++x) {
      uint8_t *pixel = pixels + ((uint64_t)y * VIDEO_TEST_WIDTH + x) * 4u;
      pixel[0] = (uint8_t)((x + index * 3u) * 255u / (VIDEO_TEST_WIDTH * 2u));
      pixel[1] = (uint8_t)(y * 255u / VIDEO_TEST_HEIGHT);
      pixel[2] = (uint8_t)(64u + (x + y) % 128u / 2u);
      pixel[3] = 255u;
      const uint32_t square_x = 40u + index * 12u;
      if (x >= square_x && x < square_x + 64u && y >= 120u && y < 184u) {
        pixel[0] = 240u;
        pixel[1] = 240u;
        pixel[2] = 240u;
      }
    }
  }
}

/* Peak signal-to-noise ratio over the colour channels, in dB. */
static float64_t video_test_psnr(const uint8_t *a, const uint8_t *b) {
  float64_t error = 0.0;
  const uint64_t pixels = (uint64_t)VIDEO_TEST_WIDTH * VIDEO_TEST_HEIGHT;
  for (uint64_t i = 0u; i < pixels; ++i) {
    for (uint32_t c = 0u; c < 3u; ++c) {
      const float64_t d = (float64_t)a[i * 4u + c] - (float64_t)b[i * 4u + c];
      error += d * d;
    }
  }
  const float64_t mse = error / (float64_t)(pixels * 3u);
  return mse <= 0.0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

/* Whether an Annex B packet holds a NAL unit of `type`. */
static bool8_t video_test_has_unit(const VideoTestPacket *packet,
                                   uint8_t type) {
  for (uint64_t i = 0u; i + 3u < packet->size; ++i) {
    if (packet->data[i] == 0u && packet->data[i + 1u] == 0u &&
        packet->data[i + 2u] == 1u && (packet->data[i + 3u] & 0x1fu) == type) {
      return true_v;
    }
  }
  return false_v;
}

/* Encodes thirty frames and decodes them back. Fails when a packet is
   missing, reordered or loses its tag or time; when the first frame and
   the requested one are not key frames carrying SPS and PPS, or another
   frame is one; when a decoded frame differs from its source by more than
   a hardware codec at 4 Mbps does (PSNR under 30 dB, as a channel swap or
   a stride error would make it); when a decoder takes a frame before any
   key frame; or when a fresh decoder cannot start at the requested key
   frame. */
static void test_video_round_trip(void) {
  printf("  Running test_video_round_trip...\n");
  const uint64_t frame_bytes =
      (uint64_t)VIDEO_TEST_WIDTH * VIDEO_TEST_HEIGHT * 4u;
  uint8_t *sources = malloc((size_t)(frame_bytes * VIDEO_TEST_FRAMES));
  uint8_t *decoded = malloc((size_t)frame_bytes);
  VideoTestStream *stream = calloc(1u, sizeof(VideoTestStream));
  assert(sources && decoded && stream);

  const VkrVideoEncoderConfig config = {.codec = VKR_VIDEO_CODEC_H264,
                                        .width = VIDEO_TEST_WIDTH,
                                        .height = VIDEO_TEST_HEIGHT,
                                        .frame_rate = 60u,
                                        .bitrate_bps = 4000000u};
  VkrVideoEncoder *encoder =
      vkr_video_encoder_create(&config, video_test_on_packet, stream);
  assert(encoder);
  for (uint32_t i = 0u; i < VIDEO_TEST_FRAMES; ++i) {
    uint8_t *source = sources + frame_bytes * i;
    video_test_frame(i, source);
    VkrVideoSurface *surface =
        vkr_video_surface_create(VIDEO_TEST_WIDTH, VIDEO_TEST_HEIGHT);
    assert(surface && vkr_video_surface_platform(surface));
    assert(vkr_video_surface_write(surface, source, VIDEO_TEST_WIDTH * 4u));
    assert(vkr_video_encoder_encode(encoder, surface, 1000u + i * 16667u,
                                    i == 0u || i == VIDEO_TEST_KEY_FRAME,
                                    100u + i));
    /* The encoder keeps the buffer while it reads it. */
    vkr_video_surface_release(surface);
  }
  vkr_video_encoder_flush(encoder);
  vkr_video_encoder_destroy(encoder);

  assert(stream->count == VIDEO_TEST_FRAMES);
  for (uint32_t i = 0u; i < VIDEO_TEST_FRAMES; ++i) {
    const VideoTestPacket *packet = &stream->packets[i];
    assert(packet->size > 0u && packet->tag == 100u + i &&
           packet->timestamp_us == 1000u + i * 16667u);
    const bool8_t requested = i == 0u || i == VIDEO_TEST_KEY_FRAME;
    assert(packet->key_frame == requested);
    assert(video_test_has_unit(packet, 7u) == requested &&
           video_test_has_unit(packet, 8u) == requested);
  }

  VkrVideoDecoder *decoder = vkr_video_decoder_create(VKR_VIDEO_CODEC_H264);
  assert(decoder);
  VkrVideoSurface *image = NULL;
  assert(!vkr_video_decoder_decode(decoder, stream->packets[1].data,
                                   stream->packets[1].size, &image) &&
         !image);
  float64_t worst = 99.0;
  for (uint32_t i = 0u; i < VIDEO_TEST_FRAMES; ++i) {
    const VideoTestPacket *packet = &stream->packets[i];
    assert(
        vkr_video_decoder_decode(decoder, packet->data, packet->size, &image));
    assert(vkr_video_surface_width(image) == VIDEO_TEST_WIDTH &&
           vkr_video_surface_height(image) == VIDEO_TEST_HEIGHT &&
           vkr_video_surface_platform(image));
    assert(vkr_video_surface_read(image, decoded, VIDEO_TEST_WIDTH * 4u));
    vkr_video_surface_release(image);
    image = NULL;
    const float64_t psnr = video_test_psnr(decoded, sources + frame_bytes * i);
    worst = psnr < worst ? psnr : worst;
  }
  vkr_video_decoder_destroy(decoder);
  printf("    worst PSNR %.1f dB over %u frames\n", worst, VIDEO_TEST_FRAMES);
  assert(worst >= 30.0);

  /* A client that joins late starts at the next key frame. */
  decoder = vkr_video_decoder_create(VKR_VIDEO_CODEC_H264);
  assert(decoder);
  for (uint32_t i = VIDEO_TEST_KEY_FRAME; i < VIDEO_TEST_FRAMES; ++i) {
    const VideoTestPacket *packet = &stream->packets[i];
    assert(
        vkr_video_decoder_decode(decoder, packet->data, packet->size, &image));
    vkr_video_surface_release(image);
    image = NULL;
  }
  vkr_video_decoder_destroy(decoder);

  for (uint32_t i = 0u; i < stream->count; ++i) {
    free(stream->packets[i].data);
  }
  free(stream);
  free(decoded);
  free(sources);
  printf("  test_video_round_trip PASSED\n");
}

bool32_t run_video_tests(void) {
  printf("--- Starting Video Tests ---\n");
  if (!vkr_video_supported(VKR_VIDEO_CODEC_H264)) {
    printf("  No hardware H.264 codec here; skipped\n");
    printf("--- Video Tests Completed ---\n");
    return true_v;
  }
  test_video_round_trip();
  printf("--- Video Tests Completed ---\n");
  return true_v;
}
