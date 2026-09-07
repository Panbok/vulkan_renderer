struct VkrMetalPacketTextVertex {
  float2 position;
  float2 texcoord;
  float4 color;
};

struct VkrMetalPacketTextRoot {
  device VkrMetalPacketTextVertex *vertices;
  texture2d<float, access::sample> atlas;
  float4x4 model;
  float4x4 view_projection;
  float4 controls;
  uint object_id;
  uint flags;
  uint2 reserved;
};

struct VkrMetalPacketTextOutput {
  float4 position [[position]];
  float2 texcoord [[user(TEXCOORD)]];
  float4 color [[user(COLOR)]];
  uint object_id [[user(TEXCOORD_1), flat]];
};

static_assert(sizeof(VkrMetalPacketTextVertex) == 32,
              "VkrTextVertex Metal ABI must remain 32 bytes");

float vkr_metal_packet_text_alpha(VkrMetalPacketTextOutput input,
                                  constant VkrMetalPacketTextRoot *root) {
  constexpr sampler atlas_sampler(coord::normalized, address::clamp_to_edge,
                                  filter::linear);
  float4 atlas_sample = root->atlas.sample(atlas_sampler, input.texcoord);
  if ((root->flags & 2u) == 0u)
    return atlas_sample.a;
  float2 dx = dfdx(input.texcoord);
  float2 dy = dfdy(input.texcoord);
  float2 gradient_squared =
      max(dx * dx + dy * dy, float2(1e-12, 1e-12));
  float2 screen_tex_size = rsqrt(gradient_squared);
  float range = max(0.5 * dot(root->controls.xy, screen_tex_size), 1.0);
  float signed_distance =
      max(min(atlas_sample.r, atlas_sample.g),
          min(max(atlas_sample.r, atlas_sample.g), atlas_sample.b)) -
      0.5;
  return saturate(range * signed_distance + 0.5);
}

vertex VkrMetalPacketTextOutput vkr_metal_packet_text_vertex(
    uint vertex_id [[vertex_id]],
    constant VkrMetalPacketTextRoot *root [[buffer(1)]]) {
  VkrMetalPacketTextVertex glyph_vertex = root->vertices[vertex_id];
  float4 position = root->model * float4(glyph_vertex.position, 0.0, 1.0);
  VkrMetalPacketTextOutput output;
  if ((root->flags & 1u) != 0u) {
    float2 target = max(root->controls.zw, float2(1.0));
    output.position =
        float4(position.x / target.x * 2.0 - 1.0,
               position.y / target.y * 2.0 - 1.0, position.z, position.w);
  } else {
    output.position = root->view_projection * position;
  }
  output.texcoord = glyph_vertex.texcoord;
  output.color = glyph_vertex.color;
  output.object_id = root->object_id;
  return output;
}

fragment float4 vkr_metal_packet_text_fragment(
    VkrMetalPacketTextOutput input [[stage_in]],
    constant VkrMetalPacketTextRoot *root [[buffer(1)]]) {
  float alpha = vkr_metal_packet_text_alpha(input, root);
  return float4(input.color.rgb, input.color.a * alpha);
}

fragment uint vkr_metal_packet_text_picking_fragment(
    VkrMetalPacketTextOutput input [[stage_in]],
    constant VkrMetalPacketTextRoot *root [[buffer(1)]]) {
  if (vkr_metal_packet_text_alpha(input, root) <= 0.01)
    discard_fragment();
  return input.object_id;
}
