#include "material_pbr_tests.h"

#include "containers/str.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_texture_system.h"

#include <ktx.h>
#include <vulkan/vulkan_core.h>

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct MaterialPbrMockPublisherState {
  uint32_t texture_create_calls;
  uint32_t texture_destroy_calls;
  uint32_t texture_destroy_rejections;
  bool8_t texture_upload_available;
  bool8_t material_live[256];
  VkrTextureHandle material_textures[256][VKR_TEXTURE_SLOT_COUNT];
} MaterialPbrMockPublisherState;

typedef struct MaterialPbrTestContext {
  RendererFrontend renderer;
  MaterialPbrMockPublisherState publisher_state;
  VkrAssetPublisher asset_publisher;
  VkrTextureSystem texture_system;
  VkrMaterialSystem material_system;
  VkrResourceLoader material_loader;
  Arena *temp_arena;
  VkrAllocator temp_allocator;
} MaterialPbrTestContext;

static void material_pbr_mock_get_device_information(
    void *state, VkrDeviceInformation *device_information, Arena *temp_arena) {
  (void)state;
  (void)temp_arena;
  assert(device_information != NULL);

  MemZero(device_information, sizeof(*device_information));
}

static const VkrRendererImplOps material_pbr_impl_ops = {
    .get_device_information = material_pbr_mock_get_device_information,
};

static bool8_t
material_pbr_mock_texture_upload_available(void *publisher_state,
                                           uint64_t upload_bytes) {
  MaterialPbrMockPublisherState *state = publisher_state;
  assert(state != NULL);
  return upload_bytes != 0u && state->texture_upload_available;
}

static bool8_t material_pbr_mock_publish_texture(
    void *publisher_state, VkrTextureHandle handle,
    const struct VkrTexturePreparedLoad *texture) {
  (void)handle;
  (void)texture;
  MaterialPbrMockPublisherState *state = publisher_state;
  assert(state != NULL);
  state->texture_create_calls++;
  return true_v;
}

static bool8_t material_pbr_mock_publish_writable_texture(
    void *publisher_state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  (void)handle;
  (void)description;
  MaterialPbrMockPublisherState *state = publisher_state;
  assert(state != NULL);
  state->texture_create_calls++;
  return true_v;
}

static bool8_t material_pbr_mock_unpublish_texture(void *publisher_state,
                                                   VkrTextureHandle handle) {
  MaterialPbrMockPublisherState *state = publisher_state;
  assert(state != NULL);
  for (uint32_t material_index = 0u;
       material_index < ArrayCount(state->material_live); ++material_index) {
    if (!state->material_live[material_index]) {
      continue;
    }
    for (uint32_t slot = 0u; slot < VKR_TEXTURE_SLOT_COUNT; ++slot) {
      const VkrTextureHandle referenced =
          state->material_textures[material_index][slot];
      if (referenced.id == handle.id &&
          referenced.generation == handle.generation) {
        state->texture_destroy_rejections++;
        return false_v;
      }
    }
  }
  state->texture_destroy_calls++;
  return true_v;
}

static bool8_t
material_pbr_mock_publish_material(void *publisher_state,
                                   VkrMaterialHandle handle,
                                   const struct VkrMaterial *material) {
  MaterialPbrMockPublisherState *state = publisher_state;
  assert(state != NULL);
  assert(material != NULL);
  assert(handle.id > 0u && handle.id <= ArrayCount(state->material_live));
  const uint32_t material_index = handle.id - 1u;
  state->material_live[material_index] = true_v;
  MemZero(state->material_textures[material_index],
          sizeof(state->material_textures[material_index]));
  for (uint32_t slot = 0u; slot < VKR_TEXTURE_SLOT_COUNT; ++slot) {
    if (material->textures[slot].enabled) {
      state->material_textures[material_index][slot] =
          material->textures[slot].handle;
    }
  }
  return true_v;
}

static bool8_t material_pbr_mock_unpublish_material(void *publisher_state,
                                                    VkrMaterialHandle handle) {
  MaterialPbrMockPublisherState *state = publisher_state;
  assert(state != NULL);
  assert(handle.id > 0u && handle.id <= ArrayCount(state->material_live));
  const uint32_t material_index = handle.id - 1u;
  state->material_live[material_index] = false_v;
  MemZero(state->material_textures[material_index],
          sizeof(state->material_textures[material_index]));
  return true_v;
}

static bool8_t material_pbr_test_make_dir(const char *path) {
  if (!path || path[0] == '\0') {
    return false_v;
  }

#if defined(_WIN32)
  int result = _mkdir(path);
#else
  int result = mkdir(path, 0755);
#endif

  return (result == 0 || errno == EEXIST) ? true_v : false_v;
}

static void material_pbr_test_remove_file(const char *path) {
  if (!path || path[0] == '\0') {
    return;
  }

  char resolved_path[1024] = {0};
  if (path[0] == '/') {
    snprintf(resolved_path, sizeof(resolved_path), "%s", path);
  } else {
    snprintf(resolved_path, sizeof(resolved_path), "%s%s", PROJECT_SOURCE_DIR,
             path);
  }

#if defined(_WIN32)
  _unlink(resolved_path);
#else
  unlink(resolved_path);
#endif
}

static bool8_t material_pbr_test_write_text_file(const char *path,
                                                 const char *text) {
  if (!path || !text) {
    return false_v;
  }

  FILE *file = fopen(path, "wb");
  if (!file) {
    return false_v;
  }

  const size_t len = strlen(text);
  const size_t written = fwrite(text, 1, len, file);
  fclose(file);
  return written == len ? true_v : false_v;
}

