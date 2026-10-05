#include "editor_internal.h"
#include "editor_projects.h"
#include "renderer/systems/vkr_render_assets.h"

#include "math/vkr_quat.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cmd expression evaluator. A statement is an expression, `name = expr` for a
 * session variable, or `path = expr` for editor or scene data:
 *
 *   sel.position.y = sel.position.y + 1
 *   entity("Sun").light.intensity = 3
 *   view.camera = "top"
 *   s = vec3(1, 2, 3) * 2
 *
 * Reads borrow the frame's scene; writes become the same typed requests the
 * panels submit, so scene edits validate, join the undo journal and apply
 * after this UI build. One statement makes at most one scene edit and one
 * view change. docs/editor-cmd.md lists every name and member. */

#define EVAL_TOKEN_CAPACITY 128u
#define EVAL_NODE_CAPACITY 128u
#define EVAL_ARG_CAPACITY 4u
#define EVAL_DEPTH_LIMIT 48u
#define EVAL_DEGREES 57.29577951308232
#define EVAL_RADIANS 0.017453292519943295

typedef VkrEditorCmdValue Value;

enum {
  EVAL_OBJECT_VIEW = 1,
  EVAL_OBJECT_UI,
  EVAL_OBJECT_SIM,
  EVAL_OBJECT_SCENE,
  EVAL_OBJECT_WORLD,
  EVAL_OBJECT_STATS,
  /* Machine graphics preferences, applied as the Scalability menu does. */
  EVAL_OBJECT_GFX,
};

typedef enum TokenKind {
  TOKEN_END,
  TOKEN_NUMBER,
  TOKEN_STRING,
  TOKEN_IDENT,
  TOKEN_OP,
} TokenKind;

typedef struct Token {
  TokenKind kind;
  String8 text;
  float64_t number;
} Token;

typedef enum NodeKind {
  NODE_NUMBER,
  NODE_STRING,
  NODE_IDENT,
  NODE_MEMBER,
  NODE_CALL,
  NODE_UNARY,
  NODE_BINARY,
  NODE_VEC3,
} NodeKind;

typedef struct Node {
  NodeKind kind;
  String8 text;
  float64_t number;
  int32_t a;
  int32_t b;
  int32_t args[EVAL_ARG_CAPACITY];
  uint32_t arg_count;
} Node;

typedef struct Eval {
  VkrEditorUi *editor;
  const VkrSampleUiFrame *frame;
  Token tokens[EVAL_TOKEN_CAPACITY];
  uint32_t token_count;
  uint32_t token;
  Node nodes[EVAL_NODE_CAPACITY];
  uint32_t node_count;
  uint32_t depth;
  char error[160];
} Eval;

/* Member names by value kind, for errors and completion. */
/* `world_position` reads the evaluated pose, which simulation moves;
   `position` is the authored local value. */
static const char *const eval_entity_members[] = {
    "name",  "position", "rotation",       "scale", "visible",
    "light", "id",       "world_position", NULL};
static const char *const eval_vec_members[] = {"x", "y", "z", "length", NULL};
static const char *const eval_view_members[] = {"camera",
                                                "mode",
                                                "grid",
                                                "grid_spacing",
                                                "camera_speed",
                                                "tool",
                                                "grid_labels",
                                                "grid_through",
                                                "collision",
                                                "snap",
                                                "snap_offset",
                                                "snap_yaw",
                                                "snap_align",
                                                "snap_centers",
                                                "camera_sensitivity",
                                                "grid_height",
                                                NULL};
static const char *const eval_snap_targets[] = {"free", "surface", "grid",
                                                NULL};
static const char *const eval_ui_members[] = {"zoom", "reduce_motion",
                                              "workbench", "selected", NULL};
static const char *const eval_gfx_members[] = {"render_scale",
                                               "dynamic",
                                               "vsync",
                                               "preset",
                                               "restart",
                                               "invert_mouse_y",
                                               "high_dpi",
                                               "texture_resolution",
                                               NULL};
static const char *const eval_gfx_presets[] = {"low",  "medium", "high",
                                               "epic", "custom", NULL};
static const char *const eval_sim_members[] = {"running", "time", NULL};
static const char *const eval_scene_members[] = {"loaded", "entities", "added",
                                                 NULL};
static const char *const eval_world_members[] = {"loaded", "entities", NULL};
static const char *const eval_stats_members[] = {"frame_ms",
                                                 "frame_ms_p95",
                                                 "finalizing",
                                                 "replaced_materials",
                                                 "pending_replacements",
                                                 "pending_textures",
                                                 "render_width",
                                                 "render_height",
                                                 "texture_mb",
                                                 "frame_ms_max",
                                                 NULL};
static const char *const eval_roots[] = {
    "sel",   "view",   "ui",   "sim",  "scene", "world",     "stats",
    "gfx",   "entity", "vec3", "len",  "sqrt",  "sin",       "cos",
    "tan",   "abs",    "min",  "max",  "clamp", "lerp",      "round",
    "floor", "ceil",   "pow",  "dot",  "cross", "normalize", "deg",
    "rad",   "str",    "pi",   "true", "false", NULL};

/* ---- Small helpers ---- */

static uint32_t eval_format(const VkrEditorCmdValue *value, char *out,
                            uint32_t capacity);

static bool8_t eval_fail(Eval *eval, const char *format, ...) {
  if (!eval->error[0]) {
    va_list args;
    va_start(args, format);
    vsnprintf(eval->error, sizeof(eval->error), format, args);
    va_end(args);
  }
  return false_v;
}

static bool8_t eval_is(String8 text, const char *word) {
  const uint64_t length = strlen(word);
  return text.length == length && MemCompare(text.str, word, length) == 0;
}

static int32_t eval_word_index(const char *const *words, String8 word) {
  for (int32_t i = 0; words[i]; ++i) {
    if (eval_is(word, words[i]))
      return i;
  }
  return -1;
}

static Value eval_number(float64_t number) {
  return (Value){.kind = VKR_EDITOR_CMD_VALUE_NUMBER, .number = number};
}

static Value eval_bool(bool8_t value) {
  return (Value){.kind = VKR_EDITOR_CMD_VALUE_BOOL, .number = value ? 1 : 0};
}

static Value eval_vec(Vec3 vector) {
  return (Value){.kind = VKR_EDITOR_CMD_VALUE_VEC3, .vector = vector};
}

static Value eval_string(const char *format, ...) {
  Value value = {.kind = VKR_EDITOR_CMD_VALUE_STRING};
  va_list args;
  va_start(args, format);
  vsnprintf(value.text, sizeof(value.text), format, args);
  va_end(args);
  return value;
}

static const char *eval_kind_name(VkrEditorCmdValueKind kind) {
  static const char *const names[] = {"nothing",   "number", "bool",
                                      "string",    "vec3",   "entity",
                                      "component", "object"};
  return names[kind];
}

static bool8_t eval_truthy(const Value *value) {
  switch (value->kind) {
  case VKR_EDITOR_CMD_VALUE_NUMBER:
  case VKR_EDITOR_CMD_VALUE_BOOL:
    return value->number != 0.0;
  case VKR_EDITOR_CMD_VALUE_STRING:
    return value->text[0] != '\0';
  case VKR_EDITOR_CMD_VALUE_ENTITY:
  case VKR_EDITOR_CMD_VALUE_COMPONENT:
    return value->entity.u64 != 0u;
  default:
    return value->kind != VKR_EDITOR_CMD_VALUE_NONE;
  }
}

/* ---- Lexer ---- */

static bool8_t eval_lex(Eval *eval, String8 line) {
  uint64_t i = 0;
  while (i <= line.length) {
    if (eval->token_count == EVAL_TOKEN_CAPACITY)
      return eval_fail(eval, "Statement is too long");
    Token *token = &eval->tokens[eval->token_count];
    if (i == line.length) {
      *token = (Token){.kind = TOKEN_END};
      ++eval->token_count;
      return true_v;
    }
    const uint8_t c = line.str[i];
    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }
    const uint64_t start = i;
    if (c >= '0' && c <= '9') {
      char buffer[64];
      uint64_t end = i;
      while (end < line.length && end - i < sizeof(buffer) - 1u &&
             ((line.str[end] >= '0' && line.str[end] <= '9') ||
              line.str[end] == '.' || line.str[end] == 'e' ||
              line.str[end] == 'E' ||
              ((line.str[end] == '-' || line.str[end] == '+') && end > i &&
               (line.str[end - 1] == 'e' || line.str[end - 1] == 'E'))))
        ++end;
      MemCopy(buffer, line.str + i, end - i);
      buffer[end - i] = '\0';
      char *parsed = NULL;
      token->number = strtod(buffer, &parsed);
      if (parsed != buffer + (end - i) || !isfinite(token->number))
        return eval_fail(eval, "Bad number '%s'", buffer);
      token->kind = TOKEN_NUMBER;
      i = end;
    } else if (c == '"') {
      ++i;
      while (i < line.length && line.str[i] != '"')
        ++i;
      if (i == line.length)
        return eval_fail(eval, "Unterminated string");
      token->kind = TOKEN_STRING;
      token->text =
          (String8){.str = line.str + start + 1, .length = i - start - 1};
      ++i;
      eval->token_count++;
      continue;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      while (i < line.length &&
             ((line.str[i] >= 'a' && line.str[i] <= 'z') ||
              (line.str[i] >= 'A' && line.str[i] <= 'Z') ||
              (line.str[i] >= '0' && line.str[i] <= '9') || line.str[i] == '_'))
        ++i;
      token->kind = TOKEN_IDENT;
    } else {
      static const char *const pairs[] = {"==", "!=", "<=", ">=", "&&", "||"};
      bool8_t pair = false_v;
      for (uint32_t p = 0; p < ArrayCount(pairs) && i + 1 < line.length; ++p)
        pair |= line.str[i] == pairs[p][0] && line.str[i + 1] == pairs[p][1];
      if (!pair && !strchr("+-*/%(),.=<>!", c))
        return eval_fail(eval, "Unexpected '%c'", c);
      i += pair ? 2u : 1u;
      token->kind = TOKEN_OP;
    }
    token->text = (String8){.str = line.str + start, .length = i - start};
    ++eval->token_count;
  }
  return true_v;
}

