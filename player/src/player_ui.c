#include "player.h"

#include "core/logger.h"
#if !VKR_PLAYER_SHIPPING
#include "debug_overlay.h"
#endif

#include <stdio.h>
#include <string.h>

static String8 player_string(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, strlen(text));
}

/* Scene fonts register before any scene text instantiates, as the editor
   registers a prepared scene's fonts. */
static bool8_t player_ui_initialize(void *state, VkrUiDockTree *dock,
                                    VkrUiSystem *system) {
  (void)dock;
  VkrPlayer *player = state;
  for (uint32_t i = 0; i < player->font_count; ++i) {
    VkrPlayerFont *font = &player->fonts[i];
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_font_system_load_from_file(system->fonts,
                                        player_string(font->name),
                                        player_string(font->config), &error)) {
      log_error("Player: cannot load font %s from %s", font->name,
                font->config);
      return false_v;
    }
    font->handle = vkr_font_system_acquire(
        system->fonts, player_string(font->name), true_v, &error);
    if (error != VKR_RENDERER_ERROR_NONE) {
      return false_v;
    }
  }
  return true_v;
}

static void player_ui_handle_input(void *state, const InputState *input) {
  VkrPlayer *player = state;
  if (player->requested && input_key_just_pressed(input, KEY_ESCAPE)) {
    player->paused = !player->paused;
    player->pause_changed = true_v;
  }
#if !VKR_PLAYER_SHIPPING
  if (input_key_just_released(input, KEY_F6)) {
    player->overlay_visible = !player->overlay_visible;
  }
#endif
}

static VkrUiWidgetConfig player_button(uint32_t row) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement = (VkrUiPlacement){
      .column = 0u,
      .row = row,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_CENTER,
  };
  config.style.min_size_pt = (Vec2){220.0f, 34.0f};
  config.style.font_size_pt = 15.0f;
  config.style.padding_pt = (VkrUiEdges){6.0f, 12.0f, 6.0f, 12.0f};
  config.style.corner_radius_pt = (Vec4){5.0f, 5.0f, 5.0f, 5.0f};
  return config;
}

/* The only player UI: Resume, the display mode, Invert mouse Y and Quit over
   the paused game. */
static void player_ui_pause_menu(VkrPlayer *player,
                                 const VkrSampleUiFrame *frame) {
  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.unit = VKR_UI_TRACK_AUTO}, {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO}, {.unit = VKR_UI_TRACK_AUTO},
      {.unit = VKR_UI_TRACK_AUTO},
  };
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_CENTER,
      .align = VKR_UI_ALIGN_CENTER,
  };
  panel.columns = &column;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style.padding_pt = (VkrUiEdges){18.0f, 22.0f, 18.0f, 22.0f};
  panel.style.gap_pt = 10.0f;
  panel.style.corner_radius_pt = (Vec4){8.0f, 8.0f, 8.0f, 8.0f};
  panel.style.background_color = (Vec4){0.04f, 0.05f, 0.07f, 0.88f};
  panel.style.text_color = (Vec4){0.90f, 0.92f, 0.96f, 1.0f};
  if (!vkr_ui_panel_begin(frame->ui, string8_lit("player.pause"), &panel)) {
    return;
  }
  VkrUiWidgetConfig title = player_button(0u);
  title.style.font_size_pt = 20.0f;
  title.style.background_color = (Vec4){0};
  vkr_ui_label(frame->ui, string8_lit("title"), string8_lit("Paused"),
               &title);
  VkrUiWidgetConfig resume = player_button(1u);
  if (vkr_ui_button(frame->ui, string8_lit("resume"), string8_lit("Resume"),
                    &resume)) {
    player->paused = false_v;
    player->pause_changed = true_v;
  }
  /* Switches between the game's covering mode (fullscreen when it starts in
     a window) and a window. */
  const VkrWindowMode mode = vkr_window_get_mode(frame->window);
  const VkrWindowMode covering = player->window_mode != VKR_WINDOW_MODE_WINDOWED
                                     ? player->window_mode
                                     : VKR_WINDOW_MODE_FULLSCREEN;
  VkrUiWidgetConfig display = player_button(2u);
  if (frame->window &&
      vkr_ui_button(frame->ui, string8_lit("display"),
                    mode == VKR_WINDOW_MODE_WINDOWED
                        ? string8_lit("Fullscreen")
                        : string8_lit("Windowed"),
                    &display)) {
    (void)vkr_window_set_mode(frame->window, mode == VKR_WINDOW_MODE_WINDOWED
                                                 ? covering
                                                 : VKR_WINDOW_MODE_WINDOWED);
  }
  /* A machine-local Graphics setting, saved with the game's preferences. */
  VkrUiWidgetConfig invert = player_button(3u);
  if (frame->graphics && frame->graphics_request) {
    const bool8_t inverted = frame->graphics->settings.invert_mouse_y;
    if (vkr_ui_button(frame->ui, string8_lit("invert"),
                      inverted ? string8_lit("Invert mouse Y: On")
                               : string8_lit("Invert mouse Y: Off"),
                      &invert)) {
      VkrGraphicsSettings settings = frame->graphics->settings;
      settings.invert_mouse_y = !inverted;
      *frame->graphics_request =
          (VkrGraphicsSettingsRequest){.settings = settings, .apply = true_v};
    }
  }
  VkrUiWidgetConfig quit = player_button(4u);
  if (vkr_ui_button(frame->ui, string8_lit("quit"), string8_lit("Quit"),
                    &quit) &&
      frame->quit_request) {
    *frame->quit_request = true_v;
  }
  (void)vkr_ui_panel_end(frame->ui);
}