static bool8_t material_pbr_test_write_uastc_texture(const char *path,
                                                     uint32_t layer_count,
                                                     uint32_t face_count) {
  if (!path || layer_count == 0u || (face_count != 1u && face_count != 6u)) {
    return false_v;
  }
  const ktxTextureCreateInfo create_info = {
      .vkFormat = VK_FORMAT_R8G8B8A8_UNORM,
      .baseWidth = 4u,
      .baseHeight = 4u,
      .baseDepth = 1u,
      .numDimensions = 2u,
      .numLevels = 1u,
      .numLayers = layer_count,
      .numFaces = face_count,
      .isArray = layer_count > 1u ? KTX_TRUE : KTX_FALSE,
      .generateMipmaps = KTX_FALSE,
  };
  ktxTexture2 *texture = NULL;
  KTX_error_code result = ktxTexture2_Create(
      &create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
  if (result != KTX_SUCCESS || !texture) {
    return false_v;
  }
  uint8_t pixels[4u * 4u * 4u];
  for (uint32_t layer = 0u; layer < layer_count; ++layer) {
    for (uint32_t face = 0u; face < face_count; ++face) {
      for (uint32_t i = 0u; i < sizeof(pixels); i += 4u) {
        pixels[i + 0u] = (uint8_t)(32u + layer * 17u);
        pixels[i + 1u] = (uint8_t)(48u + face * 19u);
        pixels[i + 2u] = (uint8_t)(64u + layer + face);
        pixels[i + 3u] = 255u;
      }
      result = ktxTexture_SetImageFromMemory(ktxTexture(texture), 0u, layer,
                                             face, pixels, sizeof(pixels));
      if (result != KTX_SUCCESS) {
        ktxTexture_Destroy(ktxTexture(texture));
        return false_v;
      }
    }
  }
  ktxBasisParams basis = {
      .structSize = sizeof(ktxBasisParams),
      .uastc = KTX_TRUE,
      .threadCount = 1u,
      .uastcFlags = KTX_PACK_UASTC_LEVEL_DEFAULT,
  };
  result = ktxTexture2_CompressBasisEx(texture, &basis);
  if (result == KTX_SUCCESS) {
    result = ktxTexture_WriteToNamedFile(ktxTexture(texture), path);
  }
  ktxTexture_Destroy(ktxTexture(texture));
  return result == KTX_SUCCESS ? true_v : false_v;
}

static void material_pbr_test_ensure_dirs(void) {
  char tests_tmp[1024];
  snprintf(tests_tmp, sizeof(tests_tmp), "%stests/tmp", PROJECT_SOURCE_DIR);
  assert(material_pbr_test_make_dir(tests_tmp) == true_v);

  char pbr_tmp[1024];
  snprintf(pbr_tmp, sizeof(pbr_tmp), "%stests/tmp/material_pbr",
           PROJECT_SOURCE_DIR);
  assert(material_pbr_test_make_dir(pbr_tmp) == true_v);
}

static void material_pbr_test_init_renderer(MaterialPbrTestContext *ctx) {
  assert(ctx != NULL);

  MemZero(ctx, sizeof(*ctx));
  MemZero(&ctx->renderer, sizeof(ctx->renderer));

  ctx->renderer.arena = arena_create(MB(8), MB(8));
  assert(ctx->renderer.arena != NULL);
  ctx->renderer.allocator = (VkrAllocator){.ctx = ctx->renderer.arena};
  assert(vkr_allocator_arena(&ctx->renderer.allocator));

  ctx->renderer.scratch_arena = arena_create(MB(8), MB(8));
  assert(ctx->renderer.scratch_arena != NULL);
  ctx->renderer.scratch_allocator =
      (VkrAllocator){.ctx = ctx->renderer.scratch_arena};
  assert(vkr_allocator_arena(&ctx->renderer.scratch_allocator));

  ctx->renderer.impl.ops = &material_pbr_impl_ops;
  ctx->renderer.impl.state = &ctx->publisher_state;
  ctx->publisher_state.texture_upload_available = true_v;
  ctx->asset_publisher = (VkrAssetPublisher){
      .state = &ctx->publisher_state,
      .texture_upload_available = material_pbr_mock_texture_upload_available,
      .publish_texture = material_pbr_mock_publish_texture,
      .publish_writable_texture = material_pbr_mock_publish_writable_texture,
      .unpublish_texture = material_pbr_mock_unpublish_texture,
      .publish_material = material_pbr_mock_publish_material,
      .unpublish_material = material_pbr_mock_unpublish_material,
  };
}

static void material_pbr_test_shutdown_renderer(MaterialPbrTestContext *ctx) {
  if (!ctx) {
    return;
  }

  if (ctx->renderer.scratch_arena) {
    arena_destroy(ctx->renderer.scratch_arena);
    ctx->renderer.scratch_arena = NULL;
  }

  if (ctx->renderer.arena) {
    arena_destroy(ctx->renderer.arena);
    ctx->renderer.arena = NULL;
  }
}

static bool8_t material_pbr_test_init_context(MaterialPbrTestContext *ctx) {
  assert(ctx != NULL);

  material_pbr_test_init_renderer(ctx);

  VkrTextureSystemConfig texture_cfg = {
      .max_texture_count = 256,
      .asset_publisher = &ctx->asset_publisher,
  };
  if (!vkr_texture_system_init(&ctx->renderer, &texture_cfg, NULL,
                               &ctx->texture_system)) {
    material_pbr_test_shutdown_renderer(ctx);
    return false_v;
  }

  VkrMaterialSystemConfig material_cfg = {
      .max_material_count = 128,
      .asset_publisher = &ctx->asset_publisher,
  };
  if (!vkr_material_system_init(&ctx->material_system, ctx->renderer.arena,
                                &ctx->texture_system, &material_cfg)) {
    vkr_texture_system_shutdown(&ctx->texture_system);
    material_pbr_test_shutdown_renderer(ctx);
    return false_v;
  }

  ctx->material_loader = vkr_material_loader_create();
  ctx->material_loader.id = 1;
  ctx->material_loader.renderer = &ctx->renderer;
  ctx->material_loader.resource_system = &ctx->material_system;

  ctx->temp_arena = arena_create(MB(8), MB(8));
  if (!ctx->temp_arena) {
    vkr_material_system_shutdown(&ctx->material_system);
    vkr_texture_system_shutdown(&ctx->texture_system);
    material_pbr_test_shutdown_renderer(ctx);
    return false_v;
  }

  ctx->temp_allocator = (VkrAllocator){.ctx = ctx->temp_arena};
  if (!vkr_allocator_arena(&ctx->temp_allocator)) {
    arena_destroy(ctx->temp_arena);
    ctx->temp_arena = NULL;
    vkr_material_system_shutdown(&ctx->material_system);
    vkr_texture_system_shutdown(&ctx->texture_system);
    material_pbr_test_shutdown_renderer(ctx);
    return false_v;
  }

  return true_v;
}

static void material_pbr_test_shutdown_context(MaterialPbrTestContext *ctx) {
  if (!ctx) {
    return;
  }

  vkr_material_system_shutdown(&ctx->material_system);
  vkr_texture_system_shutdown(&ctx->texture_system);

  if (ctx->temp_arena) {
    arena_destroy(ctx->temp_arena);
    ctx->temp_arena = NULL;
  }

  material_pbr_test_shutdown_renderer(ctx);
}

static bool8_t material_pbr_test_load_material(
    MaterialPbrTestContext *ctx, const char *stem, const char *content,
    char *out_path, size_t out_path_size, VkrResourceHandleInfo *out_info) {
  assert(ctx != NULL);
  assert(stem != NULL);
  assert(content != NULL);
  assert(out_path != NULL);
  assert(out_path_size > 0);
  assert(out_info != NULL);

  snprintf(out_path, out_path_size, "tests/tmp/material_pbr/%s.mt", stem);

  char absolute_path[1024] = {0};
  snprintf(absolute_path, sizeof(absolute_path), "%s%s", PROJECT_SOURCE_DIR,
           out_path);

  material_pbr_test_remove_file(out_path);
  if (!material_pbr_test_write_text_file(absolute_path, content)) {
    return false_v;
  }

  String8 path = string8_create_from_cstr((const uint8_t *)out_path,
                                          string_length(out_path));

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&ctx->temp_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return false_v;
  }

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  MemZero(out_info, sizeof(*out_info));
  const bool8_t loaded = ctx->material_loader.load(
      &ctx->material_loader, path, &ctx->temp_allocator, out_info, &err);
  if (loaded && out_info->as.material.id != 0u) {
    /* This fixture invokes the material loader directly, outside the global
       resource registry. Keep its texture entries fixture-owned so material
       unload cannot dispatch through an unrelated registry from another test.
     */
    const VkrMaterial *material = vkr_material_system_get_by_handle(
        &ctx->material_system, out_info->as.material);
    for (uint32_t slot = 0u; material && slot < VKR_TEXTURE_SLOT_COUNT;
         ++slot) {
      const VkrTextureHandle texture = material->textures[slot].handle;
      if (texture.id == 0u ||
          texture.id > ctx->texture_system.textures.length) {
        continue;
      }
      const char *key =
          ctx->texture_system.texture_keys_by_index[texture.id - 1u];
      VkrTextureEntry *entry = key ? vkr_hash_table_get_VkrTextureEntry(
                                         &ctx->texture_system.texture_map, key)
                                   : NULL;
      if (entry) {
        entry->auto_release = false_v;
      }
    }
  }
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_UNKNOWN);

  return (loaded == true_v && err == VKR_RENDERER_ERROR_NONE &&
          out_info->as.material.id != 0)
             ? true_v
             : false_v;
}

