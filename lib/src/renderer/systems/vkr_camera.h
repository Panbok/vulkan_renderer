#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "containers/vkr_hashtable.h"
#include "core/vkr_window.h"
#include "math/mat.h"
#include "math/vec.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"

#define VKR_MAX_MOUSE_DELTA 100.0f
#define VKR_DEFAULT_CAMERA_ZOOM 90.0f
#define VKR_DEFAULT_CAMERA_SPEED 7.5f
#define VKR_DEFAULT_CAMERA_SENSITIVITY 6.0f
#define VKR_DEFAULT_CAMERA_YAW -90.0f
#define VKR_DEFAULT_CAMERA_PITCH 0.0f

#define VKR_MIN_CAMERA_ZOOM 1.0f
#define VKR_MAX_CAMERA_ZOOM 45.0f
#define VKR_MAX_CAMERA_PITCH 89.0f
#define VKR_MIN_CAMERA_PITCH -89.0f

#define VKR_DEFAULT_CAMERA_POSITION vec3_new(-1.5f, 0.0f, -17.0f)
#define VKR_DEFAULT_CAMERA_FORWARD vec3_new(0.0f, 0.0f, -1.0f)
#define VKR_DEFAULT_CAMERA_UP vec3_new(0.0f, 1.0f, 0.0f)
#define VKR_DEFAULT_CAMERA_RIGHT vec3_new(1.0f, 0.0f, 0.0f)
#define VKR_DEFAULT_CAMERA_WORLD_UP vec3_new(0.0f, 1.0f, 0.0f)

#define VKR_CAMERA_SYSTEM_DEFAULT_ARENA_RSV MB(3)
#define VKR_CAMERA_SYSTEM_DEFAULT_ARENA_CMT MB(1)

/**
 * @brief Camera projection types.
 */
typedef enum VkrCameraType {
  VKR_CAMERA_TYPE_NONE,         /**< Uninitialized camera */
  VKR_CAMERA_TYPE_PERSPECTIVE,  /**< 3D perspective projection */
  VKR_CAMERA_TYPE_ORTHOGRAPHIC, /**< 2D/3D orthographic projection */
} CameraType;

/**
 * @brief 3D camera storing orientation and projection state.
 *
 * The camera keeps track of position/orientation vectors along with cached view
 * and projection matrices. Input devices are handled by controller systems that
 * mutate this data and mark matrices dirty before rendering.
 */
typedef struct VkrCamera {
  VkrWindow *window; /**< Window for input capture and aspect ratio */

  CameraType type;     /**< Current projection type */
  uint32_t generation; /**< Handle generation for registry validation */

  Mat4 view;       /**< View matrix */
  Mat4 projection; /**< Projection matrix */

  Vec3 position; /**< Camera world position */
  Vec3 forward;  /**< Forward direction vector */
  Vec3 up;       /**< Up direction vector */
  Vec3 right;    /**< Right direction vector */
  Vec3 world_up; /**< World up reference (usually 0,1,0) */

  float32_t yaw;   /**< Horizontal rotation (degrees) */
  float32_t pitch; /**< Vertical rotation (degrees, clamped ±89°) */

  float32_t speed;       /**< Movement speed (units per second) */
  float32_t sensitivity; /**< Mouse sensitivity multiplier */

  float32_t near_clip; /**< Near clipping plane distance */
  float32_t far_clip;  /**< Far clipping plane distance */

  // Perspective projection
  float32_t zoom; /**< Field of view for perspective (degrees) */

  // Orthographic projection
  float32_t left_clip;   /**< Left boundary for orthographic */
  float32_t right_clip;  /**< Right boundary for orthographic */
  float32_t bottom_clip; /**< Bottom boundary for orthographic */
  float32_t top_clip;    /**< Top boundary for orthographic */

  bool8_t view_dirty;       /**< Requires view matrix recompute */
  bool8_t projection_dirty; /**< Requires projection matrix recompute */

  uint32_t cached_window_width;  /**< Last window width used for projection */
  uint32_t cached_window_height; /**< Last window height used for projection */
} VkrCamera;
Array(VkrCamera);

/**
 * @brief Camera handle
 * @param id Camera id
 * @param generation Camera generation
 */
typedef struct VkrCameraHandle {
  uint32_t id;
  uint32_t generation;
} VkrCameraHandle;

/**
 * @brief Invalid camera handle
 */
#define VKR_CAMERA_HANDLE_INVALID                                              \
  (VkrCameraHandle) { .id = 0, .generation = VKR_INVALID_ID }

