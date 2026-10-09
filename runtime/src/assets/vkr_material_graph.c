#include "assets/vkr_material_graph.h"

#include "assets/vkr_material_codegen.h"
#include "core/vkr_json.h"
#include "filesystem/vkr_asset_path.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// =============================================================================
// Node registry
// =============================================================================

static const VkrMaterialPortDesc s_scalar_out[] = {
    {"value", "Value", VKR_MATERIAL_VALUE_SCALAR}};
static const VkrMaterialPortDesc s_color_out[] = {
    {"value", "Colour", VKR_MATERIAL_VALUE_COLOR}};
static const VkrMaterialPortDesc s_texture_out[] = {
    {"rgb", "RGB", VKR_MATERIAL_VALUE_COLOR},
    {"r", "R", VKR_MATERIAL_VALUE_SCALAR},
    {"g", "G", VKR_MATERIAL_VALUE_SCALAR},
    {"b", "B", VKR_MATERIAL_VALUE_SCALAR},
    {"a", "A", VKR_MATERIAL_VALUE_SCALAR},
};
_Static_assert(ArrayCount(s_texture_out) == VKR_MATERIAL_GRAPH_OUTPUT_MAX,
               "a texture has the most outputs");
/* A texture samples at its own UVs when they connect (Custom tier), else at
   the mesh's. */
static const VkrMaterialPortDesc s_texture_in[] = {
    {"uv", "UV", VKR_MATERIAL_VALUE_VEC2}};
/* Multiply takes and gives scalars or colours; graph_output_type resolves
   its output from its inputs. */
static const VkrMaterialPortDesc s_multiply_in[] = {
    {"a", "A", VKR_MATERIAL_VALUE_COLOR},
    {"b", "B", VKR_MATERIAL_VALUE_COLOR},
};
static const VkrMaterialPortDesc s_multiply_out[] = {
    {"value", "Result", VKR_MATERIAL_VALUE_COLOR}};
static const VkrMaterialPortDesc s_normal_in[] = {
    {"texture", "Texture", VKR_MATERIAL_VALUE_COLOR},
    {"strength", "Strength", VKR_MATERIAL_VALUE_SCALAR},
};
static const VkrMaterialPortDesc s_normal_out[] = {
    {"normal", "Normal", VKR_MATERIAL_VALUE_NORMAL}};
static const VkrMaterialPortDesc s_layer_out[] = {
    {"layer", "Layer", VKR_MATERIAL_VALUE_LAYER}};
static const VkrMaterialPortDesc s_layer_blend_in[] = {
    {"layer1", "Layer 1", VKR_MATERIAL_VALUE_LAYER},
    {"layer2", "Layer 2", VKR_MATERIAL_VALUE_LAYER},
    {"layer3", "Layer 3", VKR_MATERIAL_VALUE_LAYER},
};
static const VkrMaterialPortDesc s_layer_blend_out[] = {
    {"layers", "Layers", VKR_MATERIAL_VALUE_LAYERS}};

/* Custom-tier ports. Generic math takes and gives scalars, two- and
   three-component values (vkr_material_graph_output_type). */
static const VkrMaterialPortDesc s_uv_out[] = {
    {"uv", "UV", VKR_MATERIAL_VALUE_VEC2}};
static const VkrMaterialPortDesc s_vertex_color_out[] = {
    {"rgb", "RGB", VKR_MATERIAL_VALUE_COLOR},
    {"a", "A", VKR_MATERIAL_VALUE_SCALAR},
};
static const VkrMaterialPortDesc s_position_out[] = {
    {"position", "Position", VKR_MATERIAL_VALUE_COLOR}};
static const VkrMaterialPortDesc s_world_normal_out[] = {
    {"normal", "Normal", VKR_MATERIAL_VALUE_COLOR}};
static const VkrMaterialPortDesc s_seconds_out[] = {
    {"seconds", "Seconds", VKR_MATERIAL_VALUE_SCALAR}};
static const VkrMaterialPortDesc s_meters_out[] = {
    {"meters", "Meters", VKR_MATERIAL_VALUE_SCALAR}};
static const VkrMaterialPortDesc s_binary_in[] = {
    {"a", "A", VKR_MATERIAL_VALUE_COLOR},
    {"b", "B", VKR_MATERIAL_VALUE_COLOR},
};
static const VkrMaterialPortDesc s_unary_in[] = {
    {"x", "X", VKR_MATERIAL_VALUE_COLOR}};
static const VkrMaterialPortDesc s_generic_out[] = {
    {"value", "Result", VKR_MATERIAL_VALUE_COLOR}};
static const VkrMaterialPortDesc s_lerp_in[] = {
    {"a", "A", VKR_MATERIAL_VALUE_COLOR},
    {"b", "B", VKR_MATERIAL_VALUE_COLOR},
    {"t", "T", VKR_MATERIAL_VALUE_SCALAR},
};
static const VkrMaterialPortDesc s_power_in[] = {
    {"x", "X", VKR_MATERIAL_VALUE_COLOR},
    {"exponent", "Exponent", VKR_MATERIAL_VALUE_SCALAR},
};
static const VkrMaterialPortDesc s_dot_in[] = {
    {"a", "A", VKR_MATERIAL_VALUE_COLOR},
    {"b", "B", VKR_MATERIAL_VALUE_COLOR},
};
static const VkrMaterialPortDesc s_split_in[] = {
    {"rgb", "RGB", VKR_MATERIAL_VALUE_COLOR}};
static const VkrMaterialPortDesc s_split_out[] = {
    {"r", "R", VKR_MATERIAL_VALUE_SCALAR},
    {"g", "G", VKR_MATERIAL_VALUE_SCALAR},
    {"b", "B", VKR_MATERIAL_VALUE_SCALAR},
};
static const VkrMaterialPortDesc s_combine_in[] = {
    {"r", "R", VKR_MATERIAL_VALUE_SCALAR},
    {"g", "G", VKR_MATERIAL_VALUE_SCALAR},
    {"b", "B", VKR_MATERIAL_VALUE_SCALAR},
};
static const VkrMaterialPortDesc s_combine_out[] = {
    {"rgb", "RGB", VKR_MATERIAL_VALUE_COLOR}};
static const VkrMaterialPortDesc s_tile_in[] = {
    {"uv", "UV", VKR_MATERIAL_VALUE_VEC2}};

const char *const vkr_material_graph_mask_names[] = {"vertex_color", "texture",
                                                     "slope", "height", NULL};
_Static_assert(ArrayCount(vkr_material_graph_mask_names) ==
                   VKR_MATERIAL_GRAPH_MASK_COUNT + 1u,
               "one name per mask");

/* The surface output's inputs, and the `.mt` keys each lowers to: the
   factor, the texture slot, the channel a scalar reads from its texture
   (0 to 3 for r to a; -1 for rgb) and whether the slot's colour defaults
   to sRGB. One list gives the port table and the lowering table. */
#define SURFACE_INPUTS(X)                                                      \
  X(BASE_COLOR, "base_color", "Base colour", COLOR, "base_color",              \
    "base_color_texture", -1, true_v)                                          \
  X(OPACITY, "opacity", "Opacity", SCALAR, NULL, NULL, 3, false_v)             \
  X(METALLIC, "metallic", "Metallic", SCALAR, "metallic",                      \
    "metallic_roughness_texture", 2, false_v)                                  \
  X(ROUGHNESS, "roughness", "Roughness", SCALAR, "roughness",                  \
    "metallic_roughness_texture", 1, false_v)                                  \
  X(OCCLUSION, "occlusion", "Occlusion", SCALAR, "occlusion_strength",         \
    "occlusion_texture", 0, false_v)                                           \
  X(NORMAL, "normal", "Normal", NORMAL, "normal_scale", "normal_texture", -1,  \
    false_v)                                                                   \
  X(EMISSIVE, "emissive", "Emissive", COLOR, "emissive_factor",                \
    "emissive_texture", -1, true_v)                                            \
  X(SPECULAR, "specular", "Dielectric specular", COLOR, "dielectric_specular", \
    NULL, -1, false_v)                                                         \
  X(CLEARCOAT, "clearcoat", "Clearcoat", SCALAR, "clearcoat_factor",           \
    "clearcoat_texture", 0, false_v)                                           \
  X(CLEARCOAT_ROUGHNESS, "clearcoat_roughness", "Clearcoat roughness", SCALAR, \
    "clearcoat_roughness", "clearcoat_roughness_texture", 1, false_v)          \
  X(CLEARCOAT_NORMAL, "clearcoat_normal", "Clearcoat normal", NORMAL,          \
    "clearcoat_normal_scale", "clearcoat_normal_texture", -1, false_v)         \
  X(SHEEN_COLOR, "sheen_color", "Sheen colour", COLOR, "sheen_color",          \
    "sheen_color_texture", -1, true_v)                                         \
  X(SHEEN_ROUGHNESS, "sheen_roughness", "Sheen roughness", SCALAR,             \
    "sheen_roughness", "sheen_roughness_texture", 3, false_v)                  \
  X(ANISOTROPY, "anisotropy", "Anisotropy", SCALAR, "anisotropy_strength",     \
    "anisotropy_texture", 2, false_v)                                          \
  X(ANISOTROPY_ROTATION, "anisotropy_rotation", "Anisotropy rotation", SCALAR, \
    "anisotropy_rotation", NULL, -1, false_v)                                  \
  X(TRANSMISSION, "transmission", "Transmission", SCALAR,                      \
    "transmission_factor", "transmission_texture", 0, false_v)                 \
  X(THICKNESS, "thickness", "Thickness", SCALAR, "thickness_factor",           \
    "thickness_texture", 1, false_v)                                           \
  X(ATTENUATION_COLOR, "attenuation_color", "Attenuation colour", COLOR,       \
    "attenuation_color", NULL, -1, false_v)                                    \
  X(ATTENUATION_DISTANCE, "attenuation_distance", "Attenuation distance",      \
    SCALAR, "attenuation_distance", NULL, -1, false_v)                         \
  X(IOR, "ior", "IOR", SCALAR, "ior", NULL, -1, false_v)                       \
  X(SUBSURFACE, "subsurface", "Subsurface", SCALAR, "subsurface_strength",     \
    NULL, -1, false_v)                                                         \
  X(DIFFUSE_TRANSMISSION, "diffuse_transmission", "Diffuse transmission",      \
    SCALAR, "diffuse_transmission_strength", NULL, -1, false_v)                \
  X(DIFFUSE_TRANSMISSION_COLOR, "diffuse_transmission_color",                  \
    "Diffuse transmission colour", COLOR, "diffuse_transmission_color", NULL,  \
    -1, false_v)                                                               \
  X(LAYERS, "layers", "Layers", LAYERS, NULL, NULL, -1, false_v)

typedef struct SurfaceInput {
  VkrMaterialPortDesc port;
  const char *factor;
  const char *texture;
  int32_t channel;
  bool8_t srgb;
} SurfaceInput;

#define SURFACE_ENUM(id, name, label, type, factor, texture, channel, srgb)    \
  SURFACE_##id,
enum { SURFACE_INPUTS(SURFACE_ENUM) SURFACE_INPUT_COUNT };
#undef SURFACE_ENUM

#define SCALAR VKR_MATERIAL_VALUE_SCALAR
#define COLOR VKR_MATERIAL_VALUE_COLOR
#define NORMAL VKR_MATERIAL_VALUE_NORMAL
#define LAYERS VKR_MATERIAL_VALUE_LAYERS
#define SURFACE_PORT(id, name, label, type, factor, texture, channel, srgb)    \
  {name, label, type},
#define SURFACE_ROW(id, name, label, type, factor, texture, channel, srgb)     \
  {{name, label, type}, factor, texture, channel, srgb},
static const VkrMaterialPortDesc s_surface_in[SURFACE_INPUT_COUNT] = {
    SURFACE_INPUTS(SURFACE_PORT)};
static const SurfaceInput s_surface[SURFACE_INPUT_COUNT] = {
    SURFACE_INPUTS(SURFACE_ROW)};
#undef SURFACE_PORT
#undef SURFACE_ROW
#undef SCALAR
#undef COLOR
#undef NORMAL
#undef LAYERS

_Static_assert(SURFACE_INPUT_COUNT <= VKR_MATERIAL_GRAPH_INPUT_MAX,
               "the surface output's inputs fit a node");

