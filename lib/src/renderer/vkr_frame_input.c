#include "renderer/vkr_frame_input.h"

#include <math.h>

vkr_internal VkrRendererError vkr_renderer_validation_fail(
    VkrValidationError *out_error, VkrRendererError code,
    const char *field_path, const char *message) {
  if (out_error) {
    out_error->code = code;
    out_error->field_path = field_path;
    out_error->message = message;
  }
  return code;
}

vkr_internal VkrRendererError vkr_renderer_validate_packet_array(
    const void *data, uint32_t count, uint32_t capacity, const char *data_field,
    const char *count_field, VkrValidationError *out_error) {
  if (count > capacity)
    return vkr_renderer_validation_fail(
        out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, count_field,
        "exceeds the fixed packet capacity");
  if (count > 0u && !data)
    return vkr_renderer_validation_fail(
        out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, data_field,
        "must contain every declared row");
  return VKR_RENDERER_ERROR_NONE;
}

vkr_internal VkrRendererError vkr_renderer_validate_draw_ranges(
    const VkrDrawItem *draws, uint32_t draw_count, uint32_t instance_count,
    const char *field, VkrValidationError *out_error) {
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrDrawItem *draw = &draws[i];
    if (draw->first_instance > instance_count ||
        draw->instance_count > instance_count - draw->first_instance)
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, field,
          "contains a draw outside the payload instance array");
  }
  return VKR_RENDERER_ERROR_NONE;
}

vkr_internal VkrRendererError vkr_renderer_validate_text_draws(
    const VkrPreparedTextDraw *draws, uint32_t draw_count, const char *field,
    const char *count_field, VkrValidationError *out_error) {
  const VkrRendererError array_error = vkr_renderer_validate_packet_array(
      draws, draw_count, VKR_GPU_DRAW_CANDIDATE_CAPACITY, field, count_field,
      out_error);
  if (array_error != VKR_RENDERER_ERROR_NONE)
    return array_error;
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrPreparedTextDraw *draw = &draws[i];
    if (draw->vertex_count == 0u || draw->index_count == 0u ||
        !draw->vertices || !draw->indices ||
        draw->max_index >= draw->vertex_count)
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, field,
          "contains incomplete or out-of-range indexed geometry");
  }
  return VKR_RENDERER_ERROR_NONE;
}

vkr_internal bool8_t vkr_renderer_ui_vec4_finite(Vec4 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z) &&
         isfinite(value.w);
}

