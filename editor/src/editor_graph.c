#include "editor_graph.h"
#include "editor_internal.h"

#include <math.h>
#include <string.h>

/* Card geometry in canvas points; the view's zoom scales all of it. */
#define GRAPH_CARD_WIDTH 180.0f
#define GRAPH_HEADER 26.0f
#define GRAPH_DETAIL 18.0f
#define GRAPH_ROW 20.0f
#define GRAPH_ROWS_TOP 4.0f
#define GRAPH_ROWS_BOTTOM 6.0f
#define GRAPH_DOT 8.0f
#define GRAPH_DOT_INSET 10.0f
#define GRAPH_LABEL_INSET 20.0f
#define GRAPH_TEXT_INSET 8.0f
#define GRAPH_RADIUS 6.0f
#define GRAPH_WIRE_WIDTH 2.0f
/* Pointer travel in screen points before a press becomes a drag. */
#define GRAPH_DRAG_THRESHOLD 3.0f
/* Text smaller than this many screen points is left out. */
#define GRAPH_TEXT_MIN 5.5f
/* Nodes left to the rest of the frame after the canvas cards and wires; the
   canvas's own overlays and popup come out of this share. */
#define GRAPH_RESERVED_NODES 320u
/* Background grid: the canvas step doubles until lines are at least the
   minimum spacing apart and no more than the maximum count. */
#define GRAPH_GRID_STEP 32.0f
#define GRAPH_GRID_SPACING_MIN 24.0f
#define GRAPH_GRID_LINE_MAX 48u
/* Add popup metrics in screen points. */
#define GRAPH_POPUP_WIDTH 260.0f
#define GRAPH_POPUP_ROW 22.0f
#define GRAPH_POPUP_PADDING 6.0f
/* Framing leaves this many screen points around the framed cards. */
#define GRAPH_FRAME_MARGIN 40.0f

typedef enum GraphDrag {
  GRAPH_DRAG_NONE = 0,
  GRAPH_DRAG_PAN,
  GRAPH_DRAG_NODES,
  GRAPH_DRAG_BOX,
  GRAPH_DRAG_WIRE,
  /* A right press on empty canvas: a pan once it moves, the popup if not. */
  GRAPH_DRAG_RIGHT,
} GraphDrag;

typedef struct GraphPortHit {
  bool8_t valid;
  bool8_t output;
  uint32_t node;
  uint32_t port;
} GraphPortHit;

/* One row of the add popup: a category heading or a choice. */
typedef struct GraphEntry {
  uint32_t choice;
  bool8_t heading;
} GraphEntry;

typedef struct GraphPopup {
  /* Window points. */
  VkrUiRect rect;
  GraphEntry *entries;
  uint32_t entry_count;
  /* Row tracks laid out this frame. */
  uint32_t visible;
  bool8_t laid_out;
} GraphPopup;

/* One build's inputs and outputs. */
typedef struct GraphFrame {
  const VkrSampleUiFrame *frame;
  VkrUiSystem *ui;
  VkrEditorGraphView *view;
  /* Window points. */
  VkrUiRect bounds;
  const VkrEditorGraphNode *nodes;
  uint32_t node_count;
  const VkrEditorGraphWire *wires;
  uint32_t wire_count;
  const VkrEditorGraphChoice *choices;
  uint32_t choice_count;
  VkrEditorGraphEvent *events;
  uint32_t event_capacity;
  uint32_t event_count;
  /* Pointer in window points. */
  Vec2 mouse;
  /* The pointer is over the canvas and its layer is the top one there. */
  bool8_t hover;
  bool8_t over_popup;
  /* Live move preview of the selection, in canvas points. */
  Vec2 offset;
  /* An input whose wire is not drawn: the one a drag detached. */
  uint32_t hidden_node;
  uint32_t hidden_port;
  /* A connection made this frame, drawn until the caller adds its wire. */
  bool8_t pending;
  VkrEditorGraphWire pending_wire;
  GraphPopup popup;
} GraphFrame;

void vkr_editor_graph_view_init(VkrEditorGraphView *view) {
  if (!view) {
    return;
  }
  MemZero(view, sizeof(*view));
  view->zoom = 1.0f;
  view->popup_buffer.data = (uint8_t *)view->popup_text;
  view->popup_buffer.capacity = sizeof(view->popup_text);
}

bool8_t vkr_editor_graph_selected(const VkrEditorGraphView *view, uint64_t id) {
  if (!view) {
    return false_v;
  }
  for (uint32_t i = 0; i < view->selected_count; ++i) {
    if (view->selected[i] == id) {
      return true_v;
    }
  }
  return false_v;
}

/* Selection edits return whether the selection changed. */
static bool8_t graph_select_add(VkrEditorGraphView *view, uint64_t id) {
  if (vkr_editor_graph_selected(view, id) ||
      view->selected_count == VKR_EDITOR_GRAPH_SELECTION_MAX) {
    return false_v;
  }
  view->selected[view->selected_count++] = id;
  return true_v;
}

static bool8_t graph_select_remove(VkrEditorGraphView *view, uint64_t id) {
  for (uint32_t i = 0; i < view->selected_count; ++i) {
    if (view->selected[i] == id) {
      MemCopy(&view->selected[i], &view->selected[i + 1u],
              sizeof(view->selected[0]) * (view->selected_count - i - 1u));
      view->selected_count--;
      return true_v;
    }
  }
  return false_v;
}

static bool8_t graph_select_only(VkrEditorGraphView *view, uint64_t id) {
  if (view->selected_count == 1u && view->selected[0] == id) {
    return false_v;
  }
  view->selected[0] = id;
  view->selected_count = 1u;
  return true_v;
}

static uint32_t graph_find(const VkrEditorGraphNode *nodes, uint32_t count,
                           uint64_t id) {
  for (uint32_t i = 0; i < count; ++i) {
    if (nodes[i].id == id) {
      return i;
    }
  }
  return VKR_EDITOR_GRAPH_NONE;
}

static void graph_emit(GraphFrame *g, VkrEditorGraphEvent event) {
  if (g->event_count < g->event_capacity) {
    g->events[g->event_count++] = event;
  }
}

static void graph_emit_kind(GraphFrame *g, VkrEditorGraphEventKind kind) {
  graph_emit(g, (VkrEditorGraphEvent){.kind = kind,
                                      .node = VKR_EDITOR_GRAPH_NONE,
                                      .other_node = VKR_EDITOR_GRAPH_NONE});
}

static float32_t graph_rows_top(const VkrEditorGraphNode *node) {
  return GRAPH_HEADER + (node->detail ? GRAPH_DETAIL : 0.0f) + GRAPH_ROWS_TOP;
}

static float32_t graph_card_height(const VkrEditorGraphNode *node) {
  const uint32_t rows = Max(node->input_count, node->output_count);
  return graph_rows_top(node) + (float32_t)rows * GRAPH_ROW + GRAPH_ROWS_BOTTOM;
}

static Vec2 graph_to_screen(const VkrEditorGraphView *view, VkrUiRect bounds,
                            Vec2 canvas) {
  return (Vec2){bounds.x + (canvas.x - view->pan.x) * view->zoom,
                bounds.y + (canvas.y - view->pan.y) * view->zoom};
}

static Vec2 graph_to_canvas(const VkrEditorGraphView *view, VkrUiRect bounds,
                            Vec2 screen) {
  return (Vec2){view->pan.x + (screen.x - bounds.x) / view->zoom,
                view->pan.y + (screen.y - bounds.y) / view->zoom};
}

static bool8_t graph_point_in(VkrUiRect rect, Vec2 point) {
  return point.x >= rect.x && point.x < rect.x + rect.width &&
         point.y >= rect.y && point.y < rect.y + rect.height;
}

static bool8_t graph_rects_overlap(VkrUiRect a, VkrUiRect b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
         b.y < a.y + a.height;
}

/* Canvas corner of a node, moved by the live preview when it is selected. */
static Vec2 graph_node_position(const GraphFrame *g, uint32_t index) {
  Vec2 position = g->nodes[index].position;
  if ((g->offset.x != 0.0f || g->offset.y != 0.0f) &&
      vkr_editor_graph_selected(g->view, g->nodes[index].id)) {
    position.x += g->offset.x;
    position.y += g->offset.y;
  }
  return position;
}

/* A card's rectangle in window points. */
static VkrUiRect graph_card_rect(const GraphFrame *g, uint32_t index) {
  const Vec2 corner =
      graph_to_screen(g->view, g->bounds, graph_node_position(g, index));
  return (VkrUiRect){corner.x, corner.y, GRAPH_CARD_WIDTH * g->view->zoom,
                     graph_card_height(&g->nodes[index]) * g->view->zoom};
}

/* Centre of a port's dot in window points. */
static Vec2 graph_port_point(const GraphFrame *g, uint32_t node, uint32_t port,
                             bool8_t output) {
  const Vec2 corner = graph_node_position(g, node);
  const Vec2 canvas = {corner.x + (output ? GRAPH_CARD_WIDTH - GRAPH_DOT_INSET
                                          : GRAPH_DOT_INSET),
                       corner.y + graph_rows_top(&g->nodes[node]) +
                           (float32_t)port * GRAPH_ROW + GRAPH_ROW * 0.5f};
  return graph_to_screen(g->view, g->bounds, canvas);
}

