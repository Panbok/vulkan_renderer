---
status: superseded
updated: 2026-08-12
authority: design
---
# UI Element Primitives Design

**Superseded by [ui-architecture-spec.md](../ui/ui-architecture-spec.md) §7**
(rationale in [ADR-027](../architecture/adr/027-immediate-mode-grid-ui.md)).
The retained element tree with generational handles is replaced by hashed
immediate-mode identity over a retained state table, and the per-element draw
command list by a single per-frame vertex stream split into scissored batches.

**Legacy note:** This document references the deprecated view/layer system
(`VkrLayerContext`) in renderer APIs. Render orchestration now uses the render
graph; view modules are render helpers invoked by pass executors.

## Document Purpose

This document describes the base UI element types that serve as building blocks for the UI system. Elements are the visual representations of layout nodes, handling rendering, hit testing, and state management.

Related documents:
- [UI System Overview](./ui-system-overview.md) - High-level architecture
- [UI Layout Engine Design](./ui-layout-engine-design.md) - Layout computation
- [UI Components Library Design](../ui/ui-components-library-design.md) - Components built on elements

## Goals

1. **Composable**: Elements combine to form complex UIs
2. **Efficient Rendering**: Batched draw calls with texture atlasing
3. **Hit Testing**: Accurate mouse/touch interaction
4. **State Management**: Hover, focus, pressed states
5. **Text Integration**: Seamless integration with existing VkrUiText

## Location

```
lib/src/renderer/resources/ui/
├── vkr_ui_element.h        # Base element type
├── vkr_ui_element.c
├── vkr_ui_container.h      # Container element
├── vkr_ui_container.c
├── vkr_ui_image.h          # Image/texture element
├── vkr_ui_image.c
├── vkr_ui_text_element.h   # Text element (wraps VkrUiText)
└── vkr_ui_text_element.c

lib/src/renderer/systems/
├── vkr_ui_renderer.h       # Batched rendering
├── vkr_ui_renderer.c
├── vkr_ui_input.h          # Hit testing and input
└── vkr_ui_input.c
```

## Base Element

### Element Handle

```c
/**
 * @brief Opaque handle to a UI element.
 */
typedef struct VkrUiElementHandle {
  uint32_t id;          /**< Index into element array */
  uint32_t generation;  /**< Generation for stale handle detection */
} VkrUiElementHandle;

#define VKR_UI_ELEMENT_HANDLE_INVALID ((VkrUiElementHandle){0, 0})

/**
 * @brief Checks if handle is valid.
 */
static inline bool8_t vkr_ui_element_handle_is_valid(VkrUiElementHandle h) {
  return h.id != 0;
}
```

### Element Flags

```c
/**
 * @brief Element behavior flags.
 */
typedef enum VkrUiElementFlags {
  VKR_UI_ELEMENT_FLAG_NONE = 0,
  VKR_UI_ELEMENT_FLAG_FOCUSABLE = (1 << 0),     /**< Can receive keyboard focus */
  VKR_UI_ELEMENT_FLAG_DRAGGABLE = (1 << 1),     /**< Can be dragged */
  VKR_UI_ELEMENT_FLAG_DROP_TARGET = (1 << 2),   /**< Accepts drops */
  VKR_UI_ELEMENT_FLAG_CLIP_CHILDREN = (1 << 3), /**< Clip children to bounds */
  VKR_UI_ELEMENT_FLAG_DISABLED = (1 << 4),      /**< Ignores input */
  VKR_UI_ELEMENT_FLAG_HIDDEN = (1 << 5),        /**< Not rendered */
  VKR_UI_ELEMENT_FLAG_HIT_TEST_SELF = (1 << 6), /**< Self participates in hit test */
  VKR_UI_ELEMENT_FLAG_HIT_TEST_CHILDREN = (1 << 7), /**< Children participate */
} VkrUiElementFlags;

#define VKR_UI_ELEMENT_FLAGS_DEFAULT \
  (VKR_UI_ELEMENT_FLAG_HIT_TEST_SELF | VKR_UI_ELEMENT_FLAG_HIT_TEST_CHILDREN)
```

### Element State

```c
/**
 * @brief Interactive state of an element.
 */
typedef enum VkrUiElementState {
  VKR_UI_ELEMENT_STATE_NORMAL = 0,
  VKR_UI_ELEMENT_STATE_HOVERED,   /**< Mouse is over element */
  VKR_UI_ELEMENT_STATE_PRESSED,   /**< Mouse down on element */
  VKR_UI_ELEMENT_STATE_FOCUSED,   /**< Has keyboard focus */
  VKR_UI_ELEMENT_STATE_DISABLED,  /**< Interaction disabled */
} VkrUiElementState;
```

