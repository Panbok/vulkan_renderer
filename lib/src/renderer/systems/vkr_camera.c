#include "vkr_camera.h"
#include "memory/vkr_arena_allocator.h"

vkr_internal Mat4 vkr_camera_calculate_view(const VkrCamera *camera) {
  return mat4_look_at(camera->position,
                      vec3_add(camera->position, camera->forward), camera->up);
}

vkr_internal Mat4 vkr_camera_calculate_projection(const VkrCamera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->type != VKR_CAMERA_TYPE_NONE, "Camera type is NONE");

  if (camera->type == VKR_CAMERA_TYPE_PERSPECTIVE) {
    uint32_t width = camera->cached_window_width;
    uint32_t height = camera->cached_window_height;
    if (width == 0 || height == 0) {
      VkrWindowPixelSize window_size =
          vkr_window_get_pixel_size(camera->window);
      width = window_size.width;
      height = window_size.height;
    }
    assert_log(width > 0 && height > 0, "Camera size invalid");

    if (width == 0 || height == 0) {
      return mat4_identity();
    }
    float32_t aspect = (float32_t)width / (float32_t)height;
    return mat4_perspective(vkr_to_radians(camera->zoom), aspect,
                            camera->near_clip, camera->far_clip);
  }

  if (camera->type == VKR_CAMERA_TYPE_ORTHOGRAPHIC) {
    return mat4_ortho(camera->left_clip, camera->right_clip,
                      camera->bottom_clip, camera->top_clip, camera->near_clip,
                      camera->far_clip);
  }

  assert_log(false, "Unhandled camera type");
  return mat4_identity();
}

vkr_internal void vkr_camera_update_orientation(VkrCamera *camera) {
  Vec3 front = vec3_new(vkr_cos_f32(vkr_to_radians(camera->yaw)) *
                            vkr_cos_f32(vkr_to_radians(camera->pitch)),
                        vkr_sin_f32(vkr_to_radians(camera->pitch)),
                        vkr_sin_f32(vkr_to_radians(camera->yaw)) *
                            vkr_cos_f32(vkr_to_radians(camera->pitch)));
  camera->forward = vec3_normalize(front);
  camera->right = vec3_normalize(vec3_cross(camera->forward, camera->world_up));
  camera->up = vec3_normalize(vec3_cross(camera->right, camera->forward));
  camera->view_dirty = true_v;
}

vkr_internal float32_t vkr_camera_clamp_zoom(float32_t zoom) {
  return vkr_clamp_f32(zoom, VKR_MIN_CAMERA_ZOOM, VKR_MAX_CAMERA_ZOOM);
}

void vkr_camera_system_perspective_create(VkrCamera *camera, VkrWindow *window,
                                          float32_t zoom, float32_t near_clip,
                                          float32_t far_clip) {
  assert_log(window != NULL, "Window is NULL");

  camera->window = window;

  camera->type = VKR_CAMERA_TYPE_PERSPECTIVE;
  camera->generation = VKR_INVALID_ID;

  camera->speed = VKR_DEFAULT_CAMERA_SPEED;
  camera->sensitivity = VKR_DEFAULT_CAMERA_SENSITIVITY;
  camera->yaw = VKR_DEFAULT_CAMERA_YAW;
  camera->pitch = VKR_DEFAULT_CAMERA_PITCH;

  camera->position = VKR_DEFAULT_CAMERA_POSITION;
  camera->forward = VKR_DEFAULT_CAMERA_FORWARD;
  camera->up = VKR_DEFAULT_CAMERA_UP;
  camera->right = VKR_DEFAULT_CAMERA_RIGHT;
  camera->world_up = VKR_DEFAULT_CAMERA_WORLD_UP;

  camera->near_clip = near_clip;
  camera->far_clip = far_clip;
  camera->zoom = zoom;

  camera->view_dirty = true_v;
  camera->projection_dirty = true_v;

  vkr_camera_update_orientation(camera);

  VkrWindowPixelSize window_size = vkr_window_get_pixel_size(window);
  camera->cached_window_width = window_size.width;
  camera->cached_window_height = window_size.height;
  vkr_camera_system_update(camera);
}