/* ---- Parser (precedence climbing into a bounded node pool) ---- */

static const Token *eval_peek(const Eval *eval) {
  return &eval->tokens[eval->token];
}

static bool8_t eval_accept(Eval *eval, const char *op) {
  const Token *token = eval_peek(eval);
  if (token->kind == TOKEN_OP && eval_is(token->text, op)) {
    ++eval->token;
    return true_v;
  }
  return false_v;
}

static int32_t eval_node(Eval *eval, NodeKind kind) {
  if (eval->node_count == EVAL_NODE_CAPACITY) {
    (void)eval_fail(eval, "Expression is too large");
    return -1;
  }
  eval->nodes[eval->node_count] = (Node){.kind = kind, .a = -1, .b = -1};
  return (int32_t)eval->node_count++;
}

static int32_t eval_parse_expression(Eval *eval, uint32_t min_precedence);

static int32_t eval_parse_primary(Eval *eval) {
  if (++eval->depth > EVAL_DEPTH_LIMIT) {
    (void)eval_fail(eval, "Expression nests too deeply");
    return -1;
  }
  const Token token = *eval_peek(eval);
  int32_t node = -1;
  if (token.kind == TOKEN_NUMBER || token.kind == TOKEN_STRING ||
      token.kind == TOKEN_IDENT) {
    ++eval->token;
    node = eval_node(eval, token.kind == TOKEN_NUMBER   ? NODE_NUMBER
                           : token.kind == TOKEN_STRING ? NODE_STRING
                                                        : NODE_IDENT);
    if (node >= 0) {
      eval->nodes[node].number = token.number;
      eval->nodes[node].text = token.text;
    }
  } else if (eval_accept(eval, "-") || eval_accept(eval, "!")) {
    node = eval_node(eval, NODE_UNARY);
    if (node >= 0) {
      eval->nodes[node].text = eval->tokens[eval->token - 1].text;
      eval->nodes[node].a = eval_parse_primary(eval);
    }
  } else if (eval_accept(eval, "(")) {
    /* (x, y, z) is a vector literal; (x) groups. */
    const int32_t first = eval_parse_expression(eval, 0);
    if (eval_accept(eval, ",")) {
      node = eval_node(eval, NODE_VEC3);
      if (node >= 0) {
        eval->nodes[node].args[0] = first;
        eval->nodes[node].args[1] = eval_parse_expression(eval, 0);
        if (!eval_accept(eval, ","))
          (void)eval_fail(eval, "A vector needs three components");
        eval->nodes[node].args[2] = eval_parse_expression(eval, 0);
        eval->nodes[node].arg_count = 3;
      }
    } else {
      node = first;
    }
    if (!eval_accept(eval, ")"))
      (void)eval_fail(eval, "Expected ')'");
  } else {
    (void)eval_fail(eval, "Expected a value");
  }
  /* Postfix member access and calls. */
  while (node >= 0 && !eval->error[0]) {
    if (eval_accept(eval, ".")) {
      const Token *name = eval_peek(eval);
      if (name->kind != TOKEN_IDENT) {
        (void)eval_fail(eval, "Expected a member name after '.'");
        break;
      }
      ++eval->token;
      const int32_t member = eval_node(eval, NODE_MEMBER);
      if (member < 0)
        break;
      eval->nodes[member].a = node;
      eval->nodes[member].text = name->text;
      node = member;
    } else if (eval_peek(eval)->kind == TOKEN_OP &&
               eval_is(eval_peek(eval)->text, "(") &&
               eval->nodes[node].kind == NODE_IDENT) {
      ++eval->token;
      const int32_t call = eval_node(eval, NODE_CALL);
      if (call < 0)
        break;
      eval->nodes[call].text = eval->nodes[node].text;
      if (!eval_accept(eval, ")")) {
        do {
          if (eval->nodes[call].arg_count == EVAL_ARG_CAPACITY) {
            (void)eval_fail(eval, "Too many arguments");
            break;
          }
          eval->nodes[call].args[eval->nodes[call].arg_count++] =
              eval_parse_expression(eval, 0);
        } while (eval_accept(eval, ","));
        if (!eval_accept(eval, ")"))
          (void)eval_fail(eval, "Expected ')' after arguments");
      }
      node = call;
    } else {
      break;
    }
  }
  --eval->depth;
  return eval->error[0] ? -1 : node;
}

static uint32_t eval_precedence(const Token *token) {
  if (token->kind != TOKEN_OP)
    return 0;
  static const struct {
    const char *op;
    uint32_t precedence;
  } table[] = {{"||", 1}, {"&&", 2}, {"==", 3}, {"!=", 3}, {"<", 4},
               {"<=", 4}, {">", 4},  {">=", 4}, {"+", 5},  {"-", 5},
               {"*", 6},  {"/", 6},  {"%", 6}};
  for (uint32_t i = 0; i < ArrayCount(table); ++i) {
    if (eval_is(token->text, table[i].op))
      return table[i].precedence;
  }
  return 0;
}

static int32_t eval_parse_expression(Eval *eval, uint32_t min_precedence) {
  int32_t left = eval_parse_primary(eval);
  while (left >= 0) {
    const Token *op = eval_peek(eval);
    const uint32_t precedence = eval_precedence(op);
    if (!precedence || precedence <= min_precedence)
      break;
    ++eval->token;
    const int32_t binary = eval_node(eval, NODE_BINARY);
    if (binary < 0)
      return -1;
    eval->nodes[binary].text = op->text;
    eval->nodes[binary].a = left;
    eval->nodes[binary].b = eval_parse_expression(eval, precedence);
    if (eval->nodes[binary].b < 0)
      return -1;
    left = binary;
  }
  return left;
}

/* ---- Data access ---- */

static VkrEntityId eval_find_entity(const VkrScene *scene, String8 name) {
  VkrEntityId partial = VKR_ENTITY_ID_INVALID;
  for (uint32_t i = 0; i < scene->world->dir.capacity; ++i) {
    if (!scene->world->dir.records[i].chunk)
      continue;
    const VkrEntityId id = vkr_entity_id_from_index(scene->world, i);
    const String8 candidate = vkr_scene_get_name(scene, id);
    if (string8_equals(&candidate, &name))
      return id;
    if (!partial.u64 && name.length && candidate.length >= name.length) {
      for (uint64_t c = 0; c + name.length <= candidate.length; ++c) {
        if (MemCompare(candidate.str + c, name.str, name.length) == 0) {
          partial = id;
          break;
        }
      }
    }
  }
  return partial;
}

static bool8_t eval_read_entity(Eval *eval, VkrEntityId entity,
                                VkrSceneEditValues *values) {
  const VkrScene *scene = vkr_editor_entity_scene(eval->frame, entity);
  if (!scene)
    return eval_fail(eval, "No scene is loaded");
  if (!entity.u64 || !vkr_scene_entity_alive(scene, entity))
    return eval_fail(eval, "Nothing is selected");
  if (!vkr_scene_edit_read(scene, entity, values))
    return eval_fail(eval, "That entity cannot be read");
  return true_v;
}

/* The light component the entity carries, or NULL. */
static const VkrTypeDesc *eval_light_type(const VkrSceneEditValues *values) {
  if (values->fields & VKR_SCENE_EDIT_POINT_LIGHT)
    return &vkr_scene_point_light_type;
  if (values->fields & VKR_SCENE_EDIT_DIRECTIONAL_LIGHT)
    return &vkr_scene_directional_light_type;
  if (values->fields & VKR_SCENE_EDIT_RECTANGLE_LIGHT)
    return &vkr_scene_rectangle_light_type;
  return NULL;
}

/* Component type named `name` that the entity carries, or NULL: the edit
   components, then world components (ADR-076). */
static const VkrTypeDesc *eval_component_named(const Eval *eval,
                                               VkrEntityId entity,
                                               const VkrSceneEditValues *values,
                                               String8 name) {
  const VkrTypeDesc *type = NULL;
  uint8_t component[VKR_TYPE_VALUE_MAX];
  for (uint32_t i = 0; (type = vkr_scene_edit_component_type(i)); ++i) {
    if (eval_is(name, type->name) &&
        vkr_scene_edit_component_get(values, type, component))
      return type;
  }
  type = vkr_scene_world_type_named(name);
  const VkrScene *scene = vkr_editor_entity_scene(eval->frame, entity);
  return type && scene && vkr_scene_get_typed(scene, entity, type) ? type
                                                                   : NULL;
}

