---
status: implemented
updated: 2026-07-31
authority: design
---
# Text Rendering API Design

## Overview

This document outlines the text primitives and UTF-8 API for the VKR engine text module located in `core/text/`. All structures and enums use the `Vkr` prefix, functions use the `vkr_` prefix, and files are named with the `vkr_` prefix (e.g., `core/text/vkr_text.h`). The goal is to provide a minimal, efficient text layer that integrates with existing `String8`, allocators, and math types. Font systems and metrics live in their own module; this layer is only UTF-8 + text primitives.

## Goals & Constraints

- Build on existing `String8` UTF-8 infrastructure from `containers/str.h`
- Use `VkrAllocator` for all allocations
- Keep API surface minimal while covering common engine needs
- Support UTF-8 text with proper codepoint iteration
- Integrate with existing math types (`Vec2`, `Vec4`)
- Allow both immediate and retained text usage
- Separate text data, layout, and rendering concerns

## Design Principles

1. **Immutable Text Content** — Text buffers are immutable after creation; styling/layout is separate.
2. **Zero-Copy Views** — Prefer `String8` views over allocations when possible.
3. **Composition First** — Build richer text from simple primitives.
4. **Explicit Ownership** — Owned buffers vs. views are explicit on the struct.

---

## Core Primitives (in `core/text/vkr_text.h`)

### 1. `VkrText` — Renderable Text Primitive

```c
typedef struct VkrText {
    String8 content;        // UTF-8 text content (view or owned)
    VkrTextStyle style;     // Applied style
    bool8_t owns_content;   // true if content was allocated (needs free)
} VkrText;
```

### 2. `VkrTextStyle` — Reusable Style Configuration

```c
typedef struct VkrTextStyle {
    VkrFontHandle font;     // Font resource handle (id + generation)
    float32_t font_size;    // Font size in pixels (for scaling from font's native size)
    Vec4 color;             // RGBA text color
    float32_t line_height;  // Line height multiplier (1.0 = font metrics)
    float32_t letter_spacing; // Extra spacing between glyphs (pixels)
} VkrTextStyle;
```

### 3. `VkrTextSpan` — Styled Range Within Text

```c
typedef struct VkrTextSpan {
    uint64_t start;         // Start byte offset (inclusive)
    uint64_t end;           // End byte offset (exclusive)
    VkrTextStyle style;     // Style overrides for this span
} VkrTextSpan;
```

### 4. `VkrTextLayout` — Computed Layout Information

```c
typedef struct VkrTextGlyph {
    uint32_t codepoint;     // Unicode codepoint
    Vec2 position;          // Baseline position for this glyph
    float32_t advance;      // Advance used during layout
} VkrTextGlyph;

typedef struct VkrTextLayout {
    Vec2 bounds;            // Total width/height of laid-out text
    Vec2 baseline;          // Baseline of the first line relative to origin
    uint32_t line_count;    // Number of lines after layout
    uint32_t glyph_count;   // Total number of glyphs (excludes newlines)
    VkrTextGlyph *glyphs;   // Glyph positions (owned by layout)
    uint64_t glyph_data_size; // glyph_count * sizeof(VkrTextGlyph)
    VkrAllocator *allocator;
} VkrTextLayout;
```

### 5. `VkrTextBounds` — Simple Bounding Box

```c
typedef struct VkrTextBounds {
    Vec2 size;              // Width and height of text bounds
    float32_t ascent;       // Distance from baseline to top
    float32_t descent;      // Distance from baseline to bottom
} VkrTextBounds;
```

---

## Text Alignment & Anchor

```c
typedef enum VkrTextAlign {
    VKR_TEXT_ALIGN_LEFT = 0,
    VKR_TEXT_ALIGN_CENTER,
    VKR_TEXT_ALIGN_RIGHT,
    VKR_TEXT_ALIGN_JUSTIFY,
} VkrTextAlign;

typedef enum VkrTextBaseline {
    VKR_TEXT_BASELINE_TOP = 0,
    VKR_TEXT_BASELINE_MIDDLE,
    VKR_TEXT_BASELINE_BOTTOM,
    VKR_TEXT_BASELINE_ALPHABETIC,
} VkrTextBaseline;

typedef struct VkrTextAnchor {
    VkrTextAlign horizontal;
    VkrTextBaseline vertical;
} VkrTextAnchor;
```

---

## UTF-8 Codepoint Iteration (in `vkr_text.h`)

```c
typedef struct VkrCodepoint {
    uint32_t value;         // Unicode codepoint value (U+0000 to U+10FFFF)
    uint8_t byte_length;    // Number of bytes consumed (1-4, 0 on error)
} VkrCodepoint;

typedef struct VkrCodepointIter {
    const String8 *str;     // Source string being iterated
    uint64_t byte_offset;   // Current byte position
} VkrCodepointIter;
```

---

## API Functions

### Text Creation & Destruction

```c
VkrText vkr_text_from_view(String8 content, const VkrTextStyle *style);
VkrText vkr_text_from_copy(VkrAllocator *allocator, String8 content,
                           const VkrTextStyle *style);
VkrText vkr_text_from_cstr(const char *cstr, const VkrTextStyle *style);
VkrText vkr_text_formatted(VkrAllocator *allocator, const VkrTextStyle *style,
                           const char *fmt, ...);
void vkr_text_destroy(VkrAllocator *allocator, VkrText *text);
```

### `VkrTextStyle` Helpers

