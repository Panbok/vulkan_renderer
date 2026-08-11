#include "renderer/systems/vkr_editor_viewport.h"

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vkr_math.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_resource_system.h"

static Vec4 vkr_editor_viewport_compute_panel_rect(uint32_t width,
                                                   uint32_t height) {
  const uint32_t top_bar =
      (uint32_t)vkr_max_f32(32.0f, vkr_round_f32((float32_t)height * 0.06f));
  const uint32_t bottom_panel =
      (uint32_t)vkr_max_f32(180.0f, vkr_round_f32((float32_t)height * 0.24f));
  const uint32_t left_panel =
      (uint32_t)vkr_max_f32(220.0f, vkr_round_f32((float32_t)width * 0.18f));
  const uint32_t right_panel =
      (uint32_t)vkr_max_f32(280.0f, vkr_round_f32((float32_t)width * 0.22f));
  const uint32_t gutter = 8u;

  const uint32_t x = left_panel + gutter;
  const uint32_t y = top_bar + gutter;

  const uint32_t used_w = left_panel + right_panel + gutter * 2u;
  const uint32_t used_h = top_bar + bottom_panel + gutter * 2u;

  const uint32_t w = (width > used_w) ? (width - used_w) : 1u;
  const uint32_t h = (height > used_h) ? (height - used_h) : 1u;

  return (Vec4){(float32_t)x, (float32_t)y, (float32_t)w, (float32_t)h};
}

static Mat4 vkr_editor_viewport_build_model(const VkrViewportMapping *mapping,
                                            Vec2 plane_size) {
  if (!mapping || plane_size.x <= 0.0f || plane_size.y <= 0.0f) {
    return mat4_identity();
  }

  Vec4 rect = mapping->image_rect_px;
  if (rect.z <= 0.0f || rect.w <= 0.0f) {
    rect = mapping->panel_rect_px;
  }
  if (rect.z <= 0.0f || rect.w <= 0.0f) {
    return mat4_identity();
  }

  float32_t scale_x = rect.z / plane_size.x;
  float32_t scale_y = rect.w / plane_size.y;

  VkrTransform transform = vkr_transform_from_position_scale_rotation(
      vec3_new(rect.x, rect.y, 0.0f), vec3_new(scale_x, scale_y, 1.0f),
      vkr_quat_identity());
  return vkr_transform_get_world(&transform);
}

static void vkr_editor_viewport_reset(VkrEditorViewportResources *resources) {
  MemZero(resources, sizeof(*resources));
  resources->mesh_index = VKR_INVALID_ID;
  resources->geometry = VKR_GEOMETRY_HANDLE_INVALID;
  resources->material = VKR_MATERIAL_HANDLE_INVALID;
  resources->plane_size = vec2_new(2.0f, 2.0f);
}

static void
vkr_editor_viewport_release_resources(RendererFrontend *rf,
                                      VkrEditorViewportResources *resources) {
  if (resources->mesh_index != VKR_INVALID_ID) {
    vkr_mesh_manager_remove(&rf->mesh_manager, resources->mesh_index);
    resources->mesh_index = VKR_INVALID_ID;
    resources->geometry = VKR_GEOMETRY_HANDLE_INVALID;
    resources->material = VKR_MATERIAL_HANDLE_INVALID;
  } else {
    if (resources->geometry.id != 0) {
      vkr_geometry_system_release(&rf->geometry_system, resources->geometry);
      resources->geometry = VKR_GEOMETRY_HANDLE_INVALID;
    }
    if (resources->material.id != 0) {
      vkr_material_system_release(&rf->material_system, resources->material);
      resources->material = VKR_MATERIAL_HANDLE_INVALID;
    }
  }

  resources->initialized = false_v;
}

