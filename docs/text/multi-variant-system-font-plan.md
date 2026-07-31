---
status: partial
updated: 2026-07-31
authority: design
---
# Multi-Variant System Font Implementation Plan

> **Current boundary.** Multi-variant loading ships, but the “current state”
> excerpts and unchecked checklist below describe the pre-implementation
> baseline. Use the current loader code for behavior and signatures.

## Executive Summary

This document outlines the implementation plan for adding multi-variant font support (TTC font collections) to the system font loader. Currently, the system font loader (`lib/src/renderer/resources/loaders/system_font_loader.c`) only loads font index 0 from TTF/TTC files, ignoring additional font variants in TrueType Collections.

**Design Goal:** No new public API. The existing `vkr_font_system_load_from_file()` automatically loads all variants, and users access specific variants via `vkr_font_system_get_by_name()` using face names from the config.

## Current State Analysis

### System Font Loader (`system_font_loader.c`)

The current implementation has this limitation on line 152:

```c
int32_t font_offset = stbtt_GetFontOffsetForIndex(state->font_data, 0);
```

This hardcodes font index 0, meaning:
- Only the first font in a TTC collection is ever loaded
- Multiple `face=` entries in `.fontcfg` files are parsed but ignored
- Users cannot access other font variants (e.g., different CJK language variants)

### Configuration File Format (Already Supports Multi-Face)

The `.fontcfg` parser in `vkr_font_system.c` already supports multiple faces:

```ini
# Example: NotoSansCJK.fontcfg
file=NotoSansCJK-Regular.ttc
type=system
face=Noto Sans CJK JP     # Index 0
face=Noto Sans CJK KR     # Index 1
face=Noto Sans CJK SC     # Index 2
face=Noto Sans CJK TC     # Index 3
face=Noto Sans CJK HK     # Index 4
face=Noto Sans Mono CJK JP # Index 5
# ... etc
```

The `VkrFontConfig` structure (in `vkr_font_system.h`) stores up to 16 faces:

```c
typedef struct VkrFontConfig {
  String8 file;
  String8 atlas;
  VkrFontType type;
  String8 faces[VKR_FONT_CONFIG_MAX_FACES];  // Already exists!
  uint32_t face_count;                        // Already exists!
  uint32_t size;
  bool8_t is_valid;
} VkrFontConfig;
```

### stb_truetype TTC Support (Available)

`vendor/stb_truetype.h` provides full TTC support:

```c
// Get number of fonts in collection (returns 1 for single TTF)
int stbtt_GetNumberOfFonts(const unsigned char *data);

// Get byte offset for font at given index
int stbtt_GetFontOffsetForIndex(const unsigned char *data, int index);

// Get font name string (for validation/debugging)
const char *stbtt_GetFontNameString(const stbtt_fontinfo *font,
    int *length, int platformID, int encodingID, int languageID, int nameID);
```

## Design Approach

### Unified API - All System Fonts Treated as Multi-Variant

The key insight is that **every system font can be treated as having variants**, even if there's only one. This allows us to:

1. Keep the existing `vkr_font_system_load_from_file()` API unchanged
2. Automatically load all variants when a system font config has multiple faces
3. Register each variant under its face name for lookup via `vkr_font_system_get_by_name()`
4. Register the first variant (index 0) under the provided `name` parameter as an alias

### Behavior Summary

| Config | Behavior |
|--------|----------|
| Single `face=` or no face | Load index 0, register under `name` and face name |
| Multiple `face=` entries | Load all variants, register each under face name, first also under `name` |
| No faces but TTC file | Load all fonts in TTC, register as `name-0`, `name-1`, etc., first also under `name` |

---

## Detailed Implementation Plan

### Phase 1: Extend System Font Loader for Index Support

#### 1.1 Add Font Index to Request Parameters

**File:** `lib/src/renderer/resources/loaders/system_font_loader.h`

```c
// Add to constants section (after line 22)
#define VKR_SYSTEM_FONT_DEFAULT_INDEX 0
```

**File:** `lib/src/renderer/resources/loaders/system_font_loader.c`

Update `VkrSystemFontRequest` struct (around line 40):

```c
typedef struct VkrSystemFontRequest {
  String8 file_path;
  String8 query;
  uint32_t size;
  uint32_t font_index;  // NEW: Index into TTC collection (default 0)
} VkrSystemFontRequest;
```

Update `vkr_system_font_parse_request()` (around line 64) to parse `index=N`:

```c
vkr_internal VkrSystemFontRequest vkr_system_font_parse_request(String8 name) {
  String8 query = {0};
  String8 base_path = vkr_system_font_strip_query(name, &query);

  uint32_t size = VKR_SYSTEM_FONT_DEFAULT_SIZE;
  uint32_t font_index = VKR_SYSTEM_FONT_DEFAULT_INDEX;  // NEW

  uint64_t start = 0;
  while (start < query.length) {
    uint64_t end = start;
    while (end < query.length && query.str[end] != '&') {
      end++;
    }

    String8 param = string8_substring(&query, start, end);
    uint64_t eq_pos = UINT64_MAX;
    for (uint64_t i = 0; i < param.length; ++i) {
      if (param.str[i] == '=') {
        eq_pos = i;
        break;
      }
    }

    if (eq_pos != UINT64_MAX && eq_pos > 0 && eq_pos + 1 < param.length) {
      String8 key = string8_substring(&param, 0, eq_pos);
      String8 value = string8_substring(&param, eq_pos + 1, param.length);

      String8 key_size = string8_lit("size");
      String8 key_index = string8_lit("index");  // NEW

      if (string8_equalsi(&key, &key_size)) {
        int32_t parsed = 0;
        if (string8_to_i32(&value, &parsed) && parsed > 0) {
          size = (uint32_t)parsed;
        }
      } else if (string8_equalsi(&key, &key_index)) {  // NEW
        int32_t parsed = 0;
        if (string8_to_i32(&value, &parsed) && parsed >= 0) {
          font_index = (uint32_t)parsed;
        }
      }
    }

    start = end + 1;
  }

  return (VkrSystemFontRequest){
      .file_path = base_path,
      .query = query,
      .size = size,
      .font_index = font_index,  // NEW
  };
}
```

#### 1.2 Add Font Index to Parse State

**File:** `lib/src/renderer/resources/loaders/system_font_loader.c`

Update `VkrSystemFontParseState` struct (around line 13):

```c
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
  uint32_t font_index;  // NEW: Index into TTC collection
  uint32_t atlas_width;
  uint32_t atlas_height;

  String8 face_name;

  Vector_VkrFontGlyph glyphs;
  Vector_VkrFontKerning kernings;
  uint8_t *atlas_bitmap;

  VkrRendererError *out_error;
} VkrSystemFontParseState;
```

#### 1.3 Use Font Index in stbtt Initialization

**File:** `lib/src/renderer/resources/loaders/system_font_loader.c`

Update `vkr_system_font_init_stbtt()` (around line 148):

```c
vkr_internal bool8_t
vkr_system_font_init_stbtt(VkrSystemFontParseState *state) {
  assert_log(state != NULL, "State is NULL");

  // NEW: Use state->font_index instead of hardcoded 0
  int32_t font_offset = stbtt_GetFontOffsetForIndex(state->font_data,
                                                     (int)state->font_index);
  if (font_offset < 0) {
    log_error("SystemFontLoader: invalid font file or index %u",
              state->font_index);
    *state->out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // ... rest unchanged ...
}
```

#### 1.4 Wire Up Font Index in Load Function

**File:** `lib/src/renderer/resources/loaders/system_font_loader.c`

Update `vkr_system_font_loader_load()` (around line 627):

```c
  VkrSystemFontParseState state = {
      .load_allocator = &result->allocator,
      .temp_allocator = temp_alloc,
      .font_size = Clamp(request.size, VKR_SYSTEM_FONT_MIN_SIZE,
                         VKR_SYSTEM_FONT_MAX_SIZE),
      .font_index = request.font_index,  // NEW: Pass through font index
      .atlas_width = VKR_SYSTEM_FONT_DEFAULT_ATLAS_SIZE,
      .atlas_height = VKR_SYSTEM_FONT_DEFAULT_ATLAS_SIZE,
      .out_error = out_error,
  };
```

### Phase 2: Update Font System to Load All Variants

#### 2.1 Internal Helper: Load Single Variant

**File:** `lib/src/renderer/systems/vkr_font_system.c`

Add a new internal helper function (before `vkr_font_system_load_from_file`):