### Visual Style

```c
/**
 * @brief Visual style for rendering an element.
 * Separate from layout style (which is in layout node).
 */
typedef struct VkrUiVisualStyle {
  // Background
  Vec4 background_color;          /**< RGBA (0,0,0,0 = transparent) */
  VkrTextureHandle background_image;
  VkrUiBackgroundFit background_fit;

  // Border
  float32_t border_width;         /**< Border thickness in pixels */
  float32_t border_radius;        /**< Corner radius in pixels */
  Vec4 border_color;              /**< Border RGBA */

  // Text (for text elements)
  VkrFontHandle font;
  float32_t font_size;
  Vec4 text_color;
  VkrTextAlign text_align;

  // Effects
  float32_t opacity;              /**< 0.0 - 1.0 */
  Vec4 tint_color;                /**< Multiplied with content */
} VkrUiVisualStyle;

/**
 * @brief Background fit mode for images.
 */
typedef enum VkrUiBackgroundFit {
  VKR_UI_BACKGROUND_FIT_STRETCH = 0, /**< Stretch to fill */
  VKR_UI_BACKGROUND_FIT_CONTAIN,     /**< Fit inside, preserving aspect */
  VKR_UI_BACKGROUND_FIT_COVER,       /**< Fill, cropping if needed */
  VKR_UI_BACKGROUND_FIT_TILE,        /**< Repeat to fill */
} VkrUiBackgroundFit;

#define VKR_UI_VISUAL_STYLE_DEFAULT ((VkrUiVisualStyle){ \
  .background_color = {0.0f, 0.0f, 0.0f, 0.0f},          \
  .border_width = 0.0f,                                   \
  .border_radius = 0.0f,                                  \
  .border_color = {0.0f, 0.0f, 0.0f, 0.0f},              \
  .opacity = 1.0f,                                        \
  .tint_color = {1.0f, 1.0f, 1.0f, 1.0f},                \
})
```

### Element Structure

```c
/**
 * @brief Base UI element.
 * All element types embed or extend this structure.
 */
typedef struct VkrUiElement {
  // Identity
  VkrUiElementHandle handle;
  VkrUiElementType type;
  uint32_t flags;

  // Layout link (owned by layout tree)
  VkrUiLayoutNode *layout_node;

  // Visual style per state
  VkrUiVisualStyle styles[VKR_UI_ELEMENT_STATE_COUNT];
  VkrUiElementState current_state;

  // Tree structure (mirrors layout tree)
  VkrUiElementHandle parent;
  VkrUiElementHandle first_child;
  VkrUiElementHandle next_sibling;

  // Z-ordering
  VkrUiZLayer z_layer;
  int32_t z_index;            /**< Within layer */

  // Event callbacks
  VkrUiEventHandler on_mouse_enter;
  VkrUiEventHandler on_mouse_leave;
  VkrUiEventHandler on_mouse_down;
  VkrUiEventHandler on_mouse_up;
  VkrUiEventHandler on_click;
  VkrUiEventHandler on_double_click;
  VkrUiEventHandler on_drag_start;
  VkrUiEventHandler on_drag_move;
  VkrUiEventHandler on_drag_end;
  VkrUiEventHandler on_key_down;
  VkrUiEventHandler on_key_up;
  VkrUiEventHandler on_focus_gained;
  VkrUiEventHandler on_focus_lost;
  VkrUiEventHandler on_scroll;

  // User data
  void *user_data;

  // Render cache (updated when style/layout changes)
  bool8_t render_dirty;
  VkrUiRenderData render_data;
} VkrUiElement;

/**
 * @brief Event handler callback.
 */
typedef struct VkrUiEvent VkrUiEvent;
typedef bool8_t (*VkrUiEventHandler)(VkrUiElement *element, VkrUiEvent *event, void *user_data);
```

### Event Structure

