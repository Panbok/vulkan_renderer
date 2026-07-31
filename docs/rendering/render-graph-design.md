---
status: partial
updated: 2026-07-31
authority: design
---
# Render Graph System Design

## Purpose

The render graph replaced the removed `VkrViewSystem`/`VkrLayer` orchestration
path. JSON drives the declared pass order and execution, while stateless
resource owners (world/UI/skybox systems) back pass executors.

**Render graphs are defined in JSON files**. The application loads the JSON at
startup and builds the graph from that data. Hot reload is not implemented.
Graph topology is not hardcoded in C; C only supplies the executor registry and
runtime parameters.

Current status is **partial**: graph scheduling, culling, resource realization,
timing, fallible execution, and access/subresource-aware synchronization for
declared resources ship. Picking and IBL still perform undeclared GPU work, and
queue ownership and buffer byte ranges are not modeled. The architecture spec
§3.3 and §8 is the current status authority.

---

## Requirements from the Current Renderer (Parity Targets)

The renderer's existing pass relationships and behaviors (formerly expressed by the view modules) must be preserved by the graph:

### Required Passes and Dependencies

| Pass | Outputs | Inputs | Notes |
|------|---------|--------|-------|
| Shadow Cascades (N) | `shadow_map[i]` (depth) | scene geometry | One pass per cascade, depth-only targets, sampled by World pass |
| Skybox | `scene_color`, `scene_depth` | skybox cube map | Clears color + depth, renders first |
| World | `scene_color`, `scene_depth` | shadow maps, materials, meshes | Loads color/depth from Skybox, draws opaque/transparent/overlay |
| UI | `scene_color` | fonts/textures | Loads color, draws 2D overlay |
| Editor Composite (optional) | `swapchain_color` | `scene_color` | Only when editor is enabled; draws scene texture into editor viewport |
| Present | swapchain image | swapchain image | Transition to `PRESENT_SRC_KHR` before submit |

### Offscreen Mode (Editor)

When the editor is enabled:

- `Skybox`, `World`, and `UI` render into offscreen textures sized to the editor viewport.
- `Editor Composite` reads the offscreen color and writes to the swapchain.
- Offscreen size may **not** match window size (viewport scale and fit mode).

### Implications for the Render Graph

- Pass ordering must be driven by **resource dependencies**, not manual layer order.
- Passes that render to the same color/depth targets must support **load-op = LOAD** to preserve previous results.
- Per-swapchain-image resources are required (swapchain color, swapchain depth, per-image render targets).
- The graph must support **conditional pass sets** (editor on/off) and automatically handle culling when passes or outputs are absent.

---

## Goals

1. **Full replacement** of the removed view/layer orchestration and messaging.
2. **Declarative pass definition**: passes declare resources they read/write.
3. **Automatic synchronization**: barriers and layout transitions are generated.
4. **Resource lifetime management**: transient resources are graph-owned and reused.
5. **Clear dataflow**: pass order derived from dependencies, not manual ordering.
6. **Debuggability**: graph can be exported (DOT) and inspected.

### Non-Goals (Initial Implementation)

- Multi-queue execution (graphics/compute/transfer split)
- Renderpass merging/subpass optimization
- Automatic descriptor set management
- Timeline semaphore orchestration
- Virtual resources with deferred allocation

---

## Render Graph Definition (JSON, Required)

### File Location

- Graphs live under `assets/render_graphs/`.
- The primary graph is `assets/render_graphs/main.rendergraph.json`.
- The app loads the JSON at startup. File-change hot reload is future work.

### JSON Parsing and Validation

- Parsing uses the existing reader in `lib/src/core/vkr_json.h`.
- The loader produces an intermediate `VkrRgJsonGraph` and then builds the
  runtime graph via the internal builder API.
- Invalid graphs fail fast with a precise error (field path + reason).

### Pass Executor Registry

JSON references pass executors by name. These are registered in C:

```c
typedef struct VkrRgPassExecutor {
  String8 name;
  VkrRgPassExecuteFn execute;
  void *user_data;
} VkrRgPassExecutor;

typedef struct VkrRgExecutorRegistry VkrRgExecutorRegistry;

bool8_t vkr_rg_executor_registry_init(VkrRgExecutorRegistry *reg,
                                      VkrAllocator *allocator);
bool8_t vkr_rg_executor_registry_register(VkrRgExecutorRegistry *reg,
                                          const VkrRgPassExecutor *entry);
VkrRgPassExecuteFn
vkr_rg_executor_registry_find(const VkrRgExecutorRegistry *reg, String8 name,
                              void **out_user_data);
```

### Supported JSON Features (v1)

