---
status: proposed
updated: 2026-07-31
authority: design
---
# UI Layout Engine Design

## Document Purpose

This document describes the layout engine for the UI system. The layout engine is responsible for computing the position and size of all UI elements based on their constraints, styles, and parent-child relationships. It uses a flexbox-inspired algorithm that is familiar to web developers while being efficient for real-time rendering.

Related documents:
- [UI System Overview](./ui-system-overview.md) - High-level architecture
- [UI Element Primitives Design](./ui-element-primitives-design.md) - Element types that use layout

## Goals

1. **Flexbox-Inspired**: Familiar CSS-like layout model
2. **Efficient**: O(n) single-pass layout when possible
3. **Incremental**: Only recompute dirty subtrees
4. **Standalone**: Layout engine has no renderer dependencies (in `/core/ui/`)

## Location

```
lib/src/core/ui/
├── vkr_ui_layout.h       # Main layout API
├── vkr_ui_layout.c       # Layout algorithm implementation
├── vkr_ui_box_model.h    # Box model (margin, padding, border)
├── vkr_ui_size.h         # Size types (px, percent, auto)
└── vkr_ui_style.h        # Style property definitions
```

## Size Types

### VkrUiSize

Sizes can be specified in multiple units:

```c
/**
 * @brief Size unit type.
 */
typedef enum VkrUiSizeUnit {
  VKR_UI_SIZE_UNIT_AUTO = 0,    /**< Size determined by content/layout */
  VKR_UI_SIZE_UNIT_PX,          /**< Absolute pixels */
  VKR_UI_SIZE_UNIT_PERCENT,     /**< Percentage of parent */
  VKR_UI_SIZE_UNIT_EM,          /**< Relative to font size */
  VKR_UI_SIZE_UNIT_VIEWPORT_W,  /**< Percentage of viewport width */
  VKR_UI_SIZE_UNIT_VIEWPORT_H,  /**< Percentage of viewport height */
} VkrUiSizeUnit;

/**
 * @brief Size value with unit.
 */
typedef struct VkrUiSize {
  float32_t value;
  VkrUiSizeUnit unit;
} VkrUiSize;

// Convenience macros
#define VKR_UI_SIZE_AUTO      ((VkrUiSize){0.0f, VKR_UI_SIZE_UNIT_AUTO})
#define VKR_UI_SIZE_PX(v)     ((VkrUiSize){(v), VKR_UI_SIZE_UNIT_PX})
#define VKR_UI_SIZE_PERCENT(v) ((VkrUiSize){(v), VKR_UI_SIZE_UNIT_PERCENT})
#define VKR_UI_SIZE_EM(v)     ((VkrUiSize){(v), VKR_UI_SIZE_UNIT_EM})
#define VKR_UI_SIZE_VW(v)     ((VkrUiSize){(v), VKR_UI_SIZE_UNIT_VIEWPORT_W})
#define VKR_UI_SIZE_VH(v)     ((VkrUiSize){(v), VKR_UI_SIZE_UNIT_VIEWPORT_H})
```

### Size Resolution

```c
/**
 * @brief Resolves a VkrUiSize to pixels given context.
 *
 * @param size The size to resolve
 * @param parent_size Parent dimension (for percent)
 * @param viewport_size Viewport dimension (for vw/vh)
 * @param font_size Font size in pixels (for em)
 * @return Resolved size in pixels, or NAN if AUTO
 */
float32_t vkr_ui_size_resolve(VkrUiSize size, float32_t parent_size,
                               float32_t viewport_size, float32_t font_size);
```

## Box Model

### Edge Insets