void vkr_camera_system_orthographic_create(VkrCamera *camera, VkrWindow *window,
                                           float32_t left, float32_t right,
                                           float32_t bottom, float32_t top,
                                           float32_t near_clip,
                                           float32_t far_clip) {
  assert_log(window != NULL, "Window is NULL");

  camera->window = window;

  camera->type = VKR_CAMERA_TYPE_ORTHOGRAPHIC;
  camera->generation = VKR_INVALID_ID;
  camera->zoom = VKR_DEFAULT_CAMERA_ZOOM;

  camera->speed = VKR_DEFAULT_CAMERA_SPEED;
  camera->sensitivity = VKR_DEFAULT_CAMERA_SENSITIVITY;
  camera->yaw = VKR_DEFAULT_CAMERA_YAW;
  camera->pitch = VKR_DEFAULT_CAMERA_PITCH;

  camera->position = VKR_DEFAULT_CAMERA_POSITION;
  camera->forward = VKR_DEFAULT_CAMERA_FORWARD;
  camera->up = VKR_DEFAULT_CAMERA_UP;
  camera->right = VKR_DEFAULT_CAMERA_RIGHT;
  camera->world_up = VKR_DEFAULT_CAMERA_WORLD_UP;

  camera->near_clip = near_clip;
  camera->far_clip = far_clip;
  camera->left_clip = left;
  camera->right_clip = right;
  camera->bottom_clip = bottom;
  camera->top_clip = top;

  camera->view_dirty = true_v;
  camera->projection_dirty = true_v;

  vkr_camera_update_orientation(camera);

  VkrWindowPixelSize window_size = vkr_window_get_pixel_size(window);
  camera->cached_window_width = window_size.width;
  camera->cached_window_height = window_size.height;

  vkr_camera_system_update(camera);
}

void vkr_camera_system_update(VkrCamera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->window != NULL, "Camera window is NULL");
  assert_log(camera->type != VKR_CAMERA_TYPE_NONE, "Camera type is NONE");

  if (camera->view_dirty) {
    camera->view = vkr_camera_calculate_view(camera);
    camera->view_dirty = false_v;
  }

  if (camera->projection_dirty) {
    camera->projection = vkr_camera_calculate_projection(camera);
    camera->projection_dirty = false_v;
  }
}

void vkr_camera_translate(VkrCamera *camera, Vec3 delta) {
  assert_log(camera != NULL, "Camera is NULL");
  camera->position = vec3_add(camera->position, delta);
  camera->view_dirty = true_v;
}

void vkr_camera_rotate(VkrCamera *camera, float32_t yaw_delta,
                       float32_t pitch_delta) {
  assert_log(camera != NULL, "Camera is NULL");

  camera->yaw += yaw_delta;
  camera->pitch = vkr_clamp_f32(camera->pitch + pitch_delta,
                                VKR_MIN_CAMERA_PITCH, VKR_MAX_CAMERA_PITCH);

  vkr_camera_update_orientation(camera);
}

void vkr_camera_zoom(VkrCamera *camera, float32_t zoom_delta) {
  assert_log(camera != NULL, "Camera is NULL");
  camera->zoom = vkr_camera_clamp_zoom(camera->zoom + zoom_delta);
  camera->projection_dirty = true_v;
}

Mat4 vkr_camera_system_get_view_matrix(const VkrCamera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->type != VKR_CAMERA_TYPE_NONE, "Camera type is NONE");

  assert(camera->view_dirty == false_v && "View matrix requested while dirty");
  return camera->view;
}

Mat4 vkr_camera_system_get_projection_matrix(const VkrCamera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->type != VKR_CAMERA_TYPE_NONE, "Camera type is NONE");

  assert(camera->projection_dirty == false_v &&
         "Projection matrix requested while dirty");
  return camera->projection;
}

vkr_internal VkrCameraHandle
vkr_camera_registry_make_handle(uint32_t slot, uint32_t generation) {
  return (VkrCameraHandle){.id = slot + 1, .generation = generation};
}