static void
material_pbr_test_unload_material(MaterialPbrTestContext *ctx,
                                  const VkrResourceHandleInfo *handle_info,
                                  const char *path_cstr) {
  assert(ctx != NULL);
  assert(handle_info != NULL);
  assert(path_cstr != NULL);

  String8 path = string8_create_from_cstr((const uint8_t *)path_cstr,
                                          string_length(path_cstr));
  ctx->material_loader.unload(&ctx->material_loader, handle_info, path);
}

static const char *material_pbr_test_texture_key(const VkrTextureSystem *system,
                                                 VkrTextureHandle handle) {
  if (!system || handle.id == 0 || !system->texture_keys_by_index) {
    return NULL;
  }

  uint32_t index = handle.id - 1;
  if (index >= system->textures.length) {
    return NULL;
  }

  return system->texture_keys_by_index[index];
}

static bool8_t material_pbr_test_string_contains(const char *value,
                                                 const char *needle) {
  if (!value || !needle) {
    return false_v;
  }

  return strstr(value, needle) != NULL ? true_v : false_v;
}

static void
test_material_pbr_inference_from_scalar_keys(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_pbr_inference_from_scalar_keys...\n");

  const char *material_text = "pipeline=world\n"
                              "metallic=0.25\n"
                              "roughness=0.60\n"
                              "dielectric_specular=0.01,0.02,0.03\n";

  char material_path[1024] = {0};
  VkrResourceHandleInfo handle_info = {0};
  assert(material_pbr_test_load_material(
             ctx, "pbr_scalar_inference", material_text, material_path,
             sizeof(material_path), &handle_info) == true_v);

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &ctx->material_system, handle_info.as.material);
  assert(material != NULL);
  assert(material->material_type == VKR_MATERIAL_TYPE_PBR);
  assert(material->pipeline_id == VKR_PIPELINE_DOMAIN_WORLD);
  assert(fabsf(material->pbr.metallic - 0.25f) < 0.0001f);
  assert(fabsf(material->pbr.roughness - 0.60f) < 0.0001f);
  assert(fabsf(material->pbr.dielectric_specular.x - 0.01f) < 0.0001f);
  assert(fabsf(material->pbr.dielectric_specular.y - 0.02f) < 0.0001f);
  assert(fabsf(material->pbr.dielectric_specular.z - 0.03f) < 0.0001f);

  material_pbr_test_unload_material(ctx, &handle_info, material_path);
  material_pbr_test_remove_file(material_path);

  printf("  test_material_pbr_inference_from_scalar_keys PASSED\n");
}

static void
test_material_temporal_reactivity_authoring(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_temporal_reactivity_authoring...\n");

  const struct {
    const char *stem;
    const char *text;
    float32_t expected;
  } cases[] = {
      {"temporal_reactivity_default", "pipeline=world\n", 0.0f},
      {"temporal_reactivity_low",
       "pipeline=world\n"
       "temporal_reactivity=-0.5\n",
       0.0f},
      {"temporal_reactivity_high",
       "pipeline=world\n"
       "temporal_reactivity=1.5\n",
       1.0f},
      {"temporal_reactivity_authored",
       "pipeline=world\n"
       "temporal_reactivity=0.35\n",
       0.35f},
  };

  for (uint32_t i = 0u; i < ArrayCount(cases); ++i) {
    char material_path[1024] = {0};
    VkrResourceHandleInfo handle_info = {0};
    assert(material_pbr_test_load_material(ctx, cases[i].stem, cases[i].text,
                                           material_path, sizeof(material_path),
                                           &handle_info) == true_v);

    VkrMaterial *material = vkr_material_system_get_by_handle(
        &ctx->material_system, handle_info.as.material);
    assert(material != NULL);
    assert(fabsf(material->pbr.temporal_reactivity - cases[i].expected) <
           0.0001f);

    material_pbr_test_unload_material(ctx, &handle_info, material_path);
    material_pbr_test_remove_file(material_path);
  }

  printf("  test_material_temporal_reactivity_authoring PASSED\n");
}

static void test_material_transmission_is_independent_of_alpha(
    MaterialPbrTestContext *ctx) {
  printf("  Running test_material_transmission_is_independent_of_alpha...\n");

  const char *material_text = "type=pbr\n"
                              "alpha_mode=opaque\n"
                              "transmission_factor=0.75\n"
                              "ior=1.33\n"
                              "thickness_factor=0.40\n"
                              "attenuation_color=0.8,0.6,0.4\n"
                              "attenuation_distance=2.5\n";

  char material_path[1024] = {0};
  VkrResourceHandleInfo handle_info = {0};
  assert(material_pbr_test_load_material(ctx, "pbr_transmission", material_text,
                                         material_path, sizeof(material_path),
                                         &handle_info) == true_v);

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &ctx->material_system, handle_info.as.material);
  assert(material != NULL);
  assert(vkr_material_system_material_alpha_mode(
             &ctx->material_system, material) == VKR_MATERIAL_ALPHA_OPAQUE);
  assert(vkr_material_system_material_is_transmissive(&ctx->material_system,
                                                      material) == true_v);
  assert(fabsf(material->pbr.transmission_factor - 0.75f) < 0.0001f);
  assert(fabsf(material->pbr.ior - 1.33f) < 0.0001f);
  assert(fabsf(material->pbr.thickness_factor - 0.40f) < 0.0001f);
  assert(fabsf(material->pbr.attenuation_color.x - 0.8f) < 0.0001f);
  assert(fabsf(material->pbr.attenuation_distance - 2.5f) < 0.0001f);

  material_pbr_test_unload_material(ctx, &handle_info, material_path);
  material_pbr_test_remove_file(material_path);
  printf("  test_material_transmission_is_independent_of_alpha PASSED\n");
}

static void
test_material_pbr_alias_slots_and_inference(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_pbr_alias_slots_and_inference...\n");

  char material_text[4096] = {0};
  snprintf(material_text, sizeof(material_text),
           "pipeline=world\n"
           "diffuse_texture=%s\n"
           "emission_texture=%s\n",
           "assets/textures/detmoldura_02_color.png",
           "assets/textures/detmoldura_02_color.png");

  char material_path[1024] = {0};
  VkrResourceHandleInfo handle_info = {0};
  assert(material_pbr_test_load_material(
             ctx, "pbr_alias_inference", material_text, material_path,
             sizeof(material_path), &handle_info) == true_v);

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &ctx->material_system, handle_info.as.material);
  assert(material != NULL);
  assert(material->material_type == VKR_MATERIAL_TYPE_PBR);

  const VkrTextureHandle default_diffuse =
      vkr_texture_system_get_default_diffuse_handle(&ctx->texture_system);
  const VkrTextureHandle diffuse_handle =
      material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle;
  const VkrTextureHandle emission_handle =
      material->textures[VKR_TEXTURE_SLOT_EMISSION].handle;

  assert(diffuse_handle.id != 0);
  assert(emission_handle.id != 0);
  assert(diffuse_handle.id != default_diffuse.id);
  assert(emission_handle.id != default_diffuse.id);

  const char *diffuse_key =
      material_pbr_test_texture_key(&ctx->texture_system, diffuse_handle);
  assert(diffuse_key != NULL);
  assert(material_pbr_test_string_contains(diffuse_key, "tc=color_linear") ==
         true_v);

  material_pbr_test_unload_material(ctx, &handle_info, material_path);
  material_pbr_test_remove_file(material_path);

  printf("  test_material_pbr_alias_slots_and_inference PASSED\n");
}