/* Current bytes of a component value; false when the entity lost it. */
static bool8_t eval_component_get(const Eval *eval, const Value *base,
                                  VkrSceneEditValues *values, void *out) {
  if (vkr_scene_edit_component_field(base->type))
    return vkr_scene_edit_component_get(values, base->type, out);
  const void *current =
      vkr_scene_get_typed(vkr_editor_entity_scene(eval->frame, base->entity),
                          base->entity, base->type);
  if (!current)
    return false_v;
  MemCopy(out, current, base->type->size);
  return true_v;
}

/* Cmd values use stored units, except angles and rotations in degrees. */
static bool8_t eval_property_read(const VkrPropertyDesc *property,
                                  const void *component, Value *out) {
  float32_t floats[4] = {0};
  float64_t number = 0.0;
  switch (property->kind) {
  case VKR_PROPERTY_BOOL:
    (void)vkr_property_get_number(property, component, &number);
    *out = eval_bool(number != 0.0);
    return true_v;
  case VKR_PROPERTY_ENUM:
    (void)vkr_property_get_number(property, component, &number);
    *out = eval_string("%s", property->names[(uint32_t)number]);
    return true_v;
  case VKR_PROPERTY_ANGLE:
    (void)vkr_property_get_number(property, component, &number);
    *out = eval_number(number * EVAL_DEGREES);
    return true_v;
  case VKR_PROPERTY_STRING:
    *out = eval_string("%s", (const char *)component + property->offset);
    return true_v;
  case VKR_PROPERTY_ENTITY: {
    /* The id; `entity("<name>")` and a name compare against it by value. */
    const VkrEntityRef *ref =
        (const VkrEntityRef *)((const uint8_t *)component + property->offset);
    char text[37] = {0};
    if (!vkr_entity_ref_empty(ref)) {
      vkr_entity_ref_format(ref, text);
    }
    *out = eval_string("%s", text);
    return true_v;
  }
  case VKR_PROPERTY_QUAT: {
    float32_t degrees[3];
    (void)vkr_property_get_floats(property, component, floats);
    vkr_property_quat_euler((Vec4){floats[0], floats[1], floats[2], floats[3]},
                            degrees);
    *out = eval_vec((Vec3){degrees[0], degrees[1], degrees[2], 0.0f});
    return true_v;
  }
  case VKR_PROPERTY_VEC2:
  case VKR_PROPERTY_VEC3:
  case VKR_PROPERTY_VEC4:
  case VKR_PROPERTY_COLOR:
  case VKR_PROPERTY_DIRECTION:
    (void)vkr_property_get_floats(property, component, floats);
    *out = eval_vec(
        (Vec3){floats[0], floats[1],
               property->kind == VKR_PROPERTY_VEC2 ? 0.0f : floats[2], 0.0f});
    return true_v;
  default:
    (void)vkr_property_get_number(property, component, &number);
    *out = eval_number(number);
    return true_v;
  }
}

static bool8_t eval_property_write(Eval *eval, const VkrPropertyDesc *property,
                                   const VkrScene *scene, void *component,
                                   const Value *value, String8 member);

static Vec3 eval_euler_degrees(VkrQuat rotation) {
  float32_t roll = 0, pitch = 0, yaw = 0;
  vkr_quat_to_euler(rotation, &roll, &pitch, &yaw);
  return (Vec3){(float32_t)(roll * EVAL_DEGREES),
                (float32_t)(pitch * EVAL_DEGREES),
                (float32_t)(yaw * EVAL_DEGREES)};
}

static bool8_t eval_member(Eval *eval, const Value *base, String8 name,
                           Value *out) {
  const VkrSampleUiFrame *frame = eval->frame;
  switch (base->kind) {
  case VKR_EDITOR_CMD_VALUE_VEC3: {
    const int32_t index = eval_word_index(eval_vec_members, name);
    if (index < 0)
      break;
    *out = eval_number(index == 3 ? sqrt(vec3_dot(base->vector, base->vector))
                                  : base->vector.elements[index]);
    return true_v;
  }
  case VKR_EDITOR_CMD_VALUE_ENTITY: {
    VkrSceneEditValues values;
    if (!eval_read_entity(eval, base->entity, &values))
      return false_v;
    switch (eval_word_index(eval_entity_members, name)) {
    case 0:
      *out = eval_string("%s", values.name);
      return true_v;
    case 1:
      *out = eval_vec(values.position);
      return true_v;
    case 2:
      *out = eval_vec(eval_euler_degrees(values.rotation));
      return true_v;
    case 3:
      *out = eval_vec(values.scale);
      return true_v;
    case 4:
      *out = eval_bool(values.visibility.visible);
      return true_v;
    case 5:
      if (!eval_light_type(&values))
        return eval_fail(eval, "'%s' has no light", values.name);
      *out = *base;
      out->kind = VKR_EDITOR_CMD_VALUE_COMPONENT;
      out->type = eval_light_type(&values);
      return true_v;
    case 6:
      *out = eval_number(base->entity.parts.index);
      return true_v;
    case 7: {
      const VkrScene *scene =
          vkr_editor_entity_scene(eval->frame, base->entity);
      const SceneTransform *transform =
          scene ? vkr_entity_get_component(scene->world, base->entity,
                                           scene->comp_transform)
                : NULL;
      if (!transform)
        return eval_fail(eval, "'%s' has no placement", values.name);
      *out = eval_vec(mat4_position(transform->world));
      return true_v;
    }
    default: {
      const VkrTypeDesc *type =
          eval_component_named(eval, base->entity, &values, name);
      if (!type)
        break;
      *out = *base;
      out->kind = VKR_EDITOR_CMD_VALUE_COMPONENT;
      out->type = type;
      return true_v;
    }
    }
    break;
  }
  case VKR_EDITOR_CMD_VALUE_COMPONENT: {
    VkrSceneEditValues values;
    uint8_t component[VKR_TYPE_VALUE_MAX];
    if (!eval_read_entity(eval, base->entity, &values))
      return false_v;
    if (!eval_component_get(eval, base, &values, component))
      return eval_fail(eval, "'%s' no longer has a %s", values.name,
                       base->type->label);
    if (eval_is(name, "type")) {
      *out = eval_string("%s", base->type->name);
      return true_v;
    }
    const uint32_t index = vkr_type_find_property(base->type, name);
    if (index == UINT32_MAX ||
        (base->type->properties[index].flags & VKR_PROPERTY_FLAG_TRANSIENT))
      break;
    return eval_property_read(&base->type->properties[index], component, out);
  }
  case VKR_EDITOR_CMD_VALUE_OBJECT: {
    const VkrSampleViewState *view = &frame->view_state;
    if (base->object == EVAL_OBJECT_VIEW) {
      switch (eval_word_index(eval_view_members, name)) {
      case 0:
        *out = eval_string("%s",
                           vkr_editor_cmd_camera_views[Min(
                               (uint32_t)view->camera_view,
                               (uint32_t)VKR_SAMPLE_CAMERA_VIEW_COUNT - 1u)]);
        return true_v;
      case 1:
        for (uint32_t i = 0; vkr_editor_cmd_render_modes[i]; ++i) {
          if (vkr_editor_cmd_render_mode_values[i] == view->render_mode) {
            *out = eval_string("%s", vkr_editor_cmd_render_modes[i]);
            return true_v;
          }
        }
        *out = eval_number(view->render_mode);
        return true_v;
      case 2:
        *out = eval_bool(view->grid_enabled);
        return true_v;
      case 3:
        *out = eval_number(view->grid_spacing);
        return true_v;
      case 4:
        *out = eval_number(view->camera_speed);
        return true_v;
      case 6:
        *out = eval_bool(view->grid_labels);
        return true_v;
      case 7:
        *out = eval_bool(view->grid_through_geometry);
        return true_v;
      case 8:
        *out = eval_number(view->collision_display);
        return true_v;
      case 9:
        *out = eval_string("%s",
                           eval_snap_targets[eval->editor->placement.target]);
        return true_v;
      case 10:
        *out = eval_number(eval->editor->placement.offset);
        return true_v;
      case 11:
        *out = eval_number(eval->editor->placement.yaw_degrees);
        return true_v;
      case 12:
        *out = eval_bool(eval->editor->placement.align_to_normal);
        return true_v;
      case 13:
        *out = eval_bool(eval->editor->placement.cell_centers);
        return true_v;
      case 14:
        *out = eval_number(view->camera_sensitivity);
        return true_v;
      case 15:
        *out = eval_number(view->grid_height);
        return true_v;
      case 5:
        for (uint32_t i = 0; vkr_editor_cmd_tools[i]; ++i) {
          if (vkr_editor_cmd_tool_modes[i] == view->gizmo_tool) {
            *out = eval_string("%s", vkr_editor_cmd_tools[i]);
            return true_v;
          }
        }
        break;
      default:
        break;
      }
    } else if (base->object == EVAL_OBJECT_GFX && frame->graphics) {
      const VkrGraphicsSettings *settings = &frame->graphics->settings;
      switch (eval_word_index(eval_gfx_members, name)) {
      case 0:
        *out = eval_number(settings->render_scale);
        return true_v;
      case 1:
        *out = eval_bool(settings->dynamic_resolution);
        return true_v;
      case 2:
        *out = eval_bool(settings->vsync);
        return true_v;
      case 3:
        *out = eval_string(
            "%s", eval_gfx_presets[vkr_graphics_settings_preset(settings)]);
        return true_v;
      case 4:
        *out = eval_bool(frame->graphics->restart_required);
        return true_v;
      case 5:
        *out = eval_bool(settings->invert_mouse_y);
        return true_v;
      case 6:
        *out = eval_bool(settings->high_dpi);
        return true_v;
      case 7:
        /* Texels; zero is full resolution. */
        *out =
            eval_number(vkr_graphics_settings_texture_max_dimension(settings));
        return true_v;
      default:
        break;
      }
    } else if (base->object == EVAL_OBJECT_UI) {
      const int32_t index = eval_word_index(eval_ui_members, name);
      if (index == 0) {
        *out = eval_number(frame->ui->user_scale);
        return true_v;
      }
      if (index == 1) {
        *out = eval_bool(frame->ui->reduce_motion);
        return true_v;
      }
      if (index == 3) {
        VkrEntityId selection[VKR_EDITOR_SELECTION_MAX];
        *out = eval_number((float64_t)vkr_editor_selection_list(
            eval->editor, frame, selection, ArrayCount(selection)));
        return true_v;
      }
      if (index == 2) {
        *out = eval_string(
            "%s", vkr_editor_workbench_id(&eval->editor->workbenches,
                                          eval->editor->workbenches.active));
        return true_v;
      }
    } else if (base->object == EVAL_OBJECT_SIM) {
      const int32_t index = eval_word_index(eval_sim_members, name);
      if (index == 0) {
        *out = eval_bool(frame->simulation_running);
        return true_v;
      }
      if (index == 1) {
        *out = eval_number(frame->simulation_time);
        return true_v;
      }
    } else if (base->object == EVAL_OBJECT_STATS) {
      const int32_t index = eval_word_index(eval_stats_members, name);
      if (index == 9) {
        /* The longest recent frame interval: a streaming hitch. */
        const VkrEditorUi *editor = eval->editor;
        float32_t longest = 0.0f;
        for (uint32_t i = 0u; i < editor->frame_ms_count; ++i) {
          longest = Max(longest, editor->frame_ms[i]);
        }
        *out = eval_number(longest);
        return true_v;
      }
      if (index == 0 || index == 1) {
        /* Median or 95th percentile of the recent frame intervals. */
        const VkrEditorUi *editor = eval->editor;
        float32_t sorted[ArrayCount(editor->frame_ms)];
        const uint32_t count = editor->frame_ms_count;
        MemCopy(sorted, editor->frame_ms, sizeof(float32_t) * count);
        for (uint32_t i = 1u; i < count; ++i) {
          const float32_t key = sorted[i];
          uint32_t j = i;
          while (j > 0u && sorted[j - 1u] > key) {
            sorted[j] = sorted[j - 1u];
            --j;
          }
          sorted[j] = key;
        }
        const uint32_t rank =
            count ? Min(count - 1u,
                        (index == 0 ? count / 2u : (count * 95u) / 100u))
                  : 0u;
        *out = eval_number(count ? sorted[rank] : 0.0);
        return true_v;
      }
      bool8_t running = false_v;
      uint32_t applied = 0u;
      vkr_editor_projects_finalize_stats(eval->editor->projects, &running,
                                         &applied);
      if (index == 2) {
        *out = eval_bool(running);
        return true_v;
      }
      if (index == 3) {
        *out = eval_number(applied);
        return true_v;
      }
      if (index == 4) {
        *out = eval_number(vkr_material_system_pending_replacements(
            &frame->assets->material_system));
        return true_v;
      }
      if (index == 5) {
        *out = eval_number(frame->texture_pending_count);
        return true_v;
      }
      if (index == 6 || index == 7) {
        *out = eval_number(index == 6 ? frame->scene_render_width
                                      : frame->scene_render_height);
        return true_v;
      }
      if (index == 8) {
        /* Resident material textures, in MiB. */
        *out = eval_number((float64_t)frame->assets->material_system
                               .texture_stream_resident_bytes /
                           (1024.0 * 1024.0));
        return true_v;
      }
    } else if (base->object == EVAL_OBJECT_SCENE ||
               base->object == EVAL_OBJECT_WORLD) {
      /* The active scene, or the root World container (ADR-076). */
      const bool8_t world = base->object == EVAL_OBJECT_WORLD;
      const VkrScene *scene = world ? frame->world : frame->scene;
      const int32_t index = eval_word_index(
          world ? eval_world_members : eval_scene_members, name);
      if (index == 0) {
        *out = eval_bool(scene != NULL);
        return true_v;
      }
      if (index == 1) {
        uint32_t count = 0;
        for (uint32_t i = 0; scene && i < scene->world->dir.capacity; ++i)
          count += scene->world->dir.records[i].chunk != NULL;
        *out = eval_number(count);
        return true_v;
      }
      if (index == 2) {
        uint32_t added = 0;
        for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i)
          added += frame->additive[i] != NULL;
        *out = eval_number(added);
        return true_v;
      }
    }
    break;
  }
  default:
    break;
  }
  return eval_fail(eval, "No member '%.*s' on %s", (int)name.length, name.str,
                   eval_kind_name(base->kind));
}

