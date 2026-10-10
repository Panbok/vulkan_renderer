#include "vkr_net_cc.h"

#define CC_STARTUP_GAIN 2.885f
#define CC_DRAIN_GAIN (1.0f / 2.885f)
#define CC_PROBE_RTT_INTERVAL_US 10000000ull
#define CC_PROBE_RTT_DURATION_US 200000ull
#define CC_INITIAL_WINDOW_PACKETS 10u
#define CC_MIN_WINDOW_PACKETS 4u
/* The RTT assumed until the first sample. */
#define CC_INITIAL_RTT_US 100000ull
#define CC_BACKGROUND_TARGET_US 25000ull

static const float32_t cc_probe_bw_gains[8] = {1.25f, 0.75f, 1.0f, 1.0f,
                                               1.0f,  1.0f,  1.0f, 1.0f};

void vkr_net_cc_init(VkrNetCc *cc, uint32_t mss, bool8_t background,
                     uint64_t now_us) {
  *cc = (VkrNetCc){
      .mss = mss,
      .background = background,
      .mode = VKR_NET_CC_STARTUP,
      .pacing_gain = CC_STARTUP_GAIN,
      .cwnd_gain = CC_STARTUP_GAIN,
      .min_rtt_us = UINT64_MAX,
      .min_rtt_stamp = now_us,
      .cwnd = (uint64_t)mss * CC_INITIAL_WINDOW_PACKETS,
      .inflight_hi = UINT64_MAX,
  };
  cc->pacing_rate = (uint64_t)((float64_t)cc->cwnd * CC_STARTUP_GAIN *
                               1000000.0 / (float64_t)CC_INITIAL_RTT_US);
}

void vkr_net_cc_set_mss(VkrNetCc *cc, uint32_t mss) {
  cc->mss = mss;
  cc->cwnd = Max(cc->cwnd, (uint64_t)mss * CC_MIN_WINDOW_PACKETS);
}

uint64_t vkr_net_cc_bdp(const VkrNetCc *cc) {
  if (cc->bandwidth == 0u || cc->min_rtt_us == UINT64_MAX) {
    return 0u;
  }
  return (uint64_t)((float64_t)cc->bandwidth * (float64_t)cc->min_rtt_us /
                    1000000.0);
}

static uint64_t cc_min_window(const VkrNetCc *cc) {
  return (uint64_t)cc->mss * CC_MIN_WINDOW_PACKETS;
}

static void cc_update_bandwidth(VkrNetCc *cc, const VkrNetCcSample *sample,
                                bool8_t round_start) {
  const uint32_t slot = (uint32_t)(cc->round_count % VKR_NET_CC_BW_ROUNDS);
  if (round_start) {
    cc->bw_rounds[slot] = 0u;
  }
  /* An app-limited sample only counts when it raises the estimate. */
  if (sample->rate > 0u &&
      (!sample->rate_app_limited || sample->rate >= cc->bandwidth)) {
    cc->bw_rounds[slot] = Max(cc->bw_rounds[slot], sample->rate);
  }
  uint64_t best = 0u;
  for (uint32_t i = 0u; i < VKR_NET_CC_BW_ROUNDS; ++i) {
    best = Max(best, cc->bw_rounds[i]);
  }
  cc->bandwidth = best;
}

static void cc_enter_probe_bw(VkrNetCc *cc, uint64_t now_us) {
  cc->mode = VKR_NET_CC_PROBE_BW;
  cc->pacing_gain = cc_probe_bw_gains[0];
  cc->cwnd_gain = 2.0f;
  cc->cycle_index = 0u;
  cc->cycle_stamp = now_us;
}

static void cc_enter_drain(VkrNetCc *cc) {
  cc->filled = true_v;
  cc->mode = VKR_NET_CC_DRAIN;
  cc->pacing_gain = CC_DRAIN_GAIN;
  cc->cwnd_gain = CC_STARTUP_GAIN;
}