```c
/**
 * @brief Insets for all four edges (margin, padding, border).
 */
typedef struct VkrUiEdges {
  float32_t top;
  float32_t right;
  float32_t bottom;
  float32_t left;
} VkrUiEdges;

// Convenience constructors
#define VKR_UI_EDGES_ZERO     ((VkrUiEdges){0, 0, 0, 0})
#define VKR_UI_EDGES_ALL(v)   ((VkrUiEdges){(v), (v), (v), (v)})
#define VKR_UI_EDGES_SYMMETRIC(v, h) ((VkrUiEdges){(v), (h), (v), (h)})
#define VKR_UI_EDGES_TRBL(t, r, b, l) ((VkrUiEdges){(t), (r), (b), (l)})

/**
 * @brief Gets horizontal sum (left + right).
 */
static inline float32_t vkr_ui_edges_horizontal(VkrUiEdges e) {
  return e.left + e.right;
}

/**
 * @brief Gets vertical sum (top + bottom).
 */
static inline float32_t vkr_ui_edges_vertical(VkrUiEdges e) {
  return e.top + e.bottom;
}
```

### Box Model Components

```
+-----------------------------------------------+
|                   MARGIN                      |
|   +---------------------------------------+   |
|   |              BORDER                   |   |
|   |   +-------------------------------+   |   |
|   |   |          PADDING              |   |   |
|   |   |   +-----------------------+   |   |   |
|   |   |   |                       |   |   |   |
|   |   |   |     CONTENT BOX       |   |   |   |
|   |   |   |                       |   |   |   |
|   |   |   +-----------------------+   |   |   |
|   |   |                               |   |   |
|   |   +-------------------------------+   |   |
|   |                                       |   |
|   +---------------------------------------+   |
|                                               |
+-----------------------------------------------+
```

```c
/**
 * @brief Box model for a layout node.
 */
typedef struct VkrUiBoxModel {
  VkrUiEdges margin;        /**< Space outside border */
  VkrUiEdges border_width;  /**< Border thickness */
  VkrUiEdges padding;       /**< Space inside border */
} VkrUiBoxModel;

/**
 * @brief Box sizing mode.
 */
typedef enum VkrUiBoxSizing {
  VKR_UI_BOX_SIZING_CONTENT_BOX = 0, /**< width/height = content only */
  VKR_UI_BOX_SIZING_BORDER_BOX,      /**< width/height includes padding+border */
} VkrUiBoxSizing;
```

## Flexbox Layout

### Flex Container Properties

```c
/**
 * @brief Main axis direction.
 */
typedef enum VkrUiFlexDirection {
  VKR_UI_FLEX_ROW = 0,           /**< Left to right */
  VKR_UI_FLEX_ROW_REVERSE,       /**< Right to left */
  VKR_UI_FLEX_COLUMN,            /**< Top to bottom */
  VKR_UI_FLEX_COLUMN_REVERSE,    /**< Bottom to top */
} VkrUiFlexDirection;

/**
 * @brief How to distribute space on main axis.
 */
typedef enum VkrUiJustifyContent {
  VKR_UI_JUSTIFY_FLEX_START = 0, /**< Pack items to start */
  VKR_UI_JUSTIFY_FLEX_END,       /**< Pack items to end */
  VKR_UI_JUSTIFY_CENTER,         /**< Center items */
  VKR_UI_JUSTIFY_SPACE_BETWEEN,  /**< Even space between items */
  VKR_UI_JUSTIFY_SPACE_AROUND,   /**< Even space around items */
  VKR_UI_JUSTIFY_SPACE_EVENLY,   /**< Equal space between all */
} VkrUiJustifyContent;

/**
 * @brief How to align items on cross axis.
 */
typedef enum VkrUiAlignItems {
  VKR_UI_ALIGN_STRETCH = 0,      /**< Stretch to fill container */
  VKR_UI_ALIGN_FLEX_START,       /**< Align to start of cross axis */
  VKR_UI_ALIGN_FLEX_END,         /**< Align to end of cross axis */
  VKR_UI_ALIGN_CENTER,           /**< Center on cross axis */
  VKR_UI_ALIGN_BASELINE,         /**< Align text baselines */
} VkrUiAlignItems;

/**
 * @brief Whether items wrap to multiple lines.
 */
typedef enum VkrUiFlexWrap {
  VKR_UI_FLEX_NO_WRAP = 0,       /**< Single line, may overflow */
  VKR_UI_FLEX_WRAP,              /**< Wrap to next line */
  VKR_UI_FLEX_WRAP_REVERSE,      /**< Wrap to previous line */
} VkrUiFlexWrap;

/**
 * @brief How to align multiple lines on cross axis.
 */
typedef enum VkrUiAlignContent {
  VKR_UI_ALIGN_CONTENT_STRETCH = 0,
  VKR_UI_ALIGN_CONTENT_FLEX_START,
  VKR_UI_ALIGN_CONTENT_FLEX_END,
  VKR_UI_ALIGN_CONTENT_CENTER,
  VKR_UI_ALIGN_CONTENT_SPACE_BETWEEN,
  VKR_UI_ALIGN_CONTENT_SPACE_AROUND,
} VkrUiAlignContent;
```

