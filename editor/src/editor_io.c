#include "editor_io.h"

#include "editor_agent.h"
#include "editor_details.h"
#include "editor_internal.h"

#include "renderer/systems/vkr_scene_types.h"
#include "script/vkr_io_router.h"

#include <stdio.h>
#include <string.h>

#define IO_PAD_PT 10.0f
#define IO_ROW_PT 26.0f

static String8 io_cstr(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, strlen(text));
}

static String8 io_name(const VkrScene *scene, VkrEntityId entity) {
  const String8 name = scene && vkr_scene_entity_alive(scene, entity)
                           ? vkr_scene_get_name(scene, entity)
                           : (String8){0};
  return name.length ? name : string8_lit("(unnamed)");
}

static VkrEntityId io_parent(const VkrScene *scene, VkrEntityId entity) {
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  return transform ? transform->parent : VKR_ENTITY_ID_INVALID;
}

static void io_id_text(VkrEntityId entity, char *out, uint64_t capacity) {
  snprintf(out, capacity, "%u:%u:%u", (unsigned)entity.parts.world,
           (unsigned)entity.parts.index, (unsigned)entity.parts.generation);
}

static void io_select(const VkrSampleUiFrame *frame, VkrEntityId entity) {
  *frame->scene_edit =
      (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_SELECT, .entity = entity};
}

/* A clickable row: an icon, a line of text and an optional tooltip. */
static bool8_t io_row(VkrUiSystem *ui, String8 id, float32_t width,
                      float32_t *y, VkrUiIcon icon, Vec4 icon_color,
                      String8 text, String8 tooltip) {
  VkrUiWidgetConfig row = vkr_editor_details_widget(
      IO_PAD_PT, *y, width - IO_PAD_PT * 2.0f, IO_ROW_PT - 2.0f);
  vkr_editor_ghost_style(&row);
  row.placement.align = VKR_UI_ALIGN_START;
  row.leading = true_v;
  row.icon = icon;
  row.icon_size_pt = 12.0f;
  row.icon_color = icon_color;
  row.tooltip = tooltip;
  const bool8_t pressed = vkr_ui_button(ui, id, text, &row);
  *y += IO_ROW_PT;
  return pressed;
}

static void io_note(VkrUiSystem *ui, String8 id, float32_t width, float32_t *y,
                    String8 text) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig note = vkr_editor_details_widget(
      IO_PAD_PT, *y, width - IO_PAD_PT * 2.0f, IO_ROW_PT - 4.0f);
  note.style.font_size_pt = theme->font_caption;
  note.style.text_color = theme->text_secondary;
  vkr_ui_label(ui, id, text, &note);
  *y += IO_ROW_PT - 2.0f;
}

/* "on_enter -> Door A . open (0.5 s, 1 time)" for one connection. */
static void io_connection_text(const VkrScene *scene, VkrEntityId connection,
                               bool8_t incoming, char *out, uint64_t capacity) {
  const SceneIoConnection *value =
      vkr_scene_get_typed(scene, connection, &vkr_scene_io_connection_type);
  const VkrEntityId target = vkr_scene_find_entity_ref(scene, &value->target);
  const String8 other = incoming ? io_name(scene, io_parent(scene, connection))
                        : target.u64 ? io_name(scene, target)
                                     : string8_lit("no target");
  char extra[64] = {0};
  if (value->delay > 0.0f && value->limit) {
    snprintf(extra, sizeof(extra), "  (%.2g s, %u times)", value->delay,
             value->limit);
  } else if (value->delay > 0.0f) {
    snprintf(extra, sizeof(extra), "  (%.2g s)", value->delay);
  } else if (value->limit) {
    snprintf(extra, sizeof(extra), "  (%u times)", value->limit);
  }
  if (incoming) {
    snprintf(out, capacity, "%.*s . %s  ->  %s%s", (int)other.length, other.str,
             value->output, value->input[0] ? value->input : "?", extra);
  } else {
    snprintf(out, capacity, "%s  ->  %.*s . %s%s", value->output,
             (int)other.length, other.str, value->input[0] ? value->input : "?",
             extra);
  }
}

