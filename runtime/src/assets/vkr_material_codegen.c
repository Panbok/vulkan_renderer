#include "assets/vkr_material_codegen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* A float literal Metal reads exactly: nine significant digits, a decimal
   point, and the `f` suffix. */
typedef struct CodegenLiteral {
  char text[40];
} CodegenLiteral;

static CodegenLiteral codegen_literal(float32_t value) {
  CodegenLiteral literal;
  snprintf(literal.text, sizeof(literal.text), "%.9g", (double)value);
  if (!strpbrk(literal.text, ".eE")) {
    strncat(literal.text, ".0",
            sizeof(literal.text) - strlen(literal.text) - 1u);
  }
  strncat(literal.text, "f", sizeof(literal.text) - strlen(literal.text) - 1u);
  return literal;
}

// =============================================================================
// Text
// =============================================================================

typedef struct CodegenText {
  VkrAllocator *allocator;
  char *data;
  uint64_t length;
  uint64_t capacity;
  bool8_t failed;
} CodegenText;

static void codegen_append(CodegenText *text, const char *format, ...) {
  if (text->failed) {
    return;
  }
  char line[1024];
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (written < 0 || (uint64_t)written >= sizeof(line)) {
    text->failed = true_v;
    return;
  }
  const uint64_t needed = text->length + (uint64_t)written + 1u;
  if (needed > text->capacity) {
    const uint64_t next = Max(needed, Max(4096u, text->capacity * 2u));
    char *grown =
        text->data
            ? vkr_allocator_realloc(text->allocator, text->data, text->capacity,
                                    next, VKR_ALLOCATOR_MEMORY_TAG_STRING)
            : vkr_allocator_alloc(text->allocator, next,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!grown) {
      text->failed = true_v;
      return;
    }
    text->data = grown;
    text->capacity = next;
  }
  MemCopy(text->data + text->length, line, (uint64_t)written + 1u);
  text->length += (uint64_t)written;
}

// =============================================================================
// Emission
// =============================================================================

/* One value an expression gives: its MSL text and component count (1 to 3;
   4 for a texture sample, 0 for a normal map). */
typedef struct CodegenValue {
  char text[192];
  uint32_t components;
} CodegenValue;

typedef struct Codegen {
  const VkrMaterialGraph *graph;
  CodegenText *body;
  VkrMaterialCustomBinding *binding;
  VkrMaterialLowering *result;
  /* A node's locals are emitted once. */
  bool8_t emitted[VKR_MATERIAL_GRAPH_NODE_MAX];
  /* Where each texture node's RGB or channels go, for its class. */
  bool8_t feeds_normal[VKR_MATERIAL_GRAPH_NODE_MAX];
  bool8_t feeds_color[VKR_MATERIAL_GRAPH_NODE_MAX];
} Codegen;

static bool8_t codegen_fail(Codegen *codegen, const VkrMaterialNode *node,
                            const char *format, ...) {
  VkrMaterialLowering *result = codegen->result;
  result->tier = VKR_MATERIAL_TIER_UNSUPPORTED;
  snprintf(result->node, sizeof(result->node), "%s", node ? node->id : "");
  va_list args;
  va_start(args, format);
  vsnprintf(result->reason, sizeof(result->reason), format, args);
  va_end(args);
  return false_v;
}

/* Components a value type carries in generated code. */
static uint32_t codegen_components(VkrMaterialValueType type) {
  return type == VKR_MATERIAL_VALUE_VEC2    ? 2u
         : type == VKR_MATERIAL_VALUE_COLOR ? 3u
                                            : 1u;
}

static const char *codegen_type(uint32_t components) {
  return components == 1u   ? "float"
         : components == 2u ? "float2"
         : components == 3u ? "float3"
                            : "float4";
}

/* `value` widened to `components`: a scalar broadcasts; anything else must
   already have them. */
static bool8_t codegen_widen(Codegen *codegen, const VkrMaterialNode *node,
                             const CodegenValue *value, uint32_t components,
                             char *out, uint64_t capacity) {
  if (value->components == components) {
    snprintf(out, capacity, "%s", value->text);
    return true_v;
  }
  if (value->components == 1u) {
    snprintf(out, capacity, "%s(%s)", codegen_type(components), value->text);
    return true_v;
  }
  return codegen_fail(codegen, node,
                      "Mixes %u- and %u-component values; split or combine "
                      "them first",
                      value->components, components);
}