### Flex Item Properties

```c
/**
 * @brief Individual item's cross-axis alignment override.
 */
typedef enum VkrUiAlignSelf {
  VKR_UI_ALIGN_SELF_AUTO = 0,    /**< Use parent's align_items */
  VKR_UI_ALIGN_SELF_STRETCH,
  VKR_UI_ALIGN_SELF_FLEX_START,
  VKR_UI_ALIGN_SELF_FLEX_END,
  VKR_UI_ALIGN_SELF_CENTER,
  VKR_UI_ALIGN_SELF_BASELINE,
} VkrUiAlignSelf;
```

## Position Modes

```c
/**
 * @brief How the element is positioned.
 */
typedef enum VkrUiPosition {
  VKR_UI_POSITION_RELATIVE = 0,  /**< Normal flow, offsets relative to normal */
  VKR_UI_POSITION_ABSOLUTE,      /**< Removed from flow, relative to parent */
  VKR_UI_POSITION_FIXED,         /**< Relative to viewport */
} VkrUiPosition;
```

## Layout Style

```c
/**
 * @brief Complete layout style for a node.
 * Pure data - no renderer dependencies.
 */
typedef struct VkrUiLayoutStyle {
  // Display and position
  VkrUiPosition position;

  // Flex container
  VkrUiFlexDirection flex_direction;
  VkrUiFlexWrap flex_wrap;
  VkrUiJustifyContent justify_content;
  VkrUiAlignItems align_items;
  VkrUiAlignContent align_content;
  float32_t gap;             /**< Gap between flex items */
  float32_t row_gap;         /**< Gap between rows (if wrapping) */
  float32_t column_gap;      /**< Gap between columns */

  // Flex item
  float32_t flex_grow;       /**< How much to grow (0 = don't) */
  float32_t flex_shrink;     /**< How much to shrink (1 = normal) */
  VkrUiSize flex_basis;      /**< Initial main size before flex */
  VkrUiAlignSelf align_self; /**< Override parent's align_items */
  int32_t order;             /**< Sort order (lower first) */

  // Size constraints
  VkrUiSize width;
  VkrUiSize height;
  VkrUiSize min_width;
  VkrUiSize min_height;
  VkrUiSize max_width;
  VkrUiSize max_height;

  // Position offsets (for RELATIVE/ABSOLUTE/FIXED)
  VkrUiSize left;
  VkrUiSize top;
  VkrUiSize right;
  VkrUiSize bottom;

  // Box model
  VkrUiEdges margin;
  VkrUiEdges padding;
  VkrUiBoxSizing box_sizing;

  // Content sizing
  float32_t aspect_ratio;    /**< Width/height ratio (0 = none) */
} VkrUiLayoutStyle;

/**
 * @brief Default layout style values.
 */
#define VKR_UI_LAYOUT_STYLE_DEFAULT ((VkrUiLayoutStyle){ \
  .position = VKR_UI_POSITION_RELATIVE,                  \
  .flex_direction = VKR_UI_FLEX_ROW,                     \
  .flex_wrap = VKR_UI_FLEX_NO_WRAP,                      \
  .justify_content = VKR_UI_JUSTIFY_FLEX_START,          \
  .align_items = VKR_UI_ALIGN_STRETCH,                   \
  .align_content = VKR_UI_ALIGN_CONTENT_STRETCH,         \
  .gap = 0.0f,                                           \
  .flex_grow = 0.0f,                                     \
  .flex_shrink = 1.0f,                                   \
  .flex_basis = VKR_UI_SIZE_AUTO,                        \
  .align_self = VKR_UI_ALIGN_SELF_AUTO,                  \
  .order = 0,                                            \
  .width = VKR_UI_SIZE_AUTO,                             \
  .height = VKR_UI_SIZE_AUTO,                            \
  .min_width = VKR_UI_SIZE_PX(0),                        \
  .min_height = VKR_UI_SIZE_PX(0),                       \
  .max_width = VKR_UI_SIZE_PX(INFINITY),                 \
  .max_height = VKR_UI_SIZE_PX(INFINITY),                \
  .box_sizing = VKR_UI_BOX_SIZING_CONTENT_BOX,           \
  .aspect_ratio = 0.0f,                                  \
})
```