vkr_internal bool8_t vkr_camera_registry_handle_equals(VkrCameraHandle a,
                                                       VkrCameraHandle b) {
  return a.id == b.id && a.generation == b.generation;
}

vkr_internal void vkr_camera_registry_reset_camera(VkrCamera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  MemZero(camera, sizeof(*camera));
  camera->type = VKR_CAMERA_TYPE_NONE;
  camera->generation = VKR_INVALID_ID;
}

vkr_internal uint32_t
vkr_camera_registry_find_free_slot(VkrCameraSystem *system) {
  assert_log(system != NULL, "System is NULL");

  for (uint32_t i = system->next_free_index; i < system->cameras.length; i++) {
    VkrCamera *camera = &system->cameras.data[i];
    if (camera->generation == VKR_INVALID_ID ||
        camera->type == VKR_CAMERA_TYPE_NONE) {
      system->next_free_index = i + 1;
      return i;
    }
  }

  for (uint32_t i = 0; i < system->next_free_index; i++) {
    VkrCamera *camera = &system->cameras.data[i];
    if (camera->generation == VKR_INVALID_ID ||
        camera->type == VKR_CAMERA_TYPE_NONE) {
      system->next_free_index = i + 1;
      return i;
    }
  }

  return VKR_INVALID_ID;
}

vkr_internal const char *vkr_camera_registry_store_name(VkrCameraSystem *system,
                                                        String8 name) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  char *key = (char *)vkr_allocator_alloc(&system->allocator, name.length + 1,
                                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!key) {
    return NULL;
  }
  MemCopy(key, name.str, (size_t)name.length);
  key[name.length] = '\0';
  return key;
}

vkr_internal VkrCameraHandle
vkr_camera_registry_find_first_valid(VkrCameraSystem *system) {
  assert_log(system != NULL, "System is NULL");
  for (uint32_t i = 0; i < system->cameras.length; i++) {
    VkrCamera *camera = &system->cameras.data[i];
    if (camera->generation != VKR_INVALID_ID &&
        camera->type != VKR_CAMERA_TYPE_NONE) {
      return vkr_camera_registry_make_handle(i, camera->generation);
    }
  }
  return VKR_CAMERA_HANDLE_INVALID;
}

vkr_internal void vkr_camera_registry_destroy_slot(VkrCameraSystem *system,
                                                   uint32_t index) {
  assert_log(system != NULL, "System is NULL");
  assert_log(index < system->cameras.length, "Index out of bounds");

  VkrCamera *camera = &system->cameras.data[index];
  VkrCameraHandle handle =
      vkr_camera_registry_make_handle(index, camera->generation);
  vkr_camera_registry_reset_camera(camera);

  if (vkr_camera_registry_handle_equals(system->default_camera, handle)) {
    system->default_camera = VKR_CAMERA_HANDLE_INVALID;
  }
  if (vkr_camera_registry_handle_equals(system->active_camera, handle)) {
    system->active_camera = VKR_CAMERA_HANDLE_INVALID;
  }

  if (system->default_camera.id == 0) {
    system->default_camera = vkr_camera_registry_find_first_valid(system);
  }
  if (system->active_camera.id == 0) {
    system->active_camera = system->default_camera;
  }

  if (index < system->next_free_index) {
    system->next_free_index = index;
  }
}

