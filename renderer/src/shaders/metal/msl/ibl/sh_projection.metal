// L2 diffuse coefficient projection (ADR-038 §2), mirroring the Vulkan
// `ibl_sh` entry in vulkan/slang/ibl/default.slang. Both consume the shared
// basis, window, and solid-angle math in shared/sh_l2_kernel.slangh.

struct alignas(16) VkrMetalPacketShProjectRoot {
  texturecube<float, access::read> source;
  device VkrShL2Packed *destination;
  uint source_face_size;
  uint source_mip;
  float window_band_0;
  float window_band_1;
  float window_band_2;
  uint2 reserved;
};

static_assert(sizeof(VkrMetalPacketShProjectRoot) == 48,
              "Metal packet SH projection root ABI must remain 48 bytes");

// One workgroup per destination slot. A fixed lane assignment and a fixed
// reduction order make repetitions deterministic without floating-point
// atomics; the bounded source mip limits the work, but determinism comes from
// the summation order, not the mip.
constant uint vkr_metal_sh_project_lanes = 64u;

// Projects six `face_size`-square cube faces of radiance into `destination`
// (the Vulkan vk_sh_project mirror). Texels come from mip `source_mip` of
// `source`; with `clouded`, each is seen through the layer's in-scatter and
// transmittance in `cloud` (the chain's first level, in the same face, row,
// column order). `window` holds the per-band deringing factors; `partial` and
// `partial_weight` are the calling kernel's threadgroup arrays.
static void vkr_metal_sh_project(
    uint lane, uint face_size, texturecube<float, access::read> source,
    uint source_mip, device const float4 *cloud, bool clouded, float3 window,
    device VkrShL2Packed *destination, threadgroup float3 *partial,
    threadgroup float *partial_weight) {
  uint extent = max(face_size, 1u);
  uint face_texels = extent * extent;
  uint texel_count = face_texels * 6u;
  float texel_step = 2.0 / float(extent);

  float3 accumulated[9];
  for (uint i = 0u; i < 9u; ++i)
    accumulated[i] = float3(0.0);
  float accumulated_weight = 0.0;

  // Strided so every lane walks the same number of texels in the same order
  // regardless of how the domain divides.
  for (uint index = lane; index < texel_count;
       index += vkr_metal_sh_project_lanes) {
    uint face = index / face_texels;
    uint local = index - face * face_texels;
    uint y = local / extent;
    uint x = local - y * extent;

    float s0 = float(x) * texel_step - 1.0;
    float t0 = float(y) * texel_step - 1.0;
    float solid_angle = vkr_sh_cube_texel_solid_angle(s0, s0 + texel_step, t0,
                                                      t0 + texel_step);

    float2 uv = (float2(float(x), float(y)) + 0.5) / float(extent);
    float3 direction = normalize(vkr_metal_packet_cube_direction(face, uv));
    // Exact texel load at the selected mip: a sampled fetch would filter and
    // would not reproduce the CPU reference.
    float3 texel = source.read(uint2(x, y), face, source_mip).rgb;
    // A branch, not a select: the chain exists only on its own path.
    if (clouded)
      texel = vkr_cloud_sky_light_radiance(texel, cloud[index]);

    VkrShL2Basis basis = vkr_sh_l2_basis(direction);
    accumulated_weight += solid_angle;
    for (uint i = 0u; i < 9u; ++i)
      accumulated[i] += texel * (basis.y[i] * solid_angle);
  }

  for (uint i = 0u; i < 9u; ++i)
    partial[lane * 9u + i] = accumulated[i];
  partial_weight[lane] = accumulated_weight;
  threadgroup_barrier(mem_flags::mem_threadgroup);

  for (uint stride = vkr_metal_sh_project_lanes / 2u; stride > 0u;
       stride >>= 1u) {
    if (lane < stride) {
      for (uint i = 0u; i < 9u; ++i)
        partial[lane * 9u + i] += partial[(lane + stride) * 9u + i];
      partial_weight[lane] += partial_weight[lane + stride];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  if (lane != 0u)
    return;

  // Correct the accumulated weight back to the exact sphere measure so a
  // partially cancelled texel sum cannot bias every coefficient.
  float total_weight = partial_weight[0];
  float normalization =
      total_weight > 0.0 ? (4.0 * vkr_metal_packet_pi) / total_weight : 0.0;

  float3 coefficients[9];
  for (uint i = 0u; i < 9u; ++i)
    coefficients[i] = partial[i] * normalization;

  // Normalized clamped-cosine transfer (pi, 2pi/3, pi/4 divided by pi) times
  // the authored deringing window, folded in at projection time so the
  // per-pixel path stays fixed.
  float band0 = 1.0 * window.x;
  float band1 = (2.0 / 3.0) * window.y;
  float band2 = 0.25 * window.z;
  coefficients[0] *= band0;
  for (uint i = 1u; i < 4u; ++i)
    coefficients[i] *= band1;
  for (uint i = 4u; i < 9u; ++i)
    coefficients[i] *= band2;

  for (uint channel = 0u; channel < 3u; ++channel) {
    destination->v[channel] =
        float4(VKR_SH_K1 * coefficients[3][channel],
               VKR_SH_K1 * coefficients[1][channel],
               VKR_SH_K1 * coefficients[2][channel],
               VKR_SH_K0 * coefficients[0][channel] -
                   VKR_SH_K3 * coefficients[6][channel]);
    destination->v[3u + channel] =
        float4(VKR_SH_K2 * coefficients[4][channel],
               VKR_SH_K2 * coefficients[5][channel],
               3.0 * VKR_SH_K3 * coefficients[6][channel],
               VKR_SH_K2 * coefficients[7][channel]);
  }
  destination->v[6] =
      float4(VKR_SH_K4 * coefficients[8][0], VKR_SH_K4 * coefficients[8][1],
             VKR_SH_K4 * coefficients[8][2], 0.0);
}

kernel void
vkr_metal_packet_ibl_sh(uint lane [[thread_position_in_threadgroup]],
                        constant VkrMetalPacketShProjectRoot *root
                        [[buffer(0)]]) {
  // Fixed-size rather than a dynamic threadgroup binding, so the pipeline
  // exposes only the root buffer and matches the Vulkan groupshared layout.
  threadgroup float3 partial[64 * 9];
  threadgroup float partial_weight[64];
  vkr_metal_sh_project(lane, root->source_face_size, root->source,
                       root->source_mip, nullptr, false,
                       float3(root->window_band_0, root->window_band_1,
                              root->window_band_2),
                       root->destination, partial, partial_weight);
}