- **Resources**: images/buffers, with flags and usage.
- **Imports**: `swapchain`, `swapchain_depth` (external resources).
- **Passes**: graphics/compute/transfer with attachments and reads/writes.
- **Conditions**: enable/disable resources or passes based on runtime booleans.
- **Repeats**: expand resources/passes by count (e.g., shadow cascades).
- **Name templating**: `${i}` for repeat index.
- **Runtime sizes**: `window`, `viewport`, or `square` with `*_source` values.
- **Format tokens**: `SWAPCHAIN` resolves to current swapchain format.
- **Usage strings**: map to `VKR_TEXTURE_USAGE_*` and buffer usage flags.

### Top-Level Structure (v1)

```json
{
  "version": 1,
  "name": "main",
  "resources": [ ... ],
  "passes": [ ... ],
  "outputs": {
    "present": "swapchain",
    "export_images": [ "scene_color" ],
    "export_buffers": [ "gpu_readback" ]
  }
}
```

`outputs.export_*` mark resources as externally visible and prevent pass culling.

### Resource Entry (image)

```json
{
  "name": "scene_color",
  "type": "image",
  "condition": "editor_enabled",
  "flags": [ "TRANSIENT", "RESIZABLE", "PER_IMAGE" ],
  "extent": { "mode": "viewport" },
  "format": "SWAPCHAIN",
  "usage": [ "COLOR_ATTACHMENT", "SAMPLED" ]
}
```

### Extent Modes

- `{ "mode": "window" }` -> `window_width` / `window_height`
- `{ "mode": "viewport" }` -> `viewport_width` / `viewport_height`
- `{ "mode": "fixed", "width": 1024, "height": 1024 }`
- `{ "mode": "square", "size_source": "shadow_map_size" }`

### Import Tokens

- `swapchain` -> current swapchain image (per-image)
- `swapchain_depth` -> renderer-managed depth attachment (per-image)

### Resource Flags (strings)

- `TRANSIENT`, `PERSISTENT`, `EXTERNAL`, `PER_IMAGE`, `RESIZABLE`

### Pass Entry (graphics)

```json
{
  "name": "World",
  "type": "graphics",
  "domain": "WORLD",
  "reads": [
    { "image": "shadow_map_${i}", "access": "SAMPLED",
      "binding": 0, "array_index": "${i}",
      "repeat": { "count_source": "shadow_cascade_count" } }
  ],
  "attachments": {
    "color": [
      { "image": "swapchain", "load": "LOAD", "store": "STORE",
        "clear": { "color": [0, 0, 0, 1] } }
    ],
    "depth": {
      "image": "swapchain_depth", "load": "LOAD", "store": "STORE",
      "clear": { "depth": 1.0, "stencil": 0 }
    }
  },
  "execute": "pass.world"
}
```

Notes:
- `domain` accepts names matching `VKR_PIPELINE_DOMAIN_*` without the prefix
  (e.g., `WORLD`, `UI`, `SHADOW`, `SKYBOX`).
- `repeat` can appear on resources, passes, or individual read/write entries.
- `${i}` expands to the repeat index.
- `access` strings map to `VkrRgImageAccessFlags` / `VkrRgBufferAccessFlags`
  (e.g., `SAMPLED`, `COLOR_ATTACHMENT`, `TRANSFER_DST`).
- `format` strings map to `VKR_TEXTURE_FORMAT_*` without the prefix
  (e.g., `R8G8B8A8_SRGB`, `D32_SFLOAT`) or `SWAPCHAIN`.
- `usage` strings map to `VKR_TEXTURE_USAGE_*` without the prefix
  (e.g., `SAMPLED`, `COLOR_ATTACHMENT`, `DEPTH_STENCIL_ATTACHMENT`).

### Runtime Parameter Sources

These symbols are available to the JSON loader:

- `window_width`, `window_height`
- `viewport_width`, `viewport_height`
- `swapchain_format`
- `shadow_map_size`
- `shadow_cascade_count`
- `editor_enabled`

### Condition Syntax

Conditions are simple boolean expressions:

- `editor_enabled`
- `!editor_enabled`

No arbitrary expressions or numeric comparisons in v1.

---

## High-Level Flow

```
0) Load JSON (startup / hot reload)
   - Parse graph JSON into intermediate representation
   - Validate fields and names

1) Build (per frame, CPU)
   - Resolve JSON tokens using runtime parameters
   - Define resources (images/buffers)
   - Define passes and dependencies
   - Mark present output

2) Compile (CPU, each graph realization; physical objects are cached)
   - Validate
   - Build dependency edges
   - Cull unused passes
   - Topological sort
   - Allocate/resolve resources
   - Generate barriers
   - Create render passes/targets

3) Execute (per frame, GPU command recording)
   - Insert pre-pass barriers
   - Begin renderpass / execute / end
   - Transition present image

4) Reset (per frame)
   - Clear frame-local arrays
```