bool8_t vkr_camera_registry_init(const VkrCameraSystemConfig *config,
                                 VkrCameraSystem *out_system) {
  assert_log(config != NULL, "Config is NULL");
  assert_log(out_system != NULL, "Out system is NULL");
  assert_log(config->max_camera_count > 0,
             "Max camera count must be greater than 0");

  MemZero(out_system, sizeof(*out_system));

  ArenaFlags arena_flags = bitset8_create();
  bitset8_set(&arena_flags, ARENA_FLAG_LARGE_PAGES);

  uint64_t arena_reserve = config->arena_reserve
                               ? config->arena_reserve
                               : VKR_CAMERA_SYSTEM_DEFAULT_ARENA_RSV;
  uint64_t arena_commit = config->arena_commit
                              ? config->arena_commit
                              : VKR_CAMERA_SYSTEM_DEFAULT_ARENA_CMT;

  out_system->arena = arena_create(arena_reserve, arena_commit, arena_flags);
  if (!out_system->arena) {
    log_fatal("Failed to create camera system arena");
    return false_v;
  }

  out_system->allocator = (VkrAllocator){.ctx = out_system->arena};
  vkr_allocator_arena(&out_system->allocator);

  out_system->cameras =
      array_create_VkrCamera(&out_system->allocator, config->max_camera_count);
  out_system->camera_map = vkr_hash_table_create_VkrCameraEntry(
      &out_system->allocator, ((uint64_t)config->max_camera_count) * 2ULL);

  out_system->next_free_index = 0;
  out_system->generation_counter = 1;
  out_system->default_camera = VKR_CAMERA_HANDLE_INVALID;
  out_system->active_camera = VKR_CAMERA_HANDLE_INVALID;

  for (uint32_t i = 0; i < out_system->cameras.length; i++) {
    vkr_camera_registry_reset_camera(&out_system->cameras.data[i]);
  }

  return true_v;
}

void vkr_camera_registry_shutdown(VkrCameraSystem *system) {
  if (!system)
    return;

  if (system->arena) {
    arena_destroy(system->arena);
  }
  MemZero(system, sizeof(*system));
}

bool8_t vkr_camera_registry_create_perspective(
    VkrCameraSystem *system, String8 name, VkrWindow *window, float32_t zoom,
    float32_t near_clip, float32_t far_clip, VkrCameraHandle *out_handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(window != NULL, "Window is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");

  *out_handle = VKR_CAMERA_HANDLE_INVALID;

  if (vkr_hash_table_contains_VkrCameraEntry(&system->camera_map,
                                             (const char *)name.str)) {
    log_error("Camera '%s' already exists", string8_cstr(&name));
    return false_v;
  }

  uint32_t slot = vkr_camera_registry_find_free_slot(system);
  if (slot == VKR_INVALID_ID) {
    log_error("Camera registry is full (max=%u)", system->cameras.length);
    return false_v;
  }

  const char *key = vkr_camera_registry_store_name(system, name);
  if (!key) {
    log_error("Failed to store camera name");
    return false_v;
  }

  VkrCamera *camera = &system->cameras.data[slot];
  vkr_camera_registry_reset_camera(camera);
  vkr_camera_system_perspective_create(camera, window, zoom, near_clip,
                                       far_clip);
  camera->generation = system->generation_counter++;

  VkrCameraEntry entry = {
      .index = slot, .ref_count = 0, .auto_release = false_v};
  if (!vkr_hash_table_insert_VkrCameraEntry(&system->camera_map, key, entry)) {
    log_error("Failed to insert camera '%s' into registry", key);
    vkr_camera_registry_reset_camera(camera);
    return false_v;
  }

  *out_handle = vkr_camera_registry_make_handle(slot, camera->generation);
  if (system->default_camera.id == 0) {
    system->default_camera = *out_handle;
    system->active_camera = *out_handle;
  }

  return true_v;
}

bool8_t vkr_camera_registry_create_orthographic(
    VkrCameraSystem *system, String8 name, VkrWindow *window, float32_t left,
    float32_t right, float32_t bottom, float32_t top, float32_t near_clip,
    float32_t far_clip, VkrCameraHandle *out_handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(window != NULL, "Window is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");

  *out_handle = VKR_CAMERA_HANDLE_INVALID;

  if (vkr_hash_table_contains_VkrCameraEntry(&system->camera_map,
                                             (const char *)name.str)) {
    log_error("Camera '%s' already exists", string8_cstr(&name));
    return false_v;
  }

  uint32_t slot = vkr_camera_registry_find_free_slot(system);
  if (slot == VKR_INVALID_ID) {
    log_error("Camera registry is full (max=%u)", system->cameras.length);
    return false_v;
  }

  const char *key = vkr_camera_registry_store_name(system, name);
  if (!key) {
    log_error("Failed to store camera name");
    return false_v;
  }

  VkrCamera *camera = &system->cameras.data[slot];
  vkr_camera_registry_reset_camera(camera);
  vkr_camera_system_orthographic_create(camera, window, left, right, bottom,
                                        top, near_clip, far_clip);
  camera->generation = system->generation_counter++;

  VkrCameraEntry entry = {
      .index = slot, .ref_count = 0, .auto_release = false_v};
  if (!vkr_hash_table_insert_VkrCameraEntry(&system->camera_map, key, entry)) {
    log_error("Failed to insert camera '%s' into registry", key);
    vkr_camera_registry_reset_camera(camera);
    return false_v;
  }

  *out_handle = vkr_camera_registry_make_handle(slot, camera->generation);
  if (system->default_camera.id == 0) {
    system->default_camera = *out_handle;
    system->active_camera = *out_handle;
  }

  return true_v;
}

