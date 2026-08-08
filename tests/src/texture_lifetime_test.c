#include "texture_lifetime_test.h"

#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_texture_system.h"

#include <assert.h>
#include <stdio.h>

typedef struct TextureDestroyFixture {
  uint32_t calls;
  uint32_t publish_calls;
  uint32_t sampler_calls;
  bool8_t fail_next;
  bool8_t fail_publish;
  bool8_t fail_sampler;
  VkrTextureHandle last_handle;
  VkrTextureDescription last_description;
} TextureDestroyFixture;

static bool8_t
texture_lifetime_publish_writable(void *state, VkrTextureHandle handle,
                                  const VkrTextureDescription *description) {
  TextureDestroyFixture *fixture = state;
  fixture->publish_calls++;
  fixture->last_handle = handle;
  return description && description->id == handle.id &&
         description->generation == handle.generation && !fixture->fail_publish;
}

static bool8_t
texture_lifetime_update_sampler(void *state, VkrTextureHandle handle,
                                const VkrTextureDescription *description) {
  TextureDestroyFixture *fixture = state;
  fixture->sampler_calls++;
  fixture->last_handle = handle;
  if (description) {
    fixture->last_description = *description;
  }
  return description && description->id == handle.id &&
         description->generation == handle.generation && !fixture->fail_sampler;
}

static bool8_t texture_lifetime_unpublish(void *state,
                                          VkrTextureHandle handle) {
  TextureDestroyFixture *fixture = state;
  fixture->calls++;
  fixture->last_handle = handle;
  if (fixture->fail_next) {
    fixture->fail_next = false_v;
    return false_v;
  }
  return true_v;
}

static void test_texture_destroy_failure_preserves_retry_state(void) {
  printf("  Running test_texture_destroy_failure_preserves_retry_state...\n");

  TextureDestroyFixture fixture = {.fail_next = true_v};
  const VkrAssetPublisher publisher = {
      .state = &fixture,
      .unpublish_texture = texture_lifetime_unpublish,
  };
  VkrTextureSystem system = {.asset_publisher = &publisher};
  VkrTexture texture = {
      .description = {.id = 7u, .generation = 19u},
      .handle = (VkrTextureOpaqueHandle)(uintptr_t)0x1234u,
      .image = (uint8_t *)(uintptr_t)0x5678u,
  };
  const VkrTexture before = texture;

  assert(!vkr_texture_destroy(&system, &texture));
  assert(fixture.calls == 1u);
  assert(fixture.last_handle.id == before.description.id);
  assert(fixture.last_handle.generation == before.description.generation);
  assert(texture.description.id == before.description.id);
  assert(texture.description.generation == before.description.generation);
  assert(texture.handle == before.handle);
  assert(texture.image == before.image);

  assert(vkr_texture_destroy(&system, &texture));
  assert(fixture.calls == 2u);
  assert(texture.description.id == 0u);
  assert(texture.description.generation == 0u);
  assert(texture.handle == NULL);
  assert(texture.image == NULL);

  printf("  test_texture_destroy_failure_preserves_retry_state PASSED\n");
}