---

## Core Data Model

### Handles

```c
typedef struct VkrRgImageHandle {
  uint32_t id;
  uint32_t generation;
} VkrRgImageHandle;

#define VKR_RG_IMAGE_HANDLE_INVALID ((VkrRgImageHandle){0, 0})

static inline bool8_t vkr_rg_image_handle_valid(VkrRgImageHandle h) {
  return h.id != 0;
}

typedef struct VkrRgBufferHandle {
  uint32_t id;
  uint32_t generation;
} VkrRgBufferHandle;

#define VKR_RG_BUFFER_HANDLE_INVALID ((VkrRgBufferHandle){0, 0})
```

### Resource Flags

```c
typedef enum VkrRgResourceFlags {
  VKR_RG_RESOURCE_FLAG_NONE = 0,
  VKR_RG_RESOURCE_FLAG_TRANSIENT = 1 << 0,   // graph-owned, can be reused
  VKR_RG_RESOURCE_FLAG_PERSISTENT = 1 << 1,  // graph-owned, kept across frames
  VKR_RG_RESOURCE_FLAG_EXTERNAL = 1 << 2,    // imported, not destroyed by graph
  VKR_RG_RESOURCE_FLAG_PER_IMAGE = 1 << 3,   // one backing per swapchain image
  VKR_RG_RESOURCE_FLAG_RESIZABLE = 1 << 4,   // recreate when desc changes
} VkrRgResourceFlags;
```

### Image Description

```c
typedef struct VkrRgImageDesc {
  uint32_t width;
  uint32_t height;
  VkrTextureFormat format;
  VkrTextureUsageFlags usage;
  VkrSampleCount samples;
  uint32_t layers;
  uint32_t mip_levels;
  VkrTextureType type;
  VkrRgResourceFlags flags;
} VkrRgImageDesc;

#define VKR_RG_IMAGE_DESC_DEFAULT ((VkrRgImageDesc){ \
  .width = 0,                                        \
  .height = 0,                                       \
  .format = VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB,        \
  .usage = vkr_texture_usage_flags_create(),         \
  .samples = VKR_SAMPLE_COUNT_1,                     \
  .layers = 1,                                       \
  .mip_levels = 1,                                   \
  .type = VKR_TEXTURE_TYPE_2D,                       \
  .flags = VKR_RG_RESOURCE_FLAG_TRANSIENT,           \
})
```

`VkrTextureUsageFlags` is a bitset; build it with
`vkr_texture_usage_flags_from_bits(...)` or `bitset8_set(...)`.

### Buffer Description

```c
typedef struct VkrRgBufferDesc {
  uint64_t size;
  VkrBufferUsageFlags usage;
  VkrRgResourceFlags flags;
} VkrRgBufferDesc;
```

### Image Subresource (for render targets)

```c
typedef struct VkrRgImageSlice {
  uint32_t mip_level;
  uint32_t base_layer;
  uint32_t layer_count;
} VkrRgImageSlice;

#define VKR_RG_IMAGE_SLICE_DEFAULT ((VkrRgImageSlice){ \
  .mip_level = 0,                                      \
  .base_layer = 0,                                     \
  .layer_count = 1,                                    \
})
```

### Pass Types and Flags

```c
typedef enum VkrRgPassType {
  VKR_RG_PASS_TYPE_GRAPHICS = 0,
  VKR_RG_PASS_TYPE_COMPUTE = 1,
  VKR_RG_PASS_TYPE_TRANSFER = 2,
} VkrRgPassType;

typedef enum VkrRgPassFlags {
  VKR_RG_PASS_FLAG_NONE = 0,
  VKR_RG_PASS_FLAG_NO_CULL = 1 << 0,   // never culled
  VKR_RG_PASS_FLAG_DISABLED = 1 << 1,  // skip entirely
} VkrRgPassFlags;
```

### Resource Uses (for barriers and dependencies)

```c
typedef VkrImageAccessFlags VkrRgImageAccessFlags;

typedef struct VkrRgImageUse {
  VkrRgImageHandle image;
  VkrRgImageAccessFlags access;
  uint32_t binding;     // descriptor binding (optional)
  uint32_t array_index; // descriptor array index (optional)
} VkrRgImageUse;

Array(VkrRgImageUse);

typedef VkrBufferAccessFlags VkrRgBufferAccessFlags;

typedef struct VkrRgBufferUse {
  VkrRgBufferHandle buffer;
  VkrRgBufferAccessFlags access;
  uint32_t binding;     // descriptor binding (optional)
  uint32_t array_index; // descriptor array index (optional)
} VkrRgBufferUse;

Array(VkrRgBufferUse);
```

### Attachments (graphics passes)

