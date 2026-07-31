---
status: partial
updated: 2026-07-31
authority: design
---
# UI System Overview

**Legacy note:** This document still uses `VkrLayerContext` in some examples
from the removed view/layer system. Render orchestration now uses the render
graph; stateless pass executors and `VkrUiSystem` own UI rendering.

## Document Purpose

This document provides a high-level architecture overview of the UI system for the Vulkan renderer. It describes how all UI components fit together and serves as a roadmap for implementation. Related design documents:

- [UI Layout Engine Design](./ui-layout-engine-design.md) - Flexbox-inspired layout system
- [UI Element Primitives Design](./ui-element-primitives-design.md) - Base element types and rendering
- [UI Components Library Design](./ui-components-library-design.md) - Reusable UI components
- [UI Docking System Design](./ui-docking-system-design.md) - Editor docking and panels

## Goals

1. **Unified UI Framework**: Single system for both game HUD and editor UI
2. **Declarative Layout**: CSS-like flexbox layout engine for automatic positioning
3. **Component Reusability**: Composable UI components (panels, buttons, inputs, lists)
4. **Editor Docking**: Professional docking system for editor panels
5. **Performance**: Efficient batched rendering with minimal draw calls
6. **Integration**: Seamless integration with existing text, font, and rendering systems

## Current State Analysis

### Existing Systems

| System | Location | Purpose | UI Relevance |
|--------|----------|---------|--------------|
| VkrUiText | `lib/src/renderer/resources/ui/vkr_ui_text.h` | UI text rendering | Text display in UI |
| pass.ui (VkrUiSystem) | `lib/src/renderer/passes/vkr_pass_ui.c` | UI pass (HUD) | Game UI rendering |
| pass.editor (Editor viewport) | `lib/src/renderer/passes/vkr_pass_editor.c` | Editor viewport | Editor layout |
| VkrText | `lib/src/core/vkr_text.h` | Text primitives | Layout/styling |
| VkrFontSystem | `lib/src/renderer/systems/vkr_font_system.h` | Font management | Text rendering |
| VkrGeometrySystem | `lib/src/renderer/systems/vkr_geometry_system.h` | Geometry rendering | UI quad rendering |
| VkrTransform | `lib/src/math/vkr_transform.h` | Transforms | UI positioning |
| InputState | `lib/src/core/input.h` | Input handling | UI interaction |
| EventManager | `lib/src/core/event.h` | Event dispatch | UI events |
| Render Graph | `lib/src/renderer/vkr_render_graph.h` | Pass orchestration | UI/Editor passes |

### Current Limitations

1. **No Layout Engine**: Editor uses hardcoded percentages for panel sizes
2. **Text Only**: UI pass only supports text, no other UI elements
3. **No Interactivity**: No built-in click/hover handling for UI elements
4. **No Components**: Each feature manually creates quads/text
5. **No Docking**: Editor panels are fixed, not rearrangeable

## Architecture Overview

```
+------------------------------------------------------------------+
|                        Application Layer                          |
|  +--------------------+  +---------------------+                  |
|  |   Game HUD (UI)    |  |   Editor (Docking)  |                  |
|  +--------------------+  +---------------------+                  |
+------------------------------------------------------------------+
|                      Component Layer                              |
|  +--------+ +--------+ +-------+ +------+ +--------+ +----------+ |
|  | Panel  | | Button | | Input | | List | | Label  | | Scrollbar| |
|  +--------+ +--------+ +-------+ +------+ +--------+ +----------+ |
+------------------------------------------------------------------+
|                      Element Layer                                |
|  +------------------+ +------------------+ +--------------------+  |
|  | VkrUiElement     | | VkrUiContainer   | | VkrUiTextElement   |  |
|  | (base type)      | | (children)       | | (text integration) |  |
|  +------------------+ +------------------+ +--------------------+  |
+------------------------------------------------------------------+
|                      Layout Engine                                |
|  +------------------+ +------------------+ +--------------------+  |
|  | VkrUiLayoutNode  | | VkrUiConstraints | | VkrUiBoxModel      |  |
|  | (flex layout)    | | (min/max/pref)   | | (margin/padding)   |  |
|  +------------------+ +------------------+ +--------------------+  |
+------------------------------------------------------------------+
|                      Core Systems                                 |
|  +-------------+ +---------------+ +-------------+ +------------+ |
|  | VkrUiStyle  | | VkrUiRenderer | | VkrUiInput  | | VkrUiEvent | |
|  | (styling)   | | (batching)    | | (hit test)  | | (signals)  | |
|  +-------------+ +---------------+ +-------------+ +------------+ |
+------------------------------------------------------------------+
|                   Existing Infrastructure                         |
|  +------------+ +-------------+ +--------------+ +--------------+ |
|  | VkrText    | | VkrFontSys  | | VkrGeometry  | | RenderGraph | |
|  +------------+ +-------------+ +--------------+ +--------------+ |
+------------------------------------------------------------------+
```