/**
 * @brief Camera entry
 * @param index Camera index
 * @param ref_count Camera reference count
 * @param auto_release Auto release
 */
typedef struct VkrCameraEntry {
  uint32_t index;
  uint32_t ref_count;
  bool8_t auto_release;
} VkrCameraEntry;
VkrHashTable(VkrCameraEntry);

/**
 * @brief Camera system configuration
 * @param max_camera_count Maximum camera count
 * @param arena_reserve Arena reserve
 * @param arena_commit Arena commit
 */
typedef struct VkrCameraSystemConfig {
  uint32_t max_camera_count;
  uint64_t arena_reserve;
  uint64_t arena_commit;
} VkrCameraSystemConfig;

/**
 * @brief Camera system
 * @param arena Arena
 * @param cameras Cameras
 * @param camera_map Camera map
 * @param next_free_index Next free index
 * @param generation_counter Generation counter
 * @param default_camera Default camera
 * @param active_camera Active camera
 */
typedef struct VkrCameraSystem {
  Arena *arena;
  VkrAllocator allocator;
  Array_VkrCamera cameras;
  VkrHashTable_VkrCameraEntry camera_map;
  uint32_t next_free_index;
  uint32_t generation_counter;
  VkrCameraHandle default_camera;
  VkrCameraHandle active_camera;
} VkrCameraSystem;

/**
 * @brief Creates a perspective camera with 3D projection.
 * @param camera Camera to initialize
 * @param window Window for mouse capture and aspect ratio
 * @param zoom Initial zoom value (affects field of view)
 * @param near_clip Near clipping plane distance
 * @param far_clip Far clipping plane distance
 */
void vkr_camera_system_perspective_create(VkrCamera *camera, VkrWindow *window,
                                          float32_t zoom, float32_t near_clip,
                                          float32_t far_clip);

/**
 * @brief Creates an orthographic camera with 2D/3D projection.
 * @param camera Camera to initialize
 * @param window Window for mouse capture
 * @param left Left boundary of the orthographic volume
 * @param right Right boundary of the orthographic volume
 * @param bottom Bottom boundary of the orthographic volume
 * @param top Top boundary of the orthographic volume
 * @param near_clip Near clipping plane distance
 * @param far_clip Far clipping plane distance
 */
void vkr_camera_system_orthographic_create(VkrCamera *camera, VkrWindow *window,
                                           float32_t left, float32_t right,
                                           float32_t bottom, float32_t top,
                                           float32_t near_clip,
                                           float32_t far_clip);

/**
 * @brief Translates the camera by the supplied world-space delta.
 * @param camera Camera to translate
 * @param delta Delta
 */
void vkr_camera_translate(VkrCamera *camera, Vec3 delta);

/**
 * @brief Adds yaw/pitch deltas (in degrees) and refreshes orientation vectors.
 * @param camera Camera to rotate
 * @param yaw_delta Yaw delta
 * @param pitch_delta Pitch delta
 */
void vkr_camera_rotate(VkrCamera *camera, float32_t yaw_delta,
                       float32_t pitch_delta);

/**
 * @brief Adjusts zoom (perspective FOV) and marks projection dirty.
 * @param camera Camera to zoom
 * @param zoom_delta Zoom delta
 */
void vkr_camera_zoom(VkrCamera *camera, float32_t zoom_delta);

/**
 * @brief Recomputes camera matrices if marked dirty.
 *
 * @param camera Camera to update
 */
void vkr_camera_system_update(VkrCamera *camera);

/**
 * @brief Gets the view matrix for rendering.
 * @param camera Camera to get view matrix from
 * @return 4x4 view matrix for transforming world space to view space
 */
Mat4 vkr_camera_system_get_view_matrix(const VkrCamera *camera);

/**
 * @brief Gets the projection matrix for rendering.
 * @param camera Camera to get projection matrix from
 * @return 4x4 projection matrix for transforming view space to clip space
 */
Mat4 vkr_camera_system_get_projection_matrix(const VkrCamera *camera);

// =============================================================================
// Camera registry
// =============================================================================

/**
 * @brief Initializes the camera system
 * @param config Camera system configuration
 * @param out_system Output camera system
 * @return true on success, false on failure
 */
bool8_t vkr_camera_registry_init(const VkrCameraSystemConfig *config,
                                 VkrCameraSystem *out_system);