## Layout Node

```c
/**
 * @brief A node in the layout tree.
 * Contains style inputs and computed output.
 */
typedef struct VkrUiLayoutNode {
  // Identity
  uint32_t id;                   /**< Unique node ID */

  // Style input (set by user)
  VkrUiLayoutStyle style;

  // Tree structure
  struct VkrUiLayoutNode *parent;
  struct VkrUiLayoutNode *first_child;
  struct VkrUiLayoutNode *next_sibling;
  uint32_t child_count;

  // Content size callback (for text, images)
  VkrUiMeasureFunc measure;      /**< Optional content measurement */
  void *measure_context;         /**< Context for measure callback */

  // Computed output (filled by layout algorithm)
  VkrUiRect content_rect;        /**< Content box in parent coords */
  VkrUiRect border_rect;         /**< Border box in parent coords */
  VkrUiRect padding_rect;        /**< Padding box in parent coords */
  float32_t baseline;            /**< Text baseline offset from top */

  // Absolute position in screen coords (computed after tree walk)
  VkrUiRect screen_rect;

  // State flags
  bool8_t layout_dirty;          /**< Needs layout recomputation */
  bool8_t subtree_dirty;         /**< Child needs layout */
} VkrUiLayoutNode;
```

### Content Measurement

For elements with intrinsic size (text, images), a measure callback provides content dimensions:

```c
/**
 * @brief Callback to measure intrinsic content size.
 *
 * @param context User context (e.g., VkrUiText pointer)
 * @param known_width Available width (NAN if unknown)
 * @param known_height Available height (NAN if unknown)
 * @param out_width Output: content width
 * @param out_height Output: content height
 * @param out_baseline Output: baseline offset (optional, set to 0 if none)
 */
typedef void (*VkrUiMeasureFunc)(void *context,
                                  float32_t known_width,
                                  float32_t known_height,
                                  float32_t *out_width,
                                  float32_t *out_height,
                                  float32_t *out_baseline);
```

## Layout Tree

```c
/**
 * @brief Layout tree manager.
 */
typedef struct VkrUiLayoutTree {
  VkrAllocator *allocator;       /**< Node allocation */
  VkrUiLayoutNode *root;         /**< Root node */
  Array_VkrUiLayoutNode nodes;   /**< All nodes (flat storage) */
  uint32_t next_id;              /**< Next node ID */

  // Viewport for vw/vh units
  float32_t viewport_width;
  float32_t viewport_height;

  // Default font size for em units
  float32_t default_font_size;
} VkrUiLayoutTree;
```

## Layout Algorithm

### Overview

The layout algorithm follows a multi-pass approach:

1. **Dirty Propagation**: Mark ancestors of dirty nodes
2. **Measure Pass**: Bottom-up content size calculation
3. **Layout Pass**: Top-down position/size assignment
4. **Absolute Positioning**: Second pass for absolute/fixed elements
5. **Screen Coords**: Convert relative coords to screen coords

### Algorithm Pseudocode

