/**
 * @file vkr_scene_system.h
 * @brief Scene system for managing ECS-based scenes with renderer integration.
 *
 * The scene system provides:
 * - Entity/component management via VkrWorld (ECS)
 * - Transform hierarchy with topological sorting for parent-before-child
 * updates
 * - Dirty tracking for efficient render bridge sync
 * - Mesh ownership tracking for cleanup
 * - Picking result to entity mapping
 */
#pragma once

#include "containers/str.h"
#include "core/vkr_entity.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_quat.h"
#include "memory/vkr_allocator.h"
#include "renderer/resources/vkr_resources.h"

// Forward declarations
struct s_RendererFrontend;
struct VkrMeshLoadDesc;
typedef struct SceneChildIndexSlot SceneChildIndexSlot;

// ============================================================================
// Error Types
// ============================================================================

typedef enum VkrSceneError {
  VKR_SCENE_ERROR_NONE = 0,
  VKR_SCENE_ERROR_ALLOC_FAILED,
  VKR_SCENE_ERROR_WORLD_INIT_FAILED,
  VKR_SCENE_ERROR_COMPONENT_REGISTRATION_FAILED,
  VKR_SCENE_ERROR_ENTITY_LIMIT_REACHED,
  VKR_SCENE_ERROR_INVALID_ENTITY,
  VKR_SCENE_ERROR_MESH_LOAD_FAILED,
  VKR_SCENE_ERROR_FILE_NOT_FOUND,
  VKR_SCENE_ERROR_FILE_READ_FAILED,
  VKR_SCENE_ERROR_PARSE_FAILED,
  VKR_SCENE_ERROR_UNSUPPORTED_VERSION,
  VKR_SCENE_ERROR_COMPONENT_ADD_FAILED,
} VkrSceneError;

// ============================================================================
// Component Types
// ============================================================================

/**
 * @brief Name component for entities.
 * The name string is owned by the scene allocator.
 */
typedef struct SceneName {
  String8 name;
} SceneName;

// Transform dirty flags
#define SCENE_TRANSFORM_DIRTY_LOCAL 0x01 // TRS changed, recompute local matrix
#define SCENE_TRANSFORM_DIRTY_WORLD 0x02 // World matrix needs recompute
#define SCENE_TRANSFORM_DIRTY_HIERARCHY                                        \
  0x04 // Parent link changed, rebuild topo order
#define SCENE_TRANSFORM_WORLD_UPDATED                                          \
  0x08 // World matrix was updated this frame (for child propagation)

/**
 * @brief Transform component with TRS, cached matrices, and hierarchy support.
 *
 * Dirty flag semantics:
 * - DIRTY_LOCAL: Set by transform setters. Cleared after local matrix
 * recompute.
 * - DIRTY_WORLD: Set when local or parent changes. Cleared after world matrix
 * recompute.
 * - DIRTY_HIERARCHY: Set when parent link changes. Triggers topo order rebuild.
 * - WORLD_UPDATED: Set when world matrix updated this frame. Used for deferred
 *   dirty propagation to children during topo traversal. Cleared in Pass 1.
 */
typedef struct SceneTransform {
  Vec3 position;
  VkrQuat rotation;
  Vec3 scale;

  VkrEntityId parent; // VKR_ENTITY_ID_INVALID means root
  Mat4 local;         // Cached local matrix (TRS composition)
  Mat4 world;         // Cached world matrix (parent.world * local)

  uint8_t flags; // Bitmask of SCENE_TRANSFORM_DIRTY_* flags
} SceneTransform;

/**
 * @brief Mesh renderer component linking entity to mesh manager slot.
 */
typedef struct SceneMeshRenderer {
  VkrMeshInstanceHandle instance; // Handle to mesh instance
} SceneMeshRenderer;

/**
 * @brief Visibility component for controlling render visibility.
 */
typedef struct SceneVisibility {
  bool8_t visible;        // If false, entity is not rendered
  bool8_t inherit_parent; // If true, effective = parent.visible && this.visible
} SceneVisibility;

/**
 * @brief Persistent render id for picking and editor selection.
 *
 * The id is stable for the entity lifetime and is never reused.
 * Scene picking encodes object_id = render_id + 1 (kind 0, 0 = background).
 * Top bits are reserved for picking kind tags, so the render id range is
 * limited to keep object_id encodings unambiguous.
 */
