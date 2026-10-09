#pragma once

#include "containers/str.h"
#include "defines.h"
#include "math/vec.h"
#include "memory/vkr_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Material graphs (docs/proposals/artist-toolkit.md, part 2). A graph is a
 * `.mtg` JSON document of typed nodes whose surface output gives the
 * material model's inputs. Bakery, the editor and the runtime lower a graph
 * to the `.mt` definition the material loader reads: a Standard graph lowers
 * to factors, textures and state only, so it adds no shader and no pipeline.
 * A `.mt` instance names its graph (`graph=`) and overrides the graph's
 * exposed parameters (`param.<name>=`). The module owns no state; callers
 * own every graph value and the text it writes.
 */

#define VKR_MATERIAL_GRAPH_VERSION 1u
#define VKR_MATERIAL_GRAPH_NODE_MAX 128u
/* Node ids and parameter names: [a-z0-9_], 1 to 31 bytes. */
#define VKR_MATERIAL_GRAPH_ID_CAPACITY 32u
#define VKR_MATERIAL_GRAPH_PATH_CAPACITY 256u
#define VKR_MATERIAL_GRAPH_INPUT_MAX 24u
#define VKR_MATERIAL_GRAPH_OUTPUT_MAX 5u
#define VKR_MATERIAL_GRAPH_PARAM_MAX 32u
#define VKR_MATERIAL_GRAPH_ERROR_CAPACITY 192u

/* The value a port carries. A normal carries a normal map and its
   strength. */
typedef enum VkrMaterialValueType {
  VKR_MATERIAL_VALUE_SCALAR = 0,
  VKR_MATERIAL_VALUE_COLOR,
  VKR_MATERIAL_VALUE_NORMAL,
  /* A layer material, and the layers a blend mixes over the surface. */
  VKR_MATERIAL_VALUE_LAYER,
  VKR_MATERIAL_VALUE_LAYERS,
} VkrMaterialValueType;

typedef enum VkrMaterialNodeKind {
  /* A constant scalar or colour; exposed when it names a parameter. */
  VKR_MATERIAL_NODE_SCALAR = 0,
  VKR_MATERIAL_NODE_COLOR,
  /* A texture file: its rgb and its r, g, b and a channels; exposed when it
     names a parameter. */
  VKR_MATERIAL_NODE_TEXTURE,
  /* a times b: a texture by a constant scales the texture. */
  VKR_MATERIAL_NODE_MULTIPLY,
  /* A tangent-space normal map from a texture's rgb, with a strength. */
  VKR_MATERIAL_NODE_NORMAL_MAP,
  /* A layer material file (`.mt`); exposed when it names a parameter. */
  VKR_MATERIAL_NODE_LAYER,
  /* Up to three layers over the surface, weighed by its mask: the vertex
     colour, a mask texture (`path`), or slope or height across the range in
     `value` x to y. */
  VKR_MATERIAL_NODE_LAYER_BLEND,
  VKR_MATERIAL_NODE_SURFACE_OUTPUT,
  VKR_MATERIAL_NODE_KIND_COUNT,
} VkrMaterialNodeKind;

typedef struct VkrMaterialPortDesc {
  const char *name;
  const char *label;
  VkrMaterialValueType type;
} VkrMaterialPortDesc;

typedef struct VkrMaterialNodeDesc {
  const char *name;
  const char *label;
  const char *tooltip;
  const VkrMaterialPortDesc *inputs;
  uint32_t input_count;
  const VkrMaterialPortDesc *outputs;
  uint32_t output_count;
  /* Texture samples and arithmetic the node costs per pixel. */
  uint32_t samples;
  uint32_t alu;
} VkrMaterialNodeDesc;

/* An input's source: output `port` of node `node - 1`; node 0 is none. */
typedef struct VkrMaterialLink {
  uint16_t node;
  uint8_t port;
} VkrMaterialLink;

typedef enum VkrMaterialColorSpace {
  /* sRGB for base colour, emissive and sheen colour, linear for the rest. */
  VKR_MATERIAL_COLOR_SPACE_AUTO = 0,
  VKR_MATERIAL_COLOR_SPACE_SRGB,
  VKR_MATERIAL_COLOR_SPACE_LINEAR,
} VkrMaterialColorSpace;