static bool8_t codegen_link(Codegen *codegen, VkrMaterialLink link,
                            CodegenValue *out);

/* Input `port` of `node`, or false naming it when it is not connected. */
static bool8_t codegen_input(Codegen *codegen, const VkrMaterialNode *node,
                             uint32_t port, CodegenValue *out) {
  const VkrMaterialLink link = node->inputs[port];
  if (!link.node) {
    return codegen_fail(codegen, node, "'%s' needs its input '%s'",
                        vkr_material_node_desc(node->kind)->label,
                        vkr_material_node_desc(node->kind)->inputs[port].name);
  }
  return codegen_link(codegen, link, out);
}

/* A scalar input: a wider value takes its first component. */
static bool8_t codegen_scalar_input(Codegen *codegen,
                                    const VkrMaterialNode *node, uint32_t port,
                                    CodegenValue *out) {
  if (!codegen_input(codegen, node, port, out)) {
    return false_v;
  }
  if (out->components != 1u) {
    return codegen_fail(codegen, node, "'%s' takes a scalar",
                        vkr_material_node_desc(node->kind)->inputs[port].name);
  }
  return true_v;
}

/* Exposed parameter slot of `index`, assigned on first use. */
static uint32_t codegen_param(Codegen *codegen, uint32_t index) {
  VkrMaterialCustomBinding *binding = codegen->binding;
  for (uint32_t i = 0; i < binding->param_count; ++i) {
    if (binding->param_nodes[i] == index) {
      return i;
    }
  }
  if (binding->param_count == VKR_MATERIAL_CUSTOM_PARAM_MAX) {
    return UINT32_MAX;
  }
  binding->param_nodes[binding->param_count] = index;
  return binding->param_count++;
}

/* Texture slot of `index`, assigned on first use. */
static uint32_t codegen_texture(Codegen *codegen, uint32_t index) {
  VkrMaterialCustomBinding *binding = codegen->binding;
  for (uint32_t i = 0; i < binding->texture_count; ++i) {
    if (binding->texture_nodes[i] == index) {
      return i;
    }
  }
  if (binding->texture_count == VKR_MATERIAL_CUSTOM_TEXTURE_MAX) {
    return UINT32_MAX;
  }
  binding->texture_nodes[binding->texture_count] = index;
  return binding->texture_count++;
}

/* Emits node `index`'s local `n<index>` once: a value, a texture sample or a
   decoded normal. */