static bool8_t vkr_editor_viewport_create_retained_resources(
    RendererFrontend *rf, VkrEditorViewportResources *resources) {
  VkrResourceHandleInfo material_info = {0};
  VkrRendererError material_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_sync(
          VKR_RESOURCE_TYPE_MATERIAL,
          string8_lit("assets/materials/default.viewport_display.mt"),
          &rf->allocator, &material_info, &material_err)) {
    String8 err = vkr_renderer_get_error_string(material_err);
    log_error("Editor viewport material load failed: %s", string8_cstr(&err));
    return false_v;
  }
  resources->material = material_info.as.material;

  VkrVertex2d verts[4] = {0};
  verts[0].position = vec2_new(0.0f, 0.0f);
  verts[0].texcoord = vec2_new(0.0f, 1.0f);
  verts[1].position =
      vec2_new(resources->plane_size.x, resources->plane_size.y);
  verts[1].texcoord = vec2_new(1.0f, 0.0f);
  verts[2].position = vec2_new(0.0f, resources->plane_size.y);
  verts[2].texcoord = vec2_new(0.0f, 0.0f);
  verts[3].position = vec2_new(resources->plane_size.x, 0.0f);
  verts[3].texcoord = vec2_new(1.0f, 1.0f);

  uint32_t indices[6] = {2, 1, 0, 3, 0, 1};
  VkrGeometryConfig geo_cfg = {0};
  geo_cfg.vertex_size = sizeof(VkrVertex2d);
  geo_cfg.vertex_count = ArrayCount(verts);
  geo_cfg.vertices = verts;
  geo_cfg.index_size = sizeof(indices[0]);
  geo_cfg.index_count = ArrayCount(indices);
  geo_cfg.indices = indices;
  geo_cfg.center = vec3_zero();
  geo_cfg.min_extents =
      vec3_new(-resources->plane_size.x, -resources->plane_size.y, 0.0f);
  geo_cfg.max_extents =
      vec3_new(resources->plane_size.x, resources->plane_size.y, 0.0f);
  string_format(geo_cfg.name, sizeof(geo_cfg.name), "Editor Viewport Plane");

  VkrRendererError geo_err = VKR_RENDERER_ERROR_NONE;
  resources->geometry = vkr_geometry_system_create(&rf->geometry_system,
                                                   &geo_cfg, true_v, &geo_err);
  if (resources->geometry.id == 0) {
    String8 err = vkr_renderer_get_error_string(geo_err);
    log_error("Editor viewport geometry create failed: %s", string8_cstr(&err));
    return false_v;
  }

  VkrSubMeshDesc submesh = {
      .geometry = resources->geometry,
      .material = resources->material,
      .shader_override = string8_lit("shader.default.viewport_display"),
      .pipeline_domain = VKR_PIPELINE_DOMAIN_UI,
      .owns_geometry = true_v,
      .owns_material = true_v,
  };
  VkrMeshDesc mesh_desc = {
      .transform = vkr_transform_identity(),
      .submeshes = &submesh,
      .submesh_count = 1,
  };
  VkrRendererError mesh_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_mesh_manager_add(&rf->mesh_manager, &mesh_desc,
                            &resources->mesh_index, &mesh_err)) {
    String8 err = vkr_renderer_get_error_string(mesh_err);
    log_error("Editor viewport mesh create failed: %s", string8_cstr(&err));
    return false_v;
  }

  vkr_mesh_manager_update_model(&rf->mesh_manager, resources->mesh_index);
  /* The editor pass submits this plane explicitly. Keep it out of the scene

   * mesh enumeration so merely initializing editor resources cannot change

   * world work when editor mode is disabled. */
  (void)vkr_mesh_manager_set_visible(&rf->mesh_manager, resources->mesh_index,
                                     false_v);
  return true_v;
}

bool8_t vkr_editor_viewport_init(RendererFrontend *rf,
                                 VkrEditorViewportResources *resources) {
  if (!rf || !resources) {
    return false_v;
  }

  vkr_editor_viewport_reset(resources);

  if (!vkr_editor_viewport_create_retained_resources(rf, resources)) {
    vkr_editor_viewport_release_resources(rf, resources);
    return false_v;
  }

  resources->initialized = true_v;
  return true_v;
}

