/* SDF sun shadows on the tiled pipeline (the shadow quality proposal's T5,
 * vkr_metal_packet_sdf.inc). A submesh's distance volume is generated from
 * its resident triangles: each voxel keeps the unsigned distance to the
 * nearest triangle within a narrow band, in voxels, which the trace reads
 * through trilinear filtering. Each frame the field casters list themselves
 * in a light-space grid, and the trace marches each pixel's ray toward the
 * sun through the volumes of the casters in its cell and estimates the
 * penumbra from the nearest miss. */

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
                                  uint voxel [[thread_position_in_grid]]) {
  const uint3 dims = vkr_metal_sdf_dims(root);
  if (voxel >= dims.x * dims.y * dims.z)
    return;
  const float distance = as_type<float>(
      atomic_load_explicit(&root.scratch[voxel], memory_order_relaxed));
  const uint3 position = uint3(voxel % dims.x, (voxel / dims.x) % dims.y,
                               voxel / (dims.x * dims.y));
  root.volume.write(float4(distance / root.band), position);
}

// Mirrors VkrMetalSdfObject.
struct VkrMetalSdfObject {
  // Rows mapping a world point to the volume's [0, 1] coordinates.
  float4 world_to_volume[3];
  // x: the world distance a stored one stands for; y: the world voxel size.
  float4 scale;
  texture3d<float, access::sample> volume;
  // The grid cells its box covers: u0 | u1 << 16, v0 | v1 << 16; u0 above
  // u1 when none.
  uint2 cells;
};

static_assert(sizeof(VkrMetalSdfObject) == 80u, "VkrMetalSdfObject ABI drift");

// Mirrors VkrMetalSdfGridRoot. `grid` holds grid_size² + 1 offsets, then
// `entry_capacity` candidate indices, a cursor per cell and the scan's block
// sums.
struct alignas(16) VkrMetalSdfGridRoot {
  // xyz the light-space axes across the sun's direction, w the grid's
  // first coordinate along them.
  float4 light_u;
  float4 light_v;
  const device VkrGpuCandidateDrawRow *candidates;
  const device VkrMetalPacketInstance *instances;
  const device VkrGpuGeometryRow *geometry_rows;
  const device VkrMetalSdfVolumeRow *volume_rows;
  const device uint2 *mesh_volumes;
  device VkrMetalSdfObject *objects;
  device uint *grid;
  uint candidate_count;
  uint mesh_count;
  uint grid_size;
  uint entry_capacity;
  float inverse_cell;
  // The volumes' narrow band in voxels.
  float band;
};

static_assert(sizeof(VkrMetalSdfGridRoot) == 112u,
              "VkrMetalSdfGridRoot ABI drift");

static device atomic_uint *
vkr_metal_sdf_grid_cursors(constant VkrMetalSdfGridRoot &root) {
  return (device atomic_uint *)(root.grid + root.grid_size * root.grid_size +
                                1u + root.entry_capacity);
}

// Every cell starts empty.
kernel void vkr_metal_sdf_grid_clear(constant VkrMetalSdfGridRoot &root
                                     [[buffer(0)]],
                                     uint cell [[thread_position_in_grid]]) {
  if (cell >= root.grid_size * root.grid_size)
    return;
  atomic_store_explicit(&vkr_metal_sdf_grid_cursors(root)[cell], 0u,
                        memory_order_relaxed);
}