static bool8_t codegen_node(Codegen *codegen, uint32_t index) {
  const VkrMaterialGraph *graph = codegen->graph;
  const VkrMaterialNode *node = &graph->nodes[index];
  CodegenText *body = codegen->body;
  if (codegen->emitted[index]) {
    return true_v;
  }
  codegen->emitted[index] = true_v;
  const VkrMaterialNodeDesc *desc = vkr_material_node_desc(node->kind);
  CodegenValue a = {0};
  CodegenValue b = {0};
  CodegenValue c = {0};
  char wide_a[256];
  char wide_b[256];
  const uint32_t components =
      desc->output_count
          ? codegen_components(vkr_material_graph_output_type(graph, index, 0u))
          : 1u;
  const char *type = codegen_type(components);
  switch (node->kind) {
  case VKR_MATERIAL_NODE_SCALAR:
  case VKR_MATERIAL_NODE_COLOR: {
    const bool8_t scalar = node->kind == VKR_MATERIAL_NODE_SCALAR;
    if (node->parameter[0]) {
      const uint32_t slot = codegen_param(codegen, index);
      if (slot == UINT32_MAX) {
        return codegen_fail(codegen, node,
                            "A Custom graph exposes at most %u constants",
                            VKR_MATERIAL_CUSTOM_PARAM_MAX);
      }
      codegen_append(body, "    const %s n%u = custom.params[%u].%s;\n",
                     scalar ? "float" : "float3", index, slot,
                     scalar ? "x" : "xyz");
    } else if (scalar) {
      codegen_append(body, "    const float n%u = %s;\n", index,
                     codegen_literal(node->value.x).text);
    } else {
      codegen_append(body, "    const float3 n%u = float3(%s, %s, %s);\n",
                     index, codegen_literal(node->value.x).text,
                     codegen_literal(node->value.y).text,
                     codegen_literal(node->value.z).text);
    }
    return true_v;
  }
  case VKR_MATERIAL_NODE_TEXTURE: {
    const uint32_t slot = codegen_texture(codegen, index);
    if (slot == UINT32_MAX) {
      return codegen_fail(codegen, node,
                          "A Custom graph samples at most %u textures",
                          VKR_MATERIAL_CUSTOM_TEXTURE_MAX);
    }
    if (!node->path[0] && !node->parameter[0]) {
      return codegen_fail(codegen, node, "The texture names no file");
    }
    char uv[256] = "in.texcoord";
    if (node->inputs[0].node) {
      if (!codegen_input(codegen, node, 0u, &a)) {
        return false_v;
      }
      if (a.components != 2u) {
        return codegen_fail(codegen, node, "A texture's UV is two-component");
      }
      snprintf(uv, sizeof(uv), "%s", a.text);
    }
    codegen_append(body,
                   "    const float4 n%u = custom.textures[%u].sample("
                   "custom.samplers[%u], %s);\n",
                   index, slot, slot, uv);
    codegen->result->samples++;
    return true_v;
  }
  case VKR_MATERIAL_NODE_NORMAL_MAP: {
    const VkrMaterialLink texture = node->inputs[0];
    if (!texture.node ||
        graph->nodes[texture.node - 1u].kind != VKR_MATERIAL_NODE_TEXTURE ||
        texture.port != 0u) {
      return codegen_fail(codegen, node, "A normal map reads a texture's RGB");
    }
    codegen->feeds_normal[texture.node - 1u] = true_v;
    if (!codegen_node(codegen, texture.node - 1u)) {
      return false_v;
    }
    char strength[192] = "1.0f";
    if (node->inputs[1].node) {
      if (!codegen_scalar_input(codegen, node, 1u, &b)) {
        return false_v;
      }
      snprintf(strength, sizeof(strength), "%s", b.text);
    }
    codegen_append(
        body, "    const float3 n%u = vkr_normal_map_decode(n%u.xyz, %s);\n",
        index, texture.node - 1u, strength);
    return true_v;
  }
  case VKR_MATERIAL_NODE_UV:
  case VKR_MATERIAL_NODE_VERTEX_COLOR:
  case VKR_MATERIAL_NODE_WORLD_POSITION:
  case VKR_MATERIAL_NODE_WORLD_NORMAL:
  case VKR_MATERIAL_NODE_TIME:
  case VKR_MATERIAL_NODE_CAMERA_DISTANCE:
  case VKR_MATERIAL_NODE_SPLIT:
    /* Read in place by codegen_link. */
    return true_v;
  case VKR_MATERIAL_NODE_MULTIPLY:
  case VKR_MATERIAL_NODE_ADD:
  case VKR_MATERIAL_NODE_SUBTRACT:
  case VKR_MATERIAL_NODE_DIVIDE:
  case VKR_MATERIAL_NODE_MIN:
  case VKR_MATERIAL_NODE_MAX: {
    if (!codegen_input(codegen, node, 0u, &a) ||
        !codegen_input(codegen, node, 1u, &b) ||
        !codegen_widen(codegen, node, &a, components, wide_a, sizeof(wide_a)) ||
        !codegen_widen(codegen, node, &b, components, wide_b, sizeof(wide_b))) {
      return false_v;
    }
    static const char *const operators[] = {"*", "+", "-"};
    const char *operation =
        node->kind == VKR_MATERIAL_NODE_MULTIPLY   ? operators[0]
        : node->kind == VKR_MATERIAL_NODE_ADD      ? operators[1]
        : node->kind == VKR_MATERIAL_NODE_SUBTRACT ? operators[2]
                                                   : NULL;
    if (operation) {
      codegen_append(body, "    const %s n%u = %s %s %s;\n", type, index,
                     wide_a, operation, wide_b);
    } else {
      codegen_append(body, "    const %s n%u = %s(%s, %s);\n", type, index,
                     node->kind == VKR_MATERIAL_NODE_DIVIDE
                         ? "vkr_custom_divide"
                     : node->kind == VKR_MATERIAL_NODE_MIN ? "min"
                                                           : "max",
                     wide_a, wide_b);
    }
    return true_v;
  }
  case VKR_MATERIAL_NODE_LERP:
    if (!codegen_input(codegen, node, 0u, &a) ||
        !codegen_input(codegen, node, 1u, &b) ||
        !codegen_scalar_input(codegen, node, 2u, &c) ||
        !codegen_widen(codegen, node, &a, components, wide_a, sizeof(wide_a)) ||
        !codegen_widen(codegen, node, &b, components, wide_b, sizeof(wide_b))) {
      return false_v;
    }
    codegen_append(body, "    const %s n%u = mix(%s, %s, %s);\n", type, index,
                   wide_a, wide_b, c.text);
    return true_v;
  case VKR_MATERIAL_NODE_POWER:
    if (!codegen_input(codegen, node, 0u, &a) ||
        !codegen_scalar_input(codegen, node, 1u, &b)) {
      return false_v;
    }
    codegen_append(body, "    const %s n%u = vkr_custom_power(%s, %s);\n", type,
                   index, a.text, b.text);
    return true_v;
  case VKR_MATERIAL_NODE_ONE_MINUS:
  case VKR_MATERIAL_NODE_SATURATE:
  case VKR_MATERIAL_NODE_ABS:
  case VKR_MATERIAL_NODE_SINE:
    if (!codegen_input(codegen, node, 0u, &a)) {
      return false_v;
    }
    if (node->kind == VKR_MATERIAL_NODE_ONE_MINUS) {
      codegen_append(body, "    const %s n%u = 1.0f - %s;\n", type, index,
                     a.text);
    } else {
      codegen_append(body, "    const %s n%u = %s(%s);\n", type, index,
                     node->kind == VKR_MATERIAL_NODE_SATURATE ? "saturate"
                     : node->kind == VKR_MATERIAL_NODE_ABS    ? "abs"
                                                              : "sin",
                     a.text);
    }
    return true_v;
  case VKR_MATERIAL_NODE_DOT:
    if (!codegen_input(codegen, node, 0u, &a) ||
        !codegen_input(codegen, node, 1u, &b) ||
        !codegen_widen(codegen, node, &a, 3u, wide_a, sizeof(wide_a)) ||
        !codegen_widen(codegen, node, &b, 3u, wide_b, sizeof(wide_b))) {
      return false_v;
    }
    codegen_append(body, "    const float n%u = dot(%s, %s);\n", index, wide_a,
                   wide_b);
    return true_v;
  case VKR_MATERIAL_NODE_COMBINE: {
    char parts[3][192];
    for (uint32_t i = 0; i < 3u; ++i) {
      snprintf(parts[i], sizeof(parts[i]), "0.0f");
      if (node->inputs[i].node) {
        if (!codegen_scalar_input(codegen, node, i, &a)) {
          return false_v;
        }
        snprintf(parts[i], sizeof(parts[i]), "%s", a.text);
      }
    }
    codegen_append(body, "    const float3 n%u = float3(%s, %s, %s);\n", index,
                   parts[0], parts[1], parts[2]);
    return true_v;
  }
  case VKR_MATERIAL_NODE_TILE_OFFSET: {
    char uv[192] = "in.texcoord";
    if (node->inputs[0].node) {
      if (!codegen_input(codegen, node, 0u, &a)) {
        return false_v;
      }
      if (a.components != 2u) {
        return codegen_fail(codegen, node, "Tile and offset takes UVs");
      }
      snprintf(uv, sizeof(uv), "%s", a.text);
    }
    codegen_append(body,
                   "    const float2 n%u = %s * float2(%s, %s) + "
                   "float2(%s, %s);\n",
                   index, uv, codegen_literal(node->value.x).text,
                   codegen_literal(node->value.y).text,
                   codegen_literal(node->value.z).text,
                   codegen_literal(node->value.w).text);
    return true_v;
  }
  case VKR_MATERIAL_NODE_WORLD_PLANAR:
    if (!(node->value.x > 0.0f)) {
      return codegen_fail(codegen, node,
                          "World planar needs a size above zero meters");
    }
    codegen_append(body, "    const float2 n%u = in.world_position.xz * %s;\n",
                   index, codegen_literal(1.0f / node->value.x).text);
    return true_v;
  default:
    return codegen_fail(codegen, node, "'%s' has no Custom form", desc->label);
  }
}

