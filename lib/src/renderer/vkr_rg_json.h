#pragma once

#include "containers/str.h"
#include "containers/vector.h"
#include "defines.h"
#include "memory/vkr_allocator.h"
#include "renderer/vkr_render_graph.h"
#include "renderer/vkr_renderer.h"

// =============================================================================
// JSON graph representation (parsed, not yet expanded)
// =============================================================================

typedef enum VkrRgJsonConditionKind {
  VKR_RG_JSON_CONDITION_NONE = 0,
  VKR_RG_JSON_CONDITION_EDITOR_ENABLED,
  VKR_RG_JSON_CONDITION_EDITOR_DISABLED,
} VkrRgJsonConditionKind;

/**
 * Condition expression.
 * @note: The condition expression is only used to determine if a resource or
 * pass should be included in the render graph. It is not used to determine if
 * a resource or pass should be executed.
 */
typedef struct VkrRgJsonCondition {
  VkrRgJsonConditionKind kind; // The kind of condition expression.
  String8 raw;                 // The raw condition expression string.
} VkrRgJsonCondition;

/**
 * Repeat expression.
 * @note: The repeat expression is only used to determine how many times a
 * resource or pass should be included in the render graph. It is not used to
 * determine if a resource or pass should be executed.
 */
typedef struct VkrRgJsonRepeat {
  bool8_t enabled;      // Whether the repeat expression is enabled.
  String8 count_source; // The source of the repeat count.
} VkrRgJsonRepeat;

/**
 * Resource flags.
 * @note: The resource flags are only used to determine if a resource should be
 * included in the render graph. It is not used to determine if a resource
 * should be executed.
 */
typedef enum VkrRgJsonResourceFlags {
  VKR_RG_JSON_RESOURCE_FLAG_NONE = 0,
  VKR_RG_JSON_RESOURCE_FLAG_TRANSIENT = 1 << 0,  // The resource is transient.
  VKR_RG_JSON_RESOURCE_FLAG_PERSISTENT = 1 << 1, // The resource is persistent.
  VKR_RG_JSON_RESOURCE_FLAG_EXTERNAL = 1 << 2,   // The resource is external.
  VKR_RG_JSON_RESOURCE_FLAG_PER_IMAGE = 1 << 3,  // The resource is per image.
  VKR_RG_JSON_RESOURCE_FLAG_RESIZABLE = 1 << 4,  // The resource is resizable.
} VkrRgJsonResourceFlags;

/**
 * Extent mode.
 * @note: The extent mode is only used to determine the extent of a resource.
 */
typedef enum VkrRgJsonExtentMode {
  VKR_RG_JSON_EXTENT_NONE = 0,
  VKR_RG_JSON_EXTENT_WINDOW,   // The extent is the window size.
  VKR_RG_JSON_EXTENT_VIEWPORT, // The extent is the viewport size.
  VKR_RG_JSON_EXTENT_FIXED,    // The extent is a fixed size.
  VKR_RG_JSON_EXTENT_SQUARE,   // The extent is a square size.
} VkrRgJsonExtentMode;

/**
 * Extent.
 * @note: The extent is only used to determine the extent of a resource.
 */
typedef struct VkrRgJsonExtent {
  VkrRgJsonExtentMode mode; // The mode of the extent.
  uint32_t width;           // The width of the extent.
  uint32_t height;          // The height of the extent.
  String8 size_source;      // The source of the size.
} VkrRgJsonExtent;

/**
 * Image description.
 * @note: The image description is only used to describe an image.
 */
typedef struct VkrRgJsonImageDesc {
  bool8_t is_import;           // Whether the image is imported.
  String8 import_name;         // The name of the imported image.
  bool8_t format_is_swapchain; // Whether the format is the swapchain format.
  VkrTextureFormat format;     // The format of the image.
  VkrTextureUsageFlags usage;  // The usage of the image.
  bool8_t layers_is_set;       // Whether the layers are set.
  uint32_t layers;             // The layers of the image.
  String8 layers_source;       // The source of the layers.
  VkrRgJsonExtent extent;      // The extent of the image.
} VkrRgJsonImageDesc;

/**
 * Buffer description.
 * @note: The buffer description is only used to describe a buffer.
 */
typedef struct VkrRgJsonBufferDesc {
  uint64_t size;             // The size of the buffer.
  VkrBufferUsageFlags usage; // The usage of the buffer.
} VkrRgJsonBufferDesc;

/**
 * Resource type.
 * @note: The resource type is only used to determine the type of a resource.
 */
