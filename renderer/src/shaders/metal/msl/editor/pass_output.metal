/* Editor.PassOutput: one graph image over the Scene image, read texel by
 * texel and coloured by the shared pass output kernel
 * (VkrGpuDebugPayload.pass_output). A source without the output's aspect,
 * such as a square shadow cascade, shows between dark bars, and one the
 * frame's graph lacks as dark stripes. */
struct alignas(16) VkrMetalPacketPassOutputRoot {
  /* x unused (the exposure comes from exposure_state), y and z the camera's
   * near and far planes in metres, w a vector source's display scale. */
  float4 params;
  uint2 extent;
  /* VkrPassOutputKind. */
  uint kind;
  /* The shadow map layer a SHADOW_DEPTH source shows. */
  uint layer;
  device const VkrExposureState *exposure_state;
  constant VkrDisplayOutputParams *display_output;
  /* Every source but a shadow cascade. */
  texture2d<float, access::read> source;
  depth2d_array<float, access::read> shadow;
  /* Returns pre-exposed radiance to scene-linear before exposure. */
  float inverse_pre_exposure;
  uint reserved0;
  uint reserved1;
  uint reserved2;
};

static_assert(sizeof(VkrMetalPacketPassOutputRoot) == 80,
              "Metal pass output root must remain 80 bytes");

fragment float4 vkr_metal_packet_pass_output_fragment(
    VkrMetalPacketEditorGridOutput input [[stage_in]],
    constant VkrMetalPacketPassOutputRoot *root [[buffer(1)]]) {
  const uint2 pixel = uint2(input.position.xy);
  if (root->kind == VKR_PASS_OUTPUT_KIND_MISSING) {
    return float4(vkr_display_output_scale(vkr_pass_output_missing_color(pixel),
                                           *root->display_output),
                  1.0f);
  }
  const bool shadow = root->kind == VKR_PASS_OUTPUT_KIND_SHADOW_DEPTH;
  const uint2 source_extent =
      shadow ? uint2(root->shadow.get_width(), root->shadow.get_height())
             : uint2(root->source.get_width(), root->source.get_height());
  const int2 texel = vkr_pass_output_texel(pixel, root->extent, source_extent);
  float3 color = float3(0.03f);
  if (texel.x >= 0) {
    const float4 value =
        shadow ? float4(root->shadow.read(uint2(texel), root->layer))
               : root->source.read(uint2(texel));
    float4 params = root->params;
    params.x = root->exposure_state->exposure_multiplier *
               root->inverse_pre_exposure;
    color = vkr_pass_output_color(root->kind, value, params);
  }
  return float4(vkr_display_output_scale(color, *root->display_output), 1.0f);
}