```c
/**
 * @brief UI event data.
 */
typedef struct VkrUiEvent {
  VkrUiEventType type;
  VkrUiElementHandle target;      /**< Original target */
  VkrUiElementHandle current;     /**< Current handler */

  // Mouse data (for mouse events)
  Vec2 screen_position;           /**< Position in screen coords */
  Vec2 local_position;            /**< Position relative to element */
  Vec2 delta;                     /**< Movement since last event */
  Buttons button;                 /**< Which button */
  int8_t scroll_delta;            /**< Scroll amount */

  // Key data (for keyboard events)
  Keys key;
  bool8_t shift;
  bool8_t ctrl;
  bool8_t alt;

  // Propagation control
  bool8_t stop_propagation;       /**< Stop bubbling */
  bool8_t prevent_default;        /**< Prevent default behavior */
} VkrUiEvent;
```

## Element Types

### Container Element

Containers are elements that can have children. They provide visual grouping and clipping.

```c
/**
 * @brief Container element (can have children).
 */
typedef struct VkrUiContainer {
  VkrUiElement base;              /**< Embedded base element */

  // Scroll state
  bool8_t scrollable_x;
  bool8_t scrollable_y;
  Vec2 scroll_offset;
  Vec2 content_size;              /**< Total content size (may exceed bounds) */

  // Clipping
  bool8_t clip_overflow;
} VkrUiContainer;

/**
 * @brief Container creation config.
 */
typedef struct VkrUiContainerConfig {
  VkrUiLayoutStyle layout;        /**< Layout style */
  VkrUiVisualStyle style;         /**< Visual style (normal state) */
  uint32_t flags;
  bool8_t scrollable_x;
  bool8_t scrollable_y;
} VkrUiContainerConfig;
```

### Image Element

```c
/**
 * @brief Image/texture display element.
 */
typedef struct VkrUiImage {
  VkrUiElement base;

  VkrTextureHandle texture;
  VkrUiRect source_rect;          /**< Source rectangle in texture (for atlases) */
  VkrUiBackgroundFit fit;
  bool8_t preserve_aspect;

  // Nine-slice mode
  bool8_t use_nine_slice;
  VkrUiEdges slice_borders;       /**< Border sizes for nine-slice */
} VkrUiImage;

/**
 * @brief Image creation config.
 */
typedef struct VkrUiImageConfig {
  VkrTextureHandle texture;
  VkrUiRect source_rect;          /**< {0,0,0,0} = full texture */
  VkrUiBackgroundFit fit;
  bool8_t preserve_aspect;
  VkrUiLayoutStyle layout;
  VkrUiVisualStyle style;
} VkrUiImageConfig;
```

### Text Element

Wraps the existing VkrUiText for integration.

```c
/**
 * @brief Text display element.
 * Integrates with existing VkrUiText system.
 */
typedef struct VkrUiTextElement {
  VkrUiElement base;

  // Text content (owned)
  String8 content;
  bool8_t content_owned;

  // Text styling
  VkrFontHandle font;
  float32_t font_size;
  Vec4 color;
  VkrTextAlign horizontal_align;
  VkrTextBaseline vertical_align;
  float32_t line_height;
  float32_t letter_spacing;

  // Layout options
  bool8_t word_wrap;
  bool8_t clip_overflow;

  // Computed layout
  VkrTextLayout text_layout;
  bool8_t layout_dirty;

  // Render resources (from VkrUiText)
  VkrUiTextRenderState render;
} VkrUiTextElement;

/**
 * @brief Text element creation config.
 */
typedef struct VkrUiTextElementConfig {
  String8 content;
  VkrFontHandle font;             /**< Invalid = default font */
  float32_t font_size;            /**< 0 = default */
  Vec4 color;
  VkrTextAlign horizontal_align;
  VkrTextBaseline vertical_align;
  bool8_t word_wrap;
  VkrUiLayoutStyle layout;
} VkrUiTextElementConfig;
```

### Text Measurement for Layout

Text elements provide a measurement callback for the layout engine:

```c
/**
 * @brief Measures text content for layout.
 */
vkr_internal void vkr_ui_text_element_measure(void *context,
                                               float32_t known_width,
                                               float32_t known_height,
                                               float32_t *out_width,
                                               float32_t *out_height,
                                               float32_t *out_baseline) {
    VkrUiTextElement *text = (VkrUiTextElement *)context;

    // Recompute layout if dirty or constraints changed
    VkrTextLayoutOptions opts = {
        .max_width = isnan(known_width) ? 0.0f : known_width,
        .max_height = isnan(known_height) ? 0.0f : known_height,
        .word_wrap = text->word_wrap,
        .anchor = {text->horizontal_align, text->vertical_align},
    };

    VkrText vkr_text = {
        .content = text->content,
        .style = {
            .font = text->font,
            .font_size = text->font_size,
            .letter_spacing = text->letter_spacing,
            .line_height = text->line_height,
        },
    };

    VkrTextBounds bounds = vkr_text_measure_wrapped(&vkr_text, opts.max_width);

    *out_width = bounds.size.x;
    *out_height = bounds.size.y;
    *out_baseline = bounds.ascent;
}
```