/**
 * @brief Shuts down the camera system
 * @param system Camera system to shutdown
 */
void vkr_camera_registry_shutdown(VkrCameraSystem *system);

/**
 * @brief Creates a perspective camera
 * @param system Camera system
 * @param name Camera name
 * @param window Window
 * @param zoom Zoom
 * @param near_clip Near clip
 * @param far_clip Far clip
 * @param out_handle Output camera handle
 * @return true on success, false on failure
 */
bool8_t vkr_camera_registry_create_perspective(
    VkrCameraSystem *system, String8 name, VkrWindow *window, float32_t zoom,
    float32_t near_clip, float32_t far_clip, VkrCameraHandle *out_handle);

/**
 * @brief Creates an orthographic camera
 * @param system Camera system
 * @param name Camera name
 * @param window Window
 * @param left Left clip
 * @param right Right clip
 * @param bottom Bottom clip
 * @param top Top clip
 * @param near_clip Near clip
 * @param far_clip Far clip
 * @param out_handle Output camera handle
 * @return true on success, false on failure
 */
bool8_t vkr_camera_registry_create_orthographic(
    VkrCameraSystem *system, String8 name, VkrWindow *window, float32_t left,
    float32_t right, float32_t bottom, float32_t top, float32_t near_clip,
    float32_t far_clip, VkrCameraHandle *out_handle);

/**
 * @brief Acquires a camera by name
 * @param system Camera system
 * @param name Camera name
 * @param auto_release Auto release
 * @param out_ok Output success flag
 * @return Camera handle
 */
VkrCameraHandle vkr_camera_registry_acquire(VkrCameraSystem *system,
                                            String8 name, bool8_t auto_release,
                                            bool8_t *out_ok);

/**
 * @brief Releases a camera by name
 * @param system Camera system
 * @param name Camera name
 */
void vkr_camera_registry_release(VkrCameraSystem *system, String8 name);

/**
 * @brief Releases a camera by handle
 * @param system Camera system
 * @param handle Camera handle
 */
void vkr_camera_registry_release_by_handle(VkrCameraSystem *system,
                                           VkrCameraHandle handle);

/**
 * @brief Updates a camera
 * @param system Camera system
 * @param h Camera handle
 */
void vkr_camera_registry_update(VkrCameraSystem *system, VkrCameraHandle h);

/**
 * @brief Updates all cameras
 * @param system Camera system
 */
void vkr_camera_registry_update_all(VkrCameraSystem *system);

/**
 * @brief Gets the view matrix for a camera
 * @param system Camera system
 * @param h Camera handle
 * @return View matrix
 */
Mat4 vkr_camera_registry_get_view(VkrCameraSystem *system, VkrCameraHandle h);

/**
 * @brief Gets the projection matrix for a camera
 * @param system Camera system
 * @param h Camera handle
 * @return Projection matrix
 */
Mat4 vkr_camera_registry_get_projection(VkrCameraSystem *system,
                                        VkrCameraHandle h);

/**
 * @brief Sets the active camera
 * @param system Camera system
 * @param h Camera handle
 */
void vkr_camera_registry_set_active(VkrCameraSystem *system, VkrCameraHandle h);

/**
 * @brief Gets the active camera
 * @param system Camera system
 * @return Active camera handle
 */
VkrCameraHandle vkr_camera_registry_get_active(VkrCameraSystem *system);

/**
 * @brief Gets the active view matrix
 * @param system Camera system
 * @return Active view matrix
 */
Mat4 vkr_camera_registry_get_active_view(VkrCameraSystem *system);

/**
 * @brief Gets the active projection matrix
 * @param system Camera system
 * @return Active projection matrix
 */
Mat4 vkr_camera_registry_get_active_projection(VkrCameraSystem *system);

/**
 * @brief Resizes all cameras
 * @param system Camera system
 * @param width New window width
 * @param height New window height
 */
void vkr_camera_registry_resize_all(VkrCameraSystem *system, uint32_t width,
                                    uint32_t height);

/**
 * @brief Gets a camera by handle
 * @param system Camera system
 * @param h Camera handle
 * @return Camera
 */
VkrCamera *vkr_camera_registry_get_by_handle(VkrCameraSystem *system,
                                             VkrCameraHandle h);

/**
 * @brief Gets a camera by index
 * @param system Camera system
 * @param index Camera index
 * @return Camera
 */
VkrCamera *vkr_camera_registry_get_by_index(VkrCameraSystem *system,
                                            uint32_t index);