/* The expression output `link.port` of `link.node` gives. */
static bool8_t codegen_link(Codegen *codegen, VkrMaterialLink link,
                            CodegenValue *out) {
  const uint32_t index = link.node - 1u;
  const VkrMaterialNode *node = &codegen->graph->nodes[index];
  if (!codegen_node(codegen, index)) {
    return false_v;
  }
  static const char *const channels[] = {"", ".x", ".y", ".z", ".w"};
  switch (node->kind) {
  case VKR_MATERIAL_NODE_TEXTURE:
    if (link.port == 0u) {
      codegen->feeds_color[index] = true_v;
      snprintf(out->text, sizeof(out->text), "n%u.rgb", index);
      out->components = 3u;
    } else {
      snprintf(out->text, sizeof(out->text), "n%u%s", index,
               channels[link.port]);
      out->components = 1u;
    }
    return true_v;
  case VKR_MATERIAL_NODE_NORMAL_MAP:
    snprintf(out->text, sizeof(out->text), "n%u", index);
    out->components = 0u;
    return true_v;
  case VKR_MATERIAL_NODE_UV:
    snprintf(out->text, sizeof(out->text), "in.texcoord");
    out->components = 2u;
    return true_v;
  case VKR_MATERIAL_NODE_VERTEX_COLOR:
    snprintf(out->text, sizeof(out->text), "%s",
             link.port == 0u ? "in.color.rgb" : "in.color.a");
    out->components = link.port == 0u ? 3u : 1u;
    return true_v;
  case VKR_MATERIAL_NODE_WORLD_POSITION:
    snprintf(out->text, sizeof(out->text), "in.world_position");
    out->components = 3u;
    return true_v;
  case VKR_MATERIAL_NODE_WORLD_NORMAL:
    snprintf(out->text, sizeof(out->text), "in.world_normal");
    out->components = 3u;
    return true_v;
  case VKR_MATERIAL_NODE_TIME:
    snprintf(out->text, sizeof(out->text), "in.time");
    out->components = 1u;
    return true_v;
  case VKR_MATERIAL_NODE_CAMERA_DISTANCE:
    snprintf(out->text, sizeof(out->text), "in.camera_distance");
    out->components = 1u;
    return true_v;
  case VKR_MATERIAL_NODE_SPLIT: {
    CodegenValue rgb = {0};
    if (!codegen_input(codegen, node, 0u, &rgb)) {
      return false_v;
    }
    char wide[256];
    if (!codegen_widen(codegen, node, &rgb, 3u, wide, sizeof(wide))) {
      return false_v;
    }
    snprintf(out->text, sizeof(out->text), "(%s)%s", wide,
             channels[link.port + 1u]);
    out->components = 1u;
    return true_v;
  }
  default: {
    snprintf(out->text, sizeof(out->text), "n%u", index);
    out->components = codegen_components(
        vkr_material_graph_output_type(codegen->graph, index, link.port));
    return true_v;
  }
  }
}