/* The ports `entity` offers as one line, or "none". */
static void io_ports_text(const VkrScene *scene, VkrEntityId entity,
                          bool8_t inputs, char *out, uint64_t capacity) {
  uint64_t length = 0u;
  out[0] = '\0';
  for (uint32_t t = 0; t < scene->type_count && length < capacity; ++t) {
    const VkrTypeDesc *type = scene->types[t].type;
    const VkrIoPort *ports = inputs ? type->inputs : type->outputs;
    if (!ports || !vkr_scene_get_typed(scene, entity, type)) {
      continue;
    }
    for (uint32_t p = 0; ports[p].name && length < capacity; ++p) {
      length += (uint64_t)snprintf(out + length, capacity - length, "%s%s",
                                   length ? ", " : "", ports[p].name);
    }
  }
  for (uint32_t p = 0;
       inputs && vkr_io_builtin_inputs[p].name && length < capacity; ++p) {
    length +=
        (uint64_t)snprintf(out + length, capacity - length, "%s%s",
                           length ? ", " : "", vkr_io_builtin_inputs[p].name);
  }
  if (!length) {
    snprintf(out, capacity, "none");
  }
}

/* The first output `entity` fires, as "type.port", or false. */
static bool8_t io_first_output(const VkrScene *scene, VkrEntityId entity,
                               char *out, uint64_t capacity) {
  for (uint32_t t = 0; t < scene->type_count; ++t) {
    const VkrTypeDesc *type = scene->types[t].type;
    if (type->outputs && type->outputs[0].name &&
        vkr_scene_get_typed(scene, entity, type)) {
      snprintf(out, capacity, "%s", type->outputs[0].name);
      return true_v;
    }
  }
  return false_v;
}

static void io_route_section(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                             const VkrScene *scene, VkrEntityId connection,
                             float32_t width, float32_t *y,
                             VkrFontHandle heading) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!vkr_editor_details_section(
          ui, string8_lit("io.route"), width, y, VKR_UI_ICON_ARROW_RIGHT,
          (Vec4){0.98f, 0.82f, 0.35f, 1.0f}, string8_lit("Route"), heading,
          &editor->io_route_collapsed)) {
    return;
  }
  char problem[160];
  const bool8_t broken =
      vkr_io_connection_problem(scene, connection, problem, sizeof(problem));
  (void)io_row(ui, string8_lit("io.status"), width, y,
               broken ? VKR_UI_ICON_WARNING_FILL : VKR_UI_ICON_CHECK_CIRCLE,
               broken ? theme->warning : theme->success,
               broken ? io_cstr(problem)
                      : string8_lit("Routes when the game plays"),
               (String8){0});
  const VkrEntityId source = io_parent(scene, connection);
  const SceneIoConnection *value =
      vkr_scene_get_typed(scene, connection, &vkr_scene_io_connection_type);
  const VkrEntityId target = vkr_scene_find_entity_ref(scene, &value->target);
  char text[320];
  snprintf(text, sizeof(text), "Source: %.*s",
           (int)io_name(scene, source).length, io_name(scene, source).str);
  if (io_row(ui, string8_lit("io.source"), width, y, VKR_UI_ICON_ARROW_LEFT,
             theme->text_secondary, io_cstr(text),
             string8_lit("Select the source"))) {
    io_select(frame, source);
  }
  const String8 target_name =
      target.u64 ? io_name(scene, target) : string8_lit("none");
  snprintf(text, sizeof(text), "Target: %.*s", (int)target_name.length,
           target_name.str);
  if (io_row(ui, string8_lit("io.target"), width, y, VKR_UI_ICON_ARROW_RIGHT,
             theme->text_secondary, io_cstr(text),
             target.u64 ? string8_lit("Select the target")
                        : string8_lit("Pick a target below"))) {
    if (target.u64) {
      io_select(frame, target);
    }
  }
  const bool8_t picking = editor->io_pick.u64 == connection.u64;
  if (io_row(ui, string8_lit("io.pick"), width, y, VKR_UI_ICON_CROSSHAIR,
             picking ? theme->accent : theme->text_secondary,
             picking ? string8_lit("Click the target in the Scene or Outliner")
                     : string8_lit("Pick target"),
             string8_lit("The next object you select becomes the target; "
                         "Escape cancels"))) {
    editor->io_pick = picking ? VKR_ENTITY_ID_INVALID : connection;
  }
  char ports[256];
  io_ports_text(scene, source, false_v, ports, sizeof(ports));
  snprintf(text, sizeof(text), "Outputs: %s", ports);
  io_note(ui, string8_lit("io.outputs"), width, y, io_cstr(text));
  if (target.u64) {
    io_ports_text(scene, target, true_v, ports, sizeof(ports));
    snprintf(text, sizeof(text), "Inputs: %s", ports);
    io_note(ui, string8_lit("io.inputs"), width, y, io_cstr(text));
  }
  *y += 4.0f;
}