static void
test_material_alpha_mode_cutout_defaults(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_alpha_mode_cutout_defaults...\n");

  const struct {
    const char *stem;
    const char *content;
  } cases[] = {
      {.stem = "pbr_alpha_cutout_default", .content = "alpha_mode=cutout\n"},
      {.stem = "pbr_alpha_cutout_zero",
       .content = "alpha_mode=cutout\n"
                  "alpha_cutoff=0.0\n"},
  };

  for (uint32_t i = 0; i < ArrayCount(cases); ++i) {
    char material_path[1024] = {0};
    VkrResourceHandleInfo handle_info = {0};
    assert(material_pbr_test_load_material(ctx, cases[i].stem, cases[i].content,
                                           material_path, sizeof(material_path),
                                           &handle_info) == true_v);

    VkrMaterial *material = vkr_material_system_get_by_handle(
        &ctx->material_system, handle_info.as.material);
    assert(material != NULL);
    assert(material->alpha_mode_explicit == true_v);
    assert(material->alpha_mode == VKR_MATERIAL_ALPHA_CUTOUT);
    assert(fabsf(material->alpha_cutoff - VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT) <
           0.0001f);
    assert(vkr_material_system_material_alpha_mode(
               &ctx->material_system, material) == VKR_MATERIAL_ALPHA_CUTOUT);
    assert(vkr_material_system_material_has_transparency(&ctx->material_system,
                                                         material) == false_v);
    assert(vkr_material_system_material_uses_cutout(&ctx->material_system,
                                                    material) == true_v);

    material_pbr_test_unload_material(ctx, &handle_info, material_path);
    material_pbr_test_remove_file(material_path);
  }

  printf("  test_material_alpha_mode_cutout_defaults PASSED\n");
}

static void test_material_double_sided_state(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_double_sided_state...\n");

  char material_path[1024] = {0};
  VkrResourceHandleInfo handle_info = {0};
  assert(material_pbr_test_load_material(
             ctx, "pbr_double_sided", "type=pbr\ndouble_sided=true\n",
             material_path, sizeof(material_path), &handle_info) == true_v);

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &ctx->material_system, handle_info.as.material);
  assert(material != NULL);
  assert(material->double_sided == true_v);

  material_pbr_test_unload_material(ctx, &handle_info, material_path);
  material_pbr_test_remove_file(material_path);

  printf("  test_material_double_sided_state PASSED\n");
}

static void
test_material_legacy_cutout_compatibility(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_legacy_cutout_compatibility...\n");

  char material_path[1024] = {0};
  VkrResourceHandleInfo handle_info = {0};
  assert(material_pbr_test_load_material(
             ctx, "pbr_legacy_cutout", "cutout=true\n", material_path,
             sizeof(material_path), &handle_info) == true_v);

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &ctx->material_system, handle_info.as.material);
  assert(material != NULL);
  assert(material->alpha_mode == VKR_MATERIAL_ALPHA_OPAQUE);
  assert(material->alpha_mode_explicit == false_v);
  assert(fabsf(material->alpha_cutoff - VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT) <
         0.0001f);

  material_pbr_test_unload_material(ctx, &handle_info, material_path);
  material_pbr_test_remove_file(material_path);

  printf("  test_material_legacy_cutout_compatibility PASSED\n");
}

static void
test_material_texture_intent_query_normalization(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_texture_intent_query_normalization...\n");

  char material_text[4096] = {0};
  snprintf(material_text, sizeof(material_text),
           "type=pbr\n"
           "base_color_texture=%s\n"
           "base_color_colorspace=srgb\n",
           "assets/textures/detmoldura_02_color.png");

  char material_path[1024] = {0};
  VkrResourceHandleInfo handle_info = {0};
  assert(material_pbr_test_load_material(
             ctx, "pbr_intent_normalization", material_text, material_path,
             sizeof(material_path), &handle_info) == true_v);

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &ctx->material_system, handle_info.as.material);
  assert(material != NULL);

  const VkrTextureHandle diffuse_handle =
      material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle;
  const VkrTextureHandle default_diffuse =
      vkr_texture_system_get_default_diffuse_handle(&ctx->texture_system);
  assert(diffuse_handle.id != 0);
  assert(diffuse_handle.id != default_diffuse.id);

  const char *texture_key =
      material_pbr_test_texture_key(&ctx->texture_system, diffuse_handle);
  assert(texture_key != NULL);
  assert(material_pbr_test_string_contains(texture_key, "cs=srgb") == true_v);
  assert(material_pbr_test_string_contains(texture_key, "tc=color_srgb") ==
         true_v);

  material_pbr_test_unload_material(ctx, &handle_info, material_path);
  material_pbr_test_remove_file(material_path);

  printf("  test_material_texture_intent_query_normalization PASSED\n");
}

static void test_material_texture_intent_override_is_deterministic(
    MaterialPbrTestContext *ctx) {
  printf(
      "  Running test_material_texture_intent_override_is_deterministic...\n");

  char material_text[4096] = {0};
  snprintf(material_text, sizeof(material_text),
           "type=pbr\n"
           "base_color_texture=%s?cs=linear&tc=data_mask\n"
           "base_color_colorspace=srgb\n",
           "assets/textures/detmoldura_02_color.png");

  char material_path[1024] = {0};
  VkrResourceHandleInfo handle_info = {0};
  assert(material_pbr_test_load_material(
             ctx, "pbr_intent_override", material_text, material_path,
             sizeof(material_path), &handle_info) == true_v);

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &ctx->material_system, handle_info.as.material);
  assert(material != NULL);

  const VkrTextureHandle diffuse_handle =
      material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle;
  const char *texture_key =
      material_pbr_test_texture_key(&ctx->texture_system, diffuse_handle);
  assert(texture_key != NULL);
  assert(material_pbr_test_string_contains(texture_key, "cs=linear") == true_v);
  assert(material_pbr_test_string_contains(texture_key, "tc=data_mask") ==
         true_v);
  assert(material_pbr_test_string_contains(texture_key, "cs=srgb") == false_v);

  material_pbr_test_unload_material(ctx, &handle_info, material_path);
  material_pbr_test_remove_file(material_path);

  printf("  test_material_texture_intent_override_is_deterministic PASSED\n");
}

