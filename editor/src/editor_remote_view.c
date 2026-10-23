#include "editor_remote_view.h"

#include "editor_remote_order.h"

#include "containers/vkr_sort.h"
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "memory/vkr_heap.h"
#include "platform/vkr_platform.h"
#include "platform/vkr_video.h"
#include "renderer/systems/vkr_ui_system.h"
#include "vkr_bakery_json.h"
#include "vkr_bitstream.h"
#include "vkr_depot_object.h"
#include "vkr_depot_sync.h"
#include "vkr_net_crypto.h"
#include "vkr_net_host.h"
#include "vkr_net_session.h"

#define REMOTE_SERVICE_VERSION 1u
#define REMOTE_SCHEMA_HASH 0x5649455700000001ull

/* Service channels: encoded frames on a deadline, the client's camera pose
   (only the newest matters) and stream control. */
#define REMOTE_FRAME_CHANNEL 0u
#define REMOTE_POSE_CHANNEL 1u
#define REMOTE_CONTROL_CHANNEL 2u

#define REMOTE_MSG_FRAME 1u
#define REMOTE_MSG_POSE 2u
/* The client lost a frame or could not decode one: the next is a key
   frame. */
#define REMOTE_MSG_REFRESH 3u

/* A key frame of a 4K Scene fits with room to spare. */
#define REMOTE_FRAME_MAX (8u << 20)
/* Bytes before a frame's coded data. */
#define REMOTE_FRAME_HEADER 64u
/* Surfaces in flight: one a frame renders into, one encoding, two spare. */
#define REMOTE_SLOTS 4u
#define REMOTE_FRAME_RATE 60u
/* A frame the sender could not deliver within two frame intervals, or
   twice the time its bytes take at the stream's rate when that is longer
   (a key frame on a slow link), is dropped by the sender; the client asks
   for a key frame after the gap. The client waits as long for a frame that
   an early one overtook. */
#define REMOTE_DEADLINE_US (2u * 1000000u / REMOTE_FRAME_RATE)
#define REMOTE_DEADLINE_MAX_US 1000000u
/* While no key frame arrives, the client asks again this often. */
#define REMOTE_REFRESH_US 500000u
#define REMOTE_BITRATE_MIN 2000000u
#define REMOTE_BITRATE_MAX 40000000u
#define REMOTE_BITRATE_START 20000000u
/* The rate follows the path's bandwidth estimate this often. */
#define REMOTE_RATE_PERIOD_US 100000u
/* Share of the estimated bandwidth the encoder may use. */
#define REMOTE_RATE_SHARE 0.85
/* Latency samples the status summarizes. */
#define REMOTE_SAMPLES 256u
/* The smallest Scene image worth streaming. */
#define REMOTE_MIN_EXTENT 16u

typedef enum RemoteRole {
  REMOTE_NONE = 0,
  REMOTE_SERVER,
  REMOTE_CLIENT,
} RemoteRole;

/* The server's surfaces and encoder. The renderer reports exports into it
   from its own threads, possibly after the view stopped, so it is heap
   memory freed by its last reference; `lock` (a spin lock, with no
   allocator to outlive) guards `encoder` and `stopped`, and is held while
   a renderer-encoded frame sends, so no send outlives the stop. */
typedef struct RemoteRing {
  VkrAtomicUint32 references;
  VkrAtomicUint32 lock;
  struct VkrEditorRemoteView *view;
  VkrVideoEncoder *encoder;
  bool8_t stopped;
  /* The renderer encodes (vkr_renderer_view_encoding_supported): no
     surfaces or encoder of the ring's own; each export is an encode
     request whose packet comes back before its report. */
  bool8_t encoded;
  uint32_t width;
  uint32_t height;
  VkrVideoSurface *surfaces[REMOTE_SLOTS];
  /* A slot is busy from its hand-out until its frame is sent or dropped. */
  VkrAtomicUint32 busy[REMOTE_SLOTS];
  /* Written by the editor thread before the hand-out: when, the send time
     of the client pose the frame shows, and, for an encode request,
     whether it asked for a key frame. */
  uint64_t handed_us[REMOTE_SLOTS];
  uint64_t pose_time_us[REMOTE_SLOTS];
  bool8_t key_requested[REMOTE_SLOTS];
  /* When the renderer reported the slot written; read by the encoder's
     callback, which follows the encode call. */
  uint64_t written_us[REMOTE_SLOTS];
} RemoteRing;

/* One received frame message, copied out of the transport into one heap
   block. */
typedef struct RemoteFrame {
  uint32_t size;
  uint8_t bytes[];
} RemoteFrame;

/* What a frame's header carries, and the client's times for it. */
typedef struct RemoteFrameInfo {
  uint64_t frame_number;
  uint64_t pose_time_us;
  uint32_t width;
  uint32_t height;
  uint32_t render_us;
  uint32_t encode_us;
  bool8_t key_frame;
  uint64_t received_us;
  uint64_t decoded_us;
} RemoteFrameInfo;

typedef struct RemoteSamples {
  float32_t values[REMOTE_SAMPLES];
  uint32_t count;
  uint32_t next;
} RemoteSamples;

struct VkrEditorRemoteView {
  VkrAllocator *allocator;
  RemoteRole role;
  char error[192];
  char address[64];
  char key[VKR_DEPOT_ID_HEX];
  char identity[VKR_DEPOT_ID_HEX];

  /* The network, pumped by its own thread; fields marked "host lock" are
     shared with it. */
  bool8_t net_memory_ready;
  VkrDMemory net_memory;
  VkrAllocator net_allocator;
  VkrNetHost *host;
  VkrNetSession *net;
  VkrNetHostThread *thread;
  /* Host lock: the encoder thread's send buffer. */
  uint8_t *send_buffer;
  uint32_t send_capacity;

  /* Server. */
  bool8_t restricted;
  uint8_t allowed[VKR_EDITOR_REMOTE_VIEW_ALLOWED_MAX][32];
  uint32_t allowed_count;
  /* Host lock: the client's connection, whether its service is open, and
     its newest pose. */
  VkrNetConnectionId viewer;
  bool8_t viewer_open;
  bool8_t pose_new;
  Vec3 pose_position;
  float32_t pose_yaw;
  float32_t pose_pitch;
  uint64_t pose_time_us;
  /* The next frame encodes as a key frame: a client opened or asked. */
  VkrAtomicUint32 key_wanted;
  RemoteRing *ring;
  uint32_t bitrate_max;
  uint32_t bitrate;
  /* The bitrate the encoder's thread sets deadlines from. */
  VkrAtomicUint32 send_bitrate;
  /* The bitrate asked of the encoder, `bitrate` over its measured
     overshoot; the encoder's thread measures each frame against it. */
  uint32_t encode_bitrate;
  VkrAtomicUint32 asked_bitrate;
  uint64_t rate_checked_us;
  /* Editor thread: the pose time the next export shows, and frames not
     exported while the transport's backlog was over a frame interval. */
  uint64_t shown_pose_time_us;
  uint64_t frames_skipped;
  /* Host lock: frames numbered and sent, their bytes, key frames, frames
     the transport refused, refresh requests, and the server's part of the
     latency. */
  uint64_t frame_number;
  uint64_t frames_sent;
  uint64_t bytes_sent;
  uint64_t key_frames;
  uint64_t send_failures;
  uint64_t refreshes;
  /* Host lock: how far the encoder's frames run over the asked rate. */
  float64_t overshoot;
  RemoteSamples render_ms;
  RemoteSamples encode_ms;
  /* Host lock: every encoded frame's Annex B bytes in one file
     (remote_view.serve 'record'), which keeps the server encoding without
     a client, so a check can decode them on the serving computer. */
  FileHandle record;
  bool8_t recording;
  char record_path[300];
  uint64_t frames_recorded;
  uint64_t bytes_recorded;

  /* Client. */
  VkrNetConnectionId connection;
  /* Host lock. */
  bool8_t open;
  bool8_t closed;
  uint16_t close_code;
  uint64_t frames_received;
  uint64_t bytes_received;
  /* The decoder thread and the frames it decodes in stream order; `mutex`
     guards `order`, `quit`, `latest` and the decoder's counts. */
  bool8_t decoder_running;
  VkrThread decoder_thread;
  VkrMutex mutex;
  VkrCondVar cond;
  VkrEditorRemoteOrder order;
  bool8_t quit;
  VkrVideoSurface *latest;
  RemoteFrameInfo latest_info;
  uint64_t latest_serial;
  uint64_t frames_decoded;
  uint64_t decode_failures;
  /* Decoder thread only. */
  VkrVideoDecoder *decoder;
  /* Editor thread: the frame shown and the latency of each shown frame. */
  VkrVideoSurface *shown;
  RemoteFrameInfo shown_info;
  uint64_t shown_serial;
  uint64_t frames_shown;
  uint64_t refreshes_sent;
  RemoteSamples latency_ms;
  RemoteSamples client_ms;
  RemoteSamples decode_ms;
};