static const VkrMaterialNodeDesc s_nodes[VKR_MATERIAL_NODE_KIND_COUNT] = {
    [VKR_MATERIAL_NODE_SCALAR] = {"scalar", "Scalar", "A constant number", NULL,
                                  0, s_scalar_out, 1, 0, 0},
    [VKR_MATERIAL_NODE_COLOR] = {"color", "Colour", "A constant linear colour",
                                 NULL, 0, s_color_out, 1, 0, 0},
    [VKR_MATERIAL_NODE_TEXTURE] = {"texture", "Texture",
                                   "A texture file: its colour and channels, "
                                   "at its own UVs when they connect",
                                   s_texture_in, ArrayCount(s_texture_in),
                                   s_texture_out, ArrayCount(s_texture_out), 1,
                                   0},
    [VKR_MATERIAL_NODE_MULTIPLY] = {"multiply", "Multiply",
                                    "A times B; a texture times a constant "
                                    "scales the texture",
                                    s_multiply_in, ArrayCount(s_multiply_in),
                                    s_multiply_out, 1, 0, 1, true_v},
    [VKR_MATERIAL_NODE_NORMAL_MAP] = {"normal_map", "Normal map",
                                      "A tangent-space normal map from a "
                                      "texture's RGB, with a strength",
                                      s_normal_in, ArrayCount(s_normal_in),
                                      s_normal_out, 1, 0, 2},
    [VKR_MATERIAL_NODE_LAYER] = {"layer", "Layer",
                                 "A layer material (.mt) whose base colour, "
                                 "normal and ORM a layer blend mixes in",
                                 NULL, 0, s_layer_out, 1, 3, 0},
    [VKR_MATERIAL_NODE_LAYER_BLEND] = {"layer_blend", "Layer blend",
                                       "Up to three layers over the surface, "
                                       "weighed by the vertex colour, a mask "
                                       "texture, slope or height",
                                       s_layer_blend_in,
                                       ArrayCount(s_layer_blend_in),
                                       s_layer_blend_out, 1, 0, 4},
    [VKR_MATERIAL_NODE_SURFACE_OUTPUT] = {"surface_output", "Surface output",
                                          "The material model's inputs",
                                          s_surface_in, SURFACE_INPUT_COUNT,
                                          NULL, 0, 0, 0},
#define CUSTOM_NODE(kind, name, label, tooltip, in, out, alu, generic)         \
  [kind] = {name, label, tooltip, in,    ArrayCount(in), out, ArrayCount(out), \
            0,    alu,   generic, true_v}
#define CUSTOM_INPUT(kind, name, label, tooltip, out, alu)                     \
  [kind] = {name, label, tooltip, NULL,  0, out, ArrayCount(out),              \
            0,    alu,   false_v, true_v}
    CUSTOM_INPUT(VKR_MATERIAL_NODE_UV, "uv", "UV",
                 "The mesh's texture "
                 "coordinates",
                 s_uv_out, 0),
    CUSTOM_INPUT(VKR_MATERIAL_NODE_VERTEX_COLOR, "vertex_color",
                 "Vertex colour", "The mesh's vertex colour and alpha",
                 s_vertex_color_out, 0),
    CUSTOM_INPUT(VKR_MATERIAL_NODE_WORLD_POSITION, "world_position",
                 "World position", "The shaded point in world meters",
                 s_position_out, 0),
    CUSTOM_INPUT(VKR_MATERIAL_NODE_WORLD_NORMAL, "world_normal", "World normal",
                 "The surface's unit normal in world space", s_world_normal_out,
                 1),
    CUSTOM_INPUT(VKR_MATERIAL_NODE_TIME, "time", "Time",
                 "Seconds the frame clock has run", s_seconds_out, 0),
    CUSTOM_INPUT(VKR_MATERIAL_NODE_CAMERA_DISTANCE, "camera_distance",
                 "Camera distance", "Meters from the camera", s_meters_out, 1),
    CUSTOM_NODE(VKR_MATERIAL_NODE_ADD, "add", "Add", "A plus B", s_binary_in,
                s_generic_out, 1, true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_SUBTRACT, "subtract", "Subtract", "A minus B",
                s_binary_in, s_generic_out, 1, true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_DIVIDE, "divide", "Divide",
                "A over B; a zero B gives zero", s_binary_in, s_generic_out, 2,
                true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_MIN, "min", "Min", "The smaller of A and B",
                s_binary_in, s_generic_out, 1, true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_MAX, "max", "Max", "The larger of A and B",
                s_binary_in, s_generic_out, 1, true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_LERP, "lerp", "Lerp",
                "A to B as T goes from 0 to 1", s_lerp_in, s_generic_out, 2,
                true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_POWER, "power", "Power",
                "X to the exponent; X below zero counts as zero", s_power_in,
                s_generic_out, 4, true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_ONE_MINUS, "one_minus", "One minus",
                "1 minus X", s_unary_in, s_generic_out, 1, true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_SATURATE, "saturate", "Saturate",
                "X clamped to 0 to 1", s_unary_in, s_generic_out, 1, true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_ABS, "abs", "Abs", "X without its sign",
                s_unary_in, s_generic_out, 1, true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_SINE, "sine", "Sine", "The sine of X radians",
                s_unary_in, s_generic_out, 4, true_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_DOT, "dot", "Dot",
                "The dot product of two three-component values", s_dot_in,
                s_scalar_out, 2, false_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_SPLIT, "split", "Split",
                "A three-component value's components", s_split_in, s_split_out,
                0, false_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_COMBINE, "combine", "Combine",
                "Three scalars as one value", s_combine_in, s_combine_out, 0,
                false_v),
    CUSTOM_NODE(VKR_MATERIAL_NODE_TILE_OFFSET, "tile_offset", "Tile and offset",
                "UVs times the tiling (value x, y) plus the offset (z, w)",
                s_tile_in, s_uv_out, 2, false_v),
    CUSTOM_INPUT(VKR_MATERIAL_NODE_WORLD_PLANAR, "world_planar", "World planar",
                 "UVs from the world X and Z, one repeat per value x meters",
                 s_uv_out, 2),
#undef CUSTOM_NODE
#undef CUSTOM_INPUT
};

const VkrMaterialNodeDesc *vkr_material_node_desc(VkrMaterialNodeKind kind) {
  if ((uint32_t)kind >= VKR_MATERIAL_NODE_KIND_COUNT) {
    return NULL;
  }
  return &s_nodes[kind];
}

static bool8_t graph_equals(String8 a, const char *b) {
  const uint64_t length = strlen(b);
  return a.length == length && (length == 0 || MemCompare(a.str, b, length) == 0);
}

bool8_t vkr_material_node_find(String8 name, VkrMaterialNodeKind *out) {
  for (uint32_t i = 0; i < VKR_MATERIAL_NODE_KIND_COUNT; ++i) {
    if (graph_equals(name, s_nodes[i].name)) {
      *out = (VkrMaterialNodeKind)i;
      return true_v;
    }
  }
  return false_v;
}

