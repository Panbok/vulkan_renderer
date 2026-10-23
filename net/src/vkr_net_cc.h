#pragma once

#include "defines.h"

/* Congestion control of one connection (docs/proposals/network-protocol.md,
 * "Congestion control"): a model-based controller in the manner of BBR. It
 * estimates the bottleneck bandwidth (the largest delivery rate over the
 * last ten rounds) and the minimum RTT (over ten seconds), paces to the
 * bandwidth and caps the bytes in flight near their product.
 *
 * Rounds in which every sample was app-limited and below the estimate
 * do not age it, as in BBR's windowed filter: a video stream sending at a
 * share of the link would otherwise see its estimate decay to its own
 * rate and below.
 *
 * Phases: STARTUP doubles the rate each round until the bandwidth stops
 * growing by a quarter for three rounds or the round loses more than 2%;
 * DRAIN empties the queue STARTUP built; PROBE_BW cycles its pacing gain
 * through 1.25, 0.75 and six rounds of 1.0; PROBE_RTT drops to four packets
 * for 200 ms when the minimum RTT is ten seconds old. A round that loses
 * more than 2% caps the bytes in flight at 85% of what was in flight.
 *
 * Background mode scales the pacing rate down as queueing delay (smoothed
 * RTT above the minimum, less a 5 ms floor for delayed acknowledgments)
 * approaches 25 ms, so a bulk transfer yields to other traffic on the same
 * path. When the other traffic stops, a round without yielding restarts
 * STARTUP if yielding cost the flow over half its bandwidth estimate, so
 * it takes the link back within a few rounds. */

typedef enum VkrNetCcMode {
  VKR_NET_CC_STARTUP = 0,
  VKR_NET_CC_DRAIN,
  VKR_NET_CC_PROBE_BW,
  VKR_NET_CC_PROBE_RTT,
} VkrNetCcMode;

#define VKR_NET_CC_BW_ROUNDS 10u

typedef struct VkrNetCc {
  uint32_t mss;
  bool8_t background;
  uint8_t mode; /**< VkrNetCcMode. */
  bool8_t filled;
  uint8_t cycle_index;
  uint32_t full_bw_count;

  /* Per-round maximum delivery rates, bytes per second, of the last ten
     rounds that had a sample which counts. `bw_slot` is the current
     round's; it moves on only after a round whose sample counted, so a
     sender that stays below the link keeps its estimate. */
  uint64_t bw_rounds[VKR_NET_CC_BW_ROUNDS];
  uint32_t bw_slot;
  bool8_t bw_slot_counted;
  uint64_t bandwidth;
  uint64_t full_bw;

  uint64_t min_rtt_us;
  uint64_t min_rtt_stamp;

  uint64_t round_count;
  uint64_t next_round_delivered;
  uint64_t round_lost;
  uint64_t round_delivered;
  uint32_t clean_rounds;

  float32_t pacing_gain;
  float32_t cwnd_gain;
  uint64_t cycle_stamp;
  uint64_t probe_rtt_done_stamp;
  bool8_t probe_rtt_round_done;

  /* Background mode: a sample of this round scaled the rate to half or
     less; the last finished round did; the bandwidth estimate when that
     run of yielding rounds began. */
  bool8_t round_yielding;
  bool8_t yielded;
  uint64_t yield_bandwidth;

  uint64_t cwnd;
  uint64_t prior_cwnd;
  uint64_t inflight_hi;
  uint64_t pacing_rate;
} VkrNetCc;

/* One acknowledgment's effect. */
typedef struct VkrNetCcSample {
  uint64_t now_us;
  /* Bytes delivered over the connection's life, after this ack. */
  uint64_t delivered;
  /* `delivered` when the newest acknowledged packet was sent. */
  uint64_t packet_delivered;
  uint64_t newly_acked;
  /* Delivery rate in bytes per second, zero when none was measured. */
  uint64_t rate;
  bool8_t rate_app_limited;
  /* RTT sample, zero when none. */
  uint64_t rtt_us;
  uint64_t srtt_us;
  /* The path's lifetime minimum RTT (RFC 9002), zero before a sample. The
     smoothed RTT less this is the queueing delay background mode yields
     to. */
  uint64_t path_min_rtt_us;
  uint64_t bytes_in_flight;
} VkrNetCcSample;

void vkr_net_cc_init(VkrNetCc *cc, uint32_t mss, bool8_t background,
                     uint64_t now_us);

/* The datagram size grew after PMTU discovery. */
void vkr_net_cc_set_mss(VkrNetCc *cc, uint32_t mss);

void vkr_net_cc_on_ack(VkrNetCc *cc, const VkrNetCcSample *sample);

/* Bytes of ack-eliciting packets declared lost. */
void vkr_net_cc_on_loss(VkrNetCc *cc, uint64_t bytes_lost);

/* The bandwidth-delay product in bytes, or zero before a sample. */
uint64_t vkr_net_cc_bdp(const VkrNetCc *cc);
