---
status: partial
updated: 2026-07-31
authority: design
---
# System Font Loader Design Document

> **Current boundary.** The system-font loader ships, but this document retains
> planned code and an unchecked historical checklist. Verify loader signatures
> and cleanup behavior against the implementation.

## Overview

This document specifies the implementation of the **System Font Loader** (`system_font_loader.c`), which loads TrueType/OpenType fonts (`.ttf`/`.otf`) using `stb_truetype` and rasterizes them into bitmap atlases at runtime. The loader integrates with the existing font system and resource system architecture.

---

## Table of Contents

1. [Goals and Non-Goals](#goals-and-non-goals)
2. [Architecture Overview](#architecture-overview)
3. [Data Structures](#data-structures)
4. [Implementation Steps](#implementation-steps)
5. [API Reference](#api-reference)
6. [Integration Points](#integration-points)
7. [Error Handling](#error-handling)
8. [Testing Strategy](#testing-strategy)

---

## Goals and Non-Goals

### Goals

- Load TTF/OTF fonts via `stb_truetype`
- Rasterize glyphs to a GPU-uploadable bitmap atlas
- Generate `VkrFont` structure compatible with existing text rendering
- Support configurable font sizes (default: 32px, configurable via fontcfg)
- Support ASCII + extended Latin character sets (codepoints 32-255)
- Generate kerning data when available in the font
- Build glyph index hash table for O(1) lookup
- Follow the same loader pattern as `bitmap_font_loader.c`

### Non-Goals

- Dynamic glyph caching (glyphs are pre-rasterized at load time)
- Runtime font size changes (requires reload)
- Full Unicode support (CJK, emoji) - use bitmap fonts for these
- Font hinting/subpixel rendering
- Font fallback chains

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Font System                                    │
│  vkr_font_system_load_from_file()                                       │
│         │                                                                │
│         ▼ parses .fontcfg, dispatches based on type=system              │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │                    Resource System                                │   │
│  │  vkr_resource_system_load(VKR_RESOURCE_TYPE_SYSTEM_FONT, ...)    │   │
│  │         │                                                         │   │
│  │         ▼                                                         │   │
│  │  ┌────────────────────────────────────────────────────────────┐  │   │
│  │  │              System Font Loader                             │  │   │
│  │  │  1. Read TTF/OTF file into memory                          │  │   │
│  │  │  2. Initialize stbtt_fontinfo                              │  │   │
│  │  │  3. Calculate scale for requested pixel size               │  │   │
│  │  │  4. Rasterize glyphs to temporary bitmaps                  │  │   │
│  │  │  5. Pack glyphs into atlas (row-by-row or rect-pack)       │  │   │
│  │  │  6. Create GPU texture via texture system                  │  │   │
│  │  │  7. Build VkrFont with glyph metrics and atlas handle      │  │   │
│  │  │  8. Return VkrSystemFontLoaderResult                       │  │   │
│  │  └────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Data Structures

### Existing Structures (No Changes Required)

```c
// lib/src/renderer/resources/vkr_resources.h

typedef struct VkrFontGlyph {
  uint32_t codepoint;
  uint16_t x, y;           // Position in atlas
  uint16_t width, height;  // Size in atlas
  int16_t x_offset;        // Render offset from cursor
  int16_t y_offset;        // Render offset from baseline
  int16_t x_advance;       // Cursor advance after glyph
  uint8_t page_id;         // Always 0 for system fonts
} VkrFontGlyph;

typedef struct VkrFont {
  uint32_t id, generation;
  VkrFontType type;              // VKR_FONT_TYPE_SYSTEM
  char face[256];
  uint32_t size;                 // Pixel size
  int32_t line_height;
  int32_t baseline;
  int32_t ascent, descent;
  int32_t atlas_size_x, atlas_size_y;
  uint32_t page_count;           // 1 for system fonts
  VkrTextureHandle atlas;
  Array_VkrTextureHandle atlas_pages;
  VkrHashTable_uint32_t glyph_indices;
  Array_VkrFontGlyph glyphs;
  Array_VkrFontKerning kernings;
  float32_t tab_x_advance;
} VkrFont;
```

### New Structures

```c
// lib/src/renderer/resources/loaders/system_font_loader.h

#define VKR_SYSTEM_FONT_DEFAULT_SIZE 32
#define VKR_SYSTEM_FONT_DEFAULT_ATLAS_SIZE 1024
#define VKR_SYSTEM_FONT_FIRST_CODEPOINT 32
#define VKR_SYSTEM_FONT_LAST_CODEPOINT 255
#define VKR_SYSTEM_FONT_GLYPH_COUNT (VKR_SYSTEM_FONT_LAST_CODEPOINT - VKR_SYSTEM_FONT_FIRST_CODEPOINT + 1)
#define VKR_SYSTEM_FONT_ATLAS_PADDING 1

typedef struct VkrSystemFontLoaderContext {
  VkrJobSystem *job_system;   // Optional, for batch loading
  VkrArenaPool *arena_pool;   // For result allocations
} VkrSystemFontLoaderContext;

typedef struct VkrSystemFontLoaderResult {
  Arena *arena;
  void *pool_chunk;
  VkrAllocator allocator;
  VkrFont font;
  bool8_t success;
  VkrRendererError error;
} VkrSystemFontLoaderResult;

// Internal parsing state
typedef struct VkrSystemFontParseState {
  VkrAllocator *load_allocator;   // Persistent (result arena)
  VkrAllocator *temp_allocator;   // Temporary (scratch)

  // stb_truetype font info
  stbtt_fontinfo font_info;
  uint8_t *font_data;             // Raw TTF data (kept alive during parsing)
  uint64_t font_data_size;

  // Font metrics (in pixels at target size)
  float32_t scale;                // stbtt scale factor
  int32_t ascent;                 // Pixels above baseline
  int32_t descent;                // Pixels below baseline (negative)
  int32_t line_gap;               // Extra line spacing
  int32_t line_height;            // Total line height

  // Rasterization config
  uint32_t font_size;             // Target pixel size
  uint32_t atlas_width;
  uint32_t atlas_height;

  // Output
  Vector_VkrFontGlyph glyphs;
  Vector_VkrFontKerning kernings;
  uint8_t *atlas_bitmap;          // Single-channel grayscale

  VkrRendererError *out_error;
} VkrSystemFontParseState;
```

### Font Config Extension

The `.fontcfg` file format already supports system fonts:

```ini
# Example: assets/fonts/UbuntuMono.fontcfg
type=system
file=UbuntuMono-R.ttf
face=Ubuntu Mono
# Optional: size=32 (default: 32)
```

Optional extension to support font size in config:

```c
// In vkr_font_config_parse(), add handling for:
String8 key_size = string8_lit("size");
if (string8_equalsi(&key, &key_size)) {
  int32_t size_val = 0;
  if (string8_to_i32(&value, &size_val) && size_val > 0) {
    config.size = (uint32_t)size_val;
  }
}
```

---

## Implementation Steps

### Step 1: Header Setup

**File:** `lib/src/renderer/resources/loaders/system_font_loader.h`

Add constants and complete the header:

```c
#pragma once

#include "core/vkr_job_system.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Constants
// =============================================================================

#define VKR_SYSTEM_FONT_DEFAULT_SIZE 32
#define VKR_SYSTEM_FONT_MIN_SIZE 8
#define VKR_SYSTEM_FONT_MAX_SIZE 128
#define VKR_SYSTEM_FONT_DEFAULT_ATLAS_SIZE 1024
#define VKR_SYSTEM_FONT_MAX_ATLAS_SIZE 4096
#define VKR_SYSTEM_FONT_FIRST_CODEPOINT 32
#define VKR_SYSTEM_FONT_LAST_CODEPOINT 255
#define VKR_SYSTEM_FONT_GLYPH_COUNT \
  (VKR_SYSTEM_FONT_LAST_CODEPOINT - VKR_SYSTEM_FONT_FIRST_CODEPOINT + 1)
#define VKR_SYSTEM_FONT_ATLAS_PADDING 1

// ... existing context/result structs ...

VkrResourceLoader
vkr_system_font_loader_create(VkrSystemFontLoaderContext *context);
```

### Step 2: Core Implementation

**File:** `lib/src/renderer/resources/loaders/system_font_loader.c`

```c
#include "renderer/resources/loaders/system_font_loader.h"

#include <stb_truetype.h>

#include "containers/str.h"
#include "containers/vector.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_texture_system.h"

Vector(VkrFontGlyph);
Vector(VkrFontKerning);

// Internal parse state
typedef struct VkrSystemFontParseState {
  VkrAllocator *load_allocator;
  VkrAllocator *temp_allocator;

  stbtt_fontinfo font_info;
  uint8_t *font_data;
  uint64_t font_data_size;

  float32_t scale;
  int32_t ascent;
  int32_t descent;
  int32_t line_gap;
  int32_t line_height;

  uint32_t font_size;
  uint32_t atlas_width;
  uint32_t atlas_height;

  String8 face_name;

  Vector_VkrFontGlyph glyphs;
  Vector_VkrFontKerning kernings;
  uint8_t *atlas_bitmap;

  VkrRendererError *out_error;
} VkrSystemFontParseState;
```

### Step 3: Font File Loading

```c
vkr_internal bool8_t vkr_system_font_read_file(
    VkrSystemFontParseState *state,
    String8 file_path) {

  FilePath fp = file_path_create(
      (const char *)file_path.str,
      state->temp_allocator,
      FILE_PATH_TYPE_RELATIVE);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_error("SystemFontLoader: failed to open '%.*s'",
              (int32_t)file_path.length, file_path.str);
    *state->out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  ferr = file_read_all(&fh, state->temp_allocator,
                       &state->font_data, &state->font_data_size);
  file_close(&fh);

  if (ferr != FILE_ERROR_NONE || !state->font_data) {
    log_error("SystemFontLoader: failed to read '%.*s'",
              (int32_t)file_path.length, file_path.str);
    *state->out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  return true_v;
}
```

### Step 4: Font Initialization with stb_truetype

```c
vkr_internal bool8_t vkr_system_font_init_stbtt(
    VkrSystemFontParseState *state) {

  // Get font offset (handle TTC collections)
  int font_offset = stbtt_GetFontOffsetForIndex(state->font_data, 0);
  if (font_offset < 0) {
    log_error("SystemFontLoader: invalid font file or index");
    *state->out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // Initialize font info
  if (!stbtt_InitFont(&state->font_info, state->font_data, font_offset)) {
    log_error("SystemFontLoader: stbtt_InitFont failed");
    *state->out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // Calculate scale factor for target pixel size
  state->scale = stbtt_ScaleForPixelHeight(
      &state->font_info, (float)state->font_size);

  // Get vertical metrics (in unscaled units)
  int ascent_unscaled, descent_unscaled, line_gap_unscaled;
  stbtt_GetFontVMetrics(&state->font_info,
                        &ascent_unscaled,
                        &descent_unscaled,
                        &line_gap_unscaled);

  // Scale to pixels
  state->ascent = (int32_t)(ascent_unscaled * state->scale + 0.5f);
  state->descent = (int32_t)(descent_unscaled * state->scale - 0.5f);
  state->line_gap = (int32_t)(line_gap_unscaled * state->scale + 0.5f);
  state->line_height = state->ascent - state->descent + state->line_gap;

  return true_v;
}
```

### Step 5: Glyph Rasterization and Atlas Packing

```c
vkr_internal bool8_t vkr_system_font_rasterize_glyphs(
    VkrSystemFontParseState *state) {

  // Allocate atlas bitmap
  uint64_t atlas_size = (uint64_t)state->atlas_width * state->atlas_height;
  state->atlas_bitmap = vkr_allocator_alloc(
      state->temp_allocator, atlas_size, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!state->atlas_bitmap) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(state->atlas_bitmap, atlas_size);

  // Track current position in atlas (simple row packing)
  uint32_t cursor_x = VKR_SYSTEM_FONT_ATLAS_PADDING;
  uint32_t cursor_y = VKR_SYSTEM_FONT_ATLAS_PADDING;
  uint32_t row_height = 0;

  // Rasterize each codepoint
  for (uint32_t cp = VKR_SYSTEM_FONT_FIRST_CODEPOINT;
       cp <= VKR_SYSTEM_FONT_LAST_CODEPOINT; cp++) {

    // Get glyph index
    int glyph_index = stbtt_FindGlyphIndex(&state->font_info, (int)cp);
    if (glyph_index == 0 && cp != ' ') {
      // Missing glyph, skip (or use replacement)
      continue;
    }

    // Get glyph metrics
    int advance_width, left_side_bearing;
    stbtt_GetGlyphHMetrics(&state->font_info, glyph_index,
                           &advance_width, &left_side_bearing);

    // Get glyph bitmap box
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&state->font_info, glyph_index,
                            state->scale, state->scale,
                            &x0, &y0, &x1, &y1);

    int glyph_width = x1 - x0;
    int glyph_height = y1 - y0;

    // Check if glyph fits in current row
    if (cursor_x + glyph_width + VKR_SYSTEM_FONT_ATLAS_PADDING
        > state->atlas_width) {
      // Move to next row
      cursor_x = VKR_SYSTEM_FONT_ATLAS_PADDING;
      cursor_y += row_height + VKR_SYSTEM_FONT_ATLAS_PADDING;
      row_height = 0;
    }

    // Check if atlas is full
    if (cursor_y + glyph_height + VKR_SYSTEM_FONT_ATLAS_PADDING
        > state->atlas_height) {
      log_error("SystemFontLoader: atlas too small for font size %u",
                state->font_size);
      *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }

    // Rasterize glyph to atlas
    if (glyph_width > 0 && glyph_height > 0) {
      uint8_t *dest = state->atlas_bitmap +
                      cursor_y * state->atlas_width + cursor_x;
      stbtt_MakeGlyphBitmap(&state->font_info, dest,
                            glyph_width, glyph_height,
                            (int)state->atlas_width,
                            state->scale, state->scale,
                            glyph_index);
    }

    // Create glyph entry
    VkrFontGlyph glyph = {
      .codepoint = cp,
      .x = (uint16_t)cursor_x,
      .y = (uint16_t)cursor_y,
      .width = (uint16_t)glyph_width,
      .height = (uint16_t)glyph_height,
      .x_offset = (int16_t)x0,
      .y_offset = (int16_t)y0,
      .x_advance = (int16_t)(advance_width * state->scale + 0.5f),
      .page_id = 0,
    };
    vector_push_VkrFontGlyph(&state->glyphs, glyph);

    // Advance cursor
    cursor_x += glyph_width + VKR_SYSTEM_FONT_ATLAS_PADDING;
    if ((uint32_t)glyph_height > row_height) {
      row_height = (uint32_t)glyph_height;
    }
  }

  return true_v;
}
```

### Step 6: Kerning Extraction

```c
vkr_internal void vkr_system_font_extract_kerning(
    VkrSystemFontParseState *state) {

  // Only extract kerning for glyphs we have
  for (uint64_t i = 0; i < state->glyphs.length; i++) {
    for (uint64_t j = 0; j < state->glyphs.length; j++) {
      uint32_t cp1 = state->glyphs.data[i].codepoint;
      uint32_t cp2 = state->glyphs.data[j].codepoint;

      int kern = stbtt_GetCodepointKernAdvance(
          &state->font_info, (int)cp1, (int)cp2);

      if (kern != 0) {
        VkrFontKerning kerning = {
          .codepoint_0 = cp1,
          .codepoint_1 = cp2,
          .amount = (int16_t)(kern * state->scale + 0.5f),
        };
        vector_push_VkrFontKerning(&state->kernings, kerning);
      }
    }
  }
}
```

### Step 7: Atlas Texture Creation

```c
vkr_internal bool8_t vkr_system_font_create_atlas_texture(
    VkrSystemFontParseState *state,
    VkrTextureHandle *out_handle) {

  // Convert single-channel grayscale to RGBA
  uint64_t rgba_size = (uint64_t)state->atlas_width *
                       state->atlas_height * 4;
  uint8_t *rgba_data = vkr_allocator_alloc(
      state->temp_allocator, rgba_size, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!rgba_data) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // Expand grayscale to RGBA (white text with alpha)
  uint64_t pixel_count = (uint64_t)state->atlas_width * state->atlas_height;
  for (uint64_t i = 0; i < pixel_count; i++) {
    uint8_t alpha = state->atlas_bitmap[i];
    rgba_data[i * 4 + 0] = 255;   // R
    rgba_data[i * 4 + 1] = 255;   // G
    rgba_data[i * 4 + 2] = 255;   // B
    rgba_data[i * 4 + 3] = alpha; // A
  }

  // Create texture via resource system
  VkrTextureDescription desc = {
    .width = state->atlas_width,
    .height = state->atlas_height,
    .channel_count = 4,
    .format = VKR_TEXTURE_FORMAT_RGBA8_UNORM,
    .usage = VKR_TEXTURE_USAGE_SAMPLED,
    .filter = VKR_TEXTURE_FILTER_LINEAR,
    .wrap_u = VKR_TEXTURE_WRAP_CLAMP,
    .wrap_v = VKR_TEXTURE_WRAP_CLAMP,
    .is_transparent = true_v,
  };

  // Generate unique texture name
  String8 tex_name = string8_create_formatted(
      state->temp_allocator,
      "font_atlas_%.*s_%u",
      (int32_t)state->face_name.length,
      state->face_name.str,
      state->font_size);

  VkrResourceHandleInfo tex_info = {0};
  VkrRendererError tex_error = VKR_RENDERER_ERROR_NONE;

  // Use texture system directly (not via resource system to avoid caching issues)
  VkrRendererFrontendHandle renderer = vkr_resource_system_get_renderer();
  VkrTextureOpaqueHandle tex_opaque = vkr_renderer_create_texture(
      renderer, &desc, rgba_data, &tex_error);

  if (tex_error != VKR_RENDERER_ERROR_NONE) {
    log_error("SystemFontLoader: failed to create atlas texture");
    *state->out_error = tex_error;
    return false_v;
  }

  *out_handle = (VkrTextureHandle){
    .id = tex_opaque.id,
    .generation = tex_opaque.generation,
  };

  return true_v;
}
```

### Step 8: Build Final VkrFont Result

```c
vkr_internal bool8_t vkr_system_font_build_result(
    VkrSystemFontParseState *state,
    VkrTextureHandle atlas,
    VkrFont *out_font) {

  MemZero(out_font, sizeof(*out_font));

  out_font->type = VKR_FONT_TYPE_SYSTEM;
  out_font->size = state->font_size;
  out_font->line_height = state->line_height;
  out_font->baseline = state->ascent;
  out_font->ascent = state->ascent;
  out_font->descent = state->descent;
  out_font->atlas_size_x = (int32_t)state->atlas_width;
  out_font->atlas_size_y = (int32_t)state->atlas_height;
  out_font->page_count = 1;
  out_font->atlas = atlas;

  // Copy face name
  if (state->face_name.str && state->face_name.length > 0) {
    uint64_t copy_len = state->face_name.length;
    if (copy_len >= sizeof(out_font->face)) {
      copy_len = sizeof(out_font->face) - 1;
    }
    MemCopy(out_font->face, state->face_name.str, copy_len);
    out_font->face[copy_len] = '\0';
  }

  // Copy glyphs
  if (state->glyphs.length == 0) {
    log_error("SystemFontLoader: no glyphs rasterized");
    *state->out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  out_font->glyphs = array_create_VkrFontGlyph(
      state->load_allocator, state->glyphs.length);
  if (!out_font->glyphs.data) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemCopy(out_font->glyphs.data, state->glyphs.data,
          state->glyphs.length * sizeof(VkrFontGlyph));

  // Build glyph index hash table
  uint64_t table_capacity = state->glyphs.length * 2;
  if (table_capacity < VKR_HASH_TABLE_INITIAL_CAPACITY) {
    table_capacity = VKR_HASH_TABLE_INITIAL_CAPACITY;
  }
  out_font->glyph_indices = vkr_hash_table_create_uint32_t(
      state->load_allocator, table_capacity);

  for (uint64_t i = 0; i < out_font->glyphs.length; i++) {
    VkrFontGlyph *glyph = &out_font->glyphs.data[i];
    String8 key = string8_create_formatted(
        state->load_allocator, "%u", glyph->codepoint);
    vkr_hash_table_insert_uint32_t(
        &out_font->glyph_indices, string8_cstr(&key), (uint32_t)i);
  }

  // Copy kernings if any
  if (state->kernings.length > 0) {
    out_font->kernings = array_create_VkrFontKerning(
        state->load_allocator, state->kernings.length);
    if (out_font->kernings.data) {
      MemCopy(out_font->kernings.data, state->kernings.data,
              state->kernings.length * sizeof(VkrFontKerning));
    }
  }

  // Calculate tab advance from space
  VkrFontGlyph *space = NULL;
  for (uint64_t i = 0; i < out_font->glyphs.length; i++) {
    if (out_font->glyphs.data[i].codepoint == 32) {
      space = &out_font->glyphs.data[i];
      break;
    }
  }
  if (space) {
    out_font->tab_x_advance = (float32_t)space->x_advance * 4.0f;
  } else {
    out_font->tab_x_advance = (float32_t)out_font->size * 2.0f;
  }

  // Single-page atlas
  out_font->atlas_pages = array_create_VkrTextureHandle(
      state->load_allocator, 1);
  if (out_font->atlas_pages.data) {
    out_font->atlas_pages.data[0] = atlas;
  }

  return true_v;
}
```

### Step 9: Loader Interface Implementation

```c
vkr_internal bool8_t vkr_system_font_loader_can_load(
    VkrResourceLoader *self,
    String8 name) {
  (void)self;

  // Check for .ttf or .otf extension
  for (uint64_t i = name.length; i > 0; --i) {
    if (name.str[i - 1] == '.') {
      String8 ext = string8_substring(&name, i, name.length);
      String8 ttf = string8_lit("ttf");
      String8 otf = string8_lit("otf");
      return string8_equalsi(&ext, &ttf) || string8_equalsi(&ext, &otf);
    }
  }
  return false_v;
}

vkr_internal bool8_t vkr_system_font_loader_load(
    VkrResourceLoader *self,
    String8 name,
    VkrAllocator *temp_alloc,
    VkrResourceHandleInfo *out_handle,
    VkrRendererError *out_error) {

  VkrSystemFontLoaderContext *context =
      (VkrSystemFontLoaderContext *)self->resource_system;

  // Begin temp scope
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(temp_alloc);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // Acquire arena from pool
  void *pool_chunk = NULL;
  Arena *result_arena = NULL;
  if (context->arena_pool && context->arena_pool->initialized) {
    pool_chunk = vkr_arena_pool_acquire(context->arena_pool);
    if (!pool_chunk) {
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }
    result_arena = arena_create_from_buffer(
        pool_chunk, context->arena_pool->chunk_size);
  } else {
    log_fatal("SystemFontLoader: arena pool not initialized");
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrAllocator result_alloc = {.ctx = result_arena};
  vkr_allocator_arena(&result_alloc);

  // Allocate result
  VkrSystemFontLoaderResult *result = vkr_allocator_alloc(
      &result_alloc, sizeof(VkrSystemFontLoaderResult),
      VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!result) {
    arena_destroy(result_arena);
    if (pool_chunk) vkr_arena_pool_release(context->arena_pool, pool_chunk);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  MemZero(result, sizeof(*result));
  result->arena = result_arena;
  result->pool_chunk = pool_chunk;
  result->allocator = result_alloc;

  // Initialize parse state
  VkrSystemFontParseState state = {
    .load_allocator = &result->allocator,
    .temp_allocator = temp_alloc,
    .font_size = VKR_SYSTEM_FONT_DEFAULT_SIZE,
    .atlas_width = VKR_SYSTEM_FONT_DEFAULT_ATLAS_SIZE,
    .atlas_height = VKR_SYSTEM_FONT_DEFAULT_ATLAS_SIZE,
    .out_error = out_error,
  };
  state.glyphs = vector_create_VkrFontGlyph(temp_alloc);
  state.kernings = vector_create_VkrFontKerning(temp_alloc);

  // Extract face name from file path (TODO: read from TTF name table)
  state.face_name = file_path_get_filename_without_extension(temp_alloc, name);

  // Load and parse font
  if (!vkr_system_font_read_file(&state, name)) {
    goto fail;
  }

  if (!vkr_system_font_init_stbtt(&state)) {
    goto fail;
  }

  if (!vkr_system_font_rasterize_glyphs(&state)) {
    goto fail;
  }

  vkr_system_font_extract_kerning(&state);

  VkrTextureHandle atlas = VKR_TEXTURE_HANDLE_INVALID;
  if (!vkr_system_font_create_atlas_texture(&state, &atlas)) {
    goto fail;
  }

  if (!vkr_system_font_build_result(&state, atlas, &result->font)) {
    // TODO: destroy atlas texture on failure
    goto fail;
  }

  result->success = true_v;
  result->error = VKR_RENDERER_ERROR_NONE;

  out_handle->type = VKR_RESOURCE_TYPE_SYSTEM_FONT;
  out_handle->loader_id = self->id;
  out_handle->as.custom = result;
  *out_error = VKR_RENDERER_ERROR_NONE;

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;

fail:
  arena_destroy(result_arena);
  if (pool_chunk && context->arena_pool) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
  }
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return false_v;
}

vkr_internal void vkr_system_font_loader_unload(
    VkrResourceLoader *self,
    const VkrResourceHandleInfo *handle,
    String8 name) {

  (void)name;
  VkrSystemFontLoaderContext *context =
      (VkrSystemFontLoaderContext *)self->resource_system;
  VkrSystemFontLoaderResult *result =
      (VkrSystemFontLoaderResult *)handle->as.custom;

  if (!result) return;

  VkrFont *font = &result->font;

  // Destroy atlas texture
  if (font->atlas.id != 0 && font->atlas.id != VKR_INVALID_ID) {
    VkrRendererFrontendHandle renderer = vkr_resource_system_get_renderer();
    vkr_renderer_destroy_texture(renderer, (VkrTextureOpaqueHandle){
      .id = font->atlas.id,
      .generation = font->atlas.generation,
    });
  }

  // Cleanup font data
  if (font->glyph_indices.entries) {
    vkr_hash_table_destroy_uint32_t(&font->glyph_indices);
  }
  if (font->glyphs.data) {
    array_destroy_VkrFontGlyph(&font->glyphs);
  }
  if (font->kernings.data) {
    array_destroy_VkrFontKerning(&font->kernings);
  }
  if (font->atlas_pages.data) {
    array_destroy_VkrTextureHandle(&font->atlas_pages);
  }

  // Release arena
  void *pool_chunk = result->pool_chunk;
  Arena *arena = result->arena;

  if (arena) arena_destroy(arena);
  if (pool_chunk && context && context->arena_pool) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
  }
}

vkr_internal uint32_t vkr_system_font_loader_batch_load(
    VkrResourceLoader *self,
    const String8 *paths,
    uint32_t count,
    VkrAllocator *temp_alloc,
    VkrResourceHandleInfo *out_handles,
    VkrRendererError *out_errors) {

  if (count == 0) return 0;

  uint32_t loaded = 0;
  for (uint32_t i = 0; i < count; i++) {
    out_handles[i].type = VKR_RESOURCE_TYPE_UNKNOWN;
    out_handles[i].loader_id = VKR_INVALID_ID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;

    if (vkr_system_font_loader_load(self, paths[i], temp_alloc,
                                     &out_handles[i], &out_errors[i])) {
      loaded++;
    }
  }
  return loaded;
}

VkrResourceLoader vkr_system_font_loader_create(
    VkrSystemFontLoaderContext *context) {
  return (VkrResourceLoader){
    .type = VKR_RESOURCE_TYPE_SYSTEM_FONT,
    .resource_system = context,
    .can_load = vkr_system_font_loader_can_load,
    .load = vkr_system_font_loader_load,
    .unload = vkr_system_font_loader_unload,
    .batch_load = vkr_system_font_loader_batch_load,
  };
}
```

---

## Integration Points

### 1. Font System Dispatch

In `lib/src/renderer/systems/vkr_font_system.c`, the system font path is already stubbed:

```c
case VKR_FONT_TYPE_SYSTEM:
  resource_type = VKR_RESOURCE_TYPE_SYSTEM_FONT;
  break;
```

This needs to be implemented to call `vkr_resource_system_load()` similar to bitmap fonts.

### 2. Resource System Registration

Register the loader in the initialization sequence (likely in `renderer_frontend.c` or wherever loaders are registered):

```c
VkrSystemFontLoaderContext system_font_ctx = {
  .job_system = job_system,
  .arena_pool = &system_font_arena_pool,
};
VkrResourceLoader system_font_loader =
    vkr_system_font_loader_create(&system_font_ctx);
vkr_resource_system_register_loader(&system_font_ctx, system_font_loader);
```

### 3. Font Config Size Extension

Add optional `size=` parsing in `vkr_font_config_parse()`:

```c
// Add to VkrFontConfig struct:
uint32_t size;  // Font size in pixels (0 = use default)

// Add parsing in the loop:
String8 key_size = string8_lit("size");
if (string8_equalsi(&key, &key_size)) {
  int32_t size_val = 0;
  if (string8_to_i32(&value, &size_val) && size_val > 0) {
    config.size = (uint32_t)size_val;
  }
}
```

Then pass `config.size` to the loader (requires extending the load interface or using the face/config approach).

---

## Error Handling

| Error | Cause | Recovery |
|-------|-------|----------|
| `FILE_NOT_FOUND` | TTF file doesn't exist | Log error, return failure |
| `INVALID_PARAMETER` | Invalid/corrupt TTF file | Log error, return failure |
| `OUT_OF_MEMORY` | Atlas too small or arena exhausted | Try larger atlas, or fail |
| `RESOURCE_CREATION_FAILED` | GPU texture creation failed | Log error, return failure |

---

## Testing Strategy

### Unit Tests

1. **Font file loading**: Verify TTF files load correctly
2. **Glyph rasterization**: Check all ASCII glyphs are rasterized
3. **Atlas packing**: Verify no glyph overlap, all fit in atlas
4. **Metrics accuracy**: Compare stbtt metrics to expected values

### Integration Tests

1. **Font config parsing**: Load font via `.fontcfg` with `type=system`
2. **Text rendering**: Render sample text with system font
3. **Memory cleanup**: Verify no leaks on load/unload cycle

### Visual Tests

1. **Atlas inspection**: Dump atlas bitmap to file for visual verification
2. **Render comparison**: Compare rendered text to reference images

---

## Implementation Checklist

- [ ] Add constants to `system_font_loader.h`
- [ ] Implement `VkrSystemFontParseState` and helper functions
- [ ] Implement `vkr_system_font_read_file()`
- [ ] Implement `vkr_system_font_init_stbtt()`
- [ ] Implement `vkr_system_font_rasterize_glyphs()`
- [ ] Implement `vkr_system_font_extract_kerning()`
- [ ] Implement `vkr_system_font_create_atlas_texture()`
- [ ] Implement `vkr_system_font_build_result()`
- [ ] Implement loader interface functions
- [ ] Implement `vkr_system_font_loader_create()`
- [ ] Register loader in resource system initialization
- [ ] Update font system dispatch for `VKR_FONT_TYPE_SYSTEM`
- [ ] Add `size=` support to font config parser (optional)
- [ ] Add `vkr_resource_system_get_renderer()` helper if needed
- [ ] Test with UbuntuMono.fontcfg
- [ ] Verify text rendering output

---

## Future Enhancements

1. **Dynamic atlas sizing**: Start small, grow as needed
2. **SDF rendering**: Use `stbtt_GetGlyphSDF()` for scalable text
3. **Font size caching**: Multiple sizes from same TTF without reload
4. **Unicode range support**: Configurable codepoint ranges
5. **Font name extraction**: Read name from TTF name table
6. **Subpixel positioning**: For smoother small text