/* ---- Evaluation ---- */

static bool8_t eval_node_value(Eval *eval, int32_t index, Value *out);

static bool8_t eval_args(Eval *eval, const Node *node, uint32_t count,
                         Value *values) {
  if (node->arg_count != count)
    return eval_fail(eval, "%.*s takes %u argument%s", (int)node->text.length,
                     node->text.str, count, count == 1 ? "" : "s");
  for (uint32_t i = 0; i < count; ++i) {
    if (!eval_node_value(eval, node->args[i], &values[i]))
      return false_v;
  }
  return true_v;
}

static bool8_t eval_numbers(Eval *eval, const Node *node, const Value *values,
                            uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if (values[i].kind != VKR_EDITOR_CMD_VALUE_NUMBER)
      return eval_fail(eval, "%.*s expects numbers", (int)node->text.length,
                       node->text.str);
  }
  return true_v;
}

static bool8_t eval_call(Eval *eval, const Node *node, Value *out) {
  Value v[EVAL_ARG_CAPACITY];
  const String8 name = node->text;
  static const struct {
    const char *name;
    float64_t (*fn)(float64_t);
  } unary[] = {{"sqrt", sqrt},   {"sin", sin},   {"cos", cos},
               {"tan", tan},     {"abs", fabs},  {"round", round},
               {"floor", floor}, {"ceil", ceil}, {"exp", exp},
               {"log", log}};
  for (uint32_t i = 0; i < ArrayCount(unary); ++i) {
    if (!eval_is(name, unary[i].name))
      continue;
    if (!eval_args(eval, node, 1, v) || !eval_numbers(eval, node, v, 1))
      return false_v;
    *out = eval_number(unary[i].fn(v[0].number));
    return true_v;
  }
  if (eval_is(name, "deg") || eval_is(name, "rad")) {
    if (!eval_args(eval, node, 1, v) || !eval_numbers(eval, node, v, 1))
      return false_v;
    *out = eval_number(v[0].number *
                       (eval_is(name, "deg") ? EVAL_DEGREES : EVAL_RADIANS));
    return true_v;
  }
  if (eval_is(name, "min") || eval_is(name, "max") || eval_is(name, "pow")) {
    if (!eval_args(eval, node, 2, v) || !eval_numbers(eval, node, v, 2))
      return false_v;
    *out = eval_number(eval_is(name, "min")   ? Min(v[0].number, v[1].number)
                       : eval_is(name, "max") ? Max(v[0].number, v[1].number)
                                              : pow(v[0].number, v[1].number));
    return true_v;
  }
  if (eval_is(name, "clamp")) {
    if (!eval_args(eval, node, 3, v) || !eval_numbers(eval, node, v, 3))
      return false_v;
    *out = eval_number(Clamp(v[0].number, v[1].number, v[2].number));
    return true_v;
  }
  if (eval_is(name, "vec3")) {
    if (!eval_args(eval, node, 3, v) || !eval_numbers(eval, node, v, 3))
      return false_v;
    *out = eval_vec((Vec3){(float32_t)v[0].number, (float32_t)v[1].number,
                           (float32_t)v[2].number});
    return true_v;
  }
  if (eval_is(name, "lerp")) {
    if (!eval_args(eval, node, 3, v))
      return false_v;
    if (v[2].kind != VKR_EDITOR_CMD_VALUE_NUMBER || v[0].kind != v[1].kind)
      return eval_fail(eval, "lerp(a, b, t) needs matching a, b and number t");
    const float32_t t = (float32_t)v[2].number;
    if (v[0].kind == VKR_EDITOR_CMD_VALUE_VEC3)
      *out = eval_vec(vec3_add(vec3_scale(v[0].vector, 1.0f - t),
                               vec3_scale(v[1].vector, t)));
    else if (v[0].kind == VKR_EDITOR_CMD_VALUE_NUMBER)
      *out = eval_number(v[0].number + (v[1].number - v[0].number) * t);
    else
      return eval_fail(eval, "lerp needs numbers or vectors");
    return true_v;
  }
  if (eval_is(name, "dot") || eval_is(name, "cross")) {
    if (!eval_args(eval, node, 2, v))
      return false_v;
    if (v[0].kind != VKR_EDITOR_CMD_VALUE_VEC3 ||
        v[1].kind != VKR_EDITOR_CMD_VALUE_VEC3)
      return eval_fail(eval, "%.*s needs two vectors", (int)name.length,
                       name.str);
    *out = eval_is(name, "dot")
               ? eval_number(vec3_dot(v[0].vector, v[1].vector))
               : eval_vec(vec3_cross(v[0].vector, v[1].vector));
    return true_v;
  }
  if (eval_is(name, "normalize") || eval_is(name, "len")) {
    if (!eval_args(eval, node, 1, v))
      return false_v;
    if (eval_is(name, "len") && v[0].kind == VKR_EDITOR_CMD_VALUE_STRING) {
      *out = eval_number((float64_t)strlen(v[0].text));
      return true_v;
    }
    if (v[0].kind != VKR_EDITOR_CMD_VALUE_VEC3)
      return eval_fail(eval, "%.*s needs a vector", (int)name.length, name.str);
    const float64_t length = sqrt(vec3_dot(v[0].vector, v[0].vector));
    *out = eval_is(name, "len")
               ? eval_number(length)
               : eval_vec(length > 0.0 ? vec3_scale(v[0].vector,
                                                    (float32_t)(1.0 / length))
                                       : v[0].vector);
    return true_v;
  }
  if (eval_is(name, "entity")) {
    if (!eval_args(eval, node, 1, v))
      return false_v;
    if (v[0].kind != VKR_EDITOR_CMD_VALUE_STRING)
      return eval_fail(eval, "entity(\"name\") needs a name");
    if (!eval->frame->scene && !eval->frame->world && !eval->frame->additive[0])
      return eval_fail(eval, "No scene is loaded");
    const String8 wanted = {.str = (uint8_t *)v[0].text,
                            .length = strlen(v[0].text)};
    /* The scene's entities first, then the root World's. */
    VkrEntityId entity = eval->frame->scene
                             ? eval_find_entity(eval->frame->scene, wanted)
                             : VKR_ENTITY_ID_INVALID;
    for (uint32_t i = 0; !entity.u64 && i < VKR_SCENE_ADDITIVE_MAX; ++i) {
      if (eval->frame->additive[i])
        entity = eval_find_entity(eval->frame->additive[i], wanted);
    }
    if (!entity.u64 && eval->frame->world)
      entity = eval_find_entity(eval->frame->world, wanted);
    if (!entity.u64)
      return eval_fail(eval, "No entity named '%s'", v[0].text);
    *out = (Value){.kind = VKR_EDITOR_CMD_VALUE_ENTITY, .entity = entity};
    return true_v;
  }
  if (eval_is(name, "str")) {
    if (!eval_args(eval, node, 1, v))
      return false_v;
    char text[160];
    (void)eval_format(&v[0], text, sizeof(text));
    *out = eval_string("%s", text);
    return true_v;
  }
  return eval_fail(eval, "Unknown function '%.*s'", (int)name.length, name.str);
}