## UI Renderer

### Render Data

```c
/**
 * @brief Pre-computed render data for an element.
 */
typedef struct VkrUiRenderData {
  VkrUiRect screen_rect;          /**< Final screen position */
  VkrUiRect clip_rect;            /**< Clipping rectangle */
  Vec4 color;                     /**< Final color (with opacity) */
  VkrTextureHandle texture;
  VkrUiRect tex_coords;           /**< Texture coordinates */
  float32_t border_radius;
  float32_t border_width;
  Vec4 border_color;
} VkrUiRenderData;
```

### Draw Command Types

```c
/**
 * @brief Types of UI draw commands.
 */
typedef enum VkrUiDrawType {
  VKR_UI_DRAW_QUAD = 0,           /**< Solid color or textured quad */
  VKR_UI_DRAW_NINE_SLICE,         /**< Nine-slice image */
  VKR_UI_DRAW_TEXT,               /**< Text glyphs */
  VKR_UI_DRAW_ROUNDED_RECT,       /**< Rounded rectangle (requires SDF) */
  VKR_UI_DRAW_SCISSOR_PUSH,       /**< Push scissor rect */
  VKR_UI_DRAW_SCISSOR_POP,        /**< Pop scissor rect */
} VkrUiDrawType;

/**
 * @brief A single UI draw command.
 */
typedef struct VkrUiDrawCommand {
  VkrUiDrawType type;
  union {
    struct {
      VkrUiRect rect;
      Vec4 color;
      VkrTextureHandle texture;
      VkrUiRect tex_coords;
      float32_t border_radius;
    } quad;

    struct {
      VkrUiRect rect;
      VkrTextureHandle texture;
      VkrUiEdges borders;
      Vec4 tint;
    } nine_slice;

    struct {
      VkrUiTextRenderState *render;
      Vec2 position;
      Vec4 color;
    } text;

    struct {
      VkrUiRect rect;
    } scissor;
  } data;
} VkrUiDrawCommand;
```

### Renderer System

```c
/**
 * @brief Batched UI renderer.
 */
typedef struct VkrUiRenderer {
  VkrRendererFrontendHandle renderer;
  VkrAllocator *allocator;

  // Draw command buffer
  Array_VkrUiDrawCommand commands;
  uint32_t command_count;

  // Vertex/index buffers (dynamic, resized as needed)
  VkrVertexBuffer vertex_buffer;
  VkrIndexBuffer index_buffer;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t vertex_capacity;
  uint32_t index_capacity;

  // Pipelines
  VkrPipelineHandle quad_pipeline;
  VkrPipelineHandle text_pipeline;
  VkrPipelineHandle rounded_rect_pipeline;

  // White texture for solid colors
  VkrTextureHandle white_texture;

  // Scissor stack
  VkrUiRect scissor_stack[16];
  uint32_t scissor_depth;

  // Current batch state
  VkrTextureHandle current_texture;
  VkrPipelineHandle current_pipeline;
  uint32_t batch_start_vertex;
  uint32_t batch_start_index;
} VkrUiRenderer;
```

### Renderer API

```c
/**
 * @brief Initializes the UI renderer.
 */
bool8_t vkr_ui_renderer_init(VkrUiRenderer *renderer,
                              VkrRendererFrontendHandle rf,
                              VkrAllocator *allocator,
                              VkrRendererError *out_error);

/**
 * @brief Destroys the UI renderer.
 */
void vkr_ui_renderer_shutdown(VkrUiRenderer *renderer);

/**
 * @brief Begins a new render frame.
 * Clears command buffer and resets batching state.
 */
void vkr_ui_renderer_begin_frame(VkrUiRenderer *renderer);

/**
 * @brief Submits a UI element for rendering.
 * Elements are sorted by z-layer and batched by texture.
 */
void vkr_ui_renderer_submit(VkrUiRenderer *renderer, VkrUiElement *element);

/**
 * @brief Builds draw commands from submitted elements.
 * Call after submitting all elements.
 */
void vkr_ui_renderer_build_commands(VkrUiRenderer *renderer);

/**
 * @brief Executes draw commands.
 * Call during render-graph pass execution.
 */
void vkr_ui_renderer_execute(VkrUiRenderer *renderer,
                             const VkrRgPassContext *ctx);
```

