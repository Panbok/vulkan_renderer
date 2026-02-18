#include "material_pbr_tests.h"

#include "containers/str.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_texture_system.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct MaterialPbrMockBackendState {
  uintptr_t next_handle_token;
  uint32_t texture_create_calls;
  uint32_t texture_batch_create_calls;
  uint32_t texture_destroy_calls;
} MaterialPbrMockBackendState;

typedef struct MaterialPbrTestContext {
  RendererFrontend renderer;
  MaterialPbrMockBackendState backend_state;
  VkrShaderSystem shader_system;
  VkrTextureSystem texture_system;
  VkrMaterialSystem material_system;
  VkrResourceLoader material_loader;
  Arena *temp_arena;
  VkrAllocator temp_allocator;
} MaterialPbrTestContext;

static VkrBackendResourceHandle
material_pbr_mock_make_handle(MaterialPbrMockBackendState *state) {
  state->next_handle_token++;
  return (VkrBackendResourceHandle){
      .ptr = (void *)((state->next_handle_token << 4u) | 1u)};
}

static void material_pbr_mock_get_device_information(
    void *backend_state, VkrDeviceInformation *device_information,
    Arena *temp_arena) {
  (void)backend_state;
  (void)temp_arena;
  assert(device_information != NULL);

  MemZero(device_information, sizeof(*device_information));
}

static VkrBackendResourceHandle material_pbr_mock_texture_create(
    void *backend_state, const VkrTextureDescription *desc,
    const void *initial_data) {
  (void)desc;
  (void)initial_data;

  MaterialPbrMockBackendState *state =
      (MaterialPbrMockBackendState *)backend_state;
  assert(state != NULL);
  state->texture_create_calls++;
  return material_pbr_mock_make_handle(state);
}

static VkrBackendResourceHandle material_pbr_mock_texture_create_with_payload(
    void *backend_state, const VkrTextureDescription *desc,
    const VkrTextureUploadPayload *payload) {
  (void)payload;
  return material_pbr_mock_texture_create(backend_state, desc, NULL);
}