static VkrEditorCmdVariable *eval_variable(VkrEditorUi *editor, String8 name) {
  for (uint32_t i = 0; i < editor->cmd_variable_count; ++i) {
    if (eval_is(name, editor->cmd_variables[i].name))
      return &editor->cmd_variables[i];
  }
  return NULL;
}

static bool8_t eval_ident(Eval *eval, String8 name, Value *out) {
  const VkrEditorCmdVariable *variable = eval_variable(eval->editor, name);
  if (variable) {
    *out = variable->value;
    return true_v;
  }
  if (eval_is(name, "sel") || eval_is(name, "selection")) {
    *out = (Value){.kind = VKR_EDITOR_CMD_VALUE_ENTITY,
                   .entity = eval->frame->selected_entity};
    return true_v;
  }
  static const char *const objects[] = {"view",  "ui",    "sim", "scene",
                                        "world", "stats", "gfx", NULL};
  const int32_t object = eval_word_index(objects, name);
  if (object >= 0) {
    *out = (Value){.kind = VKR_EDITOR_CMD_VALUE_OBJECT,
                   .object = (uint32_t)object + 1u};
    return true_v;
  }
  if (eval_is(name, "true") || eval_is(name, "false")) {
    *out = eval_bool(eval_is(name, "true"));
    return true_v;
  }
  if (eval_is(name, "pi")) {
    *out = eval_number(3.14159265358979323846);
    return true_v;
  }
  return eval_fail(eval, "Unknown name '%.*s'", (int)name.length, name.str);
}

static bool8_t eval_binary(Eval *eval, String8 op, const Value *a,
                           const Value *b, Value *out) {
  const bool8_t numbers = a->kind == VKR_EDITOR_CMD_VALUE_NUMBER &&
                          b->kind == VKR_EDITOR_CMD_VALUE_NUMBER;
  if (eval_is(op, "&&") || eval_is(op, "||")) {
    *out = eval_bool(eval_is(op, "&&") ? eval_truthy(a) && eval_truthy(b)
                                       : eval_truthy(a) || eval_truthy(b));
    return true_v;
  }
  if (eval_is(op, "==") || eval_is(op, "!=")) {
    bool8_t equal = a->kind == b->kind;
    if (equal && a->kind == VKR_EDITOR_CMD_VALUE_STRING)
      equal = strcmp(a->text, b->text) == 0;
    else if (equal && a->kind == VKR_EDITOR_CMD_VALUE_VEC3)
      equal = MemCompare(&a->vector, &b->vector, sizeof(a->vector)) == 0;
    else if (equal && a->kind == VKR_EDITOR_CMD_VALUE_ENTITY)
      equal = a->entity.u64 == b->entity.u64;
    else if (equal && a->kind == VKR_EDITOR_CMD_VALUE_COMPONENT)
      equal = a->entity.u64 == b->entity.u64 && a->type == b->type;
    else if (equal)
      equal = a->number == b->number && a->object == b->object;
    *out = eval_bool(eval_is(op, "==") ? equal : !equal);
    return true_v;
  }
  if (eval_is(op, "+") && (a->kind == VKR_EDITOR_CMD_VALUE_STRING ||
                           b->kind == VKR_EDITOR_CMD_VALUE_STRING)) {
    char left[160];
    char right[160];
    (void)eval_format(a, left, sizeof(left));
    (void)eval_format(b, right, sizeof(right));
    *out = eval_string("%s%s", left, right);
    return true_v;
  }
  if (numbers) {
    const float64_t x = a->number;
    const float64_t y = b->number;
    switch (op.str[0]) {
    case '+':
      *out = eval_number(x + y);
      return true_v;
    case '-':
      *out = eval_number(x - y);
      return true_v;
    case '*':
      *out = eval_number(x * y);
      return true_v;
    case '/':
      if (y == 0.0)
        return eval_fail(eval, "Division by zero");
      *out = eval_number(x / y);
      return true_v;
    case '%':
      if (y == 0.0)
        return eval_fail(eval, "Division by zero");
      *out = eval_number(fmod(x, y));
      return true_v;
    case '<':
      *out = eval_bool(op.length == 2 ? x <= y : x < y);
      return true_v;
    case '>':
      *out = eval_bool(op.length == 2 ? x >= y : x > y);
      return true_v;
    default:
      break;
    }
  }
  /* Vectors combine component-wise; a number scales. */
  const bool8_t av = a->kind == VKR_EDITOR_CMD_VALUE_VEC3;
  const bool8_t bv = b->kind == VKR_EDITOR_CMD_VALUE_VEC3;
  if ((av || bv) && (av || a->kind == VKR_EDITOR_CMD_VALUE_NUMBER) &&
      (bv || b->kind == VKR_EDITOR_CMD_VALUE_NUMBER)) {
    const Vec3 x = av ? a->vector
                      : (Vec3){(float32_t)a->number, (float32_t)a->number,
                               (float32_t)a->number};
    const Vec3 y = bv ? b->vector
                      : (Vec3){(float32_t)b->number, (float32_t)b->number,
                               (float32_t)b->number};
    Vec3 r = {0};
    for (uint32_t i = 0; i < 3; ++i) {
      const float32_t p = x.elements[i];
      const float32_t q = y.elements[i];
      switch (op.str[0]) {
      case '+':
        r.elements[i] = p + q;
        break;
      case '-':
        r.elements[i] = p - q;
        break;
      case '*':
        r.elements[i] = p * q;
        break;
      case '/':
        if (q == 0.0f)
          return eval_fail(eval, "Division by zero");
        r.elements[i] = p / q;
        break;
      default:
        return eval_fail(eval, "'%.*s' does not apply to vectors",
                         (int)op.length, op.str);
      }
    }
    *out = eval_vec(r);
    return true_v;
  }
  return eval_fail(eval, "'%.*s' does not apply to %s and %s", (int)op.length,
                   op.str, eval_kind_name(a->kind), eval_kind_name(b->kind));
}

