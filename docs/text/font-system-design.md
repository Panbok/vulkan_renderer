---
status: partial
updated: 2026-07-31
authority: design
---
# Font System Implementation Design Document

> **Current boundary.** The font system ships, but this document retains planned
> code and an unchecked historical implementation checklist. Verify all
> signatures and ownership details against `lib/src/renderer/systems/vkr_font_system.*`.

## Overview

This document describes the implementation plan for `vkr_font_system.c`. The font system manages font resources with reference counting, integrates with the bitmap font loader via the resource system, and provides font data to the text rendering system.

**Target File:** `lib/src/renderer/systems/vkr_font_system.c`
**Header:** `lib/src/renderer/systems/vkr_font_system.h`
**Reference Implementation:** `lib/src/renderer/systems/vkr_texture_system.c`

---

## Dependencies

```c
#include "renderer/systems/vkr_font_system.h"

#include "containers/str.h"
#include "core/logger.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/bitmap_font_loader.h"
#include "renderer/systems/vkr_resource_system.h"
```

---

## Data Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            FONT SYSTEM DATA FLOW                             │
└─────────────────────────────────────────────────────────────────────────────┘

LOADING:
┌──────────────┐  load_from_file   ┌────────────────┐  vkr_resource_  ┌────────────────┐
│ Font System  │ ─────────────────►│ Resource       │ ─system_load──►│ BitmapFont     │
│              │                   │ System         │                 │ Loader         │
└──────────────┘                   └────────────────┘                 └────────────────┘
       │                                                                     │
       │ 1. Find free slot                                                  │ returns
       │ 2. Copy VkrFont to fonts[]         VkrBitmapFontLoaderResult ◄─────┘
       │ 3. Insert into font_map            ├─ VkrFont (glyphs, kernings)
       │ 4. Return VkrFontHandle            └─ atlas handles
       ▼

USAGE (Text Rendering):
┌──────────────┐  get_by_handle   ┌──────────────┐
│ Font System  │ ◄───────────────│ Text System  │
│              │ ───────────────►│ / UI         │
└──────────────┘    VkrFont*      └──────────────┘
                                         │
                                         ▼
                                  VkrTextStyle { font_data: VkrFont* }
```

---

## Implementation Plan

### Phase 1: Helper Functions

#### 1.1 Find Free Slot

```c
/**
 * @brief Finds a free slot in the fonts array.
 * @param system The font system.
 * @return The index of a free slot, or VKR_INVALID_ID if none available.
 *
 * PATTERN: Same as vkr_texture_system_find_free_slot
 * - Uses VkrFont.generation == VKR_INVALID_ID to detect free slots
 * - Linear probe starting from next_free_index, then wraps around
 */
vkr_internal uint32_t vkr_font_system_find_free_slot(VkrFontSystem *system) {
  assert_log(system != NULL, "System is NULL");

  uint32_t max_fonts = system->config.max_system_font_count +
                       system->config.max_bitmap_font_count;

  // First: search from next_free_index to end
  for (uint32_t i = system->next_free_index; i < max_fonts; i++) {
    VkrFont *font = &system->fonts.data[i];
    if (font->generation == VKR_INVALID_ID) {
      system->next_free_index = i + 1;
      return i;
    }
  }

  // Second: wrap around and search from 0 to next_free_index
  for (uint32_t i = 0; i < system->next_free_index; i++) {
    VkrFont *font = &system->fonts.data[i];
    if (font->generation == VKR_INVALID_ID) {
      system->next_free_index = i + 1;
      return i;
    }
  }

  return VKR_INVALID_ID;
}
```

#### 1.2 Get Font by Index (Internal)

```c
/**
 * @brief Gets a font by array index.
 * @param system The font system.
 * @param index The index into the fonts array.
 * @return The font, or NULL if index is invalid.
 */
