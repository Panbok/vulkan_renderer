---
status: proposed
updated: 2026-08-12
authority: design
---
# UI Docking System Design

**Scope note:** [ui-architecture-spec.md](./ui-architecture-spec.md) is the
authoritative UI design; this document sits under it and covers phase P6. Per
§9 of the spec, docking is in-window only, the dock tree owns the scene viewport
panel rect via `vkr_editor_viewport_mapping_from_panel_rect()`, and dock splits
are grid track lists rather than a separate splitter model. Rationale in
[ADR-027](../architecture/adr/027-immediate-mode-grid-ui.md). No code exists.

**Legacy note:** This document uses historical `VkrViewEditorState` naming from
the removed view/layer system. In the stateless renderer, editor UI is driven by
pass executors and packet payloads.

This document defines the docking system for editor-style panel arrangement, enabling drag-and-drop window management, split layouts, and tabbed containers.

## Related Documents

- [UI System Overview](../archive/ui-system-overview.md) - High-level architecture
- [UI Layout Engine Design](../archive/ui-layout-engine-design.md) - Layout computation
- [UI Element Primitives Design](../archive/ui-element-primitives-design.md) - Base elements
- [UI Components Library Design](./ui-components-library-design.md) - Panel components

---

## 1. Overview

The docking system provides:
- **Split containers**: Horizontal/vertical splits with adjustable dividers
- **Tab containers**: Multiple panels in same space with tab bar
- **Floating windows**: Detached panels that float over the main layout
- **Drag-and-drop**: Rearrange panels by dragging tabs/title bars
- **Layout persistence**: Save/restore dock layouts

### Integration with Editor UI

The docking system operates within `editor_ui.c`, managing the arrangement of editor panels (hierarchy, inspector, assets, viewport, console). Each docked panel contains UI elements from the components library.

---

## 2. Core Data Structures

### 2.1 Dock Node Types

```c
/**
 * Type of dock node in the layout tree.
 */
typedef enum VkrDockNodeType {
    VKR_DOCK_NODE_LEAF,       /**< Contains actual panel content */
    VKR_DOCK_NODE_SPLIT_H,    /**< Horizontal split (left/right children) */
    VKR_DOCK_NODE_SPLIT_V,    /**< Vertical split (top/bottom children) */
    VKR_DOCK_NODE_TABS,       /**< Tab container with multiple panels */
} VkrDockNodeType;

/**
 * Flags for dock node behavior.
 */
typedef enum VkrDockNodeFlags {
    VKR_DOCK_NODE_FLAG_NONE         = 0,
    VKR_DOCK_NODE_FLAG_NO_CLOSE     = 1 << 0,  /**< Cannot close this panel */
    VKR_DOCK_NODE_FLAG_NO_MOVE      = 1 << 1,  /**< Cannot drag this panel */
    VKR_DOCK_NODE_FLAG_NO_RESIZE    = 1 << 2,  /**< Cannot resize splits */
    VKR_DOCK_NODE_FLAG_NO_TAB_BAR   = 1 << 3,  /**< Hide tab bar for single tab */
    VKR_DOCK_NODE_FLAG_CENTRAL      = 1 << 4,  /**< Central node (viewport) */
} VkrDockNodeFlags;
```

### 2.2 Dock Node Handle

```c
/**
 * Opaque handle to a dock node.
 */
typedef struct VkrDockNodeHandle {
    uint32_t index;       /**< Index in node pool */
    uint32_t generation;  /**< Generation for validity check */
} VkrDockNodeHandle;

#define VKR_DOCK_NODE_INVALID ((VkrDockNodeHandle){0, 0})
```

### 2.3 Panel Identification

```c
/**
 * Unique identifier for a dockable panel type.
 * Used to recreate panels from saved layouts.
 */
typedef uint32_t VkrDockPanelId;

/**
 * Panel factory function for creating panel content.
 */
typedef VkrUiElementHandle (*VkrDockPanelFactory)(
    VkrUiContext* ctx,
    VkrDockPanelId panel_id,
    void* user_data
);

/**
 * Registered panel type information.
 */
typedef struct VkrDockPanelInfo {
    VkrDockPanelId id;            /**< Unique panel type ID */
    String8 name;                 /**< Display name for tab */
    String8 icon;                 /**< Icon identifier (optional) */
    VkrDockPanelFactory factory;  /**< Creates panel content */
    void* user_data;              /**< Passed to factory */
    VkrDockNodeFlags default_flags;
} VkrDockPanelInfo;
```