static bool8_t eval_node_value(Eval *eval, int32_t index, Value *out) {
  if (index < 0)
    return eval_fail(eval, "Incomplete expression");
  const Node *node = &eval->nodes[index];
  switch (node->kind) {
  case NODE_NUMBER:
    *out = eval_number(node->number);
    return true_v;
  case NODE_STRING:
    *out = eval_string("%.*s", (int)node->text.length, node->text.str);
    return true_v;
  case NODE_IDENT:
    return eval_ident(eval, node->text, out);
  case NODE_MEMBER: {
    Value base;
    return eval_node_value(eval, node->a, &base) &&
           eval_member(eval, &base, node->text, out);
  }
  case NODE_CALL:
    return eval_call(eval, node, out);
  case NODE_VEC3: {
    Value v[3];
    for (uint32_t i = 0; i < 3; ++i) {
      if (!eval_node_value(eval, node->args[i], &v[i]))
        return false_v;
      if (v[i].kind != VKR_EDITOR_CMD_VALUE_NUMBER)
        return eval_fail(eval, "Vector components must be numbers");
    }
    *out = eval_vec((Vec3){(float32_t)v[0].number, (float32_t)v[1].number,
                           (float32_t)v[2].number});
    return true_v;
  }
  case NODE_UNARY: {
    Value value;
    if (!eval_node_value(eval, node->a, &value))
      return false_v;
    if (node->text.str[0] == '!') {
      *out = eval_bool(!eval_truthy(&value));
      return true_v;
    }
    if (value.kind == VKR_EDITOR_CMD_VALUE_NUMBER) {
      *out = eval_number(-value.number);
      return true_v;
    }
    if (value.kind == VKR_EDITOR_CMD_VALUE_VEC3) {
      *out = eval_vec(vec3_scale(value.vector, -1.0f));
      return true_v;
    }
    return eval_fail(eval, "Cannot negate a %s", eval_kind_name(value.kind));
  }
  case NODE_BINARY: {
    Value a;
    Value b;
    return eval_node_value(eval, node->a, &a) &&
           eval_node_value(eval, node->b, &b) &&
           eval_binary(eval, node->text, &a, &b, out);
  }
  }
  return eval_fail(eval, "Unsupported expression");
}

/* ---- Assignment ---- */

static bool8_t eval_expect(Eval *eval, const Value *value,
                           VkrEditorCmdValueKind kind, String8 member) {
  if (value->kind == kind)
    return true_v;
  return eval_fail(eval, "'%.*s' needs a %s, not a %s", (int)member.length,
                   member.str, eval_kind_name(kind),
                   eval_kind_name(value->kind));
}

static bool8_t eval_submit_edit(Eval *eval, VkrEntityId entity,
                                VkrSceneEditValues *values) {
  if (!vkr_scene_edit_validate(values))
    return eval_fail(eval, "The scene rejected that value");
  if (eval->frame->scene_edit->action != VKR_SCENE_EDIT_NONE)
    return eval_fail(eval, "Another scene edit is pending this frame");
  *eval->frame->scene_edit = (VkrSceneEditRequest){
      .action = VKR_SCENE_EDIT_APPLY, .entity = entity, .values = *values};
  return true_v;
}

static bool8_t eval_assign_entity(Eval *eval, VkrEntityId entity,
                                  String8 member, const Value *value) {
  VkrSceneEditValues values;
  if (!eval_read_entity(eval, entity, &values))
    return false_v;
  switch (eval_word_index(eval_entity_members, member)) {
  case 0:
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_STRING, member))
      return false_v;
    snprintf(values.name, sizeof(values.name), "%s", value->text);
    values.fields = VKR_SCENE_EDIT_NAME;
    break;
  case 1:
  case 2:
  case 3: {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_VEC3, member))
      return false_v;
    if (!(values.fields & VKR_SCENE_EDIT_TRANSFORM))
      return eval_fail(eval, "'%s' has no editable transform", values.name);
    const Vec3 v = value->vector;
    if (eval_is(member, "position"))
      values.position = v;
    else if (eval_is(member, "scale"))
      values.scale = v;
    else
      values.rotation = vkr_quat_from_euler((float32_t)(v.x * EVAL_RADIANS),
                                            (float32_t)(v.y * EVAL_RADIANS),
                                            (float32_t)(v.z * EVAL_RADIANS));
    values.fields = VKR_SCENE_EDIT_TRANSFORM;
    break;
  }
  case 4:
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_BOOL, member))
      return false_v;
    values.visibility.visible = value->number != 0.0;
    values.fields = VKR_SCENE_EDIT_VISIBILITY;
    break;
  default:
    return eval_fail(eval, "Cannot assign entity member '%.*s'",
                     (int)member.length, member.str);
  }
  return eval_submit_edit(eval, entity, &values);
}

static bool8_t eval_property_write(Eval *eval, const VkrPropertyDesc *property,
                                   const VkrScene *scene, void *component,
                                   const Value *value, String8 member) {
  if (property->flags &
      (VKR_PROPERTY_FLAG_READ_ONLY | VKR_PROPERTY_FLAG_TRANSIENT))
    return eval_fail(eval, "'%.*s' is read-only", (int)member.length,
                     member.str);
  switch (property->kind) {
  case VKR_PROPERTY_BOOL:
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_BOOL, member))
      return false_v;
    return vkr_property_set_number(property, component, value->number);
  case VKR_PROPERTY_ENUM: {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_STRING, member))
      return false_v;
    const String8 word = {.str = (uint8_t *)value->text,
                          .length = strlen(value->text)};
    const int32_t choice = eval_word_index(property->names, word);
    if (choice < 0)
      return eval_fail(eval, "Unknown %.*s '%s'", (int)member.length,
                       member.str, value->text);
    return vkr_property_set_number(property, component, choice);
  }
  case VKR_PROPERTY_ENTITY: {
    /* An entity of the same container, its id, or "" for none. */
    VkrEntityRef ref = {0};
    if (value->kind == VKR_EDITOR_CMD_VALUE_ENTITY) {
      if (vkr_editor_entity_scene(eval->frame, value->entity) != scene ||
          !vkr_scene_entity_ref(scene, value->entity, &ref))
        return eval_fail(eval,
                         "'%.*s' takes an object with an id in the same scene",
                         (int)member.length, member.str);
    } else if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_STRING, member) ||
               (value->text[0] &&
                !vkr_entity_ref_parse(value->text, strlen(value->text),
                                      &ref))) {
      return eval->error[0] ? false_v
                            : eval_fail(eval, "'%.*s' takes an object or an id",
                                        (int)member.length, member.str);
    }
    MemCopy((uint8_t *)component + property->offset, &ref, sizeof(ref));
    return true_v;
  }
  case VKR_PROPERTY_STRING: {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_STRING, member))
      return false_v;
    const uint64_t length = strlen(value->text);
    if (length >= property->capacity)
      return eval_fail(eval, "'%.*s' is limited to %u bytes",
                       (int)member.length, member.str, property->capacity - 1u);
    MemCopy((uint8_t *)component + property->offset, value->text, length + 1u);
    return true_v;
  }
  case VKR_PROPERTY_QUAT: {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_VEC3, member))
      return false_v;
    const float32_t degrees[3] = {value->vector.x, value->vector.y,
                                  value->vector.z};
    const Vec4 rotation = vkr_property_quat_from_euler(degrees);
    return vkr_property_set_floats(property, component, rotation.elements);
  }
  case VKR_PROPERTY_VEC2:
  case VKR_PROPERTY_VEC3:
  case VKR_PROPERTY_VEC4:
  case VKR_PROPERTY_COLOR:
  case VKR_PROPERTY_DIRECTION: {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_VEC3, member))
      return false_v;
    float32_t floats[4] = {0};
    (void)vkr_property_get_floats(property, component, floats);
    floats[0] = value->vector.x;
    floats[1] = value->vector.y;
    if (property->kind != VKR_PROPERTY_VEC2)
      floats[2] = value->vector.z;
    return vkr_property_set_floats(property, component, floats);
  }
  case VKR_PROPERTY_ANGLE:
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_NUMBER, member))
      return false_v;
    return vkr_property_set_number(property, component,
                                   value->number * EVAL_RADIANS);
  default:
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_NUMBER, member))
      return false_v;
    return vkr_property_set_number(property, component, value->number);
  }
}

static bool8_t eval_assign_component(Eval *eval, const Value *base,
                                     String8 member, const Value *value) {
  VkrSceneEditValues values;
  uint8_t component[VKR_TYPE_VALUE_MAX];
  if (!eval_read_entity(eval, base->entity, &values))
    return false_v;
  if (!eval_component_get(eval, base, &values, component))
    return eval_fail(eval, "'%s' no longer has a %s", values.name,
                     base->type->label);
  const uint32_t index = vkr_type_find_property(base->type, member);
  if (index == UINT32_MAX)
    return eval_fail(eval, "No member '%.*s' on %s", (int)member.length,
                     member.str, base->type->name);
  if (!eval_property_write(eval, &base->type->properties[index],
                           vkr_editor_entity_scene(eval->frame, base->entity),
                           component, value, member))
    return eval->error[0] ? false_v
                          : eval_fail(eval, "'%.*s' cannot hold that value",
                                      (int)member.length, member.str);
  char error[128] = {0};
  if (!vkr_type_validate(base->type, component, error, sizeof(error)))
    return eval_fail(eval, "%s", error);
  if (vkr_scene_edit_component_field(base->type)) {
    (void)vkr_scene_edit_component_set(&values, base->type, component);
    values.fields = vkr_scene_edit_component_field(base->type);
  } else {
    values.fields = VKR_SCENE_EDIT_COMPONENT;
    values.component_type = base->type;
    MemCopy(values.component, component, base->type->size);
  }
  return eval_submit_edit(eval, base->entity, &values);
}