```c
/**
 * @brief Loads a single font variant and registers it in the font system.
 * @param system The font system
 * @param register_name The name to register this variant under
 * @param config The parsed font config
 * @param font_index The TTC font index to load
 * @param out_error Error output
 * @return true on success
 */
vkr_internal bool8_t vkr_font_system_load_single_variant(
    VkrFontSystem *system,
    String8 register_name,
    const VkrFontConfig *config,
    uint32_t font_index,
    VkrRendererError *out_error) {

  // Check if already loaded
  const char *font_key = (const char *)register_name.str;
  VkrFontSystemEntry *existing =
      vkr_hash_table_get_VkrFontSystemEntry(&system->font_map, font_key);
  if (existing) {
    // Already loaded, skip
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  // Find free slot
  uint32_t free_slot = vkr_font_system_find_free_slot(system);
  if (free_slot == VKR_INVALID_ID) {
    log_error("Font system is full");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  // Build load path with size and index
  VkrAllocatorScope load_scope =
      vkr_allocator_begin_scope(&system->temp_allocator);

  String8 load_name = string8_create_formatted(
      &system->temp_allocator, "%.*s?size=%u&index=%u",
      (int32_t)config->file.length, config->file.str,
      config->size > 0 ? config->size : VKR_SYSTEM_FONT_DEFAULT_SIZE,
      font_index);

  // Load via resource system
  VkrResourceHandleInfo handle_info = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_SYSTEM_FONT, load_name,
                                 &system->allocator, &handle_info,
                                 &load_error)) {
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = load_error;
    return false_v;
  }

  // Extract result
  VkrSystemFontLoaderResult *result =
      (VkrSystemFontLoaderResult *)handle_info.as.custom;
  if (!result || !result->success) {
    vkr_resource_system_unload(&handle_info, load_name);
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = result ? result->error : VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  // Copy font to system slot
  VkrFont *font = &system->fonts.data[free_slot];
  *font = result->font;
  font->id = free_slot + 1;
  font->generation = system->generation_counter++;

  // Allocate stable key
  char *stable_key = (char *)vkr_allocator_alloc(
      &system->allocator, register_name.length + 1,
      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
    vkr_resource_system_unload(&handle_info, load_name);
    MemZero(font, sizeof(*font));
    font->id = VKR_INVALID_ID;
    font->generation = VKR_INVALID_ID;
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemCopy(stable_key, register_name.str, register_name.length);
  stable_key[register_name.length] = '\0';

  // Register in font map
  VkrFontSystemEntry entry = {
      .index = free_slot,
      .ref_count = 0,
      .auto_release = true_v,
      .loader_id = handle_info.loader_id,
      .resource = handle_info.as.custom,
  };

  if (!vkr_hash_table_insert_VkrFontSystemEntry(&system->font_map,
                                                 stable_key, entry)) {
    vkr_resource_system_unload(&handle_info, load_name);
    MemZero(font, sizeof(*font));
    font->id = VKR_INVALID_ID;
    font->generation = VKR_INVALID_ID;
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}
```

#### 2.2 Internal Helper: Get Font Count from File

**File:** `lib/src/renderer/systems/vkr_font_system.c`

Add helper to query TTC font count:

```c
/**
 * @brief Gets the number of fonts in a TTF/TTC file.
 */
vkr_internal uint32_t vkr_font_system_get_font_count_from_file(
    String8 file_path,
    VkrAllocator *temp_alloc) {

  FilePath fp = file_path_create((const char *)file_path.str, temp_alloc,
                                  FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  if (file_open(&fp, mode, &fh) != FILE_ERROR_NONE) {
    return 0;
  }

  uint8_t *font_data = NULL;
  uint64_t font_data_size = 0;
  FileError ferr = file_read_all(&fh, temp_alloc, &font_data, &font_data_size);
  file_close(&fh);

  if (ferr != FILE_ERROR_NONE || !font_data) {
    return 0;
  }

  int num_fonts = stbtt_GetNumberOfFonts(font_data);
  return (num_fonts > 0) ? (uint32_t)num_fonts : 1;
}
```

#### 2.3 Update `vkr_font_system_load_from_file()` to Load All Variants

**File:** `lib/src/renderer/systems/vkr_font_system.c`

Replace the system font loading section in `vkr_font_system_load_from_file()`:

```c
bool8_t vkr_font_system_load_from_file(VkrFontSystem *system, String8 name,
                                       String8 fontcfg_path,
                                       VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(fontcfg_path.str != NULL, "Config path is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (name.length == 0 || fontcfg_path.length == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // Parse the .fontcfg file
  VkrAllocatorScope parse_scope =
      vkr_allocator_begin_scope(&system->temp_allocator);
  VkrFontConfig config = vkr_font_config_parse(fontcfg_path, &system->allocator,
                                               &system->temp_allocator);
  vkr_allocator_end_scope(&parse_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);

  if (!config.is_valid) {
    log_error("Failed to parse font config '%.*s'",
              (int32_t)fontcfg_path.length, fontcfg_path.str);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  // Handle based on font type
  if (config.type == VKR_FONT_TYPE_SYSTEM) {
    // === SYSTEM FONT: Load all variants ===

    VkrAllocatorScope load_scope =
        vkr_allocator_begin_scope(&system->temp_allocator);

    // Get number of fonts in file
    uint32_t font_count = vkr_font_system_get_font_count_from_file(
        config.file, &system->temp_allocator);

    if (font_count == 0) {
      log_error("Failed to read font file '%.*s'",
                (int32_t)config.file.length, config.file.str);
      vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
      return false_v;
    }

    // Determine how many variants to load
    uint32_t variants_to_load = font_count;
    if (config.face_count > 0) {
      // Use face count from config (may be less than fonts in file)
      variants_to_load = (config.face_count < font_count)
                         ? config.face_count : font_count;
    }

    // Load each variant
    uint32_t loaded = 0;
    bool8_t first_registered_under_name = false_v;

    for (uint32_t i = 0; i < variants_to_load; i++) {
      // Determine registration name for this variant
      String8 variant_name;
      if (i < config.face_count && config.faces[i].str &&
          config.faces[i].length > 0) {
        // Use face name from config
        variant_name = config.faces[i];
      } else {
        // Generate name: "basename-N"
        variant_name = string8_create_formatted(&system->temp_allocator,
            "%.*s-%u", (int32_t)name.length, name.str, i);
      }

      VkrRendererError variant_error = VKR_RENDERER_ERROR_NONE;
      if (vkr_font_system_load_single_variant(system, variant_name, &config,
                                               i, &variant_error)) {
        loaded++;

        // Also register first variant under the provided name (alias)
        if (!first_registered_under_name) {
          // Check if name != variant_name to avoid duplicate registration
          if (!string8_equals(&name, &variant_name)) {
            // Create alias entry pointing to same font
            const char *variant_key = (const char *)variant_name.str;
            VkrFontSystemEntry *entry = vkr_hash_table_get_VkrFontSystemEntry(
                &system->font_map, variant_key);
            if (entry) {
              char *alias_key = (char *)vkr_allocator_alloc(
                  &system->allocator, name.length + 1,
                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
              if (alias_key) {
                MemCopy(alias_key, name.str, name.length);
                alias_key[name.length] = '\0';

                // Create alias entry (same index, separate ref counting)
                VkrFontSystemEntry alias_entry = *entry;
                alias_entry.ref_count = 0;
                vkr_hash_table_insert_VkrFontSystemEntry(
                    &system->font_map, alias_key, alias_entry);
              }
            }
          }
          first_registered_under_name = true_v;
        }
      } else {
        log_warn("Failed to load font variant %u from '%.*s': error %d",
                 i, (int32_t)config.file.length, config.file.str,
                 (int)variant_error);
      }
    }

    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);

    if (loaded == 0) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }

    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;

  } else if (config.type == VKR_FONT_TYPE_BITMAP) {
    // ... existing bitmap font loading code unchanged ...
  } else if (config.type == VKR_FONT_TYPE_MTSDF) {
    // ... existing mtsdf font loading code unchanged ...
  }

  // ... rest of function unchanged ...
}
```

---

## API Usage (Unchanged!)

### Loading Fonts (Same API as Before)

```c
// Load all variants from NotoSansCJK.fontcfg
String8 name = string8_lit("NotoSansCJK");
String8 cfg = string8_lit("assets/fonts/NotoSansCJK.fontcfg");
VkrRendererError err;
vkr_font_system_load_from_file(&font_system, name, cfg, &err);
```

### Accessing Variants

```c
// Access by provided name (gets first variant / index 0)
VkrFont *default_font = vkr_font_system_get_by_name(&font_system,
    string8_lit("NotoSansCJK"));

// Access specific variant by face name (from fontcfg)
VkrFont *jp = vkr_font_system_get_by_name(&font_system,
    string8_lit("Noto Sans CJK JP"));
VkrFont *sc = vkr_font_system_get_by_name(&font_system,
    string8_lit("Noto Sans CJK SC"));
VkrFont *kr = vkr_font_system_get_by_name(&font_system,
    string8_lit("Noto Sans CJK KR"));
```

### Single-Face Config (Works Unchanged)

```ini
# UbuntuMono.fontcfg - single variant
file=UbuntuMono-R.ttf
face=Ubuntu Mono
type=system
size=18
```

```c
// Loads single variant, accessible by both names
vkr_font_system_load_from_file(&system, string8_lit("UbuntuMono"),
                                cfg_path, &err);

// Both work:
VkrFont *f1 = vkr_font_system_get_by_name(&system, string8_lit("UbuntuMono"));
VkrFont *f2 = vkr_font_system_get_by_name(&system, string8_lit("Ubuntu Mono"));
// f1 == f2 (same font)
```