/* At a round's end: the startup plateau and the loss cap. */
static void cc_on_round_end(VkrNetCc *cc, const VkrNetCcSample *sample) {
  const bool8_t lossy = cc->round_lost > 0u &&
                        cc->round_lost * 50u > cc->round_delivered &&
                        cc->round_lost >= (uint64_t)cc->mss * 3u;
  if (lossy) {
    const uint64_t in_flight = Max(sample->bytes_in_flight, cc->cwnd / 2u);
    cc->inflight_hi = Max(cc_min_window(cc), in_flight * 85u / 100u);
    cc->clean_rounds = 0u;
    if (cc->mode == VKR_NET_CC_STARTUP) {
      cc_enter_drain(cc);
    }
  } else if (cc->inflight_hi != UINT64_MAX) {
    /* Clean rounds let the cap grow back, doubling from the third one. */
    cc->clean_rounds += 1u;
    if (cc->clean_rounds >= 3u) {
      const uint64_t bdp = vkr_net_cc_bdp(cc);
      cc->inflight_hi *= 2u;
      if (bdp > 0u && cc->inflight_hi > bdp * 4u) {
        cc->inflight_hi = UINT64_MAX;
      }
    }
  }
  cc->round_lost = 0u;
  cc->round_delivered = 0u;

  if (cc->mode == VKR_NET_CC_STARTUP && !cc->filled) {
    if (cc->bandwidth >= cc->full_bw + cc->full_bw / 4u) {
      cc->full_bw = cc->bandwidth;
      cc->full_bw_count = 0u;
    } else if (cc->bandwidth > 0u) {
      cc->full_bw_count += 1u;
      if (cc->full_bw_count >= 3u) {
        cc_enter_drain(cc);
      }
    }
  }
}

static void cc_update_mode(VkrNetCc *cc, const VkrNetCcSample *sample,
                           bool8_t min_rtt_expired) {
  const uint64_t now = sample->now_us;
  const uint64_t bdp = vkr_net_cc_bdp(cc);

  if (cc->mode == VKR_NET_CC_DRAIN && bdp > 0u &&
      sample->bytes_in_flight <= bdp) {
    cc_enter_probe_bw(cc, now);
  }

  if (cc->mode == VKR_NET_CC_PROBE_BW && cc->min_rtt_us != UINT64_MAX) {
    const bool8_t phase_over = now - cc->cycle_stamp > cc->min_rtt_us;
    /* The 0.75 phase ends early once the queue it drains is empty. */
    const bool8_t drained =
        cc->pacing_gain < 1.0f && bdp > 0u && sample->bytes_in_flight <= bdp;
    if (phase_over || drained) {
      cc->cycle_index = (uint8_t)((cc->cycle_index + 1u) % 8u);
      cc->pacing_gain = cc_probe_bw_gains[cc->cycle_index];
      cc->cycle_stamp = now;
    }
  }

  /* An old minimum RTT may hide a route change; measure it again. */
  if (cc->mode != VKR_NET_CC_PROBE_RTT && min_rtt_expired) {
    cc->mode = VKR_NET_CC_PROBE_RTT;
    cc->pacing_gain = 1.0f;
    cc->prior_cwnd = cc->cwnd;
    cc->probe_rtt_done_stamp = 0u;
    cc->probe_rtt_round_done = false_v;
  }

  if (cc->mode == VKR_NET_CC_PROBE_RTT) {
    if (cc->probe_rtt_done_stamp == 0u &&
        sample->bytes_in_flight <= cc_min_window(cc)) {
      cc->probe_rtt_done_stamp = now + CC_PROBE_RTT_DURATION_US;
      cc->next_round_delivered = sample->delivered;
    } else if (cc->probe_rtt_done_stamp != 0u &&
               now >= cc->probe_rtt_done_stamp && cc->probe_rtt_round_done) {
      cc->min_rtt_stamp = now;
      cc->cwnd = Max(cc->cwnd, cc->prior_cwnd);
      if (cc->filled) {
        cc_enter_probe_bw(cc, now);
      } else {
        cc->mode = VKR_NET_CC_STARTUP;
        cc->pacing_gain = CC_STARTUP_GAIN;
        cc->cwnd_gain = CC_STARTUP_GAIN;
      }
    }
  }
}

