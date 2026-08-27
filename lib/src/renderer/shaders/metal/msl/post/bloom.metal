// Bloom prefilter, downsample, upsample, and combine entry points. The threshold
// curve,
// the non-finite/firefly sanitizer, and the Karis weight live in
// shared/bloom_kernel.slangh so the Vulkan path uses the same definitions. Tap
// patterns stay here because they are inseparable from the backend's sampling
// call.

struct VkrMetalPacketBloomRoot {
  texture2d<float, access::sample> source;
  // Coarser accumulation level. Written by the encoder for the upsample pass
  // only; the deepest step is pointed at the downsample chain instead, because
  // the accumulation level above it has never been written.
  texture2d<float, access::sample> coarse;
  texture2d<float, access::write> destination;
  // Extent the tap offsets are expressed in: the reduction input for the
  // prefilter and downsample, the coarse level for the upsample. The upsample's
  // fine source shares the destination extent, so it needs none.
  uint2 filter_extent;
  uint2 destination_extent;
  VkrBloomParams params;
  uint2 reserved;
};

constexpr sampler vkr_bloom_sampler(coord::normalized, address::clamp_to_edge,
                                    filter::linear);

static float3 vkr_bloom_tap(constant VkrMetalPacketBloomRoot &root,
                            texture2d<float, access::sample> source,
                            float2 uv) {
  return vkr_bloom_sanitize(root.params,
                            source.sample(vkr_bloom_sampler, uv).rgb);
}

// Thirteen-tap Call of Duty downsample. The five overlapping 2x2 boxes are what
// make the reduction stable under motion; a plain box filter at this ratio
// aliases into the chain and the aliasing is then blurred across the screen.
struct VkrBloomTaps13 {
  float3 a, b, c, d, e, f, g, h, i, j, k, l, m;
};

static VkrBloomTaps13 vkr_bloom_gather13(
    constant VkrMetalPacketBloomRoot &root,
    texture2d<float, access::sample> source, float2 uv, float2 texel) {
  VkrBloomTaps13 taps;
  taps.a = vkr_bloom_tap(root, source, uv + float2(-2.0, -2.0) * texel);
  taps.b = vkr_bloom_tap(root, source, uv + float2(0.0, -2.0) * texel);
  taps.c = vkr_bloom_tap(root, source, uv + float2(2.0, -2.0) * texel);
  taps.d = vkr_bloom_tap(root, source, uv + float2(-2.0, 0.0) * texel);
  taps.e = vkr_bloom_tap(root, source, uv);
  taps.f = vkr_bloom_tap(root, source, uv + float2(2.0, 0.0) * texel);
  taps.g = vkr_bloom_tap(root, source, uv + float2(-2.0, 2.0) * texel);
  taps.h = vkr_bloom_tap(root, source, uv + float2(0.0, 2.0) * texel);
  taps.i = vkr_bloom_tap(root, source, uv + float2(2.0, 2.0) * texel);
  taps.j = vkr_bloom_tap(root, source, uv + float2(-1.0, -1.0) * texel);
  taps.k = vkr_bloom_tap(root, source, uv + float2(1.0, -1.0) * texel);
  taps.l = vkr_bloom_tap(root, source, uv + float2(-1.0, 1.0) * texel);
  taps.m = vkr_bloom_tap(root, source, uv + float2(1.0, 1.0) * texel);
  return taps;
}

static float3 vkr_bloom_combine13(VkrBloomTaps13 taps) {
  return taps.e * 0.125 + (taps.a + taps.c + taps.g + taps.i) * 0.03125 +
         (taps.b + taps.d + taps.f + taps.h) * 0.0625 +
         (taps.j + taps.k + taps.l + taps.m) * 0.125;
}