typedef struct SceneRenderId {
  uint32_t id;
} SceneRenderId;

// ============================================================================
// Text3D and Shape Components
// ============================================================================

/**
 * @brief Shape types for scene primitive shapes.
 */
typedef enum SceneShapeType {
  SCENE_SHAPE_TYPE_CUBE = 0,
  SCENE_SHAPE_TYPE_COUNT,
} SceneShapeType;

/**
 * @brief 3D text component tied to world-resources text instances.
 *
 * text_index is the world text id (currently entity index).
 * World resources own GPU instances; the scene stores ids/size metadata.
 * world_width/world_height capture the base plane size for gizmo pivot math.
 */
typedef struct SceneText3D {
  uint32_t text_index;    // World text id (currently entity index)
  bool8_t dirty;          // True if text content changed, needs re-render
  float32_t world_width;  // Base width in world units (before entity scale)
  float32_t world_height; // Base height in world units (before entity scale)
} SceneText3D;

/**
 * @brief Primitive shape component rendered via mesh manager.
 *
 * The geometry is generated on load and tracked as a scene-owned mesh.
 */
typedef struct SceneShape {
  SceneShapeType type;
  Vec3 dimensions;     // Width, height, depth (for cube)
  Vec4 color;          // RGBA color
  uint32_t mesh_index; // Index into mesh manager (generated geometry)
} SceneShape;

// ============================================================================
// Light Components
// ============================================================================

/**
 * @brief Directional light component.
 *
 * World direction is computed as: quat_rotate(transform.rotation,
 * direction_local). If entity has no transform, direction_local is used
 * directly.
 */
typedef struct SceneDirectionalLight {
  Vec3 color;           // Linear RGB
  float32_t intensity;  // Light intensity multiplier
  Vec3 direction_local; // Local-space direction (default: {0, -1, 0})
  bool8_t enabled;      // Whether this light is active
} SceneDirectionalLight;

/**
 * @brief Point light component.
 *
 * Position is derived from entity's SceneTransform.world translation.
 * Attenuation follows the formula: 1 / (constant + linear*d + quadratic*d^2)
 */
typedef struct ScenePointLight {
  Vec3 color;          // Linear RGB
  float32_t intensity; // Light intensity multiplier
  float32_t constant;  // Attenuation constant term (usually 1.0)
  float32_t linear;    // Attenuation linear term
  float32_t quadratic; // Attenuation quadratic term
  bool8_t enabled;     // Whether this light is active
} ScenePointLight;

// ============================================================================
// Scene Type
// ============================================================================

/**
 * @brief Scene containing ECS world and renderer integration state.
 */
typedef struct VkrScene {
  VkrWorld *world;               // ECS storage (authoritative scene state)
  VkrAllocator *alloc;           // Scene-owned allocator
  struct s_RendererFrontend *rf; // Renderer for layer messages
  uint16_t world_id;             // Copied into entity IDs

  // Component type IDs (cached after registration)
  VkrComponentTypeId comp_name;
  VkrComponentTypeId comp_transform;
  VkrComponentTypeId comp_mesh_renderer;
  VkrComponentTypeId comp_visibility;
  VkrComponentTypeId comp_render_id;
  VkrComponentTypeId comp_text3d;
  VkrComponentTypeId comp_shape;
  VkrComponentTypeId comp_directional_light;
  VkrComponentTypeId comp_point_light;

  // Compiled queries for efficient per-frame iteration
  VkrQueryCompiled query_transforms;  // Entities with SceneTransform
  VkrQueryCompiled query_renderables; // (SceneTransform, SceneMeshRenderer)
  VkrQueryCompiled
      query_directional_light;         // Entities with SceneDirectionalLight
  VkrQueryCompiled query_point_lights; // (SceneTransform, ScenePointLight)
  VkrQueryCompiled query_shapes;       // (SceneTransform, SceneShape)
  bool8_t queries_valid;               // False until first compile

  // Transform hierarchy support
  VkrEntityId *topo_order; // Topologically sorted entity IDs (full IDs, not just indices)
  uint32_t topo_count;     // Number of entities in topo_order
  uint32_t topo_capacity;  // Allocated size
  bool8_t
      hierarchy_dirty; // Set when parent links change; triggers topo rebuild

  // Parent -> children index for transform hierarchy queries.
  // Stored as a slot array keyed by parent entity index with a generation
  // guard.
  SceneChildIndexSlot *child_index_slots;
  uint32_t child_index_capacity; // Slot count (>= world->dir.capacity)
  bool8_t child_index_valid;     // False until rebuilt or incrementally updated

  // Owned mesh indices (mesh-slot path; used by shapes)
  uint32_t *owned_meshes;
  uint32_t owned_mesh_count;
  uint32_t owned_mesh_capacity;

  // Owned mesh instances (for cleanup on scene destroy - new instance system)
  VkrMeshInstanceHandle *owned_instances;
  uint32_t owned_instance_count;
  uint32_t owned_instance_capacity;

  // Render dirty tracking (entities needing sync to mesh manager)
  VkrEntityId *render_dirty_entities;
  uint32_t render_dirty_count;
  uint32_t render_dirty_capacity;
  bool8_t render_full_sync_needed; // Set on scene load or dirty overflow

  uint32_t next_render_id; // Monotonic render id allocator (0 reserved)

} VkrScene;

