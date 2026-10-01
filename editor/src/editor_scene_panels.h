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
/* Menu heading of a kind, such as "Lights". */
const char *vkr_editor_object_kind_group(uint32_t kind);
/* Menus and Content list the built-in kinds; the per-script kinds that
   follow them stay for Cmd `create`. */
bool8_t vkr_editor_object_kind_listed(uint32_t kind);

/* Script slot (ADR-079): an entity's script is the first script module
   component it carries, or NULL. */
const VkrTypeDesc *vkr_editor_entity_script(const VkrScene *scene,
                                            VkrEntityId entity);
/* Component types of the loaded script modules, in module order. */
uint32_t vkr_editor_script_types(const VkrSampleUiFrame *frame,
                                 const VkrTypeDesc **out, uint32_t capacity);
/* The project source defining a script type: its module's `<Name>.c`. */
bool8_t vkr_editor_script_source(const VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 const VkrTypeDesc *type, char *out,
                                 uint32_t capacity);
/* Opens the entity's script source in the Script editor. */
bool8_t vkr_editor_open_entity_script(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      VkrEntityId entity);
/* Request one undoable edit giving the entity `type` as its script, replacing
   its current one; NULL removes it. */
void vkr_editor_request_script(const VkrSampleUiFrame *frame,
                               VkrEntityId entity, const VkrTypeDesc *type);
/* The loaded module's script type named `module`, or NULL. */
const VkrTypeDesc *vkr_editor_module_script(const VkrSampleUiFrame *frame,
                                            const char *module);
/* Attach a script to an object as one more of its components; false with a
   notice when it already runs it or cannot hold it. */
bool8_t vkr_editor_attach_script(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrEntityId entity, const VkrTypeDesc *type);
/* A Script asset dropped on the Scene at `drop_px`: attach it to the object
   there once the pick answers, else add an object running it. */
void vkr_editor_drop_script(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            const char *module, Vec2 drop_px);
/* The pick answer of a dropped Script asset. */
void vkr_editor_finish_script_drop(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame);
/* Focus the Details name field on its next build. */
void vkr_editor_scene_panels_request_rename(VkrEditorScenePanels *panels);
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

/* `entity` in container `scene` renders a mesh loaded from one of the build
   revisions `revisions`, such as those a background finalize rebuilds. */
bool8_t vkr_editor_entity_mesh_from(const VkrSampleUiFrame *frame,
                                    const VkrScene *scene, VkrEntityId entity,
                                    const char (*revisions)[37],
                                    uint32_t count);
/* The Outliner shows `entity` in a model whose textures cook in a background
   finalize (ADR-077); it stays locked against deletion and reparenting. */
bool8_t vkr_editor_scene_panels_cooking(const VkrEditorScenePanels *panels,
                                        VkrEntityId entity);
/* The Outliner turned viewport icons off for `entity` or an ancestor. */
bool8_t vkr_editor_scene_panels_icons_off(const VkrEditorScenePanels *panels,
                                          const VkrSampleUiFrame *frame,
                                          VkrEntityId entity);

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