static void test_writable_publish_failure_rolls_back_cpu_reservation(void) {
  printf("  Running "
         "test_writable_publish_failure_rolls_back_cpu_reservation...\n");

  Arena *arena = arena_create(KB(64), KB(64));
  assert(arena != NULL);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrDMemory string_memory = {0};
  assert(vkr_dmemory_create(KB(64), KB(64), &string_memory));
  VkrAllocator string_allocator = {.ctx = &string_memory};
  vkr_dmemory_allocator_create(&string_allocator);

  TextureDestroyFixture fixture = {.fail_publish = true_v};
  const VkrAssetPublisher publisher = {
      .state = &fixture,
      .publish_writable_texture = texture_lifetime_publish_writable,
      .update_texture_sampler = texture_lifetime_update_sampler,
      .unpublish_texture = texture_lifetime_unpublish,
  };
  VkrTexture textures[2] = {0};
  const char *keys[2] = {0};
  for (uint32_t i = 0; i < ArrayCount(textures); ++i) {
    textures[i].description.id = VKR_INVALID_ID;
    textures[i].description.generation = VKR_INVALID_ID;
  }
  VkrTextureSystem system = {
      .allocator = allocator,
      .string_memory = string_memory,
      .string_allocator = string_allocator,
      .config = {.max_texture_count = ArrayCount(textures)},
      .asset_publisher = &publisher,
      .textures = {.data = textures, .length = ArrayCount(textures)},
      .texture_map = vkr_hash_table_create_VkrTextureEntry(
          &allocator, ArrayCount(textures)),
      .texture_keys_by_index = keys,
      .generation_counter = 1u,
  };
  const String8 name = string8_lit("writable.transaction");
  const VkrTextureDescription description = {
      .type = VKR_TEXTURE_TYPE_CUBE_MAP,
      .width = 16u,
      .height = 16u,
      .format = VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
      .sample_count = VKR_SAMPLE_COUNT_1,
  };
  VkrTextureHandle handle = VKR_TEXTURE_HANDLE_INVALID;
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;

  assert(!vkr_texture_system_create_writable(&system, name, &description,
                                             &handle, &error));
  assert(error == VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED);
  assert(fixture.publish_calls == 1u);
  assert(fixture.calls == 0u);
  assert(system.next_free_index == 0u);
  assert(system.texture_keys_by_index[0] == NULL);
  assert(textures[0].description.id == VKR_INVALID_ID);
  assert(textures[0].description.generation == VKR_INVALID_ID);
  assert(vkr_hash_table_get_VkrTextureEntry(&system.texture_map,
                                            "writable.transaction") == NULL);

  fixture.fail_publish = false_v;
  assert(vkr_texture_system_create_writable(&system, name, &description,
                                            &handle, &error));
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(handle.id == 1u);
  assert(handle.generation == 2u);
  assert(fixture.publish_calls == 2u);
  assert(system.texture_keys_by_index[0] != NULL);
  const VkrTextureDescription initial_description = textures[0].description;
  fixture.fail_sampler = true_v;
  assert(vkr_texture_system_update_sampler(
             &system, handle, VKR_FILTER_LINEAR, VKR_FILTER_LINEAR,
             VKR_MIP_FILTER_LINEAR, true_v,
             VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
             VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
             VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE) ==
         VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED);
  assert(fixture.sampler_calls == 1u);
  assert(textures[0].description.min_filter == initial_description.min_filter);
  assert(textures[0].description.mip_filter == initial_description.mip_filter);

  fixture.fail_sampler = false_v;
  assert(vkr_texture_system_update_sampler(
             &system, handle, VKR_FILTER_LINEAR, VKR_FILTER_LINEAR,
             VKR_MIP_FILTER_LINEAR, true_v,
             VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
             VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
             VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE) == VKR_RENDERER_ERROR_NONE);
  assert(fixture.sampler_calls == 2u);
  assert(fixture.last_description.min_filter == VKR_FILTER_LINEAR);
  assert(fixture.last_description.mip_filter == VKR_MIP_FILTER_LINEAR);
  assert(textures[0].description.min_filter == VKR_FILTER_LINEAR);
  assert(textures[0].description.mip_filter == VKR_MIP_FILTER_LINEAR);
  VkrTextureEntry *entry = vkr_hash_table_get_VkrTextureEntry(
      &system.texture_map, "writable.transaction");
  assert(entry != NULL);
  entry->auto_release = false_v;
  assert(vkr_texture_system_release_by_handle(&system, handle));
  assert(entry->ref_count == 0u);
  assert(vkr_texture_system_get_by_handle(&system, handle) == &textures[0]);
  assert(vkr_texture_destroy(&system, &textures[0]));

  vkr_dmemory_destroy(&string_memory);
  arena_destroy(arena);
  printf("  test_writable_publish_failure_rolls_back_cpu_reservation PASSED\n");
}

bool32_t run_texture_lifetime_tests(void) {
  printf("--- Running texture lifetime tests... ---\n");
  test_texture_destroy_failure_preserves_retry_state();
  test_writable_publish_failure_rolls_back_cpu_reservation();
  printf("--- Texture lifetime tests completed. ---\n");
  return true_v;
}