### 2.4 Dock Node Structure

```c
/**
 * Node in the dock layout tree.
 */
typedef struct VkrDockNode {
    VkrDockNodeHandle handle;     /**< Self reference */
    VkrDockNodeType type;         /**< Node type */
    VkrDockNodeFlags flags;       /**< Behavior flags */

    /* Tree structure */
    VkrDockNodeHandle parent;     /**< Parent node */
    VkrDockNodeHandle children[2];/**< For splits: [0]=first, [1]=second */

    /* Layout */
    VkrUiRect bounds;             /**< Computed screen bounds */
    float32_t split_ratio;        /**< For splits: 0.0-1.0 position */
    float32_t min_size;           /**< Minimum size in pixels */

    /* Tab container data (when type == VKR_DOCK_NODE_TABS) */
    struct {
        VkrDockPanelId* panels;   /**< Array of panel IDs in tabs */
        uint32_t panel_count;     /**< Number of panels */
        uint32_t active_tab;      /**< Currently active tab index */
    } tabs;

    /* Leaf content (when type == VKR_DOCK_NODE_LEAF) */
    VkrDockPanelId panel_id;      /**< Panel type ID */
    VkrUiElementHandle content;   /**< UI element tree for panel */

    /* State */
    bool32_t is_hovered;          /**< Mouse over this node */
    bool32_t is_focused;          /**< Has keyboard focus */
} VkrDockNode;
```

### 2.5 Floating Window

```c
/**
 * Floating (detached) dock window.
 */
typedef struct VkrDockFloatingWindow {
    VkrDockNodeHandle root_node;  /**< Root of floating dock tree */
    VkrUiRect bounds;             /**< Window position and size */
    String8 title;                /**< Window title */
    bool32_t is_visible;          /**< Visibility state */
    bool32_t is_minimized;        /**< Minimized state */
    int32_t z_order;              /**< Stacking order */
} VkrDockFloatingWindow;
```

### 2.6 Dock Context

```c
/**
 * Docking system state.
 */
typedef struct VkrDockContext {
    VkrAllocator* allocator;      /**< Memory allocator */

    /* Node pool */
    VkrDockNode* nodes;           /**< Node storage */
    uint32_t node_count;          /**< Active nodes */
    uint32_t node_capacity;       /**< Pool capacity */
    uint32_t* free_indices;       /**< Free list */
    uint32_t free_count;          /**< Free list size */

    /* Layout tree */
    VkrDockNodeHandle root;       /**< Root of main dock tree */

    /* Floating windows */
    VkrDockFloatingWindow* floating_windows;
    uint32_t floating_count;
    uint32_t floating_capacity;

    /* Registered panel types */
    VkrDockPanelInfo* panel_types;
    uint32_t panel_type_count;

    /* Drag state */
    struct {
        bool32_t is_dragging;
        VkrDockNodeHandle source_node;
        uint32_t source_tab_index;    /**< Tab index if from tab container */
        Vec2 drag_offset;             /**< Offset from drag start */
        VkrDockDropZone preview_zone; /**< Current drop preview */
    } drag;

    /* Resize state */
    struct {
        bool32_t is_resizing;
        VkrDockNodeHandle split_node;
        float32_t start_ratio;
        Vec2 start_pos;
    } resize;

    /* UI styling */
    struct {
        float32_t tab_height;         /**< Tab bar height */
        float32_t splitter_size;      /**< Resize handle thickness */
        float32_t drop_preview_alpha; /**< Drop zone preview opacity */
        VkrColor tab_background;
        VkrColor tab_active;
        VkrColor tab_hover;
        VkrColor splitter_color;
        VkrColor drop_preview_color;
    } style;
} VkrDockContext;
```

---

## 3. Drop Zones

### 3.1 Drop Zone Types

```c
/**
 * Drop zone positions for docking.
 */
typedef enum VkrDockDropPosition {
    VKR_DOCK_DROP_NONE = 0,
    VKR_DOCK_DROP_CENTER,    /**< Add as tab to existing container */
    VKR_DOCK_DROP_LEFT,      /**< Split left */
    VKR_DOCK_DROP_RIGHT,     /**< Split right */
    VKR_DOCK_DROP_TOP,       /**< Split top */
    VKR_DOCK_DROP_BOTTOM,    /**< Split bottom */
    VKR_DOCK_DROP_FLOATING,  /**< Create floating window */
} VkrDockDropPosition;

/**
 * Drop zone hit test result.
 */
typedef struct VkrDockDropZone {
    VkrDockNodeHandle target_node;  /**< Node to dock into */
    VkrDockDropPosition position;   /**< Where to dock */
    VkrUiRect preview_rect;         /**< Visual preview bounds */
} VkrDockDropZone;
```