static uint32_t material_pbr_mock_texture_create_with_payload_batch(
    void *backend_state, const VkrTextureBatchCreateRequest *requests,
    uint32_t count, VkrBackendResourceHandle *out_handles,
    VkrRendererError *out_errors) {
  MaterialPbrMockBackendState *state =
      (MaterialPbrMockBackendState *)backend_state;
  assert(state != NULL);
  assert(requests != NULL);
  assert(out_handles != NULL);
  assert(out_errors != NULL);

  state->texture_batch_create_calls++;

  for (uint32_t i = 0; i < count; ++i) {
    out_handles[i] = material_pbr_mock_make_handle(state);
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  return count;
}

static void material_pbr_mock_texture_destroy(void *backend_state,
                                              VkrBackendResourceHandle handle) {
  (void)handle;

  MaterialPbrMockBackendState *state =
      (MaterialPbrMockBackendState *)backend_state;
  assert(state != NULL);
  state->texture_destroy_calls++;
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

  ctx->backend_state.next_handle_token = 0x1000u;
  ctx->renderer.backend_state = &ctx->backend_state;
  ctx->renderer.backend.get_device_information =
      material_pbr_mock_get_device_information;
  ctx->renderer.backend.texture_create = material_pbr_mock_texture_create;
  ctx->renderer.backend.texture_create_with_payload =
      material_pbr_mock_texture_create_with_payload;
  ctx->renderer.backend.texture_create_with_payload_batch =
      material_pbr_mock_texture_create_with_payload_batch;
  ctx->renderer.backend.texture_destroy = material_pbr_mock_texture_destroy;
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

  VkrShaderSystemConfig shader_cfg = {
      .max_shader_count = 64,
      .max_uniform_count = 64,
      .max_global_textures = 16,
      .max_instance_textures = 16,
  };
  if (!vkr_shader_system_initialize(&ctx->shader_system, shader_cfg)) {
    material_pbr_test_shutdown_renderer(ctx);
    return false_v;
  }

  VkrTextureSystemConfig texture_cfg = {
      .max_texture_count = 256,
  };
  if (!vkr_texture_system_init(&ctx->renderer, &texture_cfg, NULL,
                               &ctx->texture_system)) {
    vkr_shader_system_shutdown(&ctx->shader_system);
    material_pbr_test_shutdown_renderer(ctx);
    return false_v;
  }

  VkrMaterialSystemConfig material_cfg = {
      .max_material_count = 128,
  };
  if (!vkr_material_system_init(&ctx->material_system, ctx->renderer.arena,
                                &ctx->texture_system, &ctx->shader_system,
                                &material_cfg)) {
    vkr_texture_system_shutdown(&ctx->renderer, &ctx->texture_system);
    vkr_shader_system_shutdown(&ctx->shader_system);
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
    vkr_texture_system_shutdown(&ctx->renderer, &ctx->texture_system);
    vkr_shader_system_shutdown(&ctx->shader_system);
    material_pbr_test_shutdown_renderer(ctx);
    return false_v;
  }

  ctx->temp_allocator = (VkrAllocator){.ctx = ctx->temp_arena};
  if (!vkr_allocator_arena(&ctx->temp_allocator)) {
    arena_destroy(ctx->temp_arena);
    ctx->temp_arena = NULL;
    vkr_material_system_shutdown(&ctx->material_system);
    vkr_texture_system_shutdown(&ctx->renderer, &ctx->texture_system);
    vkr_shader_system_shutdown(&ctx->shader_system);
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
  vkr_texture_system_shutdown(&ctx->renderer, &ctx->texture_system);
  vkr_shader_system_shutdown(&ctx->shader_system);

  if (ctx->temp_arena) {
    arena_destroy(ctx->temp_arena);
    ctx->temp_arena = NULL;
  }

  material_pbr_test_shutdown_renderer(ctx);
}

static bool8_t material_pbr_test_load_material(MaterialPbrTestContext *ctx,
                                               const char *stem,
                                               const char *content,
                                               char *out_path,
                                               size_t out_path_size,
                                               VkrResourceHandleInfo *out_info) {
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
  const bool8_t loaded =
      ctx->material_loader.load(&ctx->material_loader, path,
                                &ctx->temp_allocator, out_info, &err);
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

static void test_material_pbr_inference_from_scalar_keys(
    MaterialPbrTestContext *ctx) {
  printf("  Running test_material_pbr_inference_from_scalar_keys...\n");

  const char *material_text = "pipeline=world\n"
                              "metallic=0.25\n"
                              "roughness=0.60\n";

  char material_path[1024] = {0};
  VkrResourceHandleInfo handle_info = {0};
  assert(material_pbr_test_load_material(ctx, "pbr_scalar_inference",
                                         material_text, material_path,
                                         sizeof(material_path),
                                         &handle_info) == true_v);

  VkrMaterial *material = vkr_material_system_get_by_handle(
      &ctx->material_system, handle_info.as.material);
  assert(material != NULL);
  assert(material->material_type == VKR_MATERIAL_TYPE_PBR);
  assert(material->pipeline_id == VKR_PIPELINE_DOMAIN_WORLD);
  assert(fabsf(material->pbr.metallic - 0.25f) < 0.0001f);
  assert(fabsf(material->pbr.roughness - 0.60f) < 0.0001f);

  material_pbr_test_unload_material(ctx, &handle_info, material_path);
  material_pbr_test_remove_file(material_path);

  printf("  test_material_pbr_inference_from_scalar_keys PASSED\n");
}

static void test_material_pbr_alias_slots_and_inference(
    MaterialPbrTestContext *ctx) {
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
  assert(material_pbr_test_load_material(ctx, "pbr_alias_inference",
                                         material_text, material_path,
                                         sizeof(material_path),
                                         &handle_info) == true_v);

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

static void test_material_alpha_mode_cutout_defaults(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_alpha_mode_cutout_defaults...\n");

  const struct {
    const char *stem;
    const char *content;
  } cases[] = {
      {.stem = "pbr_alpha_cutout_default",
       .content = "alpha_mode=cutout\n"},
      {.stem = "pbr_alpha_cutout_zero",
       .content = "alpha_mode=cutout\n"
                  "alpha_cutoff=0.0\n"},
  };

  for (uint32_t i = 0; i < ArrayCount(cases); ++i) {
    char material_path[1024] = {0};
    VkrResourceHandleInfo handle_info = {0};
    assert(material_pbr_test_load_material(ctx, cases[i].stem, cases[i].content,
                                           material_path,
                                           sizeof(material_path),
                                           &handle_info) == true_v);

    VkrMaterial *material = vkr_material_system_get_by_handle(
        &ctx->material_system, handle_info.as.material);
    assert(material != NULL);
    assert(material->alpha_mode_explicit == true_v);
    assert(material->alpha_mode == VKR_MATERIAL_ALPHA_CUTOUT);
    assert(fabsf(material->alpha_cutoff - VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT) <
           0.0001f);

    material_pbr_test_unload_material(ctx, &handle_info, material_path);
    material_pbr_test_remove_file(material_path);
  }

  printf("  test_material_alpha_mode_cutout_defaults PASSED\n");
}

static void
test_material_legacy_cutout_compatibility(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_legacy_cutout_compatibility...\n");

  char material_path[1024] = {0};
  VkrResourceHandleInfo handle_info = {0};
  assert(material_pbr_test_load_material(ctx, "pbr_legacy_cutout",
                                         "cutout=true\n", material_path,
                                         sizeof(material_path),
                                         &handle_info) == true_v);

  VkrMaterial *material =
      vkr_material_system_get_by_handle(&ctx->material_system,
                                        handle_info.as.material);
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
  assert(material_pbr_test_load_material(ctx, "pbr_intent_normalization",
                                         material_text, material_path,
                                         sizeof(material_path),
                                         &handle_info) == true_v);

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

static void
test_material_texture_intent_override_is_deterministic(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_texture_intent_override_is_deterministic...\n");

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
  assert(material_pbr_test_string_contains(texture_key, "cs=linear") ==
         true_v);
  assert(material_pbr_test_string_contains(texture_key, "tc=data_mask") ==
         true_v);
  assert(material_pbr_test_string_contains(texture_key, "cs=srgb") == false_v);

  material_pbr_test_unload_material(ctx, &handle_info, material_path);
  material_pbr_test_remove_file(material_path);

  printf("  test_material_texture_intent_override_is_deterministic PASSED\n");
}

static void
test_material_batch_load_honors_parsed_name_over_stem(MaterialPbrTestContext *ctx) {
  printf("  Running test_material_batch_load_honors_parsed_name_over_stem...\n");

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

bool32_t run_material_pbr_tests(void) {
  printf("--- Starting Material PBR Tests ---\n");

  material_pbr_test_ensure_dirs();

  MaterialPbrTestContext context = {0};
  assert(material_pbr_test_init_context(&context) == true_v);

  test_material_pbr_inference_from_scalar_keys(&context);
  test_material_pbr_alias_slots_and_inference(&context);
  test_material_alpha_mode_cutout_defaults(&context);
  test_material_legacy_cutout_compatibility(&context);
  test_material_texture_intent_query_normalization(&context);
  test_material_texture_intent_override_is_deterministic(&context);
  test_material_batch_load_honors_parsed_name_over_stem(&context);

  material_pbr_test_shutdown_context(&context);

  printf("--- Material PBR Tests Completed ---\n");
  return true_v;
}