uint32_t vkr_material_node_input(VkrMaterialNodeKind kind, String8 name) {
  const VkrMaterialNodeDesc *desc = vkr_material_node_desc(kind);
  for (uint32_t i = 0; desc && i < desc->input_count; ++i) {
    if (graph_equals(name, desc->inputs[i].name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

uint32_t vkr_material_node_output(VkrMaterialNodeKind kind, String8 name) {
  const VkrMaterialNodeDesc *desc = vkr_material_node_desc(kind);
  for (uint32_t i = 0; desc && i < desc->output_count; ++i) {
    if (graph_equals(name, desc->outputs[i].name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

// =============================================================================
// Graphs
// =============================================================================

static bool8_t graph_fail(char *error, uint32_t capacity, const char *format,
                          ...) {
  if (error && capacity) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, capacity, format, args);
    va_end(args);
  }
  return false_v;
}

void vkr_material_graph_init(VkrMaterialGraph *graph) {
  MemZero(graph, sizeof(*graph));
  graph->settings = (VkrMaterialGraphSettings){
      .alpha_mode = VKR_MATERIAL_GRAPH_ALPHA_INFER,
      .alpha_cutoff = -1.0f,
      .temporal_reactivity = -1.0f,
      .roughness_max = -1.0f,
  };
}

uint32_t vkr_material_graph_find(const VkrMaterialGraph *graph, String8 id) {
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    if (graph_equals(id, graph->nodes[i].id)) {
      return i;
    }
  }
  return UINT32_MAX;
}

/* Whether `name` is 1 to 31 of [a-z0-9_]. */
static bool8_t graph_name_valid(const char *name) {
  const uint64_t length = strlen(name);
  if (length == 0 || length >= VKR_MATERIAL_GRAPH_ID_CAPACITY) {
    return false_v;
  }
  for (uint64_t i = 0; i < length; ++i) {
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      return false_v;
    }
  }
  return true_v;
}

/* Components of a scalar, two- or three-component value; zero for the
   other types. */
static uint32_t graph_components(VkrMaterialValueType type) {
  return type == VKR_MATERIAL_VALUE_SCALAR  ? 1u
         : type == VKR_MATERIAL_VALUE_VEC2  ? 2u
         : type == VKR_MATERIAL_VALUE_COLOR ? 3u
                                            : 0u;
}

/* The value output `port` of node `index` carries; a generic node's has the
   most components of its inputs typed colour. `depth` bounds the walk on a
   cycle. */
static VkrMaterialValueType graph_output_type(const VkrMaterialGraph *graph,
                                              uint32_t index, uint32_t port,
                                              uint32_t depth) {
  const VkrMaterialNode *node = &graph->nodes[index];
  const VkrMaterialNodeDesc *desc = vkr_material_node_desc(node->kind);
  if (!desc->generic) {
    return desc->outputs[port].type;
  }
  if (depth > VKR_MATERIAL_GRAPH_NODE_MAX) {
    return VKR_MATERIAL_VALUE_SCALAR;
  }
  VkrMaterialValueType widest = VKR_MATERIAL_VALUE_SCALAR;
  for (uint32_t i = 0; i < desc->input_count; ++i) {
    const VkrMaterialLink link = node->inputs[i];
    if (!link.node || desc->inputs[i].type != VKR_MATERIAL_VALUE_COLOR) {
      continue;
    }
    const VkrMaterialValueType type =
        graph_output_type(graph, link.node - 1u, link.port, depth + 1u);
    if (graph_components(type) > graph_components(widest)) {
      widest = type;
    }
  }
  return widest;
}

VkrMaterialValueType
vkr_material_graph_output_type(const VkrMaterialGraph *graph, uint32_t index,
                               uint32_t port) {
  return graph_output_type(graph, index, port, 0u);
}

/* Whether a `from` value may feed a `to` input: the same type, or a scalar
   or two-component value into a generic node's colour input. */
static bool8_t graph_types_fit(const VkrMaterialNodeDesc *desc,
                               VkrMaterialValueType from,
                               VkrMaterialValueType to) {
  return from == to || (desc->generic && to == VKR_MATERIAL_VALUE_COLOR &&
                        (from == VKR_MATERIAL_VALUE_SCALAR ||
                         from == VKR_MATERIAL_VALUE_VEC2));
}

bool8_t vkr_material_graph_validate(const VkrMaterialGraph *graph, char *error,
                                    uint32_t capacity) {
  if (graph->node_count > VKR_MATERIAL_GRAPH_NODE_MAX) {
    return graph_fail(error, capacity, "A graph holds at most %u nodes",
                      VKR_MATERIAL_GRAPH_NODE_MAX);
  }
  uint32_t outputs = 0u;
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    const VkrMaterialNode *node = &graph->nodes[i];
    const VkrMaterialNodeDesc *desc = vkr_material_node_desc(node->kind);
    if (!desc) {
      return graph_fail(error, capacity, "Node %u has no type", i);
    }
    if (!graph_name_valid(node->id)) {
      return graph_fail(error, capacity,
                        "Node id '%s' is not 1 to 31 of a-z, 0-9 and _",
                        node->id);
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (strcmp(graph->nodes[j].id, node->id) == 0) {
        return graph_fail(error, capacity, "Node id '%s' repeats", node->id);
      }
    }
    if (node->parameter[0]) {
      if (node->kind != VKR_MATERIAL_NODE_SCALAR &&
          node->kind != VKR_MATERIAL_NODE_COLOR &&
          node->kind != VKR_MATERIAL_NODE_TEXTURE &&
          node->kind != VKR_MATERIAL_NODE_LAYER) {
        return graph_fail(error, capacity,
                          "Node '%s': only scalars, colours, textures and "
                          "layers are parameters",
                          node->id);
      }
      if (!graph_name_valid(node->parameter)) {
        return graph_fail(error, capacity,
                          "Node '%s': parameter '%s' is not 1 to 31 of a-z, "
                          "0-9 and _",
                          node->id, node->parameter);
      }
      for (uint32_t j = 0; j < i; ++j) {
        if (strcmp(graph->nodes[j].parameter, node->parameter) == 0) {
          return graph_fail(error, capacity, "Parameter '%s' repeats",
                            node->parameter);
        }
      }
    }
    if (node->kind == VKR_MATERIAL_NODE_SURFACE_OUTPUT && ++outputs > 1u) {
      return graph_fail(error, capacity, "A graph has one surface output");
    }
    for (uint32_t port = 0; port < VKR_MATERIAL_GRAPH_INPUT_MAX; ++port) {
      const VkrMaterialLink link = node->inputs[port];
      if (!link.node) {
        continue;
      }
      if (port >= desc->input_count) {
        return graph_fail(error, capacity, "Node '%s' has no input %u",
                          node->id, port);
      }
      if (link.node > graph->node_count) {
        return graph_fail(error, capacity,
                          "Node '%s': input '%s' names no node", node->id,
                          desc->inputs[port].name);
      }
      const VkrMaterialNode *source = &graph->nodes[link.node - 1u];
      const VkrMaterialNodeDesc *source_desc =
          vkr_material_node_desc(source->kind);
      if (!source_desc || link.port >= source_desc->output_count) {
        return graph_fail(error, capacity,
                          "Node '%s': input '%s' names no output of '%s'",
                          node->id, desc->inputs[port].name, source->id);
      }
    }
  }
  /* Cycles: a depth-first walk that colours nodes on its path. */
  uint8_t state[VKR_MATERIAL_GRAPH_NODE_MAX] = {0};
  uint32_t stack[VKR_MATERIAL_GRAPH_NODE_MAX];
  uint32_t next_port[VKR_MATERIAL_GRAPH_NODE_MAX];
  for (uint32_t root = 0; root < graph->node_count; ++root) {
    if (state[root]) {
      continue;
    }
    uint32_t depth = 0u;
    stack[depth] = root;
    next_port[depth] = 0u;
    state[root] = 1u;
    while (true_v) {
      const uint32_t at = stack[depth];
      const VkrMaterialNode *node = &graph->nodes[at];
      if (next_port[depth] < VKR_MATERIAL_GRAPH_INPUT_MAX) {
        const VkrMaterialLink link = node->inputs[next_port[depth]++];
        if (!link.node) {
          continue;
        }
        const uint32_t to = link.node - 1u;
        if (state[to] == 1u) {
          return graph_fail(error, capacity, "Node '%s' feeds itself",
                            graph->nodes[to].id);
        }
        if (state[to] == 0u) {
          state[to] = 1u;
          stack[++depth] = to;
          next_port[depth] = 0u;
        }
        continue;
      }
      state[at] = 2u;
      if (depth == 0u) {
        break;
      }
      depth--;
    }
  }
  /* Types, once links and cycles are known good. */
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    const VkrMaterialNode *node = &graph->nodes[i];
    const VkrMaterialNodeDesc *desc = vkr_material_node_desc(node->kind);
    for (uint32_t port = 0; port < desc->input_count; ++port) {
      const VkrMaterialLink link = node->inputs[port];
      if (!link.node) {
        continue;
      }
      const VkrMaterialValueType from =
          graph_output_type(graph, link.node - 1u, link.port, 0u);
      if (!graph_types_fit(desc, from, desc->inputs[port].type)) {
        static const char *const types[VKR_MATERIAL_VALUE_TYPE_COUNT] = {
            "scalar", "colour", "normal", "layer", "layers", "UV"};
        return graph_fail(error, capacity,
                          "Node '%s': input '%s' takes a %s, not a %s",
                          node->id, desc->inputs[port].name,
                          types[desc->inputs[port].type], types[from]);
      }
    }
  }
  return true_v;
}

uint32_t vkr_material_graph_add(VkrMaterialGraph *graph,
                                VkrMaterialNodeKind kind, const char *base) {
  if (graph->node_count == VKR_MATERIAL_GRAPH_NODE_MAX ||
      !vkr_material_node_desc(kind)) {
    return UINT32_MAX;
  }
  char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  snprintf(id, sizeof(id), "%s", base);
  for (uint32_t n = 2u;
       vkr_material_graph_find(graph, string8_create_from_cstr(
                                          (const uint8_t *)id, strlen(id))) !=
       UINT32_MAX;
       ++n) {
    snprintf(id, sizeof(id), "%.24s_%u", base, n);
  }
  VkrMaterialNode *node = &graph->nodes[graph->node_count];
  MemZero(node, sizeof(*node));
  node->kind = kind;
  snprintf(node->id, sizeof(node->id), "%s", id);
  if (kind == VKR_MATERIAL_NODE_COLOR) {
    node->value = vec4_new(1.0f, 1.0f, 1.0f, 0.0f);
  } else if (kind == VKR_MATERIAL_NODE_SCALAR) {
    node->value.x = 1.0f;
  } else if (kind == VKR_MATERIAL_NODE_LAYER_BLEND) {
    /* Slope: layer 1 covers what faces up past about 45 degrees. */
    node->value = vec4_new(0.6f, 0.8f, 0.0f, 0.0f);
  } else if (kind == VKR_MATERIAL_NODE_TILE_OFFSET) {
    node->value = vec4_new(1.0f, 1.0f, 0.0f, 0.0f);
  } else if (kind == VKR_MATERIAL_NODE_WORLD_PLANAR) {
    node->value.x = 4.0f;
  }
  return graph->node_count++;
}

void vkr_material_graph_remove(VkrMaterialGraph *graph, uint32_t index) {
  if (index >= graph->node_count) {
    return;
  }
  MemCopy(&graph->nodes[index], &graph->nodes[index + 1u],
          (graph->node_count - index - 1u) * sizeof(graph->nodes[0]));
  graph->node_count--;
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    for (uint32_t port = 0; port < VKR_MATERIAL_GRAPH_INPUT_MAX; ++port) {
      VkrMaterialLink *link = &graph->nodes[i].inputs[port];
      if (link->node == index + 1u) {
        *link = (VkrMaterialLink){0};
      } else if (link->node > index + 1u) {
        link->node--;
      }
    }
  }
}

bool8_t vkr_material_graph_connect(VkrMaterialGraph *graph, uint32_t from,
                                   uint32_t from_port, uint32_t to,
                                   uint32_t to_port, char *error,
                                   uint32_t capacity) {
  if (from >= graph->node_count || to >= graph->node_count) {
    return graph_fail(error, capacity, "No such node");
  }
  const VkrMaterialNodeDesc *source = vkr_material_node_desc(graph->nodes[from].kind);
  const VkrMaterialNodeDesc *target = vkr_material_node_desc(graph->nodes[to].kind);
  if (from_port >= source->output_count || to_port >= target->input_count) {
    return graph_fail(error, capacity, "No such port");
  }
  VkrMaterialLink *input = &graph->nodes[to].inputs[to_port];
  const VkrMaterialLink previous = *input;
  *input = (VkrMaterialLink){.node = (uint16_t)(from + 1u),
                             .port = (uint8_t)from_port};
  if (!vkr_material_graph_validate(graph, error, capacity)) {
    *input = previous;
    return false_v;
  }
  return true_v;
}

// =============================================================================
// Text output
// =============================================================================

typedef struct GraphText {
  VkrAllocator *allocator;
  char *data;
  uint64_t length;
  uint64_t capacity;
  bool8_t failed;
} GraphText;

static void text_append(GraphText *text, const char *format, ...) {
  if (text->failed) {
    return;
  }
  va_list args;
  va_start(args, format);
  char line[1024];
  const int written = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (written < 0 || (uint64_t)written >= sizeof(line)) {
    text->failed = true_v;
    return;
  }
  const uint64_t needed = text->length + (uint64_t)written + 1u;
  if (needed > text->capacity) {
    const uint64_t next = Max(needed, Max(4096u, text->capacity * 2u));
    char *grown = text->data ? vkr_allocator_realloc(
                                   text->allocator, text->data, text->capacity,
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

static bool8_t text_finish(GraphText *text, String8 *out) {
  if (text->failed) {
    return false_v;
  }
  if (!text->data) {
    text_append(text, "%s", "");
    if (text->failed) {
      return false_v;
    }
  }
  *out = (String8){.str = (uint8_t *)text->data, .length = text->length};
  return true_v;
}

/* `value` as JSON string content: quotes, backslashes and controls escape. */
static void text_json_string(GraphText *text, const char *value) {
  text_append(text, "\"");
  for (const char *c = value; *c; ++c) {
    if (*c == '"' || *c == '\\') {
      text_append(text, "\\%c", *c);
    } else if ((unsigned char)*c < 0x20u) {
      text_append(text, "\\u%04x", (unsigned)(unsigned char)*c);
    } else {
      text_append(text, "%c", *c);
    }
  }
  text_append(text, "\"");
}

/* Nine significant digits give back the same float32. */
#define GRAPH_FLOAT "%.9g"

// =============================================================================
// JSON
// =============================================================================

static const char *const s_alpha_names[] = {"infer", "opaque", "mask",
                                            "blend"};
static const char *const s_color_space_names[] = {"auto", "srgb", "linear"};

/* `out` at the value of `object`'s root member `name`. */
static bool8_t json_member(const VkrJsonReader *object, const char *name,
                           VkrJsonReader *out) {
  *out = *object;
  return vkr_json_find_root_field(out, name);
}

static bool8_t json_floats(VkrJsonReader *reader, float32_t *out,
                           uint32_t count) {
  vkr_json_skip_whitespace(reader);
  if (reader->pos >= reader->length || reader->data[reader->pos] != '[') {
    return false_v;
  }
  reader->pos++;
  for (uint32_t i = 0; i < count; ++i) {
    vkr_json_skip_whitespace(reader);
    if (i > 0u) {
      if (reader->pos >= reader->length || reader->data[reader->pos] != ',') {
        return false_v;
      }
      reader->pos++;
    }
    if (!vkr_json_parse_float(reader, &out[i]) || !isfinite(out[i])) {
      return false_v;
    }
  }
  vkr_json_skip_whitespace(reader);
  return reader->pos < reader->length && reader->data[reader->pos] == ']';
}

static bool8_t json_text(const VkrJsonReader *object, const char *name,
                         char *out, uint32_t capacity) {
  VkrJsonReader member;
  String8 text = {0};
  if (!json_member(object, name, &member)) {
    return true_v;
  }
  if (!vkr_json_parse_string(&member, &text) || text.length >= capacity ||
      memchr(text.str, '\\', text.length)) {
    return false_v;
  }
  MemCopy(out, text.str, text.length);
  out[text.length] = '\0';
  return true_v;
}

static bool8_t json_enum(const VkrJsonReader *object, const char *name,
                         const char *const *names, uint32_t count,
                         uint32_t *out) {
  VkrJsonReader member;
  String8 text = {0};
  if (!json_member(object, name, &member)) {
    return true_v;
  }
  if (!vkr_json_parse_string(&member, &text)) {
    return false_v;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (graph_equals(text, names[i])) {
      *out = i;
      return true_v;
    }
  }
  return false_v;
}

static bool8_t json_number(const VkrJsonReader *object, const char *name,
                           float32_t *out) {
  VkrJsonReader member;
  if (!json_member(object, name, &member)) {
    return true_v;
  }
  return vkr_json_parse_float(&member, out) && isfinite(*out);
}

/* A link written "node" (its first output) or "node.port". */
static bool8_t graph_parse_link(const VkrMaterialGraph *graph, String8 text,
                                VkrMaterialLink *out) {
  uint64_t dot = 0;
  while (dot < text.length && text.str[dot] != '.') {
    dot++;
  }
  const String8 id = {.str = text.str, .length = dot};
  const uint32_t node = vkr_material_graph_find(graph, id);
  if (node == UINT32_MAX) {
    return false_v;
  }
  uint32_t port = 0u;
  if (dot < text.length) {
    const String8 name = {.str = text.str + dot + 1u,
                          .length = text.length - dot - 1u};
    port = vkr_material_node_output(graph->nodes[node].kind, name);
    if (port == UINT32_MAX) {
      return false_v;
    }
  }
  *out = (VkrMaterialLink){.node = (uint16_t)(node + 1u), .port = (uint8_t)port};
  return true_v;
}

bool8_t vkr_material_graph_read(String8 json, VkrMaterialGraph *out,
                                char *error, uint32_t capacity) {
  vkr_material_graph_init(out);
  VkrJsonReader root = vkr_json_reader_from_string(json);
  VkrJsonReader member;
  float32_t version = 0.0f;
  if (!json_member(&root, "version", &member) ||
      !vkr_json_parse_float(&member, &version) ||
      version != (float32_t)VKR_MATERIAL_GRAPH_VERSION) {
    return graph_fail(error, capacity, "A graph needs \"version\": %u",
                      VKR_MATERIAL_GRAPH_VERSION);
  }
  if (json_member(&root, "settings", &member)) {
    VkrJsonReader settings;
    uint32_t alpha = (uint32_t)out->settings.alpha_mode;
    float32_t profile = 0.0f;
    if (!vkr_json_enter_object(&member, &settings) ||
        !json_enum(&settings, "alpha_mode", s_alpha_names,
                   ArrayCount(s_alpha_names), &alpha) ||
        !json_number(&settings, "alpha_cutoff", &out->settings.alpha_cutoff) ||
        !json_number(&settings, "temporal_reactivity",
                     &out->settings.temporal_reactivity) ||
        !json_number(&settings, "roughness_max",
                     &out->settings.roughness_max) ||
        !json_number(&settings, "subsurface_profile", &profile) ||
        profile < 0.0f || profile > 7.0f || profile != floorf(profile)) {
      return graph_fail(error, capacity, "The graph's settings are malformed");
    }
    out->settings.alpha_mode = (VkrMaterialGraphAlpha)alpha;
    out->settings.subsurface_profile = (uint32_t)profile;
    float32_t world_size[2] = {0.0f, 0.0f};
    if (json_member(&settings, "world_size", &member) &&
        (!json_floats(&member, world_size, 2u) || world_size[0] < 0.0f ||
         world_size[1] < 0.0f)) {
      return graph_fail(error, capacity,
                        "\"world_size\" is two sizes in meters");
    }
    out->settings.world_size = vec2_new(world_size[0], world_size[1]);
    if (!json_text(&settings, "surface", out->settings.surface,
                   sizeof(out->settings.surface))) {
      return graph_fail(error, capacity, "\"surface\" is a tag name");
    }
    if (json_member(&settings, "double_sided", &member) &&
        !vkr_json_parse_bool(&member, &out->settings.double_sided)) {
      return graph_fail(error, capacity, "\"double_sided\" is true or false");
    }
  }
  /* Nodes: types and fields first, so links may name later nodes. */
  VkrJsonReader nodes = root;
  if (!vkr_json_find_root_field(&nodes, "nodes")) {
    return graph_fail(error, capacity, "A graph needs a \"nodes\" array");
  }
  vkr_json_skip_whitespace(&nodes);
  if (nodes.pos >= nodes.length || nodes.data[nodes.pos] != '[') {
    return graph_fail(error, capacity, "\"nodes\" must be an array");
  }
  nodes.pos++;
  VkrJsonReader objects[VKR_MATERIAL_GRAPH_NODE_MAX];
  while (vkr_json_next_array_element(&nodes)) {
    if (out->node_count == VKR_MATERIAL_GRAPH_NODE_MAX) {
      return graph_fail(error, capacity, "A graph holds at most %u nodes",
                        VKR_MATERIAL_GRAPH_NODE_MAX);
    }
    VkrJsonReader *object = &objects[out->node_count];
    if (!vkr_json_enter_object(&nodes, object)) {
      return graph_fail(error, capacity, "Node %u is not an object",
                        out->node_count);
    }
    VkrMaterialNode *node = &out->nodes[out->node_count++];
    String8 type = {0};
    uint32_t color_space = 0u;
    if (!json_text(object, "id", node->id, sizeof(node->id)) ||
        !json_member(object, "type", &member) ||
        !vkr_json_parse_string(&member, &type) ||
        !vkr_material_node_find(type, &node->kind) ||
        !json_text(object, "parameter", node->parameter,
                   sizeof(node->parameter)) ||
        !json_text(object, "path", node->path, sizeof(node->path)) ||
        !json_enum(object, "color_space", s_color_space_names,
                   ArrayCount(s_color_space_names), &color_space)) {
      return graph_fail(error, capacity,
                        "Node %u needs an \"id\", a known \"type\" and valid "
                        "fields",
                        out->node_count - 1u);
    }
    node->color_space = (VkrMaterialColorSpace)color_space;
    uint32_t mask = 0u;
    if (!json_enum(object, "mask", vkr_material_graph_mask_names,
                   VKR_MATERIAL_GRAPH_MASK_COUNT, &mask)) {
      return graph_fail(error, capacity,
                        "Node '%s': mask is vertex_color, texture, slope or "
                        "height",
                        node->id);
    }
    node->mask = (VkrMaterialGraphMask)mask;
    if (node->kind == VKR_MATERIAL_NODE_LAYER_BLEND) {
      node->value = vec4_new(0.0f, 1.0f, 0.0f, 0.0f);
    }
    if (json_member(object, "value", &member)) {
      const bool8_t ok = node->kind == VKR_MATERIAL_NODE_COLOR
                             ? json_floats(&member, &node->value.x, 3u)
                         : node->kind == VKR_MATERIAL_NODE_LAYER_BLEND
                             ? json_floats(&member, &node->value.x, 2u)
                         : node->kind == VKR_MATERIAL_NODE_TILE_OFFSET
                             ? json_floats(&member, &node->value.x, 4u)
                             : vkr_json_parse_float(&member, &node->value.x) &&
                                   isfinite(node->value.x);
      if (!ok) {
        return graph_fail(error, capacity,
                          "Node '%s': a scalar's value is a number, a "
                          "colour's [r, g, b], a layer blend's [from, to]",
                          node->id);
      }
    }
    if (json_member(object, "position", &member) &&
        !json_floats(&member, &node->position.x, 2u)) {
      return graph_fail(error, capacity, "Node '%s': position is [x, y]",
                        node->id);
    }
  }
  for (uint32_t i = 0; i < out->node_count; ++i) {
    VkrMaterialNode *node = &out->nodes[i];
    VkrJsonReader inputs;
    if (!json_member(&objects[i], "inputs", &member)) {
      continue;
    }
    if (!vkr_json_enter_object(&member, &inputs)) {
      return graph_fail(error, capacity, "Node '%s': inputs is an object",
                        node->id);
    }
    const VkrMaterialNodeDesc *desc = vkr_material_node_desc(node->kind);
    for (uint32_t port = 0; port < desc->input_count; ++port) {
      String8 text = {0};
      if (!json_member(&inputs, desc->inputs[port].name, &member)) {
        continue;
      }
      if (!vkr_json_parse_string(&member, &text) ||
          !graph_parse_link(out, text, &node->inputs[port])) {
        return graph_fail(error, capacity,
                          "Node '%s': input '%s' names no node output",
                          node->id, desc->inputs[port].name);
      }
    }
  }
  return vkr_material_graph_validate(out, error, capacity);
}

bool8_t vkr_material_graph_write(const VkrMaterialGraph *graph,
                                 VkrAllocator *allocator, String8 *out_json) {
  GraphText text = {.allocator = allocator};
  const VkrMaterialGraphSettings *settings = &graph->settings;
  text_append(&text, "{\n  \"version\": %u,\n  \"settings\": {",
              VKR_MATERIAL_GRAPH_VERSION);
  text_append(&text, "\"alpha_mode\": \"%s\", \"alpha_cutoff\": " GRAPH_FLOAT,
              s_alpha_names[settings->alpha_mode],
              (double)settings->alpha_cutoff);
  text_append(
      &text,
      ", \"double_sided\": %s, \"subsurface_profile\": %u, "
      "\"temporal_reactivity\": " GRAPH_FLOAT
      ", \"roughness_max\": " GRAPH_FLOAT,
      settings->double_sided ? "true" : "false", settings->subsurface_profile,
      (double)settings->temporal_reactivity, (double)settings->roughness_max);
  /* Art-pass metadata, only when set. */
  if (settings->world_size.x > 0.0f) {
    text_append(&text, ", \"world_size\": [" GRAPH_FLOAT ", " GRAPH_FLOAT "]",
                (double)settings->world_size.x, (double)settings->world_size.y);
  }
  if (settings->surface[0]) {
    text_append(&text, ", \"surface\": ");
    text_json_string(&text, settings->surface);
  }
  text_append(&text, "},\n  \"nodes\": [");
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    const VkrMaterialNode *node = &graph->nodes[i];
    const VkrMaterialNodeDesc *desc = vkr_material_node_desc(node->kind);
    text_append(&text, "%s\n    {\"id\": ", i ? "," : "");
    text_json_string(&text, node->id);
    text_append(&text, ", \"type\": \"%s\"", desc->name);
    if (node->parameter[0]) {
      text_append(&text, ", \"parameter\": \"%s\"", node->parameter);
    }
    if (node->kind == VKR_MATERIAL_NODE_SCALAR ||
        node->kind == VKR_MATERIAL_NODE_WORLD_PLANAR) {
      text_append(&text, ", \"value\": " GRAPH_FLOAT, (double)node->value.x);
    } else if (node->kind == VKR_MATERIAL_NODE_TILE_OFFSET) {
      text_append(&text,
                  ", \"value\": [" GRAPH_FLOAT ", " GRAPH_FLOAT ", " GRAPH_FLOAT
                  ", " GRAPH_FLOAT "]",
                  (double)node->value.x, (double)node->value.y,
                  (double)node->value.z, (double)node->value.w);
    } else if (node->kind == VKR_MATERIAL_NODE_COLOR) {
      text_append(&text,
                  ", \"value\": [" GRAPH_FLOAT ", " GRAPH_FLOAT
                  ", " GRAPH_FLOAT "]",
                  (double)node->value.x, (double)node->value.y,
                  (double)node->value.z);
    } else if (node->kind == VKR_MATERIAL_NODE_TEXTURE) {
      text_append(&text, ", \"path\": ");
      text_json_string(&text, node->path);
      if (node->color_space != VKR_MATERIAL_COLOR_SPACE_AUTO) {
        text_append(&text, ", \"color_space\": \"%s\"",
                    s_color_space_names[node->color_space]);
      }
    } else if (node->kind == VKR_MATERIAL_NODE_LAYER) {
      text_append(&text, ", \"path\": ");
      text_json_string(&text, node->path);
    } else if (node->kind == VKR_MATERIAL_NODE_LAYER_BLEND) {
      text_append(&text, ", \"mask\": \"%s\"",
                  vkr_material_graph_mask_names[node->mask]);
      text_append(&text, ", \"value\": [" GRAPH_FLOAT ", " GRAPH_FLOAT "]",
                  (double)node->value.x, (double)node->value.y);
      if (node->path[0]) {
        text_append(&text, ", \"path\": ");
        text_json_string(&text, node->path);
      }
    }
    text_append(&text,
                ", \"position\": [" GRAPH_FLOAT ", " GRAPH_FLOAT "]",
                (double)node->position.x, (double)node->position.y);
    bool8_t first = true_v;
    for (uint32_t port = 0; port < desc->input_count; ++port) {
      const VkrMaterialLink link = node->inputs[port];
      if (!link.node) {
        continue;
      }
      const VkrMaterialNode *source = &graph->nodes[link.node - 1u];
      const VkrMaterialNodeDesc *source_desc =
          vkr_material_node_desc(source->kind);
      text_append(&text, "%s\"%s\": \"%s.%s\"",
                  first ? ", \"inputs\": {" : ", ", desc->inputs[port].name,
                  source->id, source_desc->outputs[link.port].name);
      first = false_v;
    }
    text_append(&text, "%s}", first ? "" : "}");
  }
  text_append(&text, "\n  ]\n}\n");
  return text_finish(&text, out_json);
}

// =============================================================================
// Lowering
// =============================================================================

typedef struct LowerContext {
  const VkrMaterialGraph *graph;
  String8 graph_path;
  const VkrMaterialParam *params;
  uint32_t param_count;
  VkrAllocator *allocator;
  VkrMaterialLowering *result;
  /* Texture node of each slot key written, so two inputs sharing a slot
     must share the file. */
  const char *slot_keys[SURFACE_INPUT_COUNT];
  const VkrMaterialNode *slot_nodes[SURFACE_INPUT_COUNT];
  uint32_t slot_count;
  /* Distinct texture nodes sampled. */
  const VkrMaterialNode *sampled[VKR_MATERIAL_GRAPH_NODE_MAX];
  uint32_t sampled_count;
} LowerContext;

static bool8_t lower_fail(LowerContext *context, const VkrMaterialNode *node,
                          const char *format, ...) {
  VkrMaterialLowering *result = context->result;
  result->tier = VKR_MATERIAL_TIER_UNSUPPORTED;
  snprintf(result->node, sizeof(result->node), "%s", node ? node->id : "");
  va_list args;
  va_start(args, format);
  vsnprintf(result->reason, sizeof(result->reason), format, args);
  va_end(args);
  return false_v;
}

/* The instance's override of `node`'s parameter, or NULL. */
static const char *lower_override(const LowerContext *context,
                                  const VkrMaterialNode *node) {
  if (!node->parameter[0]) {
    return NULL;
  }
  for (uint32_t i = 0; i < context->param_count; ++i) {
    if (strcmp(context->params[i].name, node->parameter) == 0) {
      return context->params[i].value;
    }
  }
  return NULL;
}

/* A constant node's value with its override; false for a bad override. */
static bool8_t lower_constant(LowerContext *context,
                              const VkrMaterialNode *node, Vec4 *out) {
  *out = node->value;
  const char *text = lower_override(context, node);
  if (!text) {
    return true_v;
  }
  const String8 value =
      string8_create_from_cstr((const uint8_t *)text, strlen(text));
  if (node->kind == VKR_MATERIAL_NODE_SCALAR) {
    if (!string8_to_f32(&value, &out->x) || !isfinite(out->x)) {
      return lower_fail(context, node, "Parameter '%s' needs a number, not '%s'",
                        node->parameter, text);
    }
    return true_v;
  }
  Vec3 color = {0};
  if (!string8_to_vec3(&value, &color) || !isfinite(color.x) ||
      !isfinite(color.y) || !isfinite(color.z)) {
    return lower_fail(context, node, "Parameter '%s' needs r,g,b, not '%s'",
                      node->parameter, text);
  }
  *out = vec4_new(color.x, color.y, color.z, 0.0f);
  return true_v;
}

/* The path a texture node lowers to: an instance override as written, which
   the loader resolves against the instance, or the graph's own path made
   content-root relative against the graph file. */
static const char *lower_texture_path(LowerContext *context,
                                      const VkrMaterialNode *node) {
  const char *text = lower_override(context, node);
  if (text) {
    return text;
  }
  const String8 raw =
      string8_create_from_cstr((const uint8_t *)node->path, strlen(node->path));
  const String8 resolved =
      vkr_asset_path_resolve(context->allocator, context->graph_path, raw);
  return resolved.str ? (const char *)resolved.str : NULL;
}

/* What feeds one surface input: an optional texture output, an optional
   constant factor. */
typedef struct LowerSource {
  const VkrMaterialNode *texture;
  uint32_t port;
  const VkrMaterialNode *constant;
  Vec4 factor;
  bool8_t has_factor;
  /* A normal map's strength. */
  const VkrMaterialNode *strength;
} LowerSource;

/* Reads a Standard chain into `out`: a constant, a texture output, or a
   multiply of one constant and one texture output (two constants fold). */
static bool8_t lower_source(LowerContext *context, VkrMaterialLink link,
                            LowerSource *out) {
  const VkrMaterialGraph *graph = context->graph;
  const VkrMaterialNode *node = &graph->nodes[link.node - 1u];
  switch (node->kind) {
  case VKR_MATERIAL_NODE_SCALAR:
  case VKR_MATERIAL_NODE_COLOR: {
    Vec4 value = {0};
    if (!lower_constant(context, node, &value)) {
      return false_v;
    }
    if (out->has_factor) {
      out->factor = vec4_new(out->factor.x * value.x,
                             out->factor.y * (node->kind == VKR_MATERIAL_NODE_SCALAR
                                                  ? value.x
                                                  : value.y),
                             out->factor.z * (node->kind == VKR_MATERIAL_NODE_SCALAR
                                                  ? value.x
                                                  : value.z),
                             0.0f);
    } else {
      out->factor = node->kind == VKR_MATERIAL_NODE_SCALAR
                        ? vec4_new(value.x, value.x, value.x, 0.0f)
                        : value;
      out->has_factor = true_v;
    }
    out->constant = node;
    return true_v;
  }
  case VKR_MATERIAL_NODE_TEXTURE:
    if (out->texture) {
      return lower_fail(context, node,
                        "Two textures multiply; blending textures needs a "
                        "Layered or Custom graph");
    }
    if (node->inputs[0].node) {
      return lower_fail(context, node,
                        "A texture at UVs of its own needs a Custom graph");
    }
    out->texture = node;
    out->port = link.port;
    return true_v;
  case VKR_MATERIAL_NODE_MULTIPLY: {
    for (uint32_t i = 0; i < ArrayCount(s_multiply_in); ++i) {
      if (!node->inputs[i].node) {
        return lower_fail(context, node, "Multiply needs both inputs");
      }
      if (!lower_source(context, node->inputs[i], out)) {
        return false_v;
      }
    }
    return true_v;
  }
  case VKR_MATERIAL_NODE_NORMAL_MAP: {
    /* Without a texture it carries only its strength, as a definition
       with a normal scale and no normal texture does. */
    const VkrMaterialLink texture = node->inputs[0];
    if (texture.node &&
        (graph->nodes[texture.node - 1u].kind != VKR_MATERIAL_NODE_TEXTURE ||
         texture.port != 0u)) {
      return lower_fail(context, node,
                        "A Standard normal map reads a texture's RGB");
    }
    if (texture.node) {
      out->texture = &graph->nodes[texture.node - 1u];
      out->port = 0u;
    }
    if (node->inputs[1].node) {
      const VkrMaterialNode *strength = &graph->nodes[node->inputs[1].node - 1u];
      if (strength->kind != VKR_MATERIAL_NODE_SCALAR) {
        return lower_fail(context, node,
                          "A Standard normal map's strength is a scalar");
      }
      Vec4 value = {0};
      if (!lower_constant(context, strength, &value)) {
        return false_v;
      }
      out->factor = vec4_new(value.x, value.x, value.x, 0.0f);
      out->has_factor = true_v;
      out->strength = strength;
    }
    return true_v;
  }
  default:
    return lower_fail(context, node, "'%s' does not feed a surface input",
                      vkr_material_node_desc(node->kind)->label);
  }
}

static void lower_sampled(LowerContext *context, const VkrMaterialNode *node) {
  for (uint32_t i = 0; i < context->sampled_count; ++i) {
    if (context->sampled[i] == node || strcmp(context->sampled[i]->path,
                                              node->path) == 0) {
      return;
    }
  }
  context->sampled[context->sampled_count++] = node;
}

/* The colour space key text of slot key `texture_key`: "base_color" for
   "base_color_texture". */
static void lower_color_space(GraphText *text, const char *texture_key,
                              const VkrMaterialNode *node, bool8_t srgb) {
  const uint64_t stem = strlen(texture_key) - strlen("_texture");
  const VkrMaterialColorSpace space =
      node->color_space != VKR_MATERIAL_COLOR_SPACE_AUTO
          ? node->color_space
          : (srgb ? VKR_MATERIAL_COLOR_SPACE_SRGB
                  : VKR_MATERIAL_COLOR_SPACE_AUTO);
  if (space == VKR_MATERIAL_COLOR_SPACE_AUTO) {
    return;
  }
  text_append(text, "%.*s_colorspace=%s\n", (int)stem, texture_key,
              space == VKR_MATERIAL_COLOR_SPACE_SRGB ? "srgb" : "linear");
}

/* A layer blend's `.mt` keys: each layer's file, then its mask. A layer
   samples its base colour, normal and ORM, a mask texture one more. */
static bool8_t lower_layers(LowerContext *context, GraphText *text,
                            const VkrMaterialNode *blend) {
  const VkrMaterialGraph *graph = context->graph;
  VkrMaterialLowering *result = context->result;
  for (uint32_t i = 0; i < ArrayCount(s_layer_blend_in); ++i) {
    if (!blend->inputs[i].node) {
      continue;
    }
    const VkrMaterialNode *layer = &graph->nodes[blend->inputs[i].node - 1u];
    if (!layer->path[0] && !lower_override(context, layer)) {
      return lower_fail(context, layer, "The layer names no material file");
    }
    const char *path = lower_texture_path(context, layer);
    if (!path) {
      return lower_fail(context, layer, "The layer path '%s' is not valid",
                        layer->path);
    }
    text_append(text, "layer%u=%s\n", i + 1u, path);
    result->layers++;
    result->samples += 3u;
  }
  if (!result->layers) {
    return lower_fail(context, blend, "A layer blend needs a layer");
  }
  if (blend->mask != VKR_MATERIAL_GRAPH_MASK_VERTEX_COLOR) {
    text_append(text, "layer_mask=%s\n",
                vkr_material_graph_mask_names[blend->mask]);
  }
  if (blend->mask == VKR_MATERIAL_GRAPH_MASK_SLOPE ||
      blend->mask == VKR_MATERIAL_GRAPH_MASK_HEIGHT) {
    if (!(blend->value.x < blend->value.y)) {
      return lower_fail(context, blend,
                        "A slope or height blend's range must rise");
    }
    text_append(text, "layer_mask_range=" GRAPH_FLOAT "," GRAPH_FLOAT "\n",
                (double)blend->value.x, (double)blend->value.y);
  }
  if (blend->mask == VKR_MATERIAL_GRAPH_MASK_TEXTURE) {
    const String8 raw = string8_create_from_cstr((const uint8_t *)blend->path,
                                                 strlen(blend->path));
    const String8 resolved =
        raw.length ? vkr_asset_path_resolve(context->allocator,
                                            context->graph_path, raw)
                   : (String8){0};
    if (!resolved.str) {
      return lower_fail(context, blend,
                        "A texture mask names its texture file");
    }
    text_append(text, "layer_mask_texture=%.*s\n", (int)resolved.length,
                (const char *)resolved.str);
    result->samples += 1u;
  }
  return true_v;
}

static bool8_t lower_standard(const VkrMaterialGraph *graph, String8 graph_path,
                              const VkrMaterialParam *params,
                              uint32_t param_count, VkrAllocator *allocator,
                              String8 *out_definition,
                              VkrMaterialLowering *out);

/* Whether a graph uses a node only the Custom tier evaluates. */
static bool8_t graph_needs_custom(const VkrMaterialGraph *graph) {
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    const VkrMaterialNode *node = &graph->nodes[i];
    if (vkr_material_node_desc(node->kind)->custom ||
        (node->kind == VKR_MATERIAL_NODE_TEXTURE && node->inputs[0].node)) {
      return true_v;
    }
  }
  return false_v;
}

/* A Custom graph's definition: the Standard fallback its constant outputs
   give, its function, and its parameters and textures as row data. */
static bool8_t lower_custom(const VkrMaterialGraph *graph, String8 graph_path,
                            const VkrMaterialParam *params,
                            uint32_t param_count, VkrAllocator *allocator,
                            String8 *out_definition, VkrMaterialLowering *out) {
  VkrMaterialCustomBinding binding;
  String8 source = {0};
  if (!vkr_material_codegen_msl(graph, allocator, &source, &binding, out)) {
    return false_v;
  }
  LowerContext context = {
      .graph = graph,
      .graph_path = graph_path,
      .params = params,
      .param_count = param_count,
      .allocator = allocator,
      .result = out,
  };
  for (uint32_t i = 0; i < param_count; ++i) {
    bool8_t found = false_v;
    for (uint32_t n = 0; n < graph->node_count && !found; ++n) {
      found = strcmp(graph->nodes[n].parameter, params[i].name) == 0;
    }
    if (!found) {
      return lower_fail(&context, NULL, "The graph has no parameter '%s'",
                        params[i].name);
    }
  }
  GraphText text = {.allocator = allocator};
  text_append(&text, "type=pbr\n");
  /* The fallback: each core output fed straight by a constant. */
  const VkrMaterialNode *output = NULL;
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    if (graph->nodes[i].kind == VKR_MATERIAL_NODE_SURFACE_OUTPUT) {
      output = &graph->nodes[i];
    }
  }
  static const uint32_t fallbacks[] = {SURFACE_BASE_COLOR, SURFACE_METALLIC,
                                       SURFACE_ROUGHNESS, SURFACE_EMISSIVE};
  for (uint32_t f = 0; output && f < ArrayCount(fallbacks); ++f) {
    const SurfaceInput *spec = &s_surface[fallbacks[f]];
    const VkrMaterialLink link = output->inputs[fallbacks[f]];
    const VkrMaterialNode *constant =
        link.node ? &graph->nodes[link.node - 1u] : NULL;
    Vec4 value = {0};
    if (!constant ||
        (constant->kind != VKR_MATERIAL_NODE_SCALAR &&
         constant->kind != VKR_MATERIAL_NODE_COLOR) ||
        !lower_constant(&context, constant, &value)) {
      continue;
    }
    if (constant->kind == VKR_MATERIAL_NODE_SCALAR) {
      value = vec4_new(value.x, value.x, value.x, 0.0f);
    }
    if (fallbacks[f] == SURFACE_BASE_COLOR) {
      text_append(&text,
                  "base_color=" GRAPH_FLOAT "," GRAPH_FLOAT "," GRAPH_FLOAT
                  ",1\n",
                  (double)value.x, (double)value.y, (double)value.z);
    } else if (spec->port.type == VKR_MATERIAL_VALUE_COLOR) {
      text_append(&text, "%s=" GRAPH_FLOAT "," GRAPH_FLOAT "," GRAPH_FLOAT "\n",
                  spec->factor, (double)value.x, (double)value.y,
                  (double)value.z);
    } else {
      text_append(&text, "%s=" GRAPH_FLOAT "\n", spec->factor, (double)value.x);
    }
  }
  text_append(&text, "custom_function=%s\n", out->function);
  for (uint32_t i = 0; i < binding.param_count; ++i) {
    const VkrMaterialNode *node = &graph->nodes[binding.param_nodes[i]];
    Vec4 value = {0};
    if (!lower_constant(&context, node, &value)) {
      return false_v;
    }
    if (node->kind == VKR_MATERIAL_NODE_SCALAR) {
      value = vec4_new(value.x, 0.0f, 0.0f, 0.0f);
    }
    text_append(&text,
                "custom_param%u=" GRAPH_FLOAT "," GRAPH_FLOAT "," GRAPH_FLOAT
                "\n",
                i, (double)value.x, (double)value.y, (double)value.z);
  }
  for (uint32_t i = 0; i < binding.texture_count; ++i) {
    const VkrMaterialNode *node = &graph->nodes[binding.texture_nodes[i]];
    const char *path = lower_texture_path(&context, node);
    if (!path) {
      return lower_fail(&context, node, "The texture path '%s' is not valid",
                        node->path);
    }
    text_append(&text, "custom%u_texture=%s\n", i, path);
    if (binding.texture_normal[i]) {
      text_append(&text, "custom%u_normal=true\n", i);
    } else {
      text_append(&text, "custom%u_colorspace=%s\n", i,
                  binding.texture_srgb[i] ? "srgb" : "linear");
    }
  }
  const VkrMaterialGraphSettings *settings = &graph->settings;
  if (settings->alpha_mode != VKR_MATERIAL_GRAPH_ALPHA_INFER) {
    text_append(&text, "alpha_mode=%s\n", s_alpha_names[settings->alpha_mode]);
  }
  if (settings->alpha_cutoff >= 0.0f) {
    text_append(&text, "alpha_cutoff=" GRAPH_FLOAT "\n",
                (double)settings->alpha_cutoff);
  }
  text_append(&text, "double_sided=%s\n",
              settings->double_sided ? "true" : "false");
  if (settings->world_size.x > 0.0f) {
    text_append(&text, "world_size=" GRAPH_FLOAT "," GRAPH_FLOAT "\n",
                (double)settings->world_size.x, (double)settings->world_size.y);
  }
  if (settings->surface[0]) {
    text_append(&text, "surface=%s\n", settings->surface);
  }
  if (!text_finish(&text, out_definition)) {
    return lower_fail(&context, NULL, "Out of memory for the definition");
  }
  out->tier = VKR_MATERIAL_TIER_CUSTOM;
  return true_v;
}

bool8_t vkr_material_graph_lower(const VkrMaterialGraph *graph,
                                 String8 graph_path,
                                 const VkrMaterialParam *params,
                                 uint32_t param_count, VkrAllocator *allocator,
                                 String8 *out_definition,
                                 VkrMaterialLowering *out) {
  /* Standard first; a graph it refuses becomes Custom when it generates.
     A graph of Standard nodes alone that does not generate keeps the
     Standard reason, which says what to change. */
  if (!graph_needs_custom(graph) &&
      lower_standard(graph, graph_path, params, param_count, allocator,
                     out_definition, out)) {
    return true_v;
  }
  const VkrMaterialLowering standard = *out;
  if (lower_custom(graph, graph_path, params, param_count, allocator,
                   out_definition, out)) {
    /* Why the graph costs a shader: what the Standard tier refused. */
    if (!graph_needs_custom(graph)) {
      snprintf(out->reason, sizeof(out->reason), "%s", standard.reason);
      snprintf(out->node, sizeof(out->node), "%s", standard.node);
    } else {
      snprintf(out->reason, sizeof(out->reason),
               "It uses nodes only a Custom graph evaluates");
      out->node[0] = '\0';
    }
    return true_v;
  }
  if (!graph_needs_custom(graph) && standard.reason[0]) {
    *out = standard;
  }
  return false_v;
}

static bool8_t lower_standard(const VkrMaterialGraph *graph, String8 graph_path,
                              const VkrMaterialParam *params,
                              uint32_t param_count, VkrAllocator *allocator,
                              String8 *out_definition,
                              VkrMaterialLowering *out) {
  *out = (VkrMaterialLowering){.tier = VKR_MATERIAL_TIER_STANDARD};
  LowerContext context = {
      .graph = graph,
      .graph_path = graph_path,
      .params = params,
      .param_count = param_count,
      .allocator = allocator,
      .result = out,
  };
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY];
  if (!vkr_material_graph_validate(graph, error, sizeof(error))) {
    return lower_fail(&context, NULL, "%s", error);
  }
  for (uint32_t i = 0; i < param_count; ++i) {
    bool8_t found = false_v;
    for (uint32_t n = 0; n < graph->node_count && !found; ++n) {
      found = strcmp(graph->nodes[n].parameter, params[i].name) == 0;
    }
    if (!found) {
      return lower_fail(&context, NULL, "The graph has no parameter '%s'",
                        params[i].name);
    }
  }
  const VkrMaterialNode *output = NULL;
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    if (graph->nodes[i].kind == VKR_MATERIAL_NODE_SURFACE_OUTPUT) {
      output = &graph->nodes[i];
    }
    out->alu += vkr_material_node_desc(graph->nodes[i].kind)->alu;
  }
  if (!output) {
    return lower_fail(&context, NULL, "The graph has no surface output");
  }

  GraphText text = {.allocator = allocator};
  text_append(&text, "type=pbr\n");
  LowerSource sources[SURFACE_INPUT_COUNT];
  MemZero(sources, sizeof(sources));
  for (uint32_t input = 0; input < SURFACE_INPUT_COUNT; ++input) {
    if (input != SURFACE_LAYERS && output->inputs[input].node &&
        !lower_source(&context, output->inputs[input], &sources[input])) {
      return false_v;
    }
  }

  /* Base colour carries opacity in its factor's alpha; a texture opacity is
     the base colour texture's own alpha. */
  const LowerSource *opacity = &sources[SURFACE_OPACITY];
  LowerSource *base = &sources[SURFACE_BASE_COLOR];
  if (opacity->texture &&
      (opacity->texture != base->texture || opacity->port != 4u)) {
    return lower_fail(&context, opacity->texture,
                      "A Standard opacity is the base colour texture's alpha");
  }
  if (base->has_factor || (opacity->has_factor && !opacity->texture)) {
    const Vec4 rgb = base->has_factor ? base->factor : vec4_new(1, 1, 1, 0);
    const float32_t alpha = opacity->has_factor ? opacity->factor.x : 1.0f;
    text_append(&text,
                "base_color=" GRAPH_FLOAT "," GRAPH_FLOAT "," GRAPH_FLOAT
                "," GRAPH_FLOAT "\n",
                (double)rgb.x, (double)rgb.y, (double)rgb.z, (double)alpha);
  }

  for (uint32_t input = 0; input < SURFACE_INPUT_COUNT; ++input) {
    const SurfaceInput *spec = &s_surface[input];
    LowerSource *source = &sources[input];
    if (input == SURFACE_OPACITY || input == SURFACE_LAYERS ||
        !output->inputs[input].node) {
      continue;
    }
    if (source->texture) {
      const bool8_t rgb = spec->channel < 0;
      const uint32_t want = rgb ? 0u : (uint32_t)spec->channel + 1u;
      if (!spec->texture) {
        return lower_fail(&context, source->texture,
                          "'%s' takes no texture in a Standard graph",
                          spec->port.label);
      }
      if (source->port != want) {
        static const char *const ports[] = {"rgb", "r", "g", "b", "a"};
        return lower_fail(&context, source->texture,
                          "A Standard '%s' reads its texture's %s",
                          spec->port.label, ports[want]);
      }
    }
    if (source->has_factor && input != SURFACE_BASE_COLOR) {
      if (!spec->factor) {
        return lower_fail(&context, source->constant,
                          "'%s' takes no constant", spec->port.label);
      }
      if (spec->port.type == VKR_MATERIAL_VALUE_COLOR) {
        text_append(&text,
                    "%s=" GRAPH_FLOAT "," GRAPH_FLOAT "," GRAPH_FLOAT "\n",
                    spec->factor, (double)source->factor.x,
                    (double)source->factor.y, (double)source->factor.z);
      } else {
        text_append(&text, "%s=" GRAPH_FLOAT "\n", spec->factor,
                    (double)source->factor.x);
      }
    }
    if (!source->texture) {
      continue;
    }
    /* Inputs sharing a slot share its file: metallic and roughness both
       read the one metallic-roughness texture. */
    bool8_t written = false_v;
    for (uint32_t s = 0; s < context.slot_count; ++s) {
      if (strcmp(context.slot_keys[s], spec->texture) == 0) {
        if (strcmp(context.slot_nodes[s]->path, source->texture->path) != 0 ||
            lower_override(&context, context.slot_nodes[s]) !=
                lower_override(&context, source->texture)) {
          return lower_fail(&context, source->texture,
                            "'%s' must read the same texture as the other "
                            "input of '%s'",
                            spec->port.label, spec->texture);
        }
        written = true_v;
      }
    }
    if (!source->texture->path[0] &&
        !lower_override(&context, source->texture)) {
      return lower_fail(&context, source->texture, "The texture names no file");
    }
    lower_sampled(&context, source->texture);
    if (written) {
      continue;
    }
    const char *path = lower_texture_path(&context, source->texture);
    if (!path) {
      return lower_fail(&context, source->texture,
                        "The texture path '%s' is not valid",
                        source->texture->path);
    }
    context.slot_keys[context.slot_count] = spec->texture;
    context.slot_nodes[context.slot_count++] = source->texture;
    text_append(&text, "%s=%s\n", spec->texture, path);
    lower_color_space(&text, spec->texture, source->texture, spec->srgb);
  }

  if (output->inputs[SURFACE_LAYERS].node &&
      !lower_layers(&context, &text,
                    &graph->nodes[output->inputs[SURFACE_LAYERS].node - 1u])) {
    return false_v;
  }

  const VkrMaterialGraphSettings *settings = &graph->settings;
  if (settings->alpha_mode != VKR_MATERIAL_GRAPH_ALPHA_INFER) {
    text_append(&text, "alpha_mode=%s\n", s_alpha_names[settings->alpha_mode]);
  }
  if (settings->alpha_cutoff >= 0.0f) {
    text_append(&text, "alpha_cutoff=" GRAPH_FLOAT "\n",
                (double)settings->alpha_cutoff);
  }
  text_append(&text, "double_sided=%s\n",
              settings->double_sided ? "true" : "false");
  if (settings->subsurface_profile) {
    text_append(&text, "subsurface_profile=%u\n",
                settings->subsurface_profile);
  }
  if (settings->temporal_reactivity >= 0.0f) {
    text_append(&text, "temporal_reactivity=" GRAPH_FLOAT "\n",
                (double)settings->temporal_reactivity);
  }
  if (settings->roughness_max >= 0.0f) {
    text_append(&text, "roughness_max=" GRAPH_FLOAT "\n",
                (double)settings->roughness_max);
  }
  if (settings->world_size.x > 0.0f) {
    text_append(&text, "world_size=" GRAPH_FLOAT "," GRAPH_FLOAT "\n",
                (double)settings->world_size.x, (double)settings->world_size.y);
  }
  if (settings->surface[0]) {
    text_append(&text, "surface=%s\n", settings->surface);
  }
  out->samples += context.sampled_count;
  if (!text_finish(&text, out_definition)) {
    return lower_fail(&context, NULL, "Out of memory for the definition");
  }
  return true_v;
}