```c
void vkr_ui_layout_compute(VkrUiLayoutTree *tree) {
    if (!tree->root || !tree->root->subtree_dirty) {
        return;
    }

    // Context for size resolution
    VkrUiLayoutContext ctx = {
        .viewport_width = tree->viewport_width,
        .viewport_height = tree->viewport_height,
        .font_size = tree->default_font_size,
    };

    // Pass 1: Measure intrinsic sizes (bottom-up)
    vkr_ui_layout_measure(tree->root, &ctx);

    // Pass 2: Compute layout (top-down)
    VkrUiConstraints root_constraints = {
        .min_width = 0,
        .max_width = tree->viewport_width,
        .min_height = 0,
        .max_height = tree->viewport_height,
    };
    vkr_ui_layout_node(tree->root, &ctx, root_constraints);

    // Pass 3: Compute screen coordinates
    vkr_ui_layout_compute_screen_rects(tree->root, 0, 0);

    // Clear dirty flags
    vkr_ui_layout_clear_dirty(tree->root);
}
```

### Flex Layout Implementation

```c
/**
 * @brief Performs flex layout on a container's children.
 */
vkr_internal void vkr_ui_layout_flex(VkrUiLayoutNode *node,
                                     VkrUiLayoutContext *ctx,
                                     float32_t available_width,
                                     float32_t available_height) {
    VkrUiLayoutStyle *style = &node->style;
    bool8_t is_row = (style->flex_direction == VKR_UI_FLEX_ROW ||
                      style->flex_direction == VKR_UI_FLEX_ROW_REVERSE);
    bool8_t is_reverse = (style->flex_direction == VKR_UI_FLEX_ROW_REVERSE ||
                          style->flex_direction == VKR_UI_FLEX_COLUMN_REVERSE);

    float32_t main_size = is_row ? available_width : available_height;
    float32_t cross_size = is_row ? available_height : available_width;

    // Step 1: Collect flex items (skip absolute positioned)
    // Step 2: Calculate hypothetical main sizes
    // Step 3: Determine flex lines (if wrapping)
    // Step 4: Resolve flexible lengths
    // Step 5: Determine cross size of lines
    // Step 6: Align items within lines
    // Step 7: Apply main axis alignment (justify-content)
    // Step 8: Position items

    // ... detailed implementation follows CSS Flexbox spec
}
```

### Size Resolution with Constraints

```c
typedef struct VkrUiConstraints {
  float32_t min_width;
  float32_t max_width;
  float32_t min_height;
  float32_t max_height;
} VkrUiConstraints;

/**
 * @brief Resolves a node's size given constraints.
 * Handles AUTO, percent, min/max, aspect ratio.
 */
vkr_internal Vec2 vkr_ui_layout_resolve_size(VkrUiLayoutNode *node,
                                              VkrUiLayoutContext *ctx,
                                              VkrUiConstraints constraints) {
    VkrUiLayoutStyle *s = &node->style;

    // Resolve explicit width
    float32_t width = vkr_ui_size_resolve(s->width, constraints.max_width,
                                           ctx->viewport_width, ctx->font_size);

    // Resolve explicit height
    float32_t height = vkr_ui_size_resolve(s->height, constraints.max_height,
                                            ctx->viewport_height, ctx->font_size);

    // If AUTO, use content size (from measure callback)
    if (isnan(width) && node->measure) {
        float32_t content_w, content_h, baseline;
        node->measure(node->measure_context, NAN, NAN,
                      &content_w, &content_h, &baseline);
        width = content_w + vkr_ui_edges_horizontal(s->padding);
    }

    if (isnan(height) && node->measure) {
        // May need re-measure with known width for text wrapping
        float32_t content_w, content_h, baseline;
        node->measure(node->measure_context, width, NAN,
                      &content_w, &content_h, &baseline);
        height = content_h + vkr_ui_edges_vertical(s->padding);
    }

    // Apply aspect ratio
    if (s->aspect_ratio > 0.0f) {
        if (isnan(width) && !isnan(height)) {
            width = height * s->aspect_ratio;
        } else if (!isnan(width) && isnan(height)) {
            height = width / s->aspect_ratio;
        }
    }

    // Apply min/max constraints
    float32_t min_w = vkr_ui_size_resolve(s->min_width, constraints.max_width,
                                           ctx->viewport_width, ctx->font_size);
    float32_t max_w = vkr_ui_size_resolve(s->max_width, constraints.max_width,
                                           ctx->viewport_width, ctx->font_size);
    float32_t min_h = vkr_ui_size_resolve(s->min_height, constraints.max_height,
                                           ctx->viewport_height, ctx->font_size);
    float32_t max_h = vkr_ui_size_resolve(s->max_height, constraints.max_height,
                                           ctx->viewport_height, ctx->font_size);

    width = vkr_clamp_f32(width, vkr_max_f32(min_w, constraints.min_width),
                                 vkr_min_f32(max_w, constraints.max_width));
    height = vkr_clamp_f32(height, vkr_max_f32(min_h, constraints.min_height),
                                   vkr_min_f32(max_h, constraints.max_height));

    return vec2_new(width, height);
}
```