vkr_internal VkrRendererError vkr_renderer_validate_ui_draw_list(
    const VkrPreparedUiDrawList *list, uint32_t target_width,
    uint32_t target_height, VkrValidationError *out_error) {
  VkrRendererError error = vkr_renderer_validate_packet_array(
      list->vertices, list->vertex_count, VKR_UI_VERTEX_CAPACITY,
      "packet.ui.draw_list.vertices", "packet.ui.draw_list.vertex_count",
      out_error);
  if (error != VKR_RENDERER_ERROR_NONE)
    return error;
  error = vkr_renderer_validate_packet_array(
      list->indices, list->index_count, VKR_UI_INDEX_CAPACITY,
      "packet.ui.draw_list.indices", "packet.ui.draw_list.index_count",
      out_error);
  if (error != VKR_RENDERER_ERROR_NONE)
    return error;
  error = vkr_renderer_validate_packet_array(
      list->batches, list->batch_count, VKR_UI_BATCH_CAPACITY,
      "packet.ui.draw_list.batches", "packet.ui.draw_list.batch_count",
      out_error);
  if (error != VKR_RENDERER_ERROR_NONE)
    return error;
  const bool8_t empty = list->vertex_count == 0u && list->index_count == 0u &&
                        list->batch_count == 0u;
  if (empty)
    return VKR_RENDERER_ERROR_NONE;
  if (list->vertex_count == 0u || list->index_count == 0u ||
      list->batch_count == 0u)
    return vkr_renderer_validation_fail(
        out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, "packet.ui.draw_list",
        "must contain complete indexed geometry");

  for (uint32_t i = 0u; i < list->vertex_count; ++i) {
    const VkrUiVertex *vertex = &list->vertices[i];
    if (!isfinite(vertex->position.x) || !isfinite(vertex->position.y) ||
        !isfinite(vertex->texcoord.x) || !isfinite(vertex->texcoord.y) ||
        !vkr_renderer_ui_vec4_finite(vertex->color))
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
          "packet.ui.draw_list.vertices", "contains a non-finite vertex");
  }
  for (uint32_t i = 0u; i < list->index_count; ++i) {
    if (list->indices[i] >= list->vertex_count)
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
          "packet.ui.draw_list.indices", "contains an out-of-range index");
  }

  uint32_t expected_first_index = 0u;
  for (uint32_t i = 0u; i < list->batch_count; ++i) {
    const VkrUiDrawBatch *batch = &list->batches[i];
    if (batch->first_index != expected_first_index ||
        batch->index_count == 0u || batch->index_count % 3u != 0u ||
        batch->index_count > list->index_count - batch->first_index)
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
          "packet.ui.draw_list.batches",
          "contains a non-contiguous or out-of-range index span");
    expected_first_index += batch->index_count;

    const VkrUiRect scissor = batch->scissor_rect_px;
    if (!vkr_ui_rect_has_area(scissor) || truncf(scissor.x) != scissor.x ||
        truncf(scissor.y) != scissor.y ||
        truncf(scissor.width) != scissor.width ||
        truncf(scissor.height) != scissor.height || scissor.x < 0.0f ||
        scissor.y < 0.0f ||
        scissor.x + scissor.width > (float32_t)target_width ||
        scissor.y + scissor.height > (float32_t)target_height)
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
          "packet.ui.draw_list.batches.scissor_rect_px",
          "must contain integral bounds within the UI attachment");
    if (batch->mode >= VKR_UI_DRAW_MODE_COUNT ||
        !isfinite(batch->screen_px_range) ||
        !isfinite(batch->sdf_unit_range.x) ||
        !isfinite(batch->sdf_unit_range.y) ||
        !isfinite(batch->rect_extent_px.x) ||
        !isfinite(batch->rect_extent_px.y) ||
        !vkr_renderer_ui_vec4_finite(batch->corner_radius_px))
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
          "packet.ui.draw_list.batches", "contains non-finite root data");
    if ((batch->mode == VKR_UI_DRAW_MODE_MTSDF_TEXT ||
         batch->mode == VKR_UI_DRAW_MODE_BITMAP_TEXT) &&
        batch->texture.id == 0u)
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
          "packet.ui.draw_list.batches.texture",
          "text batches require an atlas texture");
    if (batch->texture.id != 0u && batch->texture.generation == VKR_INVALID_ID)
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
          "packet.ui.draw_list.batches.texture",
          "contains an invalid texture generation");
    if (batch->mode == VKR_UI_DRAW_MODE_MTSDF_TEXT &&
        (batch->screen_px_range <= 0.0f || batch->sdf_unit_range.x <= 0.0f ||
         batch->sdf_unit_range.y <= 0.0f))
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
          "packet.ui.draw_list.batches.sdf_unit_range",
          "MTSDF batches require positive range data");
    if (batch->mode == VKR_UI_DRAW_MODE_ROUNDED_RECT &&
        (batch->texture.id != 0u || batch->rect_extent_px.x <= 0.0f ||
         batch->rect_extent_px.y <= 0.0f || batch->corner_radius_px.x < 0.0f ||
         batch->corner_radius_px.y < 0.0f || batch->corner_radius_px.z < 0.0f ||
         batch->corner_radius_px.w < 0.0f))
      return vkr_renderer_validation_fail(
          out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
          "packet.ui.draw_list.batches",
          "rounded rectangles require untextured positive root geometry");
  }
  if (expected_first_index != list->index_count)
    return vkr_renderer_validation_fail(
        out_error, VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
        "packet.ui.draw_list.batches", "must cover the complete index stream");
  return VKR_RENDERER_ERROR_NONE;
}

