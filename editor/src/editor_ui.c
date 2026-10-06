#include "editor_agent.h"
#include "editor_internal.h"
#include "editor_io.h"
#include "editor_physics.h"
#include "editor_projects.h"
#include "editor_scene_panels.h"
#include "editor_terrain.h"

#include "renderer/systems/vkr_editor_viewport.h"

/* Floating surfaces: menus, popups and floating windows. */
VkrUiStyle vkr_editor_glass_style(void) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiStyle style = vkr_ui_style_default();
  style.padding_pt = (VkrUiEdges){theme->space_md, theme->space_md,
                                  theme->space_md, theme->space_md};
  style.border_pt = (VkrUiEdges){1.0f, 1.0f, 1.0f, 1.0f};
  style.corner_radius_pt = (Vec4){theme->radius_large, theme->radius_large,
                                  theme->radius_large, theme->radius_large};
  style.gap_pt = theme->space_sm;
  style.font_size_pt = theme->font_body;
  style.background_color = theme->popup;
  style.border_color = theme->border;
  style.text_color = theme->text;
  style.shadow_color = theme->shadow;
  style.shadow_offset_pt = (Vec2){0.0f, 6.0f};
  style.shadow_blur_pt = 18.0f;
  return style;
}

/* Translucent chips drawn over the Scene image. */
VkrUiStyle vkr_editor_overlay_style(void) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiStyle style = vkr_editor_glass_style();
  style.padding_pt = (VkrUiEdges){3.0f, 3.0f, 3.0f, 3.0f};
  style.gap_pt = 2.0f;
  style.corner_radius_pt = (Vec4){theme->radius + 2.0f, theme->radius + 2.0f,
                                  theme->radius + 2.0f, theme->radius + 2.0f};
  style.background_color = theme->overlay;
  style.border_color = vkr_ui_color_alpha(theme->border_strong, 0.55f);
  style.shadow_offset_pt = (Vec2){0.0f, 2.0f};
  style.shadow_blur_pt = 10.0f;
  return style;
}

VkrUiWidgetConfig vkr_editor_text_config(float32_t size_pt, Vec4 color) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement.justify = VKR_UI_ALIGN_START;
  config.placement.align = VKR_UI_ALIGN_START;
  config.style.font_size_pt = size_pt;
  config.style.text_color = color;
  config.text.font_size = size_pt;
  return config;
}

void vkr_editor_field_style(VkrUiWidgetConfig *config) {
  const VkrUiTheme *theme = vkr_ui_theme();
  config->style.background_color =
      config->read_only ? theme->panel : theme->field;
  config->style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  config->style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  config->style.border_color = theme->border;
  if (config->style.padding_pt.left < 6.0f)
    config->style.padding_pt.left = config->style.padding_pt.right = 6.0f;
  config->style.text_color =
      config->read_only ? theme->text_secondary : theme->text;
}

void vkr_editor_action_style(VkrUiWidgetConfig *config, VkrFontHandle heading) {
  const VkrUiTheme *theme = vkr_ui_theme();
  config->style.background_color = theme->raised;
  config->style.hover_background_color = theme->raised_hover;
  config->style.active_background_color = theme->raised_active;
  config->style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  config->style.border_color = theme->border;
  config->style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  config->style.text_color = theme->text;
  config->text.font = heading;
}

void vkr_editor_primary_style(VkrUiWidgetConfig *config,
                              VkrFontHandle heading) {
  const VkrUiTheme *theme = vkr_ui_theme();
  vkr_editor_action_style(config, heading);
  config->style.background_color = theme->accent;
  config->style.hover_background_color = theme->accent_hover;
  config->style.active_background_color = theme->accent_active;
  config->style.border_color = theme->accent;
  config->style.text_color = theme->text_on_accent;
}

void vkr_editor_ghost_style(VkrUiWidgetConfig *config) {
  const VkrUiTheme *theme = vkr_ui_theme();
  config->style.background_color = (Vec4){0};
  config->style.hover_background_color =
      vkr_ui_color_alpha(theme->raised_hover, 1.0f);
  config->style.active_background_color = theme->raised_active;
  config->style.border_pt = (VkrUiEdges){0};
  config->style.corner_radius_pt =
      (Vec4){theme->radius, theme->radius, theme->radius, theme->radius};
  config->style.text_color = theme->text_secondary;
}