### Batching Algorithm

```c
/**
 * @brief Builds batched draw commands from elements.
 */
void vkr_ui_renderer_build_commands(VkrUiRenderer *renderer) {
    // Sort elements by (z_layer, z_index, tree_depth)
    // Elements within same batch criteria are merged

    VkrUiRenderer *r = renderer;
    r->command_count = 0;

    // Reset vertex/index counts
    r->vertex_count = 0;
    r->index_count = 0;

    // Group by texture and type for batching
    VkrTextureHandle current_texture = VKR_TEXTURE_HANDLE_INVALID;
    VkrUiDrawType current_type = VKR_UI_DRAW_QUAD;

    for (uint32_t i = 0; i < /* sorted element count */; ++i) {
        VkrUiElement *elem = /* get sorted element */;

        // Skip hidden elements
        if (elem->flags & VKR_UI_ELEMENT_FLAG_HIDDEN) {
            continue;
        }

        // Push scissor if clipping
        if (elem->flags & VKR_UI_ELEMENT_FLAG_CLIP_CHILDREN) {
            vkr_ui_renderer_push_scissor(r, elem->layout_node->screen_rect);
        }

        // Generate draw command based on element type
        switch (elem->type) {
        case VKR_UI_ELEMENT_TYPE_CONTAINER:
        case VKR_UI_ELEMENT_TYPE_PANEL:
            vkr_ui_renderer_emit_quad(r, elem);
            break;
        case VKR_UI_ELEMENT_TYPE_IMAGE:
            vkr_ui_renderer_emit_image(r, (VkrUiImage *)elem);
            break;
        case VKR_UI_ELEMENT_TYPE_TEXT:
            vkr_ui_renderer_emit_text(r, (VkrUiTextElement *)elem);
            break;
        // ... etc
        }

        // Pop scissor after children rendered
        // (handled by tree traversal order)
    }

    // Upload vertex/index data to GPU
    vkr_ui_renderer_upload_buffers(r);
}
```

### Quad Vertex Format

```c
/**
 * @brief UI quad vertex.
 */
typedef struct VkrUiVertex {
  Vec2 position;     /**< Screen position */
  Vec2 texcoord;     /**< Texture coordinates */
  Vec4 color;        /**< Vertex color (with opacity) */
  float32_t corner;  /**< Corner radius (for SDF) */
} VkrUiVertex;
```

## Hit Testing

### Hit Test System

```c
/**
 * @brief UI input handler with hit testing.
 */
typedef struct VkrUiInputHandler {
  VkrUiSystem *system;

  // Current state
  VkrUiElementHandle hovered_element;
  VkrUiElementHandle focused_element;
  VkrUiElementHandle pressed_element;
  VkrUiElementHandle drag_element;

  // Drag state
  bool8_t is_dragging;
  Vec2 drag_start;
  Vec2 drag_offset;

  // Double-click detection
  float64_t last_click_time;
  Vec2 last_click_pos;
  VkrUiElementHandle last_click_element;

  // Input state
  Vec2 mouse_position;
  Vec2 prev_mouse_position;
} VkrUiInputHandler;

/**
 * @brief Hit test result.
 */
typedef struct VkrUiHitResult {
  VkrUiElementHandle element;     /**< Deepest element hit */
  Vec2 local_position;            /**< Position in element's local coords */
  bool8_t hit;                    /**< True if any element was hit */
} VkrUiHitResult;
```

### Hit Test Algorithm