static bool8_t eval_assign_object(Eval *eval, uint32_t object, String8 member,
                                  const Value *value) {
  const VkrSampleUiFrame *frame = eval->frame;
  if (object == EVAL_OBJECT_GFX) {
    if (!frame->graphics || !frame->graphics_request)
      return eval_fail(eval, "Graphics preferences are unavailable");
    VkrGraphicsSettings settings = frame->graphics->settings;
    const int32_t index = eval_word_index(eval_gfx_members, member);
    if (index == 0 &&
        eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_NUMBER, member)) {
      settings.render_scale =
          vkr_clamp_f32((float32_t)value->number, 1.0f / 3.0f, 1.0f);
    } else if ((index == 1 || index == 2 || index == 5 || index == 6) &&
               eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_BOOL, member)) {
      if (index == 1)
        settings.dynamic_resolution = value->number != 0.0;
      else if (index == 2)
        settings.vsync = value->number != 0.0;
      else if (index == 5)
        settings.invert_mouse_y = value->number != 0.0;
      else if (!frame->graphics->high_dpi_available)
        return eval_fail(eval, "High-DPI rendering is fixed on this platform");
      else
        settings.high_dpi = value->number != 0.0;
    } else if (index == 3 &&
               eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_STRING, member)) {
      const int32_t preset = eval_word_index(
          eval_gfx_presets, (String8){.str = (uint8_t *)value->text,
                                      .length = strlen(value->text)});
      if (preset < 0 || preset >= (int32_t)VKR_GRAPHICS_PRESET_CUSTOM)
        return eval_fail(eval, "Unknown preset '%s'", value->text);
      vkr_graphics_settings_apply_preset(&settings, (VkrGraphicsPreset)preset);
    } else if (index == 7 &&
               eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_NUMBER, member)) {
      /* The one of the three choices whose texel limit is the value; zero
         is full. */
      VkrGraphicsSettings choice = settings;
      bool8_t found = false_v;
      for (uint32_t i = 0u; i < 3u && !found; ++i) {
        choice.texture_resolution = i;
        found = (float64_t)vkr_graphics_settings_texture_max_dimension(
                    &choice) == value->number;
      }
      if (!found)
        return eval_fail(eval, "Use 1024, 2048 or 0 for full resolution");
      settings.texture_resolution = choice.texture_resolution;
    } else {
      return eval->error[0] ? false_v
                            : eval_fail(eval, "Cannot assign gfx.%.*s",
                                        (int)member.length, member.str);
    }
    *frame->graphics_request =
        (VkrGraphicsSettingsRequest){.settings = settings, .apply = true_v};
    return true_v;
  }
  if (object == EVAL_OBJECT_UI) {
    const int32_t index = eval_word_index(eval_ui_members, member);
    if (index == 0 &&
        eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_NUMBER, member)) {
      vkr_ui_system_set_user_scale(frame->ui, (float32_t)value->number);
      return true_v;
    }
    if (index == 1 &&
        eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_BOOL, member)) {
      frame->ui->reduce_motion = value->number != 0.0;
      return true_v;
    }
    return eval->error[0] ? false_v
                          : eval_fail(eval, "Cannot assign ui.%.*s",
                                      (int)member.length, member.str);
  }
  if (object == EVAL_OBJECT_SIM && eval_is(member, "running")) {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_BOOL, member))
      return false_v;
    *frame->transport_action = value->number != 0.0
                                   ? VKR_SAMPLE_TRANSPORT_START_SIMULATION
                                   : VKR_SAMPLE_TRANSPORT_PAUSE_SIMULATION;
    return true_v;
  }
  if (object != EVAL_OBJECT_VIEW)
    return eval_fail(eval, "That value is read-only");
  /* Snapping is editor state; it needs no view request. */
  const int32_t snap_member = eval_word_index(eval_view_members, member);
  if (snap_member >= 9 && snap_member <= 13) {
    VkrEditorPlacement *place = &eval->editor->placement;
    if (snap_member == 9) {
      if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_STRING, member))
        return false_v;
      const int32_t target = eval_word_index(
          eval_snap_targets, (String8){.str = (uint8_t *)value->text,
                                       .length = strlen(value->text)});
      if (target < 0)
        return eval_fail(eval, "Unknown snap '%s'", value->text);
      place->target = (VkrEditorSnapTarget)target;
    } else if (snap_member == 10 || snap_member == 11) {
      if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_NUMBER, member))
        return false_v;
      if (snap_member == 10)
        place->offset = vkr_clamp_f32((float32_t)value->number, -2.0f, 2.0f);
      else
        place->yaw_degrees =
            vkr_clamp_f32((float32_t)value->number, 0.0f, 360.0f);
    } else {
      if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_BOOL, member))
        return false_v;
      if (snap_member == 12)
        place->align_to_normal = value->number != 0.0;
      else
        place->cell_centers = value->number != 0.0;
    }
    return true_v;
  }
  if (!frame->view_request)
    return eval_fail(eval, "The Scene view is not available");
  if (frame->view_request->apply)
    return eval_fail(eval, "Another view change is pending this frame");
  VkrSampleViewState next = frame->view_state;
  const int32_t index = eval_word_index(eval_view_members, member);
  if (index == 0 || index == 1 || index == 5) {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_STRING, member))
      return false_v;
    const String8 word = {.str = (uint8_t *)value->text,
                          .length = strlen(value->text)};
    const char *const *words = index == 0   ? vkr_editor_cmd_camera_views
                               : index == 1 ? vkr_editor_cmd_render_modes
                                            : vkr_editor_cmd_tools;
    const int32_t choice = eval_word_index(words, word);
    if (choice < 0)
      return eval_fail(eval, "Unknown %.*s '%s'", (int)member.length,
                       member.str, value->text);
    if (index == 0)
      next.camera_view = (VkrSampleCameraView)choice;
    else if (index == 1)
      next.render_mode = vkr_editor_cmd_render_mode_values[choice];
    else
      next.gizmo_tool = vkr_editor_cmd_tool_modes[choice];
  } else if (index == 2 || index == 6 || index == 7) {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_BOOL, member))
      return false_v;
    if (index == 2)
      next.grid_enabled = value->number != 0.0;
    else if (index == 6)
      next.grid_labels = value->number != 0.0;
    else
      next.grid_through_geometry = value->number != 0.0;
  } else if (index == 8) {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_NUMBER, member))
      return false_v;
    if (!(value->number >= 0.0 && value->number <= 2.0))
      return eval_fail(eval, "'collision' is 0 off, 1 selected or 2 all");
    next.collision_display = (uint32_t)value->number;
  } else if (index == 15) {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_NUMBER, member))
      return false_v;
    next.grid_height =
        vkr_clamp_f32((float32_t)value->number, -10000.0f, 10000.0f);
    next.grid_enabled = true_v;
  } else if (index == 3 || index == 4 || index == 14) {
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_NUMBER, member))
      return false_v;
    if (!(value->number > 0.0))
      return eval_fail(eval, "'%.*s' must be positive", (int)member.length,
                       member.str);
    if (index == 3) {
      next.grid_spacing =
          vkr_clamp_f32((float32_t)value->number, 0.001f, 10000.0f);
      next.grid_enabled = true_v;
    } else if (index == 14) {
      next.camera_sensitivity =
          vkr_clamp_f32((float32_t)value->number, 0.01f, 100.0f);
    } else {
      next.camera_speed =
          vkr_clamp_f32((float32_t)value->number, 0.01f, 1000.0f);
    }
  } else {
    return eval_fail(eval, "Cannot assign view.%.*s", (int)member.length,
                     member.str);
  }
  *frame->view_request = (VkrSampleViewRequest){.value = next, .apply = true_v};
  return true_v;
}

/* Assigns to a variable or to a member path. Vector components write back
 * through their parent path, so `sel.position.y = 2` edits the entity. */
static bool8_t eval_assign(Eval *eval, int32_t target, const Value *value) {
  const Node *node = &eval->nodes[target];
  if (node->kind == NODE_IDENT) {
    if (eval_word_index(eval_roots, node->text) >= 0 ||
        eval_is(node->text, "selection"))
      return eval_fail(eval, "'%.*s' is built in", (int)node->text.length,
                       node->text.str);
    if (node->text.length >= sizeof(eval->editor->cmd_variables[0].name))
      return eval_fail(eval, "Variable names are limited to 31 characters");
    VkrEditorCmdVariable *variable = eval_variable(eval->editor, node->text);
    if (!variable) {
      if (eval->editor->cmd_variable_count ==
          ArrayCount(eval->editor->cmd_variables))
        return eval_fail(eval, "No room for another variable");
      variable =
          &eval->editor->cmd_variables[eval->editor->cmd_variable_count++];
      snprintf(variable->name, sizeof(variable->name), "%.*s",
               (int)node->text.length, node->text.str);
    }
    variable->value = *value;
    return true_v;
  }
  if (node->kind != NODE_MEMBER)
    return eval_fail(eval, "Only names and members can be assigned");
  Value base;
  if (!eval_node_value(eval, node->a, &base))
    return false_v;
  switch (base.kind) {
  case VKR_EDITOR_CMD_VALUE_VEC3: {
    const int32_t axis = eval_word_index(eval_vec_members, node->text);
    if (axis < 0 || axis > 2)
      return eval_fail(eval, "Assign x, y or z of a vector");
    if (!eval_expect(eval, value, VKR_EDITOR_CMD_VALUE_NUMBER, node->text))
      return false_v;
    base.vector.elements[axis] = (float32_t)value->number;
    return eval_assign(eval, node->a, &base);
  }
  case VKR_EDITOR_CMD_VALUE_ENTITY:
    return eval_assign_entity(eval, base.entity, node->text, value);
  case VKR_EDITOR_CMD_VALUE_COMPONENT:
    return eval_assign_component(eval, &base, node->text, value);
  case VKR_EDITOR_CMD_VALUE_OBJECT:
    return eval_assign_object(eval, base.object, node->text, value);
  default:
    return eval_fail(eval, "A %s member cannot be assigned",
                     eval_kind_name(base.kind));
  }
}

