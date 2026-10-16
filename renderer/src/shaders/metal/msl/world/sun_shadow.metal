/* SDF sun shadows on the tiled pipeline (the shadow quality proposal's T5
 * prototype, vkr_metal_packet_sdf.inc). A static submesh's distance volume
 * is generated from its resident triangles: each voxel keeps the unsigned
 * distance to the nearest triangle within a narrow band, in voxels, which
 * the trace reads through trilinear filtering. The trace marches each
 * pixel's ray toward the sun through the volumes of the casters in its
 * light-space grid cell and estimates the penumbra from the nearest miss. */

// Mirrors VkrMetalSdfGenerateRoot.
struct alignas(16) VkrMetalSdfGenerateRoot {
  // xyz the volume's mesh-local origin, w its voxel size.
  float4 box_min_voxel;
  // A uint3 would take 16 bytes.
  uint dims[3];
  uint triangle_count;
  device atomic_uint *scratch;
  texture3d<float, access::write> volume;
  const device VkrPackedStaticVertex *vertices;
  const device uint *indices;
  const device VkrGpuGeometryDecodeRecord *decodes;
  int vertex_offset;
  uint decode_index;
  // The narrow band in voxels.
  float band;
};

static_assert(sizeof(VkrMetalSdfGenerateRoot) == 96u,
              "VkrMetalSdfGenerateRoot ABI drift");

static uint3 vkr_metal_sdf_dims(constant VkrMetalSdfGenerateRoot &root) {
  return uint3(root.dims[0], root.dims[1], root.dims[2]);
}

// Every voxel starts a band away: no triangle within it.
kernel void vkr_metal_sdf_clear(constant VkrMetalSdfGenerateRoot &root
                                [[buffer(0)]],
                                uint voxel [[thread_position_in_grid]]) {
  const uint3 dims = vkr_metal_sdf_dims(root);
  if (voxel >= dims.x * dims.y * dims.z)
    return;
  atomic_store_explicit(&root.scratch[voxel], as_type<uint>(root.band),
                        memory_order_relaxed);
}

// The distance from `p` to triangle `a b c` (Ericson, Real-Time Collision
// Detection, 5.1.5).
static float vkr_metal_sdf_triangle_distance(float3 p, float3 a, float3 b,
                                             float3 c) {
  const float3 ab = b - a;
  const float3 ac = c - a;
  const float3 ap = p - a;
  const float d1 = dot(ab, ap);
  const float d2 = dot(ac, ap);
  if (d1 <= 0.0f && d2 <= 0.0f)
    return length(ap);
  const float3 bp = p - b;
  const float d3 = dot(ab, bp);
  const float d4 = dot(ac, bp);
  if (d3 >= 0.0f && d4 <= d3)
    return length(bp);
  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    return length(ap - ab * (d1 / (d1 - d3)));
  const float3 cp = p - c;
  const float d5 = dot(ab, cp);
  const float d6 = dot(ac, cp);
  if (d6 >= 0.0f && d5 <= d6)
    return length(cp);
  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    return length(ap - ac * (d2 / (d2 - d6)));
  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    return length(bp - (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))));
  const float denominator = 1.0f / (va + vb + vc);
  return length(ap - ab * (vb * denominator) - ac * (vc * denominator));
}

// One triangle lowers the distance of every voxel within the band of it.
kernel void vkr_metal_sdf_splat(constant VkrMetalSdfGenerateRoot &root
                                [[buffer(0)]],
                                uint triangle [[thread_position_in_grid]]) {
  if (triangle >= root.triangle_count)
    return;
  const device VkrGpuGeometryDecodeRecord &decode =
      root.decodes[root.decode_index];
  float3 corners[3];
  for (uint i = 0u; i < 3u; ++i) {
    const uint vertex_index =
        uint(int(root.indices[triangle * 3u + i]) + root.vertex_offset);
    const float3 position =
        vkr_decode_packed_vertex(root.vertices[vertex_index], decode).position;
    // In voxels from the volume's origin.
    corners[i] = (position - root.box_min_voxel.xyz) / root.box_min_voxel.w;
  }
  const float3 low = min(corners[0], min(corners[1], corners[2])) - root.band;
  const float3 high = max(corners[0], max(corners[1], corners[2])) + root.band;
  const int3 first = max(int3(floor(low)), int3(0));
  const uint3 dims = vkr_metal_sdf_dims(root);
  const int3 last = min(int3(ceil(high)), int3(dims) - 1);
  for (int z = first.z; z <= last.z; ++z) {
    for (int y = first.y; y <= last.y; ++y) {
      for (int x = first.x; x <= last.x; ++x) {
        const float distance = vkr_metal_sdf_triangle_distance(
            float3(x, y, z) + 0.5f, corners[0], corners[1], corners[2]);
        if (distance < root.band)
          atomic_fetch_min_explicit(
              &root.scratch[(uint(z) * dims.y + uint(y)) * dims.x + uint(x)],
              as_type<uint>(distance), memory_order_relaxed);
      }
    }
  }
}

// The volume stores each voxel's distance over the band.
kernel void vkr_metal_sdf_convert(constant VkrMetalSdfGenerateRoot &root
                                  [[buffer(0)]],
                                  uint3 voxel [[thread_position_in_grid]]) {
  const uint3 dims = vkr_metal_sdf_dims(root);
  if (any(voxel >= dims))
    return;
  const float distance = as_type<float>(atomic_load_explicit(
      &root.scratch[(voxel.z * dims.y + voxel.y) * dims.x + voxel.x],
      memory_order_relaxed));
  root.volume.write(float4(distance / root.band), voxel);
}