### 3.2 Drop Zone Detection

```
Drop zone hit areas for a dock node:

    +-------------------+
    |        TOP        |  <- VKR_DOCK_DROP_TOP
    +---+---+-------+---+
    |   |   |       |   |
    | L |   | CENTER|   | R  <- LEFT/CENTER/RIGHT
    |   |   |       |   |
    +---+---+-------+---+
    |      BOTTOM       |  <- VKR_DOCK_DROP_BOTTOM
    +-------------------+

Detection zones (as percentage of node bounds):
- Edge zones: 25% of width/height from edges
- Center zone: Inner 50% area
```

---

## 4. API

### 4.1 Context Lifecycle

```c
/**
 * Creates dock context.
 * @param allocator Memory allocator
 * @return Initialized dock context
 */
VkrDockContext* vkr_dock_create(VkrAllocator* allocator);

/**
 * Destroys dock context and all nodes.
 */
void vkr_dock_destroy(VkrDockContext* ctx);

/**
 * Clears all nodes and resets to empty state.
 */
void vkr_dock_clear(VkrDockContext* ctx);
```

### 4.2 Panel Registration

```c
/**
 * Registers a panel type that can be docked.
 * @param ctx Dock context
 * @param info Panel type information
 * @return Panel ID for referencing
 */
VkrDockPanelId vkr_dock_register_panel(
    VkrDockContext* ctx,
    const VkrDockPanelInfo* info
);

/**
 * Gets panel info by ID.
 * @param ctx Dock context
 * @param id Panel ID
 * @return Panel info or NULL if not found
 */
const VkrDockPanelInfo* vkr_dock_get_panel_info(
    VkrDockContext* ctx,
    VkrDockPanelId id
);
```

### 4.3 Node Creation

```c
/**
 * Creates leaf node with panel content.
 * @param ctx Dock context
 * @param panel_id Registered panel type
 * @param flags Node behavior flags
 * @return Node handle
 */
VkrDockNodeHandle vkr_dock_create_leaf(
    VkrDockContext* ctx,
    VkrDockPanelId panel_id,
    VkrDockNodeFlags flags
);

/**
 * Creates split node.
 * @param ctx Dock context
 * @param direction VKR_DOCK_NODE_SPLIT_H or VKR_DOCK_NODE_SPLIT_V
 * @param first First child node
 * @param second Second child node
 * @param split_ratio Initial split position (0.0-1.0)
 * @return Split node handle
 */
VkrDockNodeHandle vkr_dock_create_split(
    VkrDockContext* ctx,
    VkrDockNodeType direction,
    VkrDockNodeHandle first,
    VkrDockNodeHandle second,
    float32_t split_ratio
);

/**
 * Creates tab container from multiple panels.
 * @param ctx Dock context
 * @param panel_ids Array of panel IDs
 * @param count Number of panels
 * @param active_tab Initially active tab index
 * @return Tab container node handle
 */
VkrDockNodeHandle vkr_dock_create_tabs(
    VkrDockContext* ctx,
    const VkrDockPanelId* panel_ids,
    uint32_t count,
    uint32_t active_tab
);
```

### 4.4 Layout Operations