// A field caster's object record, at its candidate's index, and a count in
// every cell its box covers in the plane across the sun's direction. Other
// candidates cover no cells. One SIMD group takes each candidate: its lanes
// place the box's corners and share the cells, since a large caster at a low
// sun covers thousands.
kernel void vkr_metal_sdf_grid_count(
    constant VkrMetalSdfGridRoot &root [[buffer(0)]],
    uint grid_thread [[thread_position_in_grid]],
    uint lane [[thread_index_in_simdgroup]]) {
  const uint index = grid_thread / 32u;
  if (index >= root.candidate_count)
    return;
  const device VkrGpuCandidateDrawRow &candidate = root.candidates[index];
  const device VkrMetalPacketInstance &instance =
      root.instances[candidate.instance_index];
  device VkrMetalSdfObject &object = root.objects[index];
  const uint row = vkr_metal_sdf_field_row(
      root.volume_rows, root.mesh_volumes, root.mesh_count, candidate,
      root.geometry_rows[candidate.geometry_index], instance);
  if (row == VKR_METAL_SDF_NO_ROW) {
    if (lane == 0u)
      object.cells = uint2(1u, 0u);
    return;
  }
  const device VkrMetalSdfVolumeRow &volume = root.volume_rows[row];
  const float voxel = volume.box_min_voxel.w;
  const float3 size =
      float3(volume.dims[0], volume.dims[1], volume.dims[2]) * voxel;
  const float4x4 model = instance.model;
  const uint corner = lane & 7u;
  const float3 local =
      volume.box_min_voxel.xyz +
      select(float3(0.0f), size,
             bool3((corner & 1u) != 0u, (corner & 2u) != 0u,
                   (corner & 4u) != 0u));
  const float3 world = (model * float4(local, 1.0f)).xyz;
  const float2 cell = (float2(dot(world, root.light_u.xyz),
                              dot(world, root.light_v.xyz)) -
                       float2(root.light_u.w, root.light_v.w)) *
                      root.inverse_cell;
  const float2 low = float2(simd_min(cell.x), simd_min(cell.y));
  const float2 high = float2(simd_max(cell.x), simd_max(cell.y));
  const float size_cells = float(root.grid_size);
  if (any(high < 0.0f) || any(low >= size_cells)) {
    if (lane == 0u)
      object.cells = uint2(1u, 0u);
    return;
  }
  const uint2 first = uint2(max(low, 0.0f));
  const uint2 last = uint2(min(high, size_cells - 1.0f));

  if (lane == 0u) {
    // The volume's [0, 1] coordinates of a world point, through the inverse
    // of the instance's affine transform.
    const float3 c0 = model[0].xyz;
    const float3 c1 = model[1].xyz;
    const float3 c2 = model[2].xyz;
    const float3 inverse_rows[3] = {cross(c1, c2), cross(c2, c0),
                                    cross(c0, c1)};
    const float inverse_det = 1.0f / dot(c0, inverse_rows[0]);
    for (uint axis = 0u; axis < 3u; ++axis) {
      const float3 inverse_row = inverse_rows[axis] * inverse_det;
      const float extent = size[axis];
      object.world_to_volume[axis] = float4(
          inverse_row / extent,
          (-dot(inverse_row, model[3].xyz) - volume.box_min_voxel[axis]) /
              extent);
    }
    // The smallest axis scale keeps a stretched instance's distances
    // conservative.
    const float world_voxel =
        voxel * min(length(c0), min(length(c1), length(c2)));
    object.scale = float4(root.band * world_voxel, world_voxel, 0.0f, 0.0f);
    object.volume = volume.volume;
    object.cells = uint2(first.x | (last.x << 16u), first.y | (last.y << 16u));
  }
  const uint width = last.x - first.x + 1u;
  const uint area = width * (last.y - first.y + 1u);
  device atomic_uint *cursors = vkr_metal_sdf_grid_cursors(root);
  for (uint i = lane; i < area; i += 32u)
    atomic_fetch_add_explicit(
        &cursors[(first.y + i / width) * root.grid_size + first.x + i % width],
        1u, memory_order_relaxed);
}

// The scan of the cells' counts into offsets runs over blocks of
// VKR_METAL_SDF_SCAN_BLOCK cells, a threadgroup of VKR_METAL_SDF_SCAN_THREADS
// each, four adjacent cells a thread; one threadgroup scans the blocks'
// sums between. Mirrors the host's VKR_METAL_SDF_SCAN_*.
#define VKR_METAL_SDF_SCAN_THREADS 256u
#define VKR_METAL_SDF_SCAN_BLOCK 1024u

// The blocks' sums follow the cursors.
static device uint *
vkr_metal_sdf_grid_block_sums(constant VkrMetalSdfGridRoot &root) {
  return (device uint *)vkr_metal_sdf_grid_cursors(root) +
         root.grid_size * root.grid_size;
}

// The exclusive prefix of `value` over the threadgroup's threads, and the
// threadgroup's total in `total`.
static uint vkr_metal_sdf_group_prefix(uint value, uint simd_lane,
                                       uint simd_group,
                                       threadgroup uint *simd_sums,
                                       thread uint &total) {
  const uint lane_prefix = simd_prefix_exclusive_sum(value);
  if (simd_lane == 31u)
    simd_sums[simd_group] = lane_prefix + value;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  const uint groups = VKR_METAL_SDF_SCAN_THREADS / 32u;
  uint simd_offset = 0u;
  total = 0u;
  for (uint group = 0u; group < groups; ++group) {
    simd_offset += group < simd_group ? simd_sums[group] : 0u;
    total += simd_sums[group];
  }
  return simd_offset + lane_prefix;
}

// Each block's count of entries.
kernel void vkr_metal_sdf_grid_reduce(
    constant VkrMetalSdfGridRoot &root [[buffer(0)]],
    uint block [[threadgroup_position_in_grid]],
    uint thread_index [[thread_position_in_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]],
    uint simd_group [[simdgroup_index_in_threadgroup]]) {
  threadgroup uint simd_sums[VKR_METAL_SDF_SCAN_THREADS / 32u];
  const device uint *cursors =
      (const device uint *)vkr_metal_sdf_grid_cursors(root);
  const uint first = block * VKR_METAL_SDF_SCAN_BLOCK + thread_index * 4u;
  const uint sum = cursors[first] + cursors[first + 1u] +
                   cursors[first + 2u] + cursors[first + 3u];
  const uint block_sum = simd_sum(sum);
  if (simd_lane == 0u)
    simd_sums[simd_group] = block_sum;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (thread_index == 0u) {
    uint total = 0u;
    for (uint group = 0u; group < VKR_METAL_SDF_SCAN_THREADS / 32u; ++group)
      total += simd_sums[group];
    vkr_metal_sdf_grid_block_sums(root)[block] = total;
  }
}