static void test_material_batch_load_honors_parsed_name_over_stem(
    MaterialPbrTestContext *ctx) {
  printf(
      "  Running test_material_batch_load_honors_parsed_name_over_stem...\n");

  char dir_a_abs[1024] = {0};
  char dir_b_abs[1024] = {0};
  snprintf(dir_a_abs, sizeof(dir_a_abs), "%stests/tmp/material_pbr/collision_a",
           PROJECT_SOURCE_DIR);
  snprintf(dir_b_abs, sizeof(dir_b_abs), "%stests/tmp/material_pbr/collision_b",
           PROJECT_SOURCE_DIR);
  assert(material_pbr_test_make_dir(dir_a_abs) == true_v);
  assert(material_pbr_test_make_dir(dir_b_abs) == true_v);

  const char *path_a_rel = "tests/tmp/material_pbr/collision_a/shared.mt";
  const char *path_b_rel = "tests/tmp/material_pbr/collision_b/shared.mt";
  char path_a_abs[1024] = {0};
  char path_b_abs[1024] = {0};
  snprintf(path_a_abs, sizeof(path_a_abs), "%s%s", PROJECT_SOURCE_DIR,
           path_a_rel);
  snprintf(path_b_abs, sizeof(path_b_abs), "%s%s", PROJECT_SOURCE_DIR,
           path_b_rel);

  material_pbr_test_remove_file(path_a_rel);
  material_pbr_test_remove_file(path_b_rel);

  assert(material_pbr_test_write_text_file(path_a_abs,
                                           "name=shared\n"
                                           "pipeline=world\n") == true_v);
  assert(material_pbr_test_write_text_file(path_b_abs,
                                           "name=collision_unique_b\n"
                                           "pipeline=world\n") == true_v);

  String8 batch_paths[2] = {
      string8_create_from_cstr((const uint8_t *)path_a_rel,
                               string_length(path_a_rel)),
      string8_create_from_cstr((const uint8_t *)path_b_rel,
                               string_length(path_b_rel)),
  };
  VkrResourceHandleInfo out_handles[2] = {0};
  VkrRendererError out_errors[2] = {0};

  uint32_t loaded = ctx->material_loader.batch_load(
      &ctx->material_loader, batch_paths, 2, &ctx->temp_allocator, out_handles,
      out_errors);

  assert(loaded == 2u);
  assert(out_errors[0] == VKR_RENDERER_ERROR_NONE);
  assert(out_errors[1] == VKR_RENDERER_ERROR_NONE);
  assert(out_handles[0].type == VKR_RESOURCE_TYPE_MATERIAL);
  assert(out_handles[1].type == VKR_RESOURCE_TYPE_MATERIAL);
  assert(out_handles[0].as.material.id != 0);
  assert(out_handles[1].as.material.id != 0);
  assert(out_handles[0].as.material.id != out_handles[1].as.material.id);

  VkrMaterial *material_a = vkr_material_system_get_by_handle(
      &ctx->material_system, out_handles[0].as.material);
  VkrMaterial *material_b = vkr_material_system_get_by_handle(
      &ctx->material_system, out_handles[1].as.material);
  assert(material_a != NULL);
  assert(material_b != NULL);
  assert(strcmp(material_a->name, "shared") == 0);
  assert(strcmp(material_b->name, "collision_unique_b") == 0);

  material_pbr_test_remove_file(path_a_rel);
  material_pbr_test_remove_file(path_b_rel);

  printf("  test_material_batch_load_honors_parsed_name_over_stem PASSED\n");
}

static void test_async_emissive_texture_uses_black_pending_fallback(
    MaterialPbrTestContext *ctx) {
  printf(
      "  Running test_async_emissive_texture_uses_black_pending_fallback...\n");

  const char *material_path =
      "tests/tmp/material_pbr/async_emissive_pending.mt";
  char absolute_path[1024] = {0};
  snprintf(absolute_path, sizeof(absolute_path), "%s%s", PROJECT_SOURCE_DIR,
           material_path);
  material_pbr_test_remove_file(material_path);
  assert(material_pbr_test_write_text_file(
             absolute_path,
             "type=pbr\n"
             "emissive_factor=10.0,10.0,7.0\n"
             "emissive_texture=assets/textures/objects/props/bistro/"
             "paris_ceiling_lamp_01/paris_ceiling_lamp_01_emi.png\n") ==
         true_v);

  String8 path = string8_create_from_cstr((const uint8_t *)material_path,
                                          string_length(material_path));
  VkrAllocatorScope scope = vkr_allocator_begin_scope(&ctx->temp_allocator);
  assert(vkr_allocator_scope_is_valid(&scope));

  void *payload = NULL;
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(ctx->material_loader.prepare_async(&ctx->material_loader, path,
                                            &ctx->temp_allocator, &payload,
                                            &error) == true_v);
  assert(payload != NULL);
  assert(error == VKR_RENDERER_ERROR_NONE);

  const uint32_t stream_count_before =
      ctx->material_system.texture_stream_count;
  VkrResourceHandleInfo handle_info = {0};
  assert(ctx->material_loader.finalize_async(&ctx->material_loader, path,
                                             payload, &handle_info,
                                             &error) == true_v);
  ctx->material_loader.release_async_payload(&ctx->material_loader, payload);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_UNKNOWN);

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &ctx->material_system, handle_info.as.material);
  assert(material != NULL);
  const VkrMaterialTexture emissive =
      material->textures[VKR_TEXTURE_SLOT_EMISSION];
  const VkrTextureHandle black =
      vkr_texture_system_get_default_emissive_handle(&ctx->texture_system);
  assert(emissive.enabled == true_v);
  assert(emissive.handle.id == black.id);
  assert(emissive.handle.generation == black.generation);

  assert(ctx->material_system.texture_stream_count == stream_count_before + 1u);
  const VkrMaterialTextureStream *stream =
      &ctx->material_system.texture_streams[stream_count_before];
  assert(stream->material.id == handle_info.as.material.id);
  assert(stream->slot == VKR_TEXTURE_SLOT_EMISSION);
  assert(stream->state == VKR_MATERIAL_TEXTURE_RESIDENCY_QUEUED);

  material_pbr_test_unload_material(ctx, &handle_info, material_path);
  assert(ctx->material_system.texture_stream_count == stream_count_before);
  material_pbr_test_remove_file(material_path);

  printf("  test_async_emissive_texture_uses_black_pending_fallback PASSED\n");
}

static void
test_material_texture_stream_queue_is_bounded(MaterialPbrTestContext *ctx) {
  VkrMaterialSystem *system = &ctx->material_system;
  assert(system->texture_stream_budget_bytes == UINT64_MAX);
  assert(system->texture_stream_count == 0u);
  const uint32_t capacity = system->texture_stream_capacity;
  system->texture_stream_capacity = 2u;
  const VkrMaterialHandle material = {.id = 123u, .generation = 7u};

  assert(vkr_material_system_stream_texture(system, material,
                                            VKR_TEXTURE_SLOT_DIFFUSE,
                                            "textures/a.vkt") == true_v);
  assert(vkr_material_system_stream_texture(system, material,
                                            VKR_TEXTURE_SLOT_NORMAL,
                                            "textures/b.vkt") == true_v);
  assert(vkr_material_system_stream_texture(system, material,
                                            VKR_TEXTURE_SLOT_SPECULAR,
                                            "textures/c.vkt") == false_v);
  assert(system->texture_stream_count == 2u);
  assert(system->texture_stream_queued_count == 2u);
  assert(system->texture_stream_active_count == 0u);
  assert(system->texture_streams[0].state ==
         VKR_MATERIAL_TEXTURE_RESIDENCY_QUEUED);
  assert(system->texture_streams[1].state ==
         VKR_MATERIAL_TEXTURE_RESIDENCY_QUEUED);
  VkrMaterialTextureStreamStats stats =
      vkr_material_system_get_texture_stream_stats(system);
  assert(stats.stream_count == 2u);
  assert(stats.pending_count == 2u);
  assert(stats.in_flight_count == 0u);
  assert(stats.resident_count == 0u);
  assert(stats.evicted_count == 0u);

  vkr_material_system_cancel_texture_streams(system, material);
  assert(system->texture_stream_count == 0u);
  assert(system->texture_stream_active_count == 0u);
  stats = vkr_material_system_get_texture_stream_stats(system);
  assert(stats.stream_count == 0u);
  assert(stats.pending_count == 0u);
  system->texture_stream_capacity = capacity;

  printf("  test_material_texture_stream_queue_is_bounded PASSED\n");
}