// ============================================================================
// Scene Lifecycle
// ============================================================================

/**
 * @brief Initialize a scene.
 * @param scene Scene to initialize (caller-allocated)
 * @param alloc Allocator for scene data
 * @param world_id World ID embedded in entity IDs (use 0 for single-scene apps)
 * @param initial_entity_capacity Initial entity capacity hint
 * @param out_error Optional error output
 * @return true on success
 */
bool8_t vkr_scene_init(VkrScene *scene, VkrAllocator *alloc, uint16_t world_id,
                       uint32_t initial_entity_capacity,
                       VkrSceneError *out_error);

/**
 * @brief Shutdown a scene and release all resources.
 * @param scene Scene to shutdown
 * @param rf Optional renderer frontend (to remove owned meshes); waits for
 * renderer idle before removing meshes to avoid freeing in-flight resources.
 */
void vkr_scene_shutdown(VkrScene *scene, struct s_RendererFrontend *rf);

/**
 * @brief Update scene transforms and prepare for renderer sync.
 * Call once per frame before syncing the scene to the renderer.
 * @param scene Scene to update
 * @param dt Delta time (currently unused, reserved for future animation)
 */
void vkr_scene_update(VkrScene *scene, float64_t dt);

// ============================================================================
// Scene Runtime Handle API (preferred for renderer/resource integration)
// ============================================================================

/**
 * @brief Creates a runtime scene handle with an internal render bridge.
 *
 * Intended for use by `VkrResourceSystem` loaders and other higher-level
 * systems that want a single handle for update/sync/picking.
 *
 * @param alloc Allocator for the runtime handle and scene-owned data.
 * @param world_id World ID embedded in entity IDs (use 0 for single-scene
 * apps).
 * @param initial_entity_capacity Initial ECS entity capacity hint.
 * @param initial_picking_capacity Initial render-id-to-entity mapping capacity.
 * @param out_error Optional error output.
 * @return A valid handle on success, or `VKR_SCENE_HANDLE_INVALID` on failure.
 */
VkrSceneHandle vkr_scene_handle_create(VkrAllocator *alloc, uint16_t world_id,
                                       uint32_t initial_entity_capacity,
                                       uint32_t initial_picking_capacity,
                                       VkrSceneError *out_error);

/**
 * @brief Destroys a runtime scene handle and releases owned renderer resources.
 *
 * @param handle Scene handle to destroy.
 * @param rf Optional renderer frontend (required for owned mesh cleanup).
 */
void vkr_scene_handle_destroy(VkrSceneHandle handle,
                              struct s_RendererFrontend *rf);

/**
 * @brief Gets the underlying scene pointer from a runtime handle.
 * @param handle Scene handle.
 * @return Scene pointer, or NULL if handle is invalid.
 */
VkrScene *vkr_scene_handle_get_scene(VkrSceneHandle handle);

/**
 * @brief Updates scene transforms/dirty tracking for a runtime handle.
 * @param handle Scene handle.
 * @param dt Delta time (currently unused).
 */
void vkr_scene_handle_update(VkrSceneHandle handle, float64_t dt);

/**
 * @brief Incrementally syncs dirty entities from scene to renderer.
 * @param handle Scene handle.
 * @param rf Renderer frontend.
 */
void vkr_scene_handle_sync(VkrSceneHandle handle,
                           struct s_RendererFrontend *rf);

