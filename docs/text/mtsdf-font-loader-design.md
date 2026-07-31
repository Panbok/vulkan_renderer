---
status: partial
updated: 2026-07-31
authority: design
---
# MTSDF Font Loader Design Document

**Legacy note:** This document references the deprecated view/layer system in
its integration checklist. Render orchestration now uses the render graph;
view modules are render helpers invoked by pass executors. The feature ships,
but the historical snippets and unchecked checklist have not been fully
reconciled with current code.

## Overview

This document specifies the implementation of:
1. **MTSDF Font Loader** (`mtsdf_font_loader.c`) - Loads pre-generated multi-channel signed distance field fonts
2. **3D Text Rendering** - Rendering text in 3D world space using writable textures

These are **two separate efforts** that can be implemented independently.

---

## Table of Contents

1. [Part 1: MTSDF Font Loader](#part-1-mtsdf-font-loader)
   - [Goals and Non-Goals](#goals-and-non-goals)
   - [Input Format Analysis](#input-format-analysis)
   - [Simplified JSON Parsing](#simplified-json-parsing)
   - [Data Structures](#data-structures)
   - [Implementation Steps](#implementation-steps)
   - [Unified Text Shader](#unified-text-shader-bitmap--mtsdf)
2. [Part 2: 3D Text Rendering](#part-2-3d-text-rendering)
   - [Architecture Overview](#architecture-overview-1)
   - [Writable Texture Approach](#writable-texture-approach)
   - [Implementation Steps](#implementation-steps-1)
3. [Implementation Checklist](#implementation-checklist)

---

# Part 1: MTSDF Font Loader

## Goals and Non-Goals

### Goals

- Load MTSDF font metadata from JSON (msdf-atlas-gen format)
- Load pre-generated MTSDF atlas texture (PNG)
- Generate `VkrFont` structure compatible with text rendering
- Use simplified JSON field matching (not a full parser)
- Support configurable distance range for shader

### Non-Goals

- Full JSON parser implementation
- Runtime MTSDF generation
- Font editing/modification
- Nested JSON object support (beyond what's needed)

---

## Input Format Analysis

### Font Config (`.fontcfg`)

```ini
# assets/fonts/UbuntuMono-mtsdf.fontcfg
type=mtsdf
file=UbuntuMono-metadata.json
atlas=UbuntuMono-mtsdf-atlas.png
face=Ubuntu Mono
```

### Metadata JSON Structure

```json
{
  "atlas": {
    "type": "mtsdf",
    "distanceRange": 3,
    "size": 203.75,
    "width": 1024,
    "height": 1024,
    "yOrigin": "bottom"
  },
  "metrics": {
    "emSize": 1,
    "lineHeight": 1,
    "ascender": 0.83,
    "descender": -0.17,
    "underlineY": -0.133,
    "underlineThickness": 0.02
  },
  "glyphs": [
    {
      "unicode": 32,
      "advance": 0.5
    },
    {
      "unicode": 65,
      "advance": 0.5,
      "planeBounds": {
        "left": -0.0003,
        "bottom": -0.0074,
        "right": 0.5003,
        "top": 0.6307
      },
      "atlasBounds": {
        "left": 697.5,
        "bottom": 292.5,
        "right": 799.5,
        "top": 422.5
      }
    }
  ],
  "kerning": []
}
```

### Key Fields to Extract

| Section | Field | Type | Description |
|---------|-------|------|-------------|
| `atlas` | `distanceRange` | float | SDF distance range in pixels |
| `atlas` | `size` | float | Font size used to generate atlas |
| `atlas` | `width` | int | Atlas width in pixels |
| `atlas` | `height` | int | Atlas height in pixels |
| `atlas` | `yOrigin` | string | "bottom" or "top" |
| `metrics` | `lineHeight` | float | Normalized line height |
| `metrics` | `ascender` | float | Normalized ascender |
| `metrics` | `descender` | float | Normalized descender |
| `glyphs[]` | `unicode` | int | Codepoint |
| `glyphs[]` | `advance` | float | Normalized advance width |
| `glyphs[]` | `planeBounds` | object | Quad bounds (normalized) |
| `glyphs[]` | `atlasBounds` | object | UV bounds in pixels |
| `kerning[]` | `unicode1` | int | First codepoint |
| `kerning[]` | `unicode2` | int | Second codepoint |
| `kerning[]` | `advance` | float | Kerning amount |

---

## Simplified JSON Parsing

Instead of implementing a full JSON parser, we use a **field-matching approach**. This is implemented as a reusable module in `/core` for use by any part of the codebase.

### Strategy

1. Read entire JSON file into memory
2. Use pattern matching to find specific field names
3. Extract values using simple string parsing
4. Handle arrays by finding `[` and iterating through `{` blocks

### Module Location

```
lib/src/core/
├── vkr_json.h    # Header with VkrJsonReader and public API
└── vkr_json.c    # Implementation
```

### JSON Field Matcher API

```c
// lib/src/core/vkr_json.h

#pragma once

#include "defines.h"
#include "containers/str.h"

// =============================================================================
// JSON Reader - Lightweight field-matching JSON parser
// =============================================================================

/**
 * @brief Lightweight JSON reader for field matching.
 *
 * This is NOT a full JSON parser. It provides simple field-matching
 * functionality for extracting specific values from JSON data.
 *
 * Usage:
 *   VkrJsonReader reader = vkr_json_reader_create(data, length);
 *   if (vkr_json_find_field(&reader, "fieldName")) {
 *     float32_t value;
 *     vkr_json_parse_float(&reader, &value);
 *   }
 */
typedef struct VkrJsonReader {
  const uint8_t *data;   // JSON data buffer (not owned)
  uint64_t length;       // Length of data buffer
  uint64_t pos;          // Current read position
} VkrJsonReader;

// =============================================================================
// Creation
// =============================================================================

/**
 * @brief Creates a JSON reader from a data buffer.
 * @param data Pointer to JSON data (not copied, must remain valid)
 * @param length Length of the data buffer
 * @return Initialized JSON reader
 */
VkrJsonReader vkr_json_reader_create(const uint8_t *data, uint64_t length);

/**
 * @brief Creates a JSON reader from a String8.
 * @param str String containing JSON data
 * @return Initialized JSON reader
 */
VkrJsonReader vkr_json_reader_from_string(String8 str);

/**
 * @brief Resets reader position to start.
 * @param reader The reader to reset
 */
void vkr_json_reader_reset(VkrJsonReader *reader);

// =============================================================================
// Navigation
// =============================================================================

/**
 * @brief Skips whitespace at current position.
 * @param reader The reader
 */
void vkr_json_skip_whitespace(VkrJsonReader *reader);

/**
 * @brief Skips to a specific character.
 * @param reader The reader
 * @param target Character to skip to
 */
void vkr_json_skip_to(VkrJsonReader *reader, uint8_t target);

/**
 * @brief Finds a field by name and positions reader after ':'.
 * @param reader The reader
 * @param field_name Name of the field to find (without quotes)
 * @return true if field found, reader positioned at value
 */
bool8_t vkr_json_find_field(VkrJsonReader *reader, const char *field_name);

/**
 * @brief Finds an array field and positions reader at first element.
 * @param reader The reader
 * @param array_name Name of the array field
 * @return true if array found
 */
bool8_t vkr_json_find_array(VkrJsonReader *reader, const char *array_name);

/**
 * @brief Advances to the next object in an array.
 * @param reader The reader (must be inside an array)
 * @return true if next element found, false if end of array
 */
bool8_t vkr_json_next_array_element(VkrJsonReader *reader);

/**
 * @brief Creates a sub-reader for the current object scope.
 * @param reader The parent reader (must be at '{')
 * @param out_sub_reader Output sub-reader covering the object
 * @return true if object scope extracted
 */
bool8_t vkr_json_enter_object(VkrJsonReader *reader, VkrJsonReader *out_sub_reader);

// =============================================================================
// Value Parsing
// =============================================================================

/**
 * @brief Parses a float value at current position.
 * @param reader The reader
 * @param out_value Output float value
 * @return true if parsed successfully
 */
bool8_t vkr_json_parse_float(VkrJsonReader *reader, float32_t *out_value);

/**
 * @brief Parses a double value at current position.
 * @param reader The reader
 * @param out_value Output double value
 * @return true if parsed successfully
 */
bool8_t vkr_json_parse_double(VkrJsonReader *reader, float64_t *out_value);

/**
 * @brief Parses an integer value at current position.
 * @param reader The reader
 * @param out_value Output integer value
 * @return true if parsed successfully
 */
bool8_t vkr_json_parse_int(VkrJsonReader *reader, int32_t *out_value);

/**
 * @brief Parses a string value at current position.
 *
 * Returns a view into the original buffer (not copied).
 * The string does NOT include quotes.
 *
 * @param reader The reader
 * @param out_value Output string (points into original buffer)
 * @return true if parsed successfully
 */
bool8_t vkr_json_parse_string(VkrJsonReader *reader, String8 *out_value);

/**
 * @brief Parses a boolean value at current position.
 * @param reader The reader
 * @param out_value Output boolean value
 * @return true if parsed successfully
 */
bool8_t vkr_json_parse_bool(VkrJsonReader *reader, bool8_t *out_value);

// =============================================================================
// Convenience Functions
// =============================================================================

/**
 * @brief Finds a field and parses its float value.
 * @param reader The reader
 * @param field_name Name of the field
 * @param out_value Output value
 * @return true if field found and parsed
 */
bool8_t vkr_json_get_float(VkrJsonReader *reader, const char *field_name,
                            float32_t *out_value);

/**
 * @brief Finds a field and parses its integer value.
 * @param reader The reader
 * @param field_name Name of the field
 * @param out_value Output value
 * @return true if field found and parsed
 */
bool8_t vkr_json_get_int(VkrJsonReader *reader, const char *field_name,
                          int32_t *out_value);

/**
 * @brief Finds a field and parses its string value.
 * @param reader The reader
 * @param field_name Name of the field
 * @param out_value Output value (points into original buffer)
 * @return true if field found and parsed
 */
bool8_t vkr_json_get_string(VkrJsonReader *reader, const char *field_name,
                             String8 *out_value);
```

### Implementation

```c
// lib/src/core/vkr_json.c

#include "core/vkr_json.h"
#include "containers/str.h"

VkrJsonReader vkr_json_reader_create(const uint8_t *data, uint64_t length) {
  return (VkrJsonReader){
    .data = data,
    .length = length,
    .pos = 0
  };
}

VkrJsonReader vkr_json_reader_from_string(String8 str) {
  return vkr_json_reader_create(str.str, str.length);
}

void vkr_json_reader_reset(VkrJsonReader *reader) {
  reader->pos = 0;
}

void vkr_json_skip_whitespace(VkrJsonReader *reader) {
  while (reader->pos < reader->length) {
    uint8_t c = reader->data[reader->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      reader->pos++;
    } else {
      break;
    }
  }
}

void vkr_json_skip_to(VkrJsonReader *reader, uint8_t target) {
  while (reader->pos < reader->length && reader->data[reader->pos] != target) {
    reader->pos++;
  }
}

bool8_t vkr_json_find_field(VkrJsonReader *reader, const char *field_name) {
  uint64_t field_len = string_length(field_name);
  uint64_t saved_pos = reader->pos;

  while (reader->pos < reader->length) {
    if (reader->data[reader->pos] == '"') {
      uint64_t start = reader->pos + 1;
      reader->pos++;

      // Find closing quote (handle escapes)
      while (reader->pos < reader->length && reader->data[reader->pos] != '"') {
        if (reader->data[reader->pos] == '\\') reader->pos++;
        reader->pos++;
      }

      uint64_t end = reader->pos;
      reader->pos++; // skip closing quote

      // Check if field name matches
      if (end - start == field_len &&
          MemCompare(reader->data + start, field_name, field_len) == 0) {
        vkr_json_skip_whitespace(reader);
        if (reader->pos < reader->length && reader->data[reader->pos] == ':') {
          reader->pos++;
          vkr_json_skip_whitespace(reader);
          return true_v;
        }
      }
    } else {
      reader->pos++;
    }
  }

  reader->pos = saved_pos;
  return false_v;
}

bool8_t vkr_json_parse_float(VkrJsonReader *reader, float32_t *out_value) {
  float64_t val;
  if (!vkr_json_parse_double(reader, &val)) return false_v;
  *out_value = (float32_t)val;
  return true_v;
}

bool8_t vkr_json_parse_double(VkrJsonReader *reader, float64_t *out_value) {
  vkr_json_skip_whitespace(reader);

  uint64_t start = reader->pos;

  // Handle sign
  if (reader->pos < reader->length &&
      (reader->data[reader->pos] == '-' || reader->data[reader->pos] == '+')) {
    reader->pos++;
  }

  // Parse digits and decimal
  while (reader->pos < reader->length) {
    uint8_t c = reader->data[reader->pos];
    if ((c >= '0' && c <= '9') || c == '.') {
      reader->pos++;
    } else if (c == 'e' || c == 'E') {
      reader->pos++;
      if (reader->pos < reader->length &&
          (reader->data[reader->pos] == '-' || reader->data[reader->pos] == '+')) {
        reader->pos++;
      }
      while (reader->pos < reader->length &&
             reader->data[reader->pos] >= '0' &&
             reader->data[reader->pos] <= '9') {
        reader->pos++;
      }
      break;
    } else {
      break;
    }
  }

  if (reader->pos == start) return false_v;

  String8 num_str = {.str = reader->data + start, .length = reader->pos - start};
  return string8_to_f64(&num_str, out_value);
}

bool8_t vkr_json_parse_int(VkrJsonReader *reader, int32_t *out_value) {
  float32_t val;
  if (!vkr_json_parse_float(reader, &val)) return false_v;
  *out_value = (int32_t)val;
  return true_v;
}

bool8_t vkr_json_parse_string(VkrJsonReader *reader, String8 *out_value) {
  vkr_json_skip_whitespace(reader);

  if (reader->pos >= reader->length || reader->data[reader->pos] != '"') {
    return false_v;
  }

  reader->pos++; // skip opening quote
  uint64_t start = reader->pos;

  while (reader->pos < reader->length && reader->data[reader->pos] != '"') {
    if (reader->data[reader->pos] == '\\') reader->pos++;
    reader->pos++;
  }

  out_value->str = reader->data + start;
  out_value->length = reader->pos - start;

  if (reader->pos < reader->length) reader->pos++; // skip closing quote

  return true_v;
}

bool8_t vkr_json_parse_bool(VkrJsonReader *reader, bool8_t *out_value) {
  vkr_json_skip_whitespace(reader);

  if (reader->pos + 4 <= reader->length &&
      MemCompare(reader->data + reader->pos, "true", 4) == 0) {
    reader->pos += 4;
    *out_value = true_v;
    return true_v;
  }

  if (reader->pos + 5 <= reader->length &&
      MemCompare(reader->data + reader->pos, "false", 5) == 0) {
    reader->pos += 5;
    *out_value = false_v;
    return true_v;
  }

  return false_v;
}

bool8_t vkr_json_find_array(VkrJsonReader *reader, const char *array_name) {
  if (!vkr_json_find_field(reader, array_name)) return false_v;
  vkr_json_skip_to(reader, '[');
  if (reader->pos < reader->length) {
    reader->pos++; // skip '['
    return true_v;
  }
  return false_v;
}

bool8_t vkr_json_next_array_element(VkrJsonReader *reader) {
  vkr_json_skip_whitespace(reader);

  // Check for end of array
  if (reader->pos >= reader->length || reader->data[reader->pos] == ']') {
    return false_v;
  }

  // Skip comma if present
  if (reader->data[reader->pos] == ',') {
    reader->pos++;
    vkr_json_skip_whitespace(reader);
  }

  // Check for object start
  if (reader->pos < reader->length && reader->data[reader->pos] == '{') {
    return true_v;
  }

  return false_v;
}

bool8_t vkr_json_enter_object(VkrJsonReader *reader, VkrJsonReader *out_sub_reader) {
  vkr_json_skip_whitespace(reader);

  if (reader->pos >= reader->length || reader->data[reader->pos] != '{') {
    return false_v;
  }

  uint64_t obj_start = reader->pos;
  int brace_depth = 1;
  reader->pos++;

  while (reader->pos < reader->length && brace_depth > 0) {
    if (reader->data[reader->pos] == '{') brace_depth++;
    else if (reader->data[reader->pos] == '}') brace_depth--;
    reader->pos++;
  }

  *out_sub_reader = (VkrJsonReader){
    .data = reader->data + obj_start,
    .length = reader->pos - obj_start,
    .pos = 0
  };

  return true_v;
}

// Convenience functions
bool8_t vkr_json_get_float(VkrJsonReader *reader, const char *field_name,
                            float32_t *out_value) {
  uint64_t saved_pos = reader->pos;
  if (vkr_json_find_field(reader, field_name)) {
    if (vkr_json_parse_float(reader, out_value)) {
      return true_v;
    }
  }
  reader->pos = saved_pos;
  return false_v;
}

bool8_t vkr_json_get_int(VkrJsonReader *reader, const char *field_name,
                          int32_t *out_value) {
  uint64_t saved_pos = reader->pos;
  if (vkr_json_find_field(reader, field_name)) {
    if (vkr_json_parse_int(reader, out_value)) {
      return true_v;
    }
  }
  reader->pos = saved_pos;
  return false_v;
}

bool8_t vkr_json_get_string(VkrJsonReader *reader, const char *field_name,
                             String8 *out_value) {
  uint64_t saved_pos = reader->pos;
  if (vkr_json_find_field(reader, field_name)) {
    if (vkr_json_parse_string(reader, out_value)) {
      return true_v;
    }
  }
  reader->pos = saved_pos;
  return false_v;
}
```

---

## Data Structures

### MTSDF-Specific Structures

```c
// lib/src/renderer/resources/loaders/mtsdf_font_loader.h

#pragma once

#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Constants
// =============================================================================

#define VKR_MTSDF_FONT_MAX_GLYPHS 65536
#define VKR_MTSDF_FONT_MAX_KERNINGS 65536

// =============================================================================
// MTSDF Font Loader Types
// =============================================================================

/**
 * @brief MTSDF-specific glyph data (normalized coordinates).
 */
typedef struct VkrMtsdfGlyph {
  uint32_t unicode;
  float32_t advance;           // Normalized advance

  // Plane bounds (normalized quad in EM space)
  float32_t plane_left;
  float32_t plane_bottom;
  float32_t plane_right;
  float32_t plane_top;

  // Atlas bounds (pixel coordinates in atlas)
  float32_t atlas_left;
  float32_t atlas_bottom;
  float32_t atlas_right;
  float32_t atlas_top;

  bool8_t has_geometry;        // false for space-like glyphs
} VkrMtsdfGlyph;
Array(VkrMtsdfGlyph);

/**
 * @brief MTSDF font metadata.
 */
typedef struct VkrMtsdfFontMetadata {
  // Atlas info
  float32_t distance_range;    // SDF distance range (for shader)
  float32_t em_size;           // Size used to generate atlas
  uint32_t atlas_width;
  uint32_t atlas_height;
  bool8_t y_origin_bottom;     // true if yOrigin = "bottom"

  // Metrics (normalized to EM)
  float32_t line_height;
  float32_t ascender;
  float32_t descender;
  float32_t underline_y;
  float32_t underline_thickness;

  // Glyphs and kerning
  Array_VkrMtsdfGlyph glyphs;
  Array_VkrFontKerning kernings;
} VkrMtsdfFontMetadata;

/**
 * @brief MTSDF font loader context.
 */
typedef struct VkrMtsdfFontLoaderContext {
  VkrJobSystem *job_system;
  VkrArenaPool *arena_pool;
  VkrTextureSystem *texture_system;
} VkrMtsdfFontLoaderContext;

/**
 * @brief MTSDF font loader result.
 */
typedef struct VkrMtsdfFontLoaderResult {
  Arena *arena;
  void *pool_chunk;
  VkrAllocator allocator;
  VkrFont font;
  VkrMtsdfFontMetadata metadata;     // MTSDF-specific data
  String8 atlas_texture_name;
  bool8_t success;
  VkrRendererError error;
} VkrMtsdfFontLoaderResult;

// =============================================================================
// Resource Loader Factory
// =============================================================================

VkrResourceLoader
vkr_mtsdf_font_loader_create(VkrMtsdfFontLoaderContext *context);
```

### VkrFont Extensions

The existing `VkrFont` structure needs minimal changes:

```c
// Consider adding to VkrFont or storing in loader result:
typedef struct VkrFont {
  // ... existing fields ...

  // MTSDF-specific (only valid when type == VKR_FONT_TYPE_MTSDF)
  float32_t sdf_distance_range;  // For shader uniforms
  float32_t em_size;             // Original EM size
} VkrFont;
```

Alternatively, store MTSDF metadata in the loader result and access via `VkrMtsdfFontLoaderResult` when needed.

---

## Implementation Steps

### Step 1: Header and Constants

**File:** `lib/src/renderer/resources/loaders/mtsdf_font_loader.h`

See [Data Structures](#data-structures) above.

### Step 2: Core Implementation Setup

```c
// lib/src/renderer/resources/loaders/mtsdf_font_loader.c

#include "renderer/resources/loaders/mtsdf_font_loader.h"

#include "containers/str.h"
#include "containers/vector.h"
#include "core/logger.h"
#include "core/vkr_json.h"  // Reusable JSON field matcher
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_texture_system.h"

Vector(VkrMtsdfGlyph);
Vector(VkrFontKerning);
```

The JSON parsing uses the `vkr_json.h` module from `/core`. See [Simplified JSON Parsing](#simplified-json-parsing) for the full API.

### Step 3: Parse Atlas Section

```c
vkr_internal bool8_t vkr_mtsdf_parse_atlas(VkrJsonReader *reader,
                                            VkrMtsdfFontMetadata *metadata) {
  // Find "atlas" object
  reader->pos = 0;
  if (!vkr_json_find_field(reader, "atlas")) {
    log_error("MtsdfFontLoader: missing 'atlas' field");
    return false_v;
  }

  // Skip to object start
  vkr_json_skip_to(reader, '{');
  uint64_t atlas_start = reader->pos;

  // Find distanceRange
  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "distanceRange")) {
    vkr_json_parse_float(reader, &metadata->distance_range);
  }

  // Find size (em size)
  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "size")) {
    vkr_json_parse_float(reader, &metadata->em_size);
  }

  // Find width
  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "width")) {
    int32_t w = 0;
    vkr_json_parse_int(reader, &w);
    metadata->atlas_width = (uint32_t)w;
  }

  // Find height
  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "height")) {
    int32_t h = 0;
    vkr_json_parse_int(reader, &h);
    metadata->atlas_height = (uint32_t)h;
  }

  // Find yOrigin
  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "yOrigin")) {
    String8 origin = {0};
    if (vkr_json_parse_string(reader, &origin)) {
      String8 bottom = string8_lit("bottom");
      metadata->y_origin_bottom = string8_equalsi(&origin, &bottom);
    }
  }

  return true_v;
}
```

### Step 4: Parse Metrics Section

```c
vkr_internal bool8_t vkr_mtsdf_parse_metrics(VkrJsonReader *reader,
                                              VkrMtsdfFontMetadata *metadata) {
  reader->pos = 0;
  if (!vkr_json_find_field(reader, "metrics")) {
    log_error("MtsdfFontLoader: missing 'metrics' field");
    return false_v;
  }

  vkr_json_skip_to(reader, '{');
  uint64_t metrics_start = reader->pos;

  reader->pos = metrics_start;
  if (vkr_json_find_field(reader, "lineHeight")) {
    vkr_json_parse_float(reader, &metadata->line_height);
  }

  reader->pos = metrics_start;
  if (vkr_json_find_field(reader, "ascender")) {
    vkr_json_parse_float(reader, &metadata->ascender);
  }

  reader->pos = metrics_start;
  if (vkr_json_find_field(reader, "descender")) {
    vkr_json_parse_float(reader, &metadata->descender);
  }

  reader->pos = metrics_start;
  if (vkr_json_find_field(reader, "underlineY")) {
    vkr_json_parse_float(reader, &metadata->underline_y);
  }

  reader->pos = metrics_start;
  if (vkr_json_find_field(reader, "underlineThickness")) {
    vkr_json_parse_float(reader, &metadata->underline_thickness);
  }

  return true_v;
}
```

### Step 5: Parse Glyphs Array

```c
vkr_internal bool8_t vkr_mtsdf_parse_glyph_bounds(VkrJsonReader *reader,
                                                   const char *bounds_name,
                                                   float32_t *left,
                                                   float32_t *bottom,
                                                   float32_t *right,
                                                   float32_t *top) {
  uint64_t saved_pos = reader->pos;

  if (!vkr_json_find_field(reader, bounds_name)) {
    reader->pos = saved_pos;
    return false_v;
  }

  vkr_json_skip_to(reader, '{');
  uint64_t bounds_start = reader->pos;

  reader->pos = bounds_start;
  if (vkr_json_find_field(reader, "left")) {
    vkr_json_parse_float(reader, left);
  }

  reader->pos = bounds_start;
  if (vkr_json_find_field(reader, "bottom")) {
    vkr_json_parse_float(reader, bottom);
  }

  reader->pos = bounds_start;
  if (vkr_json_find_field(reader, "right")) {
    vkr_json_parse_float(reader, right);
  }

  reader->pos = bounds_start;
  if (vkr_json_find_field(reader, "top")) {
    vkr_json_parse_float(reader, top);
  }

  return true_v;
}

vkr_internal bool8_t vkr_mtsdf_parse_glyphs(VkrJsonReader *reader,
                                             VkrAllocator *allocator,
                                             Vector_VkrMtsdfGlyph *out_glyphs) {
  reader->pos = 0;
  if (!vkr_json_find_field(reader, "glyphs")) {
    log_error("MtsdfFontLoader: missing 'glyphs' field");
    return false_v;
  }

  vkr_json_skip_to(reader, '[');
  reader->pos++; // skip '['

  // Parse each glyph object
  while (reader->pos < reader->length) {
    vkr_json_skip_whitespace(reader);

    if (reader->data[reader->pos] == ']') {
      break; // end of array
    }

    if (reader->data[reader->pos] == ',') {
      reader->pos++;
      continue;
    }

    if (reader->data[reader->pos] != '{') {
      reader->pos++;
      continue;
    }

    // Find end of this glyph object
    uint64_t glyph_start = reader->pos;
    int brace_depth = 1;
    reader->pos++;
    while (reader->pos < reader->length && brace_depth > 0) {
      if (reader->data[reader->pos] == '{') brace_depth++;
      else if (reader->data[reader->pos] == '}') brace_depth--;
      reader->pos++;
    }
    uint64_t glyph_end = reader->pos;

    // Parse glyph fields
    VkrMtsdfGlyph glyph = {0};

    // Create sub-reader for this glyph
    VkrJsonReader glyph_reader = {
      .data = reader->data + glyph_start,
      .length = glyph_end - glyph_start,
      .pos = 0
    };

    // unicode (required)
    if (vkr_json_find_field(&glyph_reader, "unicode")) {
      int32_t unicode = 0;
      vkr_json_parse_int(&glyph_reader, &unicode);
      glyph.unicode = (uint32_t)unicode;
    } else {
      continue; // skip invalid glyph
    }

    // advance (required)
    glyph_reader.pos = 0;
    if (vkr_json_find_field(&glyph_reader, "advance")) {
      vkr_json_parse_float(&glyph_reader, &glyph.advance);
    }

    // planeBounds (optional)
    glyph_reader.pos = 0;
    if (vkr_mtsdf_parse_glyph_bounds(&glyph_reader, "planeBounds",
                                      &glyph.plane_left, &glyph.plane_bottom,
                                      &glyph.plane_right, &glyph.plane_top)) {
      glyph.has_geometry = true_v;
    }

    // atlasBounds (optional, but needed if has_geometry)
    glyph_reader.pos = 0;
    vkr_mtsdf_parse_glyph_bounds(&glyph_reader, "atlasBounds",
                                  &glyph.atlas_left, &glyph.atlas_bottom,
                                  &glyph.atlas_right, &glyph.atlas_top);

    vector_push_VkrMtsdfGlyph(out_glyphs, glyph);
  }

  return out_glyphs->length > 0;
}
```

### Step 6: Convert to VkrFont

```c
vkr_internal bool8_t vkr_mtsdf_build_font(
    VkrMtsdfFontMetadata *metadata,
    VkrAllocator *allocator,
    VkrTextureHandle atlas,
    float32_t target_size,
    VkrFont *out_font) {

  MemZero(out_font, sizeof(*out_font));

  out_font->type = VKR_FONT_TYPE_MTSDF;
  out_font->size = (uint32_t)target_size;
  out_font->atlas = atlas;
  out_font->page_count = 1;

  // Scale from normalized to pixels
  float32_t scale = target_size;

  out_font->line_height = (int32_t)(metadata->line_height * scale);
  out_font->ascent = (int32_t)(metadata->ascender * scale);
  out_font->descent = (int32_t)(-metadata->descender * scale);
  out_font->baseline = out_font->ascent;
  out_font->atlas_size_x = (int32_t)metadata->atlas_width;
  out_font->atlas_size_y = (int32_t)metadata->atlas_height;

  // Convert MTSDF glyphs to VkrFontGlyph
  out_font->glyphs = array_create_VkrFontGlyph(allocator, metadata->glyphs.length);
  if (!out_font->glyphs.data) {
    return false_v;
  }

  for (uint64_t i = 0; i < metadata->glyphs.length; i++) {
    VkrMtsdfGlyph *src = &metadata->glyphs.data[i];
    VkrFontGlyph *dst = &out_font->glyphs.data[i];

    dst->codepoint = src->unicode;
    dst->x_advance = (int16_t)(src->advance * scale);
    dst->page_id = 0;

    if (src->has_geometry) {
      // Convert atlas bounds to pixel coordinates
      dst->x = (uint16_t)src->atlas_left;
      dst->y = (uint16_t)src->atlas_bottom;
      dst->width = (uint16_t)(src->atlas_right - src->atlas_left);
      dst->height = (uint16_t)(src->atlas_top - src->atlas_bottom);

      // Plane bounds define offsets (scaled)
      dst->x_offset = (int16_t)(src->plane_left * scale);
      dst->y_offset = (int16_t)(src->plane_bottom * scale);
    } else {
      dst->x = dst->y = dst->width = dst->height = 0;
      dst->x_offset = dst->y_offset = 0;
    }
  }

  // Build glyph index hash table
  uint64_t table_capacity = metadata->glyphs.length * 2;
  if (table_capacity < VKR_HASH_TABLE_INITIAL_CAPACITY) {
    table_capacity = VKR_HASH_TABLE_INITIAL_CAPACITY;
  }
  out_font->glyph_indices = vkr_hash_table_create_uint32_t(allocator, table_capacity);

  for (uint64_t i = 0; i < out_font->glyphs.length; i++) {
    String8 key = string8_create_formatted(allocator, "%u",
                                            out_font->glyphs.data[i].codepoint);
    vkr_hash_table_insert_uint32_t(&out_font->glyph_indices,
                                    string8_cstr(&key), (uint32_t)i);
  }

  // Copy kernings if any
  if (metadata->kernings.length > 0) {
    out_font->kernings = array_create_VkrFontKerning(allocator,
                                                      metadata->kernings.length);
    if (out_font->kernings.data) {
      MemCopy(out_font->kernings.data, metadata->kernings.data,
              metadata->kernings.length * sizeof(VkrFontKerning));
    }
  }

  // Tab advance
  for (uint64_t i = 0; i < out_font->glyphs.length; i++) {
    if (out_font->glyphs.data[i].codepoint == 32) {
      out_font->tab_x_advance = (float32_t)out_font->glyphs.data[i].x_advance * 4.0f;
      break;
    }
  }

  // Atlas pages
  out_font->atlas_pages = array_create_VkrTextureHandle(allocator, 1);
  if (out_font->atlas_pages.data) {
    out_font->atlas_pages.data[0] = atlas;
  }

  // Store MTSDF-specific data in VkrFont (if extended)
  out_font->sdf_distance_range = metadata->distance_range;
  out_font->em_size = metadata->em_size;

  return true_v;
}
```

### Step 7: Loader Interface

```c
vkr_internal bool8_t vkr_mtsdf_font_loader_can_load(VkrResourceLoader *self,
                                                     String8 name) {
  (void)self;
  for (uint64_t i = name.length; i > 0; --i) {
    if (name.str[i - 1] == '.') {
      String8 ext = string8_substring(&name, i, name.length);
      String8 json = string8_lit("json");
      return string8_equalsi(&ext, &json);
    }
  }
  return false_v;
}

vkr_internal bool8_t vkr_mtsdf_font_loader_load(
    VkrResourceLoader *self,
    String8 name,
    VkrAllocator *temp_alloc,
    VkrResourceHandleInfo *out_handle,
    VkrRendererError *out_error) {

  VkrMtsdfFontLoaderContext *context =
      (VkrMtsdfFontLoaderContext *)self->resource_system;

  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(temp_alloc);

  // Read JSON file
  uint8_t *json_data = NULL;
  uint64_t json_size = 0;
  // ... file reading code ...

  VkrJsonReader reader = {
    .data = json_data,
    .length = json_size,
    .pos = 0
  };

  // Allocate result
  // ... arena/pool acquisition code (same as system font loader) ...

  VkrMtsdfFontMetadata metadata = {0};

  // Parse sections
  if (!vkr_mtsdf_parse_atlas(&reader, &metadata)) {
    goto fail;
  }

  if (!vkr_mtsdf_parse_metrics(&reader, &metadata)) {
    goto fail;
  }

  Vector_VkrMtsdfGlyph glyphs = vector_create_VkrMtsdfGlyph(temp_alloc);
  if (!vkr_mtsdf_parse_glyphs(&reader, temp_alloc, &glyphs)) {
    goto fail;
  }

  // Convert to array
  metadata.glyphs = array_create_VkrMtsdfGlyph(&result->allocator, glyphs.length);
  MemCopy(metadata.glyphs.data, glyphs.data, glyphs.length * sizeof(VkrMtsdfGlyph));

  // Load atlas texture (path from fontcfg, passed separately)
  // NOTE: Atlas path should be passed via extended load interface or stored in context
  VkrTextureHandle atlas = VKR_TEXTURE_HANDLE_INVALID;
  // ... load atlas texture via resource system ...

  // Build VkrFont
  float32_t target_size = 32.0f; // Default, could be from fontcfg
  if (!vkr_mtsdf_build_font(&metadata, &result->allocator, atlas,
                             target_size, &result->font)) {
    goto fail;
  }

  result->metadata = metadata;
  result->success = true_v;

  out_handle->type = VKR_RESOURCE_TYPE_MTSDF_FONT;
  out_handle->loader_id = self->id;
  out_handle->as.custom = result;

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;

fail:
  // ... cleanup ...
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return false_v;
}

VkrResourceLoader vkr_mtsdf_font_loader_create(
    VkrMtsdfFontLoaderContext *context) {
  return (VkrResourceLoader){
    .type = VKR_RESOURCE_TYPE_MTSDF_FONT,
    .resource_system = context,
    .can_load = vkr_mtsdf_font_loader_can_load,
    .load = vkr_mtsdf_font_loader_load,
    .unload = vkr_mtsdf_font_loader_unload,
    .batch_load = vkr_mtsdf_font_loader_batch_load,
  };
}
```

---

## Unified Text Shader (Bitmap + MTSDF)

Instead of creating a separate MTSDF shader, we extend the existing `assets/shaders/default.text.slang` to support both bitmap and MTSDF fonts via a mode toggle. This keeps a single unified shader for all font types.

### Modified Shader

```slang
// assets/shaders/default.text.slang

// Uniform buffer for MVP matrices
struct UniformBufferObject
{
    column_major float4x4 view;
    column_major float4x4 projection;
};

struct LocalUniformObject
{
    float4 diffuse_color;
    float  screen_px_range;  // MTSDF: distanceRange * (fontSize / emSize)
    float  font_mode;        // 0 = bitmap (alpha-only), 1 = mtsdf
    float2 _padding;
};

struct PushConstantsObject
{
    column_major float4x4 model;
};

[[vk::binding(1, 1)]]
Texture2D<float4> texture;

[[vk::binding(0, 0)]]
ConstantBuffer<UniformBufferObject> ubo;

[[vk::binding(0, 1)]]
ConstantBuffer<LocalUniformObject> local_ubo;

[[vk::binding(2, 1)]]
SamplerState sampler;

[[vk::push_constant]]
ConstantBuffer<PushConstantsObject> push_constants;

struct VertexInput
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 texcoord : TEXCOORD;
    [[vk::location(2)]] float4 color    : COLOR;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
    float4 color    : COLOR;
};

// MTSDF median function - extracts SDF value from multi-channel texture
float median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;

    float4 worldPos = mul(push_constants.model, float4(input.position, 0.0, 1.0));
    float4 viewPos = mul(ubo.view, worldPos);
    float4 clipPos = mul(ubo.projection, viewPos);

    output.position = clipPos;
    output.texcoord = input.texcoord;
    output.color = input.color;

    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float4 tint = input.color * local_ubo.diffuse_color;
    float alpha;

    if (local_ubo.font_mode > 0.5)
    {
        // MTSDF mode - sample RGB and compute median distance
        float3 msd = texture.Sample(sampler, input.texcoord).rgb;
        float sd = median(msd.r, msd.g, msd.b);
        float screen_px_distance = local_ubo.screen_px_range * (sd - 0.5);
        alpha = clamp(screen_px_distance + 0.5, 0.0, 1.0);
    }
    else
    {
        // Bitmap mode - use alpha channel only
        alpha = texture.Sample(sampler, input.texcoord).a;
    }

    return float4(tint.rgb, tint.a * alpha);
}
```

### LocalUniformObject Changes

| Field | Type | Description |
|-------|------|-------------|
| `diffuse_color` | float4 | Existing - base color tint |
| `screen_px_range` | float | **NEW** - MTSDF distance range in screen pixels |
| `font_mode` | float | **NEW** - 0.0 = bitmap, 1.0 = MTSDF |
| `_padding` | float2 | Alignment padding |

### Shader Config Update

Update `assets/shaders/default.text.shadercfg`:

```ini
# Add to local uniforms section
local=screen_px_range,float
local=font_mode,float
```

### Screen Pixel Range Calculation

```c
// When rendering MTSDF text:
float screen_px_range = 0.0f;
float font_mode = 0.0f;  // bitmap mode

if (font->type == VKR_FONT_TYPE_MTSDF) {
    font_mode = 1.0f;
    // screen_px_range = distanceRange * (renderSize / emSize)
    screen_px_range = font->sdf_distance_range *
                      (render_font_size / font->em_size);
}

// Set in LocalUniformObject before drawing
local_ubo.screen_px_range = screen_px_range;
local_ubo.font_mode = font_mode;
```

### Benefits of Unified Shader

1. **Single shader** for all font types (bitmap, system, MTSDF)
2. **No shader switching** during text rendering
3. **Easy fallback** - just set `font_mode = 0` for non-MTSDF fonts
4. **Consistent vertex format** - `VkrTextVertex` works for all modes

---

# Part 2: 3D Text Rendering

## Architecture Overview

3D text rendering displays text in world space, attached to 3D objects or positioned in the scene. Two approaches:

### Approach A: Geometry-Based (Per-Character Quads)

- Generate 3D mesh with a quad per character
- Use font atlas as texture
- Pros: Resolution-independent, efficient for static text
- Cons: Complex for dynamic text, many draw calls

### Approach B: Writable Texture (Render-to-Texture)

- Render text to a texture using CPU or existing 2D text system
- Apply texture to a 3D quad/billboard
- Pros: Simple, works with existing text rendering
- Cons: Fixed resolution, needs texture updates

**Recommended: Approach B** for simplicity and integration with existing systems.

---

## Writable Texture Approach

### Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                     3D Text Component                            │
│  1. Create writable texture (vkr_texture_system_create_writable) │
│  2. Render text to CPU buffer using existing text system         │
│  3. Upload buffer to writable texture                            │
│  4. Render textured quad in world space                          │
└─────────────────────────────────────────────────────────────────┘
```

### Data Structures

```c
// lib/src/renderer/resources/vkr_text_3d.h

#pragma once

#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_font_system.h"

#define VKR_TEXT_3D_MAX_LENGTH 1024
#define VKR_TEXT_3D_DEFAULT_TEXTURE_SIZE 512

typedef struct VkrText3DConfig {
  String8 text;
  VkrFontHandle font;
  float32_t font_size;
  Vec4 color;
  uint32_t texture_width;   // 0 = auto-size
  uint32_t texture_height;  // 0 = auto-size
} VkrText3DConfig;

typedef struct VkrText3D {
  VkrAllocator *allocator;
  VkrRendererFrontendHandle renderer;
  VkrFontSystem *font_system;

  // Text content
  String8 text;
  VkrFontHandle font;
  float32_t font_size;
  Vec4 color;

  // Texture
  VkrTextureHandle texture;
  uint32_t texture_width;
  uint32_t texture_height;
  uint8_t *cpu_buffer;      // RGBA buffer

  // 3D transform
  Mat4 transform;
  float32_t world_width;    // Width in world units
  float32_t world_height;   // Height in world units

  // State
  bool8_t dirty;
  bool8_t initialized;
} VkrText3D;

// =============================================================================
// API
// =============================================================================

bool8_t vkr_text_3d_create(VkrText3D *text_3d,
                            VkrRendererFrontendHandle renderer,
                            VkrFontSystem *font_system,
                            VkrAllocator *allocator,
                            const VkrText3DConfig *config,
                            VkrRendererError *out_error);

void vkr_text_3d_destroy(VkrText3D *text_3d);

void vkr_text_3d_set_text(VkrText3D *text_3d, String8 text);
void vkr_text_3d_set_color(VkrText3D *text_3d, Vec4 color);
void vkr_text_3d_set_transform(VkrText3D *text_3d, Mat4 transform);

// Call before rendering if dirty
void vkr_text_3d_update(VkrText3D *text_3d);

// Render the text quad
void vkr_text_3d_draw(VkrText3D *text_3d, VkrRendererFrontendHandle renderer);
```

### Implementation

```c
// lib/src/renderer/resources/vkr_text_3d.c

#include "renderer/resources/vkr_text_3d.h"
#include "renderer/systems/vkr_texture_system.h"

bool8_t vkr_text_3d_create(VkrText3D *text_3d,
                            VkrRendererFrontendHandle renderer,
                            VkrFontSystem *font_system,
                            VkrAllocator *allocator,
                            const VkrText3DConfig *config,
                            VkrRendererError *out_error) {

  MemZero(text_3d, sizeof(*text_3d));

  text_3d->allocator = allocator;
  text_3d->renderer = renderer;
  text_3d->font_system = font_system;
  text_3d->font = config->font;
  text_3d->font_size = config->font_size;
  text_3d->color = config->color;

  // Copy text
  text_3d->text = string8_duplicate(allocator, &config->text);

  // Determine texture size
  text_3d->texture_width = config->texture_width;
  text_3d->texture_height = config->texture_height;

  if (text_3d->texture_width == 0) {
    text_3d->texture_width = VKR_TEXT_3D_DEFAULT_TEXTURE_SIZE;
  }
  if (text_3d->texture_height == 0) {
    text_3d->texture_height = VKR_TEXT_3D_DEFAULT_TEXTURE_SIZE;
  }

  // Allocate CPU buffer
  uint64_t buffer_size = (uint64_t)text_3d->texture_width *
                         text_3d->texture_height * 4;
  text_3d->cpu_buffer = vkr_allocator_alloc(allocator, buffer_size,
                                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!text_3d->cpu_buffer) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(text_3d->cpu_buffer, buffer_size);

  // Create writable texture
  VkrTextureDescription desc = {
    .width = text_3d->texture_width,
    .height = text_3d->texture_height,
    .channels = 4,
    .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
    .type = VKR_TEXTURE_TYPE_2D,
    .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
    .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
    .min_filter = VKR_FILTER_LINEAR,
    .mag_filter = VKR_FILTER_LINEAR,
  };

  String8 tex_name = string8_create_formatted(allocator, "text_3d_%p",
                                               (void*)text_3d);

  RendererFrontend *rf = (RendererFrontend *)renderer;
  if (!vkr_texture_system_create_writable(&rf->texture_system, tex_name,
                                           &desc, &text_3d->texture, out_error)) {
    return false_v;
  }

  text_3d->transform = mat4_identity();
  text_3d->world_width = 1.0f;
  text_3d->world_height = (float32_t)text_3d->texture_height /
                          (float32_t)text_3d->texture_width;
  text_3d->dirty = true_v;
  text_3d->initialized = true_v;

  return true_v;
}

void vkr_text_3d_update(VkrText3D *text_3d) {
  if (!text_3d->dirty || !text_3d->initialized) {
    return;
  }

  // Clear buffer
  uint64_t buffer_size = (uint64_t)text_3d->texture_width *
                         text_3d->texture_height * 4;
  MemZero(text_3d->cpu_buffer, buffer_size);

  // Get font
  VkrFont *font = vkr_font_system_get_by_handle(text_3d->font_system,
                                                 text_3d->font);
  if (!font) {
    return;
  }

  // Render text to CPU buffer
  // This uses software rasterization of the font glyphs
  vkr_text_3d_rasterize_text(text_3d, font);

  // Upload to GPU
  RendererFrontend *rf = (RendererFrontend *)text_3d->renderer;
  VkrTexture *texture = vkr_texture_system_get_by_handle(&rf->texture_system,
                                                          text_3d->texture);
  if (texture) {
    vkr_texture_system_write(&rf->texture_system, text_3d->texture,
                              text_3d->cpu_buffer, buffer_size);
  }

  text_3d->dirty = false_v;
}

vkr_internal void vkr_text_3d_rasterize_text(VkrText3D *text_3d,
                                              VkrFont *font) {
  // Simple software text rasterization
  // For each character:
  //   1. Look up glyph in font
  //   2. Sample from font atlas texture (CPU-side)
  //   3. Blit to cpu_buffer at appropriate position

  // This requires either:
  // A) Access to the atlas image data (keep CPU copy)
  // B) Pre-rendered glyph bitmaps
  // C) Use existing VkrTextLayout and blit from atlas

  float32_t cursor_x = 0;
  float32_t cursor_y = text_3d->font_size; // Start at top

  for (uint64_t i = 0; i < text_3d->text.length; i++) {
    uint32_t codepoint = text_3d->text.str[i];

    // Find glyph
    VkrFontGlyph *glyph = vkr_font_find_glyph(font, codepoint);
    if (!glyph) continue;

    // Scale factor
    float32_t scale = text_3d->font_size / (float32_t)font->size;

    // Glyph position
    float32_t glyph_x = cursor_x + glyph->x_offset * scale;
    float32_t glyph_y = cursor_y + glyph->y_offset * scale;
    float32_t glyph_w = glyph->width * scale;
    float32_t glyph_h = glyph->height * scale;

    // Blit glyph (requires atlas data access)
    // vkr_text_3d_blit_glyph(text_3d, font, glyph,
    //                        glyph_x, glyph_y, glyph_w, glyph_h);

    cursor_x += glyph->x_advance * scale;
  }
}

void vkr_text_3d_draw(VkrText3D *text_3d, VkrRendererFrontendHandle renderer) {
  if (!text_3d->initialized) return;

  // Ensure texture is up to date
  vkr_text_3d_update(text_3d);

  // Draw a textured quad at text_3d->transform
  // Use world_width and world_height for quad size
  // Bind text_3d->texture

  // This integrates with the existing geometry/material system:
  // 1. Create or reuse a quad geometry
  // 2. Create or reuse a material with the text texture
  // 3. Submit draw command with transform
}
```

### Alternative: Geometry-Based Rendering

For resolution-independent 3D text, generate a mesh:

```c
typedef struct VkrText3DMesh {
  VkrGeometryHandle geometry;
  VkrMaterialHandle material;
  Mat4 transform;
} VkrText3DMesh;

void vkr_text_3d_mesh_generate(VkrText3DMesh *mesh,
                                VkrFontHandle font,
                                String8 text,
                                float32_t scale) {
  // For each character:
  //   Generate 4 vertices (quad corners)
  //   Apply glyph offsets and advances
  //   Set UV coordinates from font atlas

  // Create geometry from vertex/index arrays
  // Create material with font atlas texture
}
```

---

## Integration with Existing Systems

### Font Atlas CPU Access

For software rasterization, the loader must keep a CPU copy of the atlas:

```c
// In font loader result:
typedef struct VkrFontLoaderResultExtended {
  // ... existing fields ...
  uint8_t *atlas_cpu_data;  // CPU copy of atlas for software rendering
  uint64_t atlas_cpu_size;
} VkrFontLoaderResultExtended;
```

### 3D Text in World Pass

```c
// In vkr_world_resources.c (invoked by pass.world):
// vkr_world_resources_render_text(rf, &rf->world_resources, renderpass, target);
```

---

# Implementation Checklist

## JSON Module (lib/src/core/)

- [ ] Create `vkr_json.h` with `VkrJsonReader` struct and public API
- [ ] Implement `vkr_json_reader_create()` / `vkr_json_reader_from_string()`
- [ ] Implement `vkr_json_skip_whitespace()` / `vkr_json_skip_to()`
- [ ] Implement `vkr_json_find_field()`
- [ ] Implement `vkr_json_find_array()` / `vkr_json_next_array_element()`
- [ ] Implement `vkr_json_enter_object()`
- [ ] Implement `vkr_json_parse_float()` / `vkr_json_parse_double()`
- [ ] Implement `vkr_json_parse_int()`
- [ ] Implement `vkr_json_parse_string()`
- [ ] Implement `vkr_json_parse_bool()`
- [ ] Implement convenience functions: `vkr_json_get_float()`, `vkr_json_get_int()`, `vkr_json_get_string()`
- [ ] Add `vkr_json.c` to build system

## MTSDF Font Loader

- [ ] Create `mtsdf_font_loader.h` with structures
- [ ] Implement `vkr_mtsdf_parse_atlas()`
- [ ] Implement `vkr_mtsdf_parse_metrics()`
- [ ] Implement `vkr_mtsdf_parse_glyphs()`
- [ ] Implement `vkr_mtsdf_parse_kerning()` (if needed)
- [ ] Implement `vkr_mtsdf_build_font()`
- [ ] Implement loader interface functions
- [ ] Add `VKR_RESOURCE_TYPE_MTSDF_FONT` to resource system
- [ ] Modify `default.text.slang` to add MTSDF mode toggle
- [ ] Add `screen_px_range` and `font_mode` to LocalUniformObject
- [ ] Update `default.text.shadercfg` with new uniforms
- [ ] Update font system dispatch for `VKR_FONT_TYPE_MTSDF`
- [ ] Extend `VkrFont` with `sdf_distance_range` and `em_size`
- [ ] Update UI text rendering to set `font_mode` uniform based on font type
- [ ] Register loader in renderer frontend
- [ ] Test with UbuntuMono-mtsdf.fontcfg

## 3D Text Rendering (Separate Effort)

- [ ] Create `vkr_text_3d.h` with structures
- [ ] Implement `vkr_text_3d_create()`
- [ ] Implement `vkr_text_3d_destroy()`
- [ ] Implement `vkr_text_3d_set_text()` / `set_color()` / `set_transform()`
- [ ] Implement `vkr_text_3d_update()` with texture upload
- [ ] Implement software text rasterization (or geometry approach)
- [ ] Implement `vkr_text_3d_draw()`
- [ ] Add CPU atlas data storage to font loaders
- [ ] Integrate with world pass render helpers (render graph pass executors)
  for world-space rendering
- [ ] Test with 3D scene

---

## Future Enhancements

1. **Full JSON parser** - Extend `vkr_json.h` with recursive parsing if needed for complex JSON
2. **Kerning support** - Parse and apply kerning from MTSDF metadata
3. **Text billboarding** - Always face camera option
4. **Text animation** - Per-character animation support
5. **Text effects** - Outline, shadow, glow via shader
6. **Multi-language** - Unicode range support beyond ASCII
7. **Dynamic font sizing** - Scale MTSDF without quality loss
8. **JSON validation** - Add optional schema validation to `vkr_json.h`