// Mirrors VkrMetalSdfObject.
struct VkrMetalSdfObject {
  // Rows mapping a world point to the volume's [0, 1] coordinates.
  float4 world_to_volume[3];
  // x: the world distance a stored one stands for; y: the world voxel size.
  float4 scale;
  texture3d<float, access::sample> volume;
  uint2 reserved;
};

static_assert(sizeof(VkrMetalSdfObject) == 80u, "VkrMetalSdfObject ABI drift");

// Mirrors VkrMetalSunShadowRoot.
struct alignas(16) VkrMetalSunShadowRoot {
  float4x4 inverse_view_projection;
  // xyz the light-space axes across the sun's direction, w the grid's
  // first coordinate along them.
  float4 light_u;
  float4 light_v;
  // xyz toward the sun, w one over the grid's cell size.
  float4 light_dir;
  depth2d<float, access::read> depth;
  texture2d<float, access::write> output;
  const device VkrMetalSdfObject *objects;
  // grid_size² + 1 offsets, then the cells' object indices.
  const device uint *grid;
  uint2 extent;
  uint2 depth_extent;
  uint grid_size;
  // Pixels nearer along the camera axis are not traced.
  float start_distance;
  float max_distance;
  // One over the tangent of the sun's half angle.
  float softness;
  // A depth's distance along the camera axis:
  // depth_scale / (depth + depth_offset).
  float depth_offset;
  float depth_scale;
};

static_assert(sizeof(VkrMetalSunShadowRoot) == 192u,
              "VkrMetalSunShadowRoot ABI drift");

// The steps a ray takes through one volume.
#define VKR_METAL_SDF_TRACE_STEPS 32u

// The sun's visibility at `world` through one caster's volume: sphere
// tracing from where the ray enters the volume's box, keeping the smallest
// ratio of a miss distance to its distance along the ray as the penumbra
// (Quilez). A sample at the band's edge knows no nearer surface and does not
// narrow the penumbra; a sample within half a voxel is a hit.
static float vkr_metal_sun_shadow_object(constant VkrMetalSunShadowRoot &root,
                                         const device VkrMetalSdfObject &object,
                                         float3 world, float visibility) {
  constexpr sampler volume_sampler(coord::normalized, address::clamp_to_edge,
                                   filter::linear);
  const float4 point = float4(world, 1.0f);
  const float3 origin = float3(dot(object.world_to_volume[0], point),
                               dot(object.world_to_volume[1], point),
                               dot(object.world_to_volume[2], point));
  const float3 direction =
      float3(dot(object.world_to_volume[0].xyz, root.light_dir.xyz),
             dot(object.world_to_volume[1].xyz, root.light_dir.xyz),
             dot(object.world_to_volume[2].xyz, root.light_dir.xyz));
  const float3 inverse = 1.0f / select(direction, float3(1e-8f),
                                      abs(direction) < 1e-8f);
  const float3 t0 = -origin * inverse;
  const float3 t1 = (1.0f - origin) * inverse;
  const float3 near_t = min(t0, t1);
  const float3 far_t = max(t0, t1);
  const float voxel = object.scale.y;
  const float band = object.scale.x;
  // A receiver on the caster itself leaves it by two voxels first.
  float t = max(max(near_t.x, max(near_t.y, near_t.z)), 2.0f * voxel);
  const float end = min(min(far_t.x, min(far_t.y, far_t.z)), root.max_distance);
  for (uint step = 0u; step < VKR_METAL_SDF_TRACE_STEPS && t < end; ++step) {
    const float distance =
        object.volume.sample(volume_sampler, origin + t * direction).r * band;
    if (distance < 0.5f * voxel)
      return 0.0f;
    if (distance < 0.95f * band)
      visibility = min(visibility, root.softness * distance / t);
    t += max(distance, 0.5f * voxel);
  }
  return visibility;
}

// Half-resolution SDF sun shadows past the near cascades: visibility and
// coverage, which is zero where the trace has no data (sky, nearer than its
// start, outside the grid).
kernel void
vkr_metal_sun_shadow_trace(constant VkrMetalSunShadowRoot &root [[buffer(0)]],
                           uint2 texel [[thread_position_in_grid]]) {
  if (any(texel >= root.extent))
    return;
  const uint2 pixel = min(texel * 2u, root.depth_extent - 1u);
  const float depth = root.depth.read(pixel);
  float2 result = float2(1.0f, 0.0f);
  if (depth > 0.0f &&
      root.depth_scale / (depth + root.depth_offset) >= root.start_distance) {
    const float4 world_h =
        root.inverse_view_projection *
        float4(vkr_metal_packet_resolve_ndc(float2(pixel) + 0.5f,
                                            root.depth_extent),
               depth, 1.0f);
    const float3 world = world_h.xyz / world_h.w;
    const float2 cell =
        (float2(dot(world, root.light_u.xyz), dot(world, root.light_v.xyz)) -
         float2(root.light_u.w, root.light_v.w)) *
        root.light_dir.w;
    if (all(cell >= 0.0f) && all(cell < float(root.grid_size))) {
      const uint index = uint(cell.y) * root.grid_size + uint(cell.x);
      const uint first = root.grid[index];
      const uint last = root.grid[index + 1u];
      const device uint *casters =
          root.grid + root.grid_size * root.grid_size + 1u;
      float visibility = 1.0f;
      for (uint i = first; i < last && visibility > 0.0f; ++i)
        visibility = vkr_metal_sun_shadow_object(root, root.objects[casters[i]],
                                                 world, visibility);
      result = float2(saturate(visibility), 1.0f);
    }
  }
  root.output.write(float4(result, 0.0f, 0.0f), texel);
}