```c
/**
 * @brief Performs hit testing at screen coordinates.
 */
VkrUiHitResult vkr_ui_hit_test(VkrUiSystem *system, Vec2 screen_pos) {
    VkrUiHitResult result = {
        .element = VKR_UI_ELEMENT_HANDLE_INVALID,
        .hit = false_v,
    };

    // Start from root, traverse in reverse render order (back to front)
    // so frontmost element is tested first
    vkr_ui_hit_test_recursive(system, system->root, screen_pos, &result);

    return result;
}

vkr_internal void vkr_ui_hit_test_recursive(VkrUiSystem *system,
                                             VkrUiElementHandle handle,
                                             Vec2 screen_pos,
                                             VkrUiHitResult *result) {
    VkrUiElement *elem = vkr_ui_system_get_element(system, handle);
    if (!elem) return;

    // Skip elements that don't participate in hit testing
    if (elem->flags & VKR_UI_ELEMENT_FLAG_HIDDEN) return;
    if (!(elem->flags & VKR_UI_ELEMENT_FLAG_HIT_TEST_SELF) &&
        !(elem->flags & VKR_UI_ELEMENT_FLAG_HIT_TEST_CHILDREN)) return;

    VkrUiRect rect = elem->layout_node->screen_rect;

    // Check if point is inside element bounds
    bool8_t inside = (screen_pos.x >= rect.x &&
                      screen_pos.x < rect.x + rect.width &&
                      screen_pos.y >= rect.y &&
                      screen_pos.y < rect.y + rect.height);

    if (!inside) return;

    // Check children first (front to back, so later children are in front)
    if (elem->flags & VKR_UI_ELEMENT_FLAG_HIT_TEST_CHILDREN) {
        // Traverse children in reverse order (last child is frontmost)
        VkrUiElementHandle child = elem->first_child;
        VkrUiElementHandle last_child = VKR_UI_ELEMENT_HANDLE_INVALID;

        // Find last child
        while (vkr_ui_element_handle_is_valid(child)) {
            last_child = child;
            VkrUiElement *c = vkr_ui_system_get_element(system, child);
            child = c ? c->next_sibling : VKR_UI_ELEMENT_HANDLE_INVALID;
        }

        // Traverse backwards
        child = last_child;
        while (vkr_ui_element_handle_is_valid(child)) {
            vkr_ui_hit_test_recursive(system, child, screen_pos, result);
            if (result->hit) return; // Found hit in child

            // Find previous sibling (O(n), could optimize with prev_sibling)
            // ...
        }
    }

    // No child hit, check self
    if (elem->flags & VKR_UI_ELEMENT_FLAG_HIT_TEST_SELF) {
        // Additional checks: border radius, alpha mask, etc.
        if (vkr_ui_element_hit_test_point(elem, screen_pos)) {
            result->element = handle;
            result->local_position = vec2_new(screen_pos.x - rect.x,
                                               screen_pos.y - rect.y);
            result->hit = true_v;
        }
    }
}
```

### Input Processing