/* The core surface outputs a Custom graph drives, and the output fields
   they write. */
static const struct {
  const char *input;
  const char *field;
  uint32_t components;
} s_outputs[] = {
    {"base_color", "base_color", 3u}, {"opacity", "opacity", 1u},
    {"metallic", "metallic", 1u},     {"roughness", "roughness", 1u},
    {"occlusion", "occlusion", 1u},   {"emissive", "emissive", 3u},
    {"normal", "tangent_normal", 0u},
};

static uint64_t codegen_hash(const char *text, uint64_t length) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (uint64_t i = 0; i < length; ++i) {
    hash = (hash ^ (uint8_t)text[i]) * UINT64_C(1099511628211);
  }
  return hash;
}

bool8_t vkr_material_codegen_msl(const VkrMaterialGraph *graph,
                                 VkrAllocator *allocator, String8 *out_source,
                                 VkrMaterialCustomBinding *out_binding,
                                 VkrMaterialLowering *out) {
  *out = (VkrMaterialLowering){.tier = VKR_MATERIAL_TIER_CUSTOM};
  MemZero(out_binding, sizeof(*out_binding));
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  CodegenText body = {.allocator = allocator};
  Codegen codegen = {
      .graph = graph, .body = &body, .binding = out_binding, .result = out};
  if (!vkr_material_graph_validate(graph, error, sizeof(error))) {
    return codegen_fail(&codegen, NULL, "%s", error);
  }
  const VkrMaterialNode *output = NULL;
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    if (graph->nodes[i].kind == VKR_MATERIAL_NODE_SURFACE_OUTPUT) {
      output = &graph->nodes[i];
    }
    out->alu += vkr_material_node_desc(graph->nodes[i].kind)->alu;
  }
  if (!output) {
    return codegen_fail(&codegen, NULL, "The graph has no surface output");
  }
  /* Inputs beyond the core outputs keep the Standard tier's meaning only. */
  const VkrMaterialNodeDesc *surface = vkr_material_node_desc(output->kind);
  for (uint32_t port = 0; port < surface->input_count; ++port) {
    bool8_t core = false_v;
    for (uint32_t i = 0; i < ArrayCount(s_outputs) && !core; ++i) {
      core = strcmp(surface->inputs[port].name, s_outputs[i].input) == 0;
    }
    if (!core && output->inputs[port].node) {
      return codegen_fail(&codegen, output,
                          "'%s' has no Custom form; a Custom graph drives "
                          "base colour, opacity, metallic, roughness, "
                          "occlusion, emissive and normal",
                          surface->inputs[port].label);
    }
  }
  CodegenText writes = {.allocator = allocator};
  for (uint32_t i = 0; i < ArrayCount(s_outputs); ++i) {
    const uint32_t port = vkr_material_node_input(
        output->kind,
        string8_create_from_cstr((const uint8_t *)s_outputs[i].input,
                                 strlen(s_outputs[i].input)));
    const VkrMaterialLink link = output->inputs[port];
    if (!link.node) {
      continue;
    }
    CodegenValue value = {0};
    if (!codegen_link(&codegen, link, &value)) {
      return false_v;
    }
    if (s_outputs[i].components == 0u) {
      if (value.components != 0u) {
        return codegen_fail(&codegen, output, "'normal' takes a normal map");
      }
      codegen_append(&writes,
                     "    out.tangent_normal = %s;\n"
                     "    out.normal_mapped = true;\n",
                     value.text);
      continue;
    }
    char wide[256];
    if (value.components == 0u ||
        (s_outputs[i].components == 1u && value.components != 1u)) {
      return codegen_fail(&codegen, output, "'%s' takes a %s",
                          s_outputs[i].input,
                          s_outputs[i].components == 1u ? "scalar" : "colour");
    }
    if (!codegen_widen(&codegen, output, &value, s_outputs[i].components, wide,
                       sizeof(wide))) {
      return false_v;
    }
    codegen_append(&writes, "    out.%s = %s;\n", s_outputs[i].field, wide);
  }
  for (uint32_t i = 0; i < out_binding->texture_count; ++i) {
    const uint32_t index = out_binding->texture_nodes[i];
    const VkrMaterialNode *texture = &graph->nodes[index];
    out_binding->texture_normal[i] = codegen.feeds_normal[index];
    out_binding->texture_srgb[i] =
        texture->color_space == VKR_MATERIAL_COLOR_SPACE_SRGB ||
        (texture->color_space == VKR_MATERIAL_COLOR_SPACE_AUTO &&
         codegen.feeds_color[index] && !codegen.feeds_normal[index]);
    if (codegen.feeds_normal[index] && codegen.feeds_color[index]) {
      return codegen_fail(&codegen, texture,
                          "A texture feeds a normal map and a colour; use "
                          "two texture nodes");
    }
  }
  if (body.failed || writes.failed) {
    return codegen_fail(&codegen, NULL, "Out of memory for the source");
  }

  /* The function's name is the hash of what it computes. */
  CodegenText source = {.allocator = allocator};
  codegen_append(&source, "%s%s", body.data ? body.data : "",
                 writes.data ? writes.data : "");
  const uint64_t hash =
      codegen_hash(source.data ? source.data : "", source.length);
  snprintf(out->function, sizeof(out->function), "vkr_custom_%016llx",
           (unsigned long long)hash);
  CodegenText text = {.allocator = allocator};
  codegen_append(&text,
                 "struct %s {\n"
                 "  static VkrMetalCustomOutput\n"
                 "  evaluate(thread const VkrMetalCustomInput &in,\n"
                 "           const device VkrMetalPacketCustomMaterial "
                 "&custom) {\n"
                 "    VkrMetalCustomOutput out = vkr_metal_custom_output();\n",
                 out->function);
  codegen_append(&text, "%s", body.data ? body.data : "");
  codegen_append(&text, "%s", writes.data ? writes.data : "");
  codegen_append(&text, "    return out;\n  }\n};\n");
  if (text.failed) {
    return codegen_fail(&codegen, NULL, "Out of memory for the source");
  }
  *out_source = (String8){.str = (uint8_t *)text.data, .length = text.length};
  out->tier = VKR_MATERIAL_TIER_CUSTOM;
  return true_v;
}