```c
/**
 * Sets the root node for main dock area.
 * @param ctx Dock context
 * @param root Root node handle
 */
void vkr_dock_set_root(VkrDockContext* ctx, VkrDockNodeHandle root);

/**
 * Docks a panel relative to a target node.
 * @param ctx Dock context
 * @param panel_id Panel to dock
 * @param target Target node to dock relative to
 * @param position Where to dock
 * @return New or modified node handle
 */
VkrDockNodeHandle vkr_dock_panel(
    VkrDockContext* ctx,
    VkrDockPanelId panel_id,
    VkrDockNodeHandle target,
    VkrDockDropPosition position
);

/**
 * Undocks a panel, removing it from layout.
 * @param ctx Dock context
 * @param panel_id Panel to undock
 * @param create_floating If true, creates floating window
 * @return Floating window handle if created
 */
VkrDockNodeHandle vkr_dock_undock_panel(
    VkrDockContext* ctx,
    VkrDockPanelId panel_id,
    bool32_t create_floating
);

/**
 * Sets split ratio for a split node.
 * @param ctx Dock context
 * @param node Split node handle
 * @param ratio New split ratio (0.0-1.0)
 */
void vkr_dock_set_split_ratio(
    VkrDockContext* ctx,
    VkrDockNodeHandle node,
    float32_t ratio
);

/**
 * Sets active tab in a tab container.
 * @param ctx Dock context
 * @param node Tab container node
 * @param tab_index Tab to activate
 */
void vkr_dock_set_active_tab(
    VkrDockContext* ctx,
    VkrDockNodeHandle node,
    uint32_t tab_index
);
```

### 4.5 Floating Windows

```c
/**
 * Creates a floating window with a panel.
 * @param ctx Dock context
 * @param panel_id Panel to show
 * @param bounds Initial window bounds
 * @return Floating window index
 */
uint32_t vkr_dock_create_floating(
    VkrDockContext* ctx,
    VkrDockPanelId panel_id,
    VkrUiRect bounds
);

/**
 * Docks a floating window into main layout.
 * @param ctx Dock context
 * @param floating_index Floating window index
 * @param target Target node
 * @param position Dock position
 */
void vkr_dock_dock_floating(
    VkrDockContext* ctx,
    uint32_t floating_index,
    VkrDockNodeHandle target,
    VkrDockDropPosition position
);

/**
 * Closes a floating window.
 * @param ctx Dock context
 * @param floating_index Window to close
 */
void vkr_dock_close_floating(VkrDockContext* ctx, uint32_t floating_index);

/**
 * Brings floating window to front.
 * @param ctx Dock context
 * @param floating_index Window to focus
 */
void vkr_dock_focus_floating(VkrDockContext* ctx, uint32_t floating_index);
```

### 4.6 Update and Rendering

```c
/**
 * Updates dock layout for current frame.
 * Handles input, drag-drop, resize operations.
 * @param ctx Dock context
 * @param ui_ctx UI context for input
 * @param bounds Available dock area bounds
 */
void vkr_dock_update(
    VkrDockContext* ctx,
    VkrUiContext* ui_ctx,
    VkrUiRect bounds
);

/**
 * Renders dock layout (splitters, tab bars, drop previews).
 * Panel content is rendered separately via UI element system.
 * @param ctx Dock context
 * @param ui_ctx UI context for rendering
 */
void vkr_dock_render(VkrDockContext* ctx, VkrUiContext* ui_ctx);

/**
 * Gets the content area bounds for a panel.
 * Use for positioning panel UI elements.
 * @param ctx Dock context
 * @param panel_id Panel to query
 * @param out_bounds Output bounds
 * @return true if panel is visible
 */
bool32_t vkr_dock_get_panel_bounds(
    VkrDockContext* ctx,
    VkrDockPanelId panel_id,
    VkrUiRect* out_bounds
);
```

---

## 5. Layout Serialization

### 5.1 Layout Format

```c
/**
 * Serialized dock node for save/load.
 */
typedef struct VkrDockNodeSerialized {
    VkrDockNodeType type;
    VkrDockNodeFlags flags;
    float32_t split_ratio;

    /* For leaf/tabs */
    VkrDockPanelId panel_ids[16];  /**< Up to 16 tabs */
    uint32_t panel_count;
    uint32_t active_tab;

    /* Tree structure (indices into serialized array) */
    int32_t child_indices[2];      /**< -1 for no child */
} VkrDockNodeSerialized;

/**
 * Complete serialized layout.
 */
typedef struct VkrDockLayoutSerialized {
    uint32_t version;              /**< Format version */
    uint32_t node_count;
    int32_t root_index;            /**< Index of root node */
    VkrDockNodeSerialized* nodes;

    /* Floating windows */
    uint32_t floating_count;
    struct {
        int32_t root_index;
        VkrUiRect bounds;
    }* floating_windows;
} VkrDockLayoutSerialized;
```

### 5.2 Serialization API