vkr_internal VkrFont *vkr_font_system_get_by_index(VkrFontSystem *system,
                                                    uint32_t index) {
  if (!system || index >= system->fonts.length) {
    return NULL;
  }
  return &system->fonts.data[index];
}
```

#### 1.3 Destroy Font (Internal)

```c
/**
 * @brief Destroys a font and releases its resources.
 * @param system The font system.
 * @param font The font to destroy.
 * @param name The name of the font (for resource system unload).
 *
 * IMPORTANT: This must unload atlas textures via resource system.
 */
vkr_internal void vkr_font_system_destroy_font(VkrFontSystem *system,
                                                VkrFont *font,
                                                String8 name) {
  if (!font) return;

  // Unload atlas pages via resource system
  if (font->atlas_pages.data) {
    for (uint32_t i = 0; i < font->atlas_pages.length; i++) {
      VkrTextureHandle atlas = font->atlas_pages.data[i];
      if (atlas.id != 0 && atlas.id != VKR_INVALID_ID) {
        // Build atlas path for unload (same pattern as loader)
        // Note: We need to track page file names for proper unload
        // This is handled by the bitmap font loader's unload callback
      }
    }
  }

  // The actual resource cleanup happens via resource system unload
  // which calls the bitmap font loader's unload callback

  // Clear font slot
  MemZero(font, sizeof(VkrFont));
  font->generation = VKR_INVALID_ID;
}
```

---

### Phase 2: Initialization & Shutdown

#### 2.1 Init

```c
bool8_t vkr_font_system_init(VkrFontSystem *system,
                             VkrRendererFrontendHandle renderer,
                             const VkrFontSystemConfig *config,
                             VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  MemZero(system, sizeof(*system));

  // 1. Create arena for system allocations
  ArenaFlags arena_flags = bitset8_create();
  bitset8_set(&arena_flags, ARENA_FLAG_LARGE_PAGES);
  system->arena = arena_create(VKR_FONT_SYSTEM_DEFAULT_MEM,
                               VKR_FONT_SYSTEM_DEFAULT_MEM / 4,
                               arena_flags);
  if (!system->arena) {
    log_fatal("Failed to create font system arena");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // 2. Setup allocator wrapping arena
  system->allocator = (VkrAllocator){.ctx = system->arena};
  vkr_allocator_arena(&system->allocator);

  // 3. Store config and renderer
  system->renderer = renderer;
  system->config = *config;
  system->job_system = NULL; // Set via separate call if needed

  // 4. Calculate total font capacity
  uint32_t max_fonts = config->max_system_font_count +
                       config->max_bitmap_font_count;

  // 5. Create fonts array
  system->fonts = array_create_VkrFont(&system->allocator, max_fonts);
  if (!system->fonts.data) {
    log_fatal("Failed to allocate fonts array");
    arena_destroy(system->arena);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // 6. Initialize all slots as invalid
  for (uint32_t i = 0; i < max_fonts; i++) {
    system->fonts.data[i].id = VKR_INVALID_ID;
    system->fonts.data[i].generation = VKR_INVALID_ID;
  }

  // 7. Create hash table for name -> entry mapping
  system->font_map = vkr_hash_table_create_VkrFontSystemEntry(
      &system->allocator, (uint64_t)max_fonts * 2ULL);

  // 8. Initialize counters
  system->next_free_index = 0;
  system->generation_counter = 1;

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}
```

#### 2.2 Shutdown

```c
void vkr_font_system_shutdown(VkrFontSystem *system,
                              VkrRendererFrontendHandle renderer) {
  if (!system) return;

  // 1. Iterate all fonts and destroy valid ones
  uint32_t max_fonts = system->config.max_system_font_count +
                       system->config.max_bitmap_font_count;

  for (uint32_t i = 0; i < max_fonts; i++) {
    VkrFont *font = &system->fonts.data[i];
    if (font->generation != VKR_INVALID_ID) {
      // Font resources will be cleaned up when arena is destroyed
      // Atlas textures should be released if still loaded
      // Note: In practice, resources should be explicitly unloaded
      // before shutdown via vkr_font_system_release
    }
  }

  // 2. Destroy arrays (hash table entries point into arena)
  array_destroy_VkrFont(&system->fonts);

  // 3. Destroy arena (frees all allocations)
  arena_destroy(system->arena);

  // 4. Zero out system
  MemZero(system, sizeof(*system));
}
```

---

### Phase 3: Load Functions

#### 3.1 Load From File

```c
bool8_t vkr_font_system_load_from_file(VkrFontSystem *system, String8 name,
                                       String8 path, VkrFontType type,
                                       VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  // 1. Check if font already exists
  const char *font_key = (const char *)name.str;
  VkrFontSystemEntry *existing =
      vkr_hash_table_get_VkrFontSystemEntry(&system->font_map, font_key);
  if (existing) {
    log_warn("Font '%s' already loaded", font_key);
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  // 2. Find free slot
  uint32_t free_slot = vkr_font_system_find_free_slot(system);
  if (free_slot == VKR_INVALID_ID) {
    log_error("Font system is full");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // 3. Determine resource type based on font type
  VkrResourceType resource_type = VKR_RESOURCE_TYPE_UNKNOWN;
  if (type == VKR_FONT_TYPE_BITMAP) {
    resource_type = VKR_RESOURCE_TYPE_BITMAP_FONT;
  } else {
    // System fonts not yet supported
    log_error("System font type not yet implemented");
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // 4. Create temp allocator scope for loading
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(&system->allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // 5. Load via resource system
  VkrResourceHandleInfo handle_info = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;

  if (!vkr_resource_system_load(resource_type, path, &system->allocator,
                                &handle_info, &load_error)) {
    log_error("Failed to load font '%.*s': %s",
              (int32_t)path.length, path.str,
              string8_cstr(&vkr_renderer_get_error_string(load_error)));
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    *out_error = load_error;
    return false_v;
  }

  // 6. Extract font from loader result
  VkrBitmapFontLoaderResult *result =
      (VkrBitmapFontLoaderResult *)handle_info.as.custom;
  if (!result || !result->success) {
    log_error("Bitmap font loader returned invalid result");
    vkr_resource_system_unload(&handle_info, path);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  // 7. Copy font data to system's fonts array
  VkrFont *font = &system->fonts.data[free_slot];
  *font = result->font;

  // 8. Assign stable id and generation
  font->id = free_slot + 1;
  font->generation = system->generation_counter++;

  // 9. Allocate stable key for hash table
  char *stable_key = (char *)vkr_allocator_alloc(
      &system->allocator, name.length + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
    log_error("Failed to allocate key for font map");
    vkr_resource_system_unload(&handle_info, path);
    font->generation = VKR_INVALID_ID;
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemCopy(stable_key, name.str, (size_t)name.length);
  stable_key[name.length] = '\0';

  // 10. Insert into hash table
  VkrFontSystemEntry entry = {
      .index = free_slot,
      .ref_count = 0,  // Starts at 0, incremented on acquire
      .auto_release = true_v,
  };

  if (!vkr_hash_table_insert_VkrFontSystemEntry(&system->font_map,
                                                 stable_key, entry)) {
    log_error("Failed to insert font '%s' into hash table", stable_key);
    vkr_resource_system_unload(&handle_info, path);
    font->generation = VKR_INVALID_ID;
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  *out_error = VKR_RENDERER_ERROR_NONE;

  log_debug("Loaded font '%s' (size=%u, glyphs=%llu)",
            stable_key, font->size, (unsigned long long)font->glyphs.length);

  return true_v;
}
```

#### 3.2 Load Batch

```c
uint32_t vkr_font_system_load_batch(VkrFontSystem *system,
                                    const String8 *names,
                                    const String8 *paths,
                                    uint32_t count,
                                    VkrFontType type,
                                    VkrFontHandle *out_handles,
                                    VkrRendererError *out_errors) {
  assert_log(system != NULL, "System is NULL");
  assert_log(names != NULL, "Names is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  if (count == 0) return 0;

  // Initialize outputs
  for (uint32_t i = 0; i < count; i++) {
    out_handles[i] = VKR_FONT_HANDLE_INVALID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  // For now, use sequential loading
  // TODO: Parallel loading via job system if available
  uint32_t loaded = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!names[i].str || !paths[i].str) {
      out_errors[i] = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      continue;
    }

    if (vkr_font_system_load_from_file(system, names[i], paths[i],
                                        type, &out_errors[i])) {
      // Get handle for the loaded font
      VkrFontSystemEntry *entry = vkr_hash_table_get_VkrFontSystemEntry(
          &system->font_map, (const char *)names[i].str);
      if (entry) {
        VkrFont *font = &system->fonts.data[entry->index];
        out_handles[i] = (VkrFontHandle){
            .id = font->id,
            .generation = font->generation
        };
        loaded++;
      }
    }
  }

  return loaded;
}
```

---

### Phase 4: Acquire/Release Functions

#### 4.1 Acquire by Name

```c
VkrFontHandle vkr_font_system_acquire(VkrFontSystem *system, String8 name,
                                      bool8_t auto_release,
                                      VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  const char *font_key = (const char *)name.str;
  VkrFontSystemEntry *entry =
      vkr_hash_table_get_VkrFontSystemEntry(&system->font_map, font_key);

  if (!entry) {
    log_warn("Font '%s' not loaded, use load_from_file first", font_key);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return VKR_FONT_HANDLE_INVALID;
  }

  // Set auto_release on first acquire
  if (entry->ref_count == 0) {
    entry->auto_release = auto_release;
  }
  entry->ref_count++;

  VkrFont *font = &system->fonts.data[entry->index];
  *out_error = VKR_RENDERER_ERROR_NONE;

  return (VkrFontHandle){
      .id = font->id,
      .generation = font->generation
  };
}
```

#### 4.2 Acquire by Handle

```c
VkrFontHandle vkr_font_system_acquire_by_handle(VkrFontSystem *system,
                                                VkrFontHandle handle,
                                                VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrFont *font = vkr_font_system_get_by_handle(system, handle);
  if (!font) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return VKR_FONT_HANDLE_INVALID;
  }

  // Find entry in hash table by index
  for (uint64_t i = 0; i < system->font_map.capacity; i++) {
    VkrHashEntry_VkrFontSystemEntry *hash_entry = &system->font_map.entries[i];
    if (hash_entry->occupied == VKR_OCCUPIED &&
        hash_entry->value.index == (handle.id - 1)) {
      hash_entry->value.ref_count++;
      *out_error = VKR_RENDERER_ERROR_NONE;
      return handle;
    }
  }

  *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
  return VKR_FONT_HANDLE_INVALID;
}
```

#### 4.3 Release by Name

```c
void vkr_font_system_release(VkrFontSystem *system, String8 name) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  const char *font_key = (const char *)name.str;
  VkrFontSystemEntry *entry =
      vkr_hash_table_get_VkrFontSystemEntry(&system->font_map, font_key);

  if (!entry) {
    log_warn("Attempted to release unknown font '%s'", font_key);
    return;
  }

  if (entry->ref_count == 0) {
    log_warn("Over-release detected for font '%s'", font_key);
    return;
  }

  entry->ref_count--;

  if (entry->ref_count == 0 && entry->auto_release) {
    uint32_t font_index = entry->index;
    VkrFont *font = &system->fonts.data[font_index];

    // Unload via resource system
    VkrResourceHandleInfo handle_info = {
        .type = VKR_RESOURCE_TYPE_BITMAP_FONT,
        .loader_id = vkr_resource_system_get_loader_id(
            VKR_RESOURCE_TYPE_BITMAP_FONT, name),
        .as.custom = NULL,  // Loader finds the result by name
    };
    vkr_resource_system_unload(&handle_info, name);

    // Mark slot as free
    font->generation = VKR_INVALID_ID;

    // Remove from hash table
    // Note: vkr_hash_table doesn't have remove, so we mark entry invalid
    entry->index = VKR_INVALID_ID;
    entry->ref_count = 0;
  }
}
```

#### 4.4 Release by Handle

```c
void vkr_font_system_release_by_handle(VkrFontSystem *system,
                                       VkrFontHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (handle.id == 0 || handle.id == VKR_INVALID_ID) {
    log_warn("Attempted to release invalid font handle");
    return;
  }

  // Find entry by iterating hash table
  for (uint64_t i = 0; i < system->font_map.capacity; i++) {
    VkrHashEntry_VkrFontSystemEntry *hash_entry = &system->font_map.entries[i];
    if (hash_entry->occupied == VKR_OCCUPIED) {
      uint32_t font_index = hash_entry->value.index;
      if (font_index < system->fonts.length) {
        VkrFont *font = &system->fonts.data[font_index];
        if (font->id == handle.id && font->generation == handle.generation) {
          String8 name = string8_create_from_cstr(
              (const uint8_t *)hash_entry->key, strlen(hash_entry->key));
          vkr_font_system_release(system, name);
          return;
        }
      }
    }
  }

  log_warn("Font handle not found in system");
}
```

---

### Phase 5: Getter Functions

#### 5.1 Get by Handle

```c
VkrFont *vkr_font_system_get_by_handle(VkrFontSystem *system,
                                       VkrFontHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (handle.id == 0 || handle.id == VKR_INVALID_ID) {
    return NULL;
  }

  uint32_t index = handle.id - 1;
  if (index >= system->fonts.length) {
    return NULL;
  }

  VkrFont *font = &system->fonts.data[index];
  if (font->generation != handle.generation) {
    return NULL;  // Stale handle
  }

  return font;
}
```

#### 5.2 Get by Name

```c
VkrFont *vkr_font_system_get_by_name(VkrFontSystem *system, String8 name) {
  assert_log(system != NULL, "System is NULL");

  if (!name.str) return NULL;

  const char *font_key = (const char *)name.str;
  VkrFontSystemEntry *entry =
      vkr_hash_table_get_VkrFontSystemEntry(&system->font_map, font_key);

  if (!entry || entry->index == VKR_INVALID_ID) {
    return NULL;
  }

  return &system->fonts.data[entry->index];
}
```

#### 5.3 Get Default Fonts

```c
VkrFont *vkr_font_system_get_default_system_font(VkrFontSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return vkr_font_system_get_by_handle(system,
                                       system->config.default_system_font_handle);
}

VkrFont *vkr_font_system_get_default_bitmap_font(VkrFontSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return vkr_font_system_get_by_handle(system,
                                       system->config.default_bitmap_font_handle);
}
```

---

### Phase 6: Validation Functions

#### 6.1 Validate Atlas

```c
bool8_t vkr_font_system_validate_atlas(VkrFontSystem *system,
                                       VkrFontHandle handle) {
  VkrFont *font = vkr_font_system_get_by_handle(system, handle);
  if (!font) return false_v;

  // Check primary atlas
  if (font->atlas.id == 0 || font->atlas.id == VKR_INVALID_ID) {
    return false_v;
  }

  // Check multi-page atlases if present
  if (font->atlas_pages.data) {
    for (uint32_t i = 0; i < font->atlas_pages.length; i++) {
      VkrTextureHandle page = font->atlas_pages.data[i];
      if (page.id == 0 || page.id == VKR_INVALID_ID) {
        return false_v;
      }
    }
  }

  return true_v;
}
```

#### 6.2 Validate Glyphs

```c
bool8_t vkr_font_system_validate_glyphs(VkrFontSystem *system,
                                        VkrFontHandle handle) {
  VkrFont *font = vkr_font_system_get_by_handle(system, handle);
  if (!font) return false_v;

  // Must have at least one glyph
  if (!font->glyphs.data || font->glyphs.length == 0) {
    return false_v;
  }

  // Check for space glyph (codepoint 32)
  bool8_t has_space = false_v;
  for (uint64_t i = 0; i < font->glyphs.length; i++) {
    if (font->glyphs.data[i].codepoint == 32) {
      has_space = true_v;
      break;
    }
  }

  return has_space;
}
```

#### 6.3 Is Valid

```c
bool8_t vkr_font_system_is_valid(VkrFontSystem *system, VkrFontHandle handle) {
  VkrFont *font = vkr_font_system_get_by_handle(system, handle);
  if (!font) return false_v;

  // Check basic metadata
  if (font->size == 0 || font->line_height <= 0) {
    return false_v;
  }

  // Check atlas
  if (!vkr_font_system_validate_atlas(system, handle)) {
    return false_v;
  }

  // Check glyphs
  if (!vkr_font_system_validate_glyphs(system, handle)) {
    return false_v;
  }

  return true_v;
}
```

---

## Integration Points

### With Text System

```c
// Example usage in text rendering:
VkrFontHandle font_handle = vkr_font_system_acquire(font_system,
                                                     string8_lit("UbuntuMono"),
                                                     true_v, &error);

VkrFont *font = vkr_font_system_get_by_handle(font_system, font_handle);

VkrTextStyle style = vkr_text_style_new(font_handle, 16.0f, VKR_TEXT_COLOR_WHITE);
style = vkr_text_style_with_font_data(&style, font);

VkrTextLayout layout = vkr_text_layout_compute(allocator, &text, &options);
// Render using layout.glyphs and font->atlas
```

### With Bitmap Font Loader

The bitmap font loader is registered with the resource system during initialization:

```c
// During renderer initialization:
VkrBitmapFontLoaderContext font_loader_ctx = {
    .job_system = job_system,
    .arena_pool = arena_pool,
};
VkrResourceLoader font_loader = vkr_bitmap_font_loader_create(&font_loader_ctx);
vkr_resource_system_register_loader(&font_loader_ctx, font_loader);
```

---

## Implementation Checklist

- [ ] `vkr_font_system_find_free_slot()` - Find free slot in fonts array
- [ ] `vkr_font_system_get_by_index()` - Internal helper
- [ ] `vkr_font_system_destroy_font()` - Internal cleanup helper
- [ ] `vkr_font_system_init()` - System initialization
- [ ] `vkr_font_system_shutdown()` - System cleanup
- [ ] `vkr_font_system_load_from_file()` - Load via resource system
- [ ] `vkr_font_system_load_batch()` - Batch loading
- [ ] `vkr_font_system_acquire()` - Acquire by name
- [ ] `vkr_font_system_acquire_by_handle()` - Acquire by handle
- [ ] `vkr_font_system_release()` - Release by name
- [ ] `vkr_font_system_release_by_handle()` - Release by handle
- [ ] `vkr_font_system_get_by_handle()` - Get font pointer
- [ ] `vkr_font_system_get_by_name()` - Get font by name
- [ ] `vkr_font_system_get_default_system_font()` - Get default
- [ ] `vkr_font_system_get_default_bitmap_font()` - Get default bitmap
- [ ] `vkr_font_system_validate_atlas()` - Validate atlas handles
- [ ] `vkr_font_system_validate_glyphs()` - Validate glyph data
- [ ] `vkr_font_system_is_valid()` - Full validation

---

## Notes for Implementation

1. **Handle ID Convention**: `id = array_index + 1` (0 is invalid)
2. **Generation Counter**: Starts at 1, increments on each new font
3. **Free Slot Detection**: `generation == VKR_INVALID_ID` means slot is free
4. **Memory Ownership**: Font system owns copies of font data; loader result can be freed
5. **Atlas Textures**: Owned by texture system, referenced by font system via handles
6. **Hash Table Keys**: Must be allocated with stable lifetime (system allocator)

---

## Error Handling

| Error Condition | Error Code | Action |
|----------------|------------|--------|
| System full | `VKR_RENDERER_ERROR_OUT_OF_MEMORY` | Return false, log error |
| Font not found | `VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED` | Return invalid handle |
| Invalid handle | `VKR_RENDERER_ERROR_INVALID_HANDLE` | Return NULL/invalid |
| Load failure | `VKR_RENDERER_ERROR_FILE_NOT_FOUND` | Propagate from loader |
| Over-release | Log warning | Continue without crash |
