#pragma once

#include "defines.h"

/* The order in which a view streaming client decodes frames (ADR-131).
 *
 * H.264 frames decode only in stream order, each after the one before it,
 * from a key frame on. The view service delivers frames on a deadline in
 * any order: a frame can arrive before the one before it, whose lost packet
 * is being resent, or never, when its deadline passed. The order holds an
 * early frame for a reorder window, hands frames out in stream order, drops
 * those that cannot decode, and wants a key frame after a gap the window
 * did not close, again every `refresh_us` until one arrives.
 *
 * One thread at a time; the caller locks. */

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
