// Automatic-exposure metering entry points. Shared arithmetic lives in
// shared/exposure_kernel.slangh so the backend-native entry points use the same
// luminance, binning, percentile, and adaptation definitions.

struct VkrMetalPacketExposureHistogram {
  atomic_uint bins[VKR_EXPOSURE_HISTOGRAM_BINS];
};

struct VkrMetalPacketExposureRoot {
  device VkrMetalPacketExposureHistogram *histogram;
  device VkrExposureState *state;
  device const VkrExposureState *previous_state;
  texture2d<float, access::read> source;
  uint2 extent;
  uint reset_reasons;
  uint reserved;
  VkrExposureMetering metering;
};

// The bounded histogram is cleared inside its own pass so the first frame does
// not depend on device memory arriving zeroed.
kernel void vkr_metal_packet_exposure_clear(
    constant VkrMetalPacketExposureRoot &root [[buffer(0)]],
    uint bin [[thread_position_in_grid]]) {
  atomic_store_explicit(&root.histogram->bins[bin], 0u, memory_order_relaxed);
}

kernel void vkr_metal_packet_exposure_histogram(
    constant VkrMetalPacketExposureRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]],
    uint group_index [[thread_index_in_threadgroup]]) {
  // Declared in kernel scope rather than bound, so the encoder needs no
  // threadgroup-memory length and the 16x16 group maps one thread per bin.
  threadgroup atomic_uint bins[VKR_EXPOSURE_HISTOGRAM_BINS];
  atomic_store_explicit(&bins[group_index], 0u, memory_order_relaxed);
  threadgroup_barrier(mem_flags::mem_threadgroup);

  if (all(pixel < root.extent)) {
    float luminance = vkr_exposure_luminance(root.source.read(pixel).rgb);
    if (vkr_exposure_luminance_accepted(root.metering, luminance)) {
      uint bin = vkr_exposure_bin_index(root.metering, luminance);
      atomic_fetch_add_explicit(&bins[bin], 1u, memory_order_relaxed);
    }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  // One bounded global merge per threadgroup. Whether a SIMD pre-reduction
  // would help depends on measured bin contention for this scene, not on a
  // same-predicate ballot, so it is not assumed here.
  uint local = atomic_load_explicit(&bins[group_index], memory_order_relaxed);
  if (local != 0u)
    atomic_fetch_add_explicit(&root.histogram->bins[group_index], local,
                              memory_order_relaxed);
}

struct VkrMetalPacketExposureReduction {
  float prefix[VKR_EXPOSURE_HISTOGRAM_BINS];
  float weight[VKR_EXPOSURE_HISTOGRAM_BINS];
  float weighted_log[VKR_EXPOSURE_HISTOGRAM_BINS];
  uint low_bin[VKR_EXPOSURE_HISTOGRAM_BINS];
  uint high_bin[VKR_EXPOSURE_HISTOGRAM_BINS];
};

kernel void vkr_metal_packet_exposure_resolve(
    constant VkrMetalPacketExposureRoot &root [[buffer(0)]],
    uint bin [[thread_position_in_grid]]) {
  threadgroup VkrMetalPacketExposureReduction shared_state;
  float count = float(atomic_load_explicit(&root.histogram->bins[bin],
                                           memory_order_relaxed));
  shared_state.prefix[bin] = count;
  threadgroup_barrier(mem_flags::mem_threadgroup);

  // Inclusive Hillis-Steele scan over a fixed 256-wide array. The bin count is
  // a compile-time constant, so the step count is too.
  for (uint offset = 1u; offset < VKR_EXPOSURE_HISTOGRAM_BINS; offset <<= 1u) {
    float addend = bin >= offset ? shared_state.prefix[bin - offset] : 0.0f;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    shared_state.prefix[bin] += addend;
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  float total = shared_state.prefix[VKR_EXPOSURE_HISTOGRAM_BINS - 1u];
  float low_count = root.metering.low_percentile * total;
  float high_count = root.metering.high_percentile * total;
  float inclusive = shared_state.prefix[bin];
  float retained = vkr_exposure_retained(inclusive - count, inclusive,
                                         low_count, high_count);
  shared_state.weight[bin] = retained;
  shared_state.weighted_log[bin] =
      retained * vkr_exposure_bin_log_luminance(root.metering, bin);
  // The clipped percentile range is a debug output, so the edges are reduced
  // alongside the weight rather than re-derived from a capture.
  shared_state.low_bin[bin] =
      retained > 0.0f ? bin : uint(VKR_EXPOSURE_HISTOGRAM_BINS);
  shared_state.high_bin[bin] = retained > 0.0f ? bin : 0u;
  threadgroup_barrier(mem_flags::mem_threadgroup);

  for (uint stride = VKR_EXPOSURE_HISTOGRAM_BINS / 2u; stride > 0u;
       stride >>= 1u) {
    if (bin < stride) {
      shared_state.weight[bin] += shared_state.weight[bin + stride];
      shared_state.weighted_log[bin] +=
          shared_state.weighted_log[bin + stride];
      shared_state.low_bin[bin] =
          min(shared_state.low_bin[bin], shared_state.low_bin[bin + stride]);
      shared_state.high_bin[bin] = max(shared_state.high_bin[bin],
                                        shared_state.high_bin[bin + stride]);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  if (bin != 0u)
    return;

  float weight = shared_state.weight[0];
  float average_log_luminance =
      weight > 0.0f ? shared_state.weighted_log[0] / weight : 0.0f;
  float previous_ev =
      vkr_exposure_previous_ev(root.metering, root.previous_state->adapted_ev);
  float target_ev = vkr_exposure_target_ev(root.metering, weight,
                                           average_log_luminance, previous_ev);
  float adapted_ev = vkr_exposure_adapt(root.metering, target_ev, previous_ev);
  bool retained_any = weight > 0.0f;

  VkrExposureState published;
  published.exposure_multiplier = exp2(adapted_ev);
  published.adapted_ev = adapted_ev;
  published.target_ev = target_ev;
  published.average_log_luminance = average_log_luminance;
  published.retained_low_bin =
      retained_any ? float(shared_state.low_bin[0]) : 0.0f;
  published.retained_high_bin =
      retained_any ? float(shared_state.high_bin[0]) : 0.0f;
  published.accepted_texel_count = uint(total);
  published.reset_reasons = root.reset_reasons;
  *root.state = published;
}

static_assert(sizeof(VkrExposureMetering) == 64,
              "Exposure metering ABI must remain 64 bytes");
static_assert(sizeof(VkrExposureState) == 32,
              "Exposure state ABI must remain 32 bytes");
static_assert(sizeof(VkrMetalPacketExposureRoot) == 112,
              "Exposure root ABI must remain 112 bytes");