// =============================================================================
// From a definition
// =============================================================================

/* Keys a PBR definition may carry beside the surface inputs' factors and
   textures: state, names the instance gives, and keys no material reads. */
static const char *const s_definition_ignored[] = {"name", "shader",
                                                   "pipeline", "type", NULL};

/* Aliases the loader accepts for slot keys, and the canonical key. */
static const struct {
  const char *alias;
  const char *key;
} s_definition_aliases[] = {
    {"emission_texture", "emissive_texture"},
    {"emission_colorspace", "emissive_colorspace"},
};

/* Splits `line` at its first '=' into a trimmed key and value. */
static bool8_t definition_line(String8 line, String8 *key, String8 *value) {
  uint64_t equals = 0;
  while (equals < line.length && line.str[equals] != '=') {
    equals++;
  }
  if (equals == line.length) {
    return false_v;
  }
  *key = string8_substring(&line, 0, equals);
  *value = string8_substring(&line, equals + 1u, line.length);
  string8_trim(key);
  string8_trim(value);
  return key->length > 0 && value->length > 0;
}

/* Calls `visit` with each key and value of `.mt` text; stops at a false
   return. */
typedef bool8_t (*DefinitionVisit)(void *context, String8 key, String8 value);

static bool8_t definition_each(String8 definition, DefinitionVisit visit,
                               void *context) {
  uint64_t offset = 0;
  while (offset < definition.length) {
    uint64_t end = offset;
    while (end < definition.length && definition.str[end] != '\n' &&
           definition.str[end] != '\r') {
      end++;
    }
    String8 line = string8_substring(&definition, offset, end);
    offset = end;
    while (offset < definition.length &&
           (definition.str[offset] == '\n' || definition.str[offset] == '\r')) {
      offset++;
    }
    string8_trim(&line);
    String8 key = {0};
    String8 value = {0};
    if (line.length == 0 || line.str[0] == '#' ||
        !definition_line(line, &key, &value)) {
      continue;
    }
    if (!visit(context, key, value)) {
      return false_v;
    }
  }
  return true_v;
}