VkrUiWidgetConfig vkr_editor_icon_button_config(uint32_t column, uint32_t row,
                                                VkrUiIcon icon,
                                                String8 tooltip) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = VKR_UI_PLACEMENT_DEFAULT;
  config.placement.column = column;
  config.placement.row = row;
  config.placement.justify = VKR_UI_ALIGN_CENTER;
  config.placement.align = VKR_UI_ALIGN_CENTER;
  vkr_editor_ghost_style(&config);
  config.style.min_size_pt =
      (Vec2){theme->control_height, theme->control_height};
  config.style.max_size_pt = config.style.min_size_pt;
  config.style.padding_pt = (VkrUiEdges){4.0f, 4.0f, 4.0f, 4.0f};
  config.icon = icon;
  config.icon_size_pt = theme->icon_size;
  config.tooltip = tooltip;
  return config;
}

/* A toggled icon button keeps an accent-tinted fill while active. */
void vkr_editor_toggle_style(VkrUiWidgetConfig *config, bool8_t active) {
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!active)
    return;
  config->style.background_color = vkr_ui_color_alpha(theme->accent, 0.22f);
  config->style.hover_background_color =
      vkr_ui_color_alpha(theme->accent, 0.32f);
  config->icon_color = theme->accent_hover;
  config->style.text_color = theme->text;
}

/* Inline section heading: small caps-like label in secondary text. */
VkrUiWidgetConfig vkr_editor_section_label_config(VkrFontHandle heading) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig config =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  config.text.font = heading;
  config.text.letter_spacing = 0.4f;
  return config;
}

void vkr_editor_ui_init(VkrEditorUi *editor) {
  *editor = (VkrEditorUi){
      .menu = VKR_EDITOR_MENU_NONE,
      .title_inset_pt = 4.0f,
      .ui_scale = 1.0f,
      .labels_enabled = true_v,
      .labels_directional = true_v,
      .labels_spot = true_v,
      .labels_point = true_v,
      .labels_environment = true_v,
      .labels_markers = true_v,
      .labels_text = true_v,
      .labels_empty = true_v,
      .placement = {.target = VKR_EDITOR_SNAP_SURFACE, .magnet = true_v},
      .corridor_curved = true_v,
      .face_handle_hot = -1,
      .windows =
          {
              [VKR_EDITOR_WINDOW_PARTITION] = {.position_pt = {380.0f, 120.0f},
                                               .size_pt = {380.0f, 500.0f},
                                               .z_order = 8u,
                                               .visible = false_v},
              [VKR_EDITOR_WINDOW_TERRAIN] = {.position_pt = {360.0f, 140.0f},
                                             .size_pt = {380.0f, 440.0f},
                                             .z_order = 8u,
                                             .visible = false_v},
              [VKR_EDITOR_WINDOW_LEVEL] = {.position_pt = {340.0f, 120.0f},
                                           .size_pt = {560.0f, 360.0f},
                                           .z_order = 8u,
                                           .visible = false_v},
              [VKR_EDITOR_WINDOW_CHANGES] = {.position_pt = {300.0f, 90.0f},
                                             .size_pt = {560.0f, 380.0f},
                                             .z_order = 7u,
                                             .visible = false_v},
              [VKR_EDITOR_WINDOW_PHYSICS] = {.position_pt = {180.0f, 80.0f},
                                             .size_pt = {760.0f, 620.0f},
                                             .z_order = 6u,
                                             .visible = false_v},
              [VKR_EDITOR_WINDOW_GRAPHICS] =
                  {
                      .position_pt = {250.0f, 76.0f},
                      .size_pt = {560.0f, 540.0f},
                      .z_order = 4u,
                      .visible = false_v,
                  },
              [VKR_EDITOR_WINDOW_DRAWS] =
                  {
                      .position_pt = {235.0f, 340.0f},
                      .size_pt = {520.0f, 320.0f},
                      .z_order = 1u,
                      .visible = false_v,
                  },
              [VKR_EDITOR_WINDOW_MEMORY] =
                  {
                      .position_pt = {485.0f, 145.0f},
                      .size_pt = {300.0f, 390.0f},
                      .z_order = 2u,
                      .visible = false_v,
                  },
              [VKR_EDITOR_WINDOW_ANIMATION] =
                  {
                      .position_pt = {190.0f, 95.0f},
                      .size_pt = {880.0f, 660.0f},
                      .z_order = 5u,
                      .visible = false_v,
                  },
              [VKR_EDITOR_WINDOW_HELP] =
                  {
                      .position_pt = {280.0f, 105.0f},
                      .size_pt = {460.0f, 520.0f},
                      .z_order = 3u,
                      .visible = false_v,
                  },
              [VKR_EDITOR_WINDOW_CREATE] =
                  {
                      .position_pt = {360.0f, 150.0f},
                      .size_pt = {520.0f, 350.0f},
                      .z_order = 7u,
                      .visible = false_v,
                  },
              [VKR_EDITOR_WINDOW_BUILD] =
                  {
                      .position_pt = {300.0f, 70.0f},
                      .size_pt = {640.0f, 640.0f},
                      .z_order = 8u,
                      .visible = false_v,
                  },
              [VKR_EDITOR_WINDOW_SCRIPT] =
                  {
                      .position_pt = {240.0f, 80.0f},
                      .size_pt = {860.0f, 620.0f},
                      .z_order = 9u,
                      .visible = false_v,
                  },
          },
  };
  vkr_editor_workbench_init(&editor->workbenches);
}