```c
typedef struct VkrRgAttachmentDesc {
  VkrRgImageSlice slice;
  VkrAttachmentLoadOp load_op;
  VkrAttachmentStoreOp store_op;
  VkrClearValue clear_value;
} VkrRgAttachmentDesc;
```

`load_op = VKR_ATTACHMENT_LOAD_OP_LOAD` is treated as **read + write** for dependency/barrier analysis. This is critical for UI/overlay passes that preserve existing color.

### Pass Description

```c
typedef struct VkrRgPassContext VkrRgPassContext;

typedef void (*VkrRgPassExecuteFn)(VkrRgPassContext *ctx, void *user_data);

typedef struct VkrRgPassDesc {
  String8 name;
  VkrRgPassType type;
  VkrRgPassFlags flags;

  // Graphics-only
  VkrPipelineDomain domain;
  Array_VkrRgAttachmentDesc color_attachments;
  bool8_t has_depth_attachment;
  VkrRgAttachmentDesc depth_attachment;

  // Resource reads (sampled/storage)
  Array_VkrRgImageUse image_reads;
  Array_VkrRgImageUse image_writes; // storage images, transfer dst

  // Buffers (optional, for barriers)
  Array_VkrRgBufferUse buffer_reads;
  Array_VkrRgBufferUse buffer_writes;

  VkrRgPassExecuteFn execute;
  void *user_data;
} VkrRgPassDesc;
```

### Pass Context

```c
typedef struct VkrRgPassContext {
  struct VkrRenderGraph *graph;
  const VkrRgPassDesc *pass_desc;
  uint32_t pass_index;

  struct s_RendererFrontend *renderer;
  VkrRenderPassHandle renderpass;      // graphics-only
  VkrRenderTargetHandle render_target; // graphics-only

  uint32_t frame_index;
  uint32_t image_index;
  float64_t delta_time;
} VkrRgPassContext;
```

---

## Internal Builder API (Used by JSON Loader)

The builder API is **not** the primary user surface. It is used by the JSON
loader and tests. The application only provides the JSON file and pass
executors.

```c
typedef struct VkrRenderGraph VkrRenderGraph;

typedef struct VkrRgPassBuilder {
  VkrRenderGraph *graph;
  uint32_t pass_index;
} VkrRgPassBuilder;

typedef struct VkrRenderGraphFrameInfo {
  uint32_t frame_index;
  uint32_t image_index;
  float64_t delta_time;
  uint32_t window_width;
  uint32_t window_height;
  uint32_t viewport_width;   // editor viewport, or window size when editor off
  uint32_t viewport_height;
  bool8_t editor_enabled;
} VkrRenderGraphFrameInfo;

// Lifecycle
VkrRenderGraph *vkr_rg_create(VkrAllocator *allocator);
void vkr_rg_destroy(VkrRenderGraph *graph);

void vkr_rg_begin_frame(VkrRenderGraph *graph,
                        const VkrRenderGraphFrameInfo *frame);
void vkr_rg_end_frame(VkrRenderGraph *graph);

// Resources (name-based, stable across frames)
VkrRgImageHandle vkr_rg_create_image(VkrRenderGraph *graph, String8 name,
                                     const VkrRgImageDesc *desc);
VkrRgImageHandle vkr_rg_import_image(VkrRenderGraph *graph, String8 name,
                                     VkrTextureOpaqueHandle handle,
                                     VkrRgImageAccessFlags current_access,
                                     VkrTextureLayout current_layout,
                                     const VkrRgImageDesc *desc);
VkrRgImageHandle vkr_rg_import_swapchain(VkrRenderGraph *graph);
VkrRgImageHandle vkr_rg_import_depth(VkrRenderGraph *graph);

// Resource names are stable keys. Store them in module state or use literals;
// do not allocate new formatted names every frame.

// Pass creation
VkrRgPassBuilder vkr_rg_add_pass(VkrRenderGraph *graph, VkrRgPassType type,
                                 String8 name);
void vkr_rg_pass_set_execute(VkrRgPassBuilder *pb,
                             VkrRgPassExecuteFn execute,
                             void *user_data);

// Attachments
void vkr_rg_pass_add_color_attachment(VkrRgPassBuilder *pb,
                                      VkrRgImageHandle image,
                                      const VkrRgAttachmentDesc *desc);
void vkr_rg_pass_set_depth_attachment(VkrRgPassBuilder *pb,
                                      VkrRgImageHandle image,
                                      const VkrRgAttachmentDesc *desc,
                                      bool8_t read_only);

// Resource uses
void vkr_rg_pass_read_image(VkrRgPassBuilder *pb, VkrRgImageHandle image,
                            VkrRgImageAccessFlags access, uint32_t binding,
                            uint32_t array_index);
void vkr_rg_pass_write_image(VkrRgPassBuilder *pb, VkrRgImageHandle image,
                             VkrRgImageAccessFlags access, uint32_t binding,
                             uint32_t array_index);

void vkr_rg_pass_read_buffer(VkrRgPassBuilder *pb, VkrRgBufferHandle buffer,
                             VkrRgBufferAccessFlags access, uint32_t binding,
                             uint32_t array_index);
void vkr_rg_pass_write_buffer(VkrRgPassBuilder *pb, VkrRgBufferHandle buffer,
                              VkrRgBufferAccessFlags access, uint32_t binding,
                              uint32_t array_index);

// Output
void vkr_rg_set_present_image(VkrRenderGraph *graph, VkrRgImageHandle image);
void vkr_rg_export_image(VkrRenderGraph *graph, VkrRgImageHandle image);
void vkr_rg_export_buffer(VkrRenderGraph *graph, VkrRgBufferHandle buffer);

// Compile + Execute
bool8_t vkr_rg_compile(VkrRenderGraph *graph);
VkrRendererError vkr_rg_execute(VkrRenderGraph *graph,
                                struct s_RendererFrontend *rf);
```