static float3 vkr_bloom_karis_box(float3 a, float3 b, float3 c, float3 d) {
  float wa = vkr_bloom_karis_weight(a);
  float wb = vkr_bloom_karis_weight(b);
  float wc = vkr_bloom_karis_weight(c);
  float wd = vkr_bloom_karis_weight(d);
  return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

// Firefly-suppressed variant of the same thirteen taps. Each 2x2 box is
// averaged by perceived intensity before the boxes are combined, so a single hot
// texel cannot own the reduction. Only the prefilter needs this: once the chain
// is bounded, later levels have no isolated outlier left to suppress.
static float3 vkr_bloom_combine13_karis(VkrBloomTaps13 taps) {
  return vkr_bloom_karis_box(taps.j, taps.k, taps.l, taps.m) * 0.5 +
         vkr_bloom_karis_box(taps.a, taps.b, taps.d, taps.e) * 0.125 +
         vkr_bloom_karis_box(taps.b, taps.c, taps.e, taps.f) * 0.125 +
         vkr_bloom_karis_box(taps.d, taps.e, taps.g, taps.h) * 0.125 +
         vkr_bloom_karis_box(taps.e, taps.f, taps.h, taps.i) * 0.125;
}

// Four bilinear taps, each covering a 2x2 source footprint, so the filter is a
// 4x4 box for a quarter of the sample cost. Retained as a measured alternative
// to the thirteen-tap form rather than as a fallback.
static float3 vkr_bloom_combine_box4(constant VkrMetalPacketBloomRoot &root,
                                     texture2d<float, access::sample> source,
                                     float2 uv, float2 texel) {
  return (vkr_bloom_tap(root, source, uv + float2(-1.0, -1.0) * texel) +
          vkr_bloom_tap(root, source, uv + float2(1.0, -1.0) * texel) +
          vkr_bloom_tap(root, source, uv + float2(-1.0, 1.0) * texel) +
          vkr_bloom_tap(root, source, uv + float2(1.0, 1.0) * texel)) *
         0.25;
}

// Nine-tap tent. The upsample filter must be wider than the destination texel or
// the chain reconstructs as visible squares at every level boundary.
static float3 vkr_bloom_tent9(constant VkrMetalPacketBloomRoot &root,
                              texture2d<float, access::sample> source, float2 uv,
                              float2 texel) {
  float3 sum = vkr_bloom_tap(root, source, uv + float2(-1.0, -1.0) * texel);
  sum += vkr_bloom_tap(root, source, uv + float2(0.0, -1.0) * texel) * 2.0;
  sum += vkr_bloom_tap(root, source, uv + float2(1.0, -1.0) * texel);
  sum += vkr_bloom_tap(root, source, uv + float2(-1.0, 0.0) * texel) * 2.0;
  sum += vkr_bloom_tap(root, source, uv) * 4.0;
  sum += vkr_bloom_tap(root, source, uv + float2(1.0, 0.0) * texel) * 2.0;
  sum += vkr_bloom_tap(root, source, uv + float2(-1.0, 1.0) * texel);
  sum += vkr_bloom_tap(root, source, uv + float2(0.0, 1.0) * texel) * 2.0;
  sum += vkr_bloom_tap(root, source, uv + float2(1.0, 1.0) * texel);
  return sum * (1.0 / 16.0);
}

static float2 vkr_bloom_destination_uv(constant VkrMetalPacketBloomRoot &root,
                                       uint2 pixel) {
  return (float2(pixel) + 0.5) / float2(root.destination_extent);
}

kernel void vkr_metal_packet_bloom_prefilter(
    constant VkrMetalPacketBloomRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  // Extents are odd as often as not once the chain has halved a few times, so
  // the tail group of every dispatch is partially out of range.
  if (!all(pixel < root.destination_extent))
    return;
  float2 uv = vkr_bloom_destination_uv(root, pixel);
  float2 texel = 1.0 / float2(root.filter_extent);
  VkrBloomTaps13 taps = vkr_bloom_gather13(root, root.source, uv, texel);
  // Threshold after the firefly-weighted reduction. Thresholding each tap first
  // would let the knee reintroduce the outlier the weighting removed.
  float3 reduced = vkr_bloom_combine13_karis(taps);
  root.destination.write(
      float4(vkr_bloom_soft_threshold(root.params, reduced), 1.0), pixel);
}

kernel void vkr_metal_packet_bloom_downsample_tent13(
    constant VkrMetalPacketBloomRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (!all(pixel < root.destination_extent))
    return;
  float2 uv = vkr_bloom_destination_uv(root, pixel);
  float2 texel = 1.0 / float2(root.filter_extent);
  float3 reduced =
      vkr_bloom_combine13(vkr_bloom_gather13(root, root.source, uv, texel));
  root.destination.write(float4(reduced, 1.0), pixel);
}

kernel void vkr_metal_packet_bloom_downsample_box4(
    constant VkrMetalPacketBloomRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (!all(pixel < root.destination_extent))
    return;
  float2 uv = vkr_bloom_destination_uv(root, pixel);
  float2 texel = 1.0 / float2(root.filter_extent);
  root.destination.write(
      float4(vkr_bloom_combine_box4(root, root.source, uv, texel), 1.0), pixel);
}

kernel void vkr_metal_packet_bloom_upsample(
    constant VkrMetalPacketBloomRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (!all(pixel < root.destination_extent))
    return;
  float2 uv = vkr_bloom_destination_uv(root, pixel);
  // The tent radius is expressed in coarse texels, which is what makes the
  // filter wider than the destination grid at every level.
  float2 coarse_texel = 1.0 / float2(root.filter_extent);
  float3 coarse = vkr_bloom_tent9(root, root.coarse, uv, coarse_texel);
  float3 fine = vkr_bloom_tap(root, root.source, uv);
  // Sum rather than mix: each level contributes its own band once, so the
  // resolved chain is the sum of the scales rather than a weighted average that
  // drops energy at every step.
  root.destination.write(float4(fine + coarse, 1.0), pixel);
}

kernel void vkr_metal_packet_bloom_combine(
    constant VkrMetalPacketBloomRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (!all(pixel < root.destination_extent))
    return;
  float2 uv = vkr_bloom_destination_uv(root, pixel);
  float3 hdr = root.source.sample(vkr_bloom_sampler, uv).rgb;
  float3 bloom = root.coarse.sample(vkr_bloom_sampler, uv).rgb;
  root.destination.write(float4(hdr + bloom * root.params.intensity, 1.0),
                         pixel);
}

static_assert(sizeof(VkrBloomParams) == 32,
              "Bloom parameter ABI must remain 32 bytes");
static_assert(sizeof(VkrMetalPacketBloomRoot) == 80,
              "Bloom root ABI must remain 80 bytes");