static void
test_material_texture_residency_evicts_to_budget(MaterialPbrTestContext *ctx) {
  VkrMaterialSystem *system = &ctx->material_system;
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  const VkrMaterialHandle material = vkr_material_system_create_colored(
      system, "texture_eviction_material", vec4_one(), &error);
  assert(material.id != 0u);
  const VkrTextureDescription description = {
      .width = 8u,
      .height = 8u,
      .channels = 4u,
      .mip_levels = 1u,
      .array_layers = 1u,
      .type = VKR_TEXTURE_TYPE_2D,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
      .sample_count = VKR_SAMPLE_COUNT_1,
      .properties = {0},
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
  };
  VkrTextureHandle texture = VKR_TEXTURE_HANDLE_INVALID;
  assert(vkr_texture_system_create_writable(
             &ctx->texture_system, string8_lit("texture_eviction_resident"),
             &description, &texture, &error) == true_v);
  VkrTexture *resident =
      vkr_texture_system_get_by_handle(&ctx->texture_system, texture);
  assert(resident != NULL);
  resident->resident_bytes = 256u;
  VkrTextureEntry *resident_entry = vkr_hash_table_get_VkrTextureEntry(
      &ctx->texture_system.texture_map, "texture_eviction_resident");
  assert(resident_entry != NULL);
  resident_entry->auto_release = false_v;

  VkrMaterial *loaded = vkr_material_system_get_by_handle(system, material);
  assert(loaded != NULL);
  loaded->textures[VKR_TEXTURE_SLOT_EMISSION] = (VkrMaterialTexture){
      .handle = texture,
      .slot = VKR_TEXTURE_SLOT_EMISSION,
      .enabled = true_v,
  };
  assert(vkr_material_system_stream_texture(system, material,
                                            VKR_TEXTURE_SLOT_EMISSION,
                                            "textures/evicted.vkt") == true_v);
  VkrMaterialTextureStream *stream =
      &system->texture_streams[system->texture_stream_count - 1u];
  system->texture_stream_queued_count--;
  system->texture_stream_resident_count++;
  system->texture_stream_resident_bytes += resident->resident_bytes;
  stream->state = VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT;
  stream->resident_texture = texture;
  stream->resident_bytes = resident->resident_bytes;

  vkr_material_system_begin_texture_residency_frame(system);
  vkr_material_system_touch_texture_residency(system, material);
  vkr_material_system_set_texture_residency_budget(system, 0u);
  vkr_material_system_pump_texture_streams(system, 1u);
  assert(stream->state == VKR_MATERIAL_TEXTURE_RESIDENCY_EVICTED);
  assert(system->texture_stream_resident_count == 0u);
  assert(system->texture_stream_resident_bytes == 0u);
  assert(system->texture_stream_evicted_count == 1u);
  assert(system->texture_stream_evicted_total >= 1u);
  const VkrMaterialTexture evicted =
      loaded->textures[VKR_TEXTURE_SLOT_EMISSION];
  const VkrTextureHandle black =
      vkr_texture_system_get_default_emissive_handle(&ctx->texture_system);
  assert(evicted.enabled == true_v);
  assert(evicted.handle.id == black.id);
  assert(evicted.handle.generation == black.generation);

  vkr_material_system_begin_texture_residency_frame(system);
  vkr_material_system_touch_texture_residency(system, material);
  assert(stream->state == VKR_MATERIAL_TEXTURE_RESIDENCY_EVICTED);
  vkr_material_system_begin_texture_residency_frame(system);
  vkr_material_system_begin_texture_residency_frame(system);
  vkr_material_system_touch_texture_residency(system, material);
  assert(stream->state == VKR_MATERIAL_TEXTURE_RESIDENCY_QUEUED);
  vkr_material_system_cancel_texture_streams(system, material);
  vkr_material_system_set_texture_residency_budget(system, UINT64_MAX);

  printf("  test_material_texture_residency_evicts_to_budget PASSED\n");
}

static void
test_shared_texture_eviction_tracks_unique_bytes(MaterialPbrTestContext *ctx) {
  VkrMaterialSystem *system = &ctx->material_system;
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  const VkrTextureDescription description = {
      .width = 8u,
      .height = 8u,
      .channels = 4u,
      .mip_levels = 1u,
      .array_layers = 1u,
      .type = VKR_TEXTURE_TYPE_2D,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
      .sample_count = VKR_SAMPLE_COUNT_1,
      .properties = {0},
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
  };
  VkrTextureHandle texture = VKR_TEXTURE_HANDLE_INVALID;
  assert(vkr_texture_system_create_writable(
             &ctx->texture_system,
             string8_lit("texture_eviction_shared_resident"), &description,
             &texture, &error) == true_v);
  VkrTexture *resident =
      vkr_texture_system_get_by_handle(&ctx->texture_system, texture);
  assert(resident != NULL);
  resident->resident_bytes = 256u;
  VkrTextureEntry *entry = vkr_hash_table_get_VkrTextureEntry(
      &ctx->texture_system.texture_map, "texture_eviction_shared_resident");
  assert(entry != NULL);
  entry->auto_release = false_v;
  vkr_texture_system_add_ref_by_handle(&ctx->texture_system, texture);

  const VkrMaterialHandle first = vkr_material_system_create_colored(
      system, "texture_eviction_shared_first", vec4_one(), &error);
  const VkrMaterialHandle second = vkr_material_system_create_colored(
      system, "texture_eviction_shared_second", vec4_one(), &error);
  assert(first.id != 0u && second.id != 0u);
  const VkrMaterialHandle materials[] = {first, second};
  for (uint32_t i = 0u; i < ArrayCount(materials); ++i) {
    VkrMaterial *material =
        vkr_material_system_get_by_handle(system, materials[i]);
    assert(material != NULL);
    material->textures[VKR_TEXTURE_SLOT_DIFFUSE] = (VkrMaterialTexture){
        .handle = texture,
        .slot = VKR_TEXTURE_SLOT_DIFFUSE,
        .enabled = true_v,
    };
    assert(vkr_material_system_unpublish(system, materials[i]) == true_v);
    assert(vkr_material_system_publish(system, materials[i], &error) == true_v);
  }
  assert(vkr_material_system_stream_texture(system, first,
                                            VKR_TEXTURE_SLOT_DIFFUSE,
                                            "textures/shared.vkt") == true_v);
  assert(vkr_material_system_stream_texture(system, second,
                                            VKR_TEXTURE_SLOT_DIFFUSE,
                                            "textures/shared.vkt") == true_v);
  for (uint32_t i = system->texture_stream_count - 2u;
       i < system->texture_stream_count; ++i) {
    VkrMaterialTextureStream *stream = &system->texture_streams[i];
    stream->state = VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT;
    stream->resident_texture = texture;
    stream->resident_bytes = resident->resident_bytes;
  }
  system->texture_stream_queued_count -= 2u;
  system->texture_stream_resident_count += 2u;
  system->texture_stream_resident_bytes += resident->resident_bytes;

  const uint64_t evictions_before = system->texture_stream_evicted_total;
  vkr_material_system_set_texture_residency_budget(system, 0u);
  vkr_material_system_pump_texture_streams(system, 2u);
  assert(system->texture_stream_resident_count == 0u);
  assert(system->texture_stream_resident_bytes == 0u);
  assert(system->texture_stream_evicted_total == evictions_before + 2u);
  assert(entry->ref_count == 0u);
  vkr_material_system_cancel_texture_streams(system, first);
  vkr_material_system_cancel_texture_streams(system, second);
  vkr_material_system_set_texture_residency_budget(system, UINT64_MAX);
  const uint64_t failures_before = system->texture_stream_failed_total;
  for (uint32_t cycle = 0u; cycle < 64u; ++cycle) {
    vkr_texture_system_add_ref_by_handle(&ctx->texture_system, texture);
    vkr_texture_system_add_ref_by_handle(&ctx->texture_system, texture);
    for (uint32_t i = 0u; i < ArrayCount(materials); ++i) {
      VkrMaterial *material =
          vkr_material_system_get_by_handle(system, materials[i]);
      assert(material != NULL);
      material->textures[VKR_TEXTURE_SLOT_DIFFUSE] = (VkrMaterialTexture){
          .handle = texture,
          .slot = VKR_TEXTURE_SLOT_DIFFUSE,
          .enabled = true_v,
      };
      assert(vkr_material_system_unpublish(system, materials[i]) == true_v);
      assert(vkr_material_system_publish(system, materials[i], &error) ==
             true_v);
    }
    assert(vkr_material_system_stream_texture(system, first,
                                              VKR_TEXTURE_SLOT_DIFFUSE,
                                              "textures/shared.vkt") == true_v);
    assert(vkr_material_system_stream_texture(system, second,
                                              VKR_TEXTURE_SLOT_DIFFUSE,
                                              "textures/shared.vkt") == true_v);
    for (uint32_t i = system->texture_stream_count - 2u;
         i < system->texture_stream_count; ++i) {
      VkrMaterialTextureStream *stream = &system->texture_streams[i];
      stream->state = VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT;
      stream->resident_texture = texture;
      stream->resident_bytes = resident->resident_bytes;
    }
    system->texture_stream_queued_count -= 2u;
    system->texture_stream_resident_count += 2u;
    system->texture_stream_resident_bytes += resident->resident_bytes;
    vkr_material_system_set_texture_residency_budget(system, 0u);
    vkr_material_system_pump_texture_streams(system, 2u);
    assert(system->texture_stream_resident_count == 0u);
    assert(system->texture_stream_resident_bytes == 0u);
    assert(entry->ref_count == 0u);
    vkr_material_system_cancel_texture_streams(system, first);
    vkr_material_system_cancel_texture_streams(system, second);
  }
  assert(system->texture_stream_count == 0u);
  assert(system->texture_stream_evicted_count == 0u);
  assert(system->texture_stream_failed_total == failures_before);
  vkr_material_system_set_texture_residency_budget(system, UINT64_MAX);

  printf("  test_shared_texture_eviction_tracks_unique_bytes PASSED\n");
}

