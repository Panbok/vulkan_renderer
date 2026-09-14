#include "renderer/systems/vkr_scene_simulation.h"

#include "renderer/systems/vkr_scene_physics.h"
#include <assert.h>
#include <math.h>
#include <string.h>

static bool8_t simulation_fail(VkrScene *scene, const char **error,
                               const char *message) {
  const uint64_t length =
      Min(strlen(message), sizeof(scene->simulation.error_storage) - 1);
  MemCopy(scene->simulation.error_storage, message, length);
  scene->simulation.error_storage[length] = 0;
  scene->simulation.error = scene->simulation.error_storage;
  scene->simulation.faulted = true_v;
  scene->physics_paused = true_v;
  if (error) {
    *error = scene->simulation.error;
  }
  return false_v;
}

bool8_t
vkr_scene_simulation_configure(VkrScene *scene,
                               const VkrSceneSimulationCallbacks *callbacks,
                               const char **error) {
  if (error) {
    *error = NULL;
  }
  if (!scene || !scene->world || !scene->physics_paused ||
      scene->simulation.active || scene->simulation.completed_ticks ||
      scene->simulation.accumulator != 0.0 || scene->simulation.faulted ||
      !vkr_scene_physics_mutations_allowed(scene)) {
    if (error) {
      *error = "Configure gameplay only at a paused, reset scene boundary";
    }
    return false_v;
  }
  scene->simulation.callbacks =
      callbacks ? *callbacks : (VkrSceneSimulationCallbacks){0};
  scene->simulation.enabled = callbacks != NULL;
  return true_v;
}

bool8_t vkr_scene_simulation_detach(VkrScene *scene, void *context) {
  if (!scene || !scene->physics_paused || scene->simulation.active ||
      !vkr_scene_physics_mutations_allowed(scene) ||
      scene->simulation.callbacks.context != context) {
    return false_v;
  }
  scene->simulation.callbacks = (VkrSceneSimulationCallbacks){0};
  scene->simulation.enabled = false_v;
  return true_v;
}

uint64_t vkr_scene_simulation_completed_ticks(const VkrScene *scene) {
  return scene ? scene->simulation.completed_ticks : 0;
}

bool8_t vkr_scene_simulation_tick(VkrScene *scene, const char **error) {
  VkrSceneSimulation *simulation = &scene->simulation;
  if (simulation->active || simulation->faulted) {
    if (error) {
      *error = "Reset faulted simulation; recursive ticks are forbidden";
    }
    return false_v;
  }
  if (scene->physics_disabled ||
      (!simulation->enabled && !vkr_scene_physics_body_count(scene))) {
    return true_v;
  }
  if (simulation->completed_ticks == UINT64_MAX) {
    return simulation_fail(scene, error, "Simulation tick counter exhausted");
  }
  const uint64_t tick = simulation->completed_ticks + 1;
  simulation->active = true_v;
  if (!vkr_entity_structural_read_begin(scene->world)) {
    simulation->active = false_v;
    return simulation_fail(scene, error,
                           "Cannot acquire simulation ECS barrier");
  }
  const char *tick_error = NULL;
  bool8_t success = true_v;
  if (simulation->callbacks.before_physics) {
    simulation->phase = VKR_SCENE_SIMULATION_BEFORE_PHYSICS;
    simulation->in_callback = true_v;
    success = simulation->callbacks.before_physics(
        scene, tick, simulation->callbacks.context);
    simulation->in_callback = false_v;
  }
  if (success) {
    simulation->phase = VKR_SCENE_SIMULATION_PHYSICS;
    success = vkr_scene_physics_tick(scene, &tick_error);
  }
  if (success && simulation->callbacks.after_physics) {
    simulation->phase = VKR_SCENE_SIMULATION_AFTER_PHYSICS;
    simulation->in_callback = true_v;
    success = simulation->callbacks.after_physics(
        scene, tick, simulation->callbacks.context);
    simulation->in_callback = false_v;
  }
  vkr_entity_structural_read_end(scene->world);
  simulation->phase = VKR_SCENE_SIMULATION_IDLE;
  simulation->active = false_v;
  if (!success) {
    /* A callback may have already consumed ammo or written state. Never retry
     * this tick after resume; native and gameplay state must both be reset. */
    return simulation_fail(
        scene, error,
        tick_error ? tick_error
        : simulation->error
            ? simulation->error
            : "Gameplay callback failed; reset before resuming");
  }
  simulation->completed_ticks = tick;
  return true_v;
}

void vkr_scene_simulation_update(VkrScene *scene, float64_t dt) {
  VkrSceneSimulation *simulation = &scene->simulation;
  if (simulation->active || simulation->faulted || scene->physics_paused ||
      scene->physics_disabled || !isfinite(dt) || dt < 0.0 ||
      (!simulation->enabled && !vkr_scene_physics_body_count(scene))) {
    return;
  }
  const float64_t elapsed = simulation->accumulator + dt;
  if (!isfinite(elapsed)) {
    simulation_fail(scene, NULL,
                    "Simulation elapsed-time accumulator overflow");
    return;
  }
  simulation->accumulator = elapsed;
  for (uint32_t i = 0; i < 8u && simulation->accumulator + 1e-12 >=
                                     VKR_SCENE_SIMULATION_FIXED_DT;
       ++i) {
    if (!vkr_scene_simulation_tick(scene, NULL)) {
      return;
    }
    simulation->accumulator =
        Max(0.0, simulation->accumulator - VKR_SCENE_SIMULATION_FIXED_DT);
  }
  if (simulation->accumulator + 1e-12 >= VKR_SCENE_SIMULATION_FIXED_DT) {
    simulation->overload_updates++;
    if (simulation->overload_updates >= 60u) {
      scene->physics_paused = true_v;
      simulation->error = "Simulation paused after 60 overloaded updates; "
                          "elapsed-time debt retained. Reset or resume.";
    }
  } else {
    simulation->overload_updates = 0;
  }
}

void vkr_scene_simulation_reset_state(VkrScene *scene) {
  VkrSceneSimulation *simulation = &scene->simulation;
  simulation->accumulator = 0;
  simulation->completed_ticks = 0;
  simulation->animation_ticks = 0;
  simulation->overload_updates = 0;
  simulation->faulted = false_v;
  simulation->error = NULL;
  if (simulation->callbacks.reset) {
    simulation->active = true_v;
    const bool8_t locked = vkr_entity_structural_read_begin(scene->world);
    assert(locked);
    if (!locked) {
      simulation->active = false_v;
      simulation_fail(scene, NULL, "Cannot acquire reset ECS barrier");
      return;
    }
    simulation->phase = VKR_SCENE_SIMULATION_RESET;
    simulation->in_callback = true_v;
    simulation->callbacks.reset(scene, simulation->callbacks.context);
    simulation->in_callback = false_v;
    simulation->phase = VKR_SCENE_SIMULATION_IDLE;
    vkr_entity_structural_read_end(scene->world);
    simulation->active = false_v;
  }
}