```c
VkrTextStyle vkr_text_style_default(void);
VkrTextStyle vkr_text_style_new(VkrFontHandle font, float32_t font_size,
                                Vec4 color);
VkrTextStyle vkr_text_style_with_color(const VkrTextStyle *base, Vec4 color);
VkrTextStyle vkr_text_style_with_size(const VkrTextStyle *base,
                                      float32_t font_size);
```

### Text Measurement

```c
VkrTextBounds vkr_text_measure(const VkrText *text);
VkrTextBounds vkr_text_measure_wrapped(const VkrText *text, float32_t max_width);
float32_t vkr_text_glyph_width(float32_t font_size);
```

### Text Layout

```c
typedef struct VkrTextLayoutOptions {
    float32_t max_width;    // Max width before wrap (0 = no wrap)
    float32_t max_height;   // Max height before clip (0 = no limit)
    VkrTextAnchor anchor;   // Alignment/anchor mode
    bool8_t word_wrap;      // Enable word wrapping
    bool8_t clip;           // Clip text exceeding bounds
} VkrTextLayoutOptions;

VkrTextLayoutOptions vkr_text_layout_options_default(void);
VkrTextLayout vkr_text_layout_compute(VkrAllocator *allocator,
                                      const VkrText *text,
                                      const VkrTextLayoutOptions *options);
void vkr_text_layout_destroy(VkrTextLayout *layout);
```

### UTF-8 Codepoint Iteration

```c
VkrCodepointIter vkr_codepoint_iter_begin(const String8 *str);
bool8_t vkr_codepoint_iter_has_next(const VkrCodepointIter *iter);
VkrCodepoint vkr_codepoint_iter_next(VkrCodepointIter *iter);
VkrCodepoint vkr_codepoint_iter_peek(const VkrCodepointIter *iter);
VkrCodepoint vkr_utf8_decode(const uint8_t *bytes, uint64_t max_bytes);
uint8_t vkr_utf8_encode(uint32_t codepoint, uint8_t *out);
uint64_t vkr_string8_codepoint_count(const String8 *str);
bool8_t vkr_string8_is_valid_utf8(const String8 *str);
```

### Rich Text Support

```c
typedef struct VkrRichText {
    String8 content;        // Full text content
    VkrTextStyle base_style;  // Default style for unstyled regions
    VkrTextSpan *spans;     // Array of styled spans
    uint32_t span_count;    // Number of spans
    uint32_t span_capacity; // Allocated capacity
    VkrAllocator *allocator;
} VkrRichText;

VkrRichText vkr_rich_text_create(VkrAllocator *allocator, String8 content,
                                 const VkrTextStyle *base_style);
void vkr_rich_text_add_span(VkrRichText *rt, uint64_t start, uint64_t end,
                            const VkrTextStyle *style);
void vkr_rich_text_clear_spans(VkrRichText *rt);
void vkr_rich_text_destroy(VkrRichText *rt);
```

### Convenience Macros

```c
#define vkr_text_lit(str) vkr_text_from_cstr(str, NULL)
#define VKR_TEXT_COLOR_WHITE  (Vec4){1.0f, 1.0f, 1.0f, 1.0f}
#define VKR_TEXT_COLOR_BLACK  (Vec4){0.0f, 0.0f, 0.0f, 1.0f}
#define VKR_TEXT_COLOR_RED    (Vec4){1.0f, 0.0f, 0.0f, 1.0f}
#define VKR_TEXT_COLOR_GREEN  (Vec4){0.0f, 1.0f, 0.0f, 1.0f}
#define VKR_TEXT_COLOR_BLUE   (Vec4){0.0f, 0.0f, 1.0f, 1.0f}
#define VKR_TEXT_COLOR_YELLOW (Vec4){1.0f, 1.0f, 0.0f, 1.0f}
```

---

## Implementation Placement

- `core/text/vkr_text.h` & `vkr_text.c`: UTF-8 helpers, codepoint iteration, text primitives, style helpers, bounds measurement/layout (using simple defaults), and rich text utilities.
- All structs/enums use `Vkr` prefix; all functions use `vkr_` prefix; files are `vkr_*` within `core/text/`.
- Uses `String8` from `containers/str.h`, math types from `math/vec.h`, and allocators from `memory/vkr_allocator.h`.

---

## Implementation Phases

1. **UTF-8 Foundation** — decoding/encoding, iterators, validation, codepoint counts.
2. **Core Text Primitives** — `VkrText`, `VkrTextStyle`, creation/destruction helpers.
3. **Text Measurement** — bounds queries using simple default heuristics.
4. **Text Layout** — word wrapping, alignment, glyph positioning (font system stays separate).
5. **Rich Text** — span management for inline styling.
6. **Rendering Integration** — hook layouts into renderer batches (future work).

---

## Usage Examples

```c
VkrFontHandle my_font = {.id = 1, .generation = 0}; // Or obtained from font system
VkrTextStyle title_style = vkr_text_style_new(my_font, 32.0f, VKR_TEXT_COLOR_WHITE);
VkrText title = vkr_text_from_cstr("Hello, World!", &title_style);

VkrTextBounds bounds = vkr_text_measure(&title);

VkrTextLayoutOptions opts = vkr_text_layout_options_default();
opts.anchor.horizontal = VKR_TEXT_ALIGN_CENTER;
VkrTextLayout layout =
    vkr_text_layout_compute(temp_alloc, &title, &opts);

// ... render layout ...

vkr_text_layout_destroy(&layout);
vkr_text_destroy(temp_alloc, &title);
```
