#include "renderer/systems/vkr_scene_material_override.h"

#include "core/logger.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_types.h"

#define MATERIAL_OVERRIDE_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY

typedef struct MaterialOverrideRecord {
  VkrEntityId entity;
  /* The instance the overrides were last handed to, or none. Another
     instance on the entity, as a reloaded model gives it, takes them
     again. */
  VkrMeshInstanceHandle applied;
  /* The component's materials while their publication settles; id 0 keeps
     the asset's material. The record holds one reference on each owned
     entry. */
  VkrMaterialHandle loaded[VKR_MESH_MATERIAL_OVERRIDE_MAX];
  bool8_t owned[VKR_MESH_MATERIAL_OVERRIDE_MAX];
  /* `loaded` holds the component's current materials. */
  bool8_t loading;
  /* The component changed since its materials reached the instance. */
  bool8_t dirty;
} MaterialOverrideRecord;

struct s_VkrSceneMaterialOverrides {
  MaterialOverrideRecord *records;
  uint32_t count;
  uint32_t capacity;
};

typedef struct s_VkrSceneMaterialOverrides VkrSceneMaterialOverrides;

// =============================================================================
// Records
// =============================================================================

static VkrSceneMaterialOverrides *override_state(VkrScene *scene,
                                                 bool8_t create) {
  if (scene->material_overrides || !create) {
    return scene->material_overrides;
  }
  VkrSceneMaterialOverrides *state = vkr_allocator_alloc(
      scene->alloc, sizeof(*state), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (state) {
    MemZero(state, sizeof(*state));
  }
  scene->material_overrides = state;
  return state;
}

static int32_t override_find(const VkrSceneMaterialOverrides *state,
                             VkrEntityId entity) {
  for (uint32_t i = 0u; i < state->count; ++i) {
    if (state->records[i].entity.u64 == entity.u64) {
      return (int32_t)i;
    }
  }
  return -1;
}

/* Drops the references the record took while loading. */
static void override_release_loaded(VkrScene *scene,
                                    MaterialOverrideRecord *record) {
  for (uint32_t i = 0u; i < VKR_MESH_MATERIAL_OVERRIDE_MAX; ++i) {
    if (record->owned[i] && scene->assets) {
      vkr_material_system_release(&scene->assets->material_system,
                                  record->loaded[i]);
    }
    record->owned[i] = false_v;
    record->loaded[i] = (VkrMaterialHandle){0};
  }
  record->loading = false_v;
}

/* Clears the overrides of the instance the record applied to, when it still
   lives, and forgets the record. */
static void override_remove(VkrScene *scene, VkrSceneMaterialOverrides *state,
                            uint32_t index) {
  MaterialOverrideRecord *record = &state->records[index];
  override_release_loaded(scene, record);
  if (scene->assets && record->applied.id != 0u) {
    (void)vkr_mesh_manager_instance_set_materials(&scene->assets->mesh_manager,
                                                  record->applied, NULL, 0u);
  }
  state->records[index] = state->records[state->count - 1u];
  state->count--;
}

void vkr_scene_material_override_changed(VkrScene *scene, VkrEntityId entity) {
  if (!scene || !entity.u64) {
    return;
  }
  VkrSceneMaterialOverrides *state = override_state(scene, true_v);
  if (!state) {
    return;
  }
  const int32_t index = override_find(state, entity);
  if (index >= 0) {
    /* Materials loaded for the previous paths no longer apply. */
    override_release_loaded(scene, &state->records[index]);
    state->records[index].dirty = true_v;
    return;
  }
  if (state->count == state->capacity) {
    const uint32_t capacity = Max(16u, state->capacity * 2u);
    MaterialOverrideRecord *grown = vkr_allocator_realloc(
        scene->alloc, state->records,
        (uint64_t)state->capacity * sizeof(*state->records),
        (uint64_t)capacity * sizeof(*state->records), MATERIAL_OVERRIDE_TAG);
    if (!grown) {
      log_error("Scene material overrides: no memory for entity %llu",
                (unsigned long long)entity.u64);
      return;
    }
    state->records = grown;
    state->capacity = capacity;
  }
  state->records[state->count++] = (MaterialOverrideRecord){
      .entity = entity,
      .dirty = true_v,
  };
}

void vkr_scene_material_override_entity_destroying(VkrScene *scene,
                                                   VkrEntityId entity) {
  VkrSceneMaterialOverrides *state =
      scene ? override_state(scene, false_v) : NULL;
  if (!state) {
    return;
  }
  const int32_t index = override_find(state, entity);
  if (index >= 0) {
    override_remove(scene, state, (uint32_t)index);
  }
}

void vkr_scene_material_override_shutdown(VkrScene *scene) {
  VkrSceneMaterialOverrides *state = scene ? scene->material_overrides : NULL;
  if (!state) {
    return;
  }
  /* Instances drop their own references when the scene destroys them. */
  for (uint32_t i = 0u; i < state->count; ++i) {
    override_release_loaded(scene, &state->records[i]);
  }
  if (state->records) {
    vkr_allocator_free(scene->alloc, state->records,
                       (uint64_t)state->capacity * sizeof(*state->records),
                       MATERIAL_OVERRIDE_TAG);
  }
  vkr_allocator_free(scene->alloc, state, sizeof(*state),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  scene->material_overrides = NULL;
}

// =============================================================================
// Update
// =============================================================================

/* Loads every path `value` names. A file that does not load takes the
   default material, as a brush face's does. */
static void override_load(VkrScene *scene, MaterialOverrideRecord *record,
                          const SceneMaterialOverride *value) {
  override_release_loaded(scene, record);
  for (uint32_t i = 0u; i < VKR_MESH_MATERIAL_OVERRIDE_MAX; ++i) {
    if (value->materials[i][0] == '\0') {
      continue;
    }
    record->loaded[i] =
        vkr_scene_material_load(scene, value->materials[i], &record->owned[i]);
  }
  record->loading = true_v;
}

/* Whether every loaded material finished publishing, so the instance never
   draws a submesh with a material the backend cannot resolve yet. A
   material whose publication failed takes the default material. */
static bool8_t override_published(VkrScene *scene,
                                  MaterialOverrideRecord *record) {
  VkrMaterialSystem *materials = &scene->assets->material_system;
  for (uint32_t i = 0u; i < VKR_MESH_MATERIAL_OVERRIDE_MAX; ++i) {
    if (record->loaded[i].id == 0u) {
      continue;
    }
    const VkrPublicationState *publication =
        vkr_material_system_publication(materials, record->loaded[i]);
    if (!publication) {
      continue;
    }
    if (!vkr_publication_state_settled(publication)) {
      return false_v;
    }
    if (publication->error != VKR_RENDERER_ERROR_NONE) {
      log_warn("Scene material overrides: a material of entity %llu did not "
               "publish; submesh %u uses the default material",
               (unsigned long long)record->entity.u64, i);
      if (record->owned[i]) {
        vkr_material_system_release(materials, record->loaded[i]);
      }
      record->owned[i] = false_v;
      record->loaded[i] = materials->default_material;
    }
  }
  return true_v;
}

void vkr_scene_material_override_update(VkrScene *scene) {
  VkrSceneMaterialOverrides *state =
      scene ? override_state(scene, false_v) : NULL;
  if (!state || !scene->assets) {
    return;
  }
  VkrMeshManager *meshes = &scene->assets->mesh_manager;
  uint32_t budget = VKR_SCENE_MATERIAL_OVERRIDE_LOAD_BUDGET;
  uint32_t i = 0u;
  while (i < state->count) {
    MaterialOverrideRecord *record = &state->records[i];
    const SceneMeshRenderer *renderer = vkr_entity_get_component_if_alive_const(
        scene->world, record->entity, scene->comp_mesh_renderer);
    const VkrMeshInstanceHandle handle =
        renderer ? renderer->instance : VKR_MESH_INSTANCE_HANDLE_INVALID;
    if (!record->dirty && handle.id == record->applied.id &&
        handle.generation == record->applied.generation) {
      ++i;
      continue;
    }

    const SceneMaterialOverride *value = vkr_scene_get_typed(
        scene, record->entity, &vkr_scene_material_override_type);
    if (!value) {
      /* The component left: its overrides leave with it. */
      override_remove(scene, state, i);
      continue;
    }
    ++i;

    /* An instance still loading has no submeshes to override yet; the
       record waits for it. */
    const VkrMeshInstance *instance =
        handle.id != 0u ? vkr_mesh_manager_get_instance(meshes, handle) : NULL;
    if (!instance || instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
      record->dirty = true_v;
      continue;
    }
    if (!record->loading) {
      if (budget == 0u) {
        record->dirty = true_v;
        continue;
      }
      budget--;
      override_load(scene, record, value);
    }
    if (!override_published(scene, record)) {
      record->dirty = true_v;
      continue;
    }

    /* The instance takes its own references; the record's go. */
    if (record->applied.id != 0u &&
        (record->applied.id != handle.id ||
         record->applied.generation != handle.generation)) {
      (void)vkr_mesh_manager_instance_set_materials(meshes, record->applied,
                                                    NULL, 0u);
    }
    (void)vkr_mesh_manager_instance_set_materials(
        meshes, handle, record->loaded, VKR_MESH_MATERIAL_OVERRIDE_MAX);
    override_release_loaded(scene, record);
    record->applied = handle;
    record->dirty = false_v;
  }
}
