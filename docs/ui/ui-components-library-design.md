---
status: proposed
updated: 2026-07-31
authority: design
---
# UI Components Library Design

## Document Purpose

This document describes the reusable UI components built on top of the element primitives. Components encapsulate common UI patterns like buttons, input fields, lists, and panels.

Related documents:
- [UI System Overview](./ui-system-overview.md) - High-level architecture
- [UI Layout Engine Design](./ui-layout-engine-design.md) - Layout computation
- [UI Element Primitives Design](./ui-element-primitives-design.md) - Base elements
- [UI Docking System Design](./ui-docking-system-design.md) - Editor docking

## Goals

1. **Reusable**: Common patterns implemented once
2. **Composable**: Components can contain other components
3. **Styleable**: Consistent theming support
4. **Accessible**: Keyboard navigation, focus management

## Location

```
lib/src/renderer/systems/components/
├── vkr_ui_panel.h          # Panel/box container
├── vkr_ui_panel.c
├── vkr_ui_button.h         # Clickable button
├── vkr_ui_button.c
├── vkr_ui_input_field.h    # Text input
├── vkr_ui_input_field.c
├── vkr_ui_list.h           # Scrollable list
├── vkr_ui_list.c
├── vkr_ui_scrollbar.h      # Scrollbar control
├── vkr_ui_scrollbar.c
├── vkr_ui_text_box.h       # Multi-line text
├── vkr_ui_text_box.c
├── vkr_ui_checkbox.h       # Toggle checkbox
├── vkr_ui_checkbox.c
├── vkr_ui_slider.h         # Value slider
├── vkr_ui_slider.c
├── vkr_ui_dropdown.h       # Dropdown menu
├── vkr_ui_dropdown.c
└── vkr_ui_tooltip.h        # Tooltip overlay
```

## Component Base

### Component Handle

Each component type has its own handle type for type safety:

```c
/**
 * @brief Panel component handle.
 */
typedef struct VkrUiPanelHandle {
  VkrUiElementHandle element;  /**< Underlying element */
} VkrUiPanelHandle;

/**
 * @brief Button component handle.
 */
typedef struct VkrUiButtonHandle {
  VkrUiElementHandle element;
} VkrUiButtonHandle;

// ... etc for each component type
```

### Component Callbacks

```c
/**
 * @brief Button click callback.
 */
typedef void (*VkrUiButtonCallback)(VkrUiButtonHandle button, void *user_data);

/**
 * @brief Input field change callback.
 */
typedef void (*VkrUiInputFieldCallback)(VkrUiInputFieldHandle input,
                                         String8 new_value, void *user_data);

/**
 * @brief List selection callback.
 */
typedef void (*VkrUiListSelectionCallback)(VkrUiListHandle list,
                                            uint32_t selected_index,
                                            void *user_data);

/**
 * @brief Slider value callback.
 */
typedef void (*VkrUiSliderCallback)(VkrUiSliderHandle slider,
                                     float32_t new_value, void *user_data);

/**
 * @brief Checkbox toggle callback.
 */
typedef void (*VkrUiCheckboxCallback)(VkrUiCheckboxHandle checkbox,
                                       bool8_t checked, void *user_data);
```

## Panel Component

Panels are visual containers with background and border styling.

```c
/**
 * @brief Panel visual style preset.
 */
typedef enum VkrUiPanelVariant {
  VKR_UI_PANEL_VARIANT_DEFAULT = 0, /**< Standard panel */
  VKR_UI_PANEL_VARIANT_RAISED,      /**< Elevated appearance */
  VKR_UI_PANEL_VARIANT_INSET,       /**< Recessed appearance */
  VKR_UI_PANEL_VARIANT_OUTLINED,    /**< Border only, no fill */
  VKR_UI_PANEL_VARIANT_TRANSPARENT, /**< No background */
} VkrUiPanelVariant;

/**
 * @brief Panel creation config.
 */
typedef struct VkrUiPanelConfig {
  VkrUiPanelVariant variant;
  Vec4 background_color;          /**< Override default color */
  float32_t border_radius;
  float32_t border_width;
  Vec4 border_color;
  VkrUiLayoutStyle layout;
  VkrUiEdges padding;
  bool8_t scrollable;
} VkrUiPanelConfig;

#define VKR_UI_PANEL_CONFIG_DEFAULT ((VkrUiPanelConfig){ \
  .variant = VKR_UI_PANEL_VARIANT_DEFAULT,               \
  .background_color = {0.15f, 0.15f, 0.15f, 1.0f},      \
  .border_radius = 4.0f,                                 \
  .border_width = 0.0f,                                  \
  .padding = VKR_UI_EDGES_ALL(8.0f),                    \
  .scrollable = false_v,                                 \
})
```

### Panel API

```c
/**
 * @brief Creates a panel component.
 */
VkrUiPanelHandle vkr_ui_panel_create(VkrUiSystem *system,
                                      VkrUiElementHandle parent,
                                      const VkrUiPanelConfig *config);

/**
 * @brief Gets the content container for adding children.
 */
VkrUiElementHandle vkr_ui_panel_get_content(VkrUiSystem *system,
                                             VkrUiPanelHandle panel);

/**
 * @brief Sets panel scroll position.
 */
void vkr_ui_panel_set_scroll(VkrUiSystem *system, VkrUiPanelHandle panel,
                              Vec2 offset);

/**
 * @brief Gets current scroll position.
 */
Vec2 vkr_ui_panel_get_scroll(VkrUiSystem *system, VkrUiPanelHandle panel);

/**
 * @brief Scrolls to make a child visible.
 */
void vkr_ui_panel_scroll_to_child(VkrUiSystem *system, VkrUiPanelHandle panel,
                                   VkrUiElementHandle child);
```

## Button Component

Interactive clickable buttons with multiple states.

```c
/**
 * @brief Button visual variant.
 */
typedef enum VkrUiButtonVariant {
  VKR_UI_BUTTON_VARIANT_DEFAULT = 0, /**< Standard button */
  VKR_UI_BUTTON_VARIANT_PRIMARY,     /**< Primary action */
  VKR_UI_BUTTON_VARIANT_SECONDARY,   /**< Secondary action */
  VKR_UI_BUTTON_VARIANT_DANGER,      /**< Destructive action */
  VKR_UI_BUTTON_VARIANT_GHOST,       /**< Text-only button */
  VKR_UI_BUTTON_VARIANT_ICON,        /**< Icon button (no text) */
} VkrUiButtonVariant;

/**
 * @brief Button creation config.
 */
typedef struct VkrUiButtonConfig {
  String8 label;                  /**< Button text */
  VkrTextureHandle icon;          /**< Optional icon */
  VkrUiButtonVariant variant;
  VkrUiButtonCallback on_click;
  void *user_data;
  bool8_t disabled;
  VkrUiLayoutStyle layout;
} VkrUiButtonConfig;

/**
 * @brief Internal button state.
 */
typedef struct VkrUiButtonState {
  VkrUiElementHandle root;        /**< Root container */
  VkrUiElementHandle icon_element;
  VkrUiElementHandle label_element;
  VkrUiButtonCallback on_click;
  void *user_data;
  bool8_t disabled;
  bool8_t hovered;
  bool8_t pressed;
} VkrUiButtonState;
```

### Button API

```c
/**
 * @brief Creates a button component.
 */
VkrUiButtonHandle vkr_ui_button_create(VkrUiSystem *system,
                                        VkrUiElementHandle parent,
                                        const VkrUiButtonConfig *config);

/**
 * @brief Sets button label.
 */
void vkr_ui_button_set_label(VkrUiSystem *system, VkrUiButtonHandle button,
                              String8 label);

/**
 * @brief Sets button enabled state.
 */
void vkr_ui_button_set_disabled(VkrUiSystem *system, VkrUiButtonHandle button,
                                 bool8_t disabled);

/**
 * @brief Sets click callback.
 */
void vkr_ui_button_set_on_click(VkrUiSystem *system, VkrUiButtonHandle button,
                                 VkrUiButtonCallback callback, void *user_data);
```

### Button Implementation

```c
VkrUiButtonHandle vkr_ui_button_create(VkrUiSystem *system,
                                        VkrUiElementHandle parent,
                                        const VkrUiButtonConfig *config) {
    // Create root container
    VkrUiContainerConfig root_cfg = {
        .layout = {
            .flex_direction = VKR_UI_FLEX_ROW,
            .align_items = VKR_UI_ALIGN_CENTER,
            .justify_content = VKR_UI_JUSTIFY_CENTER,
            .padding = VKR_UI_EDGES_SYMMETRIC(8, 16),
            .gap = 8.0f,
        },
        .flags = VKR_UI_ELEMENT_FLAG_FOCUSABLE | VKR_UI_ELEMENT_FLAGS_DEFAULT,
    };

    // Apply variant styling
    VkrUiVisualStyle normal_style = vkr_ui_button_get_variant_style(config->variant, VKR_UI_ELEMENT_STATE_NORMAL);
    VkrUiVisualStyle hover_style = vkr_ui_button_get_variant_style(config->variant, VKR_UI_ELEMENT_STATE_HOVERED);
    VkrUiVisualStyle pressed_style = vkr_ui_button_get_variant_style(config->variant, VKR_UI_ELEMENT_STATE_PRESSED);
    VkrUiVisualStyle disabled_style = vkr_ui_button_get_variant_style(config->variant, VKR_UI_ELEMENT_STATE_DISABLED);

    VkrUiElementHandle root = vkr_ui_container_create(system, parent, &root_cfg);

    // Set state-specific styles
    VkrUiElement *elem = vkr_ui_system_get_element(system, root);
    if (elem) {
        elem->styles[VKR_UI_ELEMENT_STATE_NORMAL] = normal_style;
        elem->styles[VKR_UI_ELEMENT_STATE_HOVERED] = hover_style;
        elem->styles[VKR_UI_ELEMENT_STATE_PRESSED] = pressed_style;
        elem->styles[VKR_UI_ELEMENT_STATE_DISABLED] = disabled_style;
    }

    // Create icon if provided
    VkrUiElementHandle icon_elem = VKR_UI_ELEMENT_HANDLE_INVALID;
    if (vkr_texture_handle_is_valid(config->icon)) {
        VkrUiImageConfig icon_cfg = {
            .texture = config->icon,
            .layout = {
                .width = VKR_UI_SIZE_PX(16),
                .height = VKR_UI_SIZE_PX(16),
            },
        };
        icon_elem = vkr_ui_image_create(system, root, &icon_cfg);
    }

    // Create label
    VkrUiElementHandle label_elem = VKR_UI_ELEMENT_HANDLE_INVALID;
    if (config->label.length > 0) {
        VkrUiTextElementConfig text_cfg = {
            .content = config->label,
            .color = normal_style.text_color,
        };
        label_elem = vkr_ui_text_element_create(system, root, &text_cfg);
    }

    // Store component state
    VkrUiButtonState *state = vkr_allocator_alloc(system->allocator,
                                                   sizeof(VkrUiButtonState),
                                                   VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    state->root = root;
    state->icon_element = icon_elem;
    state->label_element = label_elem;
    state->on_click = config->on_click;
    state->user_data = config->user_data;
    state->disabled = config->disabled;

    // Set up event handlers
    if (elem) {
        elem->user_data = state;
        elem->on_click = vkr_ui_button_handle_click;
        elem->on_mouse_enter = vkr_ui_button_handle_hover;
        elem->on_mouse_leave = vkr_ui_button_handle_leave;
    }

    return (VkrUiButtonHandle){.element = root};
}

vkr_internal bool8_t vkr_ui_button_handle_click(VkrUiElement *element,
                                                 VkrUiEvent *event,
                                                 void *user_data) {
    VkrUiButtonState *state = (VkrUiButtonState *)user_data;
    if (state->disabled) return false_v;

    if (state->on_click) {
        state->on_click((VkrUiButtonHandle){.element = element->handle},
                        state->user_data);
    }
    return true_v;
}
```

## Input Field Component

Single-line text input with cursor, selection, and editing.

```c
/**
 * @brief Input field type.
 */
typedef enum VkrUiInputFieldType {
  VKR_UI_INPUT_FIELD_TEXT = 0,    /**< Plain text */
  VKR_UI_INPUT_FIELD_PASSWORD,    /**< Masked characters */
  VKR_UI_INPUT_FIELD_NUMBER,      /**< Numeric only */
  VKR_UI_INPUT_FIELD_EMAIL,       /**< Email format */
} VkrUiInputFieldType;

/**
 * @brief Input field config.
 */
typedef struct VkrUiInputFieldConfig {
  String8 initial_value;
  String8 placeholder;            /**< Shown when empty */
  VkrUiInputFieldType type;
  uint32_t max_length;            /**< 0 = unlimited */
  VkrUiInputFieldCallback on_change;
  VkrUiInputFieldCallback on_submit; /**< Enter pressed */
  void *user_data;
  bool8_t disabled;
  VkrUiLayoutStyle layout;
} VkrUiInputFieldConfig;

/**
 * @brief Input field internal state.
 */
typedef struct VkrUiInputFieldState {
  VkrUiElementHandle root;
  VkrUiElementHandle text_element;
  VkrUiElementHandle cursor_element;
  VkrUiElementHandle selection_element;

  // Text state
  String8 value;                  /**< Current value (owned) */
  uint32_t cursor_pos;            /**< Cursor position in codepoints */
  uint32_t selection_start;       /**< Selection start (-1 = no selection) */
  uint32_t selection_end;

  // Visual state
  float32_t cursor_blink_time;
  bool8_t cursor_visible;

  // Config
  VkrUiInputFieldType type;
  uint32_t max_length;
  VkrUiInputFieldCallback on_change;
  VkrUiInputFieldCallback on_submit;
  void *user_data;
  bool8_t disabled;
} VkrUiInputFieldState;
```

### Input Field API

```c
/**
 * @brief Creates an input field.
 */
VkrUiInputFieldHandle vkr_ui_input_field_create(VkrUiSystem *system,
                                                 VkrUiElementHandle parent,
                                                 const VkrUiInputFieldConfig *config);

/**
 * @brief Gets current value.
 */
String8 vkr_ui_input_field_get_value(VkrUiSystem *system,
                                      VkrUiInputFieldHandle input);

/**
 * @brief Sets value programmatically.
 */
void vkr_ui_input_field_set_value(VkrUiSystem *system,
                                   VkrUiInputFieldHandle input,
                                   String8 value);

/**
 * @brief Sets focus to input field.
 */
void vkr_ui_input_field_focus(VkrUiSystem *system, VkrUiInputFieldHandle input);

/**
 * @brief Selects all text.
 */
void vkr_ui_input_field_select_all(VkrUiSystem *system,
                                    VkrUiInputFieldHandle input);
```

### Text Editing Implementation

```c
vkr_internal bool8_t vkr_ui_input_field_handle_key(VkrUiElement *element,
                                                    VkrUiEvent *event,
                                                    void *user_data) {
    VkrUiInputFieldState *state = (VkrUiInputFieldState *)user_data;
    if (state->disabled) return false_v;

    switch (event->key) {
    case KEY_BACKSPACE:
        if (state->selection_start != state->selection_end) {
            vkr_ui_input_field_delete_selection(state);
        } else if (state->cursor_pos > 0) {
            vkr_ui_input_field_delete_char(state, state->cursor_pos - 1);
            state->cursor_pos--;
        }
        break;

    case KEY_DELETE:
        if (state->selection_start != state->selection_end) {
            vkr_ui_input_field_delete_selection(state);
        } else if (state->cursor_pos < vkr_string8_codepoint_count(&state->value)) {
            vkr_ui_input_field_delete_char(state, state->cursor_pos);
        }
        break;

    case KEY_LEFT:
        if (state->cursor_pos > 0) {
            state->cursor_pos--;
            if (!event->shift) {
                state->selection_start = state->selection_end = state->cursor_pos;
            } else {
                state->selection_end = state->cursor_pos;
            }
        }
        break;

    case KEY_RIGHT:
        if (state->cursor_pos < vkr_string8_codepoint_count(&state->value)) {
            state->cursor_pos++;
            if (!event->shift) {
                state->selection_start = state->selection_end = state->cursor_pos;
            } else {
                state->selection_end = state->cursor_pos;
            }
        }
        break;

    case KEY_HOME:
        state->cursor_pos = 0;
        if (!event->shift) {
            state->selection_start = state->selection_end = 0;
        }
        break;

    case KEY_END:
        state->cursor_pos = vkr_string8_codepoint_count(&state->value);
        if (!event->shift) {
            state->selection_start = state->selection_end = state->cursor_pos;
        }
        break;

    case KEY_ENTER:
        if (state->on_submit) {
            VkrUiInputFieldHandle handle = {.element = element->handle};
            state->on_submit(handle, state->value, state->user_data);
        }
        break;

    default:
        // Handle Ctrl+A, Ctrl+C, Ctrl+V, Ctrl+X
        if (event->ctrl) {
            switch (event->key) {
            case KEY_A:
                state->selection_start = 0;
                state->selection_end = vkr_string8_codepoint_count(&state->value);
                state->cursor_pos = state->selection_end;
                break;
            // ... copy, paste, cut
            }
        }
        break;
    }

    vkr_ui_input_field_update_display(state);
    return true_v;
}
```

## List Component

Scrollable list with selection support.

```c
/**
 * @brief List selection mode.
 */
typedef enum VkrUiListSelectionMode {
  VKR_UI_LIST_SELECTION_NONE = 0,  /**< No selection */
  VKR_UI_LIST_SELECTION_SINGLE,    /**< Single item */
  VKR_UI_LIST_SELECTION_MULTIPLE,  /**< Multiple items */
} VkrUiListSelectionMode;

/**
 * @brief List item data.
 */
typedef struct VkrUiListItemData {
  String8 label;
  VkrTextureHandle icon;          /**< Optional icon */
  void *user_data;
  bool8_t disabled;
} VkrUiListItemData;

/**
 * @brief List config.
 */
typedef struct VkrUiListConfig {
  VkrUiListSelectionMode selection_mode;
  float32_t item_height;          /**< 0 = auto */
  bool8_t show_scrollbar;
  VkrUiListSelectionCallback on_selection_change;
  void *user_data;
  VkrUiLayoutStyle layout;
} VkrUiListConfig;

/**
 * @brief List internal state.
 */
typedef struct VkrUiListState {
  VkrUiElementHandle root;
  VkrUiElementHandle container;
  VkrUiElementHandle scrollbar;

  // Items
  Array_VkrUiListItem items;
  uint32_t item_count;

  // Selection
  VkrUiListSelectionMode selection_mode;
  Array_bool8_t selected;         /**< Selection state per item */
  int32_t focused_index;          /**< Keyboard focus (-1 = none) */

  // Scroll
  float32_t scroll_offset;
  float32_t total_height;
  float32_t visible_height;

  // Callbacks
  VkrUiListSelectionCallback on_selection_change;
  void *user_data;
} VkrUiListState;
```

### List API

```c
/**
 * @brief Creates a list component.
 */
VkrUiListHandle vkr_ui_list_create(VkrUiSystem *system,
                                    VkrUiElementHandle parent,
                                    const VkrUiListConfig *config);

/**
 * @brief Adds an item to the list.
 */
uint32_t vkr_ui_list_add_item(VkrUiSystem *system, VkrUiListHandle list,
                               const VkrUiListItemData *item);

/**
 * @brief Removes an item by index.
 */
void vkr_ui_list_remove_item(VkrUiSystem *system, VkrUiListHandle list,
                              uint32_t index);

/**
 * @brief Clears all items.
 */
void vkr_ui_list_clear(VkrUiSystem *system, VkrUiListHandle list);

/**
 * @brief Gets selected indices.
 */
uint32_t vkr_ui_list_get_selection(VkrUiSystem *system, VkrUiListHandle list,
                                    uint32_t *out_indices, uint32_t max_count);

/**
 * @brief Sets selection programmatically.
 */
void vkr_ui_list_set_selection(VkrUiSystem *system, VkrUiListHandle list,
                                uint32_t *indices, uint32_t count);

/**
 * @brief Scrolls to make item visible.
 */
void vkr_ui_list_scroll_to_item(VkrUiSystem *system, VkrUiListHandle list,
                                 uint32_t index);
```

### Virtual Scrolling

For large lists, only visible items are rendered:

```c
vkr_internal void vkr_ui_list_update_visible_items(VkrUiListState *state) {
    float32_t item_height = state->item_height;
    if (item_height <= 0.0f) item_height = 24.0f;

    // Calculate visible range
    uint32_t first_visible = (uint32_t)(state->scroll_offset / item_height);
    uint32_t visible_count = (uint32_t)(state->visible_height / item_height) + 2;
    uint32_t last_visible = first_visible + visible_count;
    if (last_visible > state->item_count) {
        last_visible = state->item_count;
    }

    // Update item visibility
    for (uint32_t i = 0; i < state->item_count; ++i) {
        VkrUiListItem *item = &state->items.data[i];
        bool8_t should_show = (i >= first_visible && i < last_visible);

        if (should_show && !item->visible) {
            // Create item element
            vkr_ui_list_create_item_element(state, i);
            item->visible = true_v;
        } else if (!should_show && item->visible) {
            // Destroy item element
            vkr_ui_element_destroy(state->system, item->element);
            item->element = VKR_UI_ELEMENT_HANDLE_INVALID;
            item->visible = false_v;
        }

        // Update position
        if (item->visible) {
            float32_t y = i * item_height - state->scroll_offset;
            vkr_ui_layout_set_top(item->layout_node, VKR_UI_SIZE_PX(y));
        }
    }
}
```

## Scrollbar Component

```c
/**
 * @brief Scrollbar orientation.
 */
typedef enum VkrUiScrollbarOrientation {
  VKR_UI_SCROLLBAR_VERTICAL = 0,
  VKR_UI_SCROLLBAR_HORIZONTAL,
} VkrUiScrollbarOrientation;

/**
 * @brief Scrollbar config.
 */
typedef struct VkrUiScrollbarConfig {
  VkrUiScrollbarOrientation orientation;
  float32_t min_thumb_size;       /**< Minimum thumb size in pixels */
  VkrUiSliderCallback on_scroll;
  void *user_data;
} VkrUiScrollbarConfig;

/**
 * @brief Scrollbar state.
 */
typedef struct VkrUiScrollbarState {
  VkrUiElementHandle root;
  VkrUiElementHandle track;
  VkrUiElementHandle thumb;

  VkrUiScrollbarOrientation orientation;
  float32_t content_size;         /**< Total content size */
  float32_t viewport_size;        /**< Visible viewport size */
  float32_t scroll_position;      /**< Current scroll (0 to content_size - viewport_size) */

  // Drag state
  bool8_t dragging;
  float32_t drag_start_pos;
  float32_t drag_start_scroll;

  VkrUiSliderCallback on_scroll;
  void *user_data;
} VkrUiScrollbarState;
```

### Scrollbar API

```c
VkrUiScrollbarHandle vkr_ui_scrollbar_create(VkrUiSystem *system,
                                              VkrUiElementHandle parent,
                                              const VkrUiScrollbarConfig *config);

void vkr_ui_scrollbar_set_sizes(VkrUiSystem *system, VkrUiScrollbarHandle scrollbar,
                                 float32_t content_size, float32_t viewport_size);

void vkr_ui_scrollbar_set_position(VkrUiSystem *system,
                                    VkrUiScrollbarHandle scrollbar,
                                    float32_t position);

float32_t vkr_ui_scrollbar_get_position(VkrUiSystem *system,
                                         VkrUiScrollbarHandle scrollbar);
```

## Text Box Component

Multi-line text display with optional editing.

```c
/**
 * @brief Text box config.
 */
typedef struct VkrUiTextBoxConfig {
  String8 initial_text;
  bool8_t editable;
  bool8_t word_wrap;
  float32_t line_height;
  VkrFontHandle font;
  float32_t font_size;
  Vec4 text_color;
  VkrUiInputFieldCallback on_change;
  void *user_data;
  VkrUiLayoutStyle layout;
} VkrUiTextBoxConfig;

/**
 * @brief Text box state.
 */
typedef struct VkrUiTextBoxState {
  VkrUiElementHandle root;
  VkrUiElementHandle content;
  VkrUiElementHandle scrollbar_v;
  VkrUiElementHandle scrollbar_h;

  // Text content (array of lines)
  Vector_String8 lines;
  uint32_t total_lines;

  // Cursor (for editable)
  uint32_t cursor_line;
  uint32_t cursor_column;
  uint32_t selection_start_line;
  uint32_t selection_start_column;
  uint32_t selection_end_line;
  uint32_t selection_end_column;

  // Scroll
  float32_t scroll_x;
  float32_t scroll_y;

  // Config
  bool8_t editable;
  bool8_t word_wrap;
  VkrUiInputFieldCallback on_change;
  void *user_data;
} VkrUiTextBoxState;
```

## Checkbox Component

```c
typedef struct VkrUiCheckboxConfig {
  String8 label;
  bool8_t initial_checked;
  bool8_t disabled;
  VkrUiCheckboxCallback on_change;
  void *user_data;
} VkrUiCheckboxConfig;

typedef struct VkrUiCheckboxState {
  VkrUiElementHandle root;
  VkrUiElementHandle box;
  VkrUiElementHandle checkmark;
  VkrUiElementHandle label;

  bool8_t checked;
  bool8_t disabled;
  VkrUiCheckboxCallback on_change;
  void *user_data;
} VkrUiCheckboxState;
```

### Checkbox API

```c
VkrUiCheckboxHandle vkr_ui_checkbox_create(VkrUiSystem *system,
                                            VkrUiElementHandle parent,
                                            const VkrUiCheckboxConfig *config);

bool8_t vkr_ui_checkbox_is_checked(VkrUiSystem *system,
                                    VkrUiCheckboxHandle checkbox);

void vkr_ui_checkbox_set_checked(VkrUiSystem *system,
                                  VkrUiCheckboxHandle checkbox,
                                  bool8_t checked);
```

## Slider Component

```c
typedef struct VkrUiSliderConfig {
  float32_t min_value;
  float32_t max_value;
  float32_t initial_value;
  float32_t step;                 /**< 0 = continuous */
  bool8_t show_value;
  VkrUiSliderCallback on_change;
  void *user_data;
  VkrUiLayoutStyle layout;
} VkrUiSliderConfig;

typedef struct VkrUiSliderState {
  VkrUiElementHandle root;
  VkrUiElementHandle track;
  VkrUiElementHandle fill;
  VkrUiElementHandle thumb;
  VkrUiElementHandle value_label;

  float32_t min_value;
  float32_t max_value;
  float32_t current_value;
  float32_t step;

  bool8_t dragging;
  VkrUiSliderCallback on_change;
  void *user_data;
} VkrUiSliderState;
```

## Dropdown Component

```c
typedef struct VkrUiDropdownConfig {
  String8 *options;
  uint32_t option_count;
  int32_t initial_selection;      /**< -1 = none */
  String8 placeholder;
  VkrUiListSelectionCallback on_change;
  void *user_data;
  VkrUiLayoutStyle layout;
} VkrUiDropdownConfig;

typedef struct VkrUiDropdownState {
  VkrUiElementHandle root;
  VkrUiElementHandle button;
  VkrUiElementHandle popup;       /**< Popup list (created on open) */
  VkrUiElementHandle selected_label;
  VkrUiElementHandle arrow;

  String8 *options;               /**< Owned copy */
  uint32_t option_count;
  int32_t selected_index;
  bool8_t is_open;

  VkrUiListSelectionCallback on_change;
  void *user_data;
} VkrUiDropdownState;
```

### Dropdown API

```c
VkrUiDropdownHandle vkr_ui_dropdown_create(VkrUiSystem *system,
                                            VkrUiElementHandle parent,
                                            const VkrUiDropdownConfig *config);

int32_t vkr_ui_dropdown_get_selected(VkrUiSystem *system,
                                      VkrUiDropdownHandle dropdown);

void vkr_ui_dropdown_set_selected(VkrUiSystem *system,
                                   VkrUiDropdownHandle dropdown,
                                   int32_t index);

void vkr_ui_dropdown_set_options(VkrUiSystem *system,
                                  VkrUiDropdownHandle dropdown,
                                  String8 *options, uint32_t count);
```

## Tooltip Component

```c
typedef struct VkrUiTooltipConfig {
  String8 text;
  float32_t delay_ms;             /**< Delay before showing */
  VkrUiElementHandle anchor;      /**< Element to attach to */
} VkrUiTooltipConfig;

/**
 * @brief Shows a tooltip near an element.
 */
void vkr_ui_tooltip_show(VkrUiSystem *system, const VkrUiTooltipConfig *config);

/**
 * @brief Hides the current tooltip.
 */
void vkr_ui_tooltip_hide(VkrUiSystem *system);
```

## Theming

### Theme Structure

```c
/**
 * @brief UI theme colors.
 */
typedef struct VkrUiThemeColors {
  Vec4 background;
  Vec4 surface;
  Vec4 primary;
  Vec4 secondary;
  Vec4 accent;
  Vec4 error;
  Vec4 text_primary;
  Vec4 text_secondary;
  Vec4 text_disabled;
  Vec4 border;
  Vec4 divider;
} VkrUiThemeColors;

/**
 * @brief Complete UI theme.
 */
typedef struct VkrUiTheme {
  VkrUiThemeColors colors;
  VkrFontHandle default_font;
  float32_t default_font_size;
  float32_t border_radius;
  float32_t spacing;
  float32_t padding;
} VkrUiTheme;

/**
 * @brief Default dark theme.
 */
VkrUiTheme vkr_ui_theme_dark();

/**
 * @brief Default light theme.
 */
VkrUiTheme vkr_ui_theme_light();

/**
 * @brief Sets the active theme.
 */
void vkr_ui_system_set_theme(VkrUiSystem *system, const VkrUiTheme *theme);
```

## Focus Management

### Focus Navigation

```c
/**
 * @brief Moves focus to next focusable element.
 */
void vkr_ui_focus_next(VkrUiSystem *system);

/**
 * @brief Moves focus to previous focusable element.
 */
void vkr_ui_focus_prev(VkrUiSystem *system);

/**
 * @brief Sets focus to specific element.
 */
void vkr_ui_focus_set(VkrUiSystem *system, VkrUiElementHandle element);

/**
 * @brief Gets currently focused element.
 */
VkrUiElementHandle vkr_ui_focus_get(VkrUiSystem *system);

/**
 * @brief Clears focus.
 */
void vkr_ui_focus_clear(VkrUiSystem *system);
```

### Tab Order

Elements are traversed in tree order by default. Custom tab order:

```c
/**
 * @brief Sets explicit tab order for an element.
 */
void vkr_ui_element_set_tab_index(VkrUiElement *element, int32_t index);
```

## Usage Examples

### Settings Panel

```c
VkrUiPanelHandle settings_panel = vkr_ui_panel_create(system, root, &(VkrUiPanelConfig){
    .variant = VKR_UI_PANEL_VARIANT_DEFAULT,
    .layout = {
        .width = VKR_UI_SIZE_PX(400),
        .height = VKR_UI_SIZE_AUTO,
        .flex_direction = VKR_UI_FLEX_COLUMN,
        .gap = 12.0f,
    },
});
VkrUiElementHandle content = vkr_ui_panel_get_content(system, settings_panel);

// Volume slider
VkrUiElementHandle volume_row = vkr_ui_container_create(system, content, &(VkrUiContainerConfig){
    .layout = {
        .flex_direction = VKR_UI_FLEX_ROW,
        .align_items = VKR_UI_ALIGN_CENTER,
        .gap = 10.0f,
    },
});
vkr_ui_text_element_create(system, volume_row, &(VkrUiTextElementConfig){
    .content = string8_lit("Volume"),
    .layout = { .width = VKR_UI_SIZE_PX(100) },
});
vkr_ui_slider_create(system, volume_row, &(VkrUiSliderConfig){
    .min_value = 0.0f,
    .max_value = 100.0f,
    .initial_value = 75.0f,
    .on_change = on_volume_change,
    .layout = { .flex_grow = 1.0f },
});

// Fullscreen checkbox
vkr_ui_checkbox_create(system, content, &(VkrUiCheckboxConfig){
    .label = string8_lit("Fullscreen"),
    .on_change = on_fullscreen_change,
});

// Resolution dropdown
VkrUiElementHandle res_row = vkr_ui_container_create(system, content, &(VkrUiContainerConfig){
    .layout = {
        .flex_direction = VKR_UI_FLEX_ROW,
        .align_items = VKR_UI_ALIGN_CENTER,
        .gap = 10.0f,
    },
});
vkr_ui_text_element_create(system, res_row, &(VkrUiTextElementConfig){
    .content = string8_lit("Resolution"),
});
String8 resolutions[] = {
    string8_lit("1920x1080"),
    string8_lit("2560x1440"),
    string8_lit("3840x2160"),
};
vkr_ui_dropdown_create(system, res_row, &(VkrUiDropdownConfig){
    .options = resolutions,
    .option_count = 3,
    .initial_selection = 0,
    .on_change = on_resolution_change,
    .layout = { .flex_grow = 1.0f },
});

// Apply button
vkr_ui_button_create(system, content, &(VkrUiButtonConfig){
    .label = string8_lit("Apply"),
    .variant = VKR_UI_BUTTON_VARIANT_PRIMARY,
    .on_click = on_apply_settings,
    .layout = { .align_self = VKR_UI_ALIGN_SELF_FLEX_END },
});
```

## Implementation Phases

### Phase 1: Core Components
- Panel
- Button
- Text element wrapper

### Phase 2: Input Components
- Input field
- Checkbox
- Slider

### Phase 3: List Components
- List with virtual scrolling
- Scrollbar
- Dropdown

### Phase 4: Advanced Components
- Text box (multi-line)
- Tooltip
- Context menu

### Phase 5: Polish
- Theming system
- Focus management
- Keyboard navigation
