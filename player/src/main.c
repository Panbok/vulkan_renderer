#include "player.h"

#include "core/logger.h"
#include "core/vkr_json.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_vfs.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_entry.h"
#include "renderer/systems/vkr_texture_transcode_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copies the decoded string member `name` of `object` into `out`. A missing
   member leaves `out` empty. */
static bool8_t player_text(VkrJsonReader object, const char *name,
                           VkrAllocator *allocator, char *out,
                           uint32_t capacity) {
  out[0] = '\0';
  String8 value = {0};
  if (!vkr_json_find_root_field(&object, name)) {
    return true_v;
  }
  if (!vkr_json_parse_string_decoded(&object, allocator, &value) ||
      value.length >= capacity) {
    return false_v;
  }
  MemCopy(out, value.str, value.length);
  out[value.length] = '\0';
  return true_v;
}

/* An identity below the content root as an absolute path; an empty identity
   stays empty. */
static bool8_t player_content_path(VkrJsonReader object, const char *name,
                                   VkrAllocator *allocator, char *out) {
  char identity[VKR_PLAYER_PATH_CAPACITY];
  if (!player_text(object, name, allocator, identity, sizeof(identity))) {
    return false_v;
  }
  if (!identity[0]) {
    out[0] = '\0';
    return true_v;
  }
  return (uint32_t)snprintf(out, VKR_PLAYER_PATH_CAPACITY, "%s%s",
                            vkr_content_root(),
                            identity) < VKR_PLAYER_PATH_CAPACITY;
}

/* A name usable as one directory: path separators and controls become '_'. */
static void player_directory_name(const char *text, char *out,
                                  uint32_t capacity) {
  uint32_t length = 0u;
  for (const char *c = text; *c && length + 1u < capacity; ++c) {
    const bool8_t reserved = (uint8_t)*c < 0x20u || *c == '/' || *c == '\\' ||
                             *c == ':' || *c == '*' || *c == '?' ||
                             *c == '"' || *c == '<' || *c == '>' || *c == '|';
    out[length++] = reserved ? '_' : *c;
  }
  while (length && (out[length - 1u] == ' ' || out[length - 1u] == '.')) {
    --length;
  }
  out[length] = '\0';
}

/* `<base>/<company>/<name><leaf>`, or `<base>/<name><leaf>` without a
   company, created when missing. */
static bool8_t player_user_directory(const VkrPlayer *player, const char *base,
                                     const char *leaf, VkrAllocator *allocator,
                                     char *out) {
  char company[VKR_PLAYER_TEXT_CAPACITY];
  char name[VKR_PLAYER_TEXT_CAPACITY];
  player_directory_name(player->company, company, sizeof(company));
  player_directory_name(player->name, name, sizeof(name));
  if (!name[0]) {
    snprintf(name, sizeof(name), "Game");
  }
  const int written =
      company[0] ? snprintf(out, VKR_PLAYER_PATH_CAPACITY, "%s/%s/%s%s", base,
                            company, name, leaf)
                 : snprintf(out, VKR_PLAYER_PATH_CAPACITY, "%s/%s%s", base,
                            name, leaf);
  if (written < 0 || (uint32_t)written >= VKR_PLAYER_PATH_CAPACITY) {
    return false_v;
  }
  const String8 path =
      string8_create_from_cstr((const uint8_t *)out, strlen(out));
  return file_ensure_directory(allocator, &path);
}

/* Per-user Graphics preferences and caches: %APPDATA% and %LOCALAPPDATA% on
   Windows, Application Support and Caches on macOS, the XDG directories
   elsewhere. */
static bool8_t player_user_paths(VkrPlayer *player, VkrAllocator *allocator) {
  char settings_base[VKR_PLAYER_PATH_CAPACITY];
  char cache_base[VKR_PLAYER_PATH_CAPACITY];
#if defined(_WIN32)
  const char *roaming = getenv("APPDATA");
  const char *local = getenv("LOCALAPPDATA");
  if (!roaming || !roaming[0] || !local || !local[0]) {
    return false_v;
  }
  snprintf(settings_base, sizeof(settings_base), "%s", roaming);
  snprintf(cache_base, sizeof(cache_base), "%s", local);
  const char *cache_leaf = "/Cache";
#elif defined(__APPLE__)
  const char *home = getenv("HOME");
  if (!home || !home[0]) {
    return false_v;
  }
  snprintf(settings_base, sizeof(settings_base),
           "%s/Library/Application Support", home);
  snprintf(cache_base, sizeof(cache_base), "%s/Library/Caches", home);
  const char *cache_leaf = "";
#else
  const char *home = getenv("HOME");
  const char *config = getenv("XDG_CONFIG_HOME");
  const char *cache = getenv("XDG_CACHE_HOME");
  if ((!config || !config[0] || !cache || !cache[0]) && (!home || !home[0])) {
    return false_v;
  }
  if (config && config[0]) {
    snprintf(settings_base, sizeof(settings_base), "%s", config);
  } else {
    snprintf(settings_base, sizeof(settings_base), "%s/.config", home);
  }
  if (cache && cache[0]) {
    snprintf(cache_base, sizeof(cache_base), "%s", cache);
  } else {
    snprintf(cache_base, sizeof(cache_base), "%s/.cache", home);
  }
  const char *cache_leaf = "";
#endif
  char settings[VKR_PLAYER_PATH_CAPACITY];
  return player_user_directory(player, settings_base, "", allocator,
                               settings) &&
         (uint32_t)snprintf(player->settings_path,
                            sizeof(player->settings_path), "%s/settings.json",
                            settings) < sizeof(player->settings_path) &&
         player_user_directory(player, cache_base, cache_leaf, allocator,
                               player->cache_root);
}