Notes:
- `vkr_rg_begin_frame` clears the **logical graph** for the frame but preserves cached physical resources.
- Resources are keyed by **name**. If a desc changes, resources marked `RESIZABLE` are recreated.
- Imported resources are never destroyed by the graph.

---

## JSON Loader API

```c
typedef struct VkrRgJsonGraph VkrRgJsonGraph;

bool8_t vkr_rg_json_load_file(VkrAllocator *allocator, const char *path,
                              VkrRgJsonGraph *out_graph);
void vkr_rg_json_destroy(VkrAllocator *allocator, VkrRgJsonGraph *graph);

bool8_t vkr_rg_build_from_json(VkrRenderGraph *rg,
                               const VkrRgJsonGraph *json_graph,
                               const VkrRenderGraphFrameInfo *frame,
                               const VkrRgExecutorRegistry *executors);
```

Notes:
- The JSON graph is cached and only reloaded when the file changes.
- `vkr_rg_build_from_json` expands `repeat` blocks and resolves runtime tokens.
- Missing executors are fatal validation errors (no silent fallback).
- The graph is recompiled when the JSON changes or when resolved resource
  descriptors differ (resize, cascade count, etc.).

---

## Allocator and Lifetime Rules

- **Graph metadata** (passes, resources, edges) must be allocated from a
  **freeable allocator** (`VkrDMemory` or pool-backed `VkrAllocator`), not a
  long-lived arena. The graph is rebuilt every frame.
- **Compile scratch** allocations must use `vkr_allocator_begin_scope()` /
  `vkr_allocator_end_scope()` to avoid arena growth.
- **Resource names** must be persistent (literals or stored in module state).
- **Imported resources** are never destroyed by the graph. Graph-owned
  resources are destroyed on graph shutdown or when resized.
- JSON graphs should be allocated from a freeable allocator and destroyed on
  reload to avoid arena growth.

---

## Compile Phase Details

### 1) Validation

- All handles referenced by passes must be valid and declared.
- Graphics passes must have at least one attachment (color or depth).
- A present image must be set if the swapchain is used.
- JSON-specific validation:
  - unknown executor names
  - unresolved `repeat` sources or conditions
  - invalid enum strings (format/usage/domain/access)

### 2) Dependency Edge Construction

Use a single-pass, per-resource tracking strategy to build a DAG without O(passes^2) scanning.

```c
// Per resource state tracked during pass declaration
typedef struct VkrRgDependencyState {
  int32_t last_writer;              // pass index, -1 if none
  Array_int32_t last_readers;       // passes that read since last write
} VkrRgDependencyState;

for each pass P in declared order:
  for each read of resource R:
    if last_writer[R] >= 0: add edge last_writer[R] -> P
    add P to last_readers[R]

  for each write of resource R:
    if last_writer[R] >= 0: add edge last_writer[R] -> P
    for each reader in last_readers[R]: add edge reader -> P
    clear last_readers[R]
    last_writer[R] = P

// Attachment load_op = LOAD counts as read+write.
```

### 3) Pass Culling

- Start from the present image and any exported resources.
- Walk dependencies backward, mark reachable passes.
- Passes with `VKR_RG_PASS_FLAG_NO_CULL` are always kept.

### 4) Topological Sort

Kahn's algorithm over the DAG; emit `execution_order[]`.

### 5) Resource Lifetime and Allocation

- For each resource, compute `first_pass` / `last_pass` in execution order.
- Graph-owned backing resources persist and are reused while their descriptions
  remain compatible; a description change recreates the backing resource.
- There is no transient aliasing pool. `first_pass`/`last_pass` are scheduling
  metadata, not evidence of overlapping-memory allocation.
