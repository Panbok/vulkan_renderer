#pragma once
#include "vkr_sample_runtime.h"

typedef struct VkrEditorScenePanels VkrEditorScenePanels;
typedef struct VkrEditorUi VkrEditorUi;
VkrEditorScenePanels *vkr_editor_scene_panels_create(VkrAllocator *allocator);
void vkr_editor_scene_panels_destroy(VkrEditorScenePanels *panels);
void vkr_editor_hierarchy_build(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame, VkrUiRect rect,
                                VkrFontHandle heading);
void vkr_editor_inspector_build(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame, VkrUiRect rect,
                                VkrFontHandle heading);

/* Icon of a world component type, shared by the Outliner, Details and the
   viewport's object icons. */
VkrUiIcon vkr_editor_world_type_icon(const VkrTypeDesc *type);
/* The Outliner's icon and tint for an entity: its lights, geometry, physics
   or world component, else a folder or an empty. */
VkrUiIcon vkr_editor_entity_icon(const VkrScene *scene, VkrEntityId entity,
                                 bool8_t has_child, Vec4 *out_color);

/* Objects the editor creates from nothing (ADR-076): an empty object, the
   four light kinds and one object per live world component type. */
uint32_t vkr_editor_object_kind_count(void);
/* Cmd word, such as "point_light" or a world type name. */
const char *vkr_editor_object_kind_word(uint32_t kind);
const char *vkr_editor_object_kind_label(uint32_t kind);
VkrUiIcon vkr_editor_object_kind_icon(uint32_t kind);
/* World id new objects go to: the selection's container, else the primary
   scene, else the World; UINT16_MAX when nothing is loaded. */
uint16_t vkr_editor_create_container(const VkrSampleUiFrame *frame);
/* Request an object of `kind`, named by its label, at the root of
   `container`; the runtime places it in front of the camera. World-only
   settings go to the World; false when their container is not loaded. */
/* Select, frame, rename or delete the loaded object a Content action names
   (ADR-076). */
struct VkrEditorContentAction;
void vkr_editor_apply_content_object(
    const VkrSampleUiFrame *frame, const struct VkrEditorContentAction *action);
/* A non-NULL `drop_px` places the object where that viewport pixel meets
   the scene; otherwise it appears in front of the camera. */
bool8_t vkr_editor_request_create(const VkrSampleUiFrame *frame, uint32_t kind,
                                  uint16_t container, const Vec2 *drop_px);

/* Current value of an entity's component: a light from its edit values, a
   world component from its typed storage. False when the entity lacks it. */
bool8_t vkr_editor_component_read(const VkrSampleUiFrame *frame,
                                  VkrEntityId entity, const VkrTypeDesc *type,
                                  void *out);
/* Request an undoable edit adding a default physics body (a static unit box)
   or removing the body and its colliders; false when nothing changes. */
bool8_t vkr_editor_request_physics_body(const VkrSampleUiFrame *frame,
                                        VkrEntityId entity, bool8_t present);
/* Request an undoable edit setting an entity's component to `value`, the way
   Details applies a finished change. */
void vkr_editor_request_component(const VkrSampleUiFrame *frame,
                                  VkrEntityId entity, const VkrTypeDesc *type,
                                  const void *value);

bool8_t vkr_editor_scene_panels_write_json(const VkrEditorScenePanels *panels,
                                           VkrJsonWriter *writer);
bool8_t vkr_editor_scene_panels_read_json(VkrEditorScenePanels *panels,
                                          String8 json);
bool8_t
vkr_editor_scene_panels_write_scene_json(const VkrEditorScenePanels *panels,
                                         const VkrSampleUiFrame *frame,
                                         VkrJsonWriter *writer);
bool8_t vkr_editor_scene_panels_read_scene_json(VkrEditorScenePanels *panels,
                                                const VkrSampleUiFrame *frame,
                                                String8 json);