```c
/**
 * @brief Processes mouse movement.
 */
void vkr_ui_input_process_mouse_move(VkrUiInputHandler *handler, Vec2 position) {
    handler->prev_mouse_position = handler->mouse_position;
    handler->mouse_position = position;

    // Hit test at new position
    VkrUiHitResult hit = vkr_ui_hit_test(handler->system, position);

    // Handle hover state changes
    if (hit.element.id != handler->hovered_element.id) {
        // Mouse leave old element
        if (vkr_ui_element_handle_is_valid(handler->hovered_element)) {
            VkrUiEvent leave_event = {
                .type = VKR_UI_EVENT_MOUSE_LEAVE,
                .target = handler->hovered_element,
            };
            vkr_ui_dispatch_event(handler->system, &leave_event);
        }

        // Mouse enter new element
        if (vkr_ui_element_handle_is_valid(hit.element)) {
            VkrUiEvent enter_event = {
                .type = VKR_UI_EVENT_MOUSE_ENTER,
                .target = hit.element,
                .screen_position = position,
                .local_position = hit.local_position,
            };
            vkr_ui_dispatch_event(handler->system, &enter_event);
        }

        handler->hovered_element = hit.element;
    }

    // Handle drag
    if (handler->is_dragging && vkr_ui_element_handle_is_valid(handler->drag_element)) {
        VkrUiEvent drag_event = {
            .type = VKR_UI_EVENT_DRAG_MOVE,
            .target = handler->drag_element,
            .screen_position = position,
            .delta = vec2_sub(position, handler->prev_mouse_position),
        };
        vkr_ui_dispatch_event(handler->system, &drag_event);
    }
}

/**
 * @brief Processes mouse button press.
 */
void vkr_ui_input_process_button(VkrUiInputHandler *handler,
                                  Buttons button, bool8_t pressed) {
    Vec2 pos = handler->mouse_position;
    VkrUiHitResult hit = vkr_ui_hit_test(handler->system, pos);

    if (pressed) {
        // Mouse down
        handler->pressed_element = hit.element;

        if (vkr_ui_element_handle_is_valid(hit.element)) {
            VkrUiElement *elem = vkr_ui_system_get_element(handler->system, hit.element);

            VkrUiEvent event = {
                .type = VKR_UI_EVENT_MOUSE_DOWN,
                .target = hit.element,
                .screen_position = pos,
                .local_position = hit.local_position,
                .button = button,
            };
            vkr_ui_dispatch_event(handler->system, &event);

            // Start drag if draggable
            if (elem && (elem->flags & VKR_UI_ELEMENT_FLAG_DRAGGABLE)) {
                handler->drag_element = hit.element;
                handler->drag_start = pos;
                handler->is_dragging = false_v; // Not dragging until threshold
            }

            // Handle focus
            if (elem && (elem->flags & VKR_UI_ELEMENT_FLAG_FOCUSABLE)) {
                vkr_ui_input_set_focus(handler, hit.element);
            }
        }
    } else {
        // Mouse up
        VkrUiElementHandle pressed = handler->pressed_element;

        if (vkr_ui_element_handle_is_valid(pressed)) {
            VkrUiEvent event = {
                .type = VKR_UI_EVENT_MOUSE_UP,
                .target = pressed,
                .screen_position = pos,
                .button = button,
            };
            vkr_ui_dispatch_event(handler->system, &event);

            // Click if released on same element
            if (pressed.id == hit.element.id) {
                // Check for double-click
                float64_t now = /* get current time */;
                bool8_t is_double = (now - handler->last_click_time < 0.3) &&
                                    (pressed.id == handler->last_click_element.id);

                VkrUiEvent click_event = {
                    .type = is_double ? VKR_UI_EVENT_DOUBLE_CLICK : VKR_UI_EVENT_CLICK,
                    .target = pressed,
                    .screen_position = pos,
                    .button = button,
                };
                vkr_ui_dispatch_event(handler->system, &click_event);

                handler->last_click_time = now;
                handler->last_click_element = pressed;
            }
        }

        // End drag
        if (handler->is_dragging) {
            VkrUiEvent drag_end = {
                .type = VKR_UI_EVENT_DRAG_END,
                .target = handler->drag_element,
                .screen_position = pos,
            };
            vkr_ui_dispatch_event(handler->system, &drag_end);
            handler->is_dragging = false_v;
        }

        handler->pressed_element = VKR_UI_ELEMENT_HANDLE_INVALID;
        handler->drag_element = VKR_UI_ELEMENT_HANDLE_INVALID;
    }
}
```

## Event Dispatch

```c
/**
 * @brief Dispatches an event through the element tree.
 * Implements capture and bubble phases.
 */
void vkr_ui_dispatch_event(VkrUiSystem *system, VkrUiEvent *event) {
    VkrUiElement *target = vkr_ui_system_get_element(system, event->target);
    if (!target) return;

    // Build path from root to target
    VkrUiElementHandle path[64];
    uint32_t path_len = 0;

    VkrUiElementHandle current = event->target;
    while (vkr_ui_element_handle_is_valid(current) && path_len < 64) {
        path[path_len++] = current;
        VkrUiElement *elem = vkr_ui_system_get_element(system, current);
        current = elem ? elem->parent : VKR_UI_ELEMENT_HANDLE_INVALID;
    }

    // Capture phase (root to target)
    for (int32_t i = path_len - 1; i >= 0; --i) {
        if (event->stop_propagation) break;

        VkrUiElement *elem = vkr_ui_system_get_element(system, path[i]);
        if (!elem) continue;

        event->current = path[i];
        // Could have on_capture_* handlers here
    }

    // At target
    if (!event->stop_propagation) {
        event->current = event->target;
        vkr_ui_element_call_handler(target, event);
    }

    // Bubble phase (target to root)
    for (uint32_t i = 1; i < path_len; ++i) {
        if (event->stop_propagation) break;

        VkrUiElement *elem = vkr_ui_system_get_element(system, path[i]);
        if (!elem) continue;

        event->current = path[i];
        vkr_ui_element_call_handler(elem, event);
    }
}

/**
 * @brief Calls the appropriate handler for an event.
 */
vkr_internal void vkr_ui_element_call_handler(VkrUiElement *elem,
                                               VkrUiEvent *event) {
    VkrUiEventHandler handler = NULL;

    switch (event->type) {
    case VKR_UI_EVENT_MOUSE_ENTER:   handler = elem->on_mouse_enter; break;
    case VKR_UI_EVENT_MOUSE_LEAVE:   handler = elem->on_mouse_leave; break;
    case VKR_UI_EVENT_MOUSE_DOWN:    handler = elem->on_mouse_down; break;
    case VKR_UI_EVENT_MOUSE_UP:      handler = elem->on_mouse_up; break;
    case VKR_UI_EVENT_CLICK:         handler = elem->on_click; break;
    case VKR_UI_EVENT_DOUBLE_CLICK:  handler = elem->on_double_click; break;
    case VKR_UI_EVENT_KEY_DOWN:      handler = elem->on_key_down; break;
    case VKR_UI_EVENT_KEY_UP:        handler = elem->on_key_up; break;
    case VKR_UI_EVENT_FOCUS_GAINED:  handler = elem->on_focus_gained; break;
    case VKR_UI_EVENT_FOCUS_LOST:    handler = elem->on_focus_lost; break;
    case VKR_UI_EVENT_SCROLL:        handler = elem->on_scroll; break;
    // ... etc
    default: break;
    }

    if (handler) {
        handler(elem, event, elem->user_data);
    }
}
```

