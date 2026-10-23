#include "editor_remote_order.h"

void vkr_editor_remote_order_init(VkrEditorRemoteOrder *order, uint64_t now_us,
                                  uint64_t window_us, uint64_t refresh_us) {
  *order = (VkrEditorRemoteOrder){
      .waiting_key = true_v,
      .asked_us = now_us,
      .window_us = window_us,
      .refresh_us = refresh_us,
  };
}

bool8_t vkr_editor_remote_order_push(VkrEditorRemoteOrder *order,
                                     const VkrEditorRemoteOrderFrame *frame) {
  if (frame->number < order->expected) {
    order->frames_late += 1u;
    return false_v;
  }
  /* It could not decode, and it would take the room the key frame needs. */
  if (order->waiting_key && !frame->key_frame) {
    order->frames_dropped += 1u;
    return false_v;
  }
  uint32_t at = order->count;
  while (at > 0u && order->held[at - 1u].number > frame->number) {
    at -= 1u;
  }
  if (at > 0u && order->held[at - 1u].number == frame->number) {
    order->frames_late += 1u;
    return false_v;
  }
  /* The decoder fell far behind: the stream starts again at a key frame. */
  if (order->count == VKR_EDITOR_REMOTE_ORDER_CAPACITY) {
    order->ask_now = order->ask_now || !order->waiting_key;
    order->waiting_key = true_v;
    order->frames_dropped += 1u;
    return false_v;
  }
  order->frames_reordered += at < order->count ? 1u : 0u;
  MemCopy(&order->held[at + 1u], &order->held[at],
          (uint64_t)(order->count - at) * sizeof(order->held[0]));
  order->held[at] = *frame;
  order->count += 1u;
  return true_v;
}

/* Hands out the oldest held frame. */
static VkrEditorRemoteOrderFrame remote_order_pop(VkrEditorRemoteOrder *order) {
  const VkrEditorRemoteOrderFrame first = order->held[0];
  order->count -= 1u;
  MemCopy(&order->held[0], &order->held[1],
          (uint64_t)order->count * sizeof(order->held[0]));
  return first;
}

VkrEditorRemoteOrderStep
vkr_editor_remote_order_next(VkrEditorRemoteOrder *order, uint64_t now_us,
                             VkrEditorRemoteOrderFrame *out_frame) {
  if (order->count == 0u) {
    return VKR_EDITOR_REMOTE_ORDER_WAIT;
  }
  const VkrEditorRemoteOrderFrame *first = &order->held[0];
  /* A key frame ahead of it moved the stream past this one. */
  if (first->number < order->expected) {
    order->frames_late += 1u;
    *out_frame = remote_order_pop(order);
    return VKR_EDITOR_REMOTE_ORDER_DROP;
  }
  /* A key frame decodes on its own, so the stream starts again at it. */
  if (first->key_frame) {
    order->frame_gaps +=
        !order->waiting_key && first->number > order->expected ? 1u : 0u;
    *out_frame = remote_order_pop(order);
    order->expected = out_frame->number + 1u;
    order->waiting_key = false_v;
    order->ask_now = false_v;
    return VKR_EDITOR_REMOTE_ORDER_DECODE;
  }
  if (order->waiting_key) {
    order->frames_dropped += 1u;
    *out_frame = remote_order_pop(order);
    return VKR_EDITOR_REMOTE_ORDER_DROP;
  }
  if (first->number == order->expected) {
    *out_frame = remote_order_pop(order);
    order->expected += 1u;
    return VKR_EDITOR_REMOTE_ORDER_DECODE;
  }
  /* A gap: the missing frame may still arrive within the window. */
  if (now_us < first->received_us + order->window_us) {
    return VKR_EDITOR_REMOTE_ORDER_WAIT;
  }
  order->frame_gaps += 1u;
  order->waiting_key = true_v;
  order->ask_now = true_v;
  order->frames_dropped += 1u;
  *out_frame = remote_order_pop(order);
  return VKR_EDITOR_REMOTE_ORDER_DROP;
}

void vkr_editor_remote_order_decode_failed(VkrEditorRemoteOrder *order) {
  order->waiting_key = true_v;
  order->ask_now = true_v;
}

bool8_t vkr_editor_remote_order_want_key(VkrEditorRemoteOrder *order,
                                         uint64_t now_us) {
  const bool8_t again =
      order->waiting_key && now_us >= order->asked_us + order->refresh_us;
  if (!order->ask_now && !again) {
    return false_v;
  }
  order->ask_now = false_v;
  order->asked_us = now_us;
  return true_v;
}

uint64_t vkr_editor_remote_frame_deadline_us(uint64_t length, uint64_t queued,
                                             uint32_t bitrate_bps,
                                             uint64_t floor_us,
                                             uint64_t cap_us) {
  const uint64_t transfer_us =
      bitrate_bps ? (length + queued) * 8u * 1000000u / bitrate_bps : 0u;
  return Min(Max(floor_us, 2u * transfer_us), cap_us);
}

bool8_t vkr_editor_remote_backlogged(uint64_t unsent, uint32_t bitrate_bps,
                                     uint64_t interval_us) {
  return bitrate_bps && unsent * 8u * 1000000u / bitrate_bps > interval_us;
}

float64_t vkr_editor_remote_overshoot(float64_t overshoot, uint64_t bytes,
                                      uint32_t asked_bps, uint32_t frame_rate) {
  if (!asked_bps || !frame_rate) {
    return overshoot;
  }
  const float64_t share = (float64_t)asked_bps / 8.0 / (float64_t)frame_rate;
  const float64_t made = (float64_t)bytes / share;
  return Min(Max(overshoot * 0.98 + made * 0.02, 1.0), 4.0);
}