/* What weighs a layer blend's layers; the loader's `layer_mask` names. */
typedef enum VkrMaterialGraphMask {
  VKR_MATERIAL_GRAPH_MASK_VERTEX_COLOR = 0,
  VKR_MATERIAL_GRAPH_MASK_TEXTURE,
  VKR_MATERIAL_GRAPH_MASK_SLOPE,
  VKR_MATERIAL_GRAPH_MASK_HEIGHT,
  VKR_MATERIAL_GRAPH_MASK_COUNT,
} VkrMaterialGraphMask;

/* Mask names as documents store them, indexed by VkrMaterialGraphMask. */
extern const char *const vkr_material_graph_mask_names[];

typedef struct VkrMaterialNode {
  char id[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  VkrMaterialNodeKind kind;
  /* Exposed parameter name of a scalar, colour or texture; empty for
     none. */
  char parameter[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  /* Texture or layer file (a layer blend's mask texture): relative to the
     graph with ./ or ../, else to the content root; query suffixes (?cs=,
     ?tc=) pass through. */
  char path[VKR_MATERIAL_GRAPH_PATH_CAPACITY];
  VkrMaterialColorSpace color_space;
  /* A layer blend's mask. */
  VkrMaterialGraphMask mask;
  /* Scalar in x, colour in xyz, a layer blend's range in xy. */
  Vec4 value;
  /* Canvas position in points; layout only. */
  Vec2 position;
  VkrMaterialLink inputs[VKR_MATERIAL_GRAPH_INPUT_MAX];
} VkrMaterialNode;

typedef enum VkrMaterialGraphAlpha {
  /* The loader infers it from the base colour's alpha and texture. */
  VKR_MATERIAL_GRAPH_ALPHA_INFER = 0,
  VKR_MATERIAL_GRAPH_ALPHA_OPAQUE,
  VKR_MATERIAL_GRAPH_ALPHA_MASK,
  VKR_MATERIAL_GRAPH_ALPHA_BLEND,
} VkrMaterialGraphAlpha;

/* Surface state the output node does not carry. A negative cutoff,
   reactivity or largest roughness leaves the loader's default. */
typedef struct VkrMaterialGraphSettings {
  VkrMaterialGraphAlpha alpha_mode;
  float32_t alpha_cutoff;
  bool8_t double_sided;
  uint32_t subsurface_profile;
  float32_t temporal_reactivity;
  float32_t roughness_max;
  /* Art-pass metadata: meters one texture repeat covers on brush faces
     (zero unset, which is 1 m), and the surface tag's name (empty none). */
  Vec2 world_size;
  char surface[VKR_MATERIAL_GRAPH_ID_CAPACITY];
} VkrMaterialGraphSettings;

typedef struct VkrMaterialGraph {
  VkrMaterialGraphSettings settings;
  uint32_t node_count;
  VkrMaterialNode nodes[VKR_MATERIAL_GRAPH_NODE_MAX];
} VkrMaterialGraph;

/* How a graph lowers. Phase 2 lowers Standard graphs only; another graph
   reports why it is not Standard. */
typedef enum VkrMaterialTier {
  VKR_MATERIAL_TIER_STANDARD = 0,
  VKR_MATERIAL_TIER_UNSUPPORTED,
} VkrMaterialTier;

typedef struct VkrMaterialLowering {
  VkrMaterialTier tier;
  /* Why the graph does not lower, and the node at fault. */
  char reason[VKR_MATERIAL_GRAPH_ERROR_CAPACITY];
  char node[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  /* Distinct texture files sampled, and the arithmetic weight. */
  uint32_t samples;
  uint32_t alu;
  /* Layers blended over the surface (0 to 3). */
  uint32_t layers;
} VkrMaterialLowering;

/* One instance override: a parameter name and its `.mt` value text. */
typedef struct VkrMaterialParam {
  char name[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  char value[VKR_MATERIAL_GRAPH_PATH_CAPACITY];
} VkrMaterialParam;

/* An instance's graph file and overrides, read from `.mt` text. Its own
   `world_size` and `surface` lines (empty for the graph's) follow the
   lowered definition, so they replace the graph's. */
typedef struct VkrMaterialInstance {
  char graph[VKR_MATERIAL_GRAPH_PATH_CAPACITY];
  char name[VKR_MATERIAL_GRAPH_PATH_CAPACITY];
  char world_size[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  char surface[VKR_MATERIAL_GRAPH_ID_CAPACITY];
  uint32_t param_count;
  VkrMaterialParam params[VKR_MATERIAL_GRAPH_PARAM_MAX];
} VkrMaterialInstance;

/* The node type registry; NULL past the last kind. */
const VkrMaterialNodeDesc *vkr_material_node_desc(VkrMaterialNodeKind kind);
bool8_t vkr_material_node_find(String8 name, VkrMaterialNodeKind *out);
/* The input or output port named `name` of `kind`, or UINT32_MAX. */
uint32_t vkr_material_node_input(VkrMaterialNodeKind kind, String8 name);
uint32_t vkr_material_node_output(VkrMaterialNodeKind kind, String8 name);

/* An empty graph with default settings. */
void vkr_material_graph_init(VkrMaterialGraph *graph);
/* The node named `id`, or UINT32_MAX. */
uint32_t vkr_material_graph_find(const VkrMaterialGraph *graph, String8 id);

/* Edits that keep a graph's links consistent; the caller validates the
   result. A new node gets id `base` or `base_<n>`, the first free one;
   UINT32_MAX when the graph is full. */
uint32_t vkr_material_graph_add(VkrMaterialGraph *graph,
                                VkrMaterialNodeKind kind, const char *base);
/* Removes node `index` and every link to it; later nodes move down one. */
void vkr_material_graph_remove(VkrMaterialGraph *graph, uint32_t index);
/* Links output `from_port` of node `from` to input `to_port` of node `to`,
   replacing that input's link; false for ports out of range, a type that
   does not fit or a cycle, leaving the graph as it was. */
bool8_t vkr_material_graph_connect(VkrMaterialGraph *graph, uint32_t from,
                                   uint32_t from_port, uint32_t to,
                                   uint32_t to_port, char *error,
                                   uint32_t capacity);

/* Checks ids, links, port types and cycles; one surface output at most. */
bool8_t vkr_material_graph_validate(const VkrMaterialGraph *graph, char *error,
                                    uint32_t capacity);

/* Reads a `.mtg` document into `out`, then validates it. */
bool8_t vkr_material_graph_read(String8 json, VkrMaterialGraph *out,
                                char *error, uint32_t capacity);
/* Writes `graph` as a `.mtg` document, one node a line, into `allocator`. */
bool8_t vkr_material_graph_write(const VkrMaterialGraph *graph,
                                 VkrAllocator *allocator, String8 *out_json);

/* Lowers `graph`, whose file is `graph_path`, with `params` over its exposed
   parameters, to `.mt` definition lines in `allocator` that the loader reads
   as a file anywhere; texture paths come out content-root relative. False
   with `out->reason` when it does not lower. */
bool8_t vkr_material_graph_lower(const VkrMaterialGraph *graph,
                                 String8 graph_path,
                                 const VkrMaterialParam *params,
                                 uint32_t param_count, VkrAllocator *allocator,
                                 String8 *out_definition,
                                 VkrMaterialLowering *out);

/* Builds the Standard graph of a PBR `.mt` definition, so the graph lowers
   back to the same material. Legacy Phong keys, unknown keys and values the
   loader would reject fail with the key. Texture paths keep their text. */
bool8_t vkr_material_graph_from_definition(String8 definition,
                                           VkrMaterialGraph *out, char *error,
                                           uint32_t capacity);

/* Whether `.mt` text names a graph, and its instance fields. */
bool8_t vkr_material_instance_read(String8 definition,
                                   VkrMaterialInstance *out, char *error,
                                   uint32_t capacity);
/* Writes `instance` as `.mt` text into `allocator`: name, graph, its own
   world size and surface, then the parameters. */
bool8_t vkr_material_instance_write(const VkrMaterialInstance *instance,
                                    VkrAllocator *allocator, String8 *out);

/* One line a node: id, type, exposure, value or path and its inputs, for
   agents to read a graph without the canvas. */
bool8_t vkr_material_graph_describe(const VkrMaterialGraph *graph,
                                    VkrAllocator *allocator, String8 *out);

#ifdef __cplusplus
}
#endif