```c
/**
 * Serializes current dock layout.
 * @param ctx Dock context
 * @param allocator Allocator for output
 * @return Serialized layout (caller must free)
 */
VkrDockLayoutSerialized* vkr_dock_serialize(
    VkrDockContext* ctx,
    VkrAllocator* allocator
);

/**
 * Restores dock layout from serialized data.
 * @param ctx Dock context (will be cleared first)
 * @param layout Serialized layout
 * @param ui_ctx UI context for panel creation
 * @return true on success
 */
bool32_t vkr_dock_deserialize(
    VkrDockContext* ctx,
    const VkrDockLayoutSerialized* layout,
    VkrUiContext* ui_ctx
);

/**
 * Saves layout to file.
 * @param layout Serialized layout
 * @param path File path
 * @return true on success
 */
bool32_t vkr_dock_layout_save(
    const VkrDockLayoutSerialized* layout,
    String8 path
);

/**
 * Loads layout from file.
 * @param path File path
 * @param allocator Allocator for result
 * @return Loaded layout or NULL on failure
 */
VkrDockLayoutSerialized* vkr_dock_layout_load(
    String8 path,
    VkrAllocator* allocator
);

/**
 * Frees serialized layout memory.
 * @param layout Layout to free
 * @param allocator Allocator used for allocation
 */
void vkr_dock_layout_free(
    VkrDockLayoutSerialized* layout,
    VkrAllocator* allocator
);
```

---

## 6. Drag-and-Drop System

### 6.1 Drag State Machine

```
States:
  IDLE -> DRAG_PENDING -> DRAGGING -> DROP

Transitions:
  IDLE: Mouse down on tab or title bar
    -> DRAG_PENDING: Record start position, source node

  DRAG_PENDING: Mouse moves > threshold
    -> DRAGGING: Begin visual feedback
    -> IDLE: Mouse up without threshold

  DRAGGING: Mouse moves
    -> Update drop zone preview
    -> DROP: Mouse up over valid zone
    -> IDLE: Mouse up outside, escape pressed

  DROP: Execute dock operation
    -> IDLE
```

### 6.2 Drag Rendering

```c
/**
 * Drag visual feedback configuration.
 */
typedef struct VkrDockDragVisuals {
    VkrColor outline_color;    /**< Dragged panel outline */
    float32_t outline_width;   /**< Outline thickness */
    VkrColor preview_color;    /**< Drop zone preview fill */
    float32_t preview_alpha;   /**< Preview transparency */
    bool32_t show_ghost;       /**< Show translucent panel at cursor */
} VkrDockDragVisuals;
```

### 6.3 Drop Zone Hit Testing

```c
/**
 * Tests for drop zone at screen position.
 * @param ctx Dock context
 * @param screen_pos Mouse position in screen coords
 * @param out_zone Output drop zone info
 * @return true if valid drop zone found
 */
bool32_t vkr_dock_hit_test_drop_zone(
    VkrDockContext* ctx,
    Vec2 screen_pos,
    VkrDockDropZone* out_zone
);
```

---

## 7. Integration with Editor View

### 7.1 Editor Panel Definitions

```c
/**
 * Standard editor panel IDs.
 */
typedef enum VkrEditorPanelId {
    VKR_EDITOR_PANEL_VIEWPORT    = 1,
    VKR_EDITOR_PANEL_HIERARCHY   = 2,
    VKR_EDITOR_PANEL_INSPECTOR   = 3,
    VKR_EDITOR_PANEL_ASSETS      = 4,
    VKR_EDITOR_PANEL_CONSOLE     = 5,
    VKR_EDITOR_PANEL_SCENE       = 6,
    VKR_EDITOR_PANEL_ANIMATION   = 7,
    VKR_EDITOR_PANEL_PROFILER    = 8,
} VkrEditorPanelId;
```

### 7.2 Editor View Integration

