#include "application/vkr_application_host.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"

/* Caller-owned state and callbacks; no scene, sample UI or renderer is created.
 */
typedef struct ExampleState {
  VkrApplicationHost *host;
  uint32_t frames;
  uint32_t initializations;
  uint32_t shutdowns;
} ExampleState;

static bool8_t example_event(void *state, Event *event) {
  ExampleState *example = state;
  if (event->type == EVENT_TYPE_APPLICATION_INIT) {
    ++example->initializations;
    vkr_application_host_stop(example->host);
  } else if (event->type == EVENT_TYPE_APPLICATION_SHUTDOWN) {
    ++example->shutdowns;
  }
  return true_v;
}

static bool8_t example_frame(void *state,
                             const VkrApplicationHostFrame *frame) {
  ExampleState *example = state;
  printf("Frame %u: %.6f seconds\n", ++example->frames, frame->delta_seconds);
  return example->frames < 3u;
}

int main(void) {
  VkrDMemory memory = {0};
  if (!vkr_dmemory_create(MB(1), MB(8), &memory))
    return 1;
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  VkrApplicationHost host = {0};
  const VkrApplicationHostConfig config = {
      .title = "Custom host",
      .width = 64u,
      .height = 64u,
      .fixed_delta_seconds = 1.0 / 60.0,
  };
  int result = 1;
  if (vkr_application_host_create(&host, &config, &allocator)) {
    ExampleState state = {.host = &host};
    const VkrApplicationHostCallbacks callbacks = {.state = &state,
                                                   .frame = example_frame,
                                                   .event = example_event};
    vkr_application_host_set_callbacks(&host, &callbacks);
    vkr_application_host_run(&host);
    const bool8_t suspended_before_frame = state.frames == 0u;
    vkr_application_host_resume(&host);
    vkr_application_host_run(&host);
    vkr_application_host_shutdown(&host);
    vkr_application_host_destroy(&host);
    result = suspended_before_frame && state.frames == 3u &&
                     state.initializations == 1u && state.shutdowns == 1u
                 ? 0 : 1;
  }
  vkr_allocator_release_global_accounting(&allocator);
  vkr_dmemory_destroy(&memory);
  return result;
}