## Directory Structure

```
lib/src/
├── core/
│   └── ui/                           # NEW: Layout engine (core, no renderer deps)
│       ├── vkr_ui_layout.h           # Layout algorithm
│       ├── vkr_ui_layout.c
│       ├── vkr_ui_box_model.h        # Box model (margin/padding/border)
│       └── vkr_ui_style.h            # Style definitions
│
├── renderer/
│   ├── resources/
│   │   └── ui/
│   │       ├── vkr_ui_text.h         # EXISTING: UI text
│   │       ├── vkr_ui_element.h      # NEW: Base UI element
│   │       ├── vkr_ui_element.c
│   │       ├── vkr_ui_container.h    # NEW: Container element
│   │       └── vkr_ui_image.h        # NEW: Image element
│   │
│   └── systems/
│       ├── vkr_ui_system.h           # NEW: Main UI system
│       ├── vkr_ui_system.c
│       ├── vkr_ui_renderer.h         # NEW: Batched UI rendering
│       ├── vkr_ui_renderer.c
│       ├── vkr_ui_input.h            # NEW: UI input handling
│       ├── vkr_ui_input.c
│       │
│       ├── components/               # NEW: UI components
│       │   ├── vkr_ui_panel.h
│       │   ├── vkr_ui_button.h
│       │   ├── vkr_ui_input_field.h
│       │   ├── vkr_ui_list.h
│       │   ├── vkr_ui_scrollbar.h
│       │   └── vkr_ui_text_box.h
│       │
│       └── views/
│           ├── vkr_ui_system.c         # MODIFY: Integrate new UI system
│           └── vkr_editor_viewport.c     # MODIFY: Use docking system
│
│       └── docking/                  # NEW: Docking system
│           ├── vkr_dock_system.h
│           ├── vkr_dock_system.c
│           ├── vkr_dock_node.h
│           └── vkr_dock_tab.h
```

## Core Data Types

### UI Element Handle

```c
/**
 * @brief Opaque handle to a UI element.
 * Uses id + generation for safe referencing.
 */
typedef struct VkrUiElementHandle {
  uint32_t id;
  uint32_t generation;
} VkrUiElementHandle;

#define VKR_UI_ELEMENT_HANDLE_INVALID ((VkrUiElementHandle){0, 0})
```

### UI Rectangle

```c
/**
 * @brief Axis-aligned rectangle in screen coordinates.
 * Origin is top-left, Y increases downward.
 */
typedef struct VkrUiRect {
  float32_t x;      /**< Left edge in pixels */
  float32_t y;      /**< Top edge in pixels */
  float32_t width;  /**< Width in pixels */
  float32_t height; /**< Height in pixels */
} VkrUiRect;
```

### UI Element Type

```c
/**
 * @brief Enumeration of UI element types.
 */
typedef enum VkrUiElementType {
  VKR_UI_ELEMENT_TYPE_CONTAINER = 0, /**< Layout container */
  VKR_UI_ELEMENT_TYPE_PANEL,         /**< Visual panel/box */
  VKR_UI_ELEMENT_TYPE_TEXT,          /**< Text label */
  VKR_UI_ELEMENT_TYPE_IMAGE,         /**< Texture/image */
  VKR_UI_ELEMENT_TYPE_BUTTON,        /**< Clickable button */
  VKR_UI_ELEMENT_TYPE_INPUT,         /**< Text input field */
  VKR_UI_ELEMENT_TYPE_LIST,          /**< Scrollable list */
  VKR_UI_ELEMENT_TYPE_SCROLLBAR,     /**< Scrollbar */
  VKR_UI_ELEMENT_TYPE_DOCK_NODE,     /**< Docking node */
  VKR_UI_ELEMENT_TYPE_CUSTOM,        /**< User-defined */
  VKR_UI_ELEMENT_TYPE_COUNT,
} VkrUiElementType;
```