static VkrUiDockInputCapture player_ui_build(void *state,
                                             const VkrSampleUiFrame *frame) {
  VkrPlayer *player = state;
  /* The first frame opens the game as the editor opens a project: the
     script library, whose types the documents use, the World, then the
     startup scene over it. */
  if (!player->requested) {
    player->requested = true_v;
    VkrSampleScriptRequest *scripts = frame->script_request;
    if (player->script_library[0] && scripts &&
        scripts->load_count < VKR_SAMPLE_SCRIPT_LOAD_MAX) {
      VkrSampleScriptLoad *load = &scripts->loads[scripts->load_count++];
      *load = (VkrSampleScriptLoad){.project = true_v, .in_place = true_v};
      snprintf(load->name, sizeof(load->name), "project");
      snprintf(load->path, sizeof(load->path), "%s", player->script_library);
    }
    if (player->world[0]) {
      *frame->world_request = (VkrSampleWorldRequest){
          .path = player_string(player->world),
          .sidecar_path = player_string(player->world_overlay),
          .load = true_v};
    }
    if (player->startup_scene[0]) {
      *frame->scene_request = (VkrSampleSceneRequest){
          .select = true_v,
          .path = player_string(player->startup_scene),
          .sidecar_path = player_string(player->startup_overlay)};
    }
  }
  /* The startup camera applies once the startup scene is active. */
  if (!player->camera_applied && frame->scene &&
      frame->editor_state_request &&
      string8_equals(&frame->scene_path,
                     &(String8){.str = (uint8_t *)player->startup_scene,
                                .length = strlen(player->startup_scene)})) {
    player->camera_applied = true_v;
    VkrSampleSceneRecall recall = {0};
    if (player->startup_camera.length &&
        vkr_sample_scene_recall_read_json(player->startup_camera, &recall)) {
      recall.selection_valid = false_v;
      frame->editor_state_request->apply_recall = true_v;
      frame->editor_state_request->recall = recall;
    }
  }
  if (player->pause_changed) {
    player->pause_changed = false_v;
    *frame->transport_action = player->paused
                                   ? VKR_SAMPLE_TRANSPORT_PAUSE_SIMULATION
                                   : VKR_SAMPLE_TRANSPORT_START_SIMULATION;
    if (player->paused && frame->window) {
      vkr_window_set_mouse_capture(frame->window, false_v);
    }
  }
  if (frame->modal) {
    *frame->modal = player->paused;
  }
  if (player->paused) {
    player_ui_pause_menu(player, frame);
  }
#if !VKR_PLAYER_SHIPPING
  if (player->overlay_visible) {
    String8 performance = string8_create_formatted(
        frame->ui->frame_allocator, "%.*s\nRender %ux%u\nOutput %ux%u\n%.*s",
        (int32_t)frame->text.performance.length, frame->text.performance.str,
        frame->scene_render_width, frame->scene_render_height,
        frame->scene_output_width, frame->scene_output_height,
        (int32_t)frame->text.system.length, frame->text.system.str);
    vkr_debug_overlay_build(frame->ui, frame->text.camera, performance);
  }
#endif
  return (VkrUiDockInputCapture){.mouse = player->paused};
}

static bool8_t player_ui_shutdown(void *state, const VkrUiDockTree *dock,
                                  VkrUiSystem *system) {
  (void)dock;
  VkrPlayer *player = state;
  for (uint32_t i = 0; i < player->font_count; ++i) {
    vkr_font_system_release_by_handle(system->fonts, player->fonts[i].handle);
  }
  player->font_count = 0u;
  return true_v;
}

VkrSampleUiClient vkr_player_ui_client(VkrPlayer *player) {
  return (VkrSampleUiClient){
      .state = player,
      .initialize = player_ui_initialize,
      .handle_input = player_ui_handle_input,
      .build = player_ui_build,
      .shutdown = player_ui_shutdown,
  };
}