typedef struct RaiseContext {
  VkrMaterialGraph *graph;
  char *error;
  uint32_t capacity;
  bool8_t pbr;
  /* Raw factor and texture text per surface input, and colour spaces per
     slot key. */
  String8 factors[SURFACE_INPUT_COUNT];
  String8 textures[SURFACE_INPUT_COUNT];
  String8 spaces[SURFACE_INPUT_COUNT];
  String8 base_color;
  /* A layered definition's layer files and mask. */
  String8 layers[3];
  String8 layer_mask;
  String8 layer_mask_range;
  String8 layer_mask_texture;
} RaiseContext;

static bool8_t raise_key(void *opaque, String8 key, String8 value) {
  RaiseContext *context = opaque;
  for (uint32_t i = 0; i < ArrayCount(s_definition_aliases); ++i) {
    if (graph_equals(key, s_definition_aliases[i].alias)) {
      key = string8_create_from_cstr(
          (const uint8_t *)s_definition_aliases[i].key,
          strlen(s_definition_aliases[i].key));
    }
  }
  for (uint32_t i = 0; s_definition_ignored[i]; ++i) {
    if (graph_equals(key, s_definition_ignored[i])) {
      if (graph_equals(key, "type")) {
        if (!graph_equals(value, "pbr")) {
          return graph_fail(context->error, context->capacity,
                            "Only PBR materials become graphs; '%.*s' is %.*s",
                            (int)key.length, key.str, (int)value.length,
                            value.str);
        }
        context->pbr = true_v;
      }
      return true_v;
    }
  }
  if (graph_equals(key, "base_color")) {
    context->base_color = value;
    context->pbr = true_v;
    return true_v;
  }
  VkrMaterialGraphSettings *settings = &context->graph->settings;
  if (graph_equals(key, "alpha_mode")) {
    if (graph_equals(value, "opaque")) {
      settings->alpha_mode = VKR_MATERIAL_GRAPH_ALPHA_OPAQUE;
    } else if (graph_equals(value, "mask") || graph_equals(value, "cutout")) {
      settings->alpha_mode = VKR_MATERIAL_GRAPH_ALPHA_MASK;
    } else if (graph_equals(value, "blend")) {
      settings->alpha_mode = VKR_MATERIAL_GRAPH_ALPHA_BLEND;
    } else {
      return graph_fail(context->error, context->capacity,
                        "alpha_mode '%.*s' is opaque, mask or blend",
                        (int)value.length, value.str);
    }
    return true_v;
  }
  if (graph_equals(key, "double_sided")) {
    if (!string8_to_bool(&value, &settings->double_sided)) {
      return graph_fail(context->error, context->capacity,
                        "double_sided '%.*s' is not a boolean",
                        (int)value.length, value.str);
    }
    return true_v;
  }
  float32_t *setting = graph_equals(key, "alpha_cutoff") ? &settings->alpha_cutoff
                       : graph_equals(key, "temporal_reactivity")
                           ? &settings->temporal_reactivity
                       : graph_equals(key, "roughness_max")
                           ? &settings->roughness_max
                           : NULL;
  if (setting) {
    if (!string8_to_f32(&value, setting) || !isfinite(*setting)) {
      return graph_fail(context->error, context->capacity,
                        "%.*s '%.*s' is not a number", (int)key.length,
                        key.str, (int)value.length, value.str);
    }
    return true_v;
  }
  if (graph_equals(key, "world_size")) {
    float32_t uniform = 0.0f;
    if (!string8_to_vec2(&value, &settings->world_size)) {
      if (!string8_to_f32(&value, &uniform) || !(uniform > 0.0f)) {
        return graph_fail(context->error, context->capacity,
                          "world_size '%.*s' is one or two sizes",
                          (int)value.length, value.str);
      }
      settings->world_size = vec2_new(uniform, uniform);
    }
    return true_v;
  }
  if (graph_equals(key, "surface")) {
    if (value.length >= sizeof(settings->surface)) {
      return graph_fail(context->error, context->capacity,
                        "surface '%.*s' is not a tag name", (int)value.length,
                        value.str);
    }
    MemCopy(settings->surface, value.str, value.length);
    settings->surface[value.length] = '\0';
    return true_v;
  }
  if (graph_equals(key, "subsurface_profile")) {
    if (!string8_to_u32(&value, &settings->subsurface_profile) ||
        settings->subsurface_profile > 7u) {
      return graph_fail(context->error, context->capacity,
                        "subsurface_profile '%.*s' is 0 to 7",
                        (int)value.length, value.str);
    }
    return true_v;
  }
  static const char *const layer_keys[] = {"layer1", "layer2", "layer3"};
  for (uint32_t i = 0; i < ArrayCount(layer_keys); ++i) {
    if (graph_equals(key, layer_keys[i])) {
      context->layers[i] = value;
      return true_v;
    }
  }
  String8 *mask_text =
      graph_equals(key, "layer_mask")           ? &context->layer_mask
      : graph_equals(key, "layer_mask_range")   ? &context->layer_mask_range
      : graph_equals(key, "layer_mask_texture") ? &context->layer_mask_texture
                                                : NULL;
  if (mask_text) {
    *mask_text = value;
    return true_v;
  }
  for (uint32_t i = 0; i < SURFACE_INPUT_COUNT; ++i) {
    const SurfaceInput *spec = &s_surface[i];
    if (spec->factor && i != SURFACE_BASE_COLOR && graph_equals(key, spec->factor)) {
      context->factors[i] = value;
      return true_v;
    }
    if (spec->texture && graph_equals(key, spec->texture)) {
      context->textures[i] = value;
      return true_v;
    }
    if (spec->texture) {
      const uint64_t stem = strlen(spec->texture) - strlen("_texture");
      if (key.length == stem + strlen("_colorspace") &&
          MemCompare(key.str, spec->texture, stem) == 0 &&
          MemCompare(key.str + stem, "_colorspace", strlen("_colorspace")) ==
              0) {
        context->spaces[i] = value;
        return true_v;
      }
    }
  }
  return graph_fail(context->error, context->capacity,
                    "'%.*s' has no graph form (legacy or unknown key)",
                    (int)key.length, key.str);
}