/**
 * @brief Full sync of all renderables (use after scene load).
 * @param handle Scene handle.
 * @param rf Renderer frontend.
 */
void vkr_scene_handle_full_sync(VkrSceneHandle handle,
                                struct s_RendererFrontend *rf);

/**
 * @brief Convenience helper: update + incremental sync.
 * @param handle Scene handle.
 * @param rf Renderer frontend.
 * @param dt Delta time.
 */
void vkr_scene_handle_update_and_sync(VkrSceneHandle handle,
                                      struct s_RendererFrontend *rf,
                                      float64_t dt);

/**
 * @brief Map picking object_id to entity for a runtime handle.
 * @param handle Scene handle.
 * @param object_id Picking result (scene kind, render_id + 1, or 0 for
 * background).
 * @return Entity ID, or VKR_ENTITY_ID_INVALID if not found.
 */
VkrEntityId vkr_scene_handle_entity_from_picking_id(VkrSceneHandle handle,
                                                    uint32_t object_id);

// ============================================================================
// Entity Management
// ============================================================================

/**
 * @brief Create a new entity in the scene.
 * @param scene Scene to create entity in
 * @param out_error Optional error output
 * @return Entity ID, or VKR_ENTITY_ID_INVALID on failure
 */
VkrEntityId vkr_scene_create_entity(VkrScene *scene, VkrSceneError *out_error);

/**
 * @brief Destroy an entity and remove it from the scene.
 * @param scene Scene containing the entity
 * @param entity Entity to destroy
 */
void vkr_scene_destroy_entity(VkrScene *scene, VkrEntityId entity);

/**
 * @brief Check if an entity is alive.
 * @param scene Scene to check in
 * @param entity Entity to check
 * @return true if entity exists and is alive
 */
bool8_t vkr_scene_entity_alive(const VkrScene *scene, VkrEntityId entity);

// ============================================================================
// Component Helpers
// ============================================================================

/**
 * @brief Set entity name (copies string into scene allocator).
 * @param scene Scene containing the entity
 * @param entity Entity to name
 * @param name Name string (copied)
 * @return true on success
 */
bool8_t vkr_scene_set_name(VkrScene *scene, VkrEntityId entity, String8 name);

/**
 * @brief Get entity name.
 * @param scene Scene containing the entity
 * @param entity Entity to query
 * @return Name string, or empty string if no name component
 */
String8 vkr_scene_get_name(const VkrScene *scene, VkrEntityId entity);

/**
 * @brief Add or update transform component.
 * @param scene Scene containing the entity
 * @param entity Entity to modify
 * @param position World position
 * @param rotation Orientation quaternion
 * @param scale Scale factors
 * @return true on success
 */
bool8_t vkr_scene_set_transform(VkrScene *scene, VkrEntityId entity,
                                Vec3 position, VkrQuat rotation, Vec3 scale);

/**
 * @brief Get transform component (mutable).
 * @param scene Scene containing the entity
 * @param entity Entity to query
 * @return Transform pointer, or NULL if no transform component
 */
SceneTransform *vkr_scene_get_transform(VkrScene *scene, VkrEntityId entity);

/**
 * @brief Set entity position (auto-marks dirty).
 */
void vkr_scene_set_position(VkrScene *scene, VkrEntityId entity, Vec3 position);

/**
 * @brief Set entity rotation (auto-marks dirty).
 */
void vkr_scene_set_rotation(VkrScene *scene, VkrEntityId entity,
                            VkrQuat rotation);

/**
 * @brief Set entity scale (auto-marks dirty).
 */
void vkr_scene_set_scale(VkrScene *scene, VkrEntityId entity, Vec3 scale);

/**
 * @brief Set entity parent (auto-marks hierarchy dirty).
 * @param scene Scene containing the entity
 * @param entity Entity to reparent
 * @param parent New parent entity (VKR_ENTITY_ID_INVALID for root)
 */
void vkr_scene_set_parent(VkrScene *scene, VkrEntityId entity,
                          VkrEntityId parent);

/**
 * @brief Add mesh renderer component and ensure a render id for picking.
 * @param scene Scene containing the entity
 * @param entity Entity to modify
 * @param instance Handle to mesh instance
 * @return true on success
 */
bool8_t vkr_scene_set_mesh_renderer(VkrScene *scene, VkrEntityId entity,
                                    VkrMeshInstanceHandle instance);

