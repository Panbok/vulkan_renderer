#pragma once

#include "renderer/resources/vkr_resources.h"

// =============================================================================
// Font System Types
// =============================================================================

#define VKR_FONT_SYSTEM_DEFAULT_MEM MB(16)

/**
 * @brief A font system entry.
 * @param index The index into the fonts array.
 * @param ref_count The reference count.
 * @param auto_release The auto release flag.
 * @param loader_id The resource loader id for unloading.
 * @param resource The loader resource handle/result for unloading.
 */
typedef struct VkrFontSystemEntry {
  void *resource;       // Loader-specific handle/result (e.g., bitmap font).
  uint32_t index;       // The index into the fonts array.
  uint32_t ref_count;   // The reference count.
  uint32_t loader_id;   // Loader id associated with this font resource.
  bool8_t auto_release; // The auto release flag.
} VkrFontSystemEntry;
VkrHashTable(VkrFontSystemEntry);

/**
 * @brief A font system config.
 * @param max_system_font_count The maximum number of system fonts.
 * @param max_bitmap_font_count The maximum number of bitmap fonts.
 * @param max_mtsdf_font_count The maximum number of mtsdf fonts.
 */
typedef struct VkrFontSystemConfig {
  uint32_t max_system_font_count; // The maximum number of system fonts.
  uint32_t max_bitmap_font_count; // The maximum number of bitmap fonts.
  uint32_t max_mtsdf_font_count;  // The maximum number of mtsdf fonts.
} VkrFontSystemConfig;

#define VKR_FONT_CONFIG_MAX_FACES 16

/**
 * @brief Parsed font configuration from .fontcfg file.
 * @param file The path to the font data file (resolved to absolute).
 * @param atlas The path to the atlas texture (resolved, mtsdf only).
 * @param type The font type (bitmap/system/mtsdf).
 * @param faces Array of face name aliases.
 * @param face_count Number of face entries.
 * @param size Optional font size for system fonts (0 = default).
 * @param is_valid Whether parsing succeeded.
 */
typedef struct VkrFontConfig {
  String8 file;                             // Required: font data file path
  String8 atlas;                            // Optional: atlas path (mtsdf only)
  VkrFontType type;                         // Parsed from type=
  String8 faces[VKR_FONT_CONFIG_MAX_FACES]; // Face aliases
  uint32_t face_count;                      // Number of faces
  uint32_t size;                            // System font size override
  bool8_t is_valid;                         // Parsing success flag
} VkrFontConfig;

/**
 * @brief A font system.
 * @param renderer The renderer handle.
 * @param config The system config.
 * @param default_system_font_handle The default system font handle.
 * @param default_bitmap_font_handle The default bitmap font handle.
 * @param default_mtsdf_font_handle The default mtsdf font handle.
 * @param allocator The allocator.
 * @param arena The arena.
 * @param fonts The fonts array.
 * @param font_map The font map.
 * @param next_free_index The next free index.
 * @param generation_counter The generation counter.
 */
typedef struct VkrFontSystem {
  VkrRendererFrontendHandle renderer; // renderer handle

  VkrFontSystemConfig config; // system config

  VkrFontHandle default_system_font_handle; // The default system font handle.
  VkrFontHandle default_bitmap_font_handle; // The default bitmap font handle.
  VkrFontHandle default_mtsdf_font_handle;  // The default mtsdf font handle.

  VkrAllocator allocator; // persistent allocator wrapping arena
  Arena *arena;           // internal arena owned by the system

  VkrAllocator temp_allocator; // temporary allocator for scratch operations
  Arena *temp_arena;           // temporary arena (reset after each operation)

  Array_VkrFont fonts; // contiguous array of fonts
  VkrHashTable_VkrFontSystemEntry
      font_map; // name -> ref (index, refcount, flags)

  uint32_t next_free_index;    // linear probe for free slot
  uint32_t generation_counter; // Monotonic generation counter for texture
                               // description generations

  VkrJobSystem *job_system; // For async font loading
} VkrFontSystem;

// =============================================================================
// Font System Functions
// =============================================================================

/**
 * @brief Initializes the font system.
 * @param system The font system.
 * @param renderer The renderer handle.
 * @param config The system config.
 * @param out_error The error output.
 */
