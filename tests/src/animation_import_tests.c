#include "animation_import_tests.h"

#include "assets/vkr_animation_import.h"
#include "memory/vkr_arena_allocator.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct AnimationImportTest {
  Arena *result_arena;
  Arena *scratch_arena;
  VkrAllocator result;
  VkrAllocator scratch;
  char directory[1024];
  char gltf_path[1100];
  char binary_path[1100];
  char glb_path[1100];
} AnimationImportTest;

static int32_t animation_test_mkdir(const char *path) {
#if defined(_WIN32)
  return _mkdir(path);
#else
  return mkdir(path, 0755);
#endif
}

static void animation_test_begin(AnimationImportTest *test) {
  *test = (AnimationImportTest){0};
  char parent[1024];
  snprintf(parent, sizeof(parent), "%stests/tmp", PROJECT_SOURCE_DIR);
  const int32_t parent_result = animation_test_mkdir(parent);
  assert(parent_result == 0 || errno == EEXIST);
  static uint32_t sequence = 0u;
  int32_t result = -1;
  for (uint32_t attempt = 0; attempt < 100u; ++attempt) {
    ++sequence;
    snprintf(test->directory, sizeof(test->directory),
             "%stests/tmp/animation_import_%u_%u", PROJECT_SOURCE_DIR,
             vkr_platform_get_process_id(), sequence);
    result = animation_test_mkdir(test->directory);
    if (result == 0 || errno != EEXIST) {
      break;
    }
  }
  assert(result == 0);
  snprintf(test->gltf_path, sizeof(test->gltf_path), "%s/source.gltf",
           test->directory);
  snprintf(test->binary_path, sizeof(test->binary_path), "%s/source.bin",
           test->directory);
  snprintf(test->glb_path, sizeof(test->glb_path), "%s/source.glb",
           test->directory);
  test->result_arena = arena_create(MB(1), KB(64));
  test->scratch_arena = arena_create(MB(1), KB(64));
  assert(test->result_arena && test->scratch_arena);
  test->result.ctx = test->result_arena;
  test->scratch.ctx = test->scratch_arena;
  assert(vkr_allocator_arena(&test->result));
  assert(vkr_allocator_arena(&test->scratch));
}

static void animation_test_end(AnimationImportTest *test) {
  arena_destroy(test->scratch_arena);
  arena_destroy(test->result_arena);
  const char *paths[] = {test->gltf_path, test->binary_path, test->glb_path};
  for (uint32_t i = 0; i < ArrayCount(paths); ++i) {
    const int32_t result = remove(paths[i]);
    assert(result == 0 || errno == ENOENT);
  }
#if defined(_WIN32)
  const int32_t result = _rmdir(test->directory);
#else
  const int32_t result = rmdir(test->directory);
#endif
  assert(result == 0);
}

static void animation_test_write(const char *path, const void *bytes,
                                 uint32_t size) {
  FILE *file = fopen(path, "wb");
  assert(file);
  const size_t written = fwrite(bytes, 1u, size, file);
  const int32_t closed = fclose(file);
  assert(written == size && closed == 0);
}

static void animation_test_u32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
  bytes[2] = (uint8_t)(value >> 16u);
  bytes[3] = (uint8_t)(value >> 24u);
}

static void animation_test_floats(uint8_t *bytes, const float32_t *values,
                                  uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t bits = 0u;
    MemCopy(&bits, &values[i], sizeof(bits));
    animation_test_u32(bytes + i * 4u, bits);
  }
}

static void animation_test_glb(AnimationImportTest *test, const char *json,
                               const uint8_t *binary, uint32_t binary_size) {
  uint8_t file[8192] = {0};
  const uint32_t json_size = (uint32_t)strlen(json);
  const uint32_t json_padded = (json_size + 3u) & ~3u;
  const uint32_t binary_padded = (binary_size + 3u) & ~3u;
  const uint32_t total = 12u + 8u + json_padded + 8u + binary_padded;
  assert(total <= sizeof(file));
  animation_test_u32(file, 0x46546c67u);
  animation_test_u32(file + 4u, 2u);
  animation_test_u32(file + 8u, total);
  animation_test_u32(file + 12u, json_padded);
  animation_test_u32(file + 16u, 0x4e4f534au);
  MemSet(file + 20u, ' ', json_padded);
  MemCopy(file + 20u, json, json_size);
  animation_test_u32(file + 20u + json_padded, binary_padded);
  animation_test_u32(file + 24u + json_padded, 0x004e4942u);
  MemCopy(file + 28u + json_padded, binary, binary_size);
  animation_test_write(test->glb_path, file, total);
}

static bool8_t animation_test_import(AnimationImportTest *test,
                                     const char *path, VkrAnimationAsset *asset,
                                     const char **error) {
  return vkr_animation_import_gltf(
      &test->result, &test->scratch,
      string8_create((uint8_t *)path, strlen(path)), asset, error);
}

