/**
 * @file vkr_script_host.h
 * @brief Runs C script modules on the loaded containers (ADR-079).
 *
 * Modules are written against `sdk.h`. The host owns the SDK table, the
 * registered modules and one session. A session attaches the active
 * container (the played scene, or the World when it plays alone) and the
 * root World. Each container-scoped module runs one instance per attached
 * container and each World-scoped module one instance on the World.
 *
 * Every instance owns its module data and a ledger of what its hooks
 * acquired; behaviors run per entity with their own ledgers. The host
 * releases ledgers in reverse order when a behavior's entity leaves, an
 * instance ends or its container detaches, so a failed start leaves nothing
 * behind. It is the active scene's only simulation callback client and the
 * input's only observer while started.
 *
 * Modules either link into the executable or load from a shared library. A
 * library module can be reloaded between frames: the host keeps instance
 * data when its shape is unchanged and keeps superseded libraries loaded
 * until the session stops.
 */
#pragma once

#include "animation/vkr_animation_player.h"
#include "core/input.h"
#include "core/vkr_job_system.h"
#include "core/vkr_type_desc.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_dmemory.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_system.h"
#include "script/vkr_io_router.h"
#include "sdk.h"

#define VKR_SCRIPT_MODULE_NAME_CAPACITY 64u
#define VKR_SCRIPT_RETIRED_LIBRARY_MAX 32u
#define VKR_SCRIPT_PATH_CAPACITY 1024u
/* Libraries loaded at once: project libraries and single-module ones. */
#define VKR_SCRIPT_LIBRARY_MAX 8u
#define VKR_SCRIPT_LIBRARY_NONE UINT32_MAX
/* Modules one prepared library may list. */
#define VKR_SCRIPT_PREPARED_MODULE_MAX 256u
/* The World and the active container. */
#define VKR_SCRIPT_CONTAINER_MAX 2u
#define VKR_SCRIPT_STATE_TYPE_MAX 64u
#define VKR_SCRIPT_HUD_CAPACITY 512u
#define VKR_SCRIPT_ERROR_CAPACITY 256u

typedef struct VkrScriptInstance VkrScriptInstance;
struct ScriptCommand;
struct ScriptTimed;
struct ScriptOwned;

typedef struct VkrScriptModule {
  const VkrModuleDesc *desc;
  char name[VKR_SCRIPT_MODULE_NAME_CAPACITY];
  /* Host-owned copies of the module's component types; scenes and editors
     hold these pointers, so they outlive every library generation. */
  VkrTypeDesc *types[VKR_SDK_EXPORT_MAX];
  uint32_t type_count;
  /* The library holding its code; VKR_SCRIPT_LIBRARY_NONE when linked into
     the executable. */
  uint32_t library;
  bool8_t dynamic;
  /* A retired module keeps its registered types and receives no calls. */
  bool8_t retired;
  /* Counts successful loads and reloads. */
  uint32_t generation;
} VkrScriptModule;

/* A loaded shared library: one project library holding many modules, or
 * one module's own library. Its modules swap together on reload. */
typedef struct VkrScriptLibrary {
  char name[VKR_SCRIPT_MODULE_NAME_CAPACITY];
  /* The loaded copy of the build; no handle once retired. */
  VkrPlatformLibrary handle;
  char loaded_path[VKR_SCRIPT_PATH_CAPACITY];
  /* Lists its modules through vkr_project_modules. */
  bool8_t project;
  /* Counts successful loads and reloads. */
  uint32_t generation;
} VkrScriptLibrary;

/* A library opened ahead of its load, so copying and opening it, which runs
 * its C runtime startup, stays off the frame thread.
 * `vkr_script_host_prepare` names it on the frame thread,
 * `vkr_script_prepare_run` copies, opens and lists it on any thread without
 * touching the host, and `vkr_script_host_commit` applies it between frames
 * or `vkr_script_host_discard` closes it. Caller-owned; the host keeps no
 * pointer to it. */
typedef struct VkrScriptPrepared {
  char name[VKR_SCRIPT_MODULE_NAME_CAPACITY];
  char path[VKR_SCRIPT_PATH_CAPACITY];
  /* The byte copy that is opened; removed when the library closes. Empty
     when the library opens in place. */
  char loaded_path[VKR_SCRIPT_PATH_CAPACITY];
  VkrPlatformLibrary handle;
  /* Lists its modules through vkr_project_modules. */
  bool8_t project;
  /* Set by the run: the library is open and lists `modules`; otherwise
     `error` says why. */
  bool8_t ready;
  uint32_t count;
  const VkrModuleDesc *modules[VKR_SCRIPT_PREPARED_MODULE_MAX];
  char error[VKR_SCRIPT_ERROR_CAPACITY];
} VkrScriptPrepared;