```c
/**
 * Editor view state with docking.
 */
typedef struct VkrViewEditorState {
    /* Existing state */
    VkrPickingState picking;
    /* ... */

    /* Docking system */
    VkrDockContext* dock_ctx;

    /* Panel states (content-specific data) */
    VkrHierarchyPanelState hierarchy_state;
    VkrInspectorPanelState inspector_state;
    VkrAssetsPanelState assets_state;
    VkrConsolePanelState console_state;
} VkrViewEditorState;

/**
 * Initializes editor with default dock layout.
 */
void editor_init_dock_layout(VkrViewEditorState* state) {
    VkrDockContext* dock = state->dock_ctx;

    /* Register panels */
    vkr_dock_register_panel(dock, &(VkrDockPanelInfo){
        .id = VKR_EDITOR_PANEL_VIEWPORT,
        .name = str8_lit("Viewport"),
        .factory = create_viewport_panel,
        .default_flags = VKR_DOCK_NODE_FLAG_CENTRAL
    });
    /* ... register other panels ... */

    /* Build default layout:
       +---------------+----------+
       | Hierarchy     | Viewport | Inspector |
       |               |          |           |
       +---------------+----------+-----------+
       |  Assets / Console (tabs) |           |
       +---------------------------+-----------+
    */

    VkrDockNodeHandle hierarchy = vkr_dock_create_leaf(
        dock, VKR_EDITOR_PANEL_HIERARCHY, 0);
    VkrDockNodeHandle viewport = vkr_dock_create_leaf(
        dock, VKR_EDITOR_PANEL_VIEWPORT, VKR_DOCK_NODE_FLAG_CENTRAL);
    VkrDockNodeHandle inspector = vkr_dock_create_leaf(
        dock, VKR_EDITOR_PANEL_INSPECTOR, 0);

    VkrDockPanelId bottom_panels[] = {
        VKR_EDITOR_PANEL_ASSETS,
        VKR_EDITOR_PANEL_CONSOLE
    };
    VkrDockNodeHandle bottom_tabs = vkr_dock_create_tabs(
        dock, bottom_panels, 2, 0);

    /* Assemble layout */
    VkrDockNodeHandle center_right = vkr_dock_create_split(
        dock, VKR_DOCK_NODE_SPLIT_H,
        viewport, inspector, 0.75f
    );

    VkrDockNodeHandle left_center = vkr_dock_create_split(
        dock, VKR_DOCK_NODE_SPLIT_H,
        hierarchy, center_right, 0.18f
    );

    VkrDockNodeHandle root = vkr_dock_create_split(
        dock, VKR_DOCK_NODE_SPLIT_V,
        left_center, bottom_tabs, 0.76f
    );

    vkr_dock_set_root(dock, root);
}
```

### 7.3 Frame Update Flow

```
vkr_pass_editor_execute():
  1. vkr_dock_update(dock_ctx, ui_ctx, editor_bounds)
     - Handle input events
     - Update drag-drop state
     - Recompute layout if needed

  2. For each visible panel:
     - vkr_dock_get_panel_bounds(dock_ctx, panel_id, &bounds)
     - Update panel content with new bounds
     - Panel renders its UI elements

  3. vkr_dock_render(dock_ctx, ui_ctx)
     - Render splitters
     - Render tab bars
     - Render drag preview if active
```

---

## 8. Layout Algorithm

### 8.1 Size Computation

```
compute_dock_layout(node, available_bounds):
  node.bounds = available_bounds

  switch node.type:
    case LEAF:
      // Content fills bounds (minus tab bar if visible)
      tab_height = should_show_tab_bar(node) ? style.tab_height : 0
      node.content_bounds = {
        x: bounds.x,
        y: bounds.y + tab_height,
        w: bounds.w,
        h: bounds.h - tab_height
      }

    case SPLIT_H:
      split_pos = bounds.x + bounds.w * node.split_ratio
      first_bounds = {bounds.x, bounds.y, split_pos - bounds.x, bounds.h}
      second_bounds = {split_pos, bounds.y, bounds.x + bounds.w - split_pos, bounds.h}
      compute_dock_layout(node.children[0], first_bounds)
      compute_dock_layout(node.children[1], second_bounds)

    case SPLIT_V:
      split_pos = bounds.y + bounds.h * node.split_ratio
      first_bounds = {bounds.x, bounds.y, bounds.w, split_pos - bounds.y}
      second_bounds = {bounds.x, split_pos, bounds.w, bounds.y + bounds.h - split_pos}
      compute_dock_layout(node.children[0], first_bounds)
      compute_dock_layout(node.children[1], second_bounds)

    case TABS:
      // All tabs share same bounds, only active is visible
      for each child in node.tabs:
        child.bounds = bounds
```

### 8.2 Minimum Size Constraints

```
compute_min_size(node, direction):
  switch node.type:
    case LEAF:
      return node.min_size  // Default or panel-specific

    case SPLIT_H:
      if direction == HORIZONTAL:
        return compute_min_size(children[0], H) +
               compute_min_size(children[1], H) +
               style.splitter_size
      else:
        return max(compute_min_size(children[0], V),
                   compute_min_size(children[1], V))

    case SPLIT_V:
      // Inverse of SPLIT_H

    case TABS:
      return max(compute_min_size(child) for child in tabs)
```