## Incremental Layout

### Dirty Flag Propagation

```c
/**
 * @brief Marks a node as needing layout.
 * Propagates subtree_dirty up to root.
 */
void vkr_ui_layout_mark_dirty(VkrUiLayoutNode *node) {
    if (!node || node->layout_dirty) {
        return;
    }

    node->layout_dirty = true_v;

    // Propagate up
    VkrUiLayoutNode *parent = node->parent;
    while (parent) {
        if (parent->subtree_dirty) {
            break; // Already marked
        }
        parent->subtree_dirty = true_v;
        parent = parent->parent;
    }
}

/**
 * @brief Sets a style property and marks layout dirty if changed.
 */
void vkr_ui_layout_set_width(VkrUiLayoutNode *node, VkrUiSize width) {
    if (node->style.width.value != width.value ||
        node->style.width.unit != width.unit) {
        node->style.width = width;
        vkr_ui_layout_mark_dirty(node);
    }
}
```

### Incremental Computation

The layout algorithm only processes dirty subtrees:

```c
vkr_internal void vkr_ui_layout_node(VkrUiLayoutNode *node,
                                      VkrUiLayoutContext *ctx,
                                      VkrUiConstraints constraints) {
    if (!node->layout_dirty && !node->subtree_dirty) {
        return; // Skip clean subtrees
    }

    // ... perform layout for this node ...

    // Process children
    VkrUiLayoutNode *child = node->first_child;
    while (child) {
        if (child->layout_dirty || child->subtree_dirty) {
            VkrUiConstraints child_constraints = /* compute from node */;
            vkr_ui_layout_node(child, ctx, child_constraints);
        }
        child = child->next_sibling;
    }
}
```

## API

### Tree Management

```c
/**
 * @brief Creates a layout tree.
 */
bool8_t vkr_ui_layout_tree_create(VkrUiLayoutTree *tree,
                                   VkrAllocator *allocator,
                                   uint32_t max_nodes);

/**
 * @brief Destroys a layout tree.
 */
void vkr_ui_layout_tree_destroy(VkrUiLayoutTree *tree);

/**
 * @brief Sets viewport dimensions (for vw/vh units and root constraints).
 */
void vkr_ui_layout_tree_set_viewport(VkrUiLayoutTree *tree,
                                      float32_t width, float32_t height);
```

### Node Management

```c
/**
 * @brief Creates a layout node.
 */
VkrUiLayoutNode *vkr_ui_layout_node_create(VkrUiLayoutTree *tree,
                                            VkrUiLayoutNode *parent);

/**
 * @brief Destroys a layout node and its children.
 */
void vkr_ui_layout_node_destroy(VkrUiLayoutTree *tree,
                                 VkrUiLayoutNode *node);

/**
 * @brief Reparents a node.
 */
void vkr_ui_layout_node_set_parent(VkrUiLayoutNode *node,
                                    VkrUiLayoutNode *new_parent);

/**
 * @brief Sets content measurement callback.
 */
void vkr_ui_layout_node_set_measure(VkrUiLayoutNode *node,
                                     VkrUiMeasureFunc measure,
                                     void *context);
```

### Style Setters (Auto-dirty)