static uint64_t remote_now_us(void) {
  return (uint64_t)(vkr_platform_get_absolute_time() * 1.0e6);
}

static void remote_sample(RemoteSamples *samples, float32_t value) {
  samples->values[samples->next] = value;
  samples->next = (samples->next + 1u) % REMOTE_SAMPLES;
  samples->count = Min(samples->count + 1u, (uint32_t)REMOTE_SAMPLES);
}

static int32_t remote_compare_f32(const void *lhs, const void *rhs) {
  const float32_t a = *(const float32_t *)lhs;
  const float32_t b = *(const float32_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* The p50 and p95 of the kept samples, zero without any. */
static void remote_percentiles(const RemoteSamples *samples, float32_t *p50,
                               float32_t *p95) {
  *p50 = 0.0f;
  *p95 = 0.0f;
  if (!samples->count) {
    return;
  }
  float32_t sorted[REMOTE_SAMPLES];
  MemCopy(sorted, samples->values, samples->count * sizeof(sorted[0]));
  vkr_sort(sorted, samples->count, sizeof(sorted[0]), remote_compare_f32);
  *p50 = sorted[(samples->count - 1u) / 2u];
  *p95 = sorted[((samples->count - 1u) * 95u) / 100u];
}

static void remote_write_f32(VkrBitWriter *writer, float32_t value) {
  uint32_t bits = 0u;
  MemCopy(&bits, &value, sizeof(bits));
  vkr_bit_write(writer, bits, 32u);
}

static float32_t remote_read_f32(VkrBitReader *reader) {
  const uint32_t bits = (uint32_t)vkr_bit_read(reader, 32u);
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

static void remote_fail(VkrEditorRemoteView *view, const char *message) {
  string_format(view->error, sizeof(view->error), "%s", message);
  log_warn("Remote view: %s", message);
}

// =============================================================================
// Server ring
// =============================================================================

static void remote_ring_lock(RemoteRing *ring) {
  uint32_t expected = 0u;
  while (!vkr_atomic_uint32_compare_exchange(&ring->lock, &expected, 1u,
                                             VKR_MEMORY_ORDER_ACQUIRE,
                                             VKR_MEMORY_ORDER_RELAXED)) {
    expected = 0u;
    vkr_thread_yield();
  }
}

static void remote_ring_unlock(RemoteRing *ring) {
  vkr_atomic_uint32_store(&ring->lock, 0u, VKR_MEMORY_ORDER_RELEASE);
}

static void remote_ring_release(RemoteRing *ring) {
  if (vkr_atomic_uint32_fetch_sub(&ring->references, 1u,
                                  VKR_MEMORY_ORDER_ACQ_REL) != 1u) {
    return;
  }
  for (uint32_t i = 0u; i < REMOTE_SLOTS; ++i) {
    vkr_video_surface_release(ring->surfaces[i]);
  }
  vkr_heap_free(ring);
}

/* Builds and sends one encoded frame from the encoder's thread. */
static void remote_send_frame(RemoteRing *ring, uint32_t slot,
                              const VkrVideoPacket *packet) {
  VkrEditorRemoteView *view = ring->view;
  const uint64_t now = remote_now_us();
  const uint64_t written = ring->written_us[slot];
  const uint32_t render_us = (uint32_t)Min(
      written - Min(written, ring->handed_us[slot]), (uint64_t)UINT32_MAX);
  const uint32_t encode_us =
      (uint32_t)Min(now - Min(now, written), (uint64_t)UINT32_MAX);
  if (packet->size > REMOTE_FRAME_MAX - REMOTE_FRAME_HEADER) {
    return;
  }
  const uint32_t size = (uint32_t)packet->size + REMOTE_FRAME_HEADER;
  vkr_net_host_thread_lock(view->thread);
  if (view->recording) {
    uint64_t written_bytes = 0u;
    const FileError written_error =
        file_write(&view->record, packet->size, packet->data, &written_bytes);
    if (written_error == FILE_ERROR_NONE && written_bytes == packet->size) {
      view->frames_recorded += 1u;
      view->bytes_recorded += packet->size;
      /* A recording without a client still reports the server's part. */
      if (!view->viewer_open) {
        remote_sample(&view->render_ms, (float32_t)render_us / 1000.0f);
        remote_sample(&view->encode_ms, (float32_t)encode_us / 1000.0f);
      }
    }
  }
  if (!view->viewer_open) {
    vkr_net_host_thread_unlock(view->thread);
    return;
  }
  if (view->send_capacity < size) {
    uint8_t *grown = vkr_heap_realloc(view->send_buffer, size);
    if (!grown) {
      vkr_net_host_thread_unlock(view->thread);
      return;
    }
    view->send_buffer = grown;
    view->send_capacity = size;
  }
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, view->send_buffer, view->send_capacity);
  vkr_bit_write(&writer, REMOTE_MSG_FRAME, 4u);
  vkr_bit_write(&writer, packet->key_frame ? 1u : 0u, 1u);
  vkr_bit_write(&writer, ring->width, 16u);
  vkr_bit_write(&writer, ring->height, 16u);
  vkr_bit_write(&writer, view->frame_number, 64u);
  vkr_bit_write(&writer, ring->pose_time_us[slot], 64u);
  vkr_bit_write(&writer, render_us, 32u);
  vkr_bit_write(&writer, encode_us, 32u);
  vkr_bit_write_bytes(&writer, packet->data, (uint32_t)packet->size);
  const uint32_t length = vkr_bit_writer_finish(&writer);
  /* Twice the time the frame's bytes and those queued ahead of it take at
     the stream's rate, so a key frame on a slow link still arrives and the
     frames behind it do not expire. */
  const uint32_t bitrate =
      vkr_atomic_uint32_load(&view->send_bitrate, VKR_MEMORY_ORDER_RELAXED);
  const uint64_t queued = vkr_net_session_queued_bytes(
      view->net, view->viewer, VKR_NET_SERVICE_VIEW, REMOTE_FRAME_CHANNEL);
  const uint64_t deadline_us = vkr_editor_remote_frame_deadline_us(
      length, queued, bitrate, REMOTE_DEADLINE_US, REMOTE_DEADLINE_MAX_US);
  const VkrNetSendOptions options = {
      .flags = VKR_NET_SEND_IMMEDIATE,
      .deadline_us = vkr_net_host_now(view->host) + deadline_us,
  };
  const VkrNetSendStatus status =
      length
          ? vkr_net_session_send(view->net, view->viewer, VKR_NET_SERVICE_VIEW,
                                 REMOTE_FRAME_CHANNEL, view->send_buffer,
                                 length, &options, vkr_net_host_now(view->host))
          : VKR_NET_SEND_INVALID;
  /* A frame the transport refused leaves a gap the client asks a key frame
     after; the numbers stay contiguous for frames that left. */
  if (status == VKR_NET_SEND_OK) {
    view->frame_number += 1u;
    view->frames_sent += 1u;
    view->bytes_sent += length;
    view->key_frames += packet->key_frame ? 1u : 0u;
    view->overshoot = vkr_editor_remote_overshoot(
        view->overshoot, packet->size,
        vkr_atomic_uint32_load(&view->asked_bitrate, VKR_MEMORY_ORDER_RELAXED),
        REMOTE_FRAME_RATE);
    remote_sample(&view->render_ms, (float32_t)render_us / 1000.0f);
    remote_sample(&view->encode_ms, (float32_t)encode_us / 1000.0f);
  } else {
    view->send_failures += 1u;
  }
  vkr_net_host_thread_unlock(view->thread);
}

/* The encoder's callback: sends the frame and frees its slot. A frame the
   renderer encoded sends while the ring is not stopped, and its slot frees
   on the report that follows. */
static void remote_packet(void *context, const VkrVideoPacket *packet) {
  RemoteRing *ring = context;
  const uint32_t slot = (uint32_t)(packet->tag % REMOTE_SLOTS);
  if (ring->encoded) {
    remote_ring_lock(ring);
    if (!ring->stopped && packet->data && packet->size) {
      ring->written_us[slot] = packet->timestamp_us;
      remote_send_frame(ring, slot, packet);
    }
    remote_ring_unlock(ring);
    return;
  }
  if (packet->data && packet->size) {
    remote_send_frame(ring, slot, packet);
  }
  vkr_atomic_uint32_store(&ring->busy[slot], 0u, VKR_MEMORY_ORDER_RELEASE);
}

/* The renderer's report of an export (VkrViewExportDone): a written slot
   encodes at once, any other is free again. A renderer-encoded slot frees
   either way, and one that asked for a key frame and made none asks
   again. Runs on the rendering or the completion thread. */
static void remote_export_done(void *context, uint64_t tag, bool8_t written) {
  RemoteRing *ring = context;
  const uint32_t slot = (uint32_t)(tag % REMOTE_SLOTS);
  if (ring->encoded) {
    if (!written && ring->key_requested[slot]) {
      remote_ring_lock(ring);
      if (!ring->stopped) {
        vkr_atomic_uint32_store(&ring->view->key_wanted, 1u,
                                VKR_MEMORY_ORDER_RELEASE);
      }
      remote_ring_unlock(ring);
    }
    vkr_atomic_uint32_store(&ring->busy[slot], 0u, VKR_MEMORY_ORDER_RELEASE);
    remote_ring_release(ring);
    return;
  }
  bool8_t queued = false_v;
  if (written) {
    remote_ring_lock(ring);
    if (!ring->stopped && ring->encoder) {
      ring->written_us[slot] = remote_now_us();
      const bool8_t key =
          vkr_atomic_uint32_exchange(&ring->view->key_wanted, 0u,
                                     VKR_MEMORY_ORDER_ACQ_REL) != 0u;
      queued = vkr_video_encoder_encode(ring->encoder, ring->surfaces[slot],
                                        ring->written_us[slot], key, tag);
      if (!queued && key) {
        vkr_atomic_uint32_store(&ring->view->key_wanted, 1u,
                                VKR_MEMORY_ORDER_RELEASE);
      }
    }
    remote_ring_unlock(ring);
  }
  if (!queued) {
    vkr_atomic_uint32_store(&ring->busy[slot], 0u, VKR_MEMORY_ORDER_RELEASE);
  }
  remote_ring_release(ring);
}

static RemoteRing *remote_ring_create(VkrEditorRemoteView *view, uint32_t width,
                                      uint32_t height, bool8_t encoded) {
  RemoteRing *ring = vkr_heap_alloc_zeroed(1u, sizeof(*ring));
  if (!ring) {
    return NULL;
  }
  vkr_atomic_uint32_store(&ring->references, 1u, VKR_MEMORY_ORDER_RELEASE);
  ring->view = view;
  ring->encoded = encoded;
  ring->width = width;
  ring->height = height;
  if (encoded) {
    return ring;
  }
  for (uint32_t i = 0u; i < REMOTE_SLOTS; ++i) {
    ring->surfaces[i] = vkr_video_surface_create(width, height);
    if (!ring->surfaces[i]) {
      goto cleanup;
    }
  }
  const VkrVideoEncoderConfig config = {
      .codec = VKR_VIDEO_CODEC_H264,
      .width = width,
      .height = height,
      .frame_rate = REMOTE_FRAME_RATE,
      .bitrate_bps = view->encode_bitrate,
  };
  ring->encoder = vkr_video_encoder_create(&config, remote_packet, ring);
  if (!ring->encoder) {
    goto cleanup;
  }
  return ring;
cleanup:
  remote_ring_release(ring);
  return NULL;
}

/* Stops encoding: exports still out free their slots, and once the encoder
   flushed no callback reaches the view. */
static void remote_ring_stop(RemoteRing *ring) {
  if (!ring) {
    return;
  }
  remote_ring_lock(ring);
  ring->stopped = true_v;
  VkrVideoEncoder *encoder = ring->encoder;
  ring->encoder = NULL;
  remote_ring_unlock(ring);
  vkr_video_encoder_destroy(encoder);
  remote_ring_release(ring);
}

// =============================================================================
// Client decoder
// =============================================================================

static bool8_t remote_read_header(const uint8_t *bytes, uint32_t size,
                                  RemoteFrameInfo *info, const uint8_t **data,
                                  uint32_t *data_size) {
  VkrBitReader reader;
  vkr_bit_reader_init(&reader, bytes, size);
  if (vkr_bit_read(&reader, 4u) != REMOTE_MSG_FRAME) {
    return false_v;
  }
  info->key_frame = vkr_bit_read(&reader, 1u) != 0u;
  info->width = (uint32_t)vkr_bit_read(&reader, 16u);
  info->height = (uint32_t)vkr_bit_read(&reader, 16u);
  info->frame_number = vkr_bit_read(&reader, 64u);
  info->pose_time_us = vkr_bit_read(&reader, 64u);
  info->render_us = (uint32_t)vkr_bit_read(&reader, 32u);
  info->encode_us = (uint32_t)vkr_bit_read(&reader, 32u);
  vkr_bit_read_align(&reader);
  if (reader.overflow || reader.bytes > size) {
    return false_v;
  }
  *data = bytes + reader.bytes;
  *data_size = size - reader.bytes;
  return *data_size > 0u;
}

/* Decodes the next frame of the stream; a refused one asks for a key
   frame. */
static void remote_decode(VkrEditorRemoteView *view, const RemoteFrame *frame,
                          uint64_t received_us) {
  RemoteFrameInfo info = {.received_us = received_us};
  const uint8_t *data = NULL;
  uint32_t data_size = 0u;
  VkrVideoSurface *surface = NULL;
  if (!remote_read_header(frame->bytes, frame->size, &info, &data,
                          &data_size) ||
      !vkr_video_decoder_decode(view->decoder, data, data_size, &surface)) {
    vkr_mutex_lock(view->mutex);
    vkr_editor_remote_order_decode_failed(&view->order);
    view->decode_failures += 1u;
    vkr_mutex_unlock(view->mutex);
    vkr_net_host_wake(view->host);
    return;
  }
  info.decoded_us = remote_now_us();
  vkr_mutex_lock(view->mutex);
  VkrVideoSurface *replaced = view->latest;
  view->latest = surface;
  view->latest_info = info;
  view->latest_serial += 1u;
  view->frames_decoded += 1u;
  vkr_mutex_unlock(view->mutex);
  vkr_video_surface_release(replaced);
}

/* Takes frames from the order as it hands them out. A frame held for the
   reorder window is looked at again when another arrives or the client's
   build wakes the thread. */
static void *remote_decoder_main(void *argument) {
  VkrEditorRemoteView *view = argument;
  (void)vkr_thread_set_name("vkr.remote_view.decode");
  for (;;) {
    VkrEditorRemoteOrderFrame frame = {0};
    VkrEditorRemoteOrderStep step = VKR_EDITOR_REMOTE_ORDER_WAIT;
    vkr_mutex_lock(view->mutex);
    while (!view->quit) {
      step =
          vkr_editor_remote_order_next(&view->order, remote_now_us(), &frame);
      if (step != VKR_EDITOR_REMOTE_ORDER_WAIT) {
        break;
      }
      vkr_cond_wait(view->cond, view->mutex);
    }
    const bool8_t quit = view->quit;
    const bool8_t ask = view->order.ask_now;
    vkr_mutex_unlock(view->mutex);
    if (quit) {
      vkr_heap_free(frame.payload);
      return NULL;
    }
    /* The host thread's step sends the key frame request. */
    if (ask) {
      vkr_net_host_wake(view->host);
    }
    if (step == VKR_EDITOR_REMOTE_ORDER_DECODE) {
      remote_decode(view, frame.payload, frame.received_us);
    }
    vkr_heap_free(frame.payload);
  }
}

/* From the host thread: copies a received frame and hands it to the
   order, which holds it until it decodes in stream order. */
static void remote_queue_frame(VkrEditorRemoteView *view, const uint8_t *data,
                               uint32_t size) {
  RemoteFrameInfo info = {0};
  const uint8_t *coded = NULL;
  uint32_t coded_size = 0u;
  if (!remote_read_header(data, size, &info, &coded, &coded_size)) {
    return;
  }
  RemoteFrame *frame = vkr_heap_alloc(sizeof(*frame) + size);
  if (!frame) {
    return;
  }
  frame->size = size;
  MemCopy(frame->bytes, data, size);
  view->frames_received += 1u;
  view->bytes_received += size;
  const VkrEditorRemoteOrderFrame held = {
      .number = info.frame_number,
      .key_frame = info.key_frame,
      .received_us = remote_now_us(),
      .payload = frame,
  };
  vkr_mutex_lock(view->mutex);
  const bool8_t accepted = vkr_editor_remote_order_push(&view->order, &held);
  if (accepted) {
    vkr_cond_signal(view->cond);
  }
  vkr_mutex_unlock(view->mutex);
  if (!accepted) {
    vkr_heap_free(frame);
  }
}

static bool8_t remote_decoder_start(VkrEditorRemoteView *view) {
  view->decoder = vkr_video_decoder_create(VKR_VIDEO_CODEC_H264);
  if (!view->decoder || !vkr_mutex_create(view->allocator, &view->mutex) ||
      !vkr_cond_create(view->allocator, &view->cond)) {
    return false_v;
  }
  /* Made again as the stream opens (remote_service_opened). */
  vkr_editor_remote_order_init(&view->order, remote_now_us(),
                               REMOTE_DEADLINE_US, REMOTE_REFRESH_US);
  view->quit = false_v;
  view->decoder_running = vkr_thread_create(
      view->allocator, &view->decoder_thread, remote_decoder_main, view);
  return view->decoder_running;
}

static void remote_decoder_stop(VkrEditorRemoteView *view) {
  if (view->decoder_running) {
    vkr_mutex_lock(view->mutex);
    view->quit = true_v;
    vkr_cond_signal(view->cond);
    vkr_mutex_unlock(view->mutex);
    (void)vkr_thread_join(view->decoder_thread);
    (void)vkr_thread_destroy(view->allocator, &view->decoder_thread);
    view->decoder_running = false_v;
  }
  for (uint32_t i = 0u; i < view->order.count; ++i) {
    vkr_heap_free(view->order.held[i].payload);
  }
  view->order.count = 0u;
  if (view->cond) {
    (void)vkr_cond_destroy(view->allocator, &view->cond);
  }
  if (view->mutex) {
    (void)vkr_mutex_destroy(view->allocator, &view->mutex);
  }
  vkr_video_decoder_destroy(view->decoder);
  view->decoder = NULL;
  vkr_video_surface_release(view->latest);
  view->latest = NULL;
}

// =============================================================================
// Network
// =============================================================================

static bool8_t remote_allowed(const VkrEditorRemoteView *view,
                              const uint8_t key[32]) {
  for (uint32_t i = 0u; i < view->allowed_count; ++i) {
    if (MemCompare(view->allowed[i], key, 32u) == 0) {
      return true_v;
    }
  }
  return false_v;
}

static void remote_accept(void *context, const VkrNetAddress *address,
                          const uint8_t peer_key[32], const uint8_t *credential,
                          uint32_t credential_size,
                          VkrNetAcceptResult *out_result) {
  (void)address;
  (void)credential;
  (void)credential_size;
  VkrEditorRemoteView *view = context;
  if (view->restricted && !remote_allowed(view, peer_key)) {
    out_result->code = VKR_NET_CLOSE_ACCESS_DENIED;
  } else if (view->viewer != VKR_NET_CONNECTION_NONE) {
    out_result->code = VKR_NET_CLOSE_SERVER_FULL;
  }
}

static uint16_t remote_service_accept(void *context,
                                      VkrNetConnectionId connection,
                                      uint16_t version) {
  (void)version;
  VkrEditorRemoteView *view = context;
  return view->role == REMOTE_SERVER && connection == view->viewer
             ? 0u
             : (uint16_t)VKR_NET_CLOSE_SERVICE_REFUSED;
}

static void remote_service_opened(void *context, VkrNetConnectionId connection,
                                  uint16_t version) {
  (void)version;
  VkrEditorRemoteView *view = context;
  if (view->role == REMOTE_SERVER && connection == view->viewer) {
    view->viewer_open = true_v;
    vkr_atomic_uint32_store(&view->key_wanted, 1u, VKR_MEMORY_ORDER_RELEASE);
    log_info("Remote view: a client opened the stream");
  } else if (view->role == REMOTE_CLIENT && connection == view->connection) {
    view->open = true_v;
    /* The server sends a key frame as the stream opens. */
    vkr_mutex_lock(view->mutex);
    vkr_editor_remote_order_init(&view->order, remote_now_us(),
                                 REMOTE_DEADLINE_US, REMOTE_REFRESH_US);
    vkr_mutex_unlock(view->mutex);
    log_info("Remote view: showing the Scene of %s", view->address);
  }
}

static void remote_service_event(void *context, VkrNetConnectionId connection,
                                 uint8_t channel, const VkrNetEvent *event,
                                 uint64_t now_us) {
  (void)now_us;
  VkrEditorRemoteView *view = context;
  if (event->type != VKR_NET_EVENT_MESSAGE || !event->size) {
    return;
  }
  if (view->role == REMOTE_CLIENT) {
    if (connection == view->connection && channel == REMOTE_FRAME_CHANNEL) {
      remote_queue_frame(view, event->data, event->size);
    }
    return;
  }
  if (view->role != REMOTE_SERVER || connection != view->viewer) {
    return;
  }
  VkrBitReader reader;
  vkr_bit_reader_init(&reader, event->data, event->size);
  const uint64_t type = vkr_bit_read(&reader, 4u);
  if (channel == REMOTE_POSE_CHANNEL && type == REMOTE_MSG_POSE) {
    Vec3 position;
    position.x = remote_read_f32(&reader);
    position.y = remote_read_f32(&reader);
    position.z = remote_read_f32(&reader);
    const float32_t yaw = remote_read_f32(&reader);
    const float32_t pitch = remote_read_f32(&reader);
    const uint64_t sent_us = vkr_bit_read(&reader, 64u);
    const bool8_t finite = vkr_is_finite_f32(position.x) &&
                           vkr_is_finite_f32(position.y) &&
                           vkr_is_finite_f32(position.z) &&
                           vkr_is_finite_f32(yaw) && vkr_is_finite_f32(pitch);
    if (!reader.overflow && finite) {
      view->pose_position = position;
      view->pose_yaw = yaw;
      view->pose_pitch = vkr_clamp_f32(pitch, -89.0f, 89.0f);
      view->pose_time_us = sent_us;
      view->pose_new = true_v;
    }
  } else if (channel == REMOTE_CONTROL_CHANNEL && type == REMOTE_MSG_REFRESH) {
    view->refreshes += 1u;
    vkr_atomic_uint32_store(&view->key_wanted, 1u, VKR_MEMORY_ORDER_RELEASE);
  }
}

static void remote_service_closed(void *context, VkrNetConnectionId connection,
                                  uint16_t code) {
  VkrEditorRemoteView *view = context;
  if (view->role == REMOTE_SERVER && connection == view->viewer) {
    view->viewer_open = false_v;
  } else if (view->role == REMOTE_CLIENT && connection == view->connection) {
    view->open = false_v;
    view->closed = true_v;
    view->close_code = code;
  }
}

static void remote_send_small(VkrEditorRemoteView *view, uint8_t channel,
                              const uint8_t *bytes, uint32_t size) {
  const VkrNetSendOptions options = {.flags = VKR_NET_SEND_IMMEDIATE};
  (void)vkr_net_session_send(view->net, view->connection, VKR_NET_SERVICE_VIEW,
                             channel, bytes, size, &options,
                             vkr_net_host_now(view->host));
}

/* The host thread's step, with its lock held: core events, then a refresh
   the decoder asked for. */
static void remote_step(void *context, uint64_t now_us) {
  VkrEditorRemoteView *view = context;
  VkrNetCore *core = vkr_net_host_core(view->host);
  VkrNetEvent event;
  while (vkr_net_core_poll(core, &event)) {
    if (vkr_net_session_dispatch(view->net, &event, now_us)) {
      continue;
    }
    if (view->role == REMOTE_SERVER) {
      if (event.type == VKR_NET_EVENT_CONNECTED &&
          view->viewer == VKR_NET_CONNECTION_NONE) {
        view->viewer = event.connection;
      } else if (event.type == VKR_NET_EVENT_CLOSED &&
                 event.connection == view->viewer) {
        view->viewer = VKR_NET_CONNECTION_NONE;
        view->viewer_open = false_v;
        log_info("Remote view: the client left");
      }
    } else if (event.connection == view->connection) {
      if (event.type == VKR_NET_EVENT_CONNECTED) {
        (void)vkr_net_session_open(view->net, view->connection,
                                   VKR_NET_SERVICE_VIEW, now_us);
      } else if (event.type == VKR_NET_EVENT_CLOSED) {
        view->open = false_v;
        view->closed = true_v;
        view->close_code = event.code;
      }
    }
  }
  bool8_t want_key = false_v;
  VkrEditorRemoteOrder order = {0};
  if (view->role == REMOTE_CLIENT && view->open) {
    vkr_mutex_lock(view->mutex);
    want_key = vkr_editor_remote_order_want_key(&view->order, remote_now_us());
    order = view->order;
    vkr_mutex_unlock(view->mutex);
  }
  if (want_key) {
    log_info("Remote view: asking for a key frame (next frame %llu, %u "
             "held, %llu gaps, %llu dropped)",
             (unsigned long long)order.expected, order.count,
             (unsigned long long)order.frame_gaps,
             (unsigned long long)order.frames_dropped);
    uint8_t bytes[1];
    VkrBitWriter writer;
    vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
    vkr_bit_write(&writer, REMOTE_MSG_REFRESH, 4u);
    const uint32_t size = vkr_bit_writer_finish(&writer);
    remote_send_small(view, REMOTE_CONTROL_CHANNEL, bytes, size);
    view->refreshes_sent += 1u;
  }
}

static bool8_t remote_start_network(VkrEditorRemoteView *view,
                                    const VkrNetHostConfig *config) {
  if (!view->net_memory_ready) {
    if (!vkr_dmemory_create(MB(8), GB(1), &view->net_memory)) {
      return false_v;
    }
    view->net_allocator = (VkrAllocator){.ctx = &view->net_memory};
    vkr_dmemory_allocator_create(&view->net_allocator);
    view->net_memory_ready = true_v;
  }
  view->host = vkr_net_host_create(&view->net_allocator, config);
  view->net = view->host ? vkr_net_session_create(&view->net_allocator,
                                                  vkr_net_host_core(view->host))
                         : NULL;
  if (!view->net) {
    return false_v;
  }
  const VkrNetService service = {
      .id = VKR_NET_SERVICE_VIEW,
      .versions = {{REMOTE_SERVICE_VERSION, REMOTE_SCHEMA_HASH}},
      .version_count = 1u,
      .channels =
          {
              [REMOTE_FRAME_CHANNEL] = {.delivery = VKR_NET_DEADLINE,
                                        .priority = 1u,
                                        .weight = 1u,
                                        .max_message_size = REMOTE_FRAME_MAX,
                                        .receive_window = 4u * REMOTE_FRAME_MAX,
                                        .send_queue = 64u,
                                        .deadline_ms = 33u},
              [REMOTE_POSE_CHANNEL] = {.delivery = VKR_NET_SEQUENCED,
                                       .priority = 0u,
                                       .weight = 1u,
                                       .max_message_size = 64u},
              [REMOTE_CONTROL_CHANNEL] = {.delivery = VKR_NET_RELIABLE_ORDERED,
                                          .priority = 0u,
                                          .weight = 1u,
                                          .max_message_size = 256u},
          },
      .channel_count = 3u,
      .context = view,
      .accept = remote_service_accept,
      .opened = remote_service_opened,
      .event = remote_service_event,
      .closed = remote_service_closed,
  };
  if (!vkr_net_session_register(view->net, &service)) {
    return false_v;
  }
  view->thread =
      vkr_net_host_thread_start(view->allocator, view->host, remote_step, view);
  return view->thread != NULL;
}

static void remote_reset(VkrEditorRemoteView *view) {
  /* No encoder callback sends once the ring stopped, so the host thread
     may stop next. */
  remote_ring_stop(view->ring);
  view->ring = NULL;
  if (view->thread) {
    vkr_net_host_thread_stop(view->thread);
    view->thread = NULL;
  }
  remote_decoder_stop(view);
  if (view->host) {
    VkrNetCore *core = vkr_net_host_core(view->host);
    const uint64_t now = vkr_net_host_now(view->host);
    if (view->role == REMOTE_CLIENT &&
        view->connection != VKR_NET_CONNECTION_NONE) {
      vkr_net_core_close(core, view->connection, VKR_NET_CLOSE_APPLICATION,
                         now);
    }
    if (view->role == REMOTE_SERVER &&
        view->viewer != VKR_NET_CONNECTION_NONE) {
      vkr_net_core_close(core, view->viewer, VKR_NET_CLOSE_APPLICATION, now);
    }
    /* Lets the close leave before the socket goes. */
    vkr_net_core_flush(core);
    (void)vkr_net_host_pump(view->host, 0);
  }
  vkr_net_session_destroy(view->net);
  vkr_net_host_destroy(view->host);
  if (view->net_memory_ready) {
    vkr_dmemory_allocator_destroy(&view->net_allocator);
  }
  vkr_heap_free(view->send_buffer);
  vkr_video_surface_release(view->shown);
  if (view->recording) {
    file_close(&view->record);
  }
  /* The reason a view ended stays for its status. */
  VkrAllocator *allocator = view->allocator;
  char error[sizeof(view->error)];
  MemCopy(error, view->error, sizeof(error));
  MemZero(view, sizeof(*view));
  view->allocator = allocator;
  MemCopy(view->error, error, sizeof(error));
}

// =============================================================================
// Public
// =============================================================================

VkrEditorRemoteView *vkr_editor_remote_view_create(VkrAllocator *allocator) {
  VkrEditorRemoteView *view = vkr_heap_alloc_zeroed(1u, sizeof(*view));
  if (view) {
    view->allocator = allocator;
  }
  return view;
}

void vkr_editor_remote_view_destroy(VkrEditorRemoteView *view) {
  if (!view) {
    return;
  }
  remote_reset(view);
  vkr_heap_free(view);
}

/* A server encodes in vkr_video.h or the renderer; a client decodes in
   vkr_video.h and imports into a renderer that takes imports. */
static bool8_t remote_begin(VkrEditorRemoteView *view,
                            const VkrSampleUiFrame *frame, bool8_t serving,
                            char *error, uint32_t capacity) {
  if (view->role != REMOTE_NONE) {
    string_format(error, capacity, "Stop the current remote view first.");
    return false_v;
  }
  const bool8_t video = vkr_video_supported(VKR_VIDEO_CODEC_H264);
  const bool8_t coding =
      serving ? video || frame->view_encoding : video && !frame->view_encoding;
  if (!frame->view_streaming || !frame->view_stream_request || !coding) {
    string_format(error, capacity,
                  "This renderer or computer cannot stream the Scene.");
    return false_v;
  }
  if (!vkr_net_crypto_init()) {
    string_format(error, capacity, "The network cryptography did not start.");
    return false_v;
  }
  view->error[0] = '\0';
  return true_v;
}

bool8_t vkr_editor_remote_view_serve(
    VkrEditorRemoteView *view, const VkrSampleUiFrame *frame, const char *bind,
    const uint8_t (*allowed)[32], uint32_t allowed_count,
    uint32_t bitrate_max_bps, const char *record_path, char *error,
    uint32_t capacity) {
  if (!view || !remote_begin(view, frame, true_v, error, capacity)) {
    return false_v;
  }
  if (allowed_count > VKR_EDITOR_REMOTE_VIEW_ALLOWED_MAX) {
    string_format(error, capacity, "Allow at most %u identity keys.",
                  VKR_EDITOR_REMOTE_VIEW_ALLOWED_MAX);
    return false_v;
  }
  VkrNetHostConfig config = {
      .socket = {.receive_buffer = 4u << 20, .send_buffer = 8u << 20}};
  if (!vkr_net_address_parse(bind && bind[0] ? bind : "[::]",
                             VKR_EDITOR_REMOTE_VIEW_PORT, &config.bind)) {
    string_format(error, capacity, "Give a numeric address to serve on.");
    return false_v;
  }
  vkr_net_keypair_generate(&config.core.static_keys);
  config.core.accept_incoming = true_v;
  config.core.accept = remote_accept;
  config.core.accept_context = view;
  config.core.max_connections = 4u;
  view->role = REMOTE_SERVER;
  view->restricted = allowed_count > 0u;
  view->allowed_count = allowed_count;
  if (allowed_count) {
    MemCopy(view->allowed, allowed, allowed_count * sizeof(view->allowed[0]));
  }
  view->bitrate_max = bitrate_max_bps
                          ? Max(bitrate_max_bps, (uint32_t)REMOTE_BITRATE_MIN)
                          : REMOTE_BITRATE_MAX;
  view->bitrate = Min(view->bitrate_max, (uint32_t)REMOTE_BITRATE_START);
  view->encode_bitrate = view->bitrate;
  view->overshoot = 1.0;
  vkr_atomic_uint32_store(&view->send_bitrate, view->bitrate,
                          VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint32_store(&view->asked_bitrate, view->encode_bitrate,
                          VKR_MEMORY_ORDER_RELAXED);
  if (record_path) {
    FileMode mode = bitset8_create();
    bitset8_set(&mode, FILE_MODE_WRITE);
    bitset8_set(&mode, FILE_MODE_BINARY);
    bitset8_set(&mode, FILE_MODE_TRUNCATE);
    const FilePath path = file_path_view(record_path);
    if (file_open(&path, mode, &view->record) != FILE_ERROR_NONE) {
      remote_reset(view);
      string_format(error, capacity, "Could not write the recording %s.",
                    record_path);
      return false_v;
    }
    view->recording = true_v;
    string_format(view->record_path, sizeof(view->record_path), "%s",
                  record_path);
  }
  if (!remote_start_network(view, &config)) {
    remote_reset(view);
    string_format(error, capacity, "Could not listen on %s.",
                  bind && bind[0] ? bind : "every interface");
    return false_v;
  }
  VkrNetAddress local;
  (void)vkr_net_host_local_address(view->host, &local);
  vkr_net_address_format(&local, view->address, sizeof(view->address));
  VkrDepotId key;
  MemCopy(key.bytes, config.core.static_keys.public_key, sizeof(key.bytes));
  vkr_depot_id_hex(&key, view->key);
  vkr_net_random(config.core.static_keys.secret_key,
                 sizeof(config.core.static_keys.secret_key));
  log_info("Serving the Scene on %s, key %s", view->address, view->key);
  return true_v;
}

bool8_t vkr_editor_remote_view_connect(VkrEditorRemoteView *view,
                                       const VkrSampleUiFrame *frame,
                                       const char *address, const char *key_hex,
                                       char *error, uint32_t capacity) {
  if (!view || !remote_begin(view, frame, false_v, error, capacity)) {
    return false_v;
  }
  VkrNetAddress server;
  VkrDepotId server_key;
  if (!address ||
      !vkr_net_address_parse(address, VKR_EDITOR_REMOTE_VIEW_PORT, &server)) {
    string_format(error, capacity, "Give the server's numeric address.");
    return false_v;
  }
  if (!key_hex || string_length(key_hex) != 64u ||
      !vkr_depot_id_parse(key_hex, &server_key)) {
    string_format(error, capacity, "The key must be 64 hexadecimal digits.");
    return false_v;
  }
  VkrNetHostConfig config = {
      .socket = {.receive_buffer = 8u << 20, .send_buffer = 1u << 20}};
  config.bind = vkr_net_address_any((VkrNetAddressFamily)server.family, 0u);
  /* The identity key is what a restricted server allows. */
  if (!vkr_depot_identity(&config.core.static_keys, true_v)) {
    log_warn("Remote view: no identity key; connecting with a fresh key, "
             "which a restricted server refuses");
    vkr_net_keypair_generate(&config.core.static_keys);
  }
  VkrDepotId identity;
  MemCopy(identity.bytes, config.core.static_keys.public_key,
          sizeof(identity.bytes));
  vkr_depot_id_hex(&identity, view->identity);
  view->role = REMOTE_CLIENT;
  if (!remote_decoder_start(view) || !remote_start_network(view, &config)) {
    remote_reset(view);
    vkr_net_random(config.core.static_keys.secret_key,
                   sizeof(config.core.static_keys.secret_key));
    string_format(error, capacity, "Could not start the remote view.");
    return false_v;
  }
  vkr_net_host_thread_lock(view->thread);
  const bool8_t started = vkr_net_core_connect(
      vkr_net_host_core(view->host), &server, server_key.bytes, NULL, 0u,
      vkr_net_host_now(view->host), &view->connection);
  vkr_net_host_thread_unlock(view->thread);
  vkr_net_random(config.core.static_keys.secret_key,
                 sizeof(config.core.static_keys.secret_key));
  if (!started) {
    remote_reset(view);
    string_format(error, capacity, "Could not start the connection.");
    return false_v;
  }
  string_format(view->address, sizeof(view->address), "%s", address);
  MemCopy(view->key, key_hex, 64u);
  view->key[64] = '\0';
  log_info("Remote view: connecting to %s", address);
  return true_v;
}

void vkr_editor_remote_view_stop(VkrEditorRemoteView *view,
                                 const VkrSampleUiFrame *frame) {
  if (!view || view->role == REMOTE_NONE) {
    return;
  }
  VkrSampleViewStreamRequest *request =
      frame ? frame->view_stream_request : NULL;
  if (request && view->role == REMOTE_SERVER) {
    request->set_export = true_v;
    request->next_export = (VkrViewExport){0};
  }
  if (request && view->role == REMOTE_CLIENT) {
    request->set_import = true_v;
    request->import_surface = NULL;
  }
  log_info("Remote view stopped");
  remote_reset(view);
}

/* Server, each build: applies the client's newest pose, follows the path's
   bandwidth and the Scene's size, and hands out a free surface. */
static void remote_server_update(VkrEditorRemoteView *view,
                                 const VkrSampleUiFrame *frame) {
  const uint64_t now = remote_now_us();
  bool8_t open = false_v;
  bool8_t pose_new = false_v;
  Vec3 position = {0};
  float32_t yaw = 0.0f;
  float32_t pitch = 0.0f;
  VkrNetPathStats path = {0};
  const bool8_t rate_due = now - view->rate_checked_us >= REMOTE_RATE_PERIOD_US;
  bool8_t has_path = false_v;
  vkr_net_host_thread_lock(view->thread);
  open = view->viewer_open;
  const bool8_t recording = view->recording;
  if (view->pose_new) {
    pose_new = true_v;
    position = view->pose_position;
    yaw = view->pose_yaw;
    pitch = view->pose_pitch;
    view->shown_pose_time_us = view->pose_time_us;
    view->pose_new = false_v;
  }
  if (open && rate_due) {
    has_path =
        vkr_net_core_path(vkr_net_host_core(view->host), view->viewer, &path);
  }
  const uint64_t unsent = open ? vkr_net_session_unsent_bytes(
                                     view->net, view->viewer,
                                     VKR_NET_SERVICE_VIEW, REMOTE_FRAME_CHANNEL)
                               : 0u;
  const float64_t overshoot = view->overshoot;
  vkr_net_host_thread_unlock(view->thread);
  if (!open && !recording) {
    return;
  }
  if (pose_new && frame->editor_state_request) {
    VkrSampleEditorStateRequest *request = frame->editor_state_request;
    request->move_camera = true_v;
    request->camera_position = position;
    request->camera_yaw = yaw;
    request->camera_pitch = pitch;
  }
  if (rate_due) {
    view->rate_checked_us = now;
    if (has_path && path.bandwidth) {
      const float64_t rate =
          REMOTE_RATE_SHARE * (float64_t)path.bandwidth * 8.0;
      view->bitrate = (uint32_t)vkr_clamp_f64(
          rate, (float64_t)REMOTE_BITRATE_MIN, (float64_t)view->bitrate_max);
    }
    /* An encoder that runs over its rate is asked for less, so the stream
       stays within the share of the path it was given. */
    view->encode_bitrate =
        (uint32_t)Max((float64_t)view->bitrate / overshoot, 1.0);
    vkr_atomic_uint32_store(&view->send_bitrate, view->bitrate,
                            VKR_MEMORY_ORDER_RELAXED);
    vkr_atomic_uint32_store(&view->asked_bitrate, view->encode_bitrate,
                            VKR_MEMORY_ORDER_RELAXED);
    if (view->ring) {
      remote_ring_lock(view->ring);
      if (view->ring->encoder) {
        vkr_video_encoder_set_bitrate(view->ring->encoder,
                                      view->encode_bitrate);
      }
      remote_ring_unlock(view->ring);
    }
  }
  /* The encoder takes even extents. */
  const uint32_t width = frame->scene_output_width & ~1u;
  const uint32_t height = frame->scene_output_height & ~1u;
  if (!frame->view_stream_request || width < REMOTE_MIN_EXTENT ||
      height < REMOTE_MIN_EXTENT) {
    return;
  }
  if (!view->ring || view->ring->width != width ||
      view->ring->height != height) {
    remote_ring_stop(view->ring);
    view->ring = remote_ring_create(view, width, height, frame->view_encoding);
    if (!view->ring) {
      remote_fail(view, "Could not make the video encoder or its surfaces.");
      return;
    }
    vkr_atomic_uint32_store(&view->key_wanted, 1u, VKR_MEMORY_ORDER_RELEASE);
  }
  /* A frame encoded now would wait behind the backlog: skip it, before
     the encoder references it. */
  if (vkr_editor_remote_backlogged(unsent, view->bitrate,
                                   1000000u / REMOTE_FRAME_RATE)) {
    view->frames_skipped += 1u;
    return;
  }
  RemoteRing *ring = view->ring;
  for (uint32_t slot = 0u; slot < REMOTE_SLOTS; ++slot) {
    if (vkr_atomic_uint32_load(&ring->busy[slot], VKR_MEMORY_ORDER_ACQUIRE)) {
      continue;
    }
    vkr_atomic_uint32_store(&ring->busy[slot], 1u, VKR_MEMORY_ORDER_RELAXED);
    ring->handed_us[slot] = now;
    ring->pose_time_us[slot] = view->shown_pose_time_us;
    vkr_atomic_uint32_fetch_add(&ring->references, 1u,
                                VKR_MEMORY_ORDER_ACQ_REL);
    frame->view_stream_request->set_export = true_v;
    if (ring->encoded) {
      /* The request carries the key frame; its unwritten report asks
         again. */
      ring->key_requested[slot] =
          vkr_atomic_uint32_exchange(&view->key_wanted, 0u,
                                     VKR_MEMORY_ORDER_ACQ_REL) != 0u;
      frame->view_stream_request->next_export = (VkrViewExport){
          .width = width,
          .height = height,
          .bitrate_bps = view->encode_bitrate,
          .frame_rate = REMOTE_FRAME_RATE,
          .key_frame = ring->key_requested[slot],
          .on_packet = remote_packet,
          .tag = slot,
          .done = remote_export_done,
          .context = ring,
      };
      return;
    }
    frame->view_stream_request->next_export = (VkrViewExport){
        .surface = ring->surfaces[slot],
        .tag = slot,
        .done = remote_export_done,
        .context = ring,
    };
    return;
  }
}

/* Client, each build: sends the Scene camera's pose and shows the newest
   decoded frame. */
static void remote_client_update(VkrEditorRemoteView *view,
                                 const VkrSampleUiFrame *frame) {
  if (!frame->view_stream_request) {
    return;
  }
  const uint64_t now = remote_now_us();
  vkr_net_host_thread_lock(view->thread);
  const bool8_t open = view->open;
  const bool8_t closed = view->closed;
  const uint16_t code = view->close_code;
  if (open && frame->scene_recall.camera_valid) {
    uint8_t bytes[32];
    VkrBitWriter writer;
    vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
    vkr_bit_write(&writer, REMOTE_MSG_POSE, 4u);
    remote_write_f32(&writer, frame->scene_recall.position.x);
    remote_write_f32(&writer, frame->scene_recall.position.y);
    remote_write_f32(&writer, frame->scene_recall.position.z);
    remote_write_f32(&writer, frame->scene_recall.yaw);
    remote_write_f32(&writer, frame->scene_recall.pitch);
    vkr_bit_write(&writer, now, 64u);
    const uint32_t size = vkr_bit_writer_finish(&writer);
    if (size) {
      remote_send_small(view, REMOTE_POSE_CHANNEL, bytes, size);
    }
  }
  vkr_net_host_thread_unlock(view->thread);
  if (closed) {
    char message[96];
    string_format(message, sizeof(message),
                  code == VKR_NET_CLOSE_ACCESS_DENIED
                      ? "The server does not allow this editor's key."
                      : "The connection to the server closed (code %u).",
                  code);
    remote_fail(view, message);
    vkr_editor_remote_view_stop(view, frame);
    return;
  }
  VkrVideoSurface *surface = NULL;
  RemoteFrameInfo info = {0};
  vkr_mutex_lock(view->mutex);
  /* A frame held for the reorder window is looked at again each build. */
  if (view->order.count) {
    vkr_cond_signal(view->cond);
  }
  if (view->latest && view->latest_serial != view->shown_serial) {
    surface = view->latest;
    vkr_video_surface_retain(surface);
    info = view->latest_info;
    view->shown_serial = view->latest_serial;
  }
  vkr_mutex_unlock(view->mutex);
  if (!surface) {
    return;
  }
  vkr_video_surface_release(view->shown);
  view->shown = surface;
  view->shown_info = info;
  view->frames_shown += 1u;
  frame->view_stream_request->set_import = true_v;
  frame->view_stream_request->import_surface = surface;
  /* The whole path on this clock; the client's part from arrival on. */
  if (info.pose_time_us && info.pose_time_us <= now) {
    remote_sample(&view->latency_ms,
                  (float32_t)(now - info.pose_time_us) / 1000.0f);
  }
  remote_sample(&view->client_ms,
                (float32_t)(now - Min(now, info.received_us)) / 1000.0f);
  remote_sample(
      &view->decode_ms,
      (float32_t)(info.decoded_us - Min(info.decoded_us, info.received_us)) /
          1000.0f);
}

void vkr_editor_remote_view_update(VkrEditorRemoteView *view,
                                   const VkrSampleUiFrame *frame) {
  if (!view || view->role == REMOTE_NONE || !view->thread) {
    return;
  }
  if (vkr_net_host_thread_failed(view->thread)) {
    remote_fail(view, "The network socket failed.");
    vkr_editor_remote_view_stop(view, frame);
    return;
  }
  if (view->role == REMOTE_SERVER) {
    remote_server_update(view, frame);
  } else {
    remote_client_update(view, frame);
  }
}

bool8_t vkr_editor_remote_view_showing(const VkrEditorRemoteView *view) {
  return view && view->role == REMOTE_CLIENT && view->shown;
}

void vkr_editor_remote_view_build(VkrEditorRemoteView *view,
                                  const VkrSampleUiFrame *frame) {
  if (!vkr_editor_remote_view_showing(view) || !frame->mapping_valid) {
    return;
  }
  VkrUiSystem *ui = frame->ui;
  const Vec4 image = frame->mapping.image_rect_px;
  const float32_t scale = ui->content_scale;
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt =
      (VkrUiEdges){image.y / scale, 0, 0, image.x / scale};
  panel.style.min_size_pt = panel.style.max_size_pt =
      (Vec2){image.z / scale, image.w / scale};
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.remote_view"), &panel)) {
    return;
  }
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement.justify = VKR_UI_ALIGN_STRETCH;
  config.placement.align = VKR_UI_ALIGN_STRETCH;
  config.style.padding_pt = (VkrUiEdges){0};
  VkrUiTextureRef texture = VKR_UI_TEXTURE_REF_REMOTE_VIEW;
  /* The image behind the reference changes every frame. */
  texture.generation = (uint32_t)ui->frame_index;
  vkr_ui_image(ui, string8_lit("image"), texture,
               (Vec2){(float32_t)view->shown_info.width,
                      (float32_t)view->shown_info.height},
               &config);
  (void)vkr_ui_panel_end(ui);
}

/* The path to the other editor: round trip, bandwidth estimate, packets
   lost, and frames the sender dropped at their deadline before they left
   (a backlog) or while waiting to resend a lost packet (loss). The caller
   holds the host lock. */
static void remote_status_path(VkrEditorRemoteView *view,
                               VkrNetConnectionId connection, Arena *arena,
                               VkrBakeryJson *result) {
  VkrNetPathStats path = {0};
  if (!vkr_net_core_path(vkr_net_host_core(view->host), connection, &path)) {
    return;
  }
  VkrBakeryJson *json = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(
      arena, json, "srtt_ms",
      vkr_bakery_json_float(arena, (float64_t)path.srtt_us / 1000.0));
  vkr_bakery_json_set(
      arena, json, "min_rtt_ms",
      vkr_bakery_json_float(arena, (float64_t)path.min_rtt_us / 1000.0));
  vkr_bakery_json_set(arena, json, "bandwidth_bps",
                      vkr_bakery_json_int(arena, (int64_t)path.bandwidth * 8));
  vkr_bakery_json_set(arena, json, "packets_sent",
                      vkr_bakery_json_int(arena, (int64_t)path.packets_sent));
  vkr_bakery_json_set(arena, json, "packets_lost",
                      vkr_bakery_json_int(arena, (int64_t)path.packets_lost));
  vkr_bakery_json_set(arena, json, "expired_unsent",
                      vkr_bakery_json_int(arena, (int64_t)path.expired_unsent));
  vkr_bakery_json_set(arena, json, "expired_resend",
                      vkr_bakery_json_int(arena, (int64_t)path.expired_resend));
  vkr_bakery_json_set(arena, result, "link", json);
}

VkrBakeryJson *vkr_editor_remote_view_status_json(VkrEditorRemoteView *view,
                                                  Arena *arena) {
  static const char *const roles[] = {"none", "server", "client"};
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "role",
                      vkr_bakery_json_cstr(arena, roles[view->role]));
  vkr_bakery_json_set(arena, result, "address",
                      vkr_bakery_json_cstr(arena, view->address));
  vkr_bakery_json_set(arena, result, "key",
                      vkr_bakery_json_cstr(arena, view->key));
  vkr_bakery_json_set(arena, result, "error",
                      vkr_bakery_json_cstr(arena, view->error));
  if (view->role == REMOTE_NONE) {
    return result;
  }
  float32_t p50 = 0.0f;
  float32_t p95 = 0.0f;
  if (view->role == REMOTE_SERVER) {
    vkr_net_host_thread_lock(view->thread);
    vkr_bakery_json_set(arena, result, "client",
                        vkr_bakery_json_bool(arena, view->viewer_open));
    vkr_bakery_json_set(arena, result, "frames_sent",
                        vkr_bakery_json_int(arena, (int64_t)view->frames_sent));
    vkr_bakery_json_set(arena, result, "bytes_sent",
                        vkr_bakery_json_int(arena, (int64_t)view->bytes_sent));
    vkr_bakery_json_set(arena, result, "key_frames",
                        vkr_bakery_json_int(arena, (int64_t)view->key_frames));
    vkr_bakery_json_set(
        arena, result, "send_failures",
        vkr_bakery_json_int(arena, (int64_t)view->send_failures));
    vkr_bakery_json_set(arena, result, "refreshes",
                        vkr_bakery_json_int(arena, (int64_t)view->refreshes));
    vkr_bakery_json_set(arena, result, "overshoot",
                        vkr_bakery_json_float(arena, view->overshoot));
    if (view->viewer_open) {
      remote_status_path(view, view->viewer, arena, result);
    }
    remote_percentiles(&view->render_ms, &p50, &p95);
    vkr_bakery_json_set(arena, result, "render_ms_p50",
                        vkr_bakery_json_float(arena, p50));
    vkr_bakery_json_set(arena, result, "render_ms_p95",
                        vkr_bakery_json_float(arena, p95));
    remote_percentiles(&view->encode_ms, &p50, &p95);
    vkr_bakery_json_set(arena, result, "encode_ms_p50",
                        vkr_bakery_json_float(arena, p50));
    vkr_bakery_json_set(arena, result, "encode_ms_p95",
                        vkr_bakery_json_float(arena, p95));
    if (view->recording) {
      vkr_bakery_json_set(arena, result, "record",
                          vkr_bakery_json_cstr(arena, view->record_path));
      vkr_bakery_json_set(
          arena, result, "frames_recorded",
          vkr_bakery_json_int(arena, (int64_t)view->frames_recorded));
      vkr_bakery_json_set(
          arena, result, "bytes_recorded",
          vkr_bakery_json_int(arena, (int64_t)view->bytes_recorded));
    }
    vkr_net_host_thread_unlock(view->thread);
    vkr_bakery_json_set(arena, result, "bitrate",
                        vkr_bakery_json_int(arena, view->bitrate));
    vkr_bakery_json_set(arena, result, "encode_bitrate",
                        vkr_bakery_json_int(arena, view->encode_bitrate));
    vkr_bakery_json_set(
        arena, result, "frames_skipped",
        vkr_bakery_json_int(arena, (int64_t)view->frames_skipped));
    vkr_bakery_json_set(
        arena, result, "width",
        vkr_bakery_json_int(arena, view->ring ? view->ring->width : 0));
    vkr_bakery_json_set(
        arena, result, "height",
        vkr_bakery_json_int(arena, view->ring ? view->ring->height : 0));
    return result;
  }
  vkr_bakery_json_set(arena, result, "identity",
                      vkr_bakery_json_cstr(arena, view->identity));
  vkr_net_host_thread_lock(view->thread);
  vkr_bakery_json_set(arena, result, "open",
                      vkr_bakery_json_bool(arena, view->open));
  if (view->open) {
    remote_status_path(view, view->connection, arena, result);
  }
  vkr_bakery_json_set(
      arena, result, "frames_received",
      vkr_bakery_json_int(arena, (int64_t)view->frames_received));
  vkr_bakery_json_set(
      arena, result, "bytes_received",
      vkr_bakery_json_int(arena, (int64_t)view->bytes_received));
  vkr_bakery_json_set(
      arena, result, "refreshes_sent",
      vkr_bakery_json_int(arena, (int64_t)view->refreshes_sent));
  vkr_net_host_thread_unlock(view->thread);
  vkr_mutex_lock(view->mutex);
  vkr_bakery_json_set(
      arena, result, "frames_decoded",
      vkr_bakery_json_int(arena, (int64_t)view->frames_decoded));
  const VkrEditorRemoteOrder *order = &view->order;
  vkr_bakery_json_set(
      arena, result, "frames_dropped",
      vkr_bakery_json_int(arena, (int64_t)order->frames_dropped));
  vkr_bakery_json_set(arena, result, "frames_late",
                      vkr_bakery_json_int(arena, (int64_t)order->frames_late));
  vkr_bakery_json_set(
      arena, result, "frames_reordered",
      vkr_bakery_json_int(arena, (int64_t)order->frames_reordered));
  vkr_bakery_json_set(arena, result, "frame_gaps",
                      vkr_bakery_json_int(arena, (int64_t)order->frame_gaps));
  vkr_bakery_json_set(
      arena, result, "decode_failures",
      vkr_bakery_json_int(arena, (int64_t)view->decode_failures));
  vkr_mutex_unlock(view->mutex);
  vkr_bakery_json_set(arena, result, "frames_shown",
                      vkr_bakery_json_int(arena, (int64_t)view->frames_shown));
  vkr_bakery_json_set(arena, result, "width",
                      vkr_bakery_json_int(arena, view->shown_info.width));
  vkr_bakery_json_set(arena, result, "height",
                      vkr_bakery_json_int(arena, view->shown_info.height));
  vkr_bakery_json_set(
      arena, result, "frame",
      vkr_bakery_json_int(arena, (int64_t)view->shown_info.frame_number));
  vkr_bakery_json_set(arena, result, "samples",
                      vkr_bakery_json_int(arena, view->latency_ms.count));
  remote_percentiles(&view->latency_ms, &p50, &p95);
  vkr_bakery_json_set(arena, result, "latency_ms_p50",
                      vkr_bakery_json_float(arena, p50));
  vkr_bakery_json_set(arena, result, "latency_ms_p95",
                      vkr_bakery_json_float(arena, p95));
  remote_percentiles(&view->client_ms, &p50, &p95);
  vkr_bakery_json_set(arena, result, "client_ms_p50",
                      vkr_bakery_json_float(arena, p50));
  vkr_bakery_json_set(arena, result, "client_ms_p95",
                      vkr_bakery_json_float(arena, p95));
  remote_percentiles(&view->decode_ms, &p50, &p95);
  vkr_bakery_json_set(arena, result, "decode_ms_p50",
                      vkr_bakery_json_float(arena, p50));
  vkr_bakery_json_set(arena, result, "decode_ms_p95",
                      vkr_bakery_json_float(arena, p95));
  vkr_bakery_json_set(
      arena, result, "server_render_ms",
      vkr_bakery_json_float(arena,
                            (float64_t)view->shown_info.render_us / 1000.0));
  vkr_bakery_json_set(
      arena, result, "server_encode_ms",
      vkr_bakery_json_float(arena,
                            (float64_t)view->shown_info.encode_us / 1000.0));
  return result;
}