static void vkr_editor_ui_build_camera(VkrUiSystem *ui, bool8_t scene_only,
                                       bool8_t scene_rendering_stopped,
                                       const VkrViewportMapping *mapping,
                                       const VkrSampleUiText *text) {
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  const float32_t width_pt = 288.0f;
  const float32_t height_pt = 98.0f;
  const float32_t inset_px = 8.0f * ui->content_scale;
  const float32_t top_inset_px =
      (scene_only ? 43.0f : 8.0f) * ui->content_scale;
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt =
          {
              .top =
                  (mapping->panel_rect_px.y + top_inset_px) / ui->content_scale,
              .left = Max(0.0f,
                          (mapping->panel_rect_px.x + mapping->panel_rect_px.z -
                           width_pt * ui->content_scale - inset_px) /
                              ui->content_scale),
          },
  };
  panel.columns = &one_track;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style = vkr_editor_overlay_style();
  panel.style.padding_pt = (VkrUiEdges){8, 10, 8, 10};
  panel.style.min_size_pt = (Vec2){width_pt, height_pt};
  panel.style.max_size_pt = panel.style.min_size_pt;
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.camera.performance"), &panel))
    return;

  VkrUiWidgetConfig title = vkr_editor_text_config(
      vkr_ui_theme()->font_caption, vkr_ui_theme()->accent_hover);
  title.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
  };
  vkr_ui_label(ui, string8_lit("title"),
               scene_rendering_stopped ? string8_lit("VIEWPORT / FROZEN")
                                       : string8_lit("VIEWPORT / LIVE"),
               &title);
  VkrUiWidgetConfig body = vkr_editor_text_config(vkr_ui_theme()->font_caption,
                                                  vkr_ui_theme()->text);
  body.placement = title.placement;
  body.placement.row = 1u;
  vkr_ui_label(ui, string8_lit("camera"), text->camera, &body);
  body.placement.row = 2u;
  body.style.text_color = vkr_ui_theme()->text_secondary;
  vkr_ui_label(ui, string8_lit("performance"), text->performance, &body);
  (void)vkr_ui_panel_end(ui);
}