// =============================================================================
// Entry points
// =============================================================================

bool8_t vkr_material_codegen_msl_entries(const char *function,
                                         VkrAllocator *allocator,
                                         String8 *out_source) {
  /* The tiled forward entries' names and template arguments, as
     tiled.metal declares them. */
  static const struct {
    const char *suffix;
    const char *lighting;
  } lightings[] = {
      {"", "VKR_METAL_TILED_LIGHTING_NONE"},
      {"_punctual", "VKR_METAL_TILED_LIGHTING_PUNCTUAL"},
      {"_shadowed", "VKR_METAL_TILED_LIGHTING_SHADOWED"},
      {"_all", "VKR_METAL_TILED_LIGHTING_ALL"},
      {"_casters", "VKR_METAL_TILED_LIGHTING_CASTERS"},
  };
  CodegenText text = {.allocator = allocator};
  for (uint32_t coverage = 0; coverage < 2u; ++coverage) {
    for (uint32_t l = 0; l < ArrayCount(lightings); ++l) {
      for (uint32_t probes = 0; probes < 2u; ++probes) {
        for (uint32_t decals = 0; decals < 2u; ++decals) {
          codegen_append(
              &text,
              "fragment float4 %s_vkr_metal_tiled_forward%s%s%s%s_fragment(\n"
              "    VkrMetalTiledVertexOutput input [[stage_in]],\n"
              "    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],\n"
              "    bool front_facing [[front_facing]]) {\n"
              "  return vkr_metal_tiled_forward<%s, %s, %s, %s,\n"
              "      VkrMetalCustomSurface<%s>>(input, root, front_facing,\n"
              "                                 float3(0.0f));\n"
              "}\n\n",
              function, lightings[l].suffix, coverage ? "_coverage" : "",
              probes ? "_probes" : "", decals ? "_decals" : "",
              lightings[l].lighting, coverage ? "true" : "false",
              probes ? "true" : "false", decals ? "true" : "false", function);
        }
      }
    }
    codegen_append(
        &text,
        "fragment float4 %s_vkr_metal_tiled_forward_inspect%s_fragment(\n"
        "    VkrMetalTiledVertexOutput input [[stage_in]],\n"
        "    constant VkrMetalPacketDrawRoot *root [[buffer(1)]],\n"
        "    bool front_facing [[front_facing]],\n"
        "    float3 barycentric [[barycentric_coord]]) {\n"
        "  return vkr_metal_tiled_forward<VKR_METAL_TILED_LIGHTING_INSPECT, "
        "%s, true, true,\n"
        "      VkrMetalCustomSurface<%s>>(input, root, front_facing,\n"
        "                                 barycentric);\n"
        "}\n\n",
        function, coverage ? "_coverage" : "", coverage ? "true" : "false",
        function);
  }
  if (text.failed) {
    return false_v;
  }
  *out_source = (String8){.str = (uint8_t *)text.data, .length = text.length};
  return true_v;
}