/* Whether factor text `text` of surface input `input` is the value the
   loader gives the key when a definition leaves it out
   (vkr_material_loader_set_parse_defaults). */
static bool8_t raise_is_default(uint32_t input, String8 text) {
  static const Vec3 defaults[SURFACE_INPUT_COUNT] = {
      [SURFACE_METALLIC] = {1.0f, 0.0f, 0.0f},
      [SURFACE_ROUGHNESS] = {1.0f, 0.0f, 0.0f},
      [SURFACE_OCCLUSION] = {1.0f, 0.0f, 0.0f},
      [SURFACE_NORMAL] = {1.0f, 0.0f, 0.0f},
      [SURFACE_SPECULAR] = {0.04f, 0.04f, 0.04f},
      [SURFACE_CLEARCOAT_NORMAL] = {1.0f, 0.0f, 0.0f},
      [SURFACE_ATTENUATION_COLOR] = {1.0f, 1.0f, 1.0f},
      [SURFACE_IOR] = {1.5f, 0.0f, 0.0f},
      [SURFACE_DIFFUSE_TRANSMISSION_COLOR] = {1.0f, 1.0f, 1.0f},
  };
  const Vec3 expected = defaults[input];
  if (s_surface[input].port.type == VKR_MATERIAL_VALUE_COLOR) {
    Vec3 value = {0};
    return string8_to_vec3(&text, &value) && value.x == expected.x &&
           value.y == expected.y && value.z == expected.z;
  }
  float32_t value = 0.0f;
  return string8_to_f32(&text, &value) && value == expected.x;
}