VkrRendererError
vkr_frame_input_validate(const VkrFrameInput *packet,
                         VkrValidationError *out_validation_error) {
  if (!packet) {
    return vkr_renderer_validation_fail(out_validation_error,
                                        VKR_RENDERER_ERROR_INVALID_PARAMETER,
                                        "packet", "must not be null");
  }

#define VKR_REJECT_PACKET(CODE, FIELD, MESSAGE)                                \
  do {                                                                         \
    return vkr_renderer_validation_fail(out_validation_error, CODE, FIELD,     \
                                        MESSAGE);                              \
  } while (0)

  if (packet->version != VKR_FRAME_INPUT_VERSION)
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_INCOMPATIBLE_SIGNATURE,
                      "packet.version",
                      "does not match VKR_FRAME_INPUT_VERSION");

  /* Tonemapping multiplies by the manual value and the metering passes raise
     two to the compensation bias with no recovery branch, so both are proven
     here instead. */
  if (packet->globals.exposure_mode >= VKR_EXPOSURE_MODE_COUNT)
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.globals.exposure_mode",
                      "must be a supported VkrExposureMode");
  if (!isfinite(packet->globals.manual_exposure) ||
      packet->globals.manual_exposure <= 0.0f)
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.globals.manual_exposure",
                      "must be finite and greater than zero");
  if (!isfinite(packet->globals.exposure_compensation_ev))
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.globals.exposure_compensation_ev",
                      "must be finite");

  if (packet->globals.bloom_enabled > true_v)
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.globals.bloom_enabled", "must be zero or one");
  if (packet->globals.bloom_enabled &&
      (!isfinite(packet->globals.bloom_threshold) ||
       packet->globals.bloom_threshold < 0.0f))
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.globals.bloom_threshold",
                      "must be finite and non-negative when bloom is enabled");
  if (packet->globals.bloom_enabled && (!isfinite(packet->globals.bloom_knee) ||
                                        packet->globals.bloom_knee < 0.0f))
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.globals.bloom_knee",
                      "must be finite and non-negative when bloom is enabled");
  if (packet->globals.bloom_enabled &&
      (!isfinite(packet->globals.bloom_intensity) ||
       packet->globals.bloom_intensity < 0.0f))
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.globals.bloom_intensity",
                      "must be finite and non-negative when bloom is enabled");

  if (packet->globals.gtao_enabled > true_v)
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.globals.gtao_enabled", "must be zero or one");
  if (packet->globals.gtao_enabled &&
      (!isfinite(packet->globals.gtao_radius) ||
       packet->globals.gtao_radius < VKR_GTAO_RADIUS_MIN ||
       packet->globals.gtao_radius > VKR_GTAO_RADIUS_MAX))
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.globals.gtao_radius",
                      "must be finite and within the supported GTAO range");
  if (packet->globals.gtao_enabled && (!isfinite(packet->globals.gtao_power) ||
                                       packet->globals.gtao_power <= 0.0f))
    VKR_REJECT_PACKET(
        VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, "packet.globals.gtao_power",
        "must be finite and greater than zero when GTAO is enabled");

  const VkrWorldPassPayload *world = packet->world;
  if (world) {
    VkrRendererError error = vkr_renderer_validate_packet_array(
        world->gpu_candidates, world->gpu_candidate_count,
        VKR_GPU_DRAW_CANDIDATE_CAPACITY, "packet.world.gpu_candidates",
        "packet.world.gpu_candidate_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_packet_array(
        world->transmission_gpu_candidates,
        world->transmission_gpu_candidate_count,
        VKR_GPU_DRAW_CANDIDATE_CAPACITY,
        "packet.world.transmission_gpu_candidates",
        "packet.world.transmission_gpu_candidate_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    if (world->static_candidate_count > world->gpu_candidate_count)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.static_candidate_count",
                        "cannot exceed the source candidate count");
    if (world->gpu_candidate_count > 0u && world->static_generation == 0u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.static_generation", "must be nonzero");
    if (world->gpu_candidate_count > 0u && world->dynamic_generation == 0u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.dynamic_generation", "must be nonzero");
    if (world->gpu_candidate_count > 0u && world->publication_generation == 0u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.publication_generation",
                        "must be nonzero");
    if (world->gpu_camera_opaque_candidate_count > world->gpu_candidate_count)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.gpu_camera_opaque_candidate_count",
                        "cannot exceed the source candidate count");
    if (world->gpu_shadow_candidate_count > world->gpu_candidate_count)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.world.gpu_shadow_candidate_count",
                        "cannot exceed the source candidate count");
    error = vkr_renderer_validate_packet_array(
        world->instances, world->instance_count,
        VKR_INSTANCE_BUFFER_MAX_INSTANCES, "packet.world.instances",
        "packet.world.instance_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_packet_array(
        world->transparent_draws, world->transparent_draw_count,
        VKR_INSTANCE_BUFFER_MAX_INSTANCES, "packet.world.transparent_draws",
        "packet.world.transparent_draw_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_draw_ranges(
        world->transparent_draws, world->transparent_draw_count,
        world->instance_count, "packet.world.transparent_draws",
        out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
    error = vkr_renderer_validate_text_draws(
        world->text_draws, world->text_draw_count, "packet.world.text_draws",
        "packet.world.text_draw_count", out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
  }
  const VkrShadowPassPayload *shadow = packet->shadow;
  if (shadow) {
    if (shadow->cascade_count == 0u ||
        shadow->cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.cascade_count",
                        "must be within the supported cascade range");
    const uint32_t cascade_mask = (UINT32_C(1) << shadow->cascade_count) - 1u;
    if ((shadow->cascade_render_mask & ~cascade_mask) != 0u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.cascade_render_mask",
                        "contains a bit outside cascade_count");
    /* The receiver hot path indexes the Poisson table and divides by the
       per-cascade depth span with no recovery branch, so every value it trusts
       is proven here instead. */
    const VkrShadowReceiverPacketData *receiver = &shadow->receiver;
    if (!vkr_shadow_pcf_sample_count_supported(receiver->pcf_sample_count))
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.pcf_sample_count",
                        "must be one of the supported tap counts");
    if (receiver->pcf_uniform_early_out > 1u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.pcf_uniform_early_out",
                        "must be zero or one");
    if (!isfinite(receiver->pcf_radius_texels) ||
        receiver->pcf_radius_texels < 0.0f)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.pcf_radius_texels",
                        "must be finite and non-negative");
    if (!isfinite(receiver->receiver_bias_texels) ||
        receiver->receiver_bias_texels < 0.0f ||
        !isfinite(receiver->slope_bias_texels) ||
        receiver->slope_bias_texels < 0.0f ||
        !isfinite(receiver->normal_offset_texels) ||
        receiver->normal_offset_texels < 0.0f)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver",
                        "bias texel counts must be finite and non-negative");
    if (!isfinite(receiver->cascade_blend_fraction) ||
        receiver->cascade_blend_fraction < 0.0f ||
        receiver->cascade_blend_fraction > 0.5f)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.cascade_blend_fraction",
                        "must be finite and within [0, 0.5]");
    if (!isfinite(receiver->fade_start) || receiver->fade_start < 0.0f)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.fade_start",
                        "must be finite and non-negative");
    if (!isfinite(receiver->fade_end) ||
        receiver->fade_end < receiver->fade_start)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.fade_end",
                        "must be finite and not precede fade_start");
    for (uint32_t i = 0; i < shadow->cascade_count; ++i) {
      const Vec4 slice = shadow->cascades[i].split_near_far_texel_depth;
      if (!isfinite(slice.x) || !isfinite(slice.y) || slice.y <= slice.x)
        VKR_REJECT_PACKET(
            VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
            "packet.shadow.cascades.split_near_far_texel_depth",
            "cascade slice bounds must be finite and strictly ordered");
      /* Both must be strictly positive: the receiver divides the light-space
         origin by the texel size to build its rotation cell, and divides the
         texel-denominated bias by the depth span. */
      if (!isfinite(slice.z) || slice.z <= 0.0f || !isfinite(slice.w) ||
          slice.w <= 0.0f)
        VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                          "packet.shadow.cascades.split_near_far_texel_depth",
                          "texel size and depth span must be positive");
      const Vec4 origin = shadow->cascades[i].origin_inv_size_pad;
      if (!isfinite(origin.x) || !isfinite(origin.y) || !isfinite(origin.z) ||
          origin.z <= 0.0f)
        VKR_REJECT_PACKET(
            VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
            "packet.shadow.cascades.origin_inv_size_pad",
            "light-space origin must be finite and inverse map size positive");
    }
    const float32_t final_split = shadow->cascades[shadow->cascade_count - 1u]
                                      .split_near_far_texel_depth.y;
    if (receiver->fade_end > final_split)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.shadow.receiver.fade_end",
                        "must not extend beyond the final cascade split");
    const VkrShadowConfigOverride *bias = shadow->config_override;
    if (bias) {
      if (!isfinite(bias->depth_bias_constant) ||
          bias->depth_bias_constant < 0.0f)
        VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                          "packet.shadow.config_override.depth_bias_constant",
                          "must be finite and non-negative");
      if (!isfinite(bias->depth_bias_slope) || bias->depth_bias_slope < 0.0f)
        VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                          "packet.shadow.config_override.depth_bias_slope",
                          "must be finite and non-negative");
      if (!isfinite(bias->depth_bias_clamp) || bias->depth_bias_clamp < 0.0f)
        VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                          "packet.shadow.config_override.depth_bias_clamp",
                          "must be finite and non-negative");
    }
  }

  const VkrUiPassPayload *ui = packet->ui;
  if (ui) {
    VkrRendererError error = vkr_renderer_validate_ui_draw_list(
        &ui->draw_list, packet->frame.window_width, packet->frame.window_height,
        out_validation_error);
    if (error != VKR_RENDERER_ERROR_NONE)
      return error;
  }

  const VkrEditorPassPayload *editor = packet->editor;
  if (packet->frame.editor_enabled > true_v)
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.frame.editor_enabled", "must be zero or one");
  if ((editor != NULL) != (packet->frame.editor_enabled != false_v))
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT, "packet.editor",
                      "presence must match packet.frame.editor_enabled");
  if (editor) {
    const Vec4 rect = editor->image_rect_px;
    if (!isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.z) ||
        !isfinite(rect.w) || rect.x < 0.0f || rect.y < 0.0f || rect.z <= 0.0f ||
        rect.w <= 0.0f || truncf(rect.x) != rect.x ||
        truncf(rect.y) != rect.y || truncf(rect.z) != rect.z ||
        truncf(rect.w) != rect.w)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.editor.image_rect_px",
                        "must contain finite integral pixel bounds");
    if (packet->frame.window_width == 0u || packet->frame.window_height == 0u ||
        rect.x + rect.z > (float32_t)packet->frame.window_width ||
        rect.y + rect.w > (float32_t)packet->frame.window_height)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.editor.image_rect_px",
                        "must fit within the packet window extent");
    if (packet->frame.viewport_width == 0u ||
        packet->frame.viewport_height == 0u)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.frame.viewport_width",
                        "editor frames require an explicit viewport extent");
  }

  const VkrFrameLighting *lighting = packet->lighting;
  if (lighting) {
    if (lighting->point_light_count > VKR_MAX_SCENE_POINT_LIGHTS)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.lighting.point_light_count",
                        "exceeds the fixed scene-light capacity");
    if (lighting->point_light_count > 0u &&
        (!lighting->point_lights || !lighting->point_light_grid))
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.lighting.point_lights",
                        "lights and their lookup grid must both be present");
    if (lighting->point_light_grid &&
        lighting->point_light_grid->cell_count > VKR_POINT_LIGHT_GRID_MAX_CELLS)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.lighting.point_light_grid.cell_count",
                        "exceeds the fixed light-grid capacity");
    if (lighting->ibl_probe_count > VKR_FRAME_IBL_PROBE_MAX)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.lighting.ibl_probe_count",
                        "exceeds the fixed frame-probe capacity");
    if (lighting->ibl_probe_count > 0u && !lighting->ibl_probes)
      VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                        "packet.lighting.ibl_probes",
                        "must contain every declared probe");
    /* Bound-checked here, at the cold boundary, so hot-path lowering can index
       the coefficient pool without a guard (ADR-038). */
    for (uint32_t i = 0u; i < lighting->ibl_probe_count; ++i) {
      if (lighting->ibl_probes[i].sh_slot >= VKR_SH_SLOT_CAPACITY)
        VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                          "packet.lighting.ibl_probes[].sh_slot",
                          "exceeds the coefficient pool capacity");
    }
  }

  if (packet->debug && packet->debug->shadow_debug_mode > 3u)
    VKR_REJECT_PACKET(VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
                      "packet.debug.shadow_debug_mode",
                      "must be a supported debug view");

#undef VKR_REJECT_PACKET
  if (out_validation_error)
    *out_validation_error = (VkrValidationError){0};
  return VKR_RENDERER_ERROR_NONE;
}