/**
 * @brief Ensure entity has a render id (assigned if missing).
 * @param scene Scene containing the entity
 * @param entity Entity to ensure render id for
 * @param out_render_id Optional output for the assigned render id
 * @return true on success, false if the render id space is exhausted
 */
bool8_t vkr_scene_ensure_render_id(VkrScene *scene, VkrEntityId entity,
                                   uint32_t *out_render_id);

/**
 * @brief Get entity render id.
 * @param scene Scene containing the entity
 * @param entity Entity to query
 * @return Render id or 0 if missing
 */
uint32_t vkr_scene_get_render_id(const VkrScene *scene, VkrEntityId entity);

/**
 * @brief Set visibility component.
 * @param scene Scene containing the entity
 * @param entity Entity to modify
 * @param visible Visibility state
 * @param inherit_parent Whether to inherit parent visibility
 */
void vkr_scene_set_visibility(VkrScene *scene, VkrEntityId entity,
                              bool8_t visible, bool8_t inherit_parent);

// ============================================================================
// Light Components
// ============================================================================

/**
 * @brief Add a point light component and ensure render id for picking.
 * @param scene Scene containing the entity.
 * @param entity Entity to add point light to (must have SceneTransform).
 * @param light Point light configuration.
 * @return true on success.
 */
bool8_t vkr_scene_set_point_light(VkrScene *scene, VkrEntityId entity,
                                  const ScenePointLight *light);

/**
 * @brief Get point light component for an entity.
 * @param scene Scene containing the entity.
 * @param entity Entity to query.
 * @return Pointer to component, or NULL if entity lacks point light.
 */
ScenePointLight *vkr_scene_get_point_light(VkrScene *scene, VkrEntityId entity);

/**
 * @brief Add a directional light component.
 * @param scene Scene containing the entity.
 * @param entity Entity to add directional light to.
 * @param light Directional light configuration.
 * @return true on success.
 */
bool8_t vkr_scene_set_directional_light(VkrScene *scene, VkrEntityId entity,
                                        const SceneDirectionalLight *light);

/**
 * @brief Get directional light component for an entity.
 * @param scene Scene containing the entity.
 * @param entity Entity to query.
 * @return Pointer to component, or NULL if entity lacks directional light.
 */
SceneDirectionalLight *vkr_scene_get_directional_light(VkrScene *scene,
                                                       VkrEntityId entity);

// ============================================================================
// Mesh Ownership
// ============================================================================

/**
 * @brief Spawn a mesh via mesh manager and track ownership.
 * Scene will destroy owned meshes on shutdown.
 * @param scene Scene to own the mesh
 * @param rf Renderer frontend
 * @param desc Mesh load descriptor
 * @param out_mesh_index Output mesh index
 * @param out_error Optional error output
 * @return true on success
 */
bool8_t vkr_scene_spawn_mesh(VkrScene *scene, struct s_RendererFrontend *rf,
                             const struct VkrMeshLoadDesc *desc,
                             uint32_t *out_mesh_index,
                             VkrSceneError *out_error);

/**
 * @brief Track an externally-created mesh as scene-owned.
 * @param scene Scene to own the mesh.
 * @param mesh_index Mesh index to claim.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_track_mesh(VkrScene *scene, uint32_t mesh_index,
                             VkrSceneError *out_error);

/**
 * @brief Release a mesh from scene ownership.
 * Scene will no longer destroy this mesh on shutdown.
 * @param scene Scene owning the mesh
 * @param mesh_index Mesh index to release
 */
void vkr_scene_release_mesh(VkrScene *scene, uint32_t mesh_index);

/**
 * @brief Track a mesh instance as scene-owned.
 * Scene will destroy this instance on shutdown.
 * @param scene Scene to own the instance.
 * @param instance Instance handle to track.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_track_instance(VkrScene *scene, VkrMeshInstanceHandle instance,
                                  VkrSceneError *out_error);

/**
 * @brief Release a mesh instance from scene ownership.
 * Scene will no longer destroy this instance on shutdown.
 * @param scene Scene owning the instance
 * @param instance Instance handle to release
 */
void vkr_scene_release_instance(VkrScene *scene, VkrMeshInstanceHandle instance);

// ============================================================================
// Text3D Component
// ============================================================================

/**
 * @brief Configuration for adding a text3d component to an entity.
 */
