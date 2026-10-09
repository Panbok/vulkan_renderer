#include "material_graph_tests.h"

#include "assets/vkr_material_graph.h"
#include "level/vkr_surface.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/material_loader.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#else
#include <direct.h>
#endif

#if !defined(_WIN32)
#include <dirent.h>
#endif

/* Independent oracles: hand-written `.mt` definitions parsed by the loader,
   and literal graph documents. A lowered graph must parse to the same
   material as the definition a person would write for it. */

static String8 graph_test_str(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, strlen(text));
}

static void graph_test_parse(VkrAllocator *allocator, const char *path,
                             String8 text, VkrParsedMaterialData *out) {
  MemZero(out, sizeof(*out));
  const bool8_t ok = vkr_material_loader_parse_definition(
      allocator, graph_test_str(path), text, out);
  assert(ok);
  (void)ok;
}

static bool8_t graph_test_vec3(Vec3 a, Vec3 b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

static bool8_t graph_test_vec4(Vec4 a, Vec4 b) {
  return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

/* Whether two parses give the same material, apart from its name. */
static bool8_t graph_test_same(const VkrParsedMaterialData *a,
                               const VkrParsedMaterialData *b,
                               char *why, uint32_t capacity) {
#define SAME(field, test)                                                      \
  if (!(test)) {                                                               \
    snprintf(why, capacity, "%s", #field);                                     \
    return false_v;                                                            \
  }
  const VkrPbrProperties *p = &a->pbr;
  const VkrPbrProperties *q = &b->pbr;
  SAME(material_type, a->material_type == b->material_type);
  SAME(alpha_mode, a->alpha_mode == b->alpha_mode);
  SAME(alpha_mode_explicit, a->alpha_mode_explicit == b->alpha_mode_explicit);
  SAME(double_sided, a->double_sided == b->double_sided);
  SAME(alpha_cutoff, a->alpha_cutoff == b->alpha_cutoff &&
                         a->alpha_cutoff_set == b->alpha_cutoff_set);
  SAME(cutout_enabled, a->cutout_enabled == b->cutout_enabled);
  SAME(world_size, a->world_size.x == b->world_size.x &&
                       a->world_size.y == b->world_size.y);
  SAME(surface, a->surface == b->surface);
  SAME(layered, a->layered == b->layered);
  SAME(layer_mask,
       a->layer_mask == b->layer_mask &&
           (!a->layered || (a->layer_mask_range.x == b->layer_mask_range.x &&
                            a->layer_mask_range.y == b->layer_mask_range.y)));
  for (uint32_t layer = 0; a->layered && layer < 3u; ++layer) {
    SAME(layers, graph_test_vec4(a->layers[layer].base_color,
                                 b->layers[layer].base_color) &&
                     a->layers[layer].roughness == b->layers[layer].roughness);
  }
  SAME(roughness_max, a->roughness_max_set == b->roughness_max_set &&
                          (!a->roughness_max_set ||
                           a->roughness_max == b->roughness_max));
  SAME(base_color, graph_test_vec4(p->base_color, q->base_color));
  SAME(metallic, p->metallic == q->metallic);
  SAME(roughness, p->roughness == q->roughness);
  SAME(normal_scale, p->normal_scale == q->normal_scale);
  SAME(occlusion_strength, p->occlusion_strength == q->occlusion_strength);
  SAME(emissive_factor, graph_test_vec3(p->emissive_factor, q->emissive_factor));
  SAME(dielectric_specular,
       graph_test_vec3(p->dielectric_specular, q->dielectric_specular));
  SAME(transmission_factor, p->transmission_factor == q->transmission_factor);
  SAME(ior, p->ior == q->ior);
  SAME(thickness_factor, p->thickness_factor == q->thickness_factor);
  SAME(attenuation_color,
       graph_test_vec3(p->attenuation_color, q->attenuation_color));
  SAME(attenuation_distance, p->attenuation_distance == q->attenuation_distance);
  SAME(temporal_reactivity, p->temporal_reactivity == q->temporal_reactivity);
  SAME(clearcoat_factor, p->clearcoat_factor == q->clearcoat_factor);
  SAME(clearcoat_roughness, p->clearcoat_roughness == q->clearcoat_roughness);
  SAME(clearcoat_normal_scale,
       p->clearcoat_normal_scale == q->clearcoat_normal_scale);
  SAME(sheen_color, graph_test_vec3(p->sheen_color, q->sheen_color));
  SAME(sheen_roughness, p->sheen_roughness == q->sheen_roughness);
  SAME(anisotropy_strength, p->anisotropy_strength == q->anisotropy_strength);
  SAME(anisotropy_rotation, p->anisotropy_rotation == q->anisotropy_rotation);
  SAME(diffuse_transmission_strength,
       p->diffuse_transmission_strength == q->diffuse_transmission_strength);
  SAME(diffuse_transmission_color, graph_test_vec3(p->diffuse_transmission_color,
                                                   q->diffuse_transmission_color));
  SAME(subsurface_strength, p->subsurface_strength == q->subsurface_strength);
  SAME(subsurface_profile, p->subsurface_profile == q->subsurface_profile);
  /* A colour space without a texture samples nothing, so only textured
     slots compare it. */
  for (uint32_t slot = 0; slot < VKR_TEXTURE_SLOT_COUNT; ++slot) {
    if (strcmp(a->texture_paths[slot], b->texture_paths[slot]) != 0 ||
        (a->texture_paths[slot][0] &&
         a->texture_colorspace[slot] != b->texture_colorspace[slot])) {
      snprintf(why, capacity, "texture slot %u: '%s' / '%s'", slot,
               a->texture_paths[slot], b->texture_paths[slot]);
      return false_v;
    }
  }
#undef SAME
  return true_v;
}

static const char s_test_graph[] =
    "{\"version\": 1, \"settings\": {\"double_sided\": true},\n"
    " \"nodes\": [\n"
    "  {\"id\": \"albedo\", \"type\": \"texture\", \"path\": \"./albedo.png\","
    " \"parameter\": \"albedo\"},\n"
    "  {\"id\": \"tint\", \"type\": \"color\", \"value\": [0.5, 0.25, 1],"
    " \"parameter\": \"tint\"},\n"
    "  {\"id\": \"base\", \"type\": \"multiply\", \"inputs\": {\"a\":"
    " \"albedo.rgb\", \"b\": \"tint\"}},\n"
    "  {\"id\": \"orm\", \"type\": \"texture\", \"path\": \"./orm.png\"},\n"
    "  {\"id\": \"rough_scale\", \"type\": \"scalar\", \"value\": 0.8},\n"
    "  {\"id\": \"rough\", \"type\": \"multiply\", \"inputs\": {\"a\":"
    " \"orm.g\", \"b\": \"rough_scale\"}},\n"
    "  {\"id\": \"nrm\", \"type\": \"texture\", \"path\": \"./normal.png\"},\n"
    "  {\"id\": \"nstr\", \"type\": \"scalar\", \"value\": 2},\n"
    "  {\"id\": \"nmap\", \"type\": \"normal_map\", \"inputs\": {\"texture\":"
    " \"nrm.rgb\", \"strength\": \"nstr\"}},\n"
    "  {\"id\": \"alpha\", \"type\": \"scalar\", \"value\": 0.5},\n"
    "  {\"id\": \"glow\", \"type\": \"color\", \"value\": [2, 1, 0]},\n"
    "  {\"id\": \"out\", \"type\": \"surface_output\", \"inputs\": {"
    "\"base_color\": \"base\", \"metallic\": \"orm.b\", \"roughness\":"
    " \"rough\", \"normal\": \"nmap\", \"opacity\": \"alpha\", \"emissive\":"
    " \"glow\"}}\n"
    " ]}\n";

/* The definition a person would write for s_test_graph at
   assets/materials/graphs/, as a material beside it. */
static const char s_test_expected[] =
    "type=pbr\n"
    "base_color=0.5,0.25,1,0.5\n"
    "base_color_texture=./albedo.png\n"
    "base_color_colorspace=srgb\n"
    "metallic_roughness_texture=./orm.png\n"
    "roughness=0.8\n"
    "normal_texture=./normal.png\n"
    "normal_scale=2\n"
    "emissive_factor=2,1,0\n"
    "double_sided=true\n";

static void test_graph_lowering(VkrAllocator *allocator) {
  VkrMaterialGraph *graph = malloc(sizeof(*graph));
  assert(graph);
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  assert(vkr_material_graph_read(graph_test_str(s_test_graph), graph, error,
                                 sizeof(error)));

  VkrMaterialLowering lowering;
  String8 definition = {0};
  assert(vkr_material_graph_lower(
      graph, graph_test_str("assets/materials/graphs/test.mtg"), NULL, 0u,
      allocator, &definition, &lowering));
  assert(lowering.tier == VKR_MATERIAL_TIER_STANDARD);
  assert(lowering.samples == 3u);

  VkrParsedMaterialData *lowered = malloc(sizeof(*lowered));
  VkrParsedMaterialData *expected = malloc(sizeof(*expected));
  assert(lowered && expected);
  graph_test_parse(allocator, "assets/materials/graphs/instance.mt", definition,
                   lowered);
  graph_test_parse(allocator, "assets/materials/graphs/instance.mt",
                   graph_test_str(s_test_expected), expected);
  char why[256] = {0};
  if (!graph_test_same(lowered, expected, why, sizeof(why))) {
    printf("  lowered graph differs from its definition: %s\n%.*s", why,
           (int)definition.length, definition.str);
    assert(false);
  }

  /* An instance overrides exposed parameters; its own texture path resolves
     against the instance. */
  VkrMaterialInstance instance;
  assert(vkr_material_instance_read(
      graph_test_str("name=red_brick\ngraph=./graphs/test.mtg\n"
                     "param.tint=1,0,0\nparam.albedo=./red.png\n"),
      &instance, error, sizeof(error)));
  assert(strcmp(instance.graph, "./graphs/test.mtg") == 0);
  assert(strcmp(instance.name, "red_brick") == 0);
  assert(instance.param_count == 2u);
  assert(vkr_material_graph_lower(
      graph, graph_test_str("assets/materials/graphs/test.mtg"),
      instance.params, instance.param_count, allocator, &definition,
      &lowering));
  graph_test_parse(allocator, "assets/materials/instance.mt", definition,
                   lowered);
  graph_test_parse(allocator, "assets/materials/instance.mt",
                   graph_test_str("type=pbr\n"
                                  "base_color=1,0,0,0.5\n"
                                  "base_color_texture=./red.png\n"
                                  "base_color_colorspace=srgb\n"
                                  "metallic_roughness_texture=./graphs/orm.png\n"
                                  "roughness=0.8\n"
                                  "normal_texture=./graphs/normal.png\n"
                                  "normal_scale=2\n"
                                  "emissive_factor=2,1,0\n"
                                  "double_sided=true\n"),
                   expected);
  assert(graph_test_same(lowered, expected, why, sizeof(why)));

  /* Text without a graph is no instance; an instance setting material keys
     is rejected with the key. */
  error[0] = '\0';
  assert(!vkr_material_instance_read(graph_test_str("type=pbr\nmetallic=0\n"),
                                     &instance, error, sizeof(error)));
  assert(!error[0]);
  assert(!vkr_material_instance_read(
      graph_test_str("graph=./g.mtg\nmetallic=0\n"), &instance, error,
      sizeof(error)));
  assert(strstr(error, "metallic"));

  /* An unknown parameter does not lower. */
  VkrMaterialParam stray = {.name = "gloss", .value = "1"};
  assert(!vkr_material_graph_lower(graph, graph_test_str("g.mtg"), &stray, 1u,
                                   allocator, &definition, &lowering));
  assert(strstr(lowering.reason, "gloss"));
  free(lowered);
  free(expected);
  free(graph);
  printf("  test_graph_lowering PASSED\n");
}

/* Graphs outside the Standard tier name the node and the reason. */
static void test_graph_tiers(VkrAllocator *allocator) {
  static const struct {
    const char *json;
    const char *node;
  } cases[] = {
      /* Roughness reads green; red is occlusion. */
      {"{\"version\":1,\"nodes\":[{\"id\":\"orm\",\"type\":\"texture\","
       "\"path\":\"a.png\"},{\"id\":\"out\",\"type\":\"surface_output\","
       "\"inputs\":{\"roughness\":\"orm.r\"}}]}",
       "orm"},
      /* Two textures multiplied blend, which a Standard graph cannot. */
      {"{\"version\":1,\"nodes\":[{\"id\":\"a\",\"type\":\"texture\","
       "\"path\":\"a.png\"},{\"id\":\"b\",\"type\":\"texture\",\"path\":"
       "\"b.png\"},{\"id\":\"m\",\"type\":\"multiply\",\"inputs\":{\"a\":"
       "\"a.rgb\",\"b\":\"b.rgb\"}},{\"id\":\"out\",\"type\":"
       "\"surface_output\",\"inputs\":{\"base_color\":\"m\"}}]}",
       "b"},
      /* A texture without a file lowers to nothing. */
      {"{\"version\":1,\"nodes\":[{\"id\":\"t\",\"type\":\"texture\"},"
       "{\"id\":\"out\",\"type\":\"surface_output\",\"inputs\":"
       "{\"base_color\":\"t.rgb\"}}]}",
       "t"},
      /* Metallic and roughness share one slot, so one file. */
      {"{\"version\":1,\"nodes\":[{\"id\":\"m\",\"type\":\"texture\","
       "\"path\":\"m.png\"},{\"id\":\"r\",\"type\":\"texture\",\"path\":"
       "\"r.png\"},{\"id\":\"out\",\"type\":\"surface_output\",\"inputs\":"
       "{\"metallic\":\"m.b\",\"roughness\":\"r.g\"}}]}",
       "r"},
  };
  VkrMaterialGraph *graph = malloc(sizeof(*graph));
  assert(graph);
  for (uint32_t i = 0; i < ArrayCount(cases); ++i) {
    char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
    assert(vkr_material_graph_read(graph_test_str(cases[i].json), graph,
                                   error, sizeof(error)));
    VkrMaterialLowering lowering;
    String8 definition = {0};
    assert(!vkr_material_graph_lower(graph, graph_test_str("g.mtg"), NULL, 0u,
                                     allocator, &definition, &lowering));
    assert(lowering.tier == VKR_MATERIAL_TIER_UNSUPPORTED);
    assert(strcmp(lowering.node, cases[i].node) == 0);
    assert(lowering.reason[0]);
  }
  free(graph);
  printf("  test_graph_tiers PASSED\n");
}

/* Documents a graph reader must refuse, each for the reason named. */
static void test_graph_validation(void) {
  static const struct {
    const char *json;
    const char *reason;
  } cases[] = {
      {"{\"nodes\":[]}", "version"},
      {"{\"version\":1,\"nodes\":[{\"id\":\"a\",\"type\":\"multiply\","
       "\"inputs\":{\"a\":\"b\"}},{\"id\":\"b\",\"type\":\"multiply\","
       "\"inputs\":{\"a\":\"a\"}}]}",
       "feeds itself"},
      {"{\"version\":1,\"nodes\":[{\"id\":\"t\",\"type\":\"texture\","
       "\"path\":\"a.png\"},{\"id\":\"n\",\"type\":\"normal_map\","
       "\"inputs\":{\"texture\":\"t.rgb\"}},{\"id\":\"out\",\"type\":"
       "\"surface_output\",\"inputs\":{\"base_color\":\"n\"}}]}",
       "takes a colour"},
      {"{\"version\":1,\"nodes\":[{\"id\":\"a\",\"type\":\"scalar\"},"
       "{\"id\":\"a\",\"type\":\"scalar\"}]}",
       "repeats"},
      {"{\"version\":1,\"nodes\":[{\"id\":\"out\",\"type\":"
       "\"surface_output\",\"inputs\":{\"metallic\":\"missing.value\"}}]}",
       "names no node"},
      {"{\"version\":1,\"nodes\":[{\"id\":\"Bad Id\",\"type\":\"scalar\"}]}",
       "is not 1 to 31"},
      {"{\"version\":1,\"nodes\":[{\"id\":\"a\",\"type\":\"surface_output\"},"
       "{\"id\":\"b\",\"type\":\"surface_output\"}]}",
       "one surface output"},
  };
  VkrMaterialGraph *graph = malloc(sizeof(*graph));
  assert(graph);
  for (uint32_t i = 0; i < ArrayCount(cases); ++i) {
    char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
    assert(!vkr_material_graph_read(graph_test_str(cases[i].json), graph,
                                    error, sizeof(error)));
    if (!strstr(error, cases[i].reason)) {
      printf("  case %u: '%s' lacks '%s'\n", i, error, cases[i].reason);
      assert(false);
    }
  }
  free(graph);
  printf("  test_graph_validation PASSED\n");
}

/* A written graph reads back to the same document. */
static void test_graph_documents(VkrAllocator *allocator) {
  VkrMaterialGraph *graph = malloc(sizeof(*graph));
  VkrMaterialGraph *again = malloc(sizeof(*again));
  assert(graph && again);
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  assert(vkr_material_graph_read(graph_test_str(s_test_graph), graph, error,
                                 sizeof(error)));
  String8 first = {0};
  String8 second = {0};
  assert(vkr_material_graph_write(graph, allocator, &first));
  assert(vkr_material_graph_read(first, again, error, sizeof(error)));
  assert(vkr_material_graph_write(again, allocator, &second));
  assert(first.length == second.length &&
         MemCompare(first.str, second.str, first.length) == 0);
  String8 description = {0};
  assert(vkr_material_graph_describe(graph, allocator, &description));
  assert(strstr((const char *)description.str,
                "base multiply a<-albedo.rgb b<-tint.value"));
  free(graph);
  free(again);
  printf("  test_graph_documents PASSED\n");
}

/* The art-pass keys: a definition's world size and surface tag parse, bad
   values reject the material, and a graph raised from the definition keeps
   them through its document and lowering. */
static void test_graph_art_metadata(VkrAllocator *allocator) {
  const char *definition = "type=pbr\nbase_color=0.5,0.5,0.5,1\n"
                           "world_size=2,1\nsurface=brick\n";
  VkrParsedMaterialData direct;
  graph_test_parse(allocator, "assets/materials/test/art.mt",
                   graph_test_str(definition), &direct);
  assert(direct.world_size.x == 2.0f && direct.world_size.y == 1.0f);
  assert(direct.surface == VKR_SURFACE_BRICK);

  VkrParsedMaterialData rejected;
  MemZero(&rejected, sizeof(rejected));
  assert(!vkr_material_loader_parse_definition(
      allocator, graph_test_str("assets/materials/test/bad.mt"),
      graph_test_str("type=pbr\nworld_size=0\n"), &rejected));
  assert(!vkr_material_loader_parse_definition(
      allocator, graph_test_str("assets/materials/test/bad.mt"),
      graph_test_str("type=pbr\nsurface=brik\n"), &rejected));
  VkrParsedMaterialData uniform;
  graph_test_parse(allocator, "assets/materials/test/uniform.mt",
                   graph_test_str("type=pbr\nworld_size=3\n"), &uniform);
  assert(uniform.world_size.x == 3.0f && uniform.world_size.y == 3.0f);

  VkrMaterialGraph *graph = malloc(sizeof(*graph));
  VkrMaterialGraph *again = malloc(sizeof(*again));
  assert(graph && again);
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  assert(vkr_material_graph_from_definition(graph_test_str(definition), graph,
                                            error, sizeof(error)));
  String8 json = {0};
  assert(vkr_material_graph_write(graph, allocator, &json));
  assert(vkr_material_graph_read(json, again, error, sizeof(error)));
  String8 lowered_text = {0};
  VkrMaterialLowering lowering = {0};
  assert(vkr_material_graph_lower(
      again, graph_test_str("assets/materials/test/art.mtg"), NULL, 0u,
      allocator, &lowered_text, &lowering));
  VkrParsedMaterialData lowered;
  graph_test_parse(allocator, "assets/materials/test/art.mt", lowered_text,
                   &lowered);
  char why[64] = {0};
  assert(graph_test_same(&direct, &lowered, why, sizeof(why)));

  /* An instance's own lines replace its graph's. */
  VkrMaterialInstance instance;
  assert(vkr_material_instance_read(
      graph_test_str("graph=./art.mtg\nworld_size=4\nsurface=wood\n"),
      &instance, error, sizeof(error)));
  assert(strcmp(instance.world_size, "4") == 0);
  assert(strcmp(instance.surface, "wood") == 0);
  free(graph);
  free(again);
  printf("  test_graph_art_metadata PASSED\n");
}

/* A layered definition becomes a graph of layer nodes and a blend that
   lowers back to the same layered material; a blend without a layer, or
   with a falling range, names the node at fault. */
static void test_graph_layers(VkrAllocator *allocator) {
  char directory[1024];
  char layer_file[1024];
  snprintf(directory, sizeof(directory), "%stests/tmp/material_graph",
           PROJECT_SOURCE_DIR);
  snprintf(layer_file, sizeof(layer_file), "%s/moss.mt", directory);
#if defined(_WIN32)
  (void)_mkdir(directory);
#else
  (void)mkdir(directory, 0755);
#endif
  FILE *file = fopen(layer_file, "wb");
  assert(file);
  fputs("name=moss\ntype=pbr\nbase_color=0.1,0.6,0.2,1\nroughness=0.9\n", file);
  fclose(file);

  const char *definition = "type=pbr\nbase_color=0.5,0.5,0.5,1\n"
                           "layer1=./moss.mt\nlayer_mask=slope\n"
                           "layer_mask_range=0.6,0.9\n";
  const char *path = "tests/tmp/material_graph/mossy.mt";
  VkrParsedMaterialData direct;
  graph_test_parse(allocator, path, graph_test_str(definition), &direct);
  assert(direct.layered);

  VkrMaterialGraph *graph = malloc(sizeof(*graph));
  assert(graph);
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  assert(vkr_material_graph_from_definition(graph_test_str(definition), graph,
                                            error, sizeof(error)));
  String8 json = {0};
  assert(vkr_material_graph_write(graph, allocator, &json));
  assert(vkr_material_graph_read(json, graph, error, sizeof(error)));
  String8 lowered_text = {0};
  VkrMaterialLowering lowering = {0};
  assert(vkr_material_graph_lower(
      graph, graph_test_str("tests/tmp/material_graph/mossy.mtg"), NULL, 0u,
      allocator, &lowered_text, &lowering));
  assert(lowering.tier == VKR_MATERIAL_TIER_STANDARD && lowering.layers == 1u);
  VkrParsedMaterialData lowered;
  graph_test_parse(allocator, path, lowered_text, &lowered);
  char why[64] = {0};
  if (!graph_test_same(&direct, &lowered, why, sizeof(why))) {
    printf("    layered round trip differs in %s\n", why);
    assert(false);
  }

  /* A falling range, then a blend whose layer is gone. */
  const uint32_t blend =
      vkr_material_graph_find(graph, graph_test_str("layers"));
  assert(blend != UINT32_MAX);
  graph->nodes[blend].value = vec4_new(0.9f, 0.6f, 0.0f, 0.0f);
  assert(!vkr_material_graph_lower(
      graph, graph_test_str("tests/tmp/material_graph/mossy.mtg"), NULL, 0u,
      allocator, &lowered_text, &lowering));
  assert(strcmp(lowering.node, "layers") == 0);
  graph->nodes[blend].value = vec4_new(0.6f, 0.9f, 0.0f, 0.0f);
  vkr_material_graph_remove(
      graph, vkr_material_graph_find(graph, graph_test_str("layer1")));
  assert(!vkr_material_graph_lower(
      graph, graph_test_str("tests/tmp/material_graph/mossy.mtg"), NULL, 0u,
      allocator, &lowered_text, &lowering));
  assert(strcmp(lowering.node, "layers") == 0);
  free(graph);
  remove(layer_file);
  printf("  test_graph_layers PASSED\n");
}

/* A PBR definition becomes a graph that lowers back to the same material:
   one with every key the importers write, then every `.mt` under
   assets/materials, such as Bistro's. */
static bool8_t graph_test_round_trip(VkrAllocator *allocator, const char *path,
                                     String8 text, VkrMaterialGraph *graph,
                                     VkrParsedMaterialData *direct,
                                     VkrParsedMaterialData *lowered,
                                     uint32_t *out_skipped) {
  char error[VKR_MATERIAL_GRAPH_ERROR_CAPACITY] = {0};
  MemZero(direct, sizeof(*direct));
  if (!vkr_material_loader_parse_definition(allocator, graph_test_str(path),
                                            text, direct)) {
    printf("  %s: the loader rejects it\n", path);
    (*out_skipped)++;
    return true_v;
  }
  if (!vkr_material_graph_from_definition(text, graph, error, sizeof(error))) {
    printf("  %s: %s\n", path, error);
    (*out_skipped)++;
    return true_v;
  }
  VkrMaterialLowering lowering;
  String8 definition = {0};
  if (!vkr_material_graph_lower(graph, graph_test_str(path), NULL, 0u,
                                allocator, &definition, &lowering)) {
    printf("  %s does not lower: %s (%s)\n", path, lowering.reason,
           lowering.node);
    return false_v;
  }
  graph_test_parse(allocator, path, definition, lowered);
  char why[256] = {0};
  if (!graph_test_same(direct, lowered, why, sizeof(why))) {
    printf("  %s differs after a round trip: %s\n", path, why);
    return false_v;
  }
  return true_v;
}

#if !defined(_WIN32)
/* Round-trips every `.mt` under `directory`, four folders deep at most. */
static void graph_test_sweep(VkrAllocator *allocator, const char *directory,
                             uint32_t depth, VkrMaterialGraph *graph,
                             VkrParsedMaterialData *direct,
                             VkrParsedMaterialData *lowered, uint32_t *checked,
                             uint32_t *skipped) {
  DIR *folder = opendir(directory);
  struct dirent *entry = NULL;
  while (folder && (entry = readdir(folder)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
    const size_t length = strlen(entry->d_name);
    if (length < 4u || strcmp(entry->d_name + length - 3u, ".mt") != 0) {
      if (depth < 4u && !strchr(entry->d_name, '.')) {
        graph_test_sweep(allocator, path, depth + 1u, graph, direct, lowered,
                         checked, skipped);
      }
      continue;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
      continue;
    }
    static char text[65536];
    const size_t size = fread(text, 1u, sizeof(text) - 1u, file);
    fclose(file);
    text[size] = '\0';
    VkrAllocatorScope scope = vkr_allocator_begin_scope(allocator);
    const bool8_t same = graph_test_round_trip(
        allocator, path, graph_test_str(text), graph, direct, lowered, skipped);
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_UNKNOWN);
    assert(same);
    (void)same;
    (*checked)++;
  }
  if (folder) {
    closedir(folder);
  }
}
#endif

static void test_graph_round_trip(VkrAllocator *allocator) {
  static const char full[] =
      "name=full\ntype=pbr\nbase_color_colorspace=srgb\n"
      "base_color=0.8,0.7,0.6,1.0\nmetallic=0.25\nroughness=0.75\n"
      "roughness_max=0.9\ndielectric_specular=0.04,0.04,0.04\n"
      "normal_scale=1.5\nocclusion_strength=0.5\n"
      "emissive_factor=0.1,0.2,0.3\ntransmission_factor=0.2\nior=1.45\n"
      "thickness_factor=0.01\nattenuation_color=1,0.9,0.8\n"
      "attenuation_distance=2\nalpha_mode=mask\ndouble_sided=true\n"
      "alpha_cutoff=0.4\nshader=shader.pbr.world\npipeline=world\n"
      "clearcoat_factor=0.5\nclearcoat_roughness=0.1\n"
      "clearcoat_normal_scale=0.75\nsheen_color=0.2,0.1,0.05\n"
      "sheen_roughness=0.6\n"
      "base_color_texture=./../../textures/a.png?cs=srgb&tc=color_srgb\n"
      "normal_texture=./../../textures/n.png?tc=normal_rg\n"
      "metallic_roughness_texture=./../../textures/orm.png?tc=data_mask\n"
      "occlusion_texture=./../../textures/orm.png?tc=data_mask\n"
      "emissive_texture=./../../textures/e.png?cs=srgb&tc=color_srgb\n"
      "emissive_colorspace=srgb\n"
      "transmission_texture=./../../textures/t.png?tc=data_mask\n"
      "clearcoat_texture=./../../textures/c.png?tc=data_mask\n"
      "sheen_color_texture=./../../textures/s.png?cs=srgb&tc=color_srgb\n"
      "sheen_color_colorspace=srgb\n";
  VkrMaterialGraph *graph = malloc(sizeof(*graph));
  VkrParsedMaterialData *direct = malloc(sizeof(*direct));
  VkrParsedMaterialData *lowered = malloc(sizeof(*lowered));
  assert(graph && direct && lowered);
  uint32_t skipped = 0u;
  assert(graph_test_round_trip(allocator, "assets/materials/fixture/full.mt",
                               graph_test_str(full), graph, direct, lowered,
                               &skipped));
  assert(skipped == 0u);

  uint32_t checked = 0u;
#if !defined(_WIN32)
  graph_test_sweep(allocator, "assets/materials", 0u, graph, direct, lowered,
                   &checked, &skipped);
#endif
  printf("  test_graph_round_trip PASSED (%u local materials, %u not PBR or "
         "not loadable)\n",
         checked, skipped);
  free(graph);
  free(direct);
  free(lowered);
}

bool32_t run_material_graph_tests(void) {
  printf("--- Material Graph Tests ---\n");
  Arena *arena = arena_create(MB(64), MB(4));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  test_graph_validation();
  test_graph_documents(&allocator);
  test_graph_lowering(&allocator);
  test_graph_tiers(&allocator);
  test_graph_art_metadata(&allocator);
  test_graph_layers(&allocator);
  test_graph_round_trip(&allocator);
  arena_destroy(arena);
  printf("--- Material Graph Tests Completed ---\n");
  return true_v;
}