/* Topmost card under `point` (window points), or VKR_EDITOR_GRAPH_NONE. */
static uint32_t graph_hit_card(const GraphFrame *g, Vec2 point) {
  for (uint32_t cursor = g->node_count; cursor > 0; --cursor) {
    if (graph_point_in(graph_card_rect(g, cursor - 1u), point)) {
      return cursor - 1u;
    }
  }
  return VKR_EDITOR_GRAPH_NONE;
}

/* The port of the topmost card under `point`. A press grabs a port within a
   row-tall square at the card edge around its dot; a drop (`drop`) takes the
   whole half row. */
static GraphPortHit graph_hit_port(const GraphFrame *g, Vec2 point,
                                   bool8_t drop) {
  GraphPortHit hit = {0};
  const uint32_t card = graph_hit_card(g, point);
  if (card == VKR_EDITOR_GRAPH_NONE) {
    return hit;
  }
  const VkrEditorGraphNode *node = &g->nodes[card];
  const VkrUiRect rect = graph_card_rect(g, card);
  const Vec2 local = {(point.x - rect.x) / g->view->zoom,
                      (point.y - rect.y) / g->view->zoom};
  const float32_t rows_top = graph_rows_top(node);
  if (local.y < rows_top) {
    return hit;
  }
  const uint32_t row = (uint32_t)((local.y - rows_top) / GRAPH_ROW);
  const float32_t zone = drop ? GRAPH_CARD_WIDTH * 0.5f : GRAPH_ROW;
  if (local.x < zone && row < node->input_count) {
    hit = (GraphPortHit){
        .valid = true_v, .output = false_v, .node = card, .port = row};
  } else if (local.x >= GRAPH_CARD_WIDTH - zone && row < node->output_count) {
    hit = (GraphPortHit){
        .valid = true_v, .output = true_v, .node = card, .port = row};
  }
  return hit;
}

static bool8_t graph_ports_connect(uint32_t output_type, uint32_t input_type) {
  return output_type == input_type || input_type == VKR_EDITOR_GRAPH_PORT_ANY;
}

static Vec4 graph_port_color(uint32_t type) {
  static const Vec4 palette[] = {
      {0.55f, 0.82f, 0.38f, 1.0f}, {0.95f, 0.78f, 0.30f, 1.0f},
      {0.98f, 0.55f, 0.30f, 1.0f}, {0.85f, 0.42f, 0.78f, 1.0f},
      {0.42f, 0.70f, 0.98f, 1.0f}, {0.40f, 0.88f, 0.84f, 1.0f},
      {0.95f, 0.46f, 0.46f, 1.0f}, {0.70f, 0.62f, 0.96f, 1.0f},
  };
  if (type == VKR_EDITOR_GRAPH_PORT_ANY) {
    return vkr_ui_theme()->text_secondary;
  }
  return palette[type % ArrayCount(palette)];
}

/* Index of the wire into input (node, port), or wire_count. */
static uint32_t graph_input_wire(const GraphFrame *g, uint32_t node,
                                 uint32_t port) {
  for (uint32_t i = 0; i < g->wire_count; ++i) {
    if (g->wires[i].to_node == node && g->wires[i].to_port == port) {
      return i;
    }
  }
  return g->wire_count;
}

static bool8_t graph_output_wired(const GraphFrame *g, uint32_t node,
                                  uint32_t port) {
  for (uint32_t i = 0; i < g->wire_count; ++i) {
    if (g->wires[i].from_node == node && g->wires[i].from_port == port) {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t graph_wire_valid(const GraphFrame *g,
                                const VkrEditorGraphWire *wire) {
  return wire->from_node < g->node_count && wire->to_node < g->node_count &&
         wire->from_port < g->nodes[wire->from_node].output_count &&
         wire->to_port < g->nodes[wire->to_node].input_count;
}

static Vec2 graph_press_point(const GraphFrame *g, Buttons button) {
  int32_t x = 0;
  int32_t y = 0;
  input_get_button_press_position(g->frame->input, button, &x, &y);
  return (Vec2){(float32_t)x / g->ui->content_scale,
                (float32_t)y / g->ui->content_scale};
}

static bool8_t graph_additive(const GraphFrame *g) {
  InputState *input = g->frame->input;
  return input_is_key_down(input, KEY_SHIFT) ||
         input_is_key_down(input, KEY_LSHIFT) ||
         input_is_key_down(input, KEY_RSHIFT) ||
         vkr_editor_selection_modifier(g->frame);
}

static bool8_t graph_text_matches(const char *text, const char *filter,
                                  uint32_t filter_length) {
  if (!text) {
    return false_v;
  }
  const size_t length = strlen(text);
  for (size_t start = 0; start + filter_length <= length; ++start) {
    uint32_t i = 0;
    while (i < filter_length) {
      const char a = text[start + i];
      const char b = filter[i];
      const char lower_a = a >= 'A' && a <= 'Z' ? (char)(a - 'A' + 'a') : a;
      const char lower_b = b >= 'A' && b <= 'Z' ? (char)(b - 'A' + 'a') : b;
      if (lower_a != lower_b) {
        break;
      }
      ++i;
    }
    if (i == filter_length) {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t graph_same_category(const VkrEditorGraphChoice *a,
                                   const VkrEditorGraphChoice *b) {
  const char *left = a->category ? a->category : "";
  const char *right = b->category ? b->category : "";
  return strcmp(left, right) == 0;
}

/* Fills `entries` with the choices matching the popup's search text, grouped
   under category headings in the order categories first appear, and returns
   how many it wrote. `entries` holds twice the choice count. */
static uint32_t graph_popup_entries(const GraphFrame *g, GraphEntry *entries) {
  const VkrEditorGraphView *view = g->view;
  const char *filter = view->popup_text;
  const uint32_t filter_length = view->popup_buffer.length;
  uint32_t count = 0;
  for (uint32_t i = 0; i < g->choice_count; ++i) {
    bool8_t seen = false_v;
    for (uint32_t j = 0; j < i; ++j) {
      if (graph_same_category(&g->choices[i], &g->choices[j])) {
        seen = true_v;
        break;
      }
    }
    if (seen) {
      continue;
    }
    bool8_t heading = g->choices[i].category && g->choices[i].category[0];
    for (uint32_t j = i; j < g->choice_count; ++j) {
      const VkrEditorGraphChoice *choice = &g->choices[j];
      if (!graph_same_category(&g->choices[i], choice)) {
        continue;
      }
      if (filter_length &&
          !graph_text_matches(choice->label, filter, filter_length) &&
          !graph_text_matches(choice->category, filter, filter_length)) {
        continue;
      }
      if (heading) {
        entries[count++] = (GraphEntry){.choice = i, .heading = true_v};
        heading = false_v;
      }
      entries[count++] = (GraphEntry){.choice = j, .heading = false_v};
    }
  }
  return count;
}

/* The first choice entry at or after `from` stepping by `direction`, or
   `fallback` when there is none. */
static uint32_t graph_popup_step(const GraphPopup *popup, uint32_t from,
                                 int32_t direction, uint32_t fallback) {
  int64_t index = (int64_t)from;
  while (index >= 0 && index < (int64_t)popup->entry_count) {
    if (!popup->entries[index].heading) {
      return (uint32_t)index;
    }
    index += direction;
  }
  return fallback;
}

/* Lays the popup out for its current search text: the entries, how many
   rows show and its rectangle kept inside the canvas where it fits. */
static void graph_popup_layout(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  GraphPopup *popup = &g->popup;
  view->popup_buffer.data = (uint8_t *)view->popup_text;
  view->popup_buffer.capacity = sizeof(view->popup_text);
  view->popup_buffer.length =
      Min(view->popup_buffer.length, (uint32_t)sizeof(view->popup_text) - 1u);
  view->popup_text[view->popup_buffer.length] = 0;
  popup->entries = NULL;
  popup->entry_count = 0;
  if (g->choice_count) {
    popup->entries = vkr_allocator_alloc(g->ui->frame_allocator,
                                         (uint64_t)g->choice_count * 2u *
                                             sizeof(*popup->entries),
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (popup->entries) {
    popup->entry_count = graph_popup_entries(g, popup->entries);
  }
  popup->visible =
      Max(1u, Min(popup->entry_count, VKR_EDITOR_GRAPH_POPUP_ROWS));
  const float32_t field_row = vkr_ui_theme()->control_height + 6.0f;
  const float32_t width = GRAPH_POPUP_WIDTH;
  const float32_t height = GRAPH_POPUP_PADDING * 2.0f + field_row +
                           (float32_t)popup->visible * GRAPH_POPUP_ROW;
  float32_t x = Min(view->popup_at.x, g->bounds.x + g->bounds.width - width);
  float32_t y = Min(view->popup_at.y, g->bounds.y + g->bounds.height - height);
  x = Max(x, g->bounds.x);
  y = Max(y, g->bounds.y);
  popup->rect = (VkrUiRect){x, y, width, height};
  popup->laid_out = true_v;
}

static void graph_popup_open(GraphFrame *g, Vec2 at) {
  VkrEditorGraphView *view = g->view;
  view->popup_open = true_v;
  view->popup_at = at;
  view->popup_position = graph_to_canvas(view, g->bounds, at);
  view->popup_wired = false_v;
  view->popup_highlight = 0;
  view->popup_scroll = 0;
  view->popup_mouse = g->mouse;
  view->popup_buffer.length = 0;
  view->popup_text[0] = 0;
}

static void graph_popup_close(VkrEditorGraphView *view, VkrUiSystem *ui) {
  view->popup_open = false_v;
  if (view->popup_field != VKR_UI_ID_NONE &&
      ui->focused_id == view->popup_field) {
    ui->focused_id = VKR_UI_ID_NONE;
    ui->focused_is_text = false_v;
  }
}

static void graph_popup_pick(GraphFrame *g, uint32_t choice) {
  VkrEditorGraphView *view = g->view;
  VkrEditorGraphEvent event = {.kind = VKR_EDITOR_GRAPH_EVENT_ADD,
                               .node = VKR_EDITOR_GRAPH_NONE,
                               .other_node = VKR_EDITOR_GRAPH_NONE,
                               .choice = choice,
                               .position = view->popup_position};
  if (view->popup_wired) {
    const uint32_t node = graph_find(g->nodes, g->node_count, view->popup_node);
    if (node != VKR_EDITOR_GRAPH_NONE) {
      event.node = node;
      event.port = view->popup_port;
      event.from_output = view->popup_from_output;
    }
  }
  graph_emit(g, event);
  graph_popup_close(view, g->ui);
}

/* Drops selected ids whose nodes the caller removed. */
static void graph_prune_selection(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  view->selected_count =
      Min(view->selected_count, (uint32_t)VKR_EDITOR_GRAPH_SELECTION_MAX);
  bool8_t changed = false_v;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < view->selected_count; ++i) {
    if (graph_find(g->nodes, g->node_count, view->selected[i]) ==
        VKR_EDITOR_GRAPH_NONE) {
      changed = true_v;
      continue;
    }
    view->selected[kept++] = view->selected[i];
  }
  view->selected_count = kept;
  if (changed) {
    graph_emit_kind(g, VKR_EDITOR_GRAPH_EVENT_SELECT);
  }
}

static void graph_drag_start(VkrEditorGraphView *view, GraphDrag drag,
                             Buttons button, Vec2 press) {
  view->drag = drag;
  view->drag_button = button;
  view->drag_press = press;
  view->drag_pan = view->pan;
  view->drag_moved = false_v;
  view->drag_additive = false_v;
  view->drag_collapse = false_v;
  view->drag_node = 0;
  view->wire_detached = false_v;
}

/* The box-select rectangle in window points. */
static VkrUiRect graph_box_rect(const GraphFrame *g) {
  const Vec2 a = g->view->drag_press;
  const Vec2 b = g->mouse;
  return (VkrUiRect){Min(a.x, b.x), Min(a.y, b.y), fabsf(b.x - a.x),
                     fabsf(b.y - a.y)};
}

/* A press on a card: Shift or Primary toggles it, a plain press selects it
   alone unless it is already selected, then a drag moves the selection. */
static void graph_press_card(GraphFrame *g, uint32_t card, Vec2 press) {
  VkrEditorGraphView *view = g->view;
  const uint64_t id = g->nodes[card].id;
  bool8_t changed = false_v;
  bool8_t collapse = false_v;
  if (graph_additive(g)) {
    if (vkr_editor_graph_selected(view, id)) {
      changed = graph_select_remove(view, id);
    } else {
      changed = graph_select_add(view, id);
    }
  } else if (!vkr_editor_graph_selected(view, id)) {
    changed = graph_select_only(view, id);
  } else {
    collapse = true_v;
  }
  if (changed) {
    graph_emit_kind(g, VKR_EDITOR_GRAPH_EVENT_SELECT);
  }
  if (vkr_editor_graph_selected(view, id)) {
    graph_drag_start(view, GRAPH_DRAG_NODES, BUTTON_LEFT, press);
    view->drag_node = id;
    view->drag_collapse = collapse;
  }
}

/* A press on a port starts a wire: from an output, from a free input, or
   picking up the wire already on an input by its output end. */
static void graph_press_port(GraphFrame *g, GraphPortHit port, Vec2 press) {
  VkrEditorGraphView *view = g->view;
  graph_drag_start(view, GRAPH_DRAG_WIRE, BUTTON_LEFT, press);
  if (port.output) {
    view->wire_node = g->nodes[port.node].id;
    view->wire_port = port.port;
    view->wire_from_output = true_v;
    return;
  }
  const uint32_t wire = graph_input_wire(g, port.node, port.port);
  if (wire < g->wire_count && graph_wire_valid(g, &g->wires[wire])) {
    view->wire_node = g->nodes[g->wires[wire].from_node].id;
    view->wire_port = g->wires[wire].from_port;
    view->wire_from_output = true_v;
    view->wire_detached = true_v;
    view->wire_detach_node = g->nodes[port.node].id;
    view->wire_detach_port = port.port;
    return;
  }
  view->wire_node = g->nodes[port.node].id;
  view->wire_port = port.port;
  view->wire_from_output = false_v;
}

/* The fixed end of the dragged wire, or false when its node is gone. */
static bool8_t graph_wire_source(const GraphFrame *g, uint32_t *out_node,
                                 const VkrEditorGraphPort **out_port) {
  const VkrEditorGraphView *view = g->view;
  const uint32_t node = graph_find(g->nodes, g->node_count, view->wire_node);
  if (node == VKR_EDITOR_GRAPH_NONE) {
    return false_v;
  }
  const VkrEditorGraphNode *source = &g->nodes[node];
  if (view->wire_from_output) {
    if (view->wire_port >= source->output_count) {
      return false_v;
    }
    *out_port = &source->outputs[view->wire_port];
  } else {
    if (view->wire_port >= source->input_count) {
      return false_v;
    }
    *out_port = &source->inputs[view->wire_port];
  }
  *out_node = node;
  return true_v;
}

/* The port a wire dropped at `point` would connect to, if any. */
static GraphPortHit graph_wire_target(const GraphFrame *g, Vec2 point) {
  GraphPortHit none = {0};
  uint32_t source = 0;
  const VkrEditorGraphPort *source_port = NULL;
  if (!graph_wire_source(g, &source, &source_port)) {
    return none;
  }
  const GraphPortHit hit = graph_hit_port(g, point, true_v);
  if (!hit.valid || hit.output == g->view->wire_from_output ||
      hit.node == source) {
    return none;
  }
  const VkrEditorGraphNode *target = &g->nodes[hit.node];
  const bool8_t fits = hit.output
                           ? graph_ports_connect(target->outputs[hit.port].type,
                                                 source_port->type)
                           : graph_ports_connect(source_port->type,
                                                 target->inputs[hit.port].type);
  return fits ? hit : none;
}

/* Releases a dragged wire: onto a compatible port it connects, onto empty
   canvas it opens the popup for that port, and a picked-up wire released
   anywhere but its own input is removed. */
static void graph_wire_drop(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  uint32_t source = 0;
  const VkrEditorGraphPort *source_port = NULL;
  if (!graph_wire_source(g, &source, &source_port)) {
    return;
  }
  const uint32_t detached =
      view->wire_detached
          ? graph_find(g->nodes, g->node_count, view->wire_detach_node)
          : VKR_EDITOR_GRAPH_NONE;
  const GraphPortHit target = graph_wire_target(g, g->mouse);
  if (target.valid) {
    const VkrEditorGraphWire wire =
        view->wire_from_output ? (VkrEditorGraphWire){source, view->wire_port,
                                                      target.node, target.port}
                               : (VkrEditorGraphWire){target.node, target.port,
                                                      source, view->wire_port};
    if (detached != VKR_EDITOR_GRAPH_NONE && wire.to_node == detached &&
        wire.to_port == view->wire_detach_port) {
      return;
    }
    if (detached != VKR_EDITOR_GRAPH_NONE) {
      graph_emit(
          g, (VkrEditorGraphEvent){.kind = VKR_EDITOR_GRAPH_EVENT_DISCONNECT,
                                   .node = detached,
                                   .port = view->wire_detach_port,
                                   .other_node = VKR_EDITOR_GRAPH_NONE});
      g->hidden_node = detached;
      g->hidden_port = view->wire_detach_port;
    }
    graph_emit(g, (VkrEditorGraphEvent){.kind = VKR_EDITOR_GRAPH_EVENT_CONNECT,
                                        .node = wire.from_node,
                                        .port = wire.from_port,
                                        .other_node = wire.to_node,
                                        .other_port = wire.to_port,
                                        .from_output = true_v});
    g->pending = true_v;
    g->pending_wire = wire;
    return;
  }
  if (detached != VKR_EDITOR_GRAPH_NONE) {
    graph_emit(g,
               (VkrEditorGraphEvent){.kind = VKR_EDITOR_GRAPH_EVENT_DISCONNECT,
                                     .node = detached,
                                     .port = view->wire_detach_port,
                                     .other_node = VKR_EDITOR_GRAPH_NONE});
    g->hidden_node = detached;
    g->hidden_port = view->wire_detach_port;
  }
  if (graph_point_in(g->bounds, g->mouse) &&
      graph_hit_card(g, g->mouse) == VKR_EDITOR_GRAPH_NONE) {
    graph_popup_open(g, g->mouse);
    view->popup_wired = true_v;
    view->popup_node = view->wire_node;
    view->popup_port = view->wire_port;
    view->popup_from_output = view->wire_from_output;
  }
}

/* Selects the cards the box touches, added to the selection when the box
   started with Shift or Primary held. */
static void graph_box_select(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  const VkrUiRect box = graph_box_rect(g);
  uint64_t before[VKR_EDITOR_GRAPH_SELECTION_MAX];
  const uint32_t before_count = view->selected_count;
  MemCopy(before, view->selected, sizeof(before[0]) * before_count);
  if (!view->drag_additive) {
    view->selected_count = 0;
  }
  for (uint32_t i = 0; i < g->node_count; ++i) {
    if (graph_rects_overlap(box, graph_card_rect(g, i))) {
      (void)graph_select_add(view, g->nodes[i].id);
    }
  }
  if (before_count != view->selected_count ||
      MemCompare(before, view->selected, sizeof(before[0]) * before_count) !=
          0) {
    graph_emit_kind(g, VKR_EDITOR_GRAPH_EVENT_SELECT);
  }
}

static void graph_drag_release(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  switch ((GraphDrag)view->drag) {
  case GRAPH_DRAG_NODES:
    if (view->drag_moved) {
      graph_emit(g, (VkrEditorGraphEvent){.kind = VKR_EDITOR_GRAPH_EVENT_MOVE,
                                          .node = VKR_EDITOR_GRAPH_NONE,
                                          .other_node = VKR_EDITOR_GRAPH_NONE,
                                          .delta = g->offset});
    } else if (view->drag_collapse &&
               graph_select_only(view, view->drag_node)) {
      graph_emit_kind(g, VKR_EDITOR_GRAPH_EVENT_SELECT);
    }
    break;
  case GRAPH_DRAG_BOX:
    if (view->drag_moved) {
      graph_box_select(g);
    } else if (!view->drag_additive && view->selected_count) {
      view->selected_count = 0;
      graph_emit_kind(g, VKR_EDITOR_GRAPH_EVENT_SELECT);
    }
    break;
  case GRAPH_DRAG_WIRE:
    if (view->drag_moved) {
      graph_wire_drop(g);
    }
    break;
  case GRAPH_DRAG_RIGHT:
    if (!view->drag_moved && graph_point_in(g->bounds, g->mouse)) {
      graph_popup_open(g, g->mouse);
    }
    break;
  case GRAPH_DRAG_PAN:
  case GRAPH_DRAG_NONE:
    break;
  }
  view->drag = GRAPH_DRAG_NONE;
}

/* Starts a gesture for this frame's press over the canvas. */
static void graph_press(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  VkrUiSystem *ui = g->ui;
  InputState *input = g->frame->input;
  if (input_button_just_pressed(input, BUTTON_MIDDLE)) {
    graph_drag_start(view, GRAPH_DRAG_PAN, BUTTON_MIDDLE,
                     graph_press_point(g, BUTTON_MIDDLE));
    return;
  }
  if (ui->mouse_pressed && input_is_key_down(input, KEY_SPACE)) {
    graph_drag_start(view, GRAPH_DRAG_PAN, BUTTON_LEFT,
                     graph_press_point(g, BUTTON_LEFT));
    view->space_armed = false_v;
    return;
  }
  if (ui->mouse_pressed) {
    const Vec2 press = graph_press_point(g, BUTTON_LEFT);
    const GraphPortHit port = graph_hit_port(g, press, false_v);
    if (port.valid) {
      graph_press_port(g, port, press);
      return;
    }
    const uint32_t card = graph_hit_card(g, press);
    if (card != VKR_EDITOR_GRAPH_NONE) {
      graph_press_card(g, card, press);
      return;
    }
    graph_drag_start(view, GRAPH_DRAG_BOX, BUTTON_LEFT, press);
    view->drag_additive = graph_additive(g);
    return;
  }
  if (input_button_just_pressed(input, BUTTON_RIGHT)) {
    const Vec2 press = graph_press_point(g, BUTTON_RIGHT);
    if (graph_hit_card(g, press) == VKR_EDITOR_GRAPH_NONE) {
      graph_drag_start(view, GRAPH_DRAG_RIGHT, BUTTON_RIGHT, press);
    }
  }
}

/* Follows the gesture in progress and ends it when its button is up. */
static void graph_drag_update(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  VkrUiSystem *ui = g->ui;
  if (ui->mouse_captured) {
    view->drag = GRAPH_DRAG_NONE;
    return;
  }
  ui->capture.mouse = true_v;
  const Vec2 travel = {g->mouse.x - view->drag_press.x,
                       g->mouse.y - view->drag_press.y};
  if (!view->drag_moved &&
      sqrtf(travel.x * travel.x + travel.y * travel.y) > GRAPH_DRAG_THRESHOLD) {
    view->drag_moved = true_v;
  }
  if (view->drag_moved) {
    if (view->drag == GRAPH_DRAG_PAN || view->drag == GRAPH_DRAG_RIGHT) {
      view->pan = (Vec2){view->drag_pan.x - travel.x / view->zoom,
                         view->drag_pan.y - travel.y / view->zoom};
      ui->cursor = VKR_WINDOW_CURSOR_GRABBING;
    } else if (view->drag == GRAPH_DRAG_NODES) {
      g->offset = (Vec2){travel.x / view->zoom, travel.y / view->zoom};
    }
  }
  if (!input_is_button_down(g->frame->input, (Buttons)view->drag_button)) {
    graph_drag_release(g);
  }
}

/* Zooms by the wheel about the pointer, keeping the canvas point under it. */
static void graph_wheel(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  VkrUiSystem *ui = g->ui;
  if (!ui->mouse_wheel) {
    return;
  }
  ui->capture.mouse = true_v;
  const float32_t next =
      vkr_clamp_f32(view->zoom * powf(1.1f, (float32_t)ui->mouse_wheel),
                    VKR_EDITOR_GRAPH_ZOOM_MIN, VKR_EDITOR_GRAPH_ZOOM_MAX);
  if (next == view->zoom) {
    return;
  }
  const Vec2 anchor = graph_to_canvas(view, g->bounds, g->mouse);
  view->zoom = next;
  view->pan = (Vec2){anchor.x - (g->mouse.x - g->bounds.x) / next,
                     anchor.y - (g->mouse.y - g->bounds.y) / next};
}

static void graph_pointer(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  VkrUiSystem *ui = g->ui;
  InputState *input = g->frame->input;
  const bool8_t pressed = ui->mouse_pressed ||
                          input_button_just_pressed(input, BUTTON_RIGHT) ||
                          input_button_just_pressed(input, BUTTON_MIDDLE);
  if (pressed && !g->over_popup) {
    view->focused = g->hover;
  }
  /* A press outside the open popup closes it and does nothing else. */
  if (view->popup_open) {
    if (pressed && !g->over_popup) {
      graph_popup_close(view, ui);
    }
    return;
  }
  if (view->drag != GRAPH_DRAG_NONE) {
    graph_drag_update(g);
    return;
  }
  if (!g->hover) {
    return;
  }
  graph_wheel(g);
  if (pressed && ui->active_id == VKR_UI_ID_NONE) {
    graph_press(g);
    if (view->drag != GRAPH_DRAG_NONE) {
      ui->capture.mouse = true_v;
    }
  }
  if (view->drag == GRAPH_DRAG_NONE &&
      graph_hit_port(g, g->mouse, false_v).valid) {
    ui->cursor = VKR_WINDOW_CURSOR_CROSSHAIR;
  }
}

/* Keeps Scene shortcuts such as Delete off the scene while the canvas has
   the keyboard. */
static void graph_block_scene_shortcuts(GraphFrame *g) {
  if (g->frame->scene_shortcuts_blocked) {
    *g->frame->scene_shortcuts_blocked = true_v;
  }
}

/* Marks this frame's key as used by the canvas. */
static void graph_take_keys(GraphFrame *g) {
  g->ui->capture.keyboard = true_v;
  graph_block_scene_shortcuts(g);
}

/* Canvas shortcuts, taken while the pointer is over the canvas or a press
   gave it focus, and no widget holds keyboard focus. */
static void graph_keys(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  VkrUiSystem *ui = g->ui;
  InputState *input = g->frame->input;
  if (view->popup_open) {
    return;
  }
  if (view->drag != GRAPH_DRAG_NONE && !g->frame->escape_taken &&
      input_key_just_pressed(input, KEY_ESCAPE)) {
    view->drag = GRAPH_DRAG_NONE;
    g->offset = (Vec2){0};
    graph_take_keys(g);
    return;
  }
  const bool8_t scope = !ui->mouse_captured &&
                        ui->focused_id == VKR_UI_ID_NONE &&
                        (view->focused || g->hover);
  if (input_key_just_released(input, KEY_SPACE) && view->space_armed) {
    view->space_armed = false_v;
    if (g->hover && view->drag == GRAPH_DRAG_NONE) {
      graph_popup_open(g, g->mouse);
    }
  }
  if (!scope) {
    return;
  }
  if (view->focused) {
    graph_block_scene_shortcuts(g);
  }
  if (g->hover && input_key_just_pressed(input, KEY_SPACE)) {
    view->space_armed = true_v;
    graph_take_keys(g);
  }
  if (view->drag != GRAPH_DRAG_NONE) {
    return;
  }
  if (view->selected_count && (input_key_just_pressed(input, KEY_DELETE) ||
                               input_key_just_pressed(input, KEY_BACKSPACE))) {
    graph_emit_kind(g, VKR_EDITOR_GRAPH_EVENT_DELETE);
    graph_take_keys(g);
  }
  if (view->selected_count && input_key_just_pressed(input, KEY_D) &&
      input_key_shortcut_modifier(input, KEY_D)) {
    graph_emit_kind(g, VKR_EDITOR_GRAPH_EVENT_DUPLICATE);
    graph_take_keys(g);
  }
  if (input_key_just_pressed(input, KEY_F) &&
      !input_key_press_modifiers(input, KEY_F)) {
    vkr_editor_graph_frame(view, g->bounds, g->nodes, g->node_count, true_v);
    graph_take_keys(g);
  }
  if (input_key_just_pressed(input, KEY_A) &&
      !input_key_press_modifiers(input, KEY_A)) {
    vkr_editor_graph_frame(view, g->bounds, g->nodes, g->node_count, false_v);
    graph_take_keys(g);
  }
}

/* Whether `cost` more nodes fit and still leave the rest of the frame its
   share. */
static bool8_t graph_budget(const VkrUiSystem *ui, uint32_t cost) {
  return ui->frame_node_count + cost + GRAPH_RESERVED_NODES <=
         ui->frame_node_capacity;
}

/* Layout cannot place a node left of or above its container's origin, so the
   part of `rect` (container-local points) there is cut off. Reports the cut
   sides; false when nothing remains. */
static bool8_t graph_clip(VkrUiRect *rect, bool8_t *cut_left,
                          bool8_t *cut_top) {
  *cut_left = rect->x < 0.0f;
  *cut_top = rect->y < 0.0f;
  if (*cut_left) {
    rect->width += rect->x;
    rect->x = 0.0f;
  }
  if (*cut_top) {
    rect->height += rect->y;
    rect->y = 0.0f;
  }
  return rect->width > 0.0f && rect->height > 0.0f;
}

static VkrUiWidgetConfig graph_placed(VkrUiRect rect) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement.column = 0;
  config.placement.row = 0;
  config.placement.justify = VKR_UI_ALIGN_START;
  config.placement.align = VKR_UI_ALIGN_START;
  config.placement.margin_pt = (VkrUiEdges){rect.y, 0, 0, rect.x};
  config.style.min_size_pt = (Vec2){rect.width, rect.height};
  config.style.max_size_pt = (Vec2){rect.width, rect.height};
  config.style.padding_pt = (VkrUiEdges){0};
  config.style.background_color = (Vec4){0};
  return config;
}

/* A filled box at `rect` (container-local points) with per-corner radii
   (top-left, top-right, bottom-right, bottom-left) and an optional border. */
static void graph_box(VkrUiSystem *ui, String8 id, VkrUiRect rect, Vec4 color,
                      Vec4 radii, float32_t border, Vec4 border_color) {
  bool8_t cut_left = false_v;
  bool8_t cut_top = false_v;
  if (!graph_clip(&rect, &cut_left, &cut_top)) {
    return;
  }
  const float32_t limit = Min(rect.width, rect.height) * 0.5f;
  VkrUiWidgetConfig box = graph_placed(rect);
  box.style.background_color = color;
  box.style.corner_radius_pt =
      (Vec4){cut_left || cut_top ? 0.0f : Min(radii.x, limit),
             cut_top ? 0.0f : Min(radii.y, limit), Min(radii.z, limit),
             cut_left ? 0.0f : Min(radii.w, limit)};
  if (border > 0.0f) {
    box.style.border_pt = (VkrUiEdges){cut_top ? 0.0f : border, border, border,
                                       cut_left ? 0.0f : border};
    box.style.border_color = border_color;
  }
  vkr_ui_label(ui, id, (String8){0}, &box);
}

static VkrUiWidgetConfig graph_text_config(float32_t size, Vec4 color) {
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement.column = 0;
  config.placement.row = 0;
  config.placement.align = VKR_UI_ALIGN_START;
  config.style.padding_pt = (VkrUiEdges){0};
  config.style.background_color = (Vec4){0};
  config.style.font_size_pt = size;
  config.style.text_color = color;
  config.text.font_size = size;
  return config;
}

/* One line of text starting at container-local (rect.x, rect.y), centred in
   a rect.height row and elided past rect.width. A start cut off on the left
   slides to the edge. */
static void graph_text(VkrUiSystem *ui, String8 id, const char *text,
                       VkrUiRect rect, float32_t size, Vec4 color) {
  if (!text || !text[0] || size < GRAPH_TEXT_MIN || rect.y < 0.0f) {
    return;
  }
  if (rect.x < 0.0f) {
    rect.width += rect.x;
    rect.x = 0.0f;
  }
  if (rect.width < size) {
    return;
  }
  VkrUiWidgetConfig config = graph_text_config(size, color);
  config.placement.justify = VKR_UI_ALIGN_START;
  config.placement.margin_pt = (VkrUiEdges){rect.y, 0, 0, rect.x};
  config.style.min_size_pt = (Vec2){0, rect.height};
  config.style.max_size_pt = (Vec2){rect.width, rect.height};
  vkr_ui_label(ui, id, string8_create((uint8_t *)text, strlen(text)), &config);
}

/* One line of text ending `right` points before the container's right content
   edge, at container-local `top`, at most `width` wide. */
static void graph_text_end(VkrUiSystem *ui, String8 id, const char *text,
                           float32_t top, float32_t right, float32_t width,
                           float32_t height, float32_t size, Vec4 color,
                           Vec4 background) {
  if (!text || !text[0] || size < GRAPH_TEXT_MIN || top < 0.0f ||
      width < size) {
    return;
  }
  VkrUiWidgetConfig config = graph_text_config(size, color);
  config.placement.justify = VKR_UI_ALIGN_END;
  config.placement.margin_pt = (VkrUiEdges){top, Max(0.0f, right), 0, 0};
  config.style.min_size_pt = (Vec2){0, height};
  config.style.max_size_pt = (Vec2){width, height};
  if (background.w > 0.0f) {
    const float32_t pad = size * 0.4f;
    config.style.padding_pt = (VkrUiEdges){0, pad, 0, pad};
    config.style.background_color = background;
    config.style.corner_radius_pt = (Vec4){pad, pad, pad, pad};
  }
  vkr_ui_label(ui, id, string8_create((uint8_t *)text, strlen(text)), &config);
}

/* A cubic wire between two container-local points, bowed horizontally, or
   nothing when it lies outside the canvas. */
static void graph_wire(GraphFrame *g, String8 id, Vec2 from, Vec2 to,
                       Vec4 color) {
  const float32_t zoom = g->view->zoom;
  const float32_t bow = Max(fabsf(to.x - from.x) * 0.5f, 30.0f * zoom);
  const Vec2 points[4] = {from, {from.x + bow, from.y}, {to.x - bow, to.y}, to};
  const float32_t left = Min(from.x, to.x - bow);
  const float32_t right = Max(to.x, from.x + bow);
  const float32_t top = Min(from.y, to.y);
  const float32_t bottom = Max(from.y, to.y);
  if (right < 0.0f || bottom < 0.0f || left > g->bounds.width ||
      top > g->bounds.height) {
    return;
  }
  VkrUiWidgetConfig wire = graph_placed((VkrUiRect){0, 0, 1, 1});
  wire.style.text_color = color;
  vkr_ui_bezier(g->ui, id, points, Max(1.0f, GRAPH_WIRE_WIDTH * zoom), &wire);
}

static Vec2 graph_local(const GraphFrame *g, Vec2 window) {
  return (Vec2){window.x - g->bounds.x, window.y - g->bounds.y};
}

static void graph_draw_grid(GraphFrame *g) {
  const VkrEditorGraphView *view = g->view;
  float32_t step = GRAPH_GRID_STEP;
  while (step * view->zoom < GRAPH_GRID_SPACING_MIN) {
    step *= 2.0f;
  }
  while (g->bounds.width / (step * view->zoom) +
             g->bounds.height / (step * view->zoom) >
         (float32_t)GRAPH_GRID_LINE_MAX) {
    step *= 2.0f;
  }
  if (!graph_budget(g->ui, GRAPH_GRID_LINE_MAX + 2u)) {
    return;
  }
  const Vec4 color = vkr_ui_color_alpha(vkr_ui_theme()->text, 0.045f);
  uint32_t line = 0;
  for (float32_t x = ceilf(view->pan.x / step) * step;
       line < GRAPH_GRID_LINE_MAX; x += step) {
    const float32_t local = (x - view->pan.x) * view->zoom;
    if (local >= g->bounds.width) {
      break;
    }
    (void)vkr_ui_push_id_u64(g->ui, line++);
    graph_box(g->ui, string8_lit("grid"),
              (VkrUiRect){local, 0, 1, g->bounds.height}, color, (Vec4){0}, 0,
              (Vec4){0});
    (void)vkr_ui_pop_id(g->ui);
  }
  for (float32_t y = ceilf(view->pan.y / step) * step;
       line < GRAPH_GRID_LINE_MAX; y += step) {
    const float32_t local = (y - view->pan.y) * view->zoom;
    if (local >= g->bounds.height) {
      break;
    }
    (void)vkr_ui_push_id_u64(g->ui, line++);
    graph_box(g->ui, string8_lit("grid"),
              (VkrUiRect){0, local, g->bounds.width, 1}, color, (Vec4){0}, 0,
              (Vec4){0});
    (void)vkr_ui_pop_id(g->ui);
  }
}

static void graph_draw_wires(GraphFrame *g) {
  for (uint32_t i = 0; i < g->wire_count; ++i) {
    const VkrEditorGraphWire *wire = &g->wires[i];
    if (!graph_wire_valid(g, wire) ||
        (wire->to_node == g->hidden_node && wire->to_port == g->hidden_port)) {
      continue;
    }
    if (!graph_budget(g->ui, 1u)) {
      g->view->truncated = true_v;
      return;
    }
    const Vec2 from = graph_local(
        g, graph_port_point(g, wire->from_node, wire->from_port, true_v));
    const Vec2 to = graph_local(
        g, graph_port_point(g, wire->to_node, wire->to_port, false_v));
    const Vec4 color = graph_port_color(
        g->nodes[wire->from_node].outputs[wire->from_port].type);
    (void)vkr_ui_push_id_u64(g->ui, i);
    graph_wire(g, string8_lit("wire"), from, to, color);
    (void)vkr_ui_pop_id(g->ui);
  }
  if (g->pending && graph_wire_valid(g, &g->pending_wire)) {
    const VkrEditorGraphWire *wire = &g->pending_wire;
    const Vec2 from = graph_local(
        g, graph_port_point(g, wire->from_node, wire->from_port, true_v));
    const Vec2 to = graph_local(
        g, graph_port_point(g, wire->to_node, wire->to_port, false_v));
    graph_wire(g, string8_lit("wire.pending"), from, to,
               graph_port_color(
                   g->nodes[wire->from_node].outputs[wire->from_port].type));
  }
}

/* Upper bound of the UI nodes one card adds. */
static uint32_t graph_card_cost(const VkrEditorGraphNode *node) {
  return 5u + 2u * (node->input_count + node->output_count);
}

/* A card's children are placed from its top-left corner in screen points;
   `origin` maps that to the card panel's content, which starts past the
   border and past any part of the card cut off by the canvas edge. */
typedef struct GraphCard {
  Vec2 origin;
  float32_t zoom;
  float32_t border;
} GraphCard;

static VkrUiRect graph_card_child(const GraphCard *card, float32_t x,
                                  float32_t y, float32_t width,
                                  float32_t height) {
  return (VkrUiRect){x + card->origin.x, y + card->origin.y, width, height};
}

/* The dot and label of one port row. */
static void graph_draw_port(GraphFrame *g, const GraphCard *card,
                            uint32_t index, uint32_t row, bool8_t output,
                            const GraphPortHit *hot) {
  VkrUiSystem *ui = g->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrEditorGraphNode *node = &g->nodes[index];
  const VkrEditorGraphPort *port =
      output ? &node->outputs[row] : &node->inputs[row];
  const float32_t z = card->zoom;
  const float32_t top = (graph_rows_top(node) + (float32_t)row * GRAPH_ROW) * z;
  const bool8_t lit = hot->valid && hot->node == index &&
                      hot->output == output && hot->port == row;
  const bool8_t wired = output
                            ? graph_output_wired(g, index, row)
                            : graph_input_wire(g, index, row) < g->wire_count;
  const Vec4 color = graph_port_color(port->type);
  const float32_t dot = GRAPH_DOT * z * (lit ? 1.35f : 1.0f);
  const float32_t center_x =
      (output ? GRAPH_CARD_WIDTH - GRAPH_DOT_INSET : GRAPH_DOT_INSET) * z;
  const float32_t center_y = top + GRAPH_ROW * z * 0.5f;
  const float32_t radius = dot * 0.5f;
  (void)vkr_ui_push_id_u64(ui, (uint64_t)row * 2u + (output ? 1u : 0u));
  graph_box(
      ui, string8_lit("dot"),
      graph_card_child(card, center_x - radius, center_y - radius, dot, dot),
      wired || lit ? color : theme->raised,
      (Vec4){radius, radius, radius, radius}, Max(1.0f, 1.5f * z),
      lit ? theme->text : color);
  const char *text = port->label ? port->label : port->name;
  const bool8_t shared =
      output ? row < node->input_count : row < node->output_count;
  const float32_t width =
      (shared ? GRAPH_CARD_WIDTH * 0.5f - GRAPH_LABEL_INSET
              : GRAPH_CARD_WIDTH - GRAPH_LABEL_INSET - GRAPH_TEXT_INSET) *
      z;
  const float32_t size = theme->font_caption * z;
  if (output) {
    graph_text_end(ui, string8_lit("label"), text, top + card->origin.y,
                   GRAPH_LABEL_INSET * z - card->border, width, GRAPH_ROW * z,
                   size, theme->text_secondary, (Vec4){0});
  } else {
    graph_text(ui, string8_lit("label"), text,
               graph_card_child(card, GRAPH_LABEL_INSET * z, top, width,
                                GRAPH_ROW * z),
               size, theme->text_secondary);
  }
  (void)vkr_ui_pop_id(ui);
}

static void graph_draw_card(GraphFrame *g, uint32_t index, bool8_t selected,
                            const GraphPortHit *hot) {
  VkrUiSystem *ui = g->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrEditorGraphNode *node = &g->nodes[index];
  const float32_t z = g->view->zoom;
  const VkrUiRect screen = graph_card_rect(g, index);
  const Vec2 local = graph_local(g, (Vec2){screen.x, screen.y});
  const float32_t cut_left = Max(0.0f, -local.x);
  const float32_t cut_top = Max(0.0f, -local.y);
  const VkrUiRect rect = {local.x + cut_left, local.y + cut_top,
                          screen.width - cut_left, screen.height - cut_top};
  if (rect.width <= 0.0f || rect.height <= 0.0f) {
    return;
  }
  const float32_t border = Max(0.75f, (selected ? 2.0f : 1.0f) * z);
  const float32_t radius = GRAPH_RADIUS * z;
  const VkrUiEdges edges = {cut_top > 0.0f ? 0.0f : border, border, border,
                            cut_left > 0.0f ? 0.0f : border};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0;
  panel.placement.row = 0;
  panel.placement.justify = VKR_UI_ALIGN_START;
  panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){rect.y, 0, 0, rect.x};
  panel.style.min_size_pt = (Vec2){rect.width, rect.height};
  panel.style.max_size_pt = (Vec2){rect.width, rect.height};
  panel.style.padding_pt = (VkrUiEdges){0};
  panel.style.border_pt = edges;
  panel.style.border_color = node->error ? theme->error
                             : selected  ? theme->accent
                                         : theme->border;
  panel.style.background_color = theme->raised;
  panel.style.corner_radius_pt = (Vec4){
      cut_left > 0.0f || cut_top > 0.0f ? 0.0f : radius,
      cut_top > 0.0f ? 0.0f : radius, radius, cut_left > 0.0f ? 0.0f : radius};
  panel.style.shadow_color = theme->shadow;
  panel.style.shadow_offset_pt = (Vec2){0.0f, 3.0f * z};
  panel.style.shadow_blur_pt = 10.0f * z;
  panel.clip_children = true_v;
  (void)vkr_ui_push_id_u64(ui, node->id);
  if (!vkr_ui_panel_begin(ui, string8_lit("card"), &panel)) {
    (void)vkr_ui_pop_id(ui);
    return;
  }
  const GraphCard card = {
      .origin = {-cut_left - edges.left, -cut_top - edges.top},
      .zoom = z,
      .border = border,
  };
  const Vec4 header_color =
      vkr_ui_color_mix(theme->raised, node->accent, 0.55f);
  const float32_t inner = Max(0.0f, radius - border);
  graph_box(ui, string8_lit("header"),
            graph_card_child(&card, border, border,
                             screen.width - border * 2.0f,
                             GRAPH_HEADER * z - border),
            header_color, (Vec4){inner, inner, 0, 0}, 0, (Vec4){0});
  graph_text(ui, string8_lit("title"), node->title,
             graph_card_child(&card, GRAPH_TEXT_INSET * z, border,
                              screen.width - GRAPH_TEXT_INSET * z * 2.0f,
                              GRAPH_HEADER * z - border),
             theme->font_body * z, theme->text);
  if (node->badge) {
    graph_text_end(
        ui, string8_lit("badge"), node->badge,
        border + card.origin.y + 4.0f * z, GRAPH_TEXT_INSET * z * 0.5f - border,
        screen.width * 0.5f, GRAPH_HEADER * z - border - 8.0f * z,
        theme->font_caption * z,
        node->error ? theme->error : theme->text_secondary, header_color);
  }
  if (node->detail) {
    graph_text(ui, string8_lit("detail"), node->detail,
               graph_card_child(&card, GRAPH_TEXT_INSET * z, GRAPH_HEADER * z,
                                screen.width - GRAPH_TEXT_INSET * z * 2.0f,
                                GRAPH_DETAIL * z),
               theme->font_caption * z, theme->text_secondary);
  }
  for (uint32_t row = 0; row < node->input_count; ++row) {
    graph_draw_port(g, &card, index, row, false_v, hot);
  }
  for (uint32_t row = 0; row < node->output_count; ++row) {
    graph_draw_port(g, &card, index, row, true_v, hot);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_pop_id(ui);
}

static void graph_draw_cards(GraphFrame *g, const GraphPortHit *hot) {
  const VkrEditorGraphView *view = g->view;
  const bool8_t boxing = view->drag == GRAPH_DRAG_BOX && view->drag_moved;
  const VkrUiRect box = boxing ? graph_box_rect(g) : (VkrUiRect){0};
  for (uint32_t i = 0; i < g->node_count; ++i) {
    const VkrUiRect rect = graph_card_rect(g, i);
    if (!graph_rects_overlap(rect, g->bounds)) {
      continue;
    }
    if (!graph_budget(g->ui, graph_card_cost(&g->nodes[i]))) {
      g->view->truncated = true_v;
      return;
    }
    const bool8_t selected = vkr_editor_graph_selected(view, g->nodes[i].id) ||
                             (boxing && graph_rects_overlap(box, rect));
    graph_draw_card(g, i, selected, hot);
  }
}

/* The wire following the pointer, snapped to a port it would connect to. */
static void graph_draw_live_wire(GraphFrame *g, const GraphPortHit *target) {
  const VkrEditorGraphView *view = g->view;
  uint32_t source = 0;
  const VkrEditorGraphPort *source_port = NULL;
  if (view->drag != GRAPH_DRAG_WIRE || !view->drag_moved ||
      !graph_wire_source(g, &source, &source_port)) {
    return;
  }
  const Vec2 fixed = graph_local(
      g, graph_port_point(g, source, view->wire_port, view->wire_from_output));
  Vec2 loose = graph_local(g, g->mouse);
  if (target->valid) {
    loose = graph_local(
        g, graph_port_point(g, target->node, target->port, target->output));
  }
  if (view->wire_from_output) {
    graph_wire(g, string8_lit("wire.live"), fixed, loose,
               graph_port_color(source_port->type));
  } else {
    graph_wire(g, string8_lit("wire.live"), loose, fixed,
               graph_port_color(source_port->type));
  }
}

static void graph_draw(GraphFrame *g) {
  VkrUiSystem *ui = g->ui;
  VkrEditorGraphView *view = g->view;
  const VkrUiTheme *theme = vkr_ui_theme();
  view->truncated = false_v;
  /* The canvas cell reaches past its right and bottom edges by the largest
     card, so cards there keep their size and are clipped, not squeezed. */
  float32_t overscan_y = 0.0f;
  for (uint32_t i = 0; i < g->node_count; ++i) {
    overscan_y = Max(overscan_y, graph_card_height(&g->nodes[i]));
  }
  const VkrUiTrack column = {.value = g->bounds.width +
                                      GRAPH_CARD_WIDTH * view->zoom + 1.0f,
                             .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack row = {.value =
                              g->bounds.height + overscan_y * view->zoom + 1.0f,
                          .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig canvas = vkr_ui_panel_config_default();
  canvas.placement.column = 0;
  canvas.placement.row = 0;
  canvas.placement.justify = VKR_UI_ALIGN_START;
  canvas.placement.align = VKR_UI_ALIGN_START;
  canvas.placement.margin_pt = (VkrUiEdges){g->bounds.y, 0, 0, g->bounds.x};
  canvas.style.min_size_pt = (Vec2){g->bounds.width, g->bounds.height};
  canvas.style.max_size_pt = (Vec2){g->bounds.width, g->bounds.height};
  canvas.style.padding_pt = (VkrUiEdges){0};
  canvas.style.background_color = theme->window;
  canvas.columns = &column;
  canvas.column_count = 1;
  canvas.rows = &row;
  canvas.row_count = 1;
  canvas.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("canvas"), &canvas)) {
    return;
  }
  const bool8_t wiring = view->drag == GRAPH_DRAG_WIRE && view->drag_moved;
  const GraphPortHit target =
      wiring ? graph_wire_target(g, g->mouse) : (GraphPortHit){0};
  GraphPortHit hot = target;
  if (!wiring && g->hover && view->drag == GRAPH_DRAG_NONE &&
      !view->popup_open) {
    hot = graph_hit_port(g, g->mouse, false_v);
  }
  graph_draw_grid(g);
  graph_draw_wires(g);
  graph_draw_cards(g, &hot);
  graph_draw_live_wire(g, &target);
  if (view->drag == GRAPH_DRAG_BOX && view->drag_moved) {
    const VkrUiRect box = graph_box_rect(g);
    const Vec2 corner = graph_local(g, (Vec2){box.x, box.y});
    graph_box(ui, string8_lit("box"),
              (VkrUiRect){corner.x, corner.y, Max(1.0f, box.width),
                          Max(1.0f, box.height)},
              vkr_ui_color_alpha(theme->accent, 0.12f), (Vec4){0}, 1.0f,
              vkr_ui_color_alpha(theme->accent, 0.7f));
  }
  if (view->truncated) {
    VkrUiWidgetConfig note =
        vkr_editor_text_config(theme->font_caption, theme->warning);
    note.placement.column = 0;
    note.placement.row = 0;
    note.placement.margin_pt =
        (VkrUiEdges){Max(0.0f, g->bounds.height - 22.0f), 0, 0, 10.0f};
    vkr_ui_label(ui, string8_lit("truncated"),
                 string8_lit("Graph truncated: too many UI nodes"), &note);
  }
  (void)vkr_ui_panel_end(ui);
}

/* Up, Down, Enter and Escape in the popup, which keeps keyboard focus in its
   search field while open. Returns the picked choice, or UINT32_MAX. */
static uint32_t graph_popup_keys(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  const GraphPopup *popup = &g->popup;
  InputState *input = g->frame->input;
  graph_take_keys(g);
  if (input_key_just_pressed(input, KEY_ESCAPE) && !g->frame->escape_taken) {
    graph_popup_close(view, g->ui);
    return UINT32_MAX;
  }
  if (input_key_just_pressed(input, KEY_DOWN) &&
      view->popup_highlight + 1u < popup->entry_count) {
    view->popup_highlight = graph_popup_step(popup, view->popup_highlight + 1u,
                                             1, view->popup_highlight);
  }
  if (input_key_just_pressed(input, KEY_UP) && view->popup_highlight > 0u) {
    view->popup_highlight = graph_popup_step(popup, view->popup_highlight - 1u,
                                             -1, view->popup_highlight);
  }
  if (input_key_just_pressed(input, KEY_ENTER) &&
      view->popup_highlight < popup->entry_count &&
      !popup->entries[view->popup_highlight].heading) {
    return popup->entries[view->popup_highlight].choice;
  }
  return UINT32_MAX;
}

/* Keeps the highlighted entry, and the heading above it, in view. */
static void graph_popup_reveal(VkrEditorGraphView *view,
                               const GraphPopup *popup) {
  const uint32_t visible = popup->visible;
  uint32_t top = view->popup_highlight;
  if (top > 0u && popup->entries[top - 1u].heading) {
    top--;
  }
  if (top < view->popup_scroll) {
    view->popup_scroll = top;
  }
  if (view->popup_highlight >= view->popup_scroll + visible) {
    view->popup_scroll = view->popup_highlight + 1u - visible;
  }
}

static void graph_popup_rows(GraphFrame *g, uint32_t *picked) {
  VkrEditorGraphView *view = g->view;
  VkrUiSystem *ui = g->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const GraphPopup *popup = &g->popup;
  if (!popup->entry_count) {
    VkrUiWidgetConfig empty =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    empty.placement.column = 0;
    empty.placement.row = 1;
    empty.placement.align = VKR_UI_ALIGN_CENTER;
    empty.placement.margin_pt.left = 8.0f;
    vkr_ui_label(ui, string8_lit("empty"), string8_lit("No matching nodes"),
                 &empty);
    return;
  }
  const bool8_t moved =
      g->mouse.x != view->popup_mouse.x || g->mouse.y != view->popup_mouse.y;
  for (uint32_t row = 0; row < popup->visible; ++row) {
    const uint32_t index = view->popup_scroll + row;
    if (index >= popup->entry_count) {
      break;
    }
    const GraphEntry *entry = &popup->entries[index];
    const VkrEditorGraphChoice *choice = &g->choices[entry->choice];
    (void)vkr_ui_push_id_u64(ui, index);
    if (entry->heading) {
      VkrUiWidgetConfig heading =
          vkr_editor_text_config(theme->font_caption, theme->text_secondary);
      heading.placement.column = 0;
      heading.placement.row = 1u + row;
      heading.placement.align = VKR_UI_ALIGN_END;
      heading.placement.margin_pt = (VkrUiEdges){0, 0, 3.0f, 8.0f};
      vkr_ui_label(
          ui, string8_lit("heading"),
          string8_create((uint8_t *)choice->category, strlen(choice->category)),
          &heading);
      (void)vkr_ui_pop_id(ui);
      continue;
    }
    const VkrUiId row_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("row"));
    if (moved && ui->hot_id == row_id) {
      view->popup_highlight = index;
    }
    const bool8_t lit = view->popup_highlight == index;
    VkrUiWidgetConfig hit = vkr_ui_widget_config_default();
    hit.placement.column = 0;
    hit.placement.row = 1u + row;
    hit.fill = true_v;
    vkr_editor_ghost_style(&hit);
    hit.style.hover_background_color = theme->accent;
    if (lit) {
      hit.style.background_color = theme->accent;
    }
    if (vkr_ui_button(ui, string8_lit("row"), (String8){0}, &hit)) {
      *picked = entry->choice;
    }
    VkrUiWidgetConfig label = vkr_editor_text_config(
        theme->font_body, lit ? theme->text_on_accent : theme->text);
    label.placement.column = 0;
    label.placement.row = 1u + row;
    label.placement.align = VKR_UI_ALIGN_CENTER;
    label.placement.margin_pt.left = 16.0f;
    label.placement.margin_pt.right = 8.0f;
    const char *text = choice->label ? choice->label : "";
    vkr_ui_label(ui, string8_lit("label"),
                 string8_create((uint8_t *)text, strlen(text)), &label);
    (void)vkr_ui_pop_id(ui);
  }
  if (moved) {
    view->popup_mouse = g->mouse;
  }
}

/* The add popup: a search field over the matching choices, on the popup
   layer above the canvas. */
static void graph_popup_build(GraphFrame *g) {
  VkrEditorGraphView *view = g->view;
  VkrUiSystem *ui = g->ui;
  if (!view->popup_open) {
    return;
  }
  GraphPopup *popup = &g->popup;
  const uint32_t previous_layer = ui->input_layer;
  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_POPUP_LAYER);
  (void)vkr_ui_keyboard_layer_set(ui, VKR_EDITOR_POPUP_LAYER);
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiTrack rows[1u + VKR_EDITOR_GRAPH_POPUP_ROWS];
  rows[0] = (VkrUiTrack){.value = theme->control_height + 6.0f,
                         .unit = VKR_UI_TRACK_PX};
  for (uint32_t i = 0; i < popup->visible; ++i) {
    rows[1u + i] =
        (VkrUiTrack){.value = GRAPH_POPUP_ROW, .unit = VKR_UI_TRACK_PX};
  }
  const VkrUiTrack column = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0;
  panel.placement.row = 0;
  panel.placement.justify = VKR_UI_ALIGN_START;
  panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){popup->rect.y, 0, 0, popup->rect.x};
  panel.columns = &column;
  panel.column_count = 1;
  panel.rows = rows;
  panel.row_count = 1u + popup->visible;
  panel.style = vkr_editor_glass_style();
  panel.style.background_color.w = 1.0f;
  panel.style.padding_pt =
      (VkrUiEdges){GRAPH_POPUP_PADDING, GRAPH_POPUP_PADDING,
                   GRAPH_POPUP_PADDING, GRAPH_POPUP_PADDING};
  panel.style.gap_pt = 0.0f;
  panel.style.min_size_pt = (Vec2){popup->rect.width, popup->rect.height};
  panel.style.max_size_pt = (Vec2){popup->rect.width, popup->rect.height};
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("popup"), &panel)) {
    (void)vkr_ui_input_layer_set(ui, previous_layer);
    return;
  }
  (void)vkr_ui_push_id_label(ui, string8_lit("search"));
  view->popup_field =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("field"));
  (void)vkr_ui_pop_id(ui);
  ui->focused_id = view->popup_field;
  ui->focused_is_text = true_v;
  const uint32_t highlight = view->popup_highlight;
  bool8_t filtered = false_v;
  VkrUiPlacement placement = VKR_UI_PLACEMENT_DEFAULT;
  placement.column = 0;
  placement.row = 0;
  if (vkr_editor_search_field(ui, string8_lit("search"), &view->popup_buffer,
                              placement, string8_lit("Search nodes"),
                              (String8){0}, (VkrFontHandle){0})) {
    /* New text: filter again; rows past this frame's tracks show next frame.
     */
    const uint32_t visible = popup->visible;
    if (popup->entries) {
      popup->entry_count = graph_popup_entries(g, popup->entries);
    }
    popup->visible = Min(visible, Max(1u, popup->entry_count));
    view->popup_highlight = graph_popup_step(popup, 0, 1, 0);
    view->popup_scroll = 0;
    filtered = true_v;
  }
  view->popup_highlight =
      graph_popup_step(popup, Min(view->popup_highlight, popup->entry_count), 1,
                       graph_popup_step(popup, 0, 1, 0));
  if (g->over_popup && ui->mouse_wheel) {
    const int64_t limit = (int64_t)popup->entry_count - (int64_t)popup->visible;
    const int64_t scroll = (int64_t)view->popup_scroll - ui->mouse_wheel;
    view->popup_scroll = (uint32_t)Max(0, Min(scroll, Max(0, limit)));
  }
  uint32_t picked = graph_popup_keys(g);
  if (view->popup_open) {
    if (view->popup_highlight != highlight || filtered) {
      graph_popup_reveal(view, popup);
    }
    graph_popup_rows(g, &picked);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_input_layer_set(ui, previous_layer);
  if (view->popup_open && picked != UINT32_MAX) {
    graph_popup_pick(g, picked);
  }
}

uint32_t vkr_editor_graph_build(
    const VkrSampleUiFrame *frame, VkrEditorGraphView *view, VkrUiRect bounds,
    uint32_t layer, const VkrEditorGraphNode *nodes, uint32_t node_count,
    const VkrEditorGraphWire *wires, uint32_t wire_count,
    const VkrEditorGraphChoice *choices, uint32_t choice_count,
    VkrEditorGraphEvent *events, uint32_t event_capacity) {
  /* Layout margins cannot be negative, so the canvas cannot start left of
     or above the window. */
  if (!frame || !frame->ui || !frame->input || !view ||
      !vkr_ui_rect_is_finite(bounds) || bounds.x < 0.0f || bounds.y < 0.0f ||
      bounds.width < 1.0f || bounds.height < 1.0f) {
    return 0;
  }
  VkrUiSystem *ui = frame->ui;
  if (!isfinite(view->zoom) || view->zoom <= 0.0f) {
    view->zoom = 1.0f;
  }
  view->zoom = vkr_clamp_f32(view->zoom, VKR_EDITOR_GRAPH_ZOOM_MIN,
                             VKR_EDITOR_GRAPH_ZOOM_MAX);
  if (!isfinite(view->pan.x) || !isfinite(view->pan.y)) {
    view->pan = (Vec2){0};
  }
  GraphFrame g = {
      .frame = frame,
      .ui = ui,
      .view = view,
      .bounds = bounds,
      .nodes = nodes,
      .node_count = nodes ? node_count : 0,
      .wires = wires,
      .wire_count = wires ? wire_count : 0,
      .choices = choices,
      .choice_count = choices ? choice_count : 0,
      .events = events,
      .event_capacity = events ? event_capacity : 0,
      .mouse = {(float32_t)ui->mouse_x / ui->content_scale,
                (float32_t)ui->mouse_y / ui->content_scale},
      .hidden_node = VKR_EDITOR_GRAPH_NONE,
  };
  if (!vkr_ui_push_id_pointer(ui, view)) {
    return 0;
  }

  /* The popup claims its rectangle before any hit test, so the pointer over
     it is not over the canvas. */
  if (view->popup_open) {
    graph_popup_layout(&g);
    const float32_t scale = ui->content_scale;
    const VkrUiRect rect = g.popup.rect;
    (void)vkr_ui_input_layer_register(
        ui, VKR_EDITOR_POPUP_LAYER,
        (VkrUiRect){rect.x * scale, rect.y * scale, rect.width * scale,
                    rect.height * scale});
    g.over_popup = ui->mouse_input_layer == VKR_EDITOR_POPUP_LAYER &&
                   graph_point_in(rect, g.mouse);
  }
  g.hover = !ui->mouse_captured && ui->mouse_input_layer == layer &&
            graph_point_in(bounds, g.mouse);

  graph_prune_selection(&g);
  graph_pointer(&g);
  graph_keys(&g);
  if (view->drag == GRAPH_DRAG_WIRE && view->drag_moved &&
      view->wire_detached) {
    g.hidden_node = graph_find(g.nodes, g.node_count, view->wire_detach_node);
    g.hidden_port = view->wire_detach_port;
  }

  graph_draw(&g);
  /* A popup opened by this frame's input is laid out for its first build;
     its rectangle claims input from the next frame. */
  if (view->popup_open && !g.popup.laid_out) {
    graph_popup_layout(&g);
  }
  graph_popup_build(&g);
  (void)vkr_ui_pop_id(ui);
  return g.event_count;
}

void vkr_editor_graph_frame(VkrEditorGraphView *view, VkrUiRect bounds,
                            const VkrEditorGraphNode *nodes,
                            uint32_t node_count, bool8_t selection_only) {
  if (!view || !nodes || !node_count || !(bounds.width > 0.0f) ||
      !(bounds.height > 0.0f)) {
    return;
  }
  bool8_t only = selection_only && view->selected_count;
  Vec2 low = {0};
  Vec2 high = {0};
  bool8_t any = false_v;
  for (uint32_t pass = 0; pass < 2u && !any; ++pass) {
    for (uint32_t i = 0; i < node_count; ++i) {
      if (only && !vkr_editor_graph_selected(view, nodes[i].id)) {
        continue;
      }
      const Vec2 corner = nodes[i].position;
      const Vec2 extent = {corner.x + GRAPH_CARD_WIDTH,
                           corner.y + graph_card_height(&nodes[i])};
      if (!any) {
        low = corner;
        high = extent;
        any = true_v;
        continue;
      }
      low = (Vec2){Min(low.x, corner.x), Min(low.y, corner.y)};
      high = (Vec2){Max(high.x, extent.x), Max(high.y, extent.y)};
    }
    only = false_v;
  }
  if (!any) {
    return;
  }
  const Vec2 size = {Max(1.0f, high.x - low.x), Max(1.0f, high.y - low.y)};
  const float32_t room_x = Max(1.0f, bounds.width - GRAPH_FRAME_MARGIN * 2.0f);
  const float32_t room_y = Max(1.0f, bounds.height - GRAPH_FRAME_MARGIN * 2.0f);
  const float32_t zoom =
      vkr_clamp_f32(Min(1.0f, Min(room_x / size.x, room_y / size.y)),
                    VKR_EDITOR_GRAPH_ZOOM_MIN, VKR_EDITOR_GRAPH_ZOOM_MAX);
  const Vec2 center = {(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f};
  view->zoom = zoom;
  view->pan = (Vec2){center.x - bounds.width * 0.5f / zoom,
                     center.y - bounds.height * 0.5f / zoom};
}