VkrCameraHandle vkr_camera_registry_acquire(VkrCameraSystem *system,
                                            String8 name, bool8_t auto_release,
                                            bool8_t *out_ok) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  if (out_ok) {
    *out_ok = false_v;
  }

  VkrCameraEntry *entry = vkr_hash_table_get_VkrCameraEntry(
      &system->camera_map, (const char *)name.str);
  if (!entry) {
    log_warn("Attempted to acquire unknown camera '%s'", string8_cstr(&name));
    return VKR_CAMERA_HANDLE_INVALID;
  }

  VkrCamera *camera = &system->cameras.data[entry->index];
  if (camera->generation == VKR_INVALID_ID ||
      camera->type == VKR_CAMERA_TYPE_NONE) {
    log_warn("Camera '%s' is not loaded", string8_cstr(&name));
    return VKR_CAMERA_HANDLE_INVALID;
  }

  if (entry->ref_count == 0) {
    entry->auto_release = auto_release;
  }
  entry->ref_count++;

  if (out_ok) {
    *out_ok = true_v;
  }

  return vkr_camera_registry_make_handle(entry->index, camera->generation);
}

void vkr_camera_registry_release(VkrCameraSystem *system, String8 name) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  VkrCameraEntry *entry = vkr_hash_table_get_VkrCameraEntry(
      &system->camera_map, (const char *)name.str);
  if (!entry) {
    log_warn("Attempted to release unknown camera '%s'", string8_cstr(&name));
    return;
  }

  if (entry->ref_count == 0) {
    log_warn("Over-release detected for camera '%s'", string8_cstr(&name));
    return;
  }

  entry->ref_count--;
  if (entry->ref_count == 0 && entry->auto_release) {
    uint32_t slot_index = entry->index;
    vkr_hash_table_remove_VkrCameraEntry(&system->camera_map,
                                         (const char *)name.str);
    vkr_camera_registry_destroy_slot(system, slot_index);
  }
}

void vkr_camera_registry_release_by_handle(VkrCameraSystem *system,
                                           VkrCameraHandle handle) {
  assert_log(system != NULL, "System is NULL");

  VkrCamera *camera = vkr_camera_registry_get_by_handle(system, handle);
  if (!camera) {
    log_warn(
        "Attempted to release unknown camera handle (id=%u, generation=%u)",
        handle.id, handle.generation);
    return;
  }

  uint32_t slot = handle.id - 1;
  for (uint64_t i = 0; i < system->camera_map.capacity; i++) {
    VkrHashEntry_VkrCameraEntry *entry = &system->camera_map.entries[i];
    if (entry->occupied == VKR_OCCUPIED && entry->value.index == slot) {
      if (system->cameras.data[slot].generation != handle.generation) {
        continue;
      }
      uint64_t name_length = string_length(entry->key);
      String8 camera_name =
          string8_create_from_cstr((const uint8_t *)entry->key, name_length);
      vkr_camera_registry_release(system, camera_name);
      return;
    }
  }

  log_warn("Attempted to release unknown camera handle (id=%u, generation=%u)",
           handle.id, handle.generation);
}

void vkr_camera_registry_update(VkrCameraSystem *system, VkrCameraHandle h) {
  assert_log(system != NULL, "System is NULL");
  VkrCamera *camera = vkr_camera_registry_get_by_handle(system, h);
  if (!camera) {
    log_warn("Camera handle invalid for update (id=%u, generation=%u)", h.id,
             h.generation);
    return;
  }
  vkr_camera_system_update(camera);
}

