/* The shape the editor's new-module template (editor_scripts.c) writes for a
 * lowercase name: the module, its component and its behavior share one
 * identifier. VKR_MODULE's hook wrappers once took the same names as
 * VKR_BEHAVIOR's, so this failed to compile with conflicting types for
 * `door_hook_start`. Compiled only, against the staged script SDK. */
#include "sdk.h"

#define DOOR_FIELDS VKR_FIELD(F32, speed, "Speed", 1.0f)
VKR_COMPONENT(door, door, "Door", DOOR_FIELDS)

static void door_start(VkrCtx *ctx, VkrEntity self, door *state) {
  (void)ctx;
  (void)self;
  state->speed = 1.0f;
}

VKR_BEHAVIOR(door, .start = door_start)

typedef struct DoorData {
  uint32_t starts;
} DoorData;

static void door_module_start(VkrCtx *ctx, DoorData *data) {
  (void)ctx;
  data->starts++;
}

VKR_MODULE(door, DoorData, VKR_EXPORT_BEHAVIOR(door),
           .start = door_module_start)