bool8_t vkr_font_system_init(VkrFontSystem *system,
                             VkrRendererFrontendHandle renderer,
                             const VkrFontSystemConfig *config,
                             VkrRendererError *out_error);

/**
 * @brief Shuts down the font system.
 * @param system The font system.
 */
void vkr_font_system_shutdown(VkrFontSystem *system);

/**
 * @brief Acquires a font by name.
 * @param system The font system.
 * @param name The name of the font.
 * @param auto_release The auto release flag.
 * @param out_error The error output.
 * @return A valid font handle on success, or an invalid handle on failure.
 */
VkrFontHandle vkr_font_system_acquire(VkrFontSystem *system, String8 name,
                                      bool8_t auto_release,
                                      VkrRendererError *out_error);

/**
 * @brief Releases a font by handle.
 * @param system The font system.
 * @param name The name of the font.
 */
void vkr_font_system_release(VkrFontSystem *system, String8 name);

/**
 * @brief Releases a font by handle.
 * @param system The font system.
 * @param handle The handle of the font.
 * @return The same handle with incremented reference count, or invalid handle
 * on failure.
 */
void vkr_font_system_release_by_handle(VkrFontSystem *system,
                                       VkrFontHandle handle);

/**
 * @brief Loads a font from a .fontcfg file.
 * @param system The font system.
 * @param name The name to register the font under.
 * @param fontcfg_path The path to the .fontcfg configuration file.
 * @param out_error The error output.
 */
bool8_t vkr_font_system_load_from_file(VkrFontSystem *system, String8 name,
                                       String8 fontcfg_path,
                                       VkrRendererError *out_error);

/**
 * @brief Loads a batch of fonts from .fontcfg files.
 * @param system The font system.
 * @param names The names to register the fonts under.
 * @param fontcfg_paths The paths to the .fontcfg configuration files.
 * @param count The number of fonts to load.
 * @param out_handles The output handles.
 * @param out_errors The error output.
 */
uint32_t vkr_font_system_load_batch(VkrFontSystem *system, const String8 *names,
                                    const String8 *fontcfg_paths,
                                    uint32_t count, VkrFontHandle *out_handles,
                                    VkrRendererError *out_errors);

/**
 * @brief Validates the font atlas.
 * @param system The font system.
 * @param handle The handle of the font.
 * @return True if the atlas is valid, false otherwise.
 */
bool8_t vkr_font_system_validate_atlas(VkrFontSystem *system,
                                       VkrFontHandle handle);

/**
 * @brief Validates the glyphs of a font.
 * @param system The font system.
 * @param handle The handle of the font.
 * @return True if the glyphs are valid, false otherwise.
 */
bool8_t vkr_font_system_validate_glyphs(VkrFontSystem *system,
                                        VkrFontHandle handle);

/**
 * @brief Checks if a font is valid.
 * @param system The font system.
 * @param handle The handle of the font.
 * @return True if the font is valid, false otherwise.
 */
bool8_t vkr_font_system_is_valid(VkrFontSystem *system, VkrFontHandle handle);

/**
 * @brief Acquires a font by handle.
 * @param system The font system.
 * @param handle The handle of the font.
 * @param out_error The error output.
 */
VkrFontHandle vkr_font_system_acquire_by_handle(VkrFontSystem *system,
                                                VkrFontHandle handle,
                                                VkrRendererError *out_error);

/**
 * @brief Gets a font by handle.
 * @param system The font system.
 * @param handle The handle of the font.
 * @return The font.
 */
VkrFont *vkr_font_system_get_by_handle(VkrFontSystem *system,
                                       VkrFontHandle handle);

/**
 * @brief Gets a font by name.
 * @param system The font system.
 * @param name The name of the font.
 * @return The font.
 */
VkrFont *vkr_font_system_get_by_name(VkrFontSystem *system, String8 name);

/**
 * @brief Gets the default font.
 * @param system The font system.
 * @return The default font.
 */
VkrFont *vkr_font_system_get_default_system_font(VkrFontSystem *system);

/**
 * @brief Gets the default bitmap font.
 * @param system The font system.
 * @return The default bitmap font.
 */
VkrFont *vkr_font_system_get_default_bitmap_font(VkrFontSystem *system);

/**
 * @brief Gets the default mtsdf font.
 * @param system The font system.
 * @return The default mtsdf font.
 */
VkrFont *vkr_font_system_get_default_mtsdf_font(VkrFontSystem *system);