void vkr_camera_registry_update_all(VkrCameraSystem *system) {
  assert_log(system != NULL, "System is NULL");
  for (uint32_t i = 0; i < system->cameras.length; i++) {
    VkrCamera *camera = &system->cameras.data[i];
    if (camera->generation == VKR_INVALID_ID ||
        camera->type == VKR_CAMERA_TYPE_NONE || camera->window == NULL) {
      continue;
    }
    vkr_camera_system_update(camera);
  }
}

Mat4 vkr_camera_registry_get_view(VkrCameraSystem *system, VkrCameraHandle h) {
  assert_log(system != NULL, "System is NULL");
  VkrCamera *camera = vkr_camera_registry_get_by_handle(system, h);
  if (!camera) {
    log_error("Camera handle invalid for view (id=%u, generation=%u)", h.id,
              h.generation);
    return mat4_identity();
  }
  return vkr_camera_system_get_view_matrix(camera);
}

Mat4 vkr_camera_registry_get_projection(VkrCameraSystem *system,
                                        VkrCameraHandle h) {
  assert_log(system != NULL, "System is NULL");
  VkrCamera *camera = vkr_camera_registry_get_by_handle(system, h);
  if (!camera) {
    log_error("Camera handle invalid for projection (id=%u, generation=%u)",
              h.id, h.generation);
    return mat4_identity();
  }
  return vkr_camera_system_get_projection_matrix(camera);
}

void vkr_camera_registry_set_active(VkrCameraSystem *system,
                                    VkrCameraHandle h) {
  assert_log(system != NULL, "System is NULL");
  if (vkr_camera_registry_get_by_handle(system, h) == NULL) {
    log_warn("Attempted to set invalid active camera (id=%u, generation=%u)",
             h.id, h.generation);
    return;
  }
  system->active_camera = h;
}

VkrCameraHandle vkr_camera_registry_get_active(VkrCameraSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return system->active_camera;
}

Mat4 vkr_camera_registry_get_active_view(VkrCameraSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return vkr_camera_registry_get_view(system, system->active_camera);
}

Mat4 vkr_camera_registry_get_active_projection(VkrCameraSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return vkr_camera_registry_get_projection(system, system->active_camera);
}

void vkr_camera_registry_resize_all(VkrCameraSystem *system, uint32_t width,
                                    uint32_t height) {
  assert_log(system != NULL, "System is NULL");

  for (uint32_t i = 0; i < system->cameras.length; i++) {
    VkrCamera *camera = &system->cameras.data[i];
    if (camera->generation == VKR_INVALID_ID ||
        camera->type == VKR_CAMERA_TYPE_NONE) {
      continue;
    }
    if (camera->cached_window_width != width ||
        camera->cached_window_height != height) {
      camera->cached_window_width = width;
      camera->cached_window_height = height;
      camera->projection_dirty = true_v;
    }
  }
}

VkrCamera *vkr_camera_registry_get_by_handle(VkrCameraSystem *system,
                                             VkrCameraHandle h) {
  assert_log(system != NULL, "System is NULL");

  if (h.id == 0 || h.generation == VKR_INVALID_ID) {
    return NULL;
  }

  uint32_t index = h.id - 1;
  if (index >= system->cameras.length) {
    return NULL;
  }

  VkrCamera *camera = &system->cameras.data[index];
  if (camera->generation != h.generation ||
      camera->type == VKR_CAMERA_TYPE_NONE) {
    return NULL;
  }

  return camera;
}

VkrCamera *vkr_camera_registry_get_by_index(VkrCameraSystem *system,
                                            uint32_t index) {
  assert_log(system != NULL, "System is NULL");

  if (index >= system->cameras.length) {
    return NULL;
  }

  VkrCamera *camera = &system->cameras.data[index];
  if (camera->generation == VKR_INVALID_ID ||
      camera->type == VKR_CAMERA_TYPE_NONE) {
    return NULL;
  }

  return camera;
}