```c
// Each setter marks dirty if value changed
void vkr_ui_layout_set_width(VkrUiLayoutNode *node, VkrUiSize width);
void vkr_ui_layout_set_height(VkrUiLayoutNode *node, VkrUiSize height);
void vkr_ui_layout_set_flex_direction(VkrUiLayoutNode *node, VkrUiFlexDirection dir);
void vkr_ui_layout_set_justify_content(VkrUiLayoutNode *node, VkrUiJustifyContent jc);
void vkr_ui_layout_set_align_items(VkrUiLayoutNode *node, VkrUiAlignItems ai);
void vkr_ui_layout_set_flex_grow(VkrUiLayoutNode *node, float32_t grow);
void vkr_ui_layout_set_flex_shrink(VkrUiLayoutNode *node, float32_t shrink);
void vkr_ui_layout_set_margin(VkrUiLayoutNode *node, VkrUiEdges margin);
void vkr_ui_layout_set_padding(VkrUiLayoutNode *node, VkrUiEdges padding);
void vkr_ui_layout_set_gap(VkrUiLayoutNode *node, float32_t gap);
// ... etc for all style properties
```

### Layout Computation

```c
/**
 * @brief Computes layout for dirty nodes.
 * Call once per frame before rendering.
 */
void vkr_ui_layout_compute(VkrUiLayoutTree *tree);

/**
 * @brief Gets the screen-space rectangle for a node.
 */
VkrUiRect vkr_ui_layout_get_screen_rect(VkrUiLayoutNode *node);

/**
 * @brief Gets the content rectangle (excluding padding).
 */
VkrUiRect vkr_ui_layout_get_content_rect(VkrUiLayoutNode *node);
```

## Usage Examples

### Horizontal Menu Bar

```c
VkrUiLayoutNode *menu_bar = vkr_ui_layout_node_create(tree, root);
vkr_ui_layout_set_width(menu_bar, VKR_UI_SIZE_PERCENT(100));
vkr_ui_layout_set_height(menu_bar, VKR_UI_SIZE_PX(40));
vkr_ui_layout_set_flex_direction(menu_bar, VKR_UI_FLEX_ROW);
vkr_ui_layout_set_justify_content(menu_bar, VKR_UI_JUSTIFY_FLEX_START);
vkr_ui_layout_set_align_items(menu_bar, VKR_UI_ALIGN_CENTER);
vkr_ui_layout_set_padding(menu_bar, VKR_UI_EDGES_SYMMETRIC(0, 10));
vkr_ui_layout_set_gap(menu_bar, 20.0f);

// Menu items
VkrUiLayoutNode *file_item = vkr_ui_layout_node_create(tree, menu_bar);
vkr_ui_layout_set_padding(file_item, VKR_UI_EDGES_SYMMETRIC(5, 10));
// Text content sets intrinsic width via measure callback

VkrUiLayoutNode *edit_item = vkr_ui_layout_node_create(tree, menu_bar);
// ...
```

### Centered Dialog

```c
VkrUiLayoutNode *overlay = vkr_ui_layout_node_create(tree, root);
vkr_ui_layout_set_position(overlay, VKR_UI_POSITION_FIXED);
vkr_ui_layout_set_width(overlay, VKR_UI_SIZE_PERCENT(100));
vkr_ui_layout_set_height(overlay, VKR_UI_SIZE_PERCENT(100));
vkr_ui_layout_set_justify_content(overlay, VKR_UI_JUSTIFY_CENTER);
vkr_ui_layout_set_align_items(overlay, VKR_UI_ALIGN_CENTER);

VkrUiLayoutNode *dialog = vkr_ui_layout_node_create(tree, overlay);
vkr_ui_layout_set_width(dialog, VKR_UI_SIZE_PX(400));
vkr_ui_layout_set_height(dialog, VKR_UI_SIZE_AUTO);
vkr_ui_layout_set_flex_direction(dialog, VKR_UI_FLEX_COLUMN);
vkr_ui_layout_set_padding(dialog, VKR_UI_EDGES_ALL(20));
vkr_ui_layout_set_gap(dialog, 15.0f);
```

### Responsive Sidebar