void vkr_editor_io_sections(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            float32_t width, float32_t *y,
                            VkrFontHandle heading) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrEntityId selected = frame->selected_entity;
  const VkrScene *scene = vkr_editor_entity_scene(frame, selected);
  if (!scene || !vkr_scene_entity_alive(scene, selected)) {
    return;
  }
  if (vkr_scene_get_typed(scene, selected, &vkr_scene_io_connection_type)) {
    io_route_section(editor, frame, scene, selected, width, y, heading);
    return;
  }
  /* Outputs: this object's connections, and + for a new one. */
  const float32_t outputs_y = *y;
  const bool8_t outputs_open = vkr_editor_details_section(
      ui, string8_lit("io.out"), width, y, VKR_UI_ICON_ARROW_RIGHT,
      (Vec4){0.98f, 0.82f, 0.35f, 1.0f}, string8_lit("Outputs"), heading,
      &editor->io_outputs_collapsed);
  char output[64];
  const bool8_t fires =
      io_first_output(scene, selected, output, sizeof(output));
  if (vkr_editor_details_section_action(
          ui, string8_lit("io.add"), width - 8.0f, outputs_y, VKR_UI_ICON_ADD,
          fires ? string8_lit("Connect an output of this object")
                : string8_lit("This object fires no outputs: add a "
                              "trigger, relay, timer, counter or script"))) {
    if (fires && !frame->simulation_running) {
      char id[48];
      io_id_text(selected, id, sizeof(id));
      char line[320];
      snprintf(line, sizeof(line),
               "{\"v\":1,\"id\":\"io\",\"op\":\"io.connect\",\"args\":{"
               "\"source\":\"%s\",\"output\":\"%s\",\"review\":false,"
               "\"select\":true}}",
               id, output);
      (void)vkr_editor_agent_submit(editor->agent, line);
    } else if (!fires) {
      vkr_editor_toast(editor, VKR_UI_ICON_INFO_FILL, theme->text_secondary,
                       "This object fires no outputs");
    }
  }
  VkrEntityRef self = {0};
  const bool8_t referable = vkr_scene_entity_ref(scene, selected, &self);
  uint32_t outgoing = 0u;
  uint32_t incoming = 0u;
  for (uint32_t pass = 0; pass < 2u; ++pass) {
    for (uint32_t i = 0; i < scene->world->dir.living; ++i) {
      const VkrEntityId entity = vkr_entity_id_from_index(scene->world, i);
      const SceneIoConnection *value =
          vkr_scene_entity_alive(scene, entity)
              ? vkr_scene_get_typed(scene, entity,
                                    &vkr_scene_io_connection_type)
              : NULL;
      if (!value) {
        continue;
      }
      const bool8_t out = io_parent(scene, entity).u64 == selected.u64;
      const bool8_t in =
          referable && MemCompare(&value->target, &self, sizeof(self)) == 0;
      if ((pass == 0u && !out) || (pass == 1u && !in)) {
        continue;
      }
      if (pass == 0u) {
        outgoing++;
      } else {
        incoming++;
      }
      if ((pass == 0u && !outputs_open) ||
          (pass == 1u && editor->io_inputs_collapsed)) {
        continue;
      }
      char problem[160];
      const bool8_t broken =
          vkr_io_connection_problem(scene, entity, problem, sizeof(problem));
      char text[256];
      io_connection_text(scene, entity, pass == 1u, text, sizeof(text));
      (void)vkr_ui_push_id_u64(ui, entity.u64);
      if (io_row(ui, string8_lit("io.connection"), width, y,
                 broken ? VKR_UI_ICON_WARNING_FILL
                        : (pass == 0u ? VKR_UI_ICON_ARROW_RIGHT
                                      : VKR_UI_ICON_ARROW_LEFT),
                 broken ? theme->warning : theme->text_secondary, io_cstr(text),
                 broken ? io_cstr(problem)
                        : (pass == 0u ? string8_lit("Select this connection")
                                      : string8_lit("Select its source")))) {
        io_select(frame, pass == 0u ? entity : io_parent(scene, entity));
      }
      (void)vkr_ui_pop_id(ui);
    }
    if (pass == 0u) {
      if (outputs_open && !outgoing) {
        io_note(ui, string8_lit("io.out.none"), width, y,
                fires ? string8_lit("No connections; + adds one")
                      : string8_lit("Fires no outputs"));
      }
      if (!incoming && !referable) {
        break;
      }
      /* Inputs: connections from other objects that reach this one. */
      if (!vkr_editor_details_section(
              ui, string8_lit("io.in"), width, y, VKR_UI_ICON_ARROW_LEFT,
              (Vec4){0.45f, 0.78f, 0.98f, 1.0f}, string8_lit("Inputs"), heading,
              &editor->io_inputs_collapsed)) {
        continue;
      }
    }
  }
  if (referable && !editor->io_inputs_collapsed && !incoming) {
    io_note(ui, string8_lit("io.in.none"), width, y,
            string8_lit("Nothing connects to this object"));
  }
}