VkrUiDockInputCapture vkr_editor_ui_build(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame) {
  VkrUiDockInputCapture dock_capture = {0};
  editor->title_inset_pt = Max(4.0f, vkr_window_title_bar_inset(frame->window));
  editor->ui_scale = frame->ui->user_scale;
  editor->reduce_motion = frame->ui->reduce_motion;
  vkr_editor_animation_update(editor, frame);
  vkr_editor_bakery_update(editor->bakery);
  vkr_editor_scripts_update(editor->scripts,
                            vkr_editor_bakery_service(editor->bakery), frame);
  vkr_editor_build_update(editor->build, editor, frame);
  /* A switch asked for during the last build applies before anything
     reads this build's dock or input. */
  vkr_editor_workbench_update(editor, frame);
  vkr_editor_selection_update(editor, frame);
  vkr_editor_commands_update(editor, frame);
  vkr_editor_cmd_update(editor, frame);
  vkr_editor_agent_update(editor->agent, editor, frame);
  vkr_editor_windows_register_input_layers(editor, frame->ui);
  vkr_editor_viewport_update(editor, frame);
  vkr_editor_io_update(editor, frame);
  vkr_editor_terrain_update(editor, frame);
  if (frame->mapping_valid) {
    if (!frame->scene_only) {
      dock_capture = vkr_ui_dock_update_input(
          frame->dock, frame->input,
          frame->mouse_captured || frame->ui->mouse_input_layer > 0u ||
              (frame->ui->active_id != VKR_UI_ID_NONE &&
               frame->dock->interaction.tab_leaf == VKR_UI_DOCK_NODE_NONE &&
               frame->dock->interaction.resize_split == VKR_UI_DOCK_NODE_NONE));
    }
  }

  (void)vkr_ui_input_layer_set(frame->ui, 0u);
  /* Files dropped from the OS (ADR-076): Content's folders claim the drop
     point while building, then the import window opens for that folder. */
  VkrWindowFileDrop *file_drop = NULL;
  if (editor->projects && editor->content && frame->window &&
      vkr_atomic_bool_load(&frame->window->file_drop_pending,
                           VKR_MEMORY_ORDER_ACQUIRE)) {
    file_drop =
        vkr_allocator_alloc(frame->ui->frame_allocator, sizeof(*file_drop),
                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (file_drop && vkr_window_take_file_drop(frame->window, file_drop)) {
      vkr_editor_content_arm_file_drop(
          editor->content,
          (Vec2){(float32_t)file_drop->x, (float32_t)file_drop->y});
    } else {
      file_drop = NULL;
    }
  }
  /* Content lists the loaded containers' objects (ADR-076). */
  if (editor->content) {
    const char *scene_ids[1 + VKR_SCENE_ADDITIVE_MAX] = {0};
    const String8 primary = vkr_editor_projects_scene_id(editor->projects);
    char primary_id[37] = {0};
    snprintf(primary_id, sizeof(primary_id), "%.*s", (int)primary.length,
             (const char *)primary.str);
    scene_ids[0] = primary_id;
    for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
      scene_ids[1 + i] = frame->additive[i]
                             ? vkr_editor_projects_added_id(
                                   editor->projects, frame->additive_names[i])
                             : "";
    }
    vkr_editor_content_sync_objects(editor->content, frame, scene_ids);
  }
  if (!frame->scene_only)
    vkr_editor_dock_build(editor, frame);
  if (file_drop) {
    char folder[VKR_EDITOR_FOLDER_PATH_CAPACITY];
    vkr_editor_content_file_drop_folder(editor->content, folder);
    (void)vkr_editor_projects_import_files(editor->projects, editor, file_drop,
                                           folder);
  }
  vkr_editor_projects_build_scene_progress(editor->projects, editor, frame);
  (void)vkr_ui_input_layer_set(frame->ui, 0u);
  const bool8_t preparing_scene =
      frame->scene_backdrop_blur && *frame->scene_backdrop_blur;
  vkr_editor_grid_build(editor, frame);
  if (!preparing_scene) {
    vkr_editor_physics_build(editor, frame);
    vkr_editor_labels_build(editor, frame);
  }
  vkr_editor_windows_build_navigation(editor, frame);
  vkr_editor_workbench_build_row(editor, frame);
  if (!preparing_scene) {
    vkr_editor_scene_overlays_build(editor, frame);
    vkr_editor_orientation_gizmo_build(editor, frame);
    vkr_editor_viewport_build(editor, frame);
  }
  if (!preparing_scene && frame->scene_only && frame->mapping.target_width > 0u)
    vkr_editor_ui_build_camera(frame->ui, frame->scene_only,
                               frame->scene_rendering_stopped, &frame->mapping,
                               &frame->text);
  vkr_editor_windows_build_floating(editor, frame->ui, frame->input, frame);
  vkr_editor_windows_build_menu(editor, frame->ui, frame);
  /* A right click on a Content item opens its menu; on empty space, the
     shown folder's menu. */
  /* A right-clicked text field takes the click before the panel under it. */
  VkrUiId text_field = VKR_UI_ID_NONE;
  Vec2 text_menu_px = {0};
  VkrUiTextFieldState text_state = {0};
  const bool8_t text_menu = vkr_ui_text_context_take(
      frame->ui, &text_field, &text_menu_px, &text_state);
  if (text_menu) {
    const float32_t scale = frame->ui->content_scale;
    vkr_editor_context_open(
        editor, VKR_EDITOR_CONTEXT_TEXT,
        (Vec2){text_menu_px.x / scale, text_menu_px.y / scale});
    editor->context_text_field = text_field;
    editor->context_text_state = text_state;
  }
  Vec2 content_menu_pt;
  bool8_t content_menu_folder = false_v;
  if (editor->content &&
      vkr_editor_content_take_menu(editor->content, &content_menu_pt,
                                   &content_menu_folder) &&
      !text_menu) {
    vkr_editor_context_open(editor,
                            content_menu_folder
                                ? VKR_EDITOR_CONTEXT_CONTENT_FOLDER
                                : VKR_EDITOR_CONTEXT_CONTENT,
                            content_menu_pt);
    editor->context_container = vkr_editor_create_container(frame);
    editor->context_entity = vkr_editor_content_menu_entity(editor->content);
  }
  /* A right click in the Scene opens the menu of the object it picked, or
     the creation menu over empty space. */
  if (frame->context_requested &&
      frame->context_purpose == VKR_SAMPLE_PICK_SCRIPT_DROP) {
    vkr_editor_finish_script_drop(editor, frame);
  } else if (frame->context_requested &&
             frame->context_purpose == VKR_SAMPLE_PICK_SELECT_TOGGLE) {
    vkr_editor_selection_toggle(editor, frame, frame->context_entity);
  } else if (frame->context_requested && !text_menu) {
    const float32_t scale = frame->ui->content_scale;
    const Vec2 point = {frame->context_position_px.x / scale,
                        frame->context_position_px.y / scale};
    const VkrScene *scene =
        vkr_editor_entity_scene(frame, frame->context_entity);
    if (scene && vkr_scene_entity_alive(scene, frame->context_entity)) {
      vkr_editor_context_open(editor, VKR_EDITOR_CONTEXT_ENTITY, point);
      editor->context_entity = frame->context_entity;
    } else {
      vkr_editor_context_open(editor, VKR_EDITOR_CONTEXT_CREATE, point);
      editor->context_container = vkr_editor_create_container(frame);
      editor->context_at_pixel = true_v;
      editor->context_pixel = frame->context_position_px;
    }
  }
  vkr_editor_context_menu_build(editor, frame);
  vkr_editor_color_picker_build(editor, frame);
  /* A dragged Content item draws above every panel and drops on the Scene. */
  vkr_editor_content_set_drop_target(
      editor->content, frame->mapping_valid
                           ? (VkrUiRect){frame->mapping.panel_rect_px.x,
                                         frame->mapping.panel_rect_px.y,
                                         frame->mapping.panel_rect_px.z,
                                         frame->mapping.panel_rect_px.w}
                           : (VkrUiRect){0});
  vkr_editor_content_build_drag(editor->content, frame->ui);
  /* Loaded objects act here in any mode. Without a project only object
     types act too; Projects owns the rest. */
  VkrEditorContentAction content_action;
  if (editor->content &&
      vkr_editor_content_take_object_action(editor->content, &content_action)) {
    vkr_editor_apply_content_object(frame, &content_action);
    /* Double-clicking a scripted object opens its script. */
    if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_SELECT_ENTITY &&
        content_action.activated)
      (void)vkr_editor_open_entity_script(editor, frame, content_action.entity);
  }
  if (!frame->world && editor->content &&
      vkr_editor_content_take_action(editor->content, &content_action)) {
    if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_CREATE_OBJECT) {
      if (vkr_editor_request_create(
              editor, frame, content_action.object,
              vkr_editor_create_container(frame),
              content_action.dropped ? &content_action.drop_px : NULL)) {
        vkr_editor_content_reveal_created(editor->content,
                                          frame->selected_entity);
      }
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_OPEN_SCRIPT) {
      (void)vkr_editor_code_open(editor->code, editor, content_action.source);
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_NEW_SCRIPT) {
      vkr_editor_code_new_script(editor->code, editor);
    } else if (content_action.kind == VKR_EDITOR_CONTENT_ACTION_DROP_SCRIPT) {
      vkr_editor_drop_script(editor, frame, content_action.name,
                             content_action.drop_px);
    }
  }
  vkr_editor_build_status_build(editor->build, editor, frame);
  vkr_editor_toasts_build(editor, frame);
  vkr_editor_cmd_suggestions_build(editor, frame);
  if (frame->scene_keyboard_focus) {
    VkrUiSystem *ui = frame->ui;
    if (!frame->mapping_valid || frame->scene_rendering_stopped ||
        editor->cmd_active ||
        (ui->keyboard_layer_claimed && ui->keyboard_input_layer != 0u))
      *frame->scene_keyboard_focus = false_v;
    if (ui->mouse_pressed && !frame->mouse_captured) {
      int32_t x = 0, y = 0;
      input_get_button_press_position(frame->input, BUTTON_LEFT, &x, &y);
      Vec4 rect = frame->mapping.panel_rect_px;
      if (frame->scene_only) {
        const float32_t top =
            VKR_EDITOR_NAVIGATION_HEIGHT_PT * ui->content_scale;
        rect.y += top;
        rect.w = Max(0.0f, rect.w - top);
      }
      const bool8_t scene_click =
          frame->mapping_valid && !frame->scene_rendering_stopped &&
          (float32_t)x >= rect.x && (float32_t)x < rect.x + rect.z &&
          (float32_t)y >= rect.y && (float32_t)y < rect.y + rect.w &&
          !ui->capture.mouse && !dock_capture.mouse &&
          ui->mouse_input_layer == 0u;
      *frame->scene_keyboard_focus = scene_click;
      if (scene_click) {
        ui->focused_id = VKR_UI_ID_NONE;
        ui->focused_is_text = false_v;
        (void)vkr_ui_keyboard_layer_set(ui, 0u);
      }
    }
    vkr_ui_keyboard_navigation_enabled(ui, !*frame->scene_keyboard_focus);
  }
  /* Tab completes in the Cmd bar instead of moving focus. */
  if (editor->cmd_active)
    vkr_ui_keyboard_navigation_enabled(frame->ui, false_v);
  /* The top bar doubles as the native title bar: its empty space drags the
   * window. Controls, menus and modal overlays keep their clicks. */
  {
    VkrUiSystem *ui = frame->ui;
    const bool8_t allowed = ui->hot_id == VKR_UI_ID_NONE &&
                            editor->menu == VKR_EDITOR_MENU_NONE &&
                            ui->mouse_input_layer == 0u;
    vkr_window_set_title_drag_region(
        frame->window, 0, 0, (int32_t)ui->target_width,
        (int32_t)(VKR_EDITOR_NAVIGATION_HEIGHT_PT * ui->content_scale),
        allowed);
  }
  return dock_capture;
}