void vkr_editor_viewport_shutdown(RendererFrontend *rf,
                                  VkrEditorViewportResources *resources) {
  if (!rf || !resources || !resources->initialized) {
    return;
  }

  vkr_editor_viewport_release_resources(rf, resources);
}

bool8_t vkr_editor_viewport_compute_mapping(uint32_t window_width,
                                            uint32_t window_height,
                                            VkrViewportFitMode fit_mode,
                                            float32_t render_scale,
                                            VkrViewportMapping *out_mapping) {
  if (!out_mapping || window_width == 0 || window_height == 0) {
    return false_v;
  }

  float32_t clamped_scale = vkr_clamp_f32(render_scale, 0.25f, 2.0f);
  Vec4 panel =
      vkr_editor_viewport_compute_panel_rect(window_width, window_height);

  const float32_t panel_w = vkr_max_f32(1.0f, panel.z);
  const float32_t panel_h = vkr_max_f32(1.0f, panel.w);

  uint32_t target_w =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(panel_w * clamped_scale));
  uint32_t target_h =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(panel_h * clamped_scale));

  Vec4 image = panel;

  if (fit_mode == VKR_VIEWPORT_FIT_CONTAIN) {
    const float32_t target_aspect = (float32_t)target_w / (float32_t)target_h;
    const float32_t panel_aspect = panel_w / panel_h;

    if (target_aspect > panel_aspect) {
      const float32_t scale = panel_w / (float32_t)target_w;
      const float32_t img_h = vkr_max_f32(1.0f, (float32_t)target_h * scale);
      const float32_t y = panel.y + (panel_h - img_h) * 0.5f;
      image = (Vec4){panel.x, y, panel_w, img_h};
    } else if (target_aspect < panel_aspect) {
      const float32_t scale = panel_h / (float32_t)target_h;
      const float32_t img_w = vkr_max_f32(1.0f, (float32_t)target_w * scale);
      const float32_t x = panel.x + (panel_w - img_w) * 0.5f;
      image = (Vec4){x, panel.y, img_w, panel_h};
    } else {
      image = panel;
    }

    image.x = vkr_round_f32(image.x);
    image.y = vkr_round_f32(image.y);
    image.z = vkr_max_f32(1.0f, vkr_round_f32(image.z));
    image.w = vkr_max_f32(1.0f, vkr_round_f32(image.w));
  }

  *out_mapping = (VkrViewportMapping){
      .panel_rect_px = panel,
      .image_rect_px = image,
      .target_width = target_w,
      .target_height = target_h,
      .fit_mode = fit_mode,
  };
  return true_v;
}

bool8_t vkr_editor_viewport_build_payload(
    const VkrEditorViewportResources *resources,
    const VkrViewportMapping *mapping, VkrDrawItem *out_draw,
    VkrInstanceDataGPU *out_instance, VkrEditorPassPayload *out_payload) {
  if (!resources || !resources->initialized || !mapping || !out_draw ||
      !out_instance || !out_payload) {
    return false_v;
  }

  if (resources->mesh_index == VKR_INVALID_ID) {
    return false_v;
  }
  if (mapping->target_width == 0 || mapping->target_height == 0) {
    return false_v;
  }

  Mat4 model = vkr_editor_viewport_build_model(mapping, resources->plane_size);
  out_instance[0] = (VkrInstanceDataGPU){
      .model = model,
      .object_id = 0,
  };

  VkrMeshHandle mesh_handle = {
      .id = resources->mesh_index + 1u,
      .generation = 0,
  };

  out_draw[0] = (VkrDrawItem){
      .mesh = mesh_handle,
      .geometry = resources->geometry,
      .submesh_index = 0,
      .material = resources->material,
      .instance_count = 1,
      .first_instance = 0,
      .sort_key = 0,
  };

  *out_payload = (VkrEditorPassPayload){
      .draws = out_draw,
      .draw_count = 1,
      .instances = out_instance,
      .instance_count = 1,
  };
  return true_v;
}