## API Summary

### Element Creation

```c
// Container
VkrUiElementHandle vkr_ui_container_create(VkrUiSystem *system,
                                            VkrUiElementHandle parent,
                                            const VkrUiContainerConfig *config);

// Image
VkrUiElementHandle vkr_ui_image_create(VkrUiSystem *system,
                                        VkrUiElementHandle parent,
                                        const VkrUiImageConfig *config);

// Text
VkrUiElementHandle vkr_ui_text_element_create(VkrUiSystem *system,
                                               VkrUiElementHandle parent,
                                               const VkrUiTextElementConfig *config);
```

### Element Manipulation

```c
// Get element by handle
VkrUiElement *vkr_ui_system_get_element(VkrUiSystem *system, VkrUiElementHandle handle);

// Destroy element (and children)
void vkr_ui_element_destroy(VkrUiSystem *system, VkrUiElementHandle handle);

// Reparent
void vkr_ui_element_set_parent(VkrUiSystem *system, VkrUiElementHandle element,
                                VkrUiElementHandle new_parent);

// Style
void vkr_ui_element_set_style(VkrUiElement *element, VkrUiElementState state,
                               const VkrUiVisualStyle *style);

// State
void vkr_ui_element_set_state(VkrUiElement *element, VkrUiElementState state);

// Flags
void vkr_ui_element_set_flags(VkrUiElement *element, uint32_t flags);
void vkr_ui_element_add_flag(VkrUiElement *element, VkrUiElementFlags flag);
void vkr_ui_element_remove_flag(VkrUiElement *element, VkrUiElementFlags flag);

// Event handlers
void vkr_ui_element_set_on_click(VkrUiElement *element, VkrUiEventHandler handler);
void vkr_ui_element_set_on_hover(VkrUiElement *element, VkrUiEventHandler handler);
// ... etc for all event types
```

### Text Element Specific

```c
// Update text content
void vkr_ui_text_element_set_content(VkrUiTextElement *text, String8 content);

// Update text style
void vkr_ui_text_element_set_font(VkrUiTextElement *text, VkrFontHandle font);
void vkr_ui_text_element_set_font_size(VkrUiTextElement *text, float32_t size);
void vkr_ui_text_element_set_color(VkrUiTextElement *text, Vec4 color);
```

## Implementation Phases

### Phase 1: Core Infrastructure
- Base element structure
- Element storage (array with handles)
- Parent-child relationships

### Phase 2: Basic Rendering
- UI renderer initialization
- Quad rendering (solid color)
- Texture rendering

### Phase 3: Text Integration
- VkrUiTextElement implementation
- Text measurement for layout
- Text rendering batching

### Phase 4: Hit Testing
- Point-in-rect testing
- Event dispatch system
- Hover/pressed states

### Phase 5: Advanced Features
- Scissor clipping
- Nine-slice images
- Rounded rectangles (SDF)

## Memory Considerations

### Element Storage

Elements are stored in a flat array with handle-based referencing:

```c
typedef struct VkrUiSystem {
  Array_VkrUiElement elements;
  uint32_t *free_list;
  uint32_t free_count;
  uint32_t generation_counter;
  // ...
} VkrUiSystem;
```

### Avoiding Fragmentation

- Use pools for fixed-size allocations
- Text content uses string interning for common strings
- Render data is computed per-frame (not stored)

## Performance Targets

- 1000+ elements at 60 FPS
- Single draw call per texture batch
- Hit testing in O(log n) average case (with spatial partitioning)
- Incremental layout (only dirty subtrees)