---

## 9. Implementation Notes

### 9.1 Directory Structure

```
lib/src/
├── core/
│   └── ui/
│       ├── vkr_ui_dock.h      # Dock system public API
│       └── vkr_ui_dock.c      # Dock implementation
└── renderer/
    └── systems/
        └── views/
            └── editor_ui.c  # Uses docking
```

### 9.2 Memory Management

- Dock nodes allocated from pool with generation counters
- Panel content (UI elements) uses UI context allocator
- Serialized layouts use provided allocator (typically scratch)
- Tab panel ID arrays copied to dock context memory

### 9.3 Input Handling Priority

```
1. Floating window title bars (drag to move)
2. Floating window edges (resize)
3. Tab bars (click to select, drag to reorder/undock)
4. Splitter handles (drag to resize)
5. Panel content (forwarded to UI system)
```

### 9.4 Z-Order Management

- Main dock area: z = 0
- Floating windows: z = 1000 + window_index
- Drag preview: z = 2000
- Drop zone overlay: z = 2001

---

## 10. Implementation Phases

### Phase 1: Core Tree Structure
1. Implement `VkrDockNode` pool and handles
2. Create/destroy leaf, split, tab nodes
3. Basic tree traversal and layout computation
4. Static layout without interaction

### Phase 2: Rendering
1. Tab bar rendering with panel names
2. Splitter rendering
3. Panel content bounds calculation
4. Integration with UI element system

### Phase 3: Interaction
1. Tab click to select
2. Splitter drag to resize
3. Tab drag to reorder within container
4. Drop zone detection and preview

### Phase 4: Drag-and-Drop
1. Tab undock to floating
2. Drop into split zones
3. Drop into tab container
4. Floating window management

### Phase 5: Persistence
1. Layout serialization
2. Layout deserialization
3. File save/load
4. Default layout restoration

### Phase 6: Polish
1. Smooth animations for tab switches
2. Splitter resize constraints
3. Keyboard navigation (Ctrl+Tab between panels)
4. Context menus for panels

---

## 11. Usage Example

```c
/* Initialize docking in editor view */
void editor_init(VkrViewEditorState* state) {
    state->dock_ctx = vkr_dock_create(&state->allocator);

    /* Register all editor panels */
    register_editor_panels(state->dock_ctx);

    /* Try to load saved layout */
    VkrDockLayoutSerialized* saved = vkr_dock_layout_load(
        str8_lit("editor_layout.dock"),
        &scratch_allocator
    );

    if (saved) {
        vkr_dock_deserialize(state->dock_ctx, saved, state->ui_ctx);
        vkr_dock_layout_free(saved, &scratch_allocator);
    } else {
        /* Create default layout */
        editor_init_dock_layout(state);
    }
}

/* Frame update */
void editor_update(VkrViewEditorState* state, VkrUiRect bounds) {
    /* Update dock layout and handle input */
    vkr_dock_update(state->dock_ctx, state->ui_ctx, bounds);

    /* Update visible panels */
    VkrUiRect panel_bounds;

    if (vkr_dock_get_panel_bounds(
            state->dock_ctx, VKR_EDITOR_PANEL_HIERARCHY, &panel_bounds)) {
        update_hierarchy_panel(&state->hierarchy_state, panel_bounds);
    }

    if (vkr_dock_get_panel_bounds(
            state->dock_ctx, VKR_EDITOR_PANEL_INSPECTOR, &panel_bounds)) {
        update_inspector_panel(&state->inspector_state, panel_bounds);
    }

    /* ... other panels ... */
}

/* Render dock chrome and panel content */
void editor_render(VkrViewEditorState* state) {
    /* Render splitters, tab bars, drop previews */
    vkr_dock_render(state->dock_ctx, state->ui_ctx);

    /* Panel content rendered via UI element system */
}

/* Save layout on exit */
void editor_shutdown(VkrViewEditorState* state) {
    VkrDockLayoutSerialized* layout = vkr_dock_serialize(
        state->dock_ctx, &scratch_allocator);

    vkr_dock_layout_save(layout, str8_lit("editor_layout.dock"));
    vkr_dock_layout_free(layout, &scratch_allocator);

    vkr_dock_destroy(state->dock_ctx);
}
```