static void test_shared_texture_eviction_republishes_all_materials(
    MaterialPbrTestContext *ctx) {
  VkrMaterialSystem *system = &ctx->material_system;
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  const VkrTextureDescription description = {
      .width = 8u,
      .height = 8u,
      .channels = 4u,
      .mip_levels = 1u,
      .array_layers = 1u,
      .type = VKR_TEXTURE_TYPE_2D,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
      .sample_count = VKR_SAMPLE_COUNT_1,
      .properties = {0},
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
  };
  VkrTextureHandle texture = VKR_TEXTURE_HANDLE_INVALID;
  assert(vkr_texture_system_create_writable(
             &ctx->texture_system,
             string8_lit("texture_eviction_shared_materials"), &description,
             &texture, &error) == true_v);
  VkrTexture *resident =
      vkr_texture_system_get_by_handle(&ctx->texture_system, texture);
  assert(resident != NULL);
  resident->resident_bytes = 256u;
  VkrTextureEntry *entry = vkr_hash_table_get_VkrTextureEntry(
      &ctx->texture_system.texture_map, "texture_eviction_shared_materials");
  assert(entry != NULL);
  entry->auto_release = false_v;
  vkr_texture_system_add_ref_by_handle(&ctx->texture_system, texture);

  const VkrMaterialHandle first = vkr_material_system_create_colored(
      system, "texture_eviction_shared_material_first", vec4_one(), &error);
  const VkrMaterialHandle second = vkr_material_system_create_colored(
      system, "texture_eviction_shared_material_second", vec4_one(), &error);
  assert(first.id != 0u && second.id != 0u);
  const VkrMaterialHandle materials[] = {first, second};
  for (uint32_t i = 0u; i < ArrayCount(materials); ++i) {
    VkrMaterial *material =
        vkr_material_system_get_by_handle(system, materials[i]);
    assert(material != NULL);
    material->textures[VKR_TEXTURE_SLOT_NORMAL] = (VkrMaterialTexture){
        .handle = texture,
        .slot = VKR_TEXTURE_SLOT_NORMAL,
        .enabled = true_v,
    };
    assert(vkr_material_system_unpublish(system, materials[i]) == true_v);
    assert(vkr_material_system_publish(system, materials[i], &error) == true_v);
    assert(vkr_material_system_stream_texture(
               system, materials[i], VKR_TEXTURE_SLOT_NORMAL,
               "textures/shared_materials.vkt") == true_v);
    VkrMaterialTextureStream *stream =
        &system->texture_streams[system->texture_stream_count - 1u];
    system->texture_stream_queued_count--;
    system->texture_stream_resident_count++;
    stream->state = VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT;
    stream->resident_texture = texture;
    stream->resident_bytes = resident->resident_bytes;
  }
  system->texture_stream_resident_bytes += resident->resident_bytes;

  const uint32_t destroy_calls_before =
      ctx->publisher_state.texture_destroy_calls;
  const uint32_t rejection_count_before =
      ctx->publisher_state.texture_destroy_rejections;
  vkr_material_system_set_texture_residency_budget(system, 0u);
  vkr_material_system_pump_texture_streams(system, 1u);
  assert(system->texture_stream_resident_count == 0u);
  assert(system->texture_stream_resident_bytes == 0u);
  assert(system->texture_stream_evicted_count == 2u);
  assert(vkr_texture_system_get_by_handle(&ctx->texture_system, texture) !=
         NULL);
  assert(material_pbr_mock_unpublish_texture(&ctx->publisher_state, texture) ==
         true_v);
  assert(ctx->publisher_state.texture_destroy_calls ==
         destroy_calls_before + 1u);
  assert(ctx->publisher_state.texture_destroy_rejections ==
         rejection_count_before);

  for (uint32_t i = 0u; i < ArrayCount(materials); ++i) {
    const VkrMaterial *material =
        vkr_material_system_get_by_handle(system, materials[i]);
    assert(material != NULL);
    const VkrTextureHandle default_normal =
        vkr_texture_system_get_default_normal_handle(&ctx->texture_system);
    assert(material->textures[VKR_TEXTURE_SLOT_NORMAL].handle.id ==
           default_normal.id);
    assert(material->textures[VKR_TEXTURE_SLOT_NORMAL].handle.generation ==
           default_normal.generation);
    vkr_material_system_cancel_texture_streams(system, materials[i]);
  }
  vkr_material_system_set_texture_residency_budget(system, UINT64_MAX);
  printf("  test_shared_texture_eviction_republishes_all_materials PASSED\n");
}

