#include "debug_draw_test.h"

#include "containers/str.h"
#include "memory/vkr_heap.h"
#include <assert.h>
#include <stdio.h>

/* A draw list is 330 KB; the tests keep it off the stack. */
static VkrDebugDraw *debug_draw_test_list(void) {
  VkrDebugDraw *draw = vkr_heap_alloc(sizeof(*draw));
  assert(draw);
  MemZero(draw, sizeof(*draw));
  return draw;
}

static void test_debug_draw_clear_keeps_other_groups(void) {
  printf("  Running test_debug_draw_clear_keeps_other_groups...\n");
  VkrDebugDraw *draw = debug_draw_test_list();
  const uint32_t probes = vkr_debug_draw_group("probes");
  const uint32_t paths = vkr_debug_draw_group("paths");
  assert(probes != paths);
  assert(vkr_debug_draw_group(NULL) == 0u && vkr_debug_draw_group("") == 0u);
  const VkrDebugDrawStyle probe = {.group = probes,
                                   .color = VKR_DEBUG_DRAW_RED};
  const VkrDebugDrawStyle path = {.group = paths,
                                  .color = VKR_DEBUG_DRAW_GREEN};
  assert(vkr_debug_draw_box(draw, vec3_zero(), vec3_one(), probe) == 12u);
  assert(vkr_debug_draw_line(draw, vec3_zero(), vec3_new(1, 2, 3), path) == 1u);
  assert(vkr_debug_draw_sphere(draw, vec3_zero(), 2.0f, probe) ==
         3u * VKR_DEBUG_DRAW_CIRCLE_SEGMENTS);
  assert(vkr_debug_draw_label(draw, vec3_one(), "door", path) == 1u);
  assert(vkr_debug_draw_label(draw, vec3_zero(), "probe 4", probe) == 1u);

  assert(vkr_debug_draw_clear(draw, probes, false_v) ==
         12u + 3u * VKR_DEBUG_DRAW_CIRCLE_SEGMENTS + 1u);
  assert(draw->segment_count == 1u && draw->label_count == 1u);
  assert(draw->segments[0].style.group == paths);
  assert(draw->segments[0].to.z == 3.0f);
  assert(string_equals(draw->labels[0].text, "door"));
  assert(vkr_debug_draw_clear(draw, 0u, true_v) == 2u);
  assert(draw->segment_count == 0u && draw->label_count == 0u);
  vkr_heap_free(draw);
}

static void test_debug_draw_expire_keeps_lasting_shapes(void) {
  printf("  Running test_debug_draw_expire_keeps_lasting_shapes...\n");
  VkrDebugDraw *draw = debug_draw_test_list();
  const VkrDebugDrawStyle lasting = {.color = VKR_DEBUG_DRAW_WHITE};
  const VkrDebugDrawStyle soon = {.expires = 10.0};
  const VkrDebugDrawStyle later = {.expires = 20.0};
  assert(vkr_debug_draw_line(draw, vec3_zero(), vec3_one(), soon) == 1u);
  assert(vkr_debug_draw_line(draw, vec3_zero(), vec3_new(1, 0, 0), lasting));
  assert(vkr_debug_draw_line(draw, vec3_zero(), vec3_new(0, 1, 0), later));
  assert(vkr_debug_draw_label(draw, vec3_zero(), "soon", soon) == 1u);
  const uint64_t revision = draw->revision;

  vkr_debug_draw_expire(draw, 9.0);
  assert(draw->segment_count == 3u && draw->revision == revision);
  /* Expiry is inclusive and keeps the survivors in order. */
  vkr_debug_draw_expire(draw, 10.0);
  assert(draw->segment_count == 2u && draw->label_count == 0u);
  assert(draw->segments[0].to.x == 1.0f && draw->segments[1].to.y == 1.0f);
  assert(draw->revision > revision);
  vkr_debug_draw_expire(draw, 1.0e9);
  assert(draw->segment_count == 1u);
  assert(draw->segments[0].style.expires == 0.0);
  vkr_heap_free(draw);
}

static void test_debug_draw_refuses_bad_and_overflowing_shapes(void) {
  printf("  Running test_debug_draw_refuses_bad_and_overflowing_shapes...\n");
  VkrDebugDraw *draw = debug_draw_test_list();
  const VkrDebugDrawStyle style = {0};
  assert(vkr_debug_draw_line(draw, vec3_new(VKR_FLOAT_INFINITY, 0, 0),
                             vec3_zero(), style) == 0u);
  assert(vkr_debug_draw_sphere(draw, vec3_zero(), 0.0f, style) == 0u);
  assert(vkr_debug_draw_label(draw, vec3_zero(), NULL, style) == 0u);
  assert(draw->segment_count == 0u && draw->dropped == 0u);
  for (uint32_t i = 0u; i < VKR_DEBUG_DRAW_SEGMENT_MAX - 5u; ++i) {
    (void)vkr_debug_draw_line(draw, vec3_zero(), vec3_one(), style);
  }
  /* A box that no longer fits stores what does and counts the rest. */
  assert(vkr_debug_draw_box(draw, vec3_zero(), vec3_one(), style) == 5u);
  assert(draw->segment_count == VKR_DEBUG_DRAW_SEGMENT_MAX);
  assert(draw->dropped == 7u);
  char text[96];
  MemSet(text, 'a', sizeof(text) - 1u);
  text[sizeof(text) - 1u] = '\0';
  assert(vkr_debug_draw_label(draw, vec3_zero(), text, style) == 1u);
  assert(string_length(draw->labels[0].text) == VKR_DEBUG_DRAW_LABEL_TEXT - 1u);
  (void)vkr_debug_draw_clear(draw, 0u, true_v);
  assert(draw->dropped == 0u);
  VkrDebugDrawColor color = VKR_DEBUG_DRAW_RED;
  assert(vkr_debug_draw_color_from_name("orange", &color) &&
         color == VKR_DEBUG_DRAW_ORANGE);
  assert(!vkr_debug_draw_color_from_name("purple", &color));
  vkr_heap_free(draw);
}

bool32_t run_debug_draw_tests(void) {
  printf("--- Running Debug draw tests... ---\n");
  test_debug_draw_clear_keeps_other_groups();
  test_debug_draw_expire_keeps_lasting_shapes();
  test_debug_draw_refuses_bad_and_overflowing_shapes();
  printf("--- Debug draw tests completed. ---\n");
  return true_v;
}