/* ---- Entry points ---- */

static uint32_t eval_format(const VkrEditorCmdValue *value, char *out,
                            uint32_t capacity) {
  int written = 0;
  switch (value->kind) {
  case VKR_EDITOR_CMD_VALUE_NUMBER:
    written = snprintf(out, capacity, "%.6g", value->number);
    break;
  case VKR_EDITOR_CMD_VALUE_BOOL:
    written = snprintf(out, capacity, "%s", value->number ? "true" : "false");
    break;
  case VKR_EDITOR_CMD_VALUE_STRING:
    written = snprintf(out, capacity, "%s", value->text);
    break;
  case VKR_EDITOR_CMD_VALUE_VEC3:
    written =
        snprintf(out, capacity, "(%.4g, %.4g, %.4g)", (double)value->vector.x,
                 (double)value->vector.y, (double)value->vector.z);
    break;
  case VKR_EDITOR_CMD_VALUE_ENTITY:
    written = snprintf(out, capacity, "entity #%u", value->entity.parts.index);
    break;
  case VKR_EDITOR_CMD_VALUE_COMPONENT:
    written = snprintf(out, capacity, "%s of entity #%u", value->type->name,
                       value->entity.parts.index);
    break;
  case VKR_EDITOR_CMD_VALUE_OBJECT: {
    static const char *const names[] = {"",      "view",  "ui",    "sim",
                                        "scene", "world", "stats", "gfx"};
    written =
        snprintf(out, capacity, "%s (type %s. for members)",
                 names[Min(value->object, 7u)], names[Min(value->object, 7u)]);
    break;
  }
  default:
    written = snprintf(out, capacity, "nothing");
    break;
  }
  return written > 0 ? (uint32_t)written : 0u;
}

bool8_t vkr_editor_cmd_eval(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            String8 line, char *message, uint32_t capacity) {
  /* The token and node pools make this too large for the stack. */
  static Eval eval;
  eval = (Eval){.editor = editor, .frame = frame};
  bool8_t ok = eval_lex(&eval, line);
  int32_t root = ok ? eval_parse_expression(&eval, 0) : -1;
  int32_t target = -1;
  if (ok && root >= 0 && eval_accept(&eval, "=")) {
    target = root;
    root = eval_parse_expression(&eval, 0);
  }
  if (!eval.error[0] && eval_peek(&eval)->kind != TOKEN_END)
    (void)eval_fail(&eval, "Unexpected '%.*s'",
                    (int)eval_peek(&eval)->text.length,
                    eval_peek(&eval)->text.str);
  Value value = {0};
  ok = !eval.error[0] && eval_node_value(&eval, root, &value);
  if (ok && target >= 0)
    ok = eval_assign(&eval, target, &value);
  if (!ok) {
    snprintf(message, capacity, "%s", eval.error);
    return false_v;
  }
  char text[160];
  (void)eval_format(&value, text, sizeof(text));
  if (target >= 0) {
    /* Echo the assigned path, everything before the '=', with the value. */
    const uint8_t *equals = memchr(line.str, '=', line.length);
    snprintf(message, capacity, "%.*s= %s",
             equals ? (int)(equals - line.str) : 0, line.str, text);
  } else {
    snprintf(message, capacity, "%s", text);
  }
  return true_v;
}

/* Completion name lists built from descriptors; valid until the next call. */
static const char *s_complete_names[VKR_TYPE_PROPERTY_MAX + 16u];

static const char *const *
eval_component_complete_names(const VkrTypeDesc *type) {
  uint32_t count = 0;
  s_complete_names[count++] = "type";
  for (uint32_t i = 0; i < type->property_count; ++i) {
    if (!(type->properties[i].flags & VKR_PROPERTY_FLAG_TRANSIENT))
      s_complete_names[count++] = type->properties[i].name;
  }
  s_complete_names[count] = NULL;
  return s_complete_names;
}

static const char *const *eval_entity_complete_names(Eval *eval,
                                                     const Value *base) {
  uint32_t count = 0;
  for (uint32_t i = 0; eval_entity_members[i]; ++i)
    s_complete_names[count++] = eval_entity_members[i];
  VkrSceneEditValues values;
  const VkrScene *scene = vkr_editor_entity_scene(eval->frame, base->entity);
  if (scene && base->entity.u64 &&
      vkr_scene_entity_alive(scene, base->entity) &&
      vkr_scene_edit_read(scene, base->entity, &values)) {
    const VkrTypeDesc *type = NULL;
    uint8_t component[VKR_TYPE_VALUE_MAX];
    for (uint32_t i = 0; (type = vkr_scene_edit_component_type(i)); ++i) {
      if (vkr_scene_edit_component_get(&values, type, component))
        s_complete_names[count++] = type->name;
    }
    for (uint32_t i = 0; (type = vkr_scene_world_type(i)) &&
                         count + 1u < ArrayCount(s_complete_names);
         ++i) {
      if (vkr_scene_get_typed(scene, base->entity, type))
        s_complete_names[count++] = type->name;
    }
  }
  s_complete_names[count] = NULL;
  return s_complete_names;
}

uint32_t vkr_editor_cmd_eval_complete(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      String8 text, char (*lines)[96],
                                      char (*hints)[96], uint32_t capacity) {
  /* The trailing path: identifiers and dots back to the last other byte. */
  uint64_t start = text.length;
  while (start > 0) {
    const uint8_t c = text.str[start - 1];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '.'))
      break;
    --start;
  }
  uint64_t dot = text.length;
  while (dot > start && text.str[dot - 1] != '.')
    --dot;
  const String8 partial = {.str = text.str + dot, .length = text.length - dot};
  const char *const *names = eval_roots;
  static Eval eval;
  eval = (Eval){.editor = editor, .frame = frame};
  Value base = {0};
  if (dot > start) {
    const String8 path = {.str = text.str + start, .length = dot - 1 - start};
    if (!eval_lex(&eval, path))
      return 0;
    const int32_t node = eval_parse_expression(&eval, 0);
    if (eval.error[0] || !eval_node_value(&eval, node, &base))
      return 0;
    names = base.kind == VKR_EDITOR_CMD_VALUE_VEC3 ? eval_vec_members
            : base.kind == VKR_EDITOR_CMD_VALUE_ENTITY
                ? eval_entity_complete_names(&eval, &base)
            : base.kind == VKR_EDITOR_CMD_VALUE_COMPONENT
                ? eval_component_complete_names(base.type)
            : base.object == EVAL_OBJECT_VIEW  ? eval_view_members
            : base.object == EVAL_OBJECT_UI    ? eval_ui_members
            : base.object == EVAL_OBJECT_SIM   ? eval_sim_members
            : base.object == EVAL_OBJECT_SCENE ? eval_scene_members
            : base.object == EVAL_OBJECT_WORLD ? eval_world_members
            : base.object == EVAL_OBJECT_STATS ? eval_stats_members
            : base.object == EVAL_OBJECT_GFX   ? eval_gfx_members
                                               : NULL;
    if (base.kind != VKR_EDITOR_CMD_VALUE_OBJECT &&
        base.kind != VKR_EDITOR_CMD_VALUE_VEC3 &&
        base.kind != VKR_EDITOR_CMD_VALUE_ENTITY &&
        base.kind != VKR_EDITOR_CMD_VALUE_COMPONENT)
      names = NULL;
  }
  uint32_t count = 0;
  /* Session variables complete at the start of a path too. */
  for (uint32_t i = 0;
       dot == start && i < editor->cmd_variable_count && count < capacity;
       ++i) {
    const char *name = editor->cmd_variables[i].name;
    if (strncmp(name, (const char *)partial.str, partial.length) != 0)
      continue;
    char value[64];
    (void)eval_format(&editor->cmd_variables[i].value, value, sizeof(value));
    snprintf(lines[count], 96, "%.*s%s", (int)dot, text.str, name);
    snprintf(hints[count++], 96, "variable = %s", value);
  }
  for (uint32_t i = 0; names && names[i] && count < capacity; ++i) {
    if (strncmp(names[i], (const char *)partial.str, partial.length) != 0)
      continue;
    snprintf(lines[count], 96, "%.*s%s", (int)dot, text.str, names[i]);
    /* Members show their current value; failures fall back to a label. */
    Value member = {0};
    eval.error[0] = '\0';
    if (dot > start && eval_member(&eval, &base,
                                   (String8){.str = (uint8_t *)names[i],
                                             .length = strlen(names[i])},
                                   &member))
      (void)eval_format(&member, hints[count], 96);
    else
      snprintf(hints[count], 96, "%s", dot > start ? "member" : "name");
    ++count;
  }
  return count;
}
