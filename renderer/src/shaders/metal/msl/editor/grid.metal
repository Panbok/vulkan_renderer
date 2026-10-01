/* Editor ground grid: a full-screen pass over the Scene image that draws the
 * plane through the world origin, hidden behind nearer opaque geometry unless
 * the grid shows through it. */
struct alignas(16) VkrMetalPacketEditorGridRoot {
  float4x4 inverse_view_projection;
  /* xyz camera position; w is 1 when the grid fades with distance. */
  float4 camera_position;
  /* x finest cell, y fade start, z fade end, w 1 when drawn through
   * geometry. */
  float4 params;
  uint2 extent;
  /* 0 is the XZ plane (y = 0), 1 the ZY plane (x = 0). */
  uint plane;
  uint reserved;
  texture2d<float, access::read> depth;
  constant VkrDisplayOutputParams *display_output;
};

static_assert(sizeof(VkrMetalPacketEditorGridRoot) == 128,
              "Metal editor grid root must remain 128 bytes");

struct VkrMetalPacketEditorGridOutput {
  float4 position [[position]];
};

vertex VkrMetalPacketEditorGridOutput
vkr_metal_packet_editor_grid_vertex(uint vertex_id [[vertex_id]]) {
  const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0),
                               float2(-1.0, 3.0)};
  VkrMetalPacketEditorGridOutput output;
  output.position = float4(positions[vertex_id], 0.0, 1.0);
  return output;
}

static float3 vkr_metal_editor_grid_unproject(
    constant VkrMetalPacketEditorGridRoot *root, float2 ndc, float depth) {
  float4 world = root->inverse_view_projection * float4(ndc, depth, 1.0);
  return world.xyz / max(abs(world.w), 1e-7f) * sign(world.w);
}

fragment float4 vkr_metal_packet_editor_grid_fragment(
    VkrMetalPacketEditorGridOutput input [[stage_in]],
    constant VkrMetalPacketEditorGridRoot *root [[buffer(1)]]) {
  const float2 uv = input.position.xy / float2(root->extent);
  const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  const float3 near_point = vkr_metal_editor_grid_unproject(root, ndc, 0.0);
  const float3 ray =
      vkr_metal_editor_grid_unproject(root, ndc, 1.0) - near_point;
  /* The plane's normal axis and its two in-plane coordinates. */
  const bool side = root->plane == 1u;
  const float origin_height = side ? near_point.x : near_point.y;
  const float slope = side ? ray.x : ray.y;
  const float t = abs(slope) > 1e-9f ? -origin_height / slope : -1.0f;
  const float3 hit = near_point + ray * t;
  const float2 coord = side ? hit.zy : hit.xz;
  /* Derivatives come before any discard. */
  const float2 footprint = fwidth(coord);
  if (t <= 0.0 || t >= 1.0)
    discard_fragment();
  float visibility = 1.0;
  if (root->params.w == 0.0) {
    const uint2 depth_extent =
        uint2(root->depth.get_width(), root->depth.get_height());
    const uint2 texel =
        min(uint2(uv * float2(depth_extent)), depth_extent - 1u);
    const float device_depth = root->depth.read(texel).x;
    if (device_depth < 1.0) {
      const float3 surface =
          vkr_metal_editor_grid_unproject(root, ndc, device_depth);
      const float s =
          dot(surface - near_point, ray) / max(dot(ray, ray), 1e-12f);
      /* A soft band keeps a coplanar floor from shimmering. */
      const float band = max(s * 0.004f, 1e-6f);
      visibility = saturate((s + band - t) / band);
    }
  }
  float fade = 1.0;
  if (root->camera_position.w > 0.0) {
    const float distance = length(hit - root->camera_position.xyz);
    fade = 1.0 - smoothstep(root->params.y, root->params.z, distance);
  }
  const float3 axis_u =
      side ? float3(0.25, 0.45, 1.0) : float3(1.0, 0.24, 0.24);
  const float3 axis_v =
      side ? float3(0.35, 0.85, 0.35) : float3(0.25, 0.45, 1.0);
  float4 color = vkr_editor_grid_shade(coord, footprint, root->params.x,
                                       fade * visibility, axis_u, axis_v);
  if (color.a <= 0.0)
    discard_fragment();
  color.rgb = vkr_display_output_ui(color.rgb, *root->display_output);
  return color;
}