/* Moves the live values of a registered component type whose fields a reload
 * changed, from its `previous` descriptor to the current one: in every scene
 * and every place that holds its bytes (ADR-079). The application owns those,
 * so it installs this; without one, a changed layout is refused. */
typedef void (*VkrScriptMigrateFn)(void *context, const VkrTypeDesc *type,
                                   const VkrTypeDesc *previous);

typedef struct VkrScriptRetiredLibrary {
  VkrPlatformLibrary library;
  char path[VKR_SCRIPT_PATH_CAPACITY];
} VkrScriptRetiredLibrary;

typedef enum VkrScriptReload {
  VKR_SCRIPT_RELOAD_FAILED = 0,
  /* A module new to the host registered. */
  VKR_SCRIPT_RELOAD_LOADED,
  /* The new code runs with the kept data, or no session was running. */
  VKR_SCRIPT_RELOAD_KEPT_STATE,
  /* The data shape changed: the session restarted with the new code. */
  VKR_SCRIPT_RELOAD_RESTARTED,
} VkrScriptReload;

typedef enum VkrScriptPhase {
  VKR_SCRIPT_PHASE_IDLE = 0,
  /* start, stop, update and late_update: structural edits allowed. */
  VKR_SCRIPT_PHASE_FRAME,
  /* fixed_update and late_fixed_update: structural edits refused. */
  VKR_SCRIPT_PHASE_TICK,
} VkrScriptPhase;

/** Variable-rate frame state the shell passes in. */
typedef struct VkrScriptFrame {
  /** Monotonic seconds measured after the input pump. */
  float64_t now;
  /** Elapsed time the scene advances this frame; a module that owns the
   * input clock may replace it. */
  float64_t scene_delta;
  /** Scene keyboard and mouse belong to gameplay. */
  bool8_t input_focused;
  bool8_t simulation_running;
  /** The Scene shows its perspective camera, which scripts may drive. */
  bool8_t camera_available;
} VkrScriptFrame;

/** Presentation the modules published after the scene advanced. */
typedef struct VkrScriptView {
  bool8_t camera_valid;
  Vec3 camera_position;
  float32_t camera_yaw_degrees;
  float32_t camera_pitch_degrees;
  /** Overlay text; empty for none. */
  char hud[VKR_SCRIPT_HUD_CAPACITY];
} VkrScriptView;

/** One attached container. */
typedef struct VkrScriptContainer {
  VkrScene *scene;
  /* The entity world id of the container's scene (ADR-076). */
  uint32_t world_id;
} VkrScriptContainer;

/** What a session runs on. */
typedef struct VkrScriptSessionDesc {
  /** The played container; its clock drives every tick. */
  VkrScene *active;
  /** The root World, or NULL or `active` when it plays alone. */
  VkrScene *world;
  InputState *input;
  struct VkrRenderAssets *assets;
  /** Workers for script tasks; NULL runs each task when it is created. */
  VkrJobSystem *jobs;
  /** The application asked for the sample gameplay content (`--gameplay`). */
  bool8_t sample_content;
} VkrScriptSessionDesc;

/** A runtime-only per-entity state type registered by name. */
typedef struct VkrScriptStateType {
  char name[VKR_SCRIPT_MODULE_NAME_CAPACITY];
  uint32_t size;
  uint32_t align;
  /* ECS component per attached container slot, registered on first use. */
  VkrComponentTypeId ids[VKR_SCRIPT_CONTAINER_MAX];
} VkrScriptStateType;

#define VKR_SCRIPT_BOUND_ANIMATION_MAX 4u

typedef struct VkrScriptBoundAnimation {
  uint64_t entity;
  VkrAnimationPlayer *player;
} VkrScriptBoundAnimation;