void vkr_editor_io_update(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  const VkrEntityId connection = editor->io_pick;
  if (!connection.u64) {
    return;
  }
  const VkrScene *scene = vkr_editor_entity_scene(frame, connection);
  if (!scene || !vkr_scene_entity_alive(scene, connection) ||
      input_key_just_pressed(frame->input, KEY_ESCAPE)) {
    editor->io_pick = VKR_ENTITY_ID_INVALID;
    return;
  }
  const VkrEntityId picked = frame->selected_entity;
  if (!picked.u64 || picked.u64 == connection.u64) {
    return;
  }
  editor->io_pick = VKR_ENTITY_ID_INVALID;
  VkrEntityRef ref;
  if (vkr_editor_entity_scene(frame, picked) != scene ||
      !vkr_scene_entity_ref(scene, picked, &ref)) {
    vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL, vkr_ui_theme()->warning,
                     "A target must be an object of the same scene with an id");
    io_select(frame, connection);
    return;
  }
  char id[48];
  char uuid[37];
  io_id_text(connection, id, sizeof(id));
  vkr_entity_ref_format(&ref, uuid);
  char line[320];
  snprintf(line, sizeof(line),
           "{\"v\":1,\"id\":\"io\",\"op\":\"component.set\",\"args\":{"
           "\"entity\":\"%s\",\"type\":\"io_connection\",\"values\":{"
           "\"target\":\"%s\"},\"review\":false}}",
           id, uuid);
  (void)vkr_editor_agent_submit(editor->agent, line);
  io_select(frame, connection);
}