static void cc_update_window_and_rate(VkrNetCc *cc,
                                      const VkrNetCcSample *sample) {
  const uint64_t bdp = vkr_net_cc_bdp(cc);
  const uint64_t floor = cc_min_window(cc);

  if (cc->mode == VKR_NET_CC_PROBE_RTT) {
    cc->cwnd = floor;
  } else {
    uint64_t cwnd = cc->cwnd;
    if (bdp == 0u) {
      cwnd += sample->newly_acked;
    } else {
      const uint64_t target =
          (uint64_t)((float64_t)bdp * cc->cwnd_gain) + 3u * cc->mss;
      if (cc->filled) {
        cwnd = Min(cwnd + sample->newly_acked, target);
      } else if (cwnd < target) {
        /* Startup grows the window only up to its target. */
        cwnd = Min(cwnd + sample->newly_acked, target);
      }
    }
    cc->cwnd = Max(floor, Min(cwnd, cc->inflight_hi));
  }

  uint64_t rate = 0u;
  if (cc->bandwidth > 0u) {
    rate = (uint64_t)((float64_t)cc->bandwidth * cc->pacing_gain * 0.99);
  } else {
    const uint64_t rtt = sample->srtt_us ? sample->srtt_us : CC_INITIAL_RTT_US;
    rate = (uint64_t)((float64_t)cc->cwnd * CC_STARTUP_GAIN * 1000000.0 /
                      (float64_t)Max(rtt, 1u));
  }
  /* Startup never lowers the rate before the pipe is full. */
  if (!cc->filled && rate < cc->pacing_rate) {
    rate = cc->pacing_rate;
  }
  if (cc->background && cc->min_rtt_us != UINT64_MAX &&
      sample->srtt_us > cc->min_rtt_us) {
    const uint64_t queue = sample->srtt_us - cc->min_rtt_us;
    float64_t scale =
        1.0 - (float64_t)queue / (float64_t)CC_BACKGROUND_TARGET_US;
    scale = Clamp(scale, 0.1, 1.0);
    rate = (uint64_t)((float64_t)rate * scale);
  }
  cc->pacing_rate = Max(rate, (uint64_t)cc->mss);
}

void vkr_net_cc_on_ack(VkrNetCc *cc, const VkrNetCcSample *sample) {
  cc->round_delivered += sample->newly_acked;

  bool8_t round_start = false_v;
  if (sample->packet_delivered >= cc->next_round_delivered) {
    cc->next_round_delivered = sample->delivered;
    cc->round_count += 1u;
    round_start = true_v;
    if (cc->mode == VKR_NET_CC_PROBE_RTT && cc->probe_rtt_done_stamp != 0u) {
      cc->probe_rtt_round_done = true_v;
    }
  }

  cc_update_bandwidth(cc, sample, round_start);

  /* An expired minimum takes the next sample and starts PROBE_RTT. */
  const bool8_t min_rtt_expired =
      cc->min_rtt_us != UINT64_MAX &&
      sample->now_us - cc->min_rtt_stamp > CC_PROBE_RTT_INTERVAL_US;
  if (sample->rtt_us > 0u &&
      (sample->rtt_us <= cc->min_rtt_us || min_rtt_expired)) {
    cc->min_rtt_us = sample->rtt_us;
    cc->min_rtt_stamp = sample->now_us;
  }

  if (round_start) {
    cc_on_round_end(cc, sample);
  }
  cc_update_mode(cc, sample, min_rtt_expired);
  cc_update_window_and_rate(cc, sample);
}

void vkr_net_cc_on_loss(VkrNetCc *cc, uint64_t bytes_lost) {
  cc->round_lost += bytes_lost;
}