- Persistent resources remain until graph destruction or explicit invalidation.

### 6) RenderPass and RenderTarget Creation

For each graphics pass:

- Build a `VkrRenderPassDesc` from attachment formats and load/store ops.
- Use `VkrRenderPassSignature` as a cache key; reuse compatible renderpasses.
- Build `VkrRenderTargetDesc` using attachment slices and backing textures.
- Materialize an exact image view for every partial mip/layer range, including
  layer 0 of an array; the backing texture's default view is valid only when
  the attachment covers its complete view range.
- Keep one render target per swapchain image when any attachment is `PER_IMAGE`.

**Layout convention for renderpasses:**

- `initial_layout` and `final_layout` are **fixed to attachment usage**
  (`COLOR_ATTACHMENT_OPTIMAL` or `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`).
- Cross-pass layout transitions are handled by the graph via explicit barriers.
- Swapchain transitions to `PRESENT_SRC_KHR` are done after the last pass.

### 7) Barrier Generation

Track each image `(mip, layer)` and each whole buffer in execution order. Gather
all declarations for a pass before emitting its pre-barriers: compatible image
accesses that require the same layout are unioned, while incompatible layouts
fail compilation.

```c
typedef struct VkrRgSubresourceState {
  VkrRgImageAccessFlags access;
  VkrTextureLayout layout;
  VkrRgImageAccessFlags pending_access;
  VkrTextureLayout pending_layout;
} VkrRgSubresourceState;

for each pass in execution_order:
  for each declared use in pass:
    desired = access_to_state(use)
    gather desired access/layout per subresource
  for each touched subresource:
    if layout changed or access changed or prior access wrote:
      emit barrier(prior -> combined desired)
    commit combined desired
```

Mapping access -> layout:

- COLOR_ATTACHMENT -> `VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`
- DEPTH_ATTACHMENT -> `VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`
- DEPTH_READ_ONLY -> `VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL`
- SAMPLED -> `VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`
- STORAGE_* -> `VKR_TEXTURE_LAYOUT_GENERAL`
- TRANSFER_SRC/DST -> transfer layouts
- PRESENT -> `VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR`

Buffer access mapping (stage/access):

- VERTEX/INDEX -> vertex input stage + read access
- UNIFORM -> shader read (vertex/fragment/compute as used)
- STORAGE_* -> shader read/write
- TRANSFER_SRC/DST -> transfer stage

---

## Execute Phase

For each pass in `execution_order`:

1. Apply pre-pass barriers.
2. If graphics:
   - `vkr_renderer_begin_render_pass(renderpass, render_target)`
   - `execute(ctx, user_data)`
   - `vkr_renderer_end_render_pass()`
3. If compute/transfer:
   - call `execute` (record compute/transfer commands).
Barrier, target, begin/end-render-pass, and executor failures stop execution and
return a `VkrRendererError` to packet submission. Missing runtime resource
handles are errors rather than silently skipped declarations.

After the final graphics pass that writes the present image, transition it to
`PRESENT_SRC_KHR`.

---

## Render Graph Replacement of the View System

### What is removed

- View-system orchestration (layer ordering, view lifecycle hooks)
- View-owned render state and messaging

### What replaces it

A JSON-defined render graph plus a set of **pass modules** that register
executors referenced by the JSON file. Pass modules use stateless resource
owners (`vkr_world_resources`, `vkr_ui_system`, `vkr_skybox_system`) and
the persistent cache systems (materials, meshes, textures) for draw logic.

```
lib/src/renderer/passes/
  vkr_pass_shadow.c/.h
  vkr_pass_skybox.c/.h
  vkr_pass_world.c/.h
  vkr_pass_ui.c/.h
  vkr_pass_editor.c/.h
```

Each module provides an execute callback, registered under a stable string
name (e.g., `pass.world`, `pass.ui`, `pass.shadow.cascade`).

### Mapping old layers to graph passes

| Old Layer | Executor Key | Notes |
|----------|---------------------|-------|
| Shadow | `pass.shadow.cascade` | N passes for cascades |
| Skybox | `pass.skybox` | Writes scene color/depth |
| World | `pass.world` | Reads shadows, writes scene color/depth |
| UI | `pass.ui` | Writes scene color (load) |
| Editor | `pass.editor` | Reads offscreen scene color, writes swapchain |

### Input/update logic

Input handling and per-frame updates are **not part of the render graph**.
View update logic lives in the respective systems (scene/UI/editor) and is
called from the application or renderer frontend.

For cross-system communication, use the existing **event system**
(`lib/src/core/event.*`) instead of view-layer messages.

---

## Example JSON Graph (Current Pipeline)

This single JSON file supports both fullscreen and editor modes via
`condition` and `repeat` directives.

