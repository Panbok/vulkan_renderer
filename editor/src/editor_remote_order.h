#pragma once

#include "defines.h"

/* The view stream's timing rules (ADR-131): the order in which a client
 * decodes frames, and the sender's frame deadline, frame skipping and
 * encoder overshoot. They read no clock and do no I/O, so vkr_net_bench's
 * simulated stream runs the editor's rules.
 *
 * Client order: H.264 frames decode only in stream order, each after the
 * one before it, from a key frame on. The view service delivers frames on
 * a deadline in any order: a frame can arrive before the one before it,
 * whose lost packet is being resent, or never, when its deadline passed.
 * The order holds an early frame for a reorder window, hands frames out in
 * stream order, drops those that cannot decode, and wants a key frame
 * after a gap the window did not close, again every `refresh_us` until one
 * arrives. One thread at a time; the caller locks. */

/* Frames held at once: one second at 60 Hz, which covers the decoder's
   start on the first key frame. A full order means the decoder fell far
   behind: the stream starts again at the next key frame. */
#define VKR_EDITOR_REMOTE_ORDER_CAPACITY 64u

typedef struct VkrEditorRemoteOrderFrame {
  uint64_t number;
  bool8_t key_frame;
  /* When it arrived, on the caller's clock. */
  uint64_t received_us;
  /* The caller's frame, handed back as it was given. */
  void *payload;
} VkrEditorRemoteOrderFrame;

typedef enum VkrEditorRemoteOrderStep {
  /* Nothing to hand out until a frame arrives or the window passes. */
  VKR_EDITOR_REMOTE_ORDER_WAIT = 0,
  /* The next frame of the stream: decode it. */
  VKR_EDITOR_REMOTE_ORDER_DECODE,
  /* A frame that can never decode: free it. */
  VKR_EDITOR_REMOTE_ORDER_DROP,
} VkrEditorRemoteOrderStep;

typedef struct VkrEditorRemoteOrder {
  /* Held frames by number, the oldest first. */
  VkrEditorRemoteOrderFrame held[VKR_EDITOR_REMOTE_ORDER_CAPACITY];
  uint32_t count;
  /* The number of the frame that decodes next, unless waiting for a key
     frame. */
  uint64_t expected;
  bool8_t waiting_key;
  /* When the last key frame was asked for, and whether one is wanted now. */
  uint64_t asked_us;
  bool8_t ask_now;
  uint64_t window_us;
  uint64_t refresh_us;
  /* Frames older than the stream or held twice; frames that arrived after
     a later one; gaps the window did not close, or a key frame skipped;
     frames dropped while the stream waited for a key frame or the order was
     full. */
  uint64_t frames_late;
  uint64_t frames_reordered;
  uint64_t frame_gaps;
  uint64_t frames_dropped;
} VkrEditorRemoteOrder;

/* An order that waits for its first key frame, as asked at `now_us` (the
   server sends one when the stream opens). */
void vkr_editor_remote_order_init(VkrEditorRemoteOrder *order, uint64_t now_us,
                                  uint64_t window_us, uint64_t refresh_us);
/* Holds `frame`. False when it was refused: older than the stream, held
   already, no room, or not a key frame while the stream waits for one; the
   caller keeps its payload. */
bool8_t vkr_editor_remote_order_push(VkrEditorRemoteOrder *order,
                                     const VkrEditorRemoteOrderFrame *frame);
/* What to do next at `now_us`: DECODE or DROP hand out `*out_frame`, which
   the order no longer holds; call again until it answers WAIT. */
VkrEditorRemoteOrderStep
vkr_editor_remote_order_next(VkrEditorRemoteOrder *order, uint64_t now_us,
                             VkrEditorRemoteOrderFrame *out_frame);
/* The decoder refused the frame `next` handed out: the stream waits for a
   key frame, asked for at once. */
void vkr_editor_remote_order_decode_failed(VkrEditorRemoteOrder *order);
/* Whether to ask the server for a key frame at `now_us`; true marks it
   asked. */
bool8_t vkr_editor_remote_order_want_key(VkrEditorRemoteOrder *order,
                                         uint64_t now_us);

/* The sender's deadline, from now, for a frame of `length` bytes with
   `queued` bytes not yet acknowledged ahead of it on its channel: twice the
   time those bytes take at `bitrate_bps`, from `floor_us` to `cap_us`.
   Counting the bytes ahead keeps the frames behind a key frame from
   expiring, each gap asking for another key frame. */
uint64_t vkr_editor_remote_frame_deadline_us(uint64_t length, uint64_t queued,
                                             uint32_t bitrate_bps,
                                             uint64_t floor_us,
                                             uint64_t cap_us);
/* Whether the sender skips the next frame before it is encoded: the bytes
   that have not left its channel yet would take longer than `interval_us`
   at `bitrate_bps`. A skipped frame costs one frame of motion; a frame
   that waits behind a backlog costs latency, or a gap when it expires. */
bool8_t vkr_editor_remote_backlogged(uint64_t unsent, uint32_t bitrate_bps,
                                     uint64_t interval_us);
/* The encoder's overshoot after a frame of `bytes`, encoded at `asked_bps`
   and `frame_rate`: a moving average, over about fifty frames, of each
   frame's size against its share of the asked rate, from 1 to 4. The
   sender asks for its target divided by it. Key frames count, so the
   frames after one pay for it. */
float64_t vkr_editor_remote_overshoot(float64_t overshoot, uint64_t bytes,
                                      uint32_t asked_bps, uint32_t frame_rate);