typedef struct VkrScriptHost {
  VkrSdkTable table;
  /* Process-lifetime storage for type copies and names. */
  VkrAllocator *allocator;
  /* Instance data, ledgers and behavior bindings, freed as they end. */
  VkrDMemory memory;
  VkrAllocator instance_allocator;
  /* Temp memory of the running hook, rewound after it returns. */
  Arena *temp;
  /* Registered modules, grown in `allocator`; an index stays valid, a
     pointer until the next registration. */
  VkrScriptModule *modules;
  uint32_t module_count;
  uint32_t module_capacity;
  VkrScriptLibrary libraries[VKR_SCRIPT_LIBRARY_MAX];
  uint32_t library_count;

  /* Session. */
  bool8_t started;
  VkrScriptSessionDesc session;
  VkrScriptContainer containers[VKR_SCRIPT_CONTAINER_MAX];
  uint32_t container_count;
  /* Index of the active container and of the World's. */
  uint32_t active_container;
  uint32_t world_container;
  /* Running instances in start order, grown in `instance_allocator`. */
  VkrScriptInstance **instances;
  uint32_t instance_count;
  uint32_t instance_capacity;
  /* Installed as the active scene's simulation callbacks and input observer. */
  bool8_t callbacks_installed;
  bool8_t observing_input;
  /* A native reset asked for a restart at the next frame boundary. */
  bool8_t restart_pending;
  /* A hook failed outside a tick; hooks stop until the session restarts. */
  bool8_t faulted;
  VkrScriptPhase phase;
  VkrScriptFrame frame;
  /* Update and late_update run: vkr_time reads the frame's time. */
  bool8_t frame_time_valid;
  VkrScriptView view;
  bool8_t time_step_set;
  float64_t time_step;
  uint64_t frame_serial;

  /* Containers whose transforms changed since their last update. */
  bool8_t transforms_dirty[VKR_SCRIPT_CONTAINER_MAX];

  VkrScriptStateType state_types[VKR_SCRIPT_STATE_TYPE_MAX];
  uint32_t state_type_count;
  /* Host-owned descriptors vkr_component_named returns, bound to the
     registered types. */
  VkrComponentDesc named_types[VKR_SCRIPT_STATE_TYPE_MAX];
  uint32_t named_type_count;
  /* Structural edits queued in fixed updates, applied right after the tick
     in order; their strings and values live in `command_arena`. */
  struct ScriptCommand *commands;
  uint32_t command_count;
  uint32_t command_capacity;
  Arena *command_arena;
  /* Reserved IDs of queued spawns not yet created. */
  uint64_t *pending;
  uint32_t pending_count;
  uint32_t pending_capacity;
  /* The reservation the replayed spawn creates, or zero. */
  uint64_t replay_reserved;
  /* Spawns that end after a simulated duration or with an owner entity. */
  struct ScriptTimed *timed;
  uint32_t timed_count;
  uint32_t timed_capacity;
  struct ScriptOwned *owned;
  uint32_t owned_count;
  uint32_t owned_capacity;

  /* The tool context of vkr_script_host_open_context, or NULL. */
  VkrScriptInstance *tool;
  /* Players tools bound to entities in place of the scene's. */
  VkrScriptBoundAnimation bound_animations[VKR_SCRIPT_BOUND_ANIMATION_MAX];
  uint32_t bound_animation_count;

  /* Libraries replaced while a session ran; closed when it stops. */
  VkrScriptRetiredLibrary retired[VKR_SCRIPT_RETIRED_LIBRARY_MAX];
  uint32_t retired_count;
  /* Script tasks not yet freed, in `instance_allocator`; a released one
     waits here until its worker job lets go of it. */
  struct ScriptTask **tasks;
  uint32_t task_count;
  uint32_t task_capacity;
  uint64_t task_serial;
  /* Routes entity IO for the session (vkr_io_router.h). */
  VkrIoRouter io;
  /* Moves component values to a reloaded layout; NULL refuses the reload. */
  VkrScriptMigrateFn migrate;
  void *migrate_context;
  uint32_t load_serial;
  char error[VKR_SCRIPT_ERROR_CAPACITY];
} VkrScriptHost;

/** Fills the SDK table. `allocator` backs type copies and names; registered
 * scene types have no removal, so it must live for the process. Keep the
 * host at a stable address: contexts, the scene and the input borrow it. */
bool8_t vkr_script_host_init(VkrScriptHost *host, VkrAllocator *allocator);

/** Stops the session, closes every library and removes their loaded copies,
 * and frees instance storage. */
void vkr_script_host_shutdown(VkrScriptHost *host);

/** Validates a linked module's description and registers copies of its
 * component types as scene world types. Call before any scene initializes. */
bool8_t vkr_script_host_add_module(VkrScriptHost *host, VkrModuleEntry entry,
                                   const char **error);

/**
 * Loads module `name` from the shared library `library_path` through its
 * `vkr_module_<name>` entry, copying the file first so the build can be
 * replaced. A new module registers its component types, which must come
 * before any scene that uses them initializes. A module already known by
 * that name reloads: its component types must keep their layout, and the
 * running data stays when its shape is unchanged. Call between frames,
 * never from a hook. A failure keeps the previous code running.
 */
VkrScriptReload vkr_script_host_load_library(VkrScriptHost *host,
                                             const char *name,
                                             const char *library_path,
                                             const char **error);

/**
 * Loads project library `name` from `library_path`: every module its
 * `vkr_project_modules` entry lists (sdk.h). The reload is atomic: when one
 * module would be refused (a changed component layout, a type name another
 * module owns), every module keeps its previous code. Otherwise modules new
 * to the library register, known ones swap code, and modules it no longer
 * lists retire. A running session keeps its data when every module keeps
 * its data shape and none was added or removed, and restarts otherwise. Call
 * between frames, never from a hook.
 */