// Each block's first entry, an exclusive prefix of the blocks' counts, in
// one threadgroup; the entry total follows the cells' offsets.
kernel void vkr_metal_sdf_grid_scan(
    constant VkrMetalSdfGridRoot &root [[buffer(0)]],
    uint thread_index [[thread_position_in_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]],
    uint simd_group [[simdgroup_index_in_threadgroup]]) {
  threadgroup uint simd_sums[VKR_METAL_SDF_SCAN_THREADS / 32u];
  const uint cells = root.grid_size * root.grid_size;
  const uint blocks = cells / VKR_METAL_SDF_SCAN_BLOCK;
  device uint *sums = vkr_metal_sdf_grid_block_sums(root);
  // A thread's run of adjacent blocks.
  const uint run = (blocks + VKR_METAL_SDF_SCAN_THREADS - 1u) /
                   VKR_METAL_SDF_SCAN_THREADS;
  const uint first = min(thread_index * run, blocks);
  const uint last = min(first + run, blocks);
  uint sum = 0u;
  for (uint block = first; block < last; ++block)
    sum += sums[block];
  uint total = 0u;
  uint offset = vkr_metal_sdf_group_prefix(sum, simd_lane, simd_group,
                                           simd_sums, total);
  for (uint block = first; block < last; ++block) {
    const uint count = sums[block];
    sums[block] = offset;
    offset += count;
  }
  if (thread_index == 0u)
    root.grid[cells] = total;
}

// Each cell's first entry, which also starts its cursor: its block's first
// plus the counts before it in the block. The total may pass the capacity,
// whose overflow the trace leaves out.
kernel void vkr_metal_sdf_grid_apply(
    constant VkrMetalSdfGridRoot &root [[buffer(0)]],
    uint block [[threadgroup_position_in_grid]],
    uint thread_index [[thread_position_in_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]],
    uint simd_group [[simdgroup_index_in_threadgroup]]) {
  threadgroup uint simd_sums[VKR_METAL_SDF_SCAN_THREADS / 32u];
  device uint *cursors = (device uint *)vkr_metal_sdf_grid_cursors(root);
  const uint first = block * VKR_METAL_SDF_SCAN_BLOCK + thread_index * 4u;
  const uint counts[4] = {cursors[first], cursors[first + 1u],
                          cursors[first + 2u], cursors[first + 3u]};
  uint total = 0u;
  uint offset =
      vkr_metal_sdf_grid_block_sums(root)[block] +
      vkr_metal_sdf_group_prefix(counts[0] + counts[1] + counts[2] + counts[3],
                                 simd_lane, simd_group, simd_sums, total);
  for (uint i = 0u; i < 4u; ++i) {
    root.grid[first + i] = offset;
    cursors[first + i] = offset;
    offset += counts[i];
  }
}

// A listed caster's candidate index in each cell its box covers, below the
// entry capacity; one SIMD group a candidate shares its cells.
kernel void vkr_metal_sdf_grid_scatter(
    constant VkrMetalSdfGridRoot &root [[buffer(0)]],
    uint grid_thread [[thread_position_in_grid]],
    uint lane [[thread_index_in_simdgroup]]) {
  const uint index = grid_thread / 32u;
  if (index >= root.candidate_count)
    return;
  const uint2 cells = root.objects[index].cells;
  const uint2 first = cells & 0xFFFFu;
  const uint2 last = cells >> 16u;
  if (first.x > last.x)
    return;
  device uint *entries = root.grid + root.grid_size * root.grid_size + 1u;
  device atomic_uint *cursors = vkr_metal_sdf_grid_cursors(root);
  const uint width = last.x - first.x + 1u;
  const uint area = width * (last.y - first.y + 1u);
  for (uint i = lane; i < area; i += 32u) {
    const uint slot = atomic_fetch_add_explicit(
        &cursors[(first.y + i / width) * root.grid_size + first.x + i % width],
        1u, memory_order_relaxed);
    if (slot < root.entry_capacity)
      entries[slot] = index;
  }
}

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
  // The casters by candidate index.
  const device VkrMetalSdfObject *objects;
  // grid_size² + 1 offsets, then `entry_capacity` candidate indices.
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
  uint entry_capacity;
  uint reserved;
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
      const uint first = min(root.grid[index], root.entry_capacity);
      const uint last = min(root.grid[index + 1u], root.entry_capacity);
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
