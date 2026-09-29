#pragma once

#include "vkr_sample_runtime.h"

/* vkr_player (docs/proposals/project-packaging.md): the executable a
 * packaged game ships. It reads the `game` object of the bundle.json beside
 * it, opens the World and startup scene as the editor does, and offers only a
 * pause menu. VKR_PLAYER_SHIPPING builds drop the development overlay. */

#define VKR_PLAYER_PATH_CAPACITY 1024u
#define VKR_PLAYER_TEXT_CAPACITY 256u
#define VKR_PLAYER_MAX_FONTS 16u

typedef struct VkrPlayerFont {
  char name[128];
  char config[VKR_PLAYER_PATH_CAPACITY]; /* Absolute content path. */
  VkrFontHandle handle;
} VkrPlayerFont;

/* Process-lifetime state; paths are absolute below the content root. */
typedef struct VkrPlayer {
  char name[VKR_PLAYER_TEXT_CAPACITY];
  char company[VKR_PLAYER_TEXT_CAPACITY];
  char world[VKR_PLAYER_PATH_CAPACITY];
  char world_overlay[VKR_PLAYER_PATH_CAPACITY];
  char startup_scene[VKR_PLAYER_PATH_CAPACITY];
  char startup_overlay[VKR_PLAYER_PATH_CAPACITY];
  char settings_path[VKR_PLAYER_PATH_CAPACITY];
  /* Per-user texture transcode cache; the install folder stays read-only. */
  char cache_root[VKR_PLAYER_PATH_CAPACITY];
  VkrPlayerFont fonts[VKR_PLAYER_MAX_FONTS];
  uint32_t font_count;
  uint32_t window_width;
  uint32_t window_height;
  /* Views into the mounted bundle description; the camera is an editor
     viewport recall (vkr_sample_scene_recall_read_json). */
  String8 graphics;
  String8 startup_camera;
  bool8_t requested;
  bool8_t camera_applied;
  bool8_t paused;
  bool8_t pause_changed;
  bool8_t overlay_visible;
} VkrPlayer;

/** Reads the mounted bundle's game object; reports a missing or invalid
 * package on stderr. */
bool8_t vkr_player_load(VkrPlayer *player);
VkrSampleUiClient vkr_player_ui_client(VkrPlayer *player);