static void animation_test_sparse_strided_gltf(void) {
  printf("  Running animation_test_sparse_strided_gltf...\n");
  AnimationImportTest test;
  animation_test_begin(&test);
  /* The second strided translation is overwritten by sparse index 1. Padding
   * has distinct values so a tightly packed read cannot satisfy the oracle. */
  const float32_t base[] = {0, 2, 1, 2, 3, -99, 4, 5, 6, -88};
  const float32_t sparse[] = {7, 8, 9};
  const float32_t rotation[] = {0, 0, 0, 1.0000002f, 0, 0, 0, -1};
  const float32_t scale[] = {0.0001f, 0.0001f, 0.0001f,
                             0.0002f, 0.0002f, 0.0002f};
  uint8_t binary[112] = {0};
  animation_test_floats(binary, base, ArrayCount(base));
  binary[40] = 1u;
  animation_test_floats(binary + 44u, sparse, ArrayCount(sparse));
  animation_test_floats(binary + 56u, rotation, ArrayCount(rotation));
  animation_test_floats(binary + 88u, scale, ArrayCount(scale));
  const char json[] =
      "{\"asset\":{\"version\":\"2.0\"},"
      "\"nodes\":[{\"name\":\"child\",\"translation\":[0,5,0]},"
      "{\"name\":\"root\",\"children\":[0],\"translation\":[10,0,0]}],"
      "\"skins\":[{\"name\":\"skin\",\"joints\":[0,1]}],"
      "\"buffers\":[{\"uri\":\"source.bin\",\"byteLength\":112}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteLength\":8},"
      "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":32,\"byteStride\":16},"
      "{\"buffer\":0,\"byteOffset\":40,\"byteLength\":1},"
      "{\"buffer\":0,\"byteOffset\":44,\"byteLength\":12},"
      "{\"buffer\":0,\"byteOffset\":56,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":88,\"byteLength\":24}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
      "\"type\":\"SCALAR\",\"min\":[0],\"max\":[2]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\","
      "\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":2,\"componentType\":"
      "5121},"
      "\"values\":{\"bufferView\":3}}},"
      "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"},"
      "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}]"
      ","
      "\"animations\":[{\"name\":\"source_clip\",\"samplers\":["
      "{\"input\":0,\"output\":1,\"interpolation\":\"LINEAR\"},"
      "{\"input\":0,\"output\":2,\"interpolation\":\"STEP\"},"
      "{\"input\":0,\"output\":3}],\"channels\":["
      "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},"
      "{\"sampler\":1,\"target\":{\"node\":0,\"path\":\"rotation\"}},"
      "{\"sampler\":2,\"target\":{\"node\":1,\"path\":\"scale\"}}]}]}";
  animation_test_write(test.binary_path, binary, sizeof(binary));
  animation_test_write(test.gltf_path, json, sizeof(json) - 1u);
  VkrAnimationAsset asset = {0};
  const char *error = NULL;
  assert(animation_test_import(&test, test.gltf_path, &asset, &error));
  assert(!error && asset.node_count == 2u && asset.skin_count == 1u);
  assert(asset.nodes[0].parent == 1u);
  assert(asset.nodes[1].parent == VKR_ANIMATION_NO_NODE);
  assert(asset.node_order[0] == 1u && asset.node_order[1] == 0u);
  assert(asset.nodes[0].rest.translation.y == 5.0f);
  assert(asset.nodes[1].rest.translation.x == 10.0f);
  assert(asset.nodes[0].name.length == 5u);
  assert(MemCompare(asset.nodes[0].name.str, "child", 5u) == 0);
  assert(asset.skins[0].joints[0] == 0u && asset.skins[0].joints[1] == 1u);
  assert(asset.skins[0].skeleton_node == VKR_ANIMATION_NO_NODE);
  for (uint32_t joint = 0; joint < 2u; ++joint) {
    for (uint32_t i = 0; i < 16u; ++i) {
      const float32_t expected = i % 5u == 0u ? 1.0f : 0.0f;
      assert(asset.skins[0].inverse_bind[joint].elements[i] == expected);
    }
  }
  assert(asset.clip_count == 1u && asset.clips[0].duration == 2.0f);
  assert(asset.clips[0].channel_count == 3u);
  const VkrAnimationChannel *channels = asset.clips[0].channels;
  assert(channels[0].path == VKR_ANIMATION_TRANSLATION);
  assert(channels[0].interpolation == VKR_ANIMATION_LINEAR);
  assert(channels[0].times[0] == 0.0f && channels[0].times[1] == 2.0f);
  assert(channels[0].values[0].x == 1.0f);
  assert(channels[0].values[0].y == 2.0f);
  assert(channels[0].values[0].z == 3.0f);
  assert(channels[0].values[1].x == 7.0f);
  assert(channels[0].values[1].y == 8.0f);
  assert(channels[0].values[1].z == 9.0f);
  assert(channels[0].values[1].w == 0.0f);
  assert(channels[1].path == VKR_ANIMATION_ROTATION);
  assert(channels[1].interpolation == VKR_ANIMATION_STEP);
  assert(channels[1].values[0].w == 1.0000002f);
  assert(channels[1].values[1].w == -1.0f);
  assert(channels[2].path == VKR_ANIMATION_SCALE && channels[2].node == 1u);
  assert(channels[2].interpolation == VKR_ANIMATION_LINEAR);
  assert(channels[2].values[0].x == 0.0001f);
  assert(channels[2].values[1].z == 0.0002f);
  animation_test_end(&test);
}

