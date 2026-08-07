#include "texture_lifetime_test.h"

#include "renderer/systems/vkr_texture_system.h"

#include <assert.h>
#include <stdio.h>

typedef struct TextureDestroyFixture {
  uint32_t calls;
  bool8_t fail_next;
  VkrTextureHandle last_handle;
} TextureDestroyFixture;

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

bool32_t run_texture_lifetime_tests(void) {
  printf("--- Running texture lifetime tests... ---\n");
  test_texture_destroy_failure_preserves_retry_state();
  printf("--- Texture lifetime tests completed. ---\n");
  return true_v;
}