typedef enum VkrRgJsonResourceType {
  VKR_RG_JSON_RESOURCE_IMAGE = 0,
  VKR_RG_JSON_RESOURCE_BUFFER = 1,
} VkrRgJsonResourceType;

/**
 * Resource.
 * @note: The resource is only used to describe a resource.
 */
typedef struct VkrRgJsonResource {
  String8 name;                 // The name of the resource.
  VkrRgJsonResourceType type;   // The type of the resource.
  VkrRgJsonCondition condition; // The condition of the resource.
  VkrRgJsonRepeat repeat;       // The repeat of the resource.
  uint32_t flags;               // The flags of the resource.
  VkrRgJsonImageDesc image;     // The image description of the resource.
  VkrRgJsonBufferDesc buffer;   // The buffer description of the resource.
} VkrRgJsonResource;
Vector(VkrRgJsonResource);

/**
 * Image access flags.
 * @note: The image access flags are only used to determine the access of an
 * image.
 */
typedef enum VkrRgJsonImageAccessFlags {
  VKR_RG_JSON_IMAGE_ACCESS_NONE = 0,
  VKR_RG_JSON_IMAGE_ACCESS_SAMPLED = 1 << 0,
  VKR_RG_JSON_IMAGE_ACCESS_STORAGE_READ = 1 << 1,
  VKR_RG_JSON_IMAGE_ACCESS_STORAGE_WRITE = 1 << 2,
  VKR_RG_JSON_IMAGE_ACCESS_COLOR_ATTACHMENT = 1 << 3,
  VKR_RG_JSON_IMAGE_ACCESS_DEPTH_ATTACHMENT = 1 << 4,
  VKR_RG_JSON_IMAGE_ACCESS_DEPTH_READ_ONLY = 1 << 5,
  VKR_RG_JSON_IMAGE_ACCESS_TRANSFER_SRC = 1 << 6,
  VKR_RG_JSON_IMAGE_ACCESS_TRANSFER_DST = 1 << 7,
  VKR_RG_JSON_IMAGE_ACCESS_PRESENT = 1 << 8,
} VkrRgJsonImageAccessFlags;

/**
 * Buffer access flags.
 * @note: The buffer access flags are only used to determine the access of a
 * buffer.
 */
typedef enum VkrRgJsonBufferAccessFlags {
  VKR_RG_JSON_BUFFER_ACCESS_NONE = 0,
  VKR_RG_JSON_BUFFER_ACCESS_VERTEX = 1 << 0,
  VKR_RG_JSON_BUFFER_ACCESS_INDEX = 1 << 1,
  VKR_RG_JSON_BUFFER_ACCESS_UNIFORM = 1 << 2,
  VKR_RG_JSON_BUFFER_ACCESS_STORAGE_READ = 1 << 3,
  VKR_RG_JSON_BUFFER_ACCESS_STORAGE_WRITE = 1 << 4,
  VKR_RG_JSON_BUFFER_ACCESS_TRANSFER_SRC = 1 << 5,
  VKR_RG_JSON_BUFFER_ACCESS_TRANSFER_DST = 1 << 6,
} VkrRgJsonBufferAccessFlags;

/**
 * Binding.
 * @note: The binding is only used to describe a binding.
 */
typedef struct VkrRgJsonBinding {
  bool8_t is_set; // Whether the binding is set.
  uint32_t value; // The value of the binding.
} VkrRgJsonBinding;

/**
 * Index.
 * @note: The index is only used to describe an index.
 */
typedef struct VkrRgJsonIndex {
  bool8_t is_set;   // Whether the index is set.
  bool8_t is_token; // Whether the index is a token.
  uint32_t value;   // The value of the index.
  String8 token;    // The token of the index.
} VkrRgJsonIndex;

/**
 * Resource use.
 * @note: The resource use is only used to describe a resource use.
 */
typedef struct VkrRgJsonResourceUse {
  bool8_t is_image;           // Whether the resource use is an image.
  String8 name;               // The name of the resource.
  VkrRgJsonRepeat repeat;     // The repeat of the resource.
  VkrRgJsonBinding binding;   // The binding of the resource.
  VkrRgJsonIndex array_index; // The array index of the resource.
  VkrRgJsonImageAccessFlags image_access; // The image access of the resource.
  VkrRgJsonBufferAccessFlags
      buffer_access; // The buffer access of the resource.
} VkrRgJsonResourceUse;
Vector(VkrRgJsonResourceUse);

/**
 * Attachment.
 * @note: The attachment is only used to describe an attachment.
 */