VkrScriptReload vkr_script_host_load_project(VkrScriptHost *host,
                                             const char *name,
                                             const char *library_path,
                                             const char **error);

/**
 * Names a library to prepare: `vkr_script_host_load_library` or
 * `_load_project` split so the copy and open can run on a worker. Picks the
 * copy's unique path, or with `in_place` opens `library_path` itself, as a
 * packaged game does: its folder may be read-only or signed, and it never
 * reloads. False with `prepared->error` when the name or path is unusable.
 * Frame thread.
 */
bool8_t vkr_script_host_prepare(VkrScriptHost *host,
                                VkrScriptPrepared *prepared, const char *name,
                                const char *library_path, bool8_t project,
                                bool8_t in_place);

/** Copies, opens and lists a prepared library. Any thread; reads nothing of
 * the host, so it may run while frames and hooks continue. */
void vkr_script_prepare_run(VkrScriptPrepared *prepared);

/** Applies a prepared library as the load functions do and takes its
 * handle; a failed run or a refused load closes it. Between frames. */
VkrScriptReload vkr_script_host_commit(VkrScriptHost *host,
                                       VkrScriptPrepared *prepared,
                                       const char **error);

/** Closes a prepared library that will not be committed and removes its
 * copy. Its run must have returned. */
void vkr_script_host_discard(VkrScriptHost *host, VkrScriptPrepared *prepared);

/** Lets reloads change a component's fields: before a session restarts or
 * continues, `migrate` moves every live value of each changed type. */
void vkr_script_host_set_migrator(VkrScriptHost *host,
                                  VkrScriptMigrateFn migrate, void *context);

/** Retires every library module, as when a project closes: the session
 * stops, their component types stay registered without hooks, and a later
 * load of the same name adopts them. */
void vkr_script_host_retire_libraries(VkrScriptHost *host);

/** Module by name, or NULL. */
const VkrScriptModule *vkr_script_host_module(const VkrScriptHost *host,
                                              const char *name);

/** Pauses the active scene, resets a simulation that already advanced and
 * starts every module's instances and behaviors. Returns false with
 * `*error` when one fails; nothing stays started then. */
bool8_t vkr_script_host_start(VkrScriptHost *host,
                              const VkrScriptSessionDesc *desc,
                              const char **error);

/** Pauses the active scene, detaches the host, ends every instance in
 * reverse order and closes superseded libraries. The caller resets the
 * scene's simulation afterwards to restore bodies. */
void vkr_script_host_stop(VkrScriptHost *host);

/** Ends the instances on `scene` before the container unloads. Detaching
 * the active container stops the session. */
void vkr_script_host_detach(VkrScriptHost *host, const VkrScene *scene);

/** Started with an enabled instance that runs module hooks or behaviors. */
bool8_t vkr_script_host_active(const VkrScriptHost *host);

/** Before the scene advances: applies a pending restart, starts and stops
 * behaviors whose entities gained or lost their component, and runs update
 * hooks. `frame->scene_delta` may change. */
void vkr_script_host_frame(VkrScriptHost *host, VkrScriptFrame *frame);

/** After the scene advanced: runs late_update hooks and returns the camera
 * and overlay they published. */
void vkr_script_host_present(VkrScriptHost *host, const VkrScriptFrame *frame,
                             VkrScriptView *view);

/** Prints each IO delivery as an `[io]` log line while on. */
void vkr_script_host_set_io_trace(VkrScriptHost *host, bool8_t trace);

/** Delivers input `input` (a port of `type`, or a built-in input without
 * one) to `target` during a session, as Cmd and agents do. */
bool8_t vkr_script_host_io_send(VkrScriptHost *host, VkrEntityId target,
                                VkrIoEndpoint input, const VkrIoValue *value);

/** The failure that faulted or refused the session, or NULL. */
const char *vkr_script_host_error(const VkrScriptHost *host);

/** A module instance's data on `scene`, for tools and tests; NULL when the
 * module has no live instance there. */
void *vkr_script_host_instance_data(VkrScriptHost *host, const char *module,
                                    const VkrScene *scene);

/** A context bound to `scene` outside any module, for tools and tests. Its
 * acquisitions are released by vkr_script_host_close_context. One at a time;
 * the host must not be started. */
VkrCtx *vkr_script_host_open_context(VkrScriptHost *host, VkrScene *scene,
                                     InputState *input,
                                     struct VkrRenderAssets *assets);
void vkr_script_host_close_context(VkrScriptHost *host);

/** Makes the tool context's animation calls on `entity` drive `player`, a
 * player the caller owns, for tests of animation scripts without a cooked
 * model; a later call for the entity replaces it. Cleared by
 * vkr_script_host_close_context. */
bool8_t vkr_script_host_bind_animation(VkrScriptHost *host, VkrEntityId entity,
                                       VkrAnimationPlayer *player);