static void animation_test_glb_cubic_triplets(void) {
  printf("  Running animation_test_glb_cubic_triplets...\n");
  AnimationImportTest test;
  animation_test_begin(&test);
  /* Derivatives deliberately are not unit quaternions. Only the central values
   * in each glTF in/value/out triplet carry that invariant. */
  const float32_t input[] = {0, 2};
  const float32_t output[] = {0, 0, 2, 0, 0, 0, 0, 1, 0, 3, 0, 0,
                              4, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 5};
  uint8_t binary[104] = {0};
  animation_test_floats(binary, input, ArrayCount(input));
  animation_test_floats(binary + 8u, output, ArrayCount(output));
  const char json[] =
      "{\"asset\":{\"version\":\"2.0\"},\"nodes\":["
      "{\"matrix\":[1,0,0,0,0.25,1,0,0,0,0,1,0,3,4,5,1],\"children\":[1]},"
      "{}],\"buffers\":[{\"byteLength\":104}],\"bufferViews\":["
      "{\"buffer\":0,\"byteLength\":8},"
      "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":96}],\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":"
      "\"SCALAR\","
      "\"min\":[0],\"max\":[2]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":6,\"type\":\"VEC4\"}]"
      ","
      "\"animations\":[{\"samplers\":["
      "{\"input\":0,\"output\":1,\"interpolation\":\"CUBICSPLINE\"}],"
      "\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":"
      "\"rotation\"}}]}]}";
  animation_test_glb(&test, json, binary, sizeof(binary));
  VkrAnimationAsset asset = {0};
  const char *error = NULL;
  assert(animation_test_import(&test, test.glb_path, &asset, &error));
  assert(!error && asset.skin_count == 0u && asset.clip_count == 1u);
  assert(asset.nodes[0].matrix_authored);
  assert(asset.nodes[0].local.elements[4] == 0.25f);
  assert(asset.nodes[0].local.elements[12] == 3.0f);
  assert(asset.nodes[0].local.elements[13] == 4.0f);
  assert(asset.nodes[0].local.elements[14] == 5.0f);
  const VkrAnimationChannel *channel = &asset.clips[0].channels[0];
  assert(channel->node == 1u && channel->key_count == 2u);
  assert(channel->interpolation == VKR_ANIMATION_CUBIC_SPLINE);
  assert(channel->values[0].z == 2.0f);
  assert(channel->values[1].w == 1.0f);
  assert(channel->values[2].y == 3.0f);
  assert(channel->values[3].x == 4.0f);
  assert(channel->values[4].z == 1.0f);
  assert(channel->values[5].w == 5.0f);
  animation_test_end(&test);
}

