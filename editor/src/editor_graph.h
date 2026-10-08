#pragma once

#include "vkr_sample_runtime.h"

/* A node-graph canvas. The canvas never edits the caller's document: it draws
   the nodes and wires it is given, keeps its own view (pan, zoom, selection,
   drags and the add popup) and reports what the user did as events the caller
   applies after the build. */

#define VKR_EDITOR_GRAPH_SELECTION_MAX 64u
#define VKR_EDITOR_GRAPH_EVENT_MAX 16u
/* An input of this type accepts an output of any type. */
#define VKR_EDITOR_GRAPH_PORT_ANY UINT32_MAX
/* An event's node field when no node applies. */
#define VKR_EDITOR_GRAPH_NONE UINT32_MAX
#define VKR_EDITOR_GRAPH_ZOOM_MIN 0.25f
#define VKR_EDITOR_GRAPH_ZOOM_MAX 2.0f

/* One input or output of a node. `type` picks the port colour (type modulo
   the palette size) and decides which ports connect: equal types, or any type
   into an input whose type is VKR_EDITOR_GRAPH_PORT_ANY. */
typedef struct VkrEditorGraphPort {
  const char *name;
  /* Text beside the port; NULL uses `name`. */
  const char *label;
  uint32_t type;
} VkrEditorGraphPort;

/* One card. Strings and port arrays are borrowed for the build. */
typedef struct VkrEditorGraphNode {
  /* Stable across frames; the selection keeps ids. */
  uint64_t id;
  /* Top-left corner in canvas points. */
  Vec2 position;
  const char *title;
  /* One line under the title, such as a value or a file; NULL draws none. */
  const char *detail;
  /* Header colour. */
  Vec4 accent;
  const VkrEditorGraphPort *inputs;
  uint32_t input_count;
  const VkrEditorGraphPort *outputs;
  uint32_t output_count;
  /* Top-right note such as "1 sample"; NULL draws none. */
  const char *badge;
  /* Red border, and the badge drawn as an error. */
  bool8_t error;
} VkrEditorGraphNode;

/* A wire from output `from_port` of node `from_node` to input `to_port` of
   node `to_node`; nodes are indices into the node array. */
typedef struct VkrEditorGraphWire {
  uint32_t from_node;
  uint32_t from_port;
  uint32_t to_node;
  uint32_t to_port;
} VkrEditorGraphWire;

/* An entry of the add-node popup. The popup groups entries by `category` in
   the order categories first appear; NULL or "" has no heading. */
typedef struct VkrEditorGraphChoice {
  const char *label;
  const char *category;
} VkrEditorGraphChoice;

typedef enum VkrEditorGraphEventKind {
  /* Every selected node moved by `delta` canvas points; sent once when the
     drag ends. */
  VKR_EDITOR_GRAPH_EVENT_MOVE,
  /* Output (node, port) to input (other_node, other_port). The caller
     replaces any wire already on that input. */
  VKR_EDITOR_GRAPH_EVENT_CONNECT,
  /* The wire on input (node, port) was removed. */
  VKR_EDITOR_GRAPH_EVENT_DISCONNECT,
  /* Add `choice` with its top-left corner at canvas `position`. When a
     dropped wire opened the popup, (node, port) names the port the wire came
     from and `from_output` says whether it is an output; otherwise node is
     VKR_EDITOR_GRAPH_NONE. */
  VKR_EDITOR_GRAPH_EVENT_ADD,
  /* Delete the selection (Delete or Backspace). */
  VKR_EDITOR_GRAPH_EVENT_DELETE,
  /* Duplicate the selection (Primary+D). */
  VKR_EDITOR_GRAPH_EVENT_DUPLICATE,
  /* The selection in the view changed. */
  VKR_EDITOR_GRAPH_EVENT_SELECT,
} VkrEditorGraphEventKind;

typedef struct VkrEditorGraphEvent {
  VkrEditorGraphEventKind kind;
  uint32_t node;
  uint32_t port;
  uint32_t other_node;
  uint32_t other_port;
  bool8_t from_output;
  uint32_t choice;
  Vec2 position;
  Vec2 delta;
} VkrEditorGraphEvent;

/* Rows the add popup shows at once; more scroll. */
#define VKR_EDITOR_GRAPH_POPUP_ROWS 12u

/* The caller keeps one view per canvas at a stable address while its popup
   is open. Fields after `truncated` are the canvas's own. */
typedef struct VkrEditorGraphView {
  /* Canvas point at the top-left corner of the bounds. */
  Vec2 pan;
  /* Screen points per canvas point, VKR_EDITOR_GRAPH_ZOOM_MIN..MAX. */
  float32_t zoom;
  uint64_t selected[VKR_EDITOR_GRAPH_SELECTION_MAX];
  uint32_t selected_count;
  /* The last build ran out of UI nodes and left cards or wires out. */
  bool8_t truncated;

  /* The canvas's own: the pointer gesture in progress, its button, where it
     was pressed (window points), the pan then, whether it passed the drag
     threshold and the card it started on. */
  uint32_t drag;
  uint32_t drag_button;
  Vec2 drag_press;
  Vec2 drag_pan;
  bool8_t drag_moved;
  bool8_t drag_additive;
  bool8_t drag_collapse;
  uint64_t drag_node;
  /* The canvas's own: the dragged wire's fixed port, and the input it was
     detached from when the drag picked up an existing wire. */
  uint64_t wire_node;
  uint32_t wire_port;
  bool8_t wire_from_output;
  bool8_t wire_detached;
  uint64_t wire_detach_node;
  uint32_t wire_detach_port;
  /* The canvas's own: keyboard focus after a press in the canvas, and a
     Space tap that opens the popup on release unless it panned. */
  bool8_t focused;
  bool8_t space_armed;
  /* The canvas's own: the add popup, its window-point corner, the canvas
     point new nodes go to, the port of a dropped wire, the highlighted entry,
     the first visible entry, the pointer when hover last moved the highlight,
     and the search field and its text. */
  bool8_t popup_open;
  Vec2 popup_at;
  Vec2 popup_position;
  bool8_t popup_wired;
  uint64_t popup_node;
  uint32_t popup_port;
  bool8_t popup_from_output;
  uint32_t popup_highlight;
  uint32_t popup_scroll;
  Vec2 popup_mouse;
  VkrUiId popup_field;
  char popup_text[64];
  VkrUiTextEditBuffer popup_buffer;
} VkrEditorGraphView;

void vkr_editor_graph_view_init(VkrEditorGraphView *view);

/* Builds the canvas in `bounds` (window points) on input layer `layer` and
   writes up to `event_capacity` events; returns how many it wrote. Call it in
   a container whose first cell starts at the window origin, such as the root
   or a full-window overlay, after registering `layer` (unless it is 0). The
   add popup uses VKR_EDITOR_POPUP_LAYER, which must lie above `layer`. */
uint32_t vkr_editor_graph_build(
    const VkrSampleUiFrame *frame, VkrEditorGraphView *view, VkrUiRect bounds,
    uint32_t layer, const VkrEditorGraphNode *nodes, uint32_t node_count,
    const VkrEditorGraphWire *wires, uint32_t wire_count,
    const VkrEditorGraphChoice *choices, uint32_t choice_count,
    VkrEditorGraphEvent *events, uint32_t event_capacity);

/* Frames every node, or the selection when `selection_only` and some
   selected node exists, in `bounds` (window points). */
void vkr_editor_graph_frame(VkrEditorGraphView *view, VkrUiRect bounds,
                            const VkrEditorGraphNode *nodes,
                            uint32_t node_count, bool8_t selection_only);

bool8_t vkr_editor_graph_selected(const VkrEditorGraphView *view, uint64_t id);