```json
{
  "version": 1,
  "name": "main",
  "resources": [
    {
      "name": "swapchain",
      "type": "image",
      "import": "swapchain",
      "flags": ["EXTERNAL", "PER_IMAGE"]
    },
    {
      "name": "swapchain_depth",
      "type": "image",
      "import": "swapchain_depth",
      "flags": ["EXTERNAL", "PER_IMAGE"]
    },
    {
      "name": "shadow_map_${i}",
      "type": "image",
      "repeat": { "count_source": "shadow_cascade_count" },
      "extent": { "mode": "square", "size_source": "shadow_map_size" },
      "format": "D32_SFLOAT",
      "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"],
      "flags": ["TRANSIENT", "PER_IMAGE"]
    },
    {
      "name": "scene_color",
      "type": "image",
      "condition": "editor_enabled",
      "extent": { "mode": "viewport" },
      "format": "SWAPCHAIN",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"],
      "flags": ["TRANSIENT", "RESIZABLE", "PER_IMAGE"]
    },
    {
      "name": "scene_depth",
      "type": "image",
      "condition": "editor_enabled",
      "extent": { "mode": "viewport" },
      "format": "D32_SFLOAT",
      "usage": ["DEPTH_STENCIL_ATTACHMENT"],
      "flags": ["TRANSIENT", "RESIZABLE", "PER_IMAGE"]
    }
  ],
  "passes": [
    {
      "name": "Shadow.Cascade.${i}",
      "type": "graphics",
      "domain": "SHADOW",
      "repeat": { "count_source": "shadow_cascade_count" },
      "attachments": {
        "depth": {
          "image": "shadow_map_${i}",
          "load": "CLEAR",
          "store": "STORE",
          "clear": { "depth": 1.0, "stencil": 0 }
        }
      },
      "execute": "pass.shadow.cascade"
    },
    {
      "name": "Skybox.Fullscreen",
      "type": "graphics",
      "domain": "SKYBOX",
      "condition": "!editor_enabled",
      "attachments": {
        "color": [
          {
            "image": "swapchain",
            "load": "CLEAR",
            "store": "STORE",
            "clear": { "color": [0.02, 0.02, 0.03, 1.0] }
          }
        ],
        "depth": {
          "image": "swapchain_depth",
          "load": "CLEAR",
          "store": "STORE",
          "clear": { "depth": 1.0, "stencil": 0 }
        }
      },
      "execute": "pass.skybox"
    },
    {
      "name": "Skybox.Editor",
      "type": "graphics",
      "domain": "SKYBOX",
      "condition": "editor_enabled",
      "attachments": {
        "color": [
          {
            "image": "scene_color",
            "load": "CLEAR",
            "store": "STORE",
            "clear": { "color": [0.02, 0.02, 0.03, 1.0] }
          }
        ],
        "depth": {
          "image": "scene_depth",
          "load": "CLEAR",
          "store": "STORE",
          "clear": { "depth": 1.0, "stencil": 0 }
        }
      },
      "execute": "pass.skybox"
    },
    {
      "name": "World.Fullscreen",
      "type": "graphics",
      "domain": "WORLD",
      "condition": "!editor_enabled",
      "reads": [
        {
          "image": "shadow_map_${i}",
          "access": "SAMPLED",
          "binding": 0,
          "array_index": "${i}",
          "repeat": { "count_source": "shadow_cascade_count" }
        }
      ],
      "attachments": {
        "color": [
          { "image": "swapchain", "load": "LOAD", "store": "STORE" }
        ],
        "depth": {
          "image": "swapchain_depth",
          "load": "LOAD",
          "store": "STORE"
        }
      },
      "execute": "pass.world"
    },
    {
      "name": "World.Editor",
      "type": "graphics",
      "domain": "WORLD",
      "condition": "editor_enabled",
      "reads": [
        {
          "image": "shadow_map_${i}",
          "access": "SAMPLED",
          "binding": 0,
          "array_index": "${i}",
          "repeat": { "count_source": "shadow_cascade_count" }
        }
      ],
      "attachments": {
        "color": [
          { "image": "scene_color", "load": "LOAD", "store": "STORE" }
        ],
        "depth": {
          "image": "scene_depth",
          "load": "LOAD",
          "store": "STORE"
        }
      },
      "execute": "pass.world"
    },
    {
      "name": "UI.Fullscreen",
      "type": "graphics",
      "domain": "UI",
      "condition": "!editor_enabled",
      "attachments": {
        "color": [
          { "image": "swapchain", "load": "LOAD", "store": "STORE" }
        ]
      },
      "execute": "pass.ui"
    },
    {
      "name": "UI.Editor",
      "type": "graphics",
      "domain": "UI",
      "condition": "editor_enabled",
      "attachments": {
        "color": [
          { "image": "scene_color", "load": "LOAD", "store": "STORE" }
        ]
      },
      "execute": "pass.ui"
    },
    {
      "name": "Editor.Composite",
      "type": "graphics",
      "domain": "UI",
      "condition": "editor_enabled",
      "reads": [
        { "image": "scene_color", "access": "SAMPLED",
          "binding": 0, "array_index": 0 }
      ],
      "attachments": {
        "color": [
          { "image": "swapchain", "load": "CLEAR", "store": "STORE",
            "clear": { "color": [0, 0, 0, 1] } }
        ]
      },
      "execute": "pass.editor"
    }
  ],
  "outputs": {
    "present": "swapchain"
  }
}
```