bool8_t vkr_editor_search_field(VkrUiSystem *ui, String8 id,
                                VkrUiTextEditBuffer *buffer,
                                VkrUiPlacement placement, String8 placeholder,
                                String8 tooltip, VkrFontHandle font) {
  const VkrUiTheme *theme = vkr_ui_theme();
  /* The field fills its cell between the placement's margins. A stretched
   * (default) row centers it vertically; an explicit alignment such as START
   * with a top margin places it exactly. The hint and clear button share the
   * same box. */
  const VkrUiAlign align = placement.align == VKR_UI_ALIGN_STRETCH
                               ? VKR_UI_ALIGN_CENTER
                               : placement.align;
  VkrUiWidgetConfig field = vkr_ui_widget_config_default();
  field.placement = placement;
  field.placement.justify = VKR_UI_ALIGN_STRETCH;
  field.placement.align = align;
  field.style.font_size_pt = theme->font_body;
  field.style.padding_pt = (VkrUiEdges){4.0f, 24.0f, 4.0f, 25.0f};
  field.style.min_size_pt.y = theme->control_height;
  field.tooltip = tooltip;
  field.fill = true_v;
  field.text.font = font;
  vkr_editor_field_style(&field);
  field.style.padding_pt.left = 25.0f;
  field.style.padding_pt.right = 24.0f;
  if (!vkr_ui_push_id_label(ui, id))
    return false_v;
  bool8_t changed = vkr_ui_text_field(ui, string8_lit("field"), buffer, &field);
  const VkrUiId field_id =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("field"));
  VkrUiWidgetConfig hint =
      vkr_editor_text_config(theme->font_body, theme->text_disabled);
  hint.placement = placement;
  hint.placement.justify = VKR_UI_ALIGN_START;
  hint.placement.align = align;
  hint.placement.margin_pt.left += 7.0f;
  hint.style.min_size_pt.y = hint.style.max_size_pt.y = theme->control_height;
  hint.icon = VKR_UI_ICON_SEARCH;
  hint.icon_size_pt = 13.0f;
  hint.icon_color =
      ui->focused_id == field_id ? theme->accent_hover : theme->text_secondary;
  const bool8_t empty = buffer->length == 0u;
  vkr_ui_label(ui, string8_lit("hint"),
               empty && ui->focused_id != field_id ? placeholder : (String8){0},
               &hint);
  if (!empty) {
    const float32_t clear_size = 18.0f;
    VkrUiWidgetConfig clear = vkr_editor_icon_button_config(
        placement.column, placement.row, VKR_UI_ICON_CLOSE,
        string8_lit("Clear search"));
    clear.placement = placement;
    clear.placement.justify = VKR_UI_ALIGN_END;
    clear.placement.align = align;
    clear.placement.margin_pt.right += 3.0f;
    if (align == VKR_UI_ALIGN_START)
      clear.placement.margin_pt.top +=
          (theme->control_height - clear_size) * 0.5f;
    clear.style.min_size_pt = clear.style.max_size_pt =
        (Vec2){clear_size, clear_size};
    clear.style.padding_pt = (VkrUiEdges){3.0f, 3.0f, 3.0f, 3.0f};
    clear.icon_size_pt = 11.0f;
    if (vkr_ui_button(ui, string8_lit("clear"), (String8){0}, &clear)) {
      buffer->length = 0u;
      if (buffer->capacity)
        buffer->data[0] = 0u;
      changed = true_v;
    }
  }
  (void)vkr_ui_pop_id(ui);
  return changed;
}