/* Adds a node and returns its index, or UINT32_MAX when the graph is
   full. */
static uint32_t raise_node(VkrMaterialGraph *graph, VkrMaterialNodeKind kind,
                           const char *id, Vec2 position) {
  if (graph->node_count == VKR_MATERIAL_GRAPH_NODE_MAX) {
    return UINT32_MAX;
  }
  VkrMaterialNode *node = &graph->nodes[graph->node_count];
  MemZero(node, sizeof(*node));
  node->kind = kind;
  snprintf(node->id, sizeof(node->id), "%s", id);
  node->position = position;
  return graph->node_count++;
}

static bool8_t raise_constant(RaiseContext *context, uint32_t input,
                              String8 text, Vec2 position, uint32_t *out) {
  const SurfaceInput *spec = &s_surface[input];
  const bool8_t color = spec->port.type == VKR_MATERIAL_VALUE_COLOR;
  char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  snprintf(id, sizeof(id), "%s_value", spec->port.name);
  *out = raise_node(context->graph,
                    color ? VKR_MATERIAL_NODE_COLOR : VKR_MATERIAL_NODE_SCALAR,
                    id, position);
  if (*out == UINT32_MAX) {
    return graph_fail(context->error, context->capacity, "Too many nodes");
  }
  VkrMaterialNode *node = &context->graph->nodes[*out];
  if (color) {
    Vec3 value = {0};
    if (!string8_to_vec3(&text, &value)) {
      return graph_fail(context->error, context->capacity,
                        "%s '%.*s' is not r,g,b", spec->factor,
                        (int)text.length, text.str);
    }
    node->value = vec4_new(value.x, value.y, value.z, 0.0f);
  } else if (!string8_to_f32(&text, &node->value.x)) {
    return graph_fail(context->error, context->capacity,
                      "%s '%.*s' is not a number", spec->factor,
                      (int)text.length, text.str);
  }
  return true_v;
}

/* The texture node of file `path` with colour space `space`, shared by the
   inputs that read it. */
static bool8_t raise_texture(RaiseContext *context, uint32_t input,
                             Vec2 position, uint32_t *out) {
  const String8 path = context->textures[input];
  const String8 space = context->spaces[input];
  const SurfaceInput *spec = &s_surface[input];
  /* The loader's own default colour space for the slot, when the definition
     names none: linear except sheen colour. */
  VkrMaterialColorSpace color_space =
      input == SURFACE_SHEEN_COLOR ? VKR_MATERIAL_COLOR_SPACE_SRGB
                                   : VKR_MATERIAL_COLOR_SPACE_LINEAR;
  if (space.length) {
    color_space = graph_equals(space, "srgb") ? VKR_MATERIAL_COLOR_SPACE_SRGB
                                              : VKR_MATERIAL_COLOR_SPACE_LINEAR;
  }
  if (path.length >= VKR_MATERIAL_GRAPH_PATH_CAPACITY) {
    return graph_fail(context->error, context->capacity,
                      "%s is longer than %u bytes", spec->texture,
                      VKR_MATERIAL_GRAPH_PATH_CAPACITY - 1u);
  }
  VkrMaterialGraph *graph = context->graph;
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    const VkrMaterialNode *node = &graph->nodes[i];
    if (node->kind == VKR_MATERIAL_NODE_TEXTURE && graph_equals(path, node->path) &&
        node->color_space == color_space) {
      *out = i;
      return true_v;
    }
  }
  char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  snprintf(id, sizeof(id), "%s_map", spec->port.name);
  *out = raise_node(graph, VKR_MATERIAL_NODE_TEXTURE, id, position);
  if (*out == UINT32_MAX) {
    return graph_fail(context->error, context->capacity, "Too many nodes");
  }
  VkrMaterialNode *node = &graph->nodes[*out];
  MemCopy(node->path, path.str, path.length);
  node->path[path.length] = '\0';
  node->color_space = color_space;
  return true_v;
}

/* A layered definition's layers as layer nodes and a blend into the
   output's layers. */
static bool8_t raise_layers(RaiseContext *context, uint32_t output,
                            float32_t row) {
  VkrMaterialGraph *graph = context->graph;
  bool8_t layered = false_v;
  for (uint32_t i = 0; i < ArrayCount(context->layers); ++i) {
    layered = layered || context->layers[i].length > 0;
  }
  if (!layered) {
    return true_v;
  }
  const uint32_t blend = raise_node(graph, VKR_MATERIAL_NODE_LAYER_BLEND,
                                    "layers", vec2_new(380.0f, row));
  if (blend == UINT32_MAX) {
    return graph_fail(context->error, context->capacity, "Too many nodes");
  }
  VkrMaterialNode *node = &graph->nodes[blend];
  node->value = vec4_new(0.0f, 1.0f, 0.0f, 0.0f);
  if (context->layer_mask.length) {
    bool8_t known = false_v;
    for (uint32_t m = 0; m < VKR_MATERIAL_GRAPH_MASK_COUNT && !known; ++m) {
      if (graph_equals(context->layer_mask, vkr_material_graph_mask_names[m])) {
        node->mask = (VkrMaterialGraphMask)m;
        known = true_v;
      }
    }
    if (!known) {
      return graph_fail(
          context->error, context->capacity, "layer_mask '%.*s' is no mask",
          (int)context->layer_mask.length, context->layer_mask.str);
    }
  }
  Vec2 range = {0};
  if (context->layer_mask_range.length) {
    if (!string8_to_vec2(&context->layer_mask_range, &range)) {
      return graph_fail(context->error, context->capacity,
                        "layer_mask_range '%.*s' is two numbers",
                        (int)context->layer_mask_range.length,
                        context->layer_mask_range.str);
    }
    node->value = vec4_new(range.x, range.y, 0.0f, 0.0f);
  }
  if (context->layer_mask_texture.length >= sizeof(node->path)) {
    return graph_fail(context->error, context->capacity,
                      "layer_mask_texture is too long");
  }
  MemCopy(node->path, context->layer_mask_texture.str,
          context->layer_mask_texture.length);
  node->path[context->layer_mask_texture.length] = '\0';
  for (uint32_t i = 0; i < ArrayCount(context->layers); ++i) {
    const String8 path = context->layers[i];
    if (!path.length) {
      continue;
    }
    char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
    snprintf(id, sizeof(id), "layer%u", i + 1u);
    const uint32_t layer =
        raise_node(graph, VKR_MATERIAL_NODE_LAYER, id,
                   vec2_new(100.0f, row + (float32_t)i * 110.0f));
    if (layer == UINT32_MAX || path.length >= sizeof(node->path)) {
      return graph_fail(context->error, context->capacity,
                        "Too many nodes or a layer path too long");
    }
    MemCopy(graph->nodes[layer].path, path.str, path.length);
    graph->nodes[layer].path[path.length] = '\0';
    graph->nodes[blend].inputs[i] =
        (VkrMaterialLink){.node = (uint16_t)(layer + 1u)};
  }
  graph->nodes[output].inputs[SURFACE_LAYERS] =
      (VkrMaterialLink){.node = (uint16_t)(blend + 1u)};
  return true_v;
}