```c
VkrUiLayoutNode *sidebar = vkr_ui_layout_node_create(tree, container);
vkr_ui_layout_set_flex_shrink(sidebar, 0.0f);  // Don't shrink
vkr_ui_layout_set_flex_grow(sidebar, 0.0f);    // Don't grow
vkr_ui_layout_set_width(sidebar, VKR_UI_SIZE_PX(250));
vkr_ui_layout_set_min_width(sidebar, VKR_UI_SIZE_PX(200));
vkr_ui_layout_set_max_width(sidebar, VKR_UI_SIZE_VW(30)); // Max 30% of viewport
vkr_ui_layout_set_height(sidebar, VKR_UI_SIZE_PERCENT(100));
vkr_ui_layout_set_flex_direction(sidebar, VKR_UI_FLEX_COLUMN);

VkrUiLayoutNode *main_content = vkr_ui_layout_node_create(tree, container);
vkr_ui_layout_set_flex_grow(main_content, 1.0f);  // Fill remaining space
vkr_ui_layout_set_flex_shrink(main_content, 1.0f);
```

## Performance Considerations

### Memory Layout

Nodes are stored in a flat array for cache efficiency:

```c
typedef struct VkrUiLayoutTree {
  Array_VkrUiLayoutNode nodes;  // Contiguous storage
  // ...
} VkrUiLayoutTree;
```

### Avoiding Allocations

- Style changes don't allocate
- Layout computation uses no dynamic allocation
- Tree modifications use pool allocation

### Batching Changes

```c
// Bad: triggers layout multiple times
vkr_ui_layout_set_width(node, width);
vkr_ui_layout_compute(tree);  // Layout 1
vkr_ui_layout_set_height(node, height);
vkr_ui_layout_compute(tree);  // Layout 2

// Good: batch changes, single layout
vkr_ui_layout_set_width(node, width);
vkr_ui_layout_set_height(node, height);
vkr_ui_layout_compute(tree);  // Single layout
```

## Implementation Phases

### Phase 1: Core Infrastructure
- Size types and resolution
- Box model calculations
- Layout node structure
- Tree management

### Phase 2: Basic Flex Layout
- Flex direction (row/column)
- Justify content
- Align items
- Gap handling

### Phase 3: Advanced Flex
- Flex grow/shrink
- Flex wrap
- Align content
- Order property

### Phase 4: Position Modes
- Absolute positioning
- Fixed positioning
- Z-layer handling

### Phase 5: Optimization
- Dirty flag propagation
- Incremental layout
- Performance profiling

## Testing Strategy

### Unit Tests

```c
void test_flex_row_justify_center() {
    VkrUiLayoutTree tree;
    vkr_ui_layout_tree_create(&tree, &allocator, 16);
    vkr_ui_layout_tree_set_viewport(&tree, 800, 600);

    VkrUiLayoutNode *root = tree.root;
    vkr_ui_layout_set_width(root, VKR_UI_SIZE_PX(800));
    vkr_ui_layout_set_height(root, VKR_UI_SIZE_PX(100));
    vkr_ui_layout_set_flex_direction(root, VKR_UI_FLEX_ROW);
    vkr_ui_layout_set_justify_content(root, VKR_UI_JUSTIFY_CENTER);

    VkrUiLayoutNode *child = vkr_ui_layout_node_create(&tree, root);
    vkr_ui_layout_set_width(child, VKR_UI_SIZE_PX(100));
    vkr_ui_layout_set_height(child, VKR_UI_SIZE_PX(50));

    vkr_ui_layout_compute(&tree);

    VkrUiRect rect = vkr_ui_layout_get_screen_rect(child);
    assert(rect.x == 350.0f);  // (800 - 100) / 2
    assert(rect.y == 0.0f);
    assert(rect.width == 100.0f);
    assert(rect.height == 50.0f);

    vkr_ui_layout_tree_destroy(&tree);
}
```

### Visual Tests

Compare rendered output against reference images for complex layouts.

## References

- [CSS Flexible Box Layout Module Level 1](https://www.w3.org/TR/css-flexbox-1/)
- [Yoga Layout Engine](https://yogalayout.com/)
- [Flutter Layout System](https://flutter.dev/docs/development/ui/layout)