/* Additive slot owning an entity, or VKR_SCENE_ADDITIVE_MAX. */
static uint32_t editor_additive_slot(const VkrSampleUiFrame *frame,
                                     VkrEntityId entity) {
  const uint32_t world = entity.parts.world;
  if (!entity.u64 || world == 0u || world > VKR_SCENE_ADDITIVE_MAX ||
      !frame->additive[world - 1u]) {
    return VKR_SCENE_ADDITIVE_MAX;
  }
  return world - 1u;
}

const VkrScene *vkr_editor_entity_scene(const VkrSampleUiFrame *frame,
                                        VkrEntityId entity) {
  if (frame->world && entity.u64 &&
      entity.parts.world == VKR_SCENE_WORLD_ROOT_ID) {
    return frame->world;
  }
  const uint32_t slot = editor_additive_slot(frame, entity);
  if (slot < VKR_SCENE_ADDITIVE_MAX) {
    return frame->additive[slot];
  }
  return frame->scene;
}

VkrSampleUiFrame vkr_editor_entity_frame(const VkrSampleUiFrame *frame,
                                         VkrEntityId entity) {
  VkrSampleUiFrame view = *frame;
  if (frame->world && entity.u64 &&
      entity.parts.world == VKR_SCENE_WORLD_ROOT_ID) {
    view.scene = frame->world;
    view.edits = frame->world_edits;
  }
  const uint32_t slot = editor_additive_slot(frame, entity);
  if (slot < VKR_SCENE_ADDITIVE_MAX) {
    view.scene = frame->additive[slot];
    view.edits = frame->additive_edits[slot];
  }
  return view;
}