bool8_t vkr_player_load(VkrPlayer *player) {
  const String8 description = vkr_vfs_bundle_description();
  VkrJsonReader root = vkr_json_reader_from_string(description);
  VkrJsonReader game = {0};
  if (!description.length || !vkr_json_find_root_field(&root, "game") ||
      !vkr_json_enter_object(&root, &game)) {
    fprintf(stderr, "No game package: this player runs beside the bundle.json "
                    "a project build writes\n");
    return false_v;
  }
  Arena *arena = arena_create(MB(1), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  if (!arena || !vkr_allocator_arena(&allocator)) {
    if (arena) {
      arena_destroy(arena);
    }
    return false_v;
  }
  bool8_t ok =
      player_text(game, "name", &allocator, player->name,
                  sizeof(player->name)) &&
      player_text(game, "company", &allocator, player->company,
                  sizeof(player->company)) &&
      player_content_path(game, "world", &allocator, player->world) &&
      player_content_path(game, "world_overlay", &allocator,
                          player->world_overlay) &&
      player_content_path(game, "startup_scene", &allocator,
                          player->startup_scene) &&
      player_content_path(game, "startup_overlay", &allocator,
                          player->startup_overlay);

  VkrJsonReader window = game;
  VkrJsonReader window_object = {0};
  if (ok && vkr_json_find_root_field(&window, "window") &&
      vkr_json_enter_object(&window, &window_object)) {
    int32_t width = 0;
    int32_t height = 0;
    VkrJsonReader field = window_object;
    if (vkr_json_find_root_field(&field, "width") &&
        vkr_json_parse_int(&field, &width) && width > 0) {
      player->window_width = (uint32_t)width;
    }
    field = window_object;
    if (vkr_json_find_root_field(&field, "height") &&
        vkr_json_parse_int(&field, &height) && height > 0) {
      player->window_height = (uint32_t)height;
    }
    char mode[16];
    if (player_text(window_object, "mode", &allocator, mode, sizeof(mode))) {
      player->window_mode = !strcmp(mode, "fullscreen")
                                ? VKR_WINDOW_MODE_FULLSCREEN
                            : !strcmp(mode, "borderless")
                                ? VKR_WINDOW_MODE_BORDERLESS
                                : VKR_WINDOW_MODE_WINDOWED;
    }
  }
  VkrJsonReader graphics = game;
  if (ok && vkr_json_find_root_field(&graphics, "graphics")) {
    (void)vkr_json_capture_composite(&graphics, &player->graphics);
  }
  VkrJsonReader camera = game;
  if (ok && vkr_json_find_root_field(&camera, "startup_camera")) {
    (void)vkr_json_capture_composite(&camera, &player->startup_camera);
  }
  VkrJsonReader fonts = game;
  if (ok && vkr_json_find_root_field(&fonts, "fonts") &&
      fonts.pos < fonts.length && fonts.data[fonts.pos] == '[') {
    ++fonts.pos;
    while (ok && vkr_json_next_array_element(&fonts)) {
      VkrJsonReader font = {0};
      if (player->font_count == VKR_PLAYER_MAX_FONTS ||
          !vkr_json_enter_object(&fonts, &font)) {
        ok = false_v;
        break;
      }
      VkrPlayerFont *entry = &player->fonts[player->font_count++];
      ok = player_text(font, "name", &allocator, entry->name,
                       sizeof(entry->name)) &&
           player_content_path(font, "config", &allocator, entry->config) &&
           entry->name[0] && entry->config[0];
    }
  }
  if (!ok) {
    fprintf(stderr, "The game package description is invalid\n");
  } else if (!player_user_paths(player, &allocator)) {
    /* The game still runs; preferences and caches stay with the package. */
    fprintf(stderr, "No per-user settings or cache folder; the package folder "
                    "holds them\n");
    player->settings_path[0] = '\0';
    player->cache_root[0] = '\0';
  }
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  return ok;
}

VKR_MAIN(argc, argv) {
  /* The package mounts before any path resolves; the runtime keeps it. */
  if (!vkr_vfs_mount_startup()) {
    fprintf(stderr, "Cannot mount the game package\n");
    return 1;
  }
  static VkrPlayer player = {0};
  if (!vkr_player_load(&player)) {
    return 1;
  }
#if !VKR_PLAYER_SHIPPING
  log_max_level_set(LOG_LEVEL_INFO);
#endif
  if (player.cache_root[0]) {
    vkr_texture_transcode_cache_set_root(player.cache_root);
  }
  VkrSampleRuntimeConfig config = vkr_sample_runtime_config_default();
  config.title = player.name[0] ? player.name : "Game";
  config.presentation.window_width_pt = player.window_width;
  config.presentation.window_height_pt = player.window_height;
  config.presentation.window_mode = player.window_mode;
  config.graphics_settings_path =
      player.settings_path[0] ? player.settings_path : NULL;
  config.graphics_defaults = player.graphics;
  config.ui = vkr_player_ui_client(&player);
  return vkr_sample_runtime_run(argc, argv, &config);
}