## System Lifecycle

### Initialization Flow

```c
// 1. Initialize UI system (called after renderer frontend init)
VkrUiSystemConfig ui_config = {
    .max_elements = 4096,
    .max_draw_commands = 1024,
    .default_font = default_font_handle,
};
vkr_ui_system_init(&rf->ui_system, rf, &ui_config, &error);

// 2. UI system initializes subsystems
//    - Layout engine
//    - UI renderer (creates pipelines, batching buffers)
//    - Input handler

// 3. Register UI layers (already exists, modified to use UI system)
vkr_ui_system_init(rf);      // Game HUD
vkr_editor_viewport_init(rf);  // Editor with docking
```

### Frame Update Flow

```c
// Per-frame update (called from application loop)
void vkr_ui_system_update(VkrUiSystem *system, float64_t delta_time) {
    // 1. Process input events
    vkr_ui_input_update(&system->input, delta_time);

    // 2. Update animations
    vkr_ui_animate_update(system, delta_time);

    // 3. Compute layout for dirty elements
    vkr_ui_layout_compute(system);

    // 4. Build render commands (batched)
    vkr_ui_renderer_build_commands(&system->renderer, system);
}
```

### Render Flow

```c
// Called from render-graph pass executors
void vkr_ui_system_render(VkrUiSystem *system, const VkrRgPassContext *ctx) {
    // Submit batched draw commands
    vkr_ui_renderer_execute(&system->renderer, ctx);
}
```

## Input Handling

### Hit Testing

```c
/**
 * @brief Result of a hit test query.
 */
typedef struct VkrUiHitResult {
  VkrUiElementHandle element; /**< Deepest element hit (or invalid) */
  Vec2 local_position;        /**< Position relative to element */
  bool8_t consumed;           /**< Whether input was consumed */
} VkrUiHitResult;

/**
 * @brief Performs hit testing at screen coordinates.
 * Traverses element tree from leaf to root.
 */
VkrUiHitResult vkr_ui_hit_test(VkrUiSystem *system, Vec2 screen_pos);
```

### Event Propagation

UI events propagate in two phases:
1. **Capture Phase**: Root to target (allows interception)
2. **Bubble Phase**: Target to root (allows handling at any level)

```c
typedef enum VkrUiEventType {
  VKR_UI_EVENT_MOUSE_ENTER,
  VKR_UI_EVENT_MOUSE_LEAVE,
  VKR_UI_EVENT_MOUSE_DOWN,
  VKR_UI_EVENT_MOUSE_UP,
  VKR_UI_EVENT_CLICK,
  VKR_UI_EVENT_DOUBLE_CLICK,
  VKR_UI_EVENT_DRAG_START,
  VKR_UI_EVENT_DRAG_MOVE,
  VKR_UI_EVENT_DRAG_END,
  VKR_UI_EVENT_KEY_DOWN,
  VKR_UI_EVENT_KEY_UP,
  VKR_UI_EVENT_TEXT_INPUT,
  VKR_UI_EVENT_FOCUS_GAINED,
  VKR_UI_EVENT_FOCUS_LOST,
  VKR_UI_EVENT_SCROLL,
  VKR_UI_EVENT_COUNT,
} VkrUiEventType;
```

## Styling System

### Style Properties