typedef struct VkrSceneText3DConfig {
  String8 text;            // Text content (passed through to world resources)
  VkrFontHandle font;      // Font handle (or invalid for default)
  float32_t font_size;     // Font size in points (0 = font's native)
  Vec4 color;              // Text color RGBA
  uint32_t texture_width;  // Texture width (0 = auto)
  uint32_t texture_height; // Texture height (0 = auto)
  float32_t uv_inset_px;   // Half-texel inset to avoid bleeding (0 = default)
} VkrSceneText3DConfig;

#define VKR_SCENE_TEXT3D_CONFIG_DEFAULT                                        \
  (VkrSceneText3DConfig){.text = {0},                                          \
                         .font = VKR_FONT_HANDLE_INVALID,                      \
                         .font_size = 32.0f,                                   \
                         .color = {1.0f, 1.0f, 1.0f, 1.0f},                    \
                         .texture_width = 512,                                 \
                         .texture_height = 128,                                \
                         .uv_inset_px = 0.5f}

/**
 * @brief Add a text3d component to an entity.
 *
 * Sends a world-resources create request and links the entity to that text id.
 * World resources own GPU resources; the scene stores text metadata.
 *
 * @param scene Scene containing the entity.
 * @param entity Entity to add text3d component to.
 * @param config Text configuration.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_set_text3d(VkrScene *scene, VkrEntityId entity,
                             const VkrSceneText3DConfig *config,
                             VkrSceneError *out_error);

/**
 * @brief Get the SceneText3D component for an entity.
 * @param scene Scene containing the entity.
 * @param entity Entity to query.
 * @return Pointer to component, or NULL if entity lacks text3d.
 */
SceneText3D *vkr_scene_get_text3d(VkrScene *scene, VkrEntityId entity);

/**
 * @brief Update text content for a text3d entity.
 *
 * Marks the text as dirty for re-rendering on next sync.
 *
 * @param scene Scene containing the entity.
 * @param entity Entity with text3d component.
 * @param text New text content (passed to world resources).
 * @return true on success.
 */
bool8_t vkr_scene_update_text3d(VkrScene *scene, VkrEntityId entity,
                                String8 text);

// ============================================================================
// Shape Component
// ============================================================================

/**
 * @brief Configuration for adding a shape component to an entity.
 */
typedef struct VkrSceneShapeConfig {
  SceneShapeType type;   // Shape type (cube only for now)
  Vec3 dimensions;       // Width, height, depth
  Vec4 color;            // RGBA color
  String8 material_name; // Material name for acquire (matches .mt name=)
  String8 material_path; // Material file path for loading
} VkrSceneShapeConfig;

#define VKR_SCENE_SHAPE_CONFIG_DEFAULT                                         \
  (VkrSceneShapeConfig) {                                                      \
    .type = SCENE_SHAPE_TYPE_CUBE, .dimensions = {1.0f, 1.0f, 1.0f},           \
    .color = {1.0f, 1.0f, 1.0f, 1.0f}, .material_name = {0},                   \
    .material_path = {0}                                                       \
  }

/**
 * @brief Add a shape component to an entity.
 *
 * Creates geometry via geometry system and adds it to mesh manager.
 * The mesh is tracked as scene-owned.
 *
 * @param scene Scene containing the entity.
 * @param rf Renderer frontend.
 * @param entity Entity to add shape to.
 * @param config Shape configuration.
 * @param out_error Optional error output.
 * @return true on success.
 */
bool8_t vkr_scene_set_shape(VkrScene *scene, struct s_RendererFrontend *rf,
                            VkrEntityId entity,
                            const VkrSceneShapeConfig *config,
                            VkrSceneError *out_error);

/**
 * @brief Get the SceneShape component for an entity.
 * @param scene Scene containing the entity.
 * @param entity Entity to query.
 * @return Pointer to component, or NULL if entity lacks shape.
 */
const SceneShape *vkr_scene_get_shape(const VkrScene *scene,
                                      VkrEntityId entity);

// ============================================================================
// Entity Lookup
// ============================================================================

/**
 * @brief Find an entity by name.
 * @param scene Scene to search.
 * @param name Entity name to find.
 * @return Entity ID, or VKR_ENTITY_ID_INVALID if not found.
 */
VkrEntityId vkr_scene_find_entity_by_name(const VkrScene *scene, String8 name);
