struct alignas(16) VkrMetalPacketMetalfxStabilizeRoot {
  texture2d<float, access::read_write> output;
  texture2d<float, access::sample> history;
  texture2d<float, access::read> validity;
  uint2 output_extent;
  uint2 source_extent;
  float2 jitter_pixels;
  uint history_valid;
  uint scene_stationary;
  uint2 reserved;
};

kernel void vkr_metal_packet_metalfx_stabilize(
    constant VkrMetalPacketMetalfxStabilizeRoot &root [[buffer(0)]],
    uint2 pixel [[thread_position_in_grid]]) {
  if (any(pixel >= root.output_extent))
    return;

  float4 current = root.output.read(pixel);
  // Host equality and SSR settling gate every previous-image read. Age zero
  // prevents moving or unsettled output from entering the next static mean.
  if (root.scene_stationary == 0u || root.history_valid == 0u) {
    root.output.write(float4(current.rgb, 0.0f), pixel);
    return;
  }

  // Map the canonical output center to the jittered, top-left source grid.
  // Reject unreliable samples in the surrounding 2x2 source footprint.
  float2 raw_pixel = (float2(pixel) + 0.5f) * float2(root.source_extent) /
                         float2(root.output_extent) +
                     root.jitter_pixels - 0.5f;
  int2 raw_min = int2(floor(raw_pixel));
  int2 raw_max = raw_min + int2(1, 1);
  int2 limit = int2(root.source_extent - 1u);
  uint2 p00 = uint2(clamp(raw_min, int2(0), limit));
  uint2 p10 = uint2(clamp(int2(raw_max.x, raw_min.y), int2(0), limit));
  uint2 p01 = uint2(clamp(int2(raw_min.x, raw_max.y), int2(0), limit));
  uint2 p11 = uint2(clamp(raw_max, int2(0), limit));
  float4 validity = float4(root.validity.read(p00).x,
                           root.validity.read(p10).x,
                           root.validity.read(p01).x,
                           root.validity.read(p11).x);
  // One marks reliable opaque/background motion. Zero is unsupported motion;
  // 2 + authored reactivity marks transmission or blending, all excluded here.
  if (any(validity <= 0.5f) || any(validity > 1.5f)) {
    root.output.write(float4(current.rgb, 0.0f), pixel);
    return;
  }

  // Current + four validity + previous = at most six reads and one write.
  // Alpha is private sample age; presentation restores opaque alpha separately.
  float4 previous = root.history.read(pixel);
  float age = min(previous.a + 1.0f, VKR_TEMPORAL_STATIC_SAMPLE_COUNT);
  float weight = (age - 1.0f) / age;
  float4 resolved = previous.a >= VKR_TEMPORAL_STATIC_SAMPLE_COUNT
                        ? previous
                        : float4(mix(current.rgb, previous.rgb, weight), age);
  root.output.write(resolved, pixel);
}

static_assert(sizeof(VkrMetalPacketMetalfxStabilizeRoot) == 64,
              "MetalFX stabilize root ABI must remain 64 bytes");