static void animation_test_rejected_channels(void) {
  printf("  Running animation_test_rejected_channels...\n");
  const char translation[] =
      "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}";
  const struct {
    const char *name;
    const char *channels;
    const char *type;
    float32_t first_time;
    float32_t last_time;
    float32_t values[8];
  } cases[] = {
      {.name = "duplicate time",
       .channels = translation,
       .type = "VEC3",
       .first_time = 0,
       .last_time = 0,
       .values = {1, 2, 3, 4, 5, 6}},
      {.name = "decreasing time",
       .channels = translation,
       .type = "VEC3",
       .first_time = 1,
       .last_time = 0,
       .values = {1, 2, 3, 4, 5, 6}},
      {.name = "negative time",
       .channels = translation,
       .type = "VEC3",
       .first_time = -1,
       .last_time = 1,
       .values = {1, 2, 3, 4, 5, 6}},
      {.name = "nonfinite time",
       .channels = translation,
       .type = "VEC3",
       .first_time = 0,
       .last_time = NAN,
       .values = {1, 2, 3, 4, 5, 6}},
      {.name = "duplicate target",
       .channels =
           "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},"
           "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}",
       .type = "VEC3",
       .first_time = 0,
       .last_time = 1,
       .values = {1, 2, 3, 4, 5, 6}},
      {.name = "missing target node",
       .channels = "{\"sampler\":0,\"target\":{\"path\":\"translation\"}}",
       .type = "VEC3",
       .first_time = 0,
       .last_time = 1,
       .values = {1, 2, 3, 4, 5, 6}},
      {.name = "nonunit rotation",
       .channels =
           "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"rotation\"}}",
       .type = "VEC4",
       .first_time = 0,
       .last_time = 1,
       .values = {0, 0, 0, 2, 0, 0, 0, 1}},
      {.name = "nonfinite value",
       .channels = translation,
       .type = "VEC3",
       .first_time = 0,
       .last_time = 1,
       .values = {1, 2, 3, 4, INFINITY, 6}},
  };
  for (uint32_t i = 0; i < ArrayCount(cases); ++i) {
    AnimationImportTest test;
    animation_test_begin(&test);
    uint8_t binary[40] = {0};
    const float32_t times[] = {cases[i].first_time, cases[i].last_time};
    animation_test_floats(binary, times, ArrayCount(times));
    animation_test_floats(binary + 8u, cases[i].values,
                          ArrayCount(cases[i].values));
    char json[2048];
    const int32_t length = snprintf(
        json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{}],"
        "\"buffers\":[{\"byteLength\":40}],\"bufferViews\":["
        "{\"buffer\":0,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":32}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
        "\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"%s\"}]"
        ","
        "\"animations\":[{\"samplers\":[{\"input\":0,\"output\":1}],"
        "\"channels\":[%s]}]}",
        cases[i].type, cases[i].channels);
    assert(length > 0 && (uint32_t)length < sizeof(json));
    animation_test_glb(&test, json, binary, sizeof(binary));
    VkrAnimationAsset asset = {.node_count = 99u};
    const char *error = NULL;
    const bool8_t imported =
        animation_test_import(&test, test.glb_path, &asset, &error);
    if (imported) {
      fprintf(stderr, "Animation importer accepted %s\n", cases[i].name);
    }
    assert(!imported && error && error[0]);
    assert(asset.node_count == 0u && !asset.nodes && !asset.clips);
    animation_test_end(&test);
  }
}

static void animation_test_unsupported_morph(void) {
  printf("  Running animation_test_unsupported_morph...\n");
  AnimationImportTest test;
  animation_test_begin(&test);
  /* A valid triangle and one morph target ensure rejection is about unsupported
   * animated weights rather than a missing target mesh or invalid accessors. */
  const float32_t values[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
                              1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1};
  uint8_t binary[88] = {0};
  animation_test_floats(binary, values, ArrayCount(values));
  const char json[] =
      "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"weights\":[0],\"primitives\":[{\"attributes\":{"
      "\"POSITION\":0},"
      "\"targets\":[{\"POSITION\":1}]}]}],\"buffers\":[{\"byteLength\":88}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},"
      "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
      "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":8},"
      "{\"buffer\":0,\"byteOffset\":80,\"byteLength\":8}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
      "\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
      "\"min\":[0,0,1],\"max\":[0,0,1]},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":"
      "\"SCALAR\","
      "\"min\":[0],\"max\":[1]},"
      "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":"
      "\"SCALAR\"}],"
      "\"animations\":[{\"samplers\":[{\"input\":2,\"output\":3}],"
      "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":"
      "\"weights\"}}]}]}";
  animation_test_glb(&test, json, binary, sizeof(binary));
  VkrAnimationAsset asset = {0};
  const char *error = NULL;
  assert(!animation_test_import(&test, test.glb_path, &asset, &error));
  assert(error && error[0] && !asset.nodes && asset.node_count == 0u);
  animation_test_end(&test);
}

static void animation_test_required_extension(void) {
  printf("  Running animation_test_required_extension...\n");
  AnimationImportTest test;
  animation_test_begin(&test);
  const char json[] =
      "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{}],"
      "\"extensionsUsed\":[\"VKR_test_required_animation\"],"
      "\"extensionsRequired\":[\"VKR_test_required_animation\"]}";
  animation_test_write(test.gltf_path, json, sizeof(json) - 1u);
  VkrAnimationAsset asset = {0};
  const char *error = NULL;
  assert(!animation_test_import(&test, test.gltf_path, &asset, &error));
  assert(error && error[0] && !asset.nodes && asset.node_count == 0u);
  animation_test_end(&test);
}

bool32_t run_animation_import_tests(void) {
  printf("Running animation importer tests...\n");
  animation_test_sparse_strided_gltf();
  animation_test_glb_cubic_triplets();
  animation_test_rejected_channels();
  animation_test_unsupported_morph();
  animation_test_required_extension();
  printf("Animation importer tests PASSED\n");
  return true_v;
}