---

## Data Flow Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│              vkr_font_system_load_from_file()                   │
│                name="NotoSansCJK"                               │
│                cfg="NotoSansCJK.fontcfg"                        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                   vkr_font_config_parse()                       │
│  file=NotoSansCJK-Regular.ttc                                   │
│  type=system                                                    │
│  faces=["Noto Sans CJK JP", "Noto Sans CJK KR", ...]           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│           stbtt_GetNumberOfFonts() → 10 fonts                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│              For each variant (i = 0..9):                       │
│                                                                 │
│  vkr_font_system_load_single_variant()                          │
│    → load "file.ttc?size=18&index=i"                           │
│    → register under faces[i] name                               │
│    → if i==0, also register under "NotoSansCJK"                │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Font System                              │
│                                                                 │
│  font_map["NotoSansCJK"]      → VkrFont (index 0) ─┐           │
│  font_map["Noto Sans CJK JP"] → VkrFont (index 0) ←┘ (alias)   │
│  font_map["Noto Sans CJK KR"] → VkrFont (index 1)              │
│  font_map["Noto Sans CJK SC"] → VkrFont (index 2)              │
│  font_map["Noto Sans CJK TC"] → VkrFont (index 3)              │
│  ...                                                            │
└─────────────────────────────────────────────────────────────────┘
```

---

## File Modification Summary

| File | Changes |
|------|---------|
| `lib/src/renderer/resources/loaders/system_font_loader.h` | Add `VKR_SYSTEM_FONT_DEFAULT_INDEX` constant |
| `lib/src/renderer/resources/loaders/system_font_loader.c` | Add `font_index` to `VkrSystemFontRequest` and `VkrSystemFontParseState`, parse `index=` query param, use index in `stbtt_GetFontOffsetForIndex()` |
| `lib/src/renderer/systems/vkr_font_system.c` | Add `vkr_font_system_load_single_variant()` helper, add `vkr_font_system_get_font_count_from_file()` helper, update `vkr_font_system_load_from_file()` to iterate over variants for system fonts |

**No changes to public API or header files (except the constant).**

---

## Testing Strategy

### Unit Tests

1. **Index Parameter Parsing**
   - Parse `"font.ttc?size=18&index=2"` → verify index=2, size=18
   - Parse `"font.ttf?size=24"` → verify index=0 (default), size=24
   - Parse `"font.ttc?index=5&size=12"` → verify index=5, size=12

2. **Font Count Detection**
   - Single TTF file → returns 1
   - TTC with 10 fonts → returns 10

3. **Variant Registration**
   - Load config with 3 faces → 3 fonts registered + 1 alias
   - Verify each accessible by face name
   - Verify first accessible by provided name

### Integration Tests

1. **Backward Compatibility**
   - Load existing single-face `UbuntuMono.fontcfg`
   - Verify accessible by both "UbuntuMono" and "Ubuntu Mono"

2. **Multi-Variant Loading**
   - Load `NotoSansCJK.fontcfg` with 10 faces
   - Verify all 10 accessible by face name
   - Verify "NotoSansCJK" returns first variant

3. **Memory Test**
   - Load/unload multi-variant font
   - Verify no memory leaks

---

## Error Handling

| Scenario | Behavior |
|----------|----------|
| TTC file not found | Return error, no variants loaded |
| Index exceeds font count | Skip that variant, log warning, continue |
| All variants fail to load | Return error |
| Some variants fail | Return success (partial load), log warnings |
| Face count > font count | Load only available fonts |

---

## Implementation Checklist

- [ ] Add `VKR_SYSTEM_FONT_DEFAULT_INDEX` constant to `system_font_loader.h`
- [ ] Add `font_index` field to `VkrSystemFontRequest` struct
- [ ] Update `vkr_system_font_parse_request()` to parse `index=` parameter
- [ ] Add `font_index` field to `VkrSystemFontParseState` struct
- [ ] Update `vkr_system_font_init_stbtt()` to use `state->font_index`
- [ ] Wire up `font_index` in `vkr_system_font_loader_load()`
- [ ] Implement `vkr_font_system_get_font_count_from_file()` helper
- [ ] Implement `vkr_font_system_load_single_variant()` helper
- [ ] Update `vkr_font_system_load_from_file()` for system fonts to load all variants
- [ ] Add alias registration for first variant under provided name
- [ ] Write unit tests for index parsing
- [ ] Write integration tests for multi-variant loading
- [ ] Test backward compatibility with existing fontcfg files
- [ ] Test with NotoSansCJK TTC file