```c
/**
 * @brief UI element style.
 * CSS-inspired with renderer-specific extensions.
 */
typedef struct VkrUiStyle {
  // Background
  Vec4 background_color;
  VkrTextureHandle background_image;
  VkrUiBackgroundFit background_fit;

  // Border
  float32_t border_width;
  float32_t border_radius;
  Vec4 border_color;

  // Text
  VkrFontHandle font;
  float32_t font_size;
  Vec4 text_color;
  VkrTextAlign text_align;

  // Layout (see Layout Engine doc for details)
  VkrUiFlexDirection flex_direction;
  VkrUiJustifyContent justify_content;
  VkrUiAlignItems align_items;
  float32_t flex_grow;
  float32_t flex_shrink;

  // Box model
  VkrUiEdges padding;
  VkrUiEdges margin;

  // Size constraints
  VkrUiSize width;
  VkrUiSize height;
  VkrUiSize min_width;
  VkrUiSize min_height;
  VkrUiSize max_width;
  VkrUiSize max_height;

  // Visibility
  bool8_t visible;
  float32_t opacity;
} VkrUiStyle;
```

### Style States

Elements can have different styles for different states:

```c
typedef enum VkrUiStyleState {
  VKR_UI_STYLE_STATE_NORMAL = 0,
  VKR_UI_STYLE_STATE_HOVERED,
  VKR_UI_STYLE_STATE_PRESSED,
  VKR_UI_STYLE_STATE_FOCUSED,
  VKR_UI_STYLE_STATE_DISABLED,
  VKR_UI_STYLE_STATE_COUNT,
} VkrUiStyleState;
```

## Rendering

### Batching Strategy

To minimize draw calls, the UI renderer batches:
1. **Quads with same texture** into single draw call
2. **Text with same font atlas** into single draw call
3. **Scissor groups** for clipping

```c
typedef struct VkrUiDrawCommand {
  VkrUiDrawType type;           /**< QUAD, TEXT, NINE_SLICE */
  VkrTextureHandle texture;     /**< Texture (or font atlas) */
  VkrUiRect scissor;            /**< Clip rectangle */
  uint32_t vertex_offset;       /**< Offset into vertex buffer */
  uint32_t vertex_count;        /**< Number of vertices */
  uint32_t index_offset;        /**< Offset into index buffer */
  uint32_t index_count;         /**< Number of indices */
} VkrUiDrawCommand;
```

### Z-Ordering

Elements render in tree traversal order (depth-first). Explicit z-index supported for overlays:

```c
typedef enum VkrUiZLayer {
  VKR_UI_Z_LAYER_BACKGROUND = 0,
  VKR_UI_Z_LAYER_CONTENT,
  VKR_UI_Z_LAYER_OVERLAY,
  VKR_UI_Z_LAYER_POPUP,
  VKR_UI_Z_LAYER_TOOLTIP,
  VKR_UI_Z_LAYER_MODAL,
  VKR_UI_Z_LAYER_COUNT,
} VkrUiZLayer;
```

## Integration Points

### With Render-Graph UI Pass

```c
// vkr_pass_ui.c executor
static void vkr_pass_ui_execute(VkrRgPassContext *ctx, void *user_data) {
    (void)user_data;
    vkr_ui_system_render_text(ctx->renderer, &ctx->renderer->ui_system);
}
```

### With Render-Graph Editor Pass

```c
// vkr_pass_editor.c executor
static void vkr_pass_editor_execute(VkrRgPassContext *ctx, void *user_data) {
    (void)user_data;
    const VkrEditorPassPayload *payload = vkr_rg_pass_get_editor_payload(ctx);
    if (!payload) {
        return;
    }
    // Editor pass draws the viewport quad and any editor UI using packet data.
    vkr_pass_editor_draw_list(ctx->renderer, payload);
}
```

### With Picking System

The UI system integrates with the existing picking system for selection:

```c
// UI elements can have picking IDs
uint32_t picking_id = vkr_picking_encode_id(VKR_PICKING_ID_KIND_UI_ELEMENT, element_id);

// In picking pass, render UI elements with their picking IDs
vkr_ui_system_render_picking(&rf->ui_system, picking_pipeline);
```

## Memory Management

### Allocation Strategy

- **Long-lived elements**: Allocate from UI system arena
- **Per-frame data**: Use scratch allocator (scoped)
- **Text content**: Copy to element's allocator

```c
typedef struct VkrUiSystem {
  Arena *arena;                    /**< Owns element storage */
  VkrAllocator allocator;          /**< Wraps arena */
  VkrAllocator *scratch_allocator; /**< For temporary computations */
  // ...
} VkrUiSystem;
```

### Element Lifetime

Elements follow acquire/release pattern:

```c
// Create element (allocates from arena)
VkrUiElementHandle button = vkr_ui_button_create(system, parent, config);

// Element persists until explicitly destroyed
vkr_ui_element_destroy(system, button);
```

## Usage Examples

### Game HUD

```c
// Create HUD root
VkrUiElementHandle hud_root = vkr_ui_container_create(system, VKR_UI_ELEMENT_HANDLE_INVALID, &(VkrUiContainerConfig){
    .style = {
        .width = VKR_UI_SIZE_PERCENT(100),
        .height = VKR_UI_SIZE_PERCENT(100),
    },
});

// Health bar at bottom-left
VkrUiElementHandle health_bar = vkr_ui_panel_create(system, hud_root, &(VkrUiPanelConfig){
    .style = {
        .position = VKR_UI_POSITION_ABSOLUTE,
        .left = VKR_UI_SIZE_PX(20),
        .bottom = VKR_UI_SIZE_PX(20),
        .width = VKR_UI_SIZE_PX(200),
        .height = VKR_UI_SIZE_PX(20),
        .background_color = {0.2f, 0.2f, 0.2f, 0.8f},
        .border_radius = 4.0f,
    },
});

// Score at top-right
VkrUiElementHandle score_label = vkr_ui_text_create(system, hud_root, &(VkrUiTextConfig){
    .content = string8_lit("Score: 0"),
    .style = {
        .position = VKR_UI_POSITION_ABSOLUTE,
        .right = VKR_UI_SIZE_PX(20),
        .top = VKR_UI_SIZE_PX(20),
        .font_size = 24.0f,
        .text_color = {1.0f, 1.0f, 1.0f, 1.0f},
    },
});
```

### Editor Panel

```c
// Create dockable property panel
VkrDockNodeHandle properties = vkr_dock_node_create(dock_system, &(VkrDockNodeConfig){
    .title = string8_lit("Properties"),
    .min_size = {200, 100},
    .initial_size = {300, 400},
});

// Add content to panel
VkrUiElementHandle panel_content = vkr_dock_node_get_content(dock_system, properties);

// Property list
VkrUiElementHandle prop_list = vkr_ui_list_create(system, panel_content, &(VkrUiListConfig){
    .style = {
        .width = VKR_UI_SIZE_PERCENT(100),
        .height = VKR_UI_SIZE_PERCENT(100),
        .flex_direction = VKR_UI_FLEX_COLUMN,
    },
});

// Add property rows
for (uint32_t i = 0; i < property_count; ++i) {
    vkr_ui_property_row_create(system, prop_list, &properties[i]);
}
```

## Implementation Phases

### Phase 1: Core Foundation (Required First)
1. Layout engine (`lib/src/core/ui/`)
2. Base element types (`VkrUiElement`, `VkrUiContainer`)
3. UI renderer with batching
4. Basic hit testing

### Phase 2: Basic Components
1. Panel component
2. Text label component (integrate VkrUiText)
3. Button component
4. Scrollbar component

### Phase 3: Advanced Components
1. Input field component
2. List component
3. Text box component (multi-line)

### Phase 4: Docking System
1. Dock nodes and splits
2. Tab containers
3. Drag-and-drop rearrangement
4. Layout serialization

### Phase 5: Integration
1. UI pass uses `VkrUiSystem` for text rendering
2. Editor pass uses editor viewport resources + packet payloads
3. Picking system integration

### Phase 6: Polish
1. Animation system
2. Theme support
3. Accessibility features

## Dependencies Between Documents

```
ui-system-overview.md (this document)
        │
        ├──────────────────────┬─────────────────────┐
        │                      │                     │
        v                      v                     v
ui-layout-engine-design.md   ui-element-primitives-design.md
        │                      │
        └──────────┬───────────┘
                   │
                   v
        ui-components-library-design.md
                   │
                   v
        ui-docking-system-design.md
```

## Success Criteria

1. **Performance**: 60 FPS with 1000+ visible UI elements
2. **Memory**: No leaks on element create/destroy cycles
3. **Layout**: Accurate flexbox layout matching CSS specification
4. **Interactivity**: Correct hit testing and event propagation
5. **Editor**: Functional docking with drag-and-drop
6. **Integration**: Minimal changes to existing systems