typedef struct VkrRgJsonAttachment {
  String8 image;                    // The name of the image.
  VkrAttachmentLoadOp load_op;      // The load operation of the attachment.
  VkrAttachmentStoreOp store_op;    // The store operation of the attachment.
  bool8_t has_clear;                // Whether the attachment has a clear value.
  VkrClearValue clear_value;        // The clear value of the attachment.
  bool8_t has_slice;                // Whether the attachment has a slice.
  VkrRgJsonIndex slice_mip_level;   // The slice mip level of the attachment.
  VkrRgJsonIndex slice_base_layer;  // The slice base layer of the attachment.
  VkrRgJsonIndex slice_layer_count; // The slice layer count of the attachment.
} VkrRgJsonAttachment;
Vector(VkrRgJsonAttachment);

/**
 * Attachments.
 * @note: The attachments are only used to describe the attachments of a pass.
 */
typedef struct VkrRgJsonAttachments {
  Vector_VkrRgJsonAttachment colors; // The color attachments.
  bool8_t has_depth;                 // Whether the pass has a depth attachment.
  VkrRgJsonAttachment depth;         // The depth attachment.
  bool8_t depth_read_only; // Whether the depth attachment is read only.
} VkrRgJsonAttachments;

/**
 * Pass type.
 * @note: The pass type is only used to determine the type of a pass.
 */
typedef enum VkrRgJsonPassType {
  VKR_RG_JSON_PASS_GRAPHICS = 0,
  VKR_RG_JSON_PASS_COMPUTE = 1,
  VKR_RG_JSON_PASS_TRANSFER = 2,
} VkrRgJsonPassType;

/**
 * Pass.
 * @note: The pass is only used to describe a pass.
 */
typedef struct VkrRgJsonPass {
  String8 name;                       // The name of the pass.
  VkrRgJsonPassType type;             // The type of the pass.
  VkrRgPassFlags flags;               // The flags of the pass.
  bool8_t has_domain;                 // Whether the pass has a domain.
  VkrPipelineDomain domain;           // The domain of the pass.
  VkrRgJsonCondition condition;       // The condition of the pass.
  VkrRgJsonRepeat repeat;             // The repeat of the pass.
  Vector_VkrRgJsonResourceUse reads;  // The reads of the pass.
  Vector_VkrRgJsonResourceUse writes; // The writes of the pass.
  VkrRgJsonAttachments attachments;   // The attachments of the pass.
  String8 execute;                    // The execute of the pass.
} VkrRgJsonPass;
Vector(VkrRgJsonPass);

/**
 * Outputs.
 * @note: The outputs are only used to describe the outputs of a graph.
 */
typedef struct VkrRgJsonOutputs {
  String8 present;               // The present output.
  Vector_String8 export_images;  // The export images.
  Vector_String8 export_buffers; // The export buffers.
} VkrRgJsonOutputs;

/**
 * Graph.
 * @note: The graph is only used to describe a graph.
 */
typedef struct VkrRgJsonGraph {
  uint32_t version;                   // The version of the graph.
  String8 name;                       // The name of the graph.
  Vector_VkrRgJsonResource resources; // The resources of the graph.
  Vector_VkrRgJsonPass passes;        // The passes of the graph.
  VkrRgJsonOutputs outputs;           // The outputs of the graph.
  String8 source;                     // The source of the graph.
  VkrAllocator *allocator;            // The allocator of the graph.
} VkrRgJsonGraph;

/**
 * Load a graph from a file.
 * @note: The graph is only used to describe a graph.
 * @param allocator: The allocator to use.
 * @param path: The path to the file.
 * @param out_graph: The output graph.
 * @return: true_v if the graph was loaded successfully, false_v otherwise.
 */
bool8_t vkr_rg_json_load_file(VkrAllocator *allocator, const char *path,
                              VkrRgJsonGraph *out_graph);

/**
 * Destroy a graph.
 * @note: The graph is only used to describe a graph.
 * @param graph: The graph to destroy.
 */
void vkr_rg_json_destroy(VkrRgJsonGraph *graph);

/**
 * Build a render graph from a JSON graph.
 * @note: The render graph is only used to describe a render graph.
 * @param rg: The render graph to build.
 * @param json_graph: The JSON graph to build.
 * @param frame: The frame info.
 * @param executors: The executors.
 * @return: true_v if the render graph was built successfully, false_v
 * otherwise.
 */
bool8_t vkr_rg_build_from_json(VkrRenderGraph *rg,
                               const VkrRgJsonGraph *json_graph,
                               const VkrRenderGraphFrameInfo *frame,
                               const VkrRgExecutorRegistry *executors);