static void
test_compressed_texture_subresource_shapes(MaterialPbrTestContext *ctx) {
  static const struct {
    const char *name;
    uint32_t layers;
    uint32_t faces;
    VkrTextureType type;
    uint32_t physical_layers;
  } cases[] = {
      {"texture_array.vkt", 3u, 1u, VKR_TEXTURE_TYPE_2D_ARRAY, 3u},
      {"texture_cube.vkt", 1u, 6u, VKR_TEXTURE_TYPE_CUBE_MAP, 6u},
      {"texture_cube_array.vkt", 2u, 6u, VKR_TEXTURE_TYPE_CUBE_MAP_ARRAY, 12u},
  };
  ctx->texture_system.supports_texture_astc_4x4 = true_v;
  for (uint32_t c = 0u; c < ArrayCount(cases); ++c) {
    char relative_path[256] = {0};
    char absolute_path[1024] = {0};
    snprintf(relative_path, sizeof(relative_path), "tests/tmp/material_pbr/%s",
             cases[c].name);
    snprintf(absolute_path, sizeof(absolute_path), "%s%s", PROJECT_SOURCE_DIR,
             relative_path);
    material_pbr_test_remove_file(relative_path);
    assert(material_pbr_test_write_uastc_texture(absolute_path, cases[c].layers,
                                                 cases[c].faces) == true_v);

    char request_path[320] = {0};
    snprintf(request_path, sizeof(request_path), "%s?cs=linear&tc=color_linear",
             relative_path);
    VkrTexturePreparedLoad prepared = {0};
    VkrRendererError error = VKR_RENDERER_ERROR_NONE;
    assert(vkr_texture_system_prepare_load_from_file(
               &ctx->texture_system,
               string8_create_from_cstr((const uint8_t *)request_path,
                                        string_length(request_path)),
               VKR_TEXTURE_RGBA_CHANNELS, &ctx->temp_allocator, &prepared,
               &error) == true_v);
    assert(prepared.description.type == cases[c].type);
    assert(prepared.description.array_layers == cases[c].physical_layers);
    assert(prepared.upload_array_layers == cases[c].physical_layers);
    assert(prepared.upload_region_count == cases[c].physical_layers);
    assert(prepared.upload_is_compressed == true_v);
    for (uint32_t layer = 0u; layer < cases[c].physical_layers; ++layer) {
      assert(prepared.upload_regions[layer].array_layer == layer);
      assert(prepared.upload_regions[layer].mip_level == 0u);
      assert(prepared.upload_regions[layer].byte_size > 0u);
    }

    VkrTextureHandle handle = VKR_TEXTURE_HANDLE_INVALID;
    assert(vkr_texture_system_finalize_prepared_load(
               &ctx->texture_system,
               string8_create_from_cstr((const uint8_t *)request_path,
                                        string_length(request_path)),
               &prepared, &handle, &error) == true_v);
    VkrTexture *texture =
        vkr_texture_system_get_by_handle(&ctx->texture_system, handle);
    assert(texture != NULL);
    assert(texture->description.type == cases[c].type);
    assert(texture->resident_bytes == prepared.upload_data_size);
    vkr_texture_system_release_prepared_load(&prepared);
    if (!getenv("VKR_KEEP_TEST_TEXTURE_FIXTURES")) {
      material_pbr_test_remove_file(relative_path);
    }
  }
  printf("  test_compressed_texture_subresource_shapes PASSED\n");
}

static void test_texture_publication_backpressure_is_retryable(
    MaterialPbrTestContext *ctx) {
  uint8_t pixels[4] = {255u, 255u, 255u, 255u};
  VkrTextureUploadRegion region = {
      .width = 1u,
      .height = 1u,
      .depth = 1u,
      .byte_size = sizeof(pixels),
  };
  VkrTexturePreparedLoad prepared = {
      .description =
          {
              .type = VKR_TEXTURE_TYPE_2D,
              .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
              .width = 1u,
              .height = 1u,
              .array_layers = 1u,
          },
      .upload_data = pixels,
      .upload_data_size = sizeof(pixels),
      .upload_regions = &region,
      .upload_region_count = 1u,
      .upload_mip_levels = 1u,
      .upload_array_layers = 1u,
  };
  String8 name = string8_lit("texture_backpressure_retry");
  VkrTextureHandle handle = VKR_TEXTURE_HANDLE_INVALID;
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  const uint32_t create_calls_before =
      ctx->publisher_state.texture_create_calls;
  const uint32_t next_free_before = ctx->texture_system.next_free_index;

  ctx->publisher_state.texture_upload_available = false_v;
  assert(vkr_texture_system_finalize_prepared_load(&ctx->texture_system, name,
                                                   &prepared, &handle,
                                                   &error) == false_v);
  assert(error == VKR_RENDERER_ERROR_RESOURCE_BUSY);
  assert(handle.id == VKR_TEXTURE_HANDLE_INVALID.id);
  assert(ctx->publisher_state.texture_create_calls == create_calls_before);
  assert(ctx->texture_system.next_free_index == next_free_before);
  assert(vkr_hash_table_get_VkrTextureEntry(&ctx->texture_system.texture_map,
                                            "texture_backpressure_retry") ==
         NULL);

  ctx->publisher_state.texture_upload_available = true_v;
  assert(vkr_texture_system_finalize_prepared_load(
             &ctx->texture_system, name, &prepared, &handle, &error) == true_v);
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(handle.id != VKR_TEXTURE_HANDLE_INVALID.id);
  assert(ctx->publisher_state.texture_create_calls == create_calls_before + 1u);

  printf("  test_texture_publication_backpressure_is_retryable PASSED\n");
}

static void test_repeated_texture_finalize_reuses_canonical_handle(
    MaterialPbrTestContext *ctx) {
  uint8_t pixels[4] = {127u, 127u, 255u, 255u};
  VkrTextureUploadRegion region = {
      .width = 1u,
      .height = 1u,
      .depth = 1u,
      .byte_size = sizeof(pixels),
  };
  VkrTexturePreparedLoad prepared = {
      .description =
          {
              .type = VKR_TEXTURE_TYPE_2D,
              .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
              .width = 1u,
              .height = 1u,
              .array_layers = 1u,
          },
      .upload_data = pixels,
      .upload_data_size = sizeof(pixels),
      .upload_regions = &region,
      .upload_region_count = 1u,
      .upload_mip_levels = 1u,
      .upload_array_layers = 1u,
  };
  String8 name = string8_lit("repeated_texture_finalize?tc=normal_rg");
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  VkrTextureHandle first = VKR_TEXTURE_HANDLE_INVALID;
  VkrTextureHandle second = VKR_TEXTURE_HANDLE_INVALID;
  const uint32_t create_calls_before =
      ctx->publisher_state.texture_create_calls;
  const uint64_t map_size_before = ctx->texture_system.texture_map.size;

  assert(vkr_texture_system_finalize_prepared_load(
             &ctx->texture_system, name, &prepared, &first, &error) == true_v);
  assert(vkr_texture_system_finalize_prepared_load(
             &ctx->texture_system, name, &prepared, &second, &error) == true_v);
  assert(second.id == first.id);
  assert(second.generation == first.generation);
  assert(ctx->publisher_state.texture_create_calls == create_calls_before + 1u);
  assert(ctx->texture_system.texture_map.size == map_size_before + 1u);
  assert(ctx->texture_system.texture_keys_by_index[first.id - 1u] != NULL);

  printf("  test_repeated_texture_finalize_reuses_canonical_handle PASSED\n");
}

bool32_t run_material_pbr_tests(void) {
  printf("--- Starting Material PBR Tests ---\n");

  material_pbr_test_ensure_dirs();

  MaterialPbrTestContext context = {0};
  assert(material_pbr_test_init_context(&context) == true_v);

  test_material_pbr_inference_from_scalar_keys(&context);
  test_material_temporal_reactivity_authoring(&context);
  test_material_transmission_is_independent_of_alpha(&context);
  test_material_pbr_alias_slots_and_inference(&context);
  test_material_alpha_mode_cutout_defaults(&context);
  test_material_double_sided_state(&context);
  test_material_legacy_cutout_compatibility(&context);
  test_material_texture_intent_query_normalization(&context);
  test_material_texture_intent_override_is_deterministic(&context);
  test_material_batch_load_honors_parsed_name_over_stem(&context);
  test_async_emissive_texture_uses_black_pending_fallback(&context);
  test_material_texture_stream_queue_is_bounded(&context);
  test_material_texture_residency_evicts_to_budget(&context);
  test_shared_texture_eviction_tracks_unique_bytes(&context);
  test_shared_texture_eviction_republishes_all_materials(&context);
  test_compressed_texture_subresource_shapes(&context);
  test_texture_publication_backpressure_is_retryable(&context);
  test_repeated_texture_finalize_reuses_canonical_handle(&context);

  material_pbr_test_shutdown_context(&context);

  printf("--- Material PBR Tests Completed ---\n");
  return true_v;
}