bool8_t vkr_material_graph_from_definition(String8 definition,
                                           VkrMaterialGraph *out, char *error,
                                           uint32_t capacity) {
  vkr_material_graph_init(out);
  RaiseContext context = {.graph = out, .error = error, .capacity = capacity};
  if (!definition_each(definition, raise_key, &context)) {
    return false_v;
  }
  for (uint32_t i = 0; i < SURFACE_INPUT_COUNT && !context.pbr; ++i) {
    context.pbr = context.factors[i].length || context.textures[i].length;
  }
  if (!context.pbr) {
    return graph_fail(error, capacity,
                      "Only PBR materials become graphs; add type=pbr");
  }
  const uint32_t output =
      raise_node(out, VKR_MATERIAL_NODE_SURFACE_OUTPUT, "output",
                 vec2_new(680.0f, 0.0f));
  float32_t row = 0.0f;
  for (uint32_t input = 0; input < SURFACE_INPUT_COUNT; ++input) {
    const SurfaceInput *spec = &s_surface[input];
    String8 factor = context.factors[input];
    /* Base colour and opacity share the base_color factor. */
    const bool8_t shared =
        input == SURFACE_BASE_COLOR || input == SURFACE_OPACITY;
    Vec4 base = vec4_new(1.0f, 1.0f, 1.0f, 1.0f);
    if (shared && context.base_color.length &&
        !string8_to_vec4(&context.base_color, &base)) {
      return graph_fail(error, capacity, "base_color '%.*s' is not r,g,b,a",
                        (int)context.base_color.length, context.base_color.str);
    }
    const bool8_t textured = context.textures[input].length > 0;
    /* A factor equal to the loader's default stays out of the graph: the
       lowered definition omits it and the loader gives it back. */
    bool8_t constant = false_v;
    if (input == SURFACE_BASE_COLOR) {
      constant = base.x != 1.0f || base.y != 1.0f || base.z != 1.0f;
    } else if (input == SURFACE_OPACITY) {
      constant = base.w != 1.0f;
    } else if (factor.length) {
      constant = !raise_is_default(input, factor);
    }
    if (!textured && !constant) {
      continue;
    }
    const Vec2 position = vec2_new(textured ? -40.0f : 160.0f, row);
    row += textured ? 150.0f : 110.0f;
    uint32_t constant_node = UINT32_MAX;
    if (constant) {
      if (shared) {
        char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
        snprintf(id, sizeof(id), "%s_value", spec->port.name);
        constant_node = raise_node(
            out,
            input == SURFACE_BASE_COLOR ? VKR_MATERIAL_NODE_COLOR
                                        : VKR_MATERIAL_NODE_SCALAR,
            id, position);
        if (constant_node == UINT32_MAX) {
          return graph_fail(error, capacity, "Too many nodes");
        }
        out->nodes[constant_node].value =
            input == SURFACE_BASE_COLOR ? vec4_new(base.x, base.y, base.z, 0.0f)
                                        : vec4_new(base.w, 0.0f, 0.0f, 0.0f);
      } else if (!raise_constant(&context, input, factor, position,
                                 &constant_node)) {
        return false_v;
      }
    }
    VkrMaterialLink link = {.node = (uint16_t)(constant_node + 1u)};
    if (!textured && spec->port.type == VKR_MATERIAL_VALUE_NORMAL) {
      char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
      snprintf(id, sizeof(id), "%s", spec->port.name);
      const uint32_t normal = raise_node(out, VKR_MATERIAL_NODE_NORMAL_MAP, id,
                                         vec2_new(380.0f, position.y));
      if (normal == UINT32_MAX) {
        return graph_fail(error, capacity, "Too many nodes");
      }
      out->nodes[normal].inputs[1] = link;
      link = (VkrMaterialLink){.node = (uint16_t)(normal + 1u)};
    }
    if (textured) {
      uint32_t texture = UINT32_MAX;
      if (!raise_texture(&context, input, vec2_new(-320.0f, position.y),
                         &texture)) {
        return false_v;
      }
      const uint8_t port =
          spec->channel < 0 ? 0u : (uint8_t)(spec->channel + 1);
      VkrMaterialLink texture_link = {.node = (uint16_t)(texture + 1u),
                                      .port = port};
      if (spec->port.type == VKR_MATERIAL_VALUE_NORMAL) {
        char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
        snprintf(id, sizeof(id), "%s", spec->port.name);
        const uint32_t normal = raise_node(out, VKR_MATERIAL_NODE_NORMAL_MAP,
                                           id, vec2_new(380.0f, position.y));
        if (normal == UINT32_MAX) {
          return graph_fail(error, capacity, "Too many nodes");
        }
        out->nodes[normal].inputs[0] = texture_link;
        if (constant_node != UINT32_MAX) {
          out->nodes[normal].inputs[1] = link;
        }
        link = (VkrMaterialLink){.node = (uint16_t)(normal + 1u)};
      } else if (constant_node != UINT32_MAX &&
                 input != SURFACE_OPACITY) {
        char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
        snprintf(id, sizeof(id), "%s", spec->port.name);
        const uint32_t multiply = raise_node(out, VKR_MATERIAL_NODE_MULTIPLY,
                                             id, vec2_new(380.0f, position.y));
        if (multiply == UINT32_MAX) {
          return graph_fail(error, capacity, "Too many nodes");
        }
        out->nodes[multiply].inputs[0] = texture_link;
        out->nodes[multiply].inputs[1] = link;
        link = (VkrMaterialLink){.node = (uint16_t)(multiply + 1u)};
      } else {
        link = texture_link;
      }
    }
    out->nodes[output].inputs[input] = link;
  }
  if (!raise_layers(&context, output, row)) {
    return false_v;
  }
  /* A base colour texture keeps its alpha implicit, as the loader infers
     opacity from it; the factor's alpha stays the opacity constant. */
  return vkr_material_graph_validate(out, error, capacity);
}

// =============================================================================
// Instances
// =============================================================================

typedef struct InstanceContext {
  VkrMaterialInstance *instance;
  char *error;
  uint32_t capacity;
} InstanceContext;

static bool8_t instance_key(void *opaque, String8 key, String8 value) {
  InstanceContext *context = opaque;
  VkrMaterialInstance *instance = context->instance;
  if (value.length >= VKR_MATERIAL_GRAPH_PATH_CAPACITY) {
    return graph_fail(context->error, context->capacity,
                      "'%.*s' is longer than %u bytes", (int)key.length,
                      key.str, VKR_MATERIAL_GRAPH_PATH_CAPACITY - 1u);
  }
  char *target = NULL;
  if (graph_equals(key, "graph")) {
    target = instance->graph;
  } else if (graph_equals(key, "name")) {
    target = instance->name;
  } else if (graph_equals(key, "world_size") || graph_equals(key, "surface")) {
    if (value.length >= VKR_MATERIAL_GRAPH_ID_CAPACITY) {
      return graph_fail(context->error, context->capacity,
                        "'%.*s' is longer than %u bytes", (int)key.length,
                        key.str, VKR_MATERIAL_GRAPH_ID_CAPACITY - 1u);
    }
    target =
        graph_equals(key, "surface") ? instance->surface : instance->world_size;
  } else if (key.length > 6u && MemCompare(key.str, "param.", 6u) == 0) {
    if (instance->param_count == VKR_MATERIAL_GRAPH_PARAM_MAX ||
        key.length - 6u >= VKR_MATERIAL_GRAPH_ID_CAPACITY) {
      return graph_fail(context->error, context->capacity,
                        "An instance sets at most %u parameters of 31 bytes",
                        VKR_MATERIAL_GRAPH_PARAM_MAX);
    }
    VkrMaterialParam *param = &instance->params[instance->param_count++];
    MemCopy(param->name, key.str + 6u, key.length - 6u);
    param->name[key.length - 6u] = '\0';
    target = param->value;
  } else {
    return graph_fail(context->error, context->capacity,
                      "A graph instance sets name, graph, world_size, "
                      "surface and param.<name>, not '%.*s'",
                      (int)key.length, key.str);
  }
  MemCopy(target, value.str, value.length);
  target[value.length] = '\0';
  return true_v;
}

/* Whether `definition` has a `graph=` line. */
static bool8_t instance_has_graph(void *opaque, String8 key, String8 value) {
  (void)value;
  bool8_t *found = opaque;
  *found = *found || graph_equals(key, "graph");
  return true_v;
}

bool8_t vkr_material_instance_read(String8 definition,
                                   VkrMaterialInstance *out, char *error,
                                   uint32_t capacity) {
  MemZero(out, sizeof(*out));
  bool8_t found = false_v;
  (void)definition_each(definition, instance_has_graph, &found);
  if (!found) {
    return false_v;
  }
  InstanceContext context = {.instance = out, .error = error,
                             .capacity = capacity};
  if (!definition_each(definition, instance_key, &context)) {
    out->graph[0] = '\0';
    return false_v;
  }
  return true_v;
}

bool8_t vkr_material_instance_write(const VkrMaterialInstance *instance,
                                    VkrAllocator *allocator, String8 *out) {
  GraphText text = {.allocator = allocator};
  if (instance->name[0]) {
    text_append(&text, "name=%s\n", instance->name);
  }
  text_append(&text, "graph=%s\n", instance->graph);
  if (instance->world_size[0]) {
    text_append(&text, "world_size=%s\n", instance->world_size);
  }
  if (instance->surface[0]) {
    text_append(&text, "surface=%s\n", instance->surface);
  }
  for (uint32_t i = 0; i < instance->param_count; ++i) {
    text_append(&text, "param.%s=%s\n", instance->params[i].name,
                instance->params[i].value);
  }
  return text_finish(&text, out);
}

// =============================================================================
// Description
// =============================================================================

bool8_t vkr_material_graph_describe(const VkrMaterialGraph *graph,
                                    VkrAllocator *allocator, String8 *out) {
  GraphText text = {.allocator = allocator};
  const VkrMaterialGraphSettings *settings = &graph->settings;
  text_append(&text, "settings alpha=%s double_sided=%s",
              s_alpha_names[settings->alpha_mode],
              settings->double_sided ? "true" : "false");
  if (settings->world_size.x > 0.0f) {
    text_append(&text, " world_size=" GRAPH_FLOAT "," GRAPH_FLOAT,
                (double)settings->world_size.x, (double)settings->world_size.y);
  }
  if (settings->surface[0]) {
    text_append(&text, " surface=%s", settings->surface);
  }
  text_append(&text, "\n");
  for (uint32_t i = 0; i < graph->node_count; ++i) {
    const VkrMaterialNode *node = &graph->nodes[i];
    const VkrMaterialNodeDesc *desc = vkr_material_node_desc(node->kind);
    text_append(&text, "%s %s", node->id, desc->name);
    if (node->parameter[0]) {
      text_append(&text, " param=%s", node->parameter);
    }
    if (node->kind == VKR_MATERIAL_NODE_SCALAR) {
      text_append(&text, " value=" GRAPH_FLOAT, (double)node->value.x);
    } else if (node->kind == VKR_MATERIAL_NODE_COLOR) {
      text_append(&text,
                  " value=" GRAPH_FLOAT "," GRAPH_FLOAT "," GRAPH_FLOAT,
                  (double)node->value.x, (double)node->value.y,
                  (double)node->value.z);
    } else if (node->kind == VKR_MATERIAL_NODE_TEXTURE ||
               node->kind == VKR_MATERIAL_NODE_LAYER) {
      text_append(&text, " path=%s", node->path);
    } else if (node->kind == VKR_MATERIAL_NODE_LAYER_BLEND) {
      text_append(&text, " mask=%s range=" GRAPH_FLOAT "," GRAPH_FLOAT,
                  vkr_material_graph_mask_names[node->mask],
                  (double)node->value.x, (double)node->value.y);
      if (node->path[0]) {
        text_append(&text, " path=%s", node->path);
      }
    }
    for (uint32_t port = 0; port < desc->input_count; ++port) {
      const VkrMaterialLink link = node->inputs[port];
      if (!link.node) {
        continue;
      }
      const VkrMaterialNode *source = &graph->nodes[link.node - 1u];
      text_append(&text, " %s<-%s.%s", desc->inputs[port].name, source->id,
                  vkr_material_node_desc(source->kind)->outputs[link.port].name);
    }
    text_append(&text, "\n");
  }
  return text_finish(&text, out);
}
