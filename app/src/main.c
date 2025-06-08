#include "application.h"
#include "containers/array.h"
#include "core/event.h"
#include "core/input.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "renderer/renderer.h"

#define PLAYER_SPEED 50.0
#define ENTITY_COUNT 1

typedef struct State {
  Array_uint16_t entities;

  Array_float64_t player_position_x;
  Array_float64_t player_position_y;

  InputState *input_state;

  Arena *app_arena;
  Arena *event_arena;
  Arena *stats_arena;

  GraphicsPipelineDescription pipeline;
} State;

State *state = NULL;

bool8_t application_on_event(Event *event) {
  // log_debug("Application on event: %d", event->type);
  return true_v;
}

bool8_t application_on_window_event(Event *event) {
  // log_debug("Application on window event: %d", event->type);
  return true_v;
}

bool8_t application_on_key_event(Event *event) { return true_v; }

bool8_t application_on_mouse_event(Event *event) {
  // log_debug("Application on mouse event: %d", event->type);
  return true_v;
}

void application_update(Application *application, float64_t delta) {
  double fps_value = 0.0;
  if (delta > 0.000001) {
    fps_value = 1.0 / delta;
  } else {
    fps_value = 1.0 / application->config->target_frame_rate;
  }
  // log_debug("CALCULATED FPS VALUE: %f, DELTA WAS: %f", fps_value, delta);

  if (state == NULL || state->input_state == NULL) {
    log_error("State or input state is NULL");
    return;
  }

  InputState *input_state = state->input_state;
  for (uint16_t i = 0; i < state->entities.length; i++) {
    float64_t *player_position_x =
        array_get_float64_t(&state->player_position_x, i);
    float64_t *player_position_y =
        array_get_float64_t(&state->player_position_y, i);

    if (input_is_key_down(input_state, KEY_W)) {
      *player_position_x += PLAYER_SPEED * delta;
    }
    if (input_is_key_down(input_state, KEY_S)) {
      *player_position_x -= PLAYER_SPEED * delta;
    }
    if (input_is_key_down(input_state, KEY_A)) {
      *player_position_y += PLAYER_SPEED * delta;
    }
    if (input_is_key_down(input_state, KEY_D)) {
      *player_position_y -= PLAYER_SPEED * delta;
    }

    // log_debug("Entity ID: %u, Player Position: (x - %f, y - %f)",
    //           *array_get_uint16_t(&state->entities, i), *player_position_x,
    //           *player_position_y);
  }

  if (input_is_key_up(state->input_state, KEY_M) &&
      input_was_key_down(state->input_state, KEY_M)) {
    Scratch scratch_stats = scratch_create(state->stats_arena);
    char *arena_stats =
        arena_format_statistics(state->app_arena, scratch_stats.arena);
    char *event_stats =
        arena_format_statistics(state->event_arena, scratch_stats.arena);
    log_debug("Application arena stats:\n%s", arena_stats);
    log_debug("Event arena stats:\n%s", event_stats);
    scratch_destroy(scratch_stats, ARENA_MEMORY_TAG_STRING);
  }
}

// todo: should look into using DLLs in debug builds for hot reload
// and static for release builds, also should look into how we can
// implement hot reload for the application in debug builds
int main(int argc, char **argv) {
  ApplicationConfig config = {0};
  config.title = "Hello, World!";
  config.x = 100;
  config.y = 100;
  config.width = 800;
  config.height = 600;
  config.app_arena_size = MB(1);
  config.target_frame_rate = 60;

  Application application;
  if (!application_create(&application, &config)) {
    log_fatal("Application creation failed!");
    return 1;
  }

  state = arena_alloc(application.app_arena, sizeof(State),
                      ARENA_MEMORY_TAG_STRUCT);
  state->stats_arena = arena_create(KB(1), KB(1));
  state->entities = array_create_uint16_t(application.app_arena, ENTITY_COUNT);
  state->player_position_x =
      array_create_float64_t(application.app_arena, ENTITY_COUNT);
  state->player_position_y =
      array_create_float64_t(application.app_arena, ENTITY_COUNT);
  state->input_state = &application.window.input_state;
  state->app_arena = application.app_arena;
  state->event_arena = application.event_manager.arena;

  const char *shader_path = "assets/triangle.spv";
  const FilePath path = file_path_create(
      shader_path, application.renderer_arena, FILE_PATH_TYPE_RELATIVE);
  if (!file_exists(&path)) {
    log_fatal("Vertex shader file does not exist: %s", shader_path);
    return 1;
  }

  // Load shaders
  uint8_t *shader_data = NULL;
  uint64_t shader_size = 0;
  FileError file_error = file_load_spirv_shader(
      &path, application.renderer_arena, &shader_data, &shader_size);
  if (file_error != FILE_ERROR_NONE) {
    log_fatal("Failed to load shader: %s", file_get_error_string(file_error));
    return 1;
  }

  ShaderModuleDescription vertex_shader_desc = {
      .stage = SHADER_STAGE_VERTEX_BIT,
      .code = (const uint8_t *)shader_data,
      .size = shader_size,
      .entry_point = string8_lit("vertexMain"),
  };

  RendererError renderer_error = RENDERER_ERROR_NONE;
  ShaderHandle vertex_shader = renderer_create_shader_from_source(
      application.renderer, &vertex_shader_desc, &renderer_error);
  if (renderer_error != RENDERER_ERROR_NONE) {
    log_fatal("Failed to create vertex shader: %s",
              renderer_get_error_string(renderer_error));
    return 1;
  }

  ShaderModuleDescription fragment_shader_desc = {
      .stage = SHADER_STAGE_FRAGMENT_BIT,
      .code = (const uint8_t *)shader_data,
      .size = shader_size,
      .entry_point = string8_lit("fragmentMain"),
  };

  ShaderHandle fragment_shader = renderer_create_shader_from_source(
      application.renderer, &fragment_shader_desc, &renderer_error);
  if (renderer_error != RENDERER_ERROR_NONE) {
    log_fatal("Failed to create fragment shader: %s",
              renderer_get_error_string(renderer_error));
    return 1;
  }

  log_debug("Vertex shader: %p", vertex_shader);
  log_debug("Fragment shader: %p", fragment_shader);

  const GraphicsPipelineDescription pipeline_desc = {
      .vertex_shader = vertex_shader,
      .fragment_shader = fragment_shader,
      .attribute_count = 0,
      .attributes = NULL,
      .binding_count = 0,
      .bindings = NULL,
      .topology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };

  PipelineHandle pipeline = renderer_create_pipeline(
      application.renderer, &pipeline_desc, &renderer_error);
  if (renderer_error != RENDERER_ERROR_NONE) {
    log_fatal("Failed to create pipeline: %s",
              renderer_get_error_string(renderer_error));
    return 1;
  }

  application_start(&application);
  application_close(&application);

  renderer_destroy_pipeline(application.renderer, pipeline);
  renderer_destroy_shader(application.renderer, vertex_shader);
  renderer_destroy_shader(application.renderer, fragment_shader);

  array_destroy_uint16_t(&state->entities);
  array_destroy_float64_t(&state->player_position_x);
  array_destroy_float64_t(&state->player_position_y);

  application_shutdown(&application);
  return 0;
}