#include "editor_internal.h"
#include "editor_ui.h"

#include "containers/vkr_hashtable.h"
#include "containers/vkr_sort.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include "vkr_rg_debug.h"

/* The Render graph window: the passes and resources of the Scene
   renderer's last compiled graph, the reads and writes of the selected
   one, and the graph's memory. A description joins the render thread, so
   the window copies one into its rows four times a second while it shows,
   or on Refresh while paused, and every build reads the copy. */

#define RENDER_GRAPH_REFRESH_SECONDS 0.25
#define RENDER_GRAPH_TITLE_PT 18.0f
#define RENDER_GRAPH_BAR_PT 6.0f
#define RENDER_GRAPH_SEARCH_PT 200.0f
#define RENDER_GRAPH_PAD_X_PT 12.0f
#define RENDER_GRAPH_PAD_Y_PT 10.0f
/* Rows kept above a selection scrolled into view. */
#define RENDER_GRAPH_REVEAL_ROWS 3.0f

/* How a pass uses a resource, from VkrRgDescribedUse::role. */
typedef enum RenderGraphRole {
  RENDER_GRAPH_ROLE_COLOR = 0,
  RENDER_GRAPH_ROLE_DEPTH,
  RENDER_GRAPH_ROLE_DEPTH_READ,
  RENDER_GRAPH_ROLE_READ,
  RENDER_GRAPH_ROLE_WRITE,
  RENDER_GRAPH_ROLE_COUNT,
} RenderGraphRole;

static const char *const s_role_tokens[RENDER_GRAPH_ROLE_COUNT] = {
    "color", "depth", "depth_read", "read", "write"};

static const char *const s_role_names[RENDER_GRAPH_ROLE_COUNT] = {
    "Color attachment", "Depth attachment", "Depth (read-only)", "Read",
    "Write"};

enum {
  RENDER_GRAPH_PASS_ORDER = 0,
  RENDER_GRAPH_PASS_NAME,
  RENDER_GRAPH_PASS_KIND,
  RENDER_GRAPH_PASS_CPU,
  RENDER_GRAPH_PASS_GPU,
  RENDER_GRAPH_PASS_READS,
  RENDER_GRAPH_PASS_WRITES,
  RENDER_GRAPH_PASS_COLUMN_COUNT,
};

enum {
  RENDER_GRAPH_RESOURCE_NAME = 0,
  RENDER_GRAPH_RESOURCE_KIND,
  RENDER_GRAPH_RESOURCE_SIZE,
  RENDER_GRAPH_RESOURCE_FORMAT,
  RENDER_GRAPH_RESOURCE_BYTES,
  RENDER_GRAPH_RESOURCE_LIFETIME,
  RENDER_GRAPH_RESOURCE_USES,
  RENDER_GRAPH_RESOURCE_COLUMN_COUNT,
};

/* The uses of the selected pass, and the passes of the selected
   resource. */
enum {
  RENDER_GRAPH_USE_RESOURCE = 0,
  RENDER_GRAPH_USE_ACCESS,
  RENDER_GRAPH_USE_ROLE,
  RENDER_GRAPH_USE_KIND,
  RENDER_GRAPH_USE_BYTES,
  RENDER_GRAPH_USE_COLUMN_COUNT,
};

enum {
  RENDER_GRAPH_USER_ORDER = 0,
  RENDER_GRAPH_USER_PASS,
  RENDER_GRAPH_USER_ACCESS,
  RENDER_GRAPH_USER_ROLE,
  RENDER_GRAPH_USER_KIND,
  RENDER_GRAPH_USER_COLUMN_COUNT,
};

static const VkrEditorTableColumn s_pass_columns[] = {
    {.title = "#",
     .width_pt = 40.0f,
     .min_pt = 32.0f,
     .numeric = true_v,
     .tooltip = "Execution order; culled passes have none and come last"},
    {.title = "Pass", .min_pt = 120.0f},
    {.title = "Kind", .width_pt = 76.0f, .min_pt = 48.0f},
    {.title = "CPU ms",
     .width_pt = 66.0f,
     .min_pt = 48.0f,
     .numeric = true_v,
     .tooltip = "CPU encode ms of the newest collected frame at the refresh"},
    {.title = "GPU ms",
     .width_pt = 66.0f,
     .min_pt = 48.0f,
     .numeric = true_v,
     .tooltip = "GPU ms of the newest collected frame; blank when the pass "
                "was not timed"},
    {.title = "Reads", .width_pt = 56.0f, .min_pt = 40.0f, .numeric = true_v},
    {.title = "Writes", .width_pt = 56.0f, .min_pt = 40.0f, .numeric = true_v},
};

static const VkrEditorTableColumn s_resource_columns[] = {
    {.title = "Resource", .min_pt = 120.0f},
    {.title = "Kind", .width_pt = 62.0f, .min_pt = 44.0f},
    {.title = "Size",
     .width_pt = 128.0f,
     .min_pt = 56.0f,
     .tooltip = "Width x height, then the depth or layer count above one"},
    {.title = "Format", .width_pt = 156.0f, .min_pt = 56.0f},
    {.title = "Memory",
     .width_pt = 76.0f,
     .min_pt = 48.0f,
     .numeric = true_v,
     .tooltip = "One instance over every mip, layer and sample; blank for a "
                "block-compressed or unknown format"},
    {.title = "Lifetime", .width_pt = 104.0f, .min_pt = 56.0f},
    {.title = "Passes",
     .width_pt = 68.0f,
     .min_pt = 48.0f,
     .numeric = true_v,
     .tooltip = "Execution order of the first and last pass that uses it"},
};

static const VkrEditorTableColumn s_use_columns[] = {
    {.title = "Resource", .min_pt = 120.0f},
    {.title = "Access", .width_pt = 64.0f, .min_pt = 44.0f},
    {.title = "Role", .width_pt = 124.0f, .min_pt = 56.0f},
    {.title = "Kind", .width_pt = 62.0f, .min_pt = 44.0f},
    {.title = "Memory", .width_pt = 76.0f, .min_pt = 48.0f, .numeric = true_v},
};

static const VkrEditorTableColumn s_user_columns[] = {
    {.title = "#",
     .width_pt = 40.0f,
     .min_pt = 32.0f,
     .numeric = true_v,
     .tooltip = "Execution order"},
    {.title = "Pass", .min_pt = 120.0f},
    {.title = "Access", .width_pt = 64.0f, .min_pt = 44.0f},
    {.title = "Role", .width_pt = 124.0f, .min_pt = 56.0f},
    {.title = "Kind", .width_pt = 76.0f, .min_pt = 48.0f},
};

/* ------------------------------------------------------------------------ */
/* Copying a description                                                     */

typedef struct RenderGraphVisit {
  VkrEditorRenderGraph *graph;
  const VkrRendererMetricsPassTable *timings;
} RenderGraphVisit;

/* A graph name cut to the row's name buffer. */
vkr_internal void render_graph_copy_name(char *out, String8 name) {
  const uint64_t length =
      Min(name.length, (uint64_t)(VKR_EDITOR_RENDER_GRAPH_NAME_MAX - 1u));
  MemCopy(out, name.str, length);
  out[length] = '\0';
}

/* A description token as a label: "per_frame_slot" reads "Per frame
   slot". */
vkr_internal void render_graph_copy_token(char *out, uint64_t capacity,
                                          const char *token) {
  string_format(out, capacity, "%s", token ? token : "");
  for (char *c = out; *c; ++c) {
    if (*c == '_') {
      *c = ' ';
    }
  }
  if (out[0] >= 'a' && out[0] <= 'z') {
    out[0] = (char)(out[0] - 'a' + 'A');
  }
}

vkr_internal uint8_t render_graph_role(const char *token) {
  for (uint32_t i = 0u; token && i < RENDER_GRAPH_ROLE_COUNT; ++i) {
    if (string_equals(token, s_role_tokens[i])) {
      return (uint8_t)i;
    }
  }
  return RENDER_GRAPH_ROLE_READ;
}

vkr_internal void render_graph_visit_pass(void *user,
                                          const VkrRgDescribedPass *pass) {
  RenderGraphVisit *visit = user;
  VkrEditorRenderGraph *graph = visit->graph;
  graph->pass_seen += 1u;
  if (graph->pass_count >= VKR_EDITOR_RENDER_GRAPH_ROW_MAX) {
    return;
  }
  const uint32_t index = graph->pass_count++;
  VkrEditorRenderGraphPass *row = &graph->passes[index];
  *row = (VkrEditorRenderGraphPass){
      .order = pass->order,
      .use_first = graph->use_count,
      .uses_truncated = pass->use_total > pass->use_count,
      .culled = pass->culled,
      .disabled = pass->disabled,
  };
  render_graph_copy_name(row->name, pass->name);
  render_graph_copy_token(row->kind, sizeof(row->kind), pass->type_name);

  /* Uses resolve to resource rows once the resources are visited, which
     happens after every pass. */
  for (uint32_t i = 0u; i < pass->use_count; ++i) {
    const VkrRgDescribedUse *use = &pass->uses[i];
    if (use->write) {
      row->writes += 1u;
    } else {
      row->reads += 1u;
    }
    if (graph->use_count >= VKR_EDITOR_RENDER_GRAPH_USE_MAX) {
      graph->uses_capped = true_v;
      continue;
    }
    graph->uses[graph->use_count++] = (VkrEditorRenderGraphUse){
        .resource_hash = vkr_hash_table_key_string8(use->resource).hash,
        .pass = (uint16_t)index,
        .resource = UINT16_MAX,
        .role = render_graph_role(use->role),
        .buffer = use->buffer,
        .write = use->write,
    };
    row->use_count += 1u;
  }

  /* Timings of the newest collected frame, which may be a frame older or
     newer than this build; a culled pass has none. */
  const VkrRendererMetricsPassSample *timing =
      pass->culled ? NULL
                   : vkr_renderer_metrics_pass_find(visit->timings, pass->name);
  if (timing && !timing->culled && !timing->disabled) {
    row->timed = true_v;
    row->cpu_ms = (float32_t)timing->cpu_ms;
    row->gpu_valid = timing->gpu_valid;
    row->gpu_ms = (float32_t)timing->gpu_ms;
  }
}

vkr_internal void
render_graph_visit_resource(void *user,
                            const VkrRgDescribedResource *resource) {
  RenderGraphVisit *visit = user;
  VkrEditorRenderGraph *graph = visit->graph;
  /* Resources of earlier frames stay in the graph undeclared. */
  if (!resource->declared) {
    return;
  }
  graph->resource_seen += 1u;
  if (graph->resource_count >= VKR_EDITOR_RENDER_GRAPH_ROW_MAX) {
    return;
  }
  const uint32_t index = graph->resource_count++;
  VkrEditorRenderGraphResource *row = &graph->resources[index];
  *row = (VkrEditorRenderGraphResource){
      .name_hash = vkr_hash_table_key_string8(resource->name).hash,
      .bytes = resource->bytes,
      .width = resource->width,
      .height = resource->height,
      .depth = resource->depth,
      .layers = resource->layers,
      .mip_levels = resource->mip_levels,
      .samples = resource->samples,
      .first_order = resource->first_order,
      .last_order = resource->last_order,
      .buffer = resource->buffer,
      .imported = resource->imported,
      .exported = resource->exported,
      .present = resource->present,
  };
  render_graph_copy_name(row->name, resource->name);
  render_graph_copy_token(row->lifetime, sizeof(row->lifetime),
                          resource->lifetime);
  if (!resource->buffer) {
    if (resource->format_name) {
      string_format(row->format, sizeof(row->format), "%s",
                    resource->format_name);
    } else {
      string_format(row->format, sizeof(row->format), "Format %u",
                    (uint32_t)resource->format);
    }
  }

  /* Images and buffers have separate names, so the kind joins the name. */
  for (uint32_t i = 0u; i < graph->use_count; ++i) {
    VkrEditorRenderGraphUse *use = &graph->uses[i];
    if (use->resource == UINT16_MAX && use->buffer == row->buffer &&
        use->resource_hash == row->name_hash) {
      use->resource = (uint16_t)index;
    }
  }
}

/* Copies the Scene renderer's graph into the rows, keeping the selection
   by name, and with `dot_path` also writes it as DOT. False without a
   graph; `out_dot_written` says the DOT file was written. */
vkr_internal bool8_t render_graph_refresh(VkrEditorRenderGraph *graph,
                                          const VkrSampleUiFrame *frame,
                                          const char *dot_path,
                                          bool8_t *out_dot_written) {
  graph->refreshed_at = vkr_platform_get_absolute_time();
  if (out_dot_written) {
    *out_dot_written = false_v;
  }
  if (!frame->describe_graph) {
    graph->has_graph = false_v;
    return false_v;
  }
  char pass_name[VKR_EDITOR_RENDER_GRAPH_NAME_MAX] = {0};
  char resource_name[VKR_EDITOR_RENDER_GRAPH_NAME_MAX] = {0};
  bool8_t resource_buffer = false_v;
  if (graph->selected_pass < graph->pass_count) {
    MemCopy(pass_name, graph->passes[graph->selected_pass].name,
            sizeof(pass_name));
  }
  if (graph->selected_resource < graph->resource_count) {
    const VkrEditorRenderGraphResource *row =
        &graph->resources[graph->selected_resource];
    MemCopy(resource_name, row->name, sizeof(resource_name));
    resource_buffer = row->buffer;
  }

  graph->pass_count = 0u;
  graph->resource_count = 0u;
  graph->use_count = 0u;
  graph->pass_seen = 0u;
  graph->resource_seen = 0u;
  graph->uses_capped = false_v;
  RenderGraphVisit visit = {
      .graph = graph,
      .timings = frame->pass_table,
  };
  const VkrRgDescribeVisitor visitor = {
      .user = &visit,
      .pass = render_graph_visit_pass,
      .resource = render_graph_visit_resource,
  };
  VkrRgDescription description = {0};
  VkrRendererGraphInfo info = {0};
  graph->has_graph = frame->describe_graph(
      frame->describe_graph_context, &visitor, dot_path, &description, &info);
  graph->view_dirty = true_v;
  if (!graph->has_graph) {
    graph->pass_count = 0u;
    graph->resource_count = 0u;
    graph->use_count = 0u;
    return false_v;
  }
  if (out_dot_written) {
    *out_dot_written = info.dot_written;
  }

  render_graph_copy_token(graph->backend, sizeof(graph->backend),
                          info.backend ? info.backend : "unknown");
  render_graph_copy_token(graph->pipeline, sizeof(graph->pipeline),
                          info.pipeline);
  graph->compiled = description.compiled;
  graph->frame_index = description.frame_index;
  graph->viewport_width = description.viewport_width;
  graph->viewport_height = description.viewport_height;
  graph->render_scale = description.render_scale;
  graph->scheduled_count = description.scheduled_count;
  graph->culled_count = description.culled_count;
  graph->image_count = description.declared_image_count;
  graph->buffer_count = description.declared_buffer_count;
  graph->declared_image_bytes = description.declared_image_bytes;
  graph->declared_buffer_bytes = description.declared_buffer_bytes;
  graph->live_valid = info.live_valid;
  graph->live_images = info.live_images;
  graph->live_buffers = info.live_buffers;
  graph->live_image_bytes = info.live_image_bytes;
  graph->live_buffer_bytes = info.live_buffer_bytes;
  graph->timings_frame =
      frame->pass_table ? frame->pass_table->cpu_frame_index : 0u;

  graph->selected_pass = UINT32_MAX;
  for (uint32_t i = 0u; pass_name[0] && i < graph->pass_count; ++i) {
    if (string_equals(graph->passes[i].name, pass_name)) {
      graph->selected_pass = i;
      break;
    }
  }
  graph->selected_resource = UINT32_MAX;
  for (uint32_t i = 0u; resource_name[0] && i < graph->resource_count; ++i) {
    if (graph->resources[i].buffer == resource_buffer &&
        string_equals(graph->resources[i].name, resource_name)) {
      graph->selected_resource = i;
      break;
    }
  }
  return true_v;
}

/* Writes the graph as render_graph.dot in the recordings directory and
   tells the designer where. */
vkr_internal void render_graph_export_dot(VkrEditorUi *editor,
                                          const VkrSampleUiFrame *frame) {
  const VkrUiTheme *theme = vkr_ui_theme();
  char directory[VKR_RECORDING_PATH_MAX];
  if (!vkr_editor_recording_directory(editor, directory, sizeof(directory))) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                     "No directory for the DOT file");
    return;
  }
  const FilePath directory_path = file_path_view(directory);
  if (!file_create_directories(&directory_path)) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, theme->warning,
                     "Could not create the recordings directory");
    return;
  }
  char path[VKR_RECORDING_PATH_MAX + 32u];
  string_format(path, sizeof(path), "%s/render_graph.dot", directory);
  bool8_t written = false_v;
  const bool8_t described =
      render_graph_refresh(&editor->render_graph, frame, path, &written);
  char message[VKR_RECORDING_PATH_MAX + 64u];
  if (!described) {
    string_format(message, sizeof(message),
                  "The Scene renderer has no render graph yet");
  } else if (!written) {
    string_format(message, sizeof(message), "Could not write %s", path);
  } else {
    string_format(message, sizeof(message), "Wrote %s", path);
    log_info("Render graph written as DOT to %s", path);
  }
  vkr_editor_toast(editor,
                   written ? VKR_UI_ICON_CHECK : VKR_UI_ICON_WARNING_FILL,
                   written ? theme->success : theme->warning, message);
}

/* ------------------------------------------------------------------------ */
/* Cells                                                                     */

static const String8 s_dash = {.str = (uint8_t *)"\xe2\x80\x94", .length = 3u};

vkr_internal VkrUiPlacement render_graph_cell(uint32_t column, uint32_t row) {
  return (VkrUiPlacement){.column = column,
                          .row = row,
                          .column_span = 1u,
                          .row_span = 1u,
                          .justify = VKR_UI_ALIGN_STRETCH,
                          .align = VKR_UI_ALIGN_STRETCH};
}

vkr_internal VkrUiPanelConfig render_graph_grid(VkrUiPlacement placement,
                                                const VkrUiTrack *columns,
                                                uint32_t column_count,
                                                const VkrUiTrack *rows,
                                                uint32_t row_count) {
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = placement;
  panel.columns = columns;
  panel.column_count = column_count;
  panel.rows = rows;
  panel.row_count = row_count;
  panel.style.padding_pt = (VkrUiEdges){0};
  return panel;
}

vkr_internal void render_graph_caption(VkrUiSystem *ui, const char *id,
                                       String8 text, VkrUiPlacement placement,
                                       Vec4 color, String8 tooltip) {
  VkrUiWidgetConfig label =
      vkr_editor_text_config(vkr_ui_theme()->font_caption, color);
  label.placement = placement;
  label.placement.justify = VKR_UI_ALIGN_START;
  label.placement.align = VKR_UI_ALIGN_CENTER;
  label.tooltip = tooltip;
  vkr_ui_label(ui,
               string8_create_from_cstr((const uint8_t *)id, string_length(id)),
               text, &label);
}

vkr_internal String8 render_graph_text(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, string_length(text));
}

/* Bytes in binary multiples; zero is unknown. */
vkr_internal String8 render_graph_bytes(VkrAllocator *allocator,
                                        uint64_t bytes) {
  if (bytes == 0u) {
    return s_dash;
  }
  if (bytes >= 1024u * 1024u) {
    return string8_create_formatted(allocator, "%.1f MB",
                                    (float64_t)bytes / (1024.0 * 1024.0));
  }
  return string8_create_formatted(allocator, "%.1f KB",
                                  (float64_t)bytes / 1024.0);
}

vkr_internal String8 render_graph_order(VkrAllocator *allocator,
                                        uint32_t order) {
  return order == UINT32_MAX ? s_dash
                             : string8_create_formatted(allocator, "%u", order);
}

/* An image's extent: width x height, then the depth or layer count above
   one. */
vkr_internal String8 render_graph_extent(
    VkrAllocator *allocator, const VkrEditorRenderGraphResource *resource) {
  if (resource->buffer) {
    return s_dash;
  }
  if (resource->depth > 1u) {
    return string8_create_formatted(allocator, "%u\xc3\x97%u\xc3\x97%u",
                                    resource->width, resource->height,
                                    resource->depth);
  }
  if (resource->layers > 1u) {
    return string8_create_formatted(allocator, "%u\xc3\x97%u, %u layers",
                                    resource->width, resource->height,
                                    resource->layers);
  }
  return string8_create_formatted(allocator, "%u\xc3\x97%u", resource->width,
                                  resource->height);
}

/* ------------------------------------------------------------------------ */
/* Filter and sort                                                           */

typedef struct RenderGraphSortItem {
  float64_t key;
  const char *name;
  uint32_t index;
} RenderGraphSortItem;

vkr_internal int32_t render_graph_compare(const void *lhs, const void *rhs) {
  const RenderGraphSortItem *a = lhs;
  const RenderGraphSortItem *b = rhs;
  if (a->name && b->name) {
    const int32_t order = string_compare(a->name, b->name);
    if (order != 0) {
      return order;
    }
  } else if (a->key != b->key) {
    return a->key < b->key ? -1 : 1;
  }
  return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

/* Whether `name` holds the filter text, ignoring ASCII case. */
vkr_internal bool8_t render_graph_matches(const VkrEditorRenderGraph *graph,
                                          const char *name) {
  const uint32_t length = graph->search_length;
  if (length == 0u) {
    return true_v;
  }
  const uint64_t name_length = string_length(name);
  for (uint64_t start = 0u; start + length <= name_length; ++start) {
    if (string_n_equalsi(name + start, (const char *)graph->search, length)) {
      return true_v;
    }
  }
  return false_v;
}

/* What pass `row` sorts by in `column`, or its name; culled passes and
   untimed values sort below every value. */
vkr_internal RenderGraphSortItem render_graph_pass_key(
    const VkrEditorRenderGraphPass *row, uint32_t column, uint32_t index) {
  RenderGraphSortItem item = {.index = index};
  switch (column) {
  case RENDER_GRAPH_PASS_NAME:
    item.name = row->name;
    break;
  case RENDER_GRAPH_PASS_KIND:
    item.name = row->kind;
    break;
  case RENDER_GRAPH_PASS_ORDER:
    item.key = row->order == UINT32_MAX ? 1.0e9 : (float64_t)row->order;
    break;
  case RENDER_GRAPH_PASS_CPU:
    item.key = row->timed ? (float64_t)row->cpu_ms : -1.0;
    break;
  case RENDER_GRAPH_PASS_GPU:
    item.key = row->timed && row->gpu_valid ? (float64_t)row->gpu_ms : -1.0;
    break;
  case RENDER_GRAPH_PASS_READS:
    item.key = (float64_t)row->reads;
    break;
  case RENDER_GRAPH_PASS_WRITES:
    item.key = (float64_t)row->writes;
    break;
  default:
    break;
  }
  return item;
}

vkr_internal RenderGraphSortItem render_graph_resource_key(
    const VkrEditorRenderGraphResource *row, uint32_t column, uint32_t index) {
  RenderGraphSortItem item = {.index = index};
  switch (column) {
  case RENDER_GRAPH_RESOURCE_NAME:
    item.name = row->name;
    break;
  case RENDER_GRAPH_RESOURCE_KIND:
    item.key = row->buffer ? 1.0 : 0.0;
    break;
  case RENDER_GRAPH_RESOURCE_SIZE:
    item.key = (float64_t)row->width * (float64_t)row->height *
               (float64_t)Max(row->depth, row->layers);
    break;
  case RENDER_GRAPH_RESOURCE_FORMAT:
    item.name = row->format;
    break;
  case RENDER_GRAPH_RESOURCE_BYTES:
    item.key = (float64_t)row->bytes;
    break;
  case RENDER_GRAPH_RESOURCE_LIFETIME:
    item.name = row->lifetime;
    break;
  case RENDER_GRAPH_RESOURCE_USES:
    item.key =
        row->first_order == UINT32_MAX ? 1.0e9 : (float64_t)row->first_order;
    break;
  default:
    break;
  }
  return item;
}

/* The shown list's rows: those whose name holds the filter, in the
   table's sort or the graph's order. */
vkr_internal void render_graph_update_view(VkrEditorRenderGraph *graph) {
  const VkrEditorTable *table =
      graph->show_resources ? &graph->resource_table : &graph->pass_table;
  RenderGraphSortItem items[VKR_EDITOR_RENDER_GRAPH_ROW_MAX];
  uint32_t count = 0u;
  const uint32_t rows =
      graph->show_resources ? graph->resource_count : graph->pass_count;
  for (uint32_t i = 0u; i < rows; ++i) {
    const char *name = graph->show_resources ? graph->resources[i].name
                                             : graph->passes[i].name;
    if (!render_graph_matches(graph, name)) {
      continue;
    }
    items[count++] =
        graph->show_resources
            ? render_graph_resource_key(&graph->resources[i],
                                        table->sort_column, i)
            : render_graph_pass_key(&graph->passes[i], table->sort_column, i);
  }
  if (table->sort_column != VKR_EDITOR_TABLE_NONE) {
    vkr_sort(items, count, sizeof(items[0]), render_graph_compare);
    if (table->sort_descending) {
      for (uint32_t i = 0u; i < count / 2u; ++i) {
        const RenderGraphSortItem swap = items[i];
        items[i] = items[count - 1u - i];
        items[count - 1u - i] = swap;
      }
    }
  }
  for (uint32_t i = 0u; i < count; ++i) {
    graph->view_rows[i] = (uint16_t)items[i].index;
  }
  graph->view_count = count;
  graph->view_dirty = false_v;
}

/* Scrolls `table` so the shown list's row `selected` is in view. */
vkr_internal void render_graph_reveal(VkrEditorRenderGraph *graph,
                                      VkrEditorTable *table,
                                      uint32_t selected) {
  graph->reveal = false_v;
  for (uint32_t i = 0u; i < graph->view_count; ++i) {
    if (graph->view_rows[i] == selected) {
      table->scroll_pt = Max(0.0f, ((float32_t)i - RENDER_GRAPH_REVEAL_ROWS) *
                                       VKR_EDITOR_TABLE_ROW_PT);
      return;
    }
  }
}

/* Shows the passes or the resources with `row` selected and in view; a
   filter that hides the row is cleared. */
vkr_internal void render_graph_jump(VkrEditorRenderGraph *graph,
                                    bool8_t resources, uint32_t row) {
  graph->show_resources = resources;
  if (resources) {
    graph->selected_resource = row;
  } else {
    graph->selected_pass = row;
  }
  const char *name =
      resources ? graph->resources[row].name : graph->passes[row].name;
  if (!render_graph_matches(graph, name)) {
    graph->search_length = 0u;
    graph->search[0] = '\0';
  }
  graph->view_dirty = true_v;
  graph->reveal = true_v;
}

/* ------------------------------------------------------------------------ */
/* Lists                                                                     */

vkr_internal void render_graph_pass_cells(VkrAllocator *allocator,
                                          const VkrEditorRenderGraphPass *row,
                                          String8 *cells) {
  cells[RENDER_GRAPH_PASS_ORDER] = render_graph_order(allocator, row->order);
  cells[RENDER_GRAPH_PASS_NAME] = render_graph_text(row->name);
  cells[RENDER_GRAPH_PASS_KIND] = render_graph_text(row->kind);
  cells[RENDER_GRAPH_PASS_CPU] =
      row->timed ? string8_create_formatted(allocator, "%.3f", row->cpu_ms)
                 : s_dash;
  cells[RENDER_GRAPH_PASS_GPU] =
      row->timed && row->gpu_valid
          ? string8_create_formatted(allocator, "%.3f", row->gpu_ms)
          : s_dash;
  cells[RENDER_GRAPH_PASS_READS] =
      string8_create_formatted(allocator, "%u", row->reads);
  cells[RENDER_GRAPH_PASS_WRITES] = string8_create_formatted(
      allocator, "%u%s", row->writes, row->uses_truncated ? "+" : "");
}

vkr_internal String8 render_graph_pass_tooltip(
    VkrAllocator *allocator, const VkrEditorRenderGraphPass *row) {
  const char *state = row->disabled ? "  \xc2\xb7  disabled"
                      : row->culled ? "  \xc2\xb7  culled: nothing reads its "
                                      "outputs"
                                    : "";
  return string8_create_formatted(
      allocator, "%s  \xc2\xb7  %s pass%s%s", row->name, row->kind, state,
      row->uses_truncated ? "  \xc2\xb7  more uses than the description lists"
                          : "");
}

vkr_internal void render_graph_pass_list(VkrEditorRenderGraph *graph,
                                         VkrUiSystem *ui,
                                         VkrUiPlacement placement) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrEditorTable *table = &graph->pass_table;
  if (graph->reveal) {
    render_graph_reveal(graph, table, graph->selected_pass);
  }
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "passes", table, s_pass_columns,
                              RENDER_GRAPH_PASS_COLUMN_COUNT, graph->view_count,
                              placement, &sorted, &first, &end)) {
    return;
  }
  if (sorted) {
    render_graph_update_view(graph);
  }
  table->selected_key =
      graph->selected_pass == UINT32_MAX ? UINT64_MAX : graph->selected_pass;
  for (uint32_t r = first; r < end; ++r) {
    const uint32_t index = graph->view_rows[r];
    const VkrEditorRenderGraphPass *pass = &graph->passes[index];
    String8 cells[RENDER_GRAPH_PASS_COLUMN_COUNT];
    render_graph_pass_cells(ui->frame_allocator, pass, cells);
    const VkrEditorTableRow row = {
        .key = index,
        .cells = cells,
        .text_color = pass->culled ? theme->text_disabled : (Vec4){0},
        .tooltip = render_graph_pass_tooltip(ui->frame_allocator, pass),
    };
    if (vkr_editor_table_row(ui, table, s_pass_columns,
                             RENDER_GRAPH_PASS_COLUMN_COUNT, r, &row)) {
      graph->selected_pass = index;
    }
  }
  vkr_editor_table_end(ui);
}

vkr_internal String8 render_graph_resource_tooltip(
    VkrAllocator *allocator, const VkrEditorRenderGraphResource *row) {
  if (row->buffer) {
    return string8_create_formatted(
        allocator, "%s  \xc2\xb7  buffer of %llu bytes  \xc2\xb7  %s%s%s",
        row->name, (unsigned long long)row->bytes, row->lifetime,
        row->imported ? ", imported" : "", row->exported ? ", exported" : "");
  }
  return string8_create_formatted(
      allocator,
      "%s  \xc2\xb7  %u\xc3\x97%u\xc3\x97%u, %u layers, %u mips, %u "
      "samples  \xc2\xb7  %s%s%s%s",
      row->name, row->width, row->height, row->depth, row->layers,
      row->mip_levels, row->samples, row->lifetime,
      row->imported ? ", imported" : "", row->exported ? ", exported" : "",
      row->present ? ", presented" : "");
}

vkr_internal void render_graph_resource_list(VkrEditorRenderGraph *graph,
                                             VkrUiSystem *ui,
                                             VkrUiPlacement placement) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrAllocator *allocator = ui->frame_allocator;
  VkrEditorTable *table = &graph->resource_table;
  if (graph->reveal) {
    render_graph_reveal(graph, table, graph->selected_resource);
  }
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "resources", table, s_resource_columns,
                              RENDER_GRAPH_RESOURCE_COLUMN_COUNT,
                              graph->view_count, placement, &sorted, &first,
                              &end)) {
    return;
  }
  if (sorted) {
    render_graph_update_view(graph);
  }
  table->selected_key = graph->selected_resource == UINT32_MAX
                            ? UINT64_MAX
                            : graph->selected_resource;
  for (uint32_t r = first; r < end; ++r) {
    const uint32_t index = graph->view_rows[r];
    const VkrEditorRenderGraphResource *resource = &graph->resources[index];
    String8 cells[RENDER_GRAPH_RESOURCE_COLUMN_COUNT];
    cells[RENDER_GRAPH_RESOURCE_NAME] = render_graph_text(resource->name);
    cells[RENDER_GRAPH_RESOURCE_KIND] =
        resource->buffer ? string8_lit("Buffer") : string8_lit("Image");
    cells[RENDER_GRAPH_RESOURCE_SIZE] =
        render_graph_extent(allocator, resource);
    cells[RENDER_GRAPH_RESOURCE_FORMAT] =
        resource->buffer ? s_dash : render_graph_text(resource->format);
    cells[RENDER_GRAPH_RESOURCE_BYTES] =
        render_graph_bytes(allocator, resource->bytes);
    cells[RENDER_GRAPH_RESOURCE_LIFETIME] =
        render_graph_text(resource->lifetime);
    cells[RENDER_GRAPH_RESOURCE_USES] =
        resource->first_order == UINT32_MAX
            ? s_dash
            : string8_create_formatted(allocator, "%u\xe2\x80\x93%u",
                                       resource->first_order,
                                       resource->last_order);
    const VkrEditorTableRow row = {
        .key = index,
        .cells = cells,
        .text_color = resource->first_order == UINT32_MAX ? theme->text_disabled
                                                          : (Vec4){0},
        .tooltip = render_graph_resource_tooltip(allocator, resource),
    };
    if (vkr_editor_table_row(ui, table, s_resource_columns,
                             RENDER_GRAPH_RESOURCE_COLUMN_COUNT, r, &row)) {
      graph->selected_resource = index;
    }
  }
  vkr_editor_table_end(ui);
}

/* The selected pass's reads and writes; a click shows that resource. */
vkr_internal void render_graph_pass_uses(VkrEditorRenderGraph *graph,
                                         VkrUiSystem *ui, uint32_t title_row) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrAllocator *allocator = ui->frame_allocator;
  const VkrEditorRenderGraphPass *pass = &graph->passes[graph->selected_pass];
  render_graph_caption(ui, "uses.title",
                       string8_create_formatted(
                           allocator,
                           "%s reads %u and writes %u%s  \xc2\xb7  click "
                           "one to show it",
                           pass->name, pass->reads, pass->writes,
                           pass->uses_truncated ? " (more not listed)" : ""),
                       render_graph_cell(0u, title_row), theme->text_secondary,
                       (String8){0});
  VkrEditorTable *table = &graph->uses_table;
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "uses", table, s_use_columns,
                              RENDER_GRAPH_USE_COLUMN_COUNT, pass->use_count,
                              render_graph_cell(0u, title_row + 1u), &sorted,
                              &first, &end)) {
    return;
  }
  table->selected_key = UINT64_MAX;
  for (uint32_t r = first; r < end; ++r) {
    const VkrEditorRenderGraphUse *use = &graph->uses[pass->use_first + r];
    const bool8_t known = use->resource < graph->resource_count;
    const VkrEditorRenderGraphResource *resource =
        known ? &graph->resources[use->resource] : NULL;
    String8 cells[RENDER_GRAPH_USE_COLUMN_COUNT];
    cells[RENDER_GRAPH_USE_RESOURCE] =
        known ? render_graph_text(resource->name)
              : string8_lit("(past the resource cap)");
    cells[RENDER_GRAPH_USE_ACCESS] =
        use->write ? string8_lit("Write") : string8_lit("Read");
    cells[RENDER_GRAPH_USE_ROLE] = render_graph_text(s_role_names[use->role]);
    cells[RENDER_GRAPH_USE_KIND] =
        use->buffer ? string8_lit("Buffer") : string8_lit("Image");
    cells[RENDER_GRAPH_USE_BYTES] =
        known ? render_graph_bytes(allocator, resource->bytes) : s_dash;
    const VkrEditorTableRow row = {
        .key = r,
        .cells = cells,
        .text_color = use->write ? theme->text : theme->text_secondary,
    };
    if (vkr_editor_table_row(ui, table, s_use_columns,
                             RENDER_GRAPH_USE_COLUMN_COUNT, r, &row) &&
        known) {
      render_graph_jump(graph, true_v, use->resource);
    }
  }
  vkr_editor_table_end(ui);
}

/* The passes that read or write the selected resource, in execution
   order; a click shows that pass. */
vkr_internal void render_graph_resource_users(VkrEditorRenderGraph *graph,
                                              VkrUiSystem *ui,
                                              uint32_t title_row) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrAllocator *allocator = ui->frame_allocator;
  const uint32_t selected = graph->selected_resource;
  const VkrEditorRenderGraphResource *resource = &graph->resources[selected];
  /* Uses are stored pass by pass, so they come in the passes' order. */
  uint16_t users[VKR_EDITOR_RENDER_GRAPH_ROW_MAX];
  uint32_t count = 0u;
  uint32_t reads = 0u;
  uint32_t writes = 0u;
  for (uint32_t i = 0u;
       i < graph->use_count && count < VKR_EDITOR_RENDER_GRAPH_ROW_MAX; ++i) {
    if (graph->uses[i].resource == selected) {
      users[count++] = (uint16_t)i;
      if (graph->uses[i].write) {
        writes += 1u;
      } else {
        reads += 1u;
      }
    }
  }
  render_graph_caption(
      ui, "users.title",
      string8_create_formatted(allocator,
                               "%s: %u writes, %u reads  \xc2\xb7  click one "
                               "to show the pass",
                               resource->name, writes, reads),
      render_graph_cell(0u, title_row), theme->text_secondary, (String8){0});
  VkrEditorTable *table = &graph->users_table;
  bool8_t sorted = false_v;
  uint32_t first = 0u;
  uint32_t end = 0u;
  if (!vkr_editor_table_begin(ui, "users", table, s_user_columns,
                              RENDER_GRAPH_USER_COLUMN_COUNT, count,
                              render_graph_cell(0u, title_row + 1u), &sorted,
                              &first, &end)) {
    return;
  }
  table->selected_key = UINT64_MAX;
  for (uint32_t r = first; r < end; ++r) {
    const VkrEditorRenderGraphUse *use = &graph->uses[users[r]];
    const VkrEditorRenderGraphPass *pass = &graph->passes[use->pass];
    String8 cells[RENDER_GRAPH_USER_COLUMN_COUNT];
    cells[RENDER_GRAPH_USER_ORDER] = render_graph_order(allocator, pass->order);
    cells[RENDER_GRAPH_USER_PASS] = render_graph_text(pass->name);
    cells[RENDER_GRAPH_USER_ACCESS] =
        use->write ? string8_lit("Write") : string8_lit("Read");
    cells[RENDER_GRAPH_USER_ROLE] = render_graph_text(s_role_names[use->role]);
    cells[RENDER_GRAPH_USER_KIND] = render_graph_text(pass->kind);
    const VkrEditorTableRow row = {
        .key = r,
        .cells = cells,
        .text_color = pass->culled ? theme->text_disabled
                      : use->write ? theme->text
                                   : theme->text_secondary,
    };
    if (vkr_editor_table_row(ui, table, s_user_columns,
                             RENDER_GRAPH_USER_COLUMN_COUNT, r, &row)) {
      render_graph_jump(graph, false_v, use->pass);
    }
  }
  vkr_editor_table_end(ui);
}

/* The pane under the list: the selection's uses, or how to get them. */
vkr_internal void render_graph_details(VkrEditorRenderGraph *graph,
                                       VkrUiSystem *ui, uint32_t title_row) {
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!graph->show_resources && graph->selected_pass < graph->pass_count) {
    render_graph_pass_uses(graph, ui, title_row);
    return;
  }
  if (graph->show_resources &&
      graph->selected_resource < graph->resource_count) {
    render_graph_resource_users(graph, ui, title_row);
    return;
  }
  render_graph_caption(
      ui, "details.title",
      graph->show_resources ? string8_lit("Select a resource to list the "
                                          "passes that read and write it")
                            : string8_lit("Select a pass to list the "
                                          "resources it reads and writes"),
      render_graph_cell(0u, title_row), theme->text_secondary, (String8){0});
}

/* ------------------------------------------------------------------------ */
/* Header                                                                    */

/* The toolbar: the pipeline, the filter, the Passes and Resources switch,
   and pause, refresh and DOT export. */
vkr_internal void render_graph_toolbar(VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       uint32_t row) {
  VkrEditorRenderGraph *graph = &editor->render_graph;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack search_track = {.value = RENDER_GRAPH_SEARCH_PT,
                                   .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack columns[] = {fill,       search_track, auto_track,
                                auto_track, auto_track,   auto_track,
                                auto_track};
  VkrUiPanelConfig toolbar = render_graph_grid(
      render_graph_cell(0u, row), columns, ArrayCount(columns), &fill, 1u);
  toolbar.style.gap_pt = theme->space_sm;
  toolbar.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("toolbar"), &toolbar)) {
    return;
  }

  VkrUiWidgetConfig title =
      vkr_editor_text_config(theme->font_body, theme->text);
  title.placement = render_graph_cell(0u, 0u);
  title.placement.justify = VKR_UI_ALIGN_START;
  title.placement.align = VKR_UI_ALIGN_CENTER;
  title.text.font = editor->heading_font;
  vkr_ui_label(ui, string8_lit("title"),
               string8_create_formatted(ui->frame_allocator,
                                        "%s pipeline  \xc2\xb7  %s  \xc2\xb7  "
                                        "%u\xc3\x97%u at %.0f%%",
                                        graph->pipeline, graph->backend,
                                        graph->viewport_width,
                                        graph->viewport_height,
                                        (float64_t)graph->render_scale * 100.0),
               &title);

  VkrUiTextEditBuffer edit = {graph->search, graph->search_length,
                              sizeof(graph->search)};
  VkrUiPlacement field = render_graph_cell(1u, 0u);
  field.align = VKR_UI_ALIGN_CENTER;
  if (vkr_editor_search_field(
          ui, string8_lit("search"), &edit, field,
          graph->show_resources ? string8_lit("Filter resources")
                                : string8_lit("Filter passes"),
          string8_lit("Show the rows whose name holds this text "
                      "(case-insensitive)"),
          VKR_FONT_HANDLE_INVALID)) {
    graph->search_length = edit.length;
    graph->view_dirty = true_v;
    graph->pass_table.scroll_pt = 0.0f;
    graph->resource_table.scroll_pt = 0.0f;
  }

  const String8 tabs[] = {
      string8_create_formatted(ui->frame_allocator, "Passes  %u",
                               graph->pass_count),
      string8_create_formatted(ui->frame_allocator, "Resources  %u",
                               graph->resource_count),
  };
  static const char *const tips[] = {
      "The passes in execution order, then the culled ones",
      "The images and buffers this frame's graph declared",
  };
  for (uint32_t i = 0u; i < ArrayCount(tabs); ++i) {
    const bool8_t resources = i == 1u;
    VkrUiWidgetConfig tab = vkr_ui_widget_config_default();
    tab.placement = render_graph_cell(2u + i, 0u);
    tab.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_editor_ghost_style(&tab);
    vkr_editor_toggle_style(&tab, graph->show_resources == resources);
    tab.icon = resources ? VKR_UI_ICON_LAYERS : VKR_UI_ICON_LIST;
    vkr_editor_button_size(&tab);
    tab.tooltip = render_graph_text(tips[i]);
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("tab"), tabs[i], &tab) &&
        graph->show_resources != resources) {
      graph->show_resources = resources;
      graph->view_dirty = true_v;
      graph->reveal = true_v;
    }
    (void)vkr_ui_pop_id(ui);
  }

  VkrUiWidgetConfig pause = vkr_editor_icon_button_config(
      4u, 0u, graph->paused ? VKR_UI_ICON_PLAY : VKR_UI_ICON_PAUSE,
      graph->paused ? string8_lit("Refresh four times a second again")
                    : string8_lit("Stop refreshing; each refresh waits for "
                                  "the render thread"));
  vkr_editor_toggle_style(&pause, graph->paused);
  if (vkr_ui_button(ui, string8_lit("pause"), (String8){0}, &pause)) {
    graph->paused = !graph->paused;
  }
  VkrUiWidgetConfig refresh = vkr_editor_icon_button_config(
      5u, 0u, VKR_UI_ICON_REFRESH,
      string8_lit("Describe the Scene renderer's graph again now"));
  if (vkr_ui_button(ui, string8_lit("refresh"), (String8){0}, &refresh)) {
    (void)render_graph_refresh(graph, frame, NULL, NULL);
  }
  VkrUiWidgetConfig dot = vkr_editor_icon_button_config(
      6u, 0u, VKR_UI_ICON_EXPORT,
      string8_lit("Write the graph as Graphviz DOT to render_graph.dot in "
                  "the recordings directory"));
  if (vkr_ui_button(ui, string8_lit("dot"), (String8){0}, &dot)) {
    render_graph_export_dot(editor, frame);
  }
  (void)vkr_ui_panel_end(ui);
}

/* The line under the toolbar: the frame, the counts and the memory. */
vkr_internal void render_graph_summary(const VkrEditorRenderGraph *graph,
                                       VkrUiSystem *ui, uint32_t row) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrAllocator *allocator = ui->frame_allocator;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack columns[] = {fill, auto_track};
  VkrUiPanelConfig line = render_graph_grid(render_graph_cell(0u, row), columns,
                                            ArrayCount(columns), &fill, 1u);
  line.style.gap_pt = theme->space_md;
  line.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("summary"), &line)) {
    return;
  }
  const bool8_t capped = graph->pass_seen > graph->pass_count ||
                         graph->resource_seen > graph->resource_count ||
                         graph->uses_capped;
  /* Passes carry GPU times only while pass timing is on (F7). */
  bool8_t gpu_timed = false_v;
  for (uint32_t i = 0u; i < graph->pass_count && !gpu_timed; ++i) {
    gpu_timed = graph->passes[i].timed && graph->passes[i].gpu_valid;
  }
  render_graph_caption(
      ui, "counts",
      string8_create_formatted(
          allocator,
          "%u passes: %u scheduled, %u culled  \xc2\xb7  %u images, %u "
          "buffers%s%s",
          graph->scheduled_count + graph->culled_count, graph->scheduled_count,
          graph->culled_count, graph->image_count, graph->buffer_count,
          gpu_timed ? "" : "  \xc2\xb7  GPU pass timing off (F7)",
          capped ? "  \xc2\xb7  some rows not shown" : ""),
      render_graph_cell(0u, 0u),
      graph->compiled ? theme->text_secondary : theme->warning,
      graph->compiled
          ? string8_create_formatted(
                allocator,
                "Graph of frame %u; pass times of frame %llu, the newest "
                "collected at the refresh",
                graph->frame_index, (unsigned long long)graph->timings_frame)
          : string8_lit("The last compile failed; this is its build"));

  /* The graph declares one instance of each resource; the backend makes
     per-image and per-frame-slot copies. Graph memory never aliases. */
  const uint64_t declared =
      graph->declared_image_bytes + graph->declared_buffer_bytes;
  const String8 live =
      graph->live_valid
          ? string8_create_formatted(allocator, "live %.1f MB",
                                     (float64_t)(graph->live_image_bytes +
                                                 graph->live_buffer_bytes) /
                                         (1024.0 * 1024.0))
          : string8_lit("live not reported");
  const String8 tooltip =
      graph->live_valid
          ? string8_create_formatted(
                allocator,
                "Declared: %.1f MB of images, %.1f MB of buffers, one "
                "instance each, imports excluded.\nLive: %.1f MB in %u "
                "images, %.1f MB in %u buffers, every backend instance.",
                (float64_t)graph->declared_image_bytes / (1024.0 * 1024.0),
                (float64_t)graph->declared_buffer_bytes / (1024.0 * 1024.0),
                (float64_t)graph->live_image_bytes / (1024.0 * 1024.0),
                graph->live_images,
                (float64_t)graph->live_buffer_bytes / (1024.0 * 1024.0),
                graph->live_buffers)
          : string8_lit("Declared: one instance of every graph-owned "
                        "resource, imports excluded");
  render_graph_caption(
      ui, "memory",
      string8_create_formatted(allocator, "Memory: declared %.1f MB, %.*s",
                               (float64_t)declared / (1024.0 * 1024.0),
                               (int32_t)live.length, (const char *)live.str),
      render_graph_cell(1u, 0u), theme->text_secondary, tooltip);
  (void)vkr_ui_panel_end(ui);
}

/* No graph to show: why, and a button to look again. */
vkr_internal void render_graph_empty(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack auto_track = {.unit = VKR_UI_TRACK_AUTO};
  const VkrUiTrack rows[] = {auto_track, auto_track, auto_track, auto_track};
  VkrUiPanelConfig body =
      render_graph_grid(render_graph_cell(0u, 0u), &fill, 1u, &fill, 1u);
  if (!vkr_ui_panel_begin(ui, string8_lit("render_graph.empty"), &body)) {
    return;
  }
  VkrUiPanelConfig stack = render_graph_grid(
      render_graph_cell(0u, 0u), &auto_track, 1u, rows, ArrayCount(rows));
  stack.placement.justify = VKR_UI_ALIGN_CENTER;
  stack.placement.align = VKR_UI_ALIGN_CENTER;
  stack.style.gap_pt = theme->space_md;
  if (vkr_ui_panel_begin(ui, string8_lit("stack"), &stack)) {
    VkrUiWidgetConfig icon = vkr_ui_widget_config_default();
    icon.placement = render_graph_cell(0u, 0u);
    icon.placement.justify = VKR_UI_ALIGN_CENTER;
    icon.icon = VKR_UI_ICON_GRAPH;
    icon.icon_size_pt = 32.0f;
    icon.icon_color = theme->text_disabled;
    vkr_ui_label(ui, string8_lit("icon"), (String8){0}, &icon);

    VkrUiWidgetConfig title =
        vkr_editor_text_config(theme->font_body, theme->text);
    title.placement = render_graph_cell(0u, 1u);
    title.placement.justify = VKR_UI_ALIGN_CENTER;
    title.text.font = editor->heading_font;
    vkr_ui_label(ui, string8_lit("title"),
                 string8_lit("No render graph compiled yet"), &title);

    VkrUiWidgetConfig text =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    text.placement = render_graph_cell(0u, 2u);
    text.placement.justify = VKR_UI_ALIGN_CENTER;
    text.text.layout.word_wrap = true_v;
    text.text.layout.max_width = 360.0f;
    text.text.layout.anchor.horizontal = VKR_TEXT_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("text"),
                 frame->describe_graph
                     ? string8_lit("The Scene's passes, resources and graph "
                                   "memory show here once the Scene "
                                   "renderer has built a frame.")
                     : string8_lit("This editor has no Scene renderer to "
                                   "describe."),
                 &text);

    VkrUiWidgetConfig refresh = vkr_ui_widget_config_default();
    refresh.placement = render_graph_cell(0u, 3u);
    refresh.placement.justify = VKR_UI_ALIGN_CENTER;
    refresh.style.margin_pt.top = theme->space_sm;
    vkr_editor_action_style(&refresh, editor->heading_font);
    refresh.icon = VKR_UI_ICON_REFRESH;
    vkr_editor_button_size(&refresh);
    refresh.disabled = frame->describe_graph == NULL;
    if (vkr_ui_button(ui, string8_lit("refresh"), string8_lit("Refresh"),
                      &refresh)) {
      (void)render_graph_refresh(&editor->render_graph, frame, NULL, NULL);
    }
    (void)vkr_ui_panel_end(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_render_graph_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrUiRect bounds) {
  VkrEditorRenderGraph *graph = &editor->render_graph;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!graph->initialized) {
    graph->selected_pass = UINT32_MAX;
    graph->selected_resource = UINT32_MAX;
    /* The lists sort before a table's first build initializes it. */
    graph->pass_table.sort_column = VKR_EDITOR_TABLE_NONE;
    graph->resource_table.sort_column = VKR_EDITOR_TABLE_NONE;
    graph->split =
        (VkrEditorSplit){.ratio = 0.66f, .min_ratio = 0.3f, .max_ratio = 0.85f};
    graph->initialized = true_v;
  }
  /* Only a shown window describes the graph, at most four times a
     second. */
  const float64_t now = vkr_platform_get_absolute_time();
  if (graph->refreshed_at <= 0.0 ||
      (!graph->paused &&
       now - graph->refreshed_at >= RENDER_GRAPH_REFRESH_SECONDS)) {
    (void)render_graph_refresh(graph, frame, NULL, NULL);
  }
  if (!graph->has_graph) {
    render_graph_empty(editor, frame);
    return;
  }
  if (graph->view_dirty) {
    render_graph_update_view(graph);
  }

  const float32_t scale = ui->content_scale;
  const float32_t gap = theme->space_sm;
  const float32_t toolbar_pt = theme->control_height + 4.0f;
  const VkrUiTrack fill = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack title = {.value = RENDER_GRAPH_TITLE_PT,
                            .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack rows[] = {
      {.value = toolbar_pt, .unit = VKR_UI_TRACK_PX},
      title,
      {.value = graph->split.ratio, .unit = VKR_UI_TRACK_FR},
      {.value = RENDER_GRAPH_BAR_PT, .unit = VKR_UI_TRACK_PX},
      title,
      {.value = 1.0f - graph->split.ratio, .unit = VKR_UI_TRACK_FR},
  };
  VkrUiPanelConfig body = render_graph_grid(render_graph_cell(0u, 0u), &fill,
                                            1u, rows, ArrayCount(rows));
  body.style.padding_pt =
      (VkrUiEdges){RENDER_GRAPH_PAD_Y_PT, RENDER_GRAPH_PAD_X_PT,
                   RENDER_GRAPH_PAD_Y_PT, RENDER_GRAPH_PAD_X_PT};
  body.style.gap_pt = gap;
  body.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("render_graph.body"), &body)) {
    return;
  }
  render_graph_toolbar(editor, frame, 0u);
  render_graph_summary(graph, ui, 1u);
  if (graph->show_resources) {
    render_graph_resource_list(graph, ui, render_graph_cell(0u, 2u));
  } else {
    render_graph_pass_list(graph, ui, render_graph_cell(0u, 2u));
  }
  /* The lists' joint height: the body without padding, the fixed rows and
     the gaps. */
  const float32_t split_height =
      bounds.height / scale - 2.0f * RENDER_GRAPH_PAD_Y_PT - toolbar_pt -
      2.0f * RENDER_GRAPH_TITLE_PT - RENDER_GRAPH_BAR_PT - 5.0f * gap;
  vkr_editor_split_bar(ui, "render_graph.split", &graph->split,
                       render_graph_cell(0u, 3u), true_v, split_height);
  render_graph_details(graph, ui, 4u);
  (void)vkr_ui_panel_end(ui);
}
