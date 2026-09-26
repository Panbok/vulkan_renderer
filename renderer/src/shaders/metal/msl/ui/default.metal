/* One UI stream record. The mode selects vertex color, MTSDF or bitmap glyph
 * coverage, a signed-distance box, or a textured image. */
struct VkrMetalPacketUiVertex {
  float2 position;
  float2 texcoord;
  float4 color;
  float4 border_color;
  /* top-left, top-right, bottom-right, bottom-left */
  float4 corner_radius;
  /* Y-down pixel offset from the box center */
  float2 local;
  float2 half_extent;
  float border;
  float softness;
  uint mode;
  uint reserved;
};

static_assert(sizeof(VkrMetalPacketUiVertex) == 96,
              "VkrMetalPacketUiVertex must remain 96 bytes");

struct VkrMetalPacketUiRoot {
  device VkrMetalPacketUiVertex *vertices;
  texture2d<float, access::sample> texture;
  float4 target_unit_range;
  uint flags;
  uint reserved;
  constant VkrDisplayOutputParams *display_output;
};

struct VkrMetalPacketUiOutput {
  float4 position [[position]];
  float2 texcoord [[user(TEXCOORD)]];
  float4 color [[user(COLOR)]];
  float4 border_color [[user(BORDER_COLOR)]];
  float4 corner_radius [[flat]];
  float2 local [[user(LOCAL)]];
  float2 half_extent [[flat]];
  float border [[flat]];
  float softness [[flat]];
  uint mode [[flat]];
};

static_assert(sizeof(VkrMetalPacketUiRoot) == 48,
              "VkrMetalPacketUiRoot must remain 48 bytes");

constant uint VKR_METAL_UI_MODE_QUAD = 0u;
constant uint VKR_METAL_UI_MODE_MTSDF_TEXT = 1u;
constant uint VKR_METAL_UI_MODE_BOX = 3u;
constant uint VKR_METAL_UI_MODE_IMAGE = 4u;

static float4 vkr_metal_packet_ui_display_color(
    float4 color, constant VkrMetalPacketUiRoot *root) {
  color.rgb = vkr_display_output_ui(color.rgb, *root->display_output);
  return color;
}

/* Signed distance to a rounded box; negative inside. `local` is Y-down. */
static float vkr_metal_packet_ui_box_distance(float2 local, float2 half_extent,
                                              float4 corner_radius) {
  const bool left = local.x < 0.0;
  const bool top = local.y < 0.0;
  const float radius = left ? (top ? corner_radius.x : corner_radius.w)
                            : (top ? corner_radius.y : corner_radius.z);
  const float2 q = abs(local) - (half_extent - radius);
  return length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

vertex VkrMetalPacketUiOutput vkr_metal_packet_ui_vertex(
    uint vertex_id [[vertex_id]],
    constant VkrMetalPacketUiRoot *root [[buffer(1)]]) {
  const VkrMetalPacketUiVertex vertex_data = root->vertices[vertex_id];
  const float2 target = max(root->target_unit_range.xy, float2(1.0));
  VkrMetalPacketUiOutput output;
  output.position =
      float4(vertex_data.position.x / target.x * 2.0 - 1.0,
             vertex_data.position.y / target.y * 2.0 - 1.0, 0.0, 1.0);
  output.texcoord = vertex_data.texcoord;
  output.color = vertex_data.color;
  output.border_color = vertex_data.border_color;
  output.corner_radius = vertex_data.corner_radius;
  output.local = vertex_data.local;
  output.half_extent = vertex_data.half_extent;
  output.border = vertex_data.border;
  output.softness = vertex_data.softness;
  output.mode = vertex_data.mode;
  return output;
}

fragment float4 vkr_metal_packet_ui_fragment(
    VkrMetalPacketUiOutput input [[stage_in]],
    constant VkrMetalPacketUiRoot *root [[buffer(1)]]) {
  constexpr sampler ui_sampler(coord::normalized, address::clamp_to_edge,
                               filter::linear);
  if (input.mode == VKR_METAL_UI_MODE_QUAD)
    return vkr_metal_packet_ui_display_color(input.color, root);

  if (input.mode == VKR_METAL_UI_MODE_BOX) {
    const float distance = vkr_metal_packet_ui_box_distance(
        input.local, input.half_extent, input.corner_radius);
    const float edge = max(fwidth(distance), 1e-4);
    /* A feathered box ramps coverage across twice its softness. */
    const float coverage =
        input.softness > 0.0
            ? 1.0 - smoothstep(-input.softness, input.softness, distance)
            : saturate(0.5 - distance / edge);
    float4 color = input.color;
    if (input.border > 0.0) {
      const float border_mix = saturate(0.5 + (distance + input.border) / edge);
      color = mix(input.color, input.border_color, border_mix);
    }
    color.a *= coverage;
    return vkr_metal_packet_ui_display_color(color, root);
  }

  const float4 texel = root->texture.sample(ui_sampler, input.texcoord);
  if (input.mode == VKR_METAL_UI_MODE_IMAGE)
    return vkr_metal_packet_ui_display_color(input.color * texel, root);

  float alpha = texel.a;
  if (input.mode == VKR_METAL_UI_MODE_MTSDF_TEXT) {
    const float2 dx = dfdx(input.texcoord);
    const float2 dy = dfdy(input.texcoord);
    const float2 gradient_squared =
        max(dx * dx + dy * dy, float2(1e-12, 1e-12));
    const float2 screen_tex_size = rsqrt(gradient_squared);
    const float range =
        max(0.5 * dot(root->target_unit_range.zw, screen_tex_size), 1.0);
    const float signed_distance =
        max(min(texel.r, texel.g), min(max(texel.r, texel.g), texel.b)) - 0.5;
    alpha = saturate(range * signed_distance + 0.5);
  }
  return vkr_metal_packet_ui_display_color(
      float4(input.color.rgb, input.color.a * alpha), root);
}