---

## File Structure Changes

### New Files

| File | Purpose |
|------|---------|
| `lib/src/renderer/vkr_render_graph.h` | Public graph API, builder types |
| `lib/src/renderer/vkr_render_graph.c` | Core graph lifecycle + build state |
| `lib/src/renderer/vkr_rg_compile.c` | Dependency, culling, barriers, targets |
| `lib/src/renderer/vkr_rg_execute.c` | Command recording + execution |
| `lib/src/renderer/vkr_rg_debug.c` | DOT export, validation helpers |
| `lib/src/renderer/vkr_rg_json.c` | JSON loader and validation |
| `lib/src/renderer/vkr_rg_json.h` | JSON loader API |
| `lib/src/renderer/passes/vkr_pass_*.c/.h` | Pass modules (shadow, skybox, world, UI, editor) |
| `assets/render_graphs/main.rendergraph.json` | Primary render graph definition |
| `docs/rendering/render-graph-schema.json` | JSON schema (documented contract) |

### Modified Files

| File | Changes |
|------|---------|
| `lib/src/renderer/renderer_frontend.h` | Add `VkrRenderGraph *render_graph` |
| `lib/src/renderer/renderer_frontend.c` | Build/compile/execute graph in render loop |
| `lib/src/renderer/vkr_renderer.h` | Add `vkr_renderer_get_swapchain_format()` helper |
| `lib/src/renderer/vulkan/vulkan_backend.c` | Barrier helpers for graph |
| `app/src/` | Load JSON graph, register executors, call graph build per frame |

### Removed Files

| File | Reason |
|------|--------|
| `VkrViewSystem` and `VkrLayer` implementation | Removed; render graph owns orchestration |
| View helper modules | Replaced by stateless resource owners + pass executors |

---

## Implementation Plan (No Backward Compatibility)

**Status:** The core migration and synchronization for declared resources are
implemented; complete graph coverage remains partial. The historical phase log is
`docs/archive/render-graph-progress.md`.

### Phase 1: JSON Contract + Loader

1. Define JSON schema and sample graph file.
2. Implement `vkr_rg_json.c/.h` loader + validation.
3. Implement executor registry for pass callbacks.
4. Wire loader into renderer frontend (startup load; hot reload remains future
   work).

**Validation:** JSON graph loads and validates without building passes.

### Phase 2: Render Graph Core

1. Implement `vkr_render_graph.h/.c` with builder API and frame lifecycle.
2. Implement dependency analysis, culling, and topological sort.
3. Implement barrier generation (image + buffer).
4. Implement renderpass/target creation and cache.

**Validation:** graph executes a single test pass without validation errors.

### Phase 3: Port Rendering Passes

1. Create pass modules: shadow, skybox, world, UI, editor.
2. Author `main.rendergraph.json` for fullscreen and editor modes.
3. Ensure pass dependencies and load-op semantics preserve visuals.

**Validation:** output matches the previous renderer baseline for the same scenes.

### Phase 4: Remove View System

1. Delete the view/layer implementation and messaging helpers.
2. Remove view-layer messages; replace with event system or direct system APIs.
3. Update app and renderer frontend to only use render graph.

**Validation:** build succeeds with the view system removed and pass executors
rendering through stateless resource owners.

### Phase 5: Optimization + Debugging

1. Add DOT export and validation toggles.
2. Add graph live/peak resource metrics.
3. Add graph stats (passes, barriers, resources).

**Validation:** graph export shows correct dependencies; perf within budget.

---

## Debugging and Validation

- `vkr_rg_export_dot(graph, "render_graph.dot")`
- Per-pass debug markers (name + domain)
- JSON parse/validation errors include field path and line/offset
- Validation warnings for:
  - invalid handles
  - missing attachments
  - cyclic dependencies
  - load-op mismatch (LOAD without prior writer)

---

## Future Enhancements

1. Transient aliasing and memory overlap scheduling
2. Async compute queue support
3. Renderpass merging / subpass optimization
4. Virtual resources (late allocation)
5. Automatic descriptor set management
