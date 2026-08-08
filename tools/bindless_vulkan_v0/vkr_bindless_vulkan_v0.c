#include <spirv_reflect.h>
#include <vulkan/vulkan.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V0_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define V0_MAX_DEVICES 16u
#define V0_MAX_EXTENSIONS 256u
#define V0_MAX_REPORT_ENTRIES 128u
#define V0_SAMPLED_IMAGE_CAPACITY 16u
#define V0_STORAGE_IMAGE_CAPACITY 8u
#define V0_SAMPLER_CAPACITY 16u
#define V0_ROOT_PUSH_CONSTANT_SIZE 16u
#define V0_TARGET_WIDTH 4u
#define V0_TARGET_HEIGHT 4u
#define V0_UPLOAD_SIZE 256u
#define V0_ROOT_OFFSET 0u
#define V0_VERTEX_OFFSET 96u
#define V0_INDEX_OFFSET 144u
#define V0_TEXTURE_OFFSET 152u
#define V0_TIMELINE_VALUE 1u

#ifndef VKR_V0_VERTEX_SPV
#define VKR_V0_VERTEX_SPV "v0.vert.spv"
#endif

#ifndef VKR_V0_FRAGMENT_SPV
#define VKR_V0_FRAGMENT_SPV "v0.frag.spv"
#endif

typedef struct V0Vertex {
  float position[2];
  float uv[2];
} V0Vertex;

typedef struct V0DrawRoot {
  uint64_t vertices;
  float tint[4];
  float transform[16];
  uint32_t texture_index;
  uint32_t sampler_index;
} V0DrawRoot;

typedef struct V0PushConstants {
  uint64_t root;
  uint32_t material_index;
  uint32_t flags;
} V0PushConstants;

_Static_assert(sizeof(V0Vertex) == 16u, "V0Vertex ABI drift");
_Static_assert(_Alignof(V0Vertex) == 4u, "V0Vertex alignment drift");
_Static_assert(offsetof(V0Vertex, position) == 0u,
               "V0Vertex.position ABI drift");
_Static_assert(offsetof(V0Vertex, uv) == 8u, "V0Vertex.uv ABI drift");
_Static_assert(sizeof(V0DrawRoot) == 96u, "V0DrawRoot ABI drift");
_Static_assert(_Alignof(V0DrawRoot) == 8u, "V0DrawRoot alignment drift");
_Static_assert(offsetof(V0DrawRoot, vertices) == 0u,
               "V0DrawRoot.vertices ABI drift");
_Static_assert(offsetof(V0DrawRoot, tint) == 8u, "V0DrawRoot.tint ABI drift");
_Static_assert(offsetof(V0DrawRoot, transform) == 24u,
               "V0DrawRoot.transform ABI drift");
_Static_assert(offsetof(V0DrawRoot, texture_index) == 88u,
               "V0DrawRoot.texture_index ABI drift");
_Static_assert(offsetof(V0DrawRoot, sampler_index) == 92u,
               "V0DrawRoot.sampler_index ABI drift");
_Static_assert(sizeof(V0PushConstants) == V0_ROOT_PUSH_CONSTANT_SIZE,
               "V0PushConstants ABI drift");
_Static_assert(_Alignof(V0PushConstants) == 8u,
               "V0PushConstants alignment drift");
_Static_assert(offsetof(V0PushConstants, root) == 0u,
               "V0PushConstants.root ABI drift");
_Static_assert(offsetof(V0PushConstants, material_index) == 8u,
               "V0PushConstants.material_index ABI drift");
_Static_assert(offsetof(V0PushConstants, flags) == 12u,
               "V0PushConstants.flags ABI drift");

typedef enum V0ReportKind {
  V0_REPORT_API_VERSION,
  V0_REPORT_INSTANCE_EXTENSION,
  V0_REPORT_DEVICE_EXTENSION,
  V0_REPORT_FEATURE,
  V0_REPORT_LIMIT,
  V0_REPORT_QUEUE,
  V0_REPORT_FORMAT,
  V0_REPORT_DEVICE_CREATE,
  V0_REPORT_LAYOUT,
} V0ReportKind;

typedef struct V0ReportEntry {
  V0ReportKind kind;
  bool required;
  bool present;
  char name[80];
  char detail[192];
} V0ReportEntry;

typedef struct V0Report {
  V0ReportEntry entries[V0_MAX_REPORT_ENTRIES];
  uint32_t count;
  bool overflowed;
} V0Report;

typedef struct V0Options {
  bool reflect_only;
  bool validation;
  bool synchronization_validation;
  bool gpu_assisted;
} V0Options;

typedef struct V0FeatureSet {
  bool shader_int64;
  bool buffer_device_address;
  bool timeline_semaphore;
  bool descriptor_indexing;
  bool runtime_descriptor_array;
  bool sampled_image_non_uniform;
  bool storage_image_non_uniform;
  bool scalar_block_layout;
  bool host_query_reset;
  bool dynamic_rendering;
  bool synchronization2;
  bool maintenance4;
  bool maintenance5;
  bool host_image_copy;
  bool descriptor_buffer;
  bool descriptor_buffer_capture_replay;
  bool descriptor_buffer_image_layout_ignored;
  bool descriptor_buffer_push_descriptors;
  bool swapchain_maintenance1;
} V0FeatureSet;

typedef struct V0Candidate {
  VkPhysicalDevice physical_device;
  VkPhysicalDeviceProperties2 properties;
  VkPhysicalDeviceDriverProperties driver;
  VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_properties;
  VkPhysicalDeviceMemoryProperties memory_properties;
  V0FeatureSet features;
  V0Report report;
  uint32_t queue_family_index;
  uint32_t score;
  bool common_viable;
  bool window_preflight_viable;
  bool has_descriptor_buffer_extension;
  bool has_swapchain_extension;
  bool has_swapchain_maintenance_extension;
} V0Candidate;

typedef struct V0Allocation {
  VkDeviceMemory memory;
  VkDeviceSize size;
  void *mapped;
  uint32_t memory_type_index;
  VkMemoryPropertyFlags properties;
} V0Allocation;

typedef struct V0Buffer {
  VkBuffer buffer;
  V0Allocation allocation;
  VkDeviceAddress address;
  VkDeviceSize size;
} V0Buffer;

typedef struct V0Image {
  VkImage image;
  VkImageView view;
  V0Allocation allocation;
} V0Image;

typedef struct V0DescriptorLayout {
  VkDescriptorSetLayout layout;
  VkDeviceSize size;
  VkDeviceSize sampled_image_offset;
  VkDeviceSize storage_image_offset;
  VkDeviceSize sampler_offset;
} V0DescriptorLayout;

typedef struct V0Context {
  V0Options options;
  VkInstance instance;
  VkDebugUtilsMessengerEXT debug_messenger;
  VkDevice device;
  VkQueue queue;
  V0Candidate candidates[V0_MAX_DEVICES];
  uint32_t candidate_count;
  uint32_t selected_candidate_index;
  V0Candidate *selected;
  V0DescriptorLayout resource_layout;
  V0DescriptorLayout sampler_layout;
  V0Buffer resource_descriptor_buffer;
  V0Buffer sampler_descriptor_buffer;
  V0Buffer upload_buffer;
  V0Buffer readback_buffer;
  V0Image sampled_image;
  V0Image target_image;
  VkSampler sampler;
  VkShaderModule vertex_shader;
  VkShaderModule fragment_shader;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  VkSemaphore timeline;
  bool submission_pending;
  bool gpu_assisted_unavailable;
  uint32_t validation_setup_notice_count;
  uint32_t validation_warning_count;
  uint32_t validation_error_count;
  PFN_vkGetDescriptorSetLayoutSizeEXT get_layout_size;
  PFN_vkGetDescriptorSetLayoutBindingOffsetEXT get_binding_offset;
  PFN_vkGetDescriptorEXT get_descriptor;
  PFN_vkCmdBindDescriptorBuffersEXT cmd_bind_descriptor_buffers;
  PFN_vkCmdSetDescriptorBufferOffsetsEXT cmd_set_descriptor_offsets;
} V0Context;

static const char *v0_report_kind_name(V0ReportKind kind) {
  switch (kind) {
  case V0_REPORT_API_VERSION:
    return "API_VERSION";
  case V0_REPORT_INSTANCE_EXTENSION:
    return "INSTANCE_EXTENSION";
  case V0_REPORT_DEVICE_EXTENSION:
    return "DEVICE_EXTENSION";
  case V0_REPORT_FEATURE:
    return "FEATURE";
  case V0_REPORT_LIMIT:
    return "LIMIT";
  case V0_REPORT_QUEUE:
    return "QUEUE";
  case V0_REPORT_FORMAT:
    return "FORMAT";
  case V0_REPORT_DEVICE_CREATE:
    return "DEVICE_CREATE";
  case V0_REPORT_LAYOUT:
    return "LAYOUT";
  }
  return "UNKNOWN";
}

static const char *v0_vk_result_name(VkResult result) {
  switch (result) {
  case VK_SUCCESS:
    return "VK_SUCCESS";
  case VK_NOT_READY:
    return "VK_NOT_READY";
  case VK_TIMEOUT:
    return "VK_TIMEOUT";
  case VK_ERROR_OUT_OF_HOST_MEMORY:
    return "VK_ERROR_OUT_OF_HOST_MEMORY";
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
  case VK_ERROR_INITIALIZATION_FAILED:
    return "VK_ERROR_INITIALIZATION_FAILED";
  case VK_ERROR_DEVICE_LOST:
    return "VK_ERROR_DEVICE_LOST";
  case VK_ERROR_MEMORY_MAP_FAILED:
    return "VK_ERROR_MEMORY_MAP_FAILED";
  case VK_ERROR_LAYER_NOT_PRESENT:
    return "VK_ERROR_LAYER_NOT_PRESENT";
  case VK_ERROR_EXTENSION_NOT_PRESENT:
    return "VK_ERROR_EXTENSION_NOT_PRESENT";
  case VK_ERROR_FEATURE_NOT_PRESENT:
    return "VK_ERROR_FEATURE_NOT_PRESENT";
  case VK_ERROR_INCOMPATIBLE_DRIVER:
    return "VK_ERROR_INCOMPATIBLE_DRIVER";
  case VK_ERROR_FORMAT_NOT_SUPPORTED:
    return "VK_ERROR_FORMAT_NOT_SUPPORTED";
  default:
    return "VkResult(unlisted)";
  }
}

static void v0_report_add(V0Report *report, V0ReportKind kind, const char *name,
                          bool required, bool present, const char *detail) {
  if (report->count >= V0_MAX_REPORT_ENTRIES) {
    report->overflowed = true;
    return;
  }

  V0ReportEntry *entry = &report->entries[report->count++];
  memset(entry, 0, sizeof(*entry));
  entry->kind = kind;
  entry->required = required;
  entry->present = present;
  snprintf(entry->name, sizeof(entry->name), "%s", name ? name : "");
  snprintf(entry->detail, sizeof(entry->detail), "%s", detail ? detail : "");
}

static void v0_report_add_u64(V0Report *report, V0ReportKind kind,
                              const char *name, bool required, uint64_t actual,
                              uint64_t minimum) {
  char detail[192];
  snprintf(detail, sizeof(detail), "actual=%" PRIu64 " minimum=%" PRIu64,
           actual, minimum);
  v0_report_add(report, kind, name, required, actual >= minimum, detail);
}

static void v0_report_record_u64(V0Report *report, const char *name,
                                 uint64_t actual) {
  char detail[192];
  snprintf(detail, sizeof(detail), "actual=%" PRIu64, actual);
  v0_report_add(report, V0_REPORT_LIMIT, name, false, true, detail);
}

static bool v0_report_required_entries_present(const V0Report *report) {
  if (report->overflowed) {
    return false;
  }
  for (uint32_t i = 0; i < report->count; ++i) {
    if (report->entries[i].required && !report->entries[i].present) {
      return false;
    }
  }
  return true;
}

static void v0_print_report(const V0Candidate *candidate, uint32_t index) {
  const VkConformanceVersion *version = &candidate->driver.conformanceVersion;
  printf("\nDEVICE[%u] %s\n", index,
         candidate->properties.properties.deviceName);
  printf("  api=%u.%u.%u driverID=%u driverName=%s\n",
         VK_API_VERSION_MAJOR(candidate->properties.properties.apiVersion),
         VK_API_VERSION_MINOR(candidate->properties.properties.apiVersion),
         VK_API_VERSION_PATCH(candidate->properties.properties.apiVersion),
         candidate->driver.driverID, candidate->driver.driverName);
  printf("  driverInfo=%s conformance=%u.%u.%u.%u\n",
         candidate->driver.driverInfo, version->major, version->minor,
         version->subminor, version->patch);
  printf("  common/offscreen=%s window-preflight=%s queue-family=%u\n",
         candidate->common_viable ? "viable" : "rejected",
         candidate->window_preflight_viable ? "viable" : "rejected",
         candidate->queue_family_index);

  for (uint32_t i = 0; i < candidate->report.count; ++i) {
    const V0ReportEntry *entry = &candidate->report.entries[i];
    printf("  %-20s %-7s %-4s %-44s %s\n", v0_report_kind_name(entry->kind),
           entry->required ? "required" : "record",
           entry->present ? "yes" : "NO", entry->name, entry->detail);
  }
  if (candidate->report.overflowed) {
    printf("  REPORT OVERFLOW: capacity=%u\n", V0_MAX_REPORT_ENTRIES);
  }
}

static bool v0_extension_present(const VkExtensionProperties *extensions,
                                 uint32_t count, const char *name) {
  for (uint32_t i = 0; i < count; ++i) {
    if (strcmp(extensions[i].extensionName, name) == 0) {
      return true;
    }
  }
  return false;
}

static bool v0_find_layer(const char *name, VkLayerProperties *out_layer) {
  uint32_t count = 0;
  if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS ||
      count == 0 || count > V0_MAX_EXTENSIONS) {
    return false;
  }
  VkLayerProperties layers[V0_MAX_EXTENSIONS];
  if (vkEnumerateInstanceLayerProperties(&count, layers) != VK_SUCCESS) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (strcmp(layers[i].layerName, name) == 0) {
      if (out_layer) {
        *out_layer = layers[i];
      }
      return true;
    }
  }
  return false;
}

static bool v0_read_file(const char *path, uint8_t **out_bytes,
                         size_t *out_size) {
  *out_bytes = NULL;
  *out_size = 0;
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "failed to open %s\n", path);
    return false;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false;
  }
  long end = ftell(file);
  if (end <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return false;
  }
  uint8_t *bytes = (uint8_t *)malloc((size_t)end);
  if (!bytes) {
    fclose(file);
    return false;
  }
  size_t read_size = fread(bytes, 1, (size_t)end, file);
  fclose(file);
  if (read_size != (size_t)end) {
    free(bytes);
    return false;
  }
  *out_bytes = bytes;
  *out_size = read_size;
  return true;
}

static SpvReflectBlockVariable *
v0_find_block_member(SpvReflectBlockVariable *parent, const char *name) {
  if (!parent || !name) {
    return NULL;
  }
  for (uint32_t i = 0; i < parent->member_count; ++i) {
    SpvReflectBlockVariable *member = &parent->members[i];
    if (member->name && strcmp(member->name, name) == 0) {
      return member;
    }
  }
  return NULL;
}

static bool v0_expect_member(SpvReflectBlockVariable *parent, const char *name,
                             uint32_t expected_offset) {
  SpvReflectBlockVariable *member = v0_find_block_member(parent, name);
  if (!member) {
    fprintf(stderr, "reflection: member '%s' is missing below '%s'\n", name,
            parent && parent->name ? parent->name : "<unnamed>");
    return false;
  }
  printf("  ABI %-28s offset=%u size=%u padded=%u members=%u\n", name,
         member->offset, member->size, member->padded_size,
         member->member_count);
  if (member->offset != expected_offset) {
    fprintf(stderr, "reflection: member '%s' offset=%u expected=%u\n", name,
            member->offset, expected_offset);
    return false;
  }
  return true;
}

static bool v0_validate_pointer_abi(SpvReflectBlockVariable *push_block) {
  if (!push_block || push_block->size != sizeof(V0PushConstants)) {
    fprintf(stderr, "reflection: push block size=%u expected=%zu\n",
            push_block ? push_block->size : 0u, sizeof(V0PushConstants));
    return false;
  }

  bool ok = true;
  SpvReflectBlockVariable *root = v0_find_block_member(push_block, "root");
  ok &= v0_expect_member(push_block, "root",
                         (uint32_t)offsetof(V0PushConstants, root));
  ok &= v0_expect_member(push_block, "material_index",
                         (uint32_t)offsetof(V0PushConstants, material_index));
  ok &= v0_expect_member(push_block, "flags",
                         (uint32_t)offsetof(V0PushConstants, flags));
  if (!root || root->member_count == 0) {
    fprintf(stderr,
            "reflection: SPIRV-Reflect did not recurse through the root "
            "PhysicalStorageBuffer pointer\n");
    return false;
  }

  SpvReflectBlockVariable *vertices = v0_find_block_member(root, "vertices");
  ok &= v0_expect_member(root, "vertices",
                         (uint32_t)offsetof(V0DrawRoot, vertices));
  ok &= v0_expect_member(root, "tint", (uint32_t)offsetof(V0DrawRoot, tint));
  ok &= v0_expect_member(root, "transform",
                         (uint32_t)offsetof(V0DrawRoot, transform));
  ok &= v0_expect_member(root, "texture_index",
                         (uint32_t)offsetof(V0DrawRoot, texture_index));
  ok &= v0_expect_member(root, "sampler_index",
                         (uint32_t)offsetof(V0DrawRoot, sampler_index));
  if (!vertices || vertices->member_count == 0) {
    fprintf(stderr,
            "reflection: SPIRV-Reflect did not recurse through the vertex "
            "PhysicalStorageBuffer pointer\n");
    return false;
  }
  ok &= v0_expect_member(vertices, "position",
                         (uint32_t)offsetof(V0Vertex, position));
  ok &= v0_expect_member(vertices, "uv", (uint32_t)offsetof(V0Vertex, uv));
  return ok;
}

static bool v0_validate_reflection_module(const char *path,
                                          const char *entry_point,
                                          SpvReflectShaderStageFlagBits stage,
                                          bool require_pointer_recursion) {
  uint8_t *bytes = NULL;
  size_t size = 0;
  if (!v0_read_file(path, &bytes, &size)) {
    return false;
  }

  SpvReflectShaderModule module;
  memset(&module, 0, sizeof(module));
  SpvReflectResult reflect_result =
      spvReflectCreateShaderModule(size, bytes, &module);
  free(bytes);
  if (reflect_result != SPV_REFLECT_RESULT_SUCCESS) {
    fprintf(stderr, "reflection: create failed for %s (%d)\n", path,
            reflect_result);
    return false;
  }

  bool ok = true;
  const SpvReflectEntryPoint *entry =
      spvReflectGetEntryPoint(&module, entry_point);
  if (!entry || entry->shader_stage != stage) {
    fprintf(stderr, "reflection: entry point %s missing or wrong stage in %s\n",
            entry_point, path);
    ok = false;
  }

  uint32_t push_count = 0;
  if (spvReflectEnumerateEntryPointPushConstantBlocks(&module, entry_point,
                                                      &push_count, NULL) !=
          SPV_REFLECT_RESULT_SUCCESS ||
      push_count != 1u) {
    fprintf(stderr, "reflection: %s push block count=%u expected=1\n",
            entry_point, push_count);
    ok = false;
  } else {
    SpvReflectBlockVariable *push_blocks[1] = {0};
    if (spvReflectEnumerateEntryPointPushConstantBlocks(
            &module, entry_point, &push_count, push_blocks) !=
        SPV_REFLECT_RESULT_SUCCESS) {
      ok = false;
    } else if (require_pointer_recursion) {
      ok &= v0_validate_pointer_abi(push_blocks[0]);
    } else if (push_blocks[0]->size != sizeof(V0PushConstants)) {
      fprintf(stderr, "reflection: fragment push block size=%u expected=%zu\n",
              push_blocks[0]->size, sizeof(V0PushConstants));
      ok = false;
    }
  }

  uint32_t binding_count = 0;
  if (spvReflectEnumerateEntryPointDescriptorBindings(&module, entry_point,
                                                      &binding_count, NULL) !=
      SPV_REFLECT_RESULT_SUCCESS) {
    ok = false;
  } else {
    SpvReflectDescriptorBinding *bindings[4] = {0};
    if (binding_count > V0_ARRAY_COUNT(bindings) ||
        spvReflectEnumerateEntryPointDescriptorBindings(
            &module, entry_point, &binding_count, bindings) !=
            SPV_REFLECT_RESULT_SUCCESS) {
      ok = false;
    } else if (stage == SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT) {
      bool sampled_ok = false;
      bool sampler_ok = false;
      for (uint32_t i = 0; i < binding_count; ++i) {
        const SpvReflectDescriptorBinding *binding = bindings[i];
        const bool runtime_array =
            binding->array.dims_count == 1u && binding->array.dims[0] == 0u;
        if (binding->set == 0u && binding->binding == 0u &&
            binding->descriptor_type ==
                SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
            runtime_array) {
          sampled_ok = true;
        }
        if (binding->set == 1u && binding->binding == 0u &&
            binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER &&
            runtime_array) {
          sampler_ok = true;
        }
      }
      if (!sampled_ok || !sampler_ok) {
        fprintf(stderr,
                "reflection: fragment runtime arrays sampled=%s sampler=%s\n",
                sampled_ok ? "yes" : "no", sampler_ok ? "yes" : "no");
        ok = false;
      }
    }
  }

  printf("REFLECTION %s entry=%s stage=0x%x bindings=%u result=%s\n", path,
         entry_point, stage, binding_count, ok ? "PASS" : "FAIL");
  spvReflectDestroyShaderModule(&module);
  return ok;
}

static bool v0_validate_shader_abi(void) {
  printf("SHADER ABI\n");
  bool vertex_ok =
      v0_validate_reflection_module(VKR_V0_VERTEX_SPV, "vert_main",
                                    SPV_REFLECT_SHADER_STAGE_VERTEX_BIT, true);
  bool fragment_ok = v0_validate_reflection_module(
      VKR_V0_FRAGMENT_SPV, "frag_main", SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT,
      false);
  return vertex_ok && fragment_ok;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
v0_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                  VkDebugUtilsMessageTypeFlagsEXT type,
                  const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
                  void *user_data) {
  V0Context *context = (V0Context *)user_data;
  const bool validation_message =
      (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0u;
  const bool gpuav_setup_notice =
      context->options.gpu_assisted && callback_data &&
      callback_data->pMessageIdName &&
      strcmp(callback_data->pMessageIdName, "WARNING-Setting-Limit-Adjusted") ==
          0;
  if (context->options.gpu_assisted && callback_data &&
      callback_data->pMessage &&
      strstr(callback_data->pMessage,
             "GPU-AV does not currently support validation of descriptor "
             "buffers")) {
    context->gpu_assisted_unavailable = true;
  }
  if (validation_message) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      context->validation_error_count++;
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) &&
               gpuav_setup_notice) {
      context->validation_setup_notice_count++;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
      context->validation_warning_count++;
    }
  }
  const char *source = validation_message ? "VALIDATION" : "LOADER";
  fprintf(stderr, "%s[%s] id=%s(%d) %s\n", source,
          (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
              ? "ERROR"
              : ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                     ? "WARN"
                     : "INFO"),
          callback_data && callback_data->pMessageIdName
              ? callback_data->pMessageIdName
              : "<none>",
          callback_data ? callback_data->messageIdNumber : 0,
          callback_data && callback_data->pMessage ? callback_data->pMessage
                                                   : "<no message>");
  return VK_FALSE;
}

static bool v0_create_instance(V0Context *context) {
  uint32_t loader_version = VK_API_VERSION_1_0;
  PFN_vkEnumerateInstanceVersion enumerate_instance_version =
      (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
          NULL, "vkEnumerateInstanceVersion");
  if (enumerate_instance_version) {
    VkResult version_result = enumerate_instance_version(&loader_version);
    if (version_result != VK_SUCCESS) {
      fprintf(stderr, "vkEnumerateInstanceVersion failed: %s (%d)\n",
              v0_vk_result_name(version_result), version_result);
      return false;
    }
  }
  printf("INSTANCE loader=%u.%u.%u required=1.4.0\n",
         VK_API_VERSION_MAJOR(loader_version),
         VK_API_VERSION_MINOR(loader_version),
         VK_API_VERSION_PATCH(loader_version));
  if (loader_version < VK_API_VERSION_1_4) {
    fprintf(stderr, "Vulkan loader does not expose Vulkan 1.4\n");
    return false;
  }

  uint32_t extension_count = 0;
  VkResult result =
      vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL);
  if (result != VK_SUCCESS || extension_count > V0_MAX_EXTENSIONS) {
    fprintf(stderr, "instance extension enumeration failed or exceeded %u\n",
            V0_MAX_EXTENSIONS);
    return false;
  }
  VkExtensionProperties extensions[V0_MAX_EXTENSIONS];
  result = vkEnumerateInstanceExtensionProperties(NULL, &extension_count,
                                                  extensions);
  if (result != VK_SUCCESS) {
    return false;
  }

  const bool has_debug_utils = v0_extension_present(
      extensions, extension_count, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  if (context->options.validation && !has_debug_utils) {
    fprintf(stderr, "validation requested but VK_EXT_debug_utils is absent\n");
    return false;
  }
  if (context->options.validation) {
    VkLayerProperties validation_layer;
    if (!v0_find_layer("VK_LAYER_KHRONOS_validation", &validation_layer)) {
      fprintf(
          stderr,
          "validation requested but VK_LAYER_KHRONOS_validation is absent\n");
      return false;
    }
    printf("VALIDATION LAYER spec=%u.%u.%u implementation=%u\n",
           VK_API_VERSION_MAJOR(validation_layer.specVersion),
           VK_API_VERSION_MINOR(validation_layer.specVersion),
           VK_API_VERSION_PATCH(validation_layer.specVersion),
           validation_layer.implementationVersion);
  }

  const char *enabled_extensions[1];
  uint32_t enabled_extension_count = 0;
  if (context->options.validation) {
    enabled_extensions[enabled_extension_count++] =
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  }
  const char *enabled_layers[1];
  uint32_t enabled_layer_count = 0;
  if (context->options.validation) {
    enabled_layers[enabled_layer_count++] = "VK_LAYER_KHRONOS_validation";
  }

  VkDebugUtilsMessengerCreateInfoEXT debug_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = v0_debug_callback,
      .pUserData = context,
  };
  VkValidationFeatureEnableEXT validation_enables[2];
  uint32_t validation_enable_count = 0;
  if (context->options.synchronization_validation) {
    validation_enables[validation_enable_count++] =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
  }
  if (context->options.gpu_assisted) {
    validation_enables[validation_enable_count++] =
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT;
  }
  const VkValidationFeatureDisableEXT validation_disables[] = {
      VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT,
  };
  const uint32_t validation_disable_count =
      (context->options.gpu_assisted &&
       !context->options.synchronization_validation)
          ? 1u
          : 0u;
  VkValidationFeaturesEXT validation_features = {
      .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
      .pNext = context->options.validation ? &debug_create_info : NULL,
      .enabledValidationFeatureCount = validation_enable_count,
      .pEnabledValidationFeatures = validation_enables,
      .disabledValidationFeatureCount = validation_disable_count,
      .pDisabledValidationFeatures = validation_disables,
  };
  VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vkr_bindless_vulkan_v0",
      .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
      .pEngineName = "VKR V0 spike",
      .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
      .apiVersion = VK_API_VERSION_1_4,
  };
  VkInstanceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = context->options.validation ? &validation_features : NULL,
      .pApplicationInfo = &app_info,
      .enabledLayerCount = enabled_layer_count,
      .ppEnabledLayerNames = enabled_layers,
      .enabledExtensionCount = enabled_extension_count,
      .ppEnabledExtensionNames = enabled_extensions,
  };
  result = vkCreateInstance(&create_info, NULL, &context->instance);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkCreateInstance failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }

  if (context->options.validation) {
    PFN_vkCreateDebugUtilsMessengerEXT create_debug_messenger =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            context->instance, "vkCreateDebugUtilsMessengerEXT");
    if (!create_debug_messenger ||
        create_debug_messenger(context->instance, &debug_create_info, NULL,
                               &context->debug_messenger) != VK_SUCCESS) {
      fprintf(stderr, "failed to create debug messenger\n");
      return false;
    }
  }
  return true;
}

static void v0_add_feature(V0Report *report, const char *name, bool present) {
  v0_report_add(report, V0_REPORT_FEATURE, name, true, present,
                present ? "enabled in candidate device chain"
                        : "required feature is VK_FALSE");
}

static void v0_query_candidate(V0Candidate *candidate,
                               VkPhysicalDevice physical_device,
                               const VkExtensionProperties *instance_extensions,
                               uint32_t instance_extension_count) {
  memset(candidate, 0, sizeof(*candidate));
  candidate->physical_device = physical_device;
  candidate->queue_family_index = UINT32_MAX;

  candidate->properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  candidate->driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
  candidate->descriptor_properties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
  candidate->properties.pNext = &candidate->driver;
  candidate->driver.pNext = &candidate->descriptor_properties;
  vkGetPhysicalDeviceProperties2(physical_device, &candidate->properties);
  candidate->properties.pNext = NULL;
  candidate->driver.pNext = NULL;

  vkGetPhysicalDeviceMemoryProperties(physical_device,
                                      &candidate->memory_properties);

  VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
  };
  VkPhysicalDeviceVulkan14Features features14 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
      .pNext = &descriptor_features,
  };
  VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &features14,
  };
  VkPhysicalDeviceVulkan12Features features12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = &features13,
  };
  VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &features12,
  };
  vkGetPhysicalDeviceFeatures2(physical_device, &features2);

  candidate->features.shader_int64 = features2.features.shaderInt64 == VK_TRUE;
  candidate->features.buffer_device_address =
      features12.bufferDeviceAddress == VK_TRUE;
  candidate->features.timeline_semaphore =
      features12.timelineSemaphore == VK_TRUE;
  candidate->features.descriptor_indexing =
      features12.descriptorIndexing == VK_TRUE;
  candidate->features.runtime_descriptor_array =
      features12.runtimeDescriptorArray == VK_TRUE;
  candidate->features.sampled_image_non_uniform =
      features12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
  candidate->features.storage_image_non_uniform =
      features12.shaderStorageImageArrayNonUniformIndexing == VK_TRUE;
  candidate->features.scalar_block_layout =
      features12.scalarBlockLayout == VK_TRUE;
  candidate->features.host_query_reset = features12.hostQueryReset == VK_TRUE;
  candidate->features.dynamic_rendering =
      features13.dynamicRendering == VK_TRUE;
  candidate->features.synchronization2 = features13.synchronization2 == VK_TRUE;
  candidate->features.maintenance4 = features13.maintenance4 == VK_TRUE;
  candidate->features.maintenance5 = features14.maintenance5 == VK_TRUE;
  candidate->features.host_image_copy = features14.hostImageCopy == VK_TRUE;
  candidate->features.descriptor_buffer =
      descriptor_features.descriptorBuffer == VK_TRUE;
  candidate->features.descriptor_buffer_capture_replay =
      descriptor_features.descriptorBufferCaptureReplay == VK_TRUE;
  candidate->features.descriptor_buffer_image_layout_ignored =
      descriptor_features.descriptorBufferImageLayoutIgnored == VK_TRUE;
  candidate->features.descriptor_buffer_push_descriptors =
      descriptor_features.descriptorBufferPushDescriptors == VK_TRUE;
  candidate->features.swapchain_maintenance1 =
      false; /* Populated by the extension-specific query below. */

  uint32_t extension_count = 0;
  VkResult result = vkEnumerateDeviceExtensionProperties(
      physical_device, NULL, &extension_count, NULL);
  VkExtensionProperties extensions[V0_MAX_EXTENSIONS];
  memset(extensions, 0, sizeof(extensions));
  if (result == VK_SUCCESS && extension_count <= V0_MAX_EXTENSIONS) {
    result = vkEnumerateDeviceExtensionProperties(physical_device, NULL,
                                                  &extension_count, extensions);
  }
  if (result != VK_SUCCESS || extension_count > V0_MAX_EXTENSIONS) {
    extension_count = 0;
  }
  candidate->has_descriptor_buffer_extension = v0_extension_present(
      extensions, extension_count, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
  candidate->has_swapchain_extension = v0_extension_present(
      extensions, extension_count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  candidate->has_swapchain_maintenance_extension =
      v0_extension_present(extensions, extension_count,
                           VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);

  if (candidate->has_swapchain_maintenance_extension) {
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchain_maintenance = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
    };
    VkPhysicalDeviceFeatures2 swapchain_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &swapchain_maintenance,
    };
    vkGetPhysicalDeviceFeatures2(physical_device, &swapchain_features);
    candidate->features.swapchain_maintenance1 =
        swapchain_maintenance.swapchainMaintenance1 == VK_TRUE;
  }

  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties2(physical_device,
                                            &queue_family_count, NULL);
  if (queue_family_count > 0 && queue_family_count <= V0_MAX_EXTENSIONS) {
    VkQueueFamilyProperties2 queue_families[V0_MAX_EXTENSIONS];
    memset(queue_families, 0, sizeof(queue_families));
    for (uint32_t i = 0; i < queue_family_count; ++i) {
      queue_families[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(
        physical_device, &queue_family_count, queue_families);
    const VkQueueFlags required_queue_flags =
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    for (uint32_t i = 0; i < queue_family_count; ++i) {
      if ((queue_families[i].queueFamilyProperties.queueFlags &
           required_queue_flags) == required_queue_flags &&
          queue_families[i].queueFamilyProperties.queueCount > 0u) {
        candidate->queue_family_index = i;
        break;
      }
    }
  }

  const uint32_t api_version = candidate->properties.properties.apiVersion;
  char api_detail[96];
  snprintf(api_detail, sizeof(api_detail), "actual=%u.%u.%u minimum=1.4.0",
           VK_API_VERSION_MAJOR(api_version), VK_API_VERSION_MINOR(api_version),
           VK_API_VERSION_PATCH(api_version));
  v0_report_add(&candidate->report, V0_REPORT_API_VERSION, "apiVersion", true,
                api_version >= VK_API_VERSION_1_4, api_detail);
  v0_report_add(&candidate->report, V0_REPORT_DEVICE_EXTENSION,
                VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, true,
                candidate->has_descriptor_buffer_extension,
                candidate->has_descriptor_buffer_extension
                    ? "extension enumerated"
                    : "extension absent");
  static const char *optional_extensions[] = {
      "VK_EXT_descriptor_heap", "VK_KHR_unified_image_layouts",
      "VK_EXT_mesh_shader",     "VK_EXT_device_generated_commands",
      "VK_EXT_shader_object",   "VK_EXT_graphics_pipeline_library",
      "VK_KHR_pipeline_binary",
  };
  for (uint32_t i = 0; i < V0_ARRAY_COUNT(optional_extensions); ++i) {
    const bool present = v0_extension_present(extensions, extension_count,
                                              optional_extensions[i]);
    v0_report_add(&candidate->report, V0_REPORT_DEVICE_EXTENSION,
                  optional_extensions[i], false, present,
                  present ? "recorded optional capability"
                          : "optional capability absent");
  }
  v0_add_feature(&candidate->report, "shaderInt64",
                 candidate->features.shader_int64);
  v0_add_feature(&candidate->report, "bufferDeviceAddress",
                 candidate->features.buffer_device_address);
  v0_add_feature(&candidate->report, "timelineSemaphore",
                 candidate->features.timeline_semaphore);
  v0_add_feature(&candidate->report, "descriptorIndexing",
                 candidate->features.descriptor_indexing);
  v0_add_feature(&candidate->report, "runtimeDescriptorArray",
                 candidate->features.runtime_descriptor_array);
  v0_add_feature(&candidate->report,
                 "shaderSampledImageArrayNonUniformIndexing",
                 candidate->features.sampled_image_non_uniform);
  v0_add_feature(&candidate->report,
                 "shaderStorageImageArrayNonUniformIndexing",
                 candidate->features.storage_image_non_uniform);
  v0_add_feature(&candidate->report, "scalarBlockLayout",
                 candidate->features.scalar_block_layout);
  v0_add_feature(&candidate->report, "hostQueryReset",
                 candidate->features.host_query_reset);
  v0_add_feature(&candidate->report, "dynamicRendering",
                 candidate->features.dynamic_rendering);
  v0_add_feature(&candidate->report, "synchronization2",
                 candidate->features.synchronization2);
  v0_add_feature(&candidate->report, "maintenance4",
                 candidate->features.maintenance4);
  v0_add_feature(&candidate->report, "maintenance5",
                 candidate->features.maintenance5);
  v0_add_feature(&candidate->report, "descriptorBuffer",
                 candidate->features.descriptor_buffer);
  v0_report_add(&candidate->report, V0_REPORT_FEATURE,
                "descriptorBufferCaptureReplay", false,
                candidate->features.descriptor_buffer_capture_replay,
                candidate->features.descriptor_buffer_capture_replay
                    ? "recorded optional diagnostic capability"
                    : "debugger capture support unavailable");
  v0_report_add(&candidate->report, V0_REPORT_FEATURE, "hostImageCopy", false,
                candidate->features.host_image_copy,
                candidate->features.host_image_copy
                    ? "recorded optional capability"
                    : "optional capability unavailable");
  v0_report_add(&candidate->report, V0_REPORT_FEATURE,
                "descriptorBufferImageLayoutIgnored", false,
                candidate->features.descriptor_buffer_image_layout_ignored,
                candidate->features.descriptor_buffer_image_layout_ignored
                    ? "recorded extension feature"
                    : "extension feature unavailable");
  v0_report_add(&candidate->report, V0_REPORT_FEATURE,
                "descriptorBufferPushDescriptors", false,
                candidate->features.descriptor_buffer_push_descriptors,
                candidate->features.descriptor_buffer_push_descriptors
                    ? "recorded extension feature"
                    : "extension feature unavailable");

  const VkPhysicalDeviceLimits *limits =
      &candidate->properties.properties.limits;
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "maxPerStageDescriptorSampledImages", true,
                    limits->maxPerStageDescriptorSampledImages,
                    V0_SAMPLED_IMAGE_CAPACITY);
  v0_report_add_u64(
      &candidate->report, V0_REPORT_LIMIT, "maxDescriptorSetSampledImages",
      true, limits->maxDescriptorSetSampledImages, V0_SAMPLED_IMAGE_CAPACITY);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "maxPerStageDescriptorSamplers", true,
                    limits->maxPerStageDescriptorSamplers, V0_SAMPLER_CAPACITY);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "maxDescriptorSetSamplers", true,
                    limits->maxDescriptorSetSamplers, V0_SAMPLER_CAPACITY);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "maxPerStageDescriptorStorageImages", true,
                    limits->maxPerStageDescriptorStorageImages,
                    V0_STORAGE_IMAGE_CAPACITY);
  v0_report_add_u64(
      &candidate->report, V0_REPORT_LIMIT, "maxDescriptorSetStorageImages",
      true, limits->maxDescriptorSetStorageImages, V0_STORAGE_IMAGE_CAPACITY);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT, "maxPerStageResources",
                    true, limits->maxPerStageResources,
                    V0_SAMPLED_IMAGE_CAPACITY + V0_STORAGE_IMAGE_CAPACITY + 1u);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT, "maxPushConstantsSize",
                    true, limits->maxPushConstantsSize,
                    V0_ROOT_PUSH_CONSTANT_SIZE);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "maxBoundDescriptorSets", true,
                    limits->maxBoundDescriptorSets, 2u);

  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *descriptor_properties =
      &candidate->descriptor_properties;
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "maxDescriptorBufferBindings", true,
                    descriptor_properties->maxDescriptorBufferBindings, 2u);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "maxResourceDescriptorBufferBindings", true,
                    descriptor_properties->maxResourceDescriptorBufferBindings,
                    1u);
  v0_report_add_u64(
      &candidate->report, V0_REPORT_LIMIT, "maxSamplerDescriptorBufferBindings",
      true, descriptor_properties->maxSamplerDescriptorBufferBindings, 1u);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "sampledImageDescriptorSize", true,
                    descriptor_properties->sampledImageDescriptorSize, 1u);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "storageImageDescriptorSize", true,
                    descriptor_properties->storageImageDescriptorSize, 1u);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "samplerDescriptorSize", true,
                    descriptor_properties->samplerDescriptorSize, 1u);
  v0_report_add_u64(&candidate->report, V0_REPORT_LIMIT,
                    "descriptorBufferOffsetAlignment", true,
                    descriptor_properties->descriptorBufferOffsetAlignment, 1u);

  v0_report_record_u64(&candidate->report, "bufferImageGranularity",
                       limits->bufferImageGranularity);
  v0_report_record_u64(&candidate->report, "nonCoherentAtomSize",
                       limits->nonCoherentAtomSize);
  v0_report_record_u64(&candidate->report, "minMemoryMapAlignment",
                       limits->minMemoryMapAlignment);
  v0_report_record_u64(&candidate->report, "optimalBufferCopyOffsetAlignment",
                       limits->optimalBufferCopyOffsetAlignment);
  v0_report_record_u64(&candidate->report, "optimalBufferCopyRowPitchAlignment",
                       limits->optimalBufferCopyRowPitchAlignment);
  v0_report_add(&candidate->report, V0_REPORT_LIMIT,
                "timestampComputeAndGraphics", false,
                limits->timestampComputeAndGraphics == VK_TRUE,
                limits->timestampComputeAndGraphics ? "actual=VK_TRUE"
                                                    : "actual=VK_FALSE");
  char timestamp_detail[64];
  snprintf(timestamp_detail, sizeof(timestamp_detail), "actual=%.9g",
           (double)limits->timestampPeriod);
  v0_report_add(&candidate->report, V0_REPORT_LIMIT, "timestampPeriod", false,
                true, timestamp_detail);

  v0_report_add(
      &candidate->report, V0_REPORT_LIMIT,
      "combinedImageSamplerDescriptorSingleArray", false,
      descriptor_properties->combinedImageSamplerDescriptorSingleArray ==
          VK_TRUE,
      descriptor_properties->combinedImageSamplerDescriptorSingleArray
          ? "actual=VK_TRUE"
          : "actual=VK_FALSE");
  v0_report_add(
      &candidate->report, V0_REPORT_LIMIT, "bufferlessPushDescriptors", false,
      descriptor_properties->bufferlessPushDescriptors == VK_TRUE,
      descriptor_properties->bufferlessPushDescriptors ? "actual=VK_TRUE"
                                                       : "actual=VK_FALSE");
  v0_report_add(
      &candidate->report, V0_REPORT_LIMIT,
      "allowSamplerImageViewPostSubmitCreation", false,
      descriptor_properties->allowSamplerImageViewPostSubmitCreation == VK_TRUE,
      descriptor_properties->allowSamplerImageViewPostSubmitCreation
          ? "actual=VK_TRUE"
          : "actual=VK_FALSE");
  v0_report_record_u64(
      &candidate->report, "maxEmbeddedImmutableSamplerBindings",
      descriptor_properties->maxEmbeddedImmutableSamplerBindings);
  v0_report_record_u64(&candidate->report, "maxEmbeddedImmutableSamplers",
                       descriptor_properties->maxEmbeddedImmutableSamplers);
  v0_report_record_u64(
      &candidate->report, "bufferCaptureReplayDescriptorDataSize",
      descriptor_properties->bufferCaptureReplayDescriptorDataSize);
  v0_report_record_u64(
      &candidate->report, "imageCaptureReplayDescriptorDataSize",
      descriptor_properties->imageCaptureReplayDescriptorDataSize);
  v0_report_record_u64(
      &candidate->report, "imageViewCaptureReplayDescriptorDataSize",
      descriptor_properties->imageViewCaptureReplayDescriptorDataSize);
  v0_report_record_u64(
      &candidate->report, "samplerCaptureReplayDescriptorDataSize",
      descriptor_properties->samplerCaptureReplayDescriptorDataSize);
  v0_report_record_u64(
      &candidate->report,
      "accelerationStructureCaptureReplayDescriptorDataSize",
      descriptor_properties
          ->accelerationStructureCaptureReplayDescriptorDataSize);
  v0_report_record_u64(
      &candidate->report, "combinedImageSamplerDescriptorSize",
      descriptor_properties->combinedImageSamplerDescriptorSize);
  v0_report_record_u64(&candidate->report, "uniformTexelBufferDescriptorSize",
                       descriptor_properties->uniformTexelBufferDescriptorSize);
  v0_report_record_u64(
      &candidate->report, "robustUniformTexelBufferDescriptorSize",
      descriptor_properties->robustUniformTexelBufferDescriptorSize);
  v0_report_record_u64(&candidate->report, "storageTexelBufferDescriptorSize",
                       descriptor_properties->storageTexelBufferDescriptorSize);
  v0_report_record_u64(
      &candidate->report, "robustStorageTexelBufferDescriptorSize",
      descriptor_properties->robustStorageTexelBufferDescriptorSize);
  v0_report_record_u64(&candidate->report, "uniformBufferDescriptorSize",
                       descriptor_properties->uniformBufferDescriptorSize);
  v0_report_record_u64(
      &candidate->report, "robustUniformBufferDescriptorSize",
      descriptor_properties->robustUniformBufferDescriptorSize);
  v0_report_record_u64(&candidate->report, "storageBufferDescriptorSize",
                       descriptor_properties->storageBufferDescriptorSize);
  v0_report_record_u64(
      &candidate->report, "robustStorageBufferDescriptorSize",
      descriptor_properties->robustStorageBufferDescriptorSize);
  v0_report_record_u64(&candidate->report, "inputAttachmentDescriptorSize",
                       descriptor_properties->inputAttachmentDescriptorSize);
  v0_report_record_u64(
      &candidate->report, "accelerationStructureDescriptorSize",
      descriptor_properties->accelerationStructureDescriptorSize);
  v0_report_record_u64(&candidate->report, "maxSamplerDescriptorBufferRange",
                       descriptor_properties->maxSamplerDescriptorBufferRange);
  v0_report_record_u64(&candidate->report, "maxResourceDescriptorBufferRange",
                       descriptor_properties->maxResourceDescriptorBufferRange);
  v0_report_record_u64(
      &candidate->report, "samplerDescriptorBufferAddressSpaceSize",
      descriptor_properties->samplerDescriptorBufferAddressSpaceSize);
  v0_report_record_u64(
      &candidate->report, "resourceDescriptorBufferAddressSpaceSize",
      descriptor_properties->resourceDescriptorBufferAddressSpaceSize);
  v0_report_record_u64(&candidate->report, "descriptorBufferAddressSpaceSize",
                       descriptor_properties->descriptorBufferAddressSpaceSize);

  const bool queue_present = candidate->queue_family_index != UINT32_MAX;
  char queue_detail[96];
  snprintf(queue_detail, sizeof(queue_detail), "family=%u required-flags=G|C|T",
           candidate->queue_family_index);
  v0_report_add(&candidate->report, V0_REPORT_QUEUE,
                "graphics+compute+transfer", true, queue_present, queue_detail);

  VkFormatProperties3 format_properties3 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
  };
  VkFormatProperties2 format_properties = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &format_properties3,
  };
  vkGetPhysicalDeviceFormatProperties2(
      physical_device, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
  const VkFormatFeatureFlags2 texture_features =
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
  const VkFormatFeatureFlags2 target_features =
      VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
  const VkFormatFeatureFlags2 optimal_features =
      format_properties3.optimalTilingFeatures;
  v0_report_add(&candidate->report, V0_REPORT_FORMAT,
                "R8G8B8A8_UNORM sampled+transfer-dst", true,
                (optimal_features & texture_features) == texture_features,
                "optimal tiling");
  v0_report_add(&candidate->report, V0_REPORT_FORMAT,
                "R8G8B8A8_UNORM color+transfer-src", true,
                (optimal_features & target_features) == target_features,
                "optimal tiling");

  const bool has_surface =
      v0_extension_present(instance_extensions, instance_extension_count,
                           VK_KHR_SURFACE_EXTENSION_NAME);
  const bool has_surface_caps2 =
      v0_extension_present(instance_extensions, instance_extension_count,
                           VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
  const bool has_surface_maintenance =
      v0_extension_present(instance_extensions, instance_extension_count,
                           VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#if defined(_WIN32)
  const bool has_platform_surface =
      v0_extension_present(instance_extensions, instance_extension_count,
                           VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#else
  const bool has_platform_surface = false;
#endif
  v0_report_add(&candidate->report, V0_REPORT_INSTANCE_EXTENSION,
                "window.VK_KHR_surface", false, has_surface,
                "window profile preflight");
  v0_report_add(&candidate->report, V0_REPORT_INSTANCE_EXTENSION,
                "window.VK_KHR_get_surface_capabilities2", false,
                has_surface_caps2, "window profile preflight");
  v0_report_add(&candidate->report, V0_REPORT_INSTANCE_EXTENSION,
                "window.VK_KHR_surface_maintenance1", false,
                has_surface_maintenance, "window profile preflight");
  v0_report_add(&candidate->report, V0_REPORT_INSTANCE_EXTENSION,
                "window.VK_KHR_win32_surface", false, has_platform_surface,
                "window profile preflight");
  v0_report_add(&candidate->report, V0_REPORT_DEVICE_EXTENSION,
                "window.VK_KHR_swapchain", false,
                candidate->has_swapchain_extension, "window profile preflight");
  v0_report_add(&candidate->report, V0_REPORT_DEVICE_EXTENSION,
                "window.VK_KHR_swapchain_maintenance1", false,
                candidate->has_swapchain_maintenance_extension,
                "window profile preflight");
  v0_report_add(&candidate->report, V0_REPORT_FEATURE,
                "window.swapchainMaintenance1", false,
                candidate->features.swapchain_maintenance1,
                "window profile preflight");

  candidate->common_viable =
      v0_report_required_entries_present(&candidate->report);
  candidate->window_preflight_viable =
      candidate->common_viable && has_surface && has_surface_caps2 &&
      has_surface_maintenance && has_platform_surface &&
      candidate->has_swapchain_extension &&
      candidate->has_swapchain_maintenance_extension &&
      candidate->features.swapchain_maintenance1;

  switch (candidate->properties.properties.deviceType) {
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    candidate->score = 400u;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    candidate->score = 300u;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    candidate->score = 200u;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_CPU:
    candidate->score = 100u;
    break;
  default:
    candidate->score = 0u;
    break;
  }
}

static bool v0_enumerate_candidates(V0Context *context) {
  uint32_t instance_extension_count = 0;
  VkResult result = vkEnumerateInstanceExtensionProperties(
      NULL, &instance_extension_count, NULL);
  if (result != VK_SUCCESS || instance_extension_count > V0_MAX_EXTENSIONS) {
    return false;
  }
  VkExtensionProperties instance_extensions[V0_MAX_EXTENSIONS];
  result = vkEnumerateInstanceExtensionProperties(
      NULL, &instance_extension_count, instance_extensions);
  if (result != VK_SUCCESS) {
    return false;
  }

  uint32_t device_count = 0;
  result = vkEnumeratePhysicalDevices(context->instance, &device_count, NULL);
  if (result != VK_SUCCESS || device_count == 0u ||
      device_count > V0_MAX_DEVICES) {
    fprintf(stderr, "physical-device count=%u exceeds supported range 1..%u\n",
            device_count, V0_MAX_DEVICES);
    return false;
  }
  VkPhysicalDevice devices[V0_MAX_DEVICES];
  result =
      vkEnumeratePhysicalDevices(context->instance, &device_count, devices);
  if (result != VK_SUCCESS) {
    return false;
  }
  context->candidate_count = device_count;
  for (uint32_t i = 0; i < device_count; ++i) {
    v0_query_candidate(&context->candidates[i], devices[i], instance_extensions,
                       instance_extension_count);
  }
  return true;
}

static bool v0_load_descriptor_buffer_functions(V0Context *context) {
  context->get_layout_size =
      (PFN_vkGetDescriptorSetLayoutSizeEXT)vkGetDeviceProcAddr(
          context->device, "vkGetDescriptorSetLayoutSizeEXT");
  context->get_binding_offset =
      (PFN_vkGetDescriptorSetLayoutBindingOffsetEXT)vkGetDeviceProcAddr(
          context->device, "vkGetDescriptorSetLayoutBindingOffsetEXT");
  context->get_descriptor = (PFN_vkGetDescriptorEXT)vkGetDeviceProcAddr(
      context->device, "vkGetDescriptorEXT");
  context->cmd_bind_descriptor_buffers =
      (PFN_vkCmdBindDescriptorBuffersEXT)vkGetDeviceProcAddr(
          context->device, "vkCmdBindDescriptorBuffersEXT");
  context->cmd_set_descriptor_offsets =
      (PFN_vkCmdSetDescriptorBufferOffsetsEXT)vkGetDeviceProcAddr(
          context->device, "vkCmdSetDescriptorBufferOffsetsEXT");
  return context->get_layout_size && context->get_binding_offset &&
         context->get_descriptor && context->cmd_bind_descriptor_buffers &&
         context->cmd_set_descriptor_offsets;
}

static void v0_destroy_candidate_device(V0Context *context) {
  if (!context->device) {
    return;
  }
  if (context->resource_layout.layout) {
    vkDestroyDescriptorSetLayout(context->device,
                                 context->resource_layout.layout, NULL);
  }
  if (context->sampler_layout.layout) {
    vkDestroyDescriptorSetLayout(context->device,
                                 context->sampler_layout.layout, NULL);
  }
  memset(&context->resource_layout, 0, sizeof(context->resource_layout));
  memset(&context->sampler_layout, 0, sizeof(context->sampler_layout));
  vkDestroyDevice(context->device, NULL);
  context->device = VK_NULL_HANDLE;
  context->queue = VK_NULL_HANDLE;
  context->get_layout_size = NULL;
  context->get_binding_offset = NULL;
  context->get_descriptor = NULL;
  context->cmd_bind_descriptor_buffers = NULL;
  context->cmd_set_descriptor_offsets = NULL;
}

static bool v0_create_candidate_layouts(V0Context *context,
                                        V0Candidate *candidate) {
  VkDescriptorSetLayoutBinding resource_bindings[2] = {
      {
          .binding = 0u,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = V0_SAMPLED_IMAGE_CAPACITY,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
      {
          .binding = 1u,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = V0_STORAGE_IMAGE_CAPACITY,
          .stageFlags =
              VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
      },
  };
  VkDescriptorSetLayoutCreateInfo resource_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
      .bindingCount = (uint32_t)V0_ARRAY_COUNT(resource_bindings),
      .pBindings = resource_bindings,
  };
  VkDescriptorSetLayoutBinding sampler_binding = {
      .binding = 0u,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
      .descriptorCount = V0_SAMPLER_CAPACITY,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  VkDescriptorSetLayoutCreateInfo sampler_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
      .bindingCount = 1u,
      .pBindings = &sampler_binding,
  };

  VkDescriptorSetLayoutSupport resource_support = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
  };
  VkDescriptorSetLayoutSupport sampler_support = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
  };
  vkGetDescriptorSetLayoutSupport(context->device, &resource_create_info,
                                  &resource_support);
  vkGetDescriptorSetLayoutSupport(context->device, &sampler_create_info,
                                  &sampler_support);
  v0_report_add(&candidate->report, V0_REPORT_LAYOUT,
                "resource descriptor-buffer layout support", true,
                resource_support.supported == VK_TRUE,
                "vkGetDescriptorSetLayoutSupport");
  v0_report_add(&candidate->report, V0_REPORT_LAYOUT,
                "sampler descriptor-buffer layout support", true,
                sampler_support.supported == VK_TRUE,
                "vkGetDescriptorSetLayoutSupport");
  if (!resource_support.supported || !sampler_support.supported) {
    return false;
  }

  VkResult result =
      vkCreateDescriptorSetLayout(context->device, &resource_create_info, NULL,
                                  &context->resource_layout.layout);
  if (result != VK_SUCCESS) {
    v0_report_add(&candidate->report, V0_REPORT_LAYOUT,
                  "resource descriptor-buffer layout create", true, false,
                  v0_vk_result_name(result));
    return false;
  }
  result = vkCreateDescriptorSetLayout(context->device, &sampler_create_info,
                                       NULL, &context->sampler_layout.layout);
  if (result != VK_SUCCESS) {
    v0_report_add(&candidate->report, V0_REPORT_LAYOUT,
                  "sampler descriptor-buffer layout create", true, false,
                  v0_vk_result_name(result));
    return false;
  }

  context->get_layout_size(context->device, context->resource_layout.layout,
                           &context->resource_layout.size);
  context->get_layout_size(context->device, context->sampler_layout.layout,
                           &context->sampler_layout.size);
  context->get_binding_offset(context->device, context->resource_layout.layout,
                              0u,
                              &context->resource_layout.sampled_image_offset);
  context->get_binding_offset(context->device, context->resource_layout.layout,
                              1u,
                              &context->resource_layout.storage_image_offset);
  context->get_binding_offset(context->device, context->sampler_layout.layout,
                              0u, &context->sampler_layout.sampler_offset);

  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      &candidate->descriptor_properties;
  const bool resource_range_ok = context->resource_layout.size <=
                                 properties->maxResourceDescriptorBufferRange;
  const bool sampler_range_ok = context->sampler_layout.size <=
                                properties->maxSamplerDescriptorBufferRange;
  const bool resource_space_ok =
      context->resource_layout.size <=
      properties->resourceDescriptorBufferAddressSpaceSize;
  const bool sampler_space_ok =
      context->sampler_layout.size <=
      properties->samplerDescriptorBufferAddressSpaceSize;
  const uint64_t combined_size =
      context->resource_layout.size + context->sampler_layout.size;
  const bool total_space_ok =
      combined_size <= properties->descriptorBufferAddressSpaceSize;
  char detail[192];
  snprintf(detail, sizeof(detail), "actual=%" PRIu64 " maximum=%" PRIu64,
           (uint64_t)context->resource_layout.size,
           (uint64_t)properties->maxResourceDescriptorBufferRange);
  v0_report_add(&candidate->report, V0_REPORT_LIMIT,
                "resource descriptor layout bytes", true, resource_range_ok,
                detail);
  snprintf(detail, sizeof(detail), "actual=%" PRIu64 " maximum=%" PRIu64,
           (uint64_t)context->sampler_layout.size,
           (uint64_t)properties->maxSamplerDescriptorBufferRange);
  v0_report_add(&candidate->report, V0_REPORT_LIMIT,
                "sampler descriptor layout bytes", true, sampler_range_ok,
                detail);
  snprintf(detail, sizeof(detail),
           "resource=%" PRIu64 " sampler=%" PRIu64 " total=%" PRIu64,
           (uint64_t)context->resource_layout.size,
           (uint64_t)context->sampler_layout.size, combined_size);
  v0_report_add(
      &candidate->report, V0_REPORT_LIMIT, "descriptor buffer address spaces",
      true, resource_space_ok && sampler_space_ok && total_space_ok, detail);
  printf("DESCRIPTOR LAYOUT resource=%" PRIu64 " sampled-offset=%" PRIu64
         " storage-offset=%" PRIu64 " sampled-stride=%zu storage-stride=%zu\n",
         (uint64_t)context->resource_layout.size,
         (uint64_t)context->resource_layout.sampled_image_offset,
         (uint64_t)context->resource_layout.storage_image_offset,
         properties->sampledImageDescriptorSize,
         properties->storageImageDescriptorSize);
  printf("DESCRIPTOR LAYOUT sampler=%" PRIu64 " sampler-offset=%" PRIu64
         " sampler-stride=%zu\n",
         (uint64_t)context->sampler_layout.size,
         (uint64_t)context->sampler_layout.sampler_offset,
         properties->samplerDescriptorSize);
  return resource_range_ok && sampler_range_ok && resource_space_ok &&
         sampler_space_ok && total_space_ok;
}

static bool v0_try_create_candidate_device(V0Context *context,
                                           V0Candidate *candidate) {
  float queue_priority = 1.0f;
  VkDeviceQueueCreateInfo queue_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = candidate->queue_family_index,
      .queueCount = 1u,
      .pQueuePriorities = &queue_priority,
  };
  VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
      .descriptorBuffer = VK_TRUE,
      .descriptorBufferCaptureReplay =
          (context->options.validation &&
           candidate->features.descriptor_buffer_capture_replay)
              ? VK_TRUE
              : VK_FALSE,
  };
  VkPhysicalDeviceVulkan14Features features14 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
      .pNext = &descriptor_features,
      .maintenance5 = VK_TRUE,
  };
  VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &features14,
      .synchronization2 = VK_TRUE,
      .dynamicRendering = VK_TRUE,
      .maintenance4 = VK_TRUE,
  };
  VkPhysicalDeviceVulkan12Features features12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = &features13,
      .descriptorIndexing = VK_TRUE,
      .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
      .shaderStorageImageArrayNonUniformIndexing = VK_TRUE,
      .runtimeDescriptorArray = VK_TRUE,
      .scalarBlockLayout = VK_TRUE,
      .hostQueryReset = VK_TRUE,
      .timelineSemaphore = VK_TRUE,
      .bufferDeviceAddress = VK_TRUE,
  };
  VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &features12,
      .features = {.shaderInt64 = VK_TRUE},
  };
  const char *device_extensions[] = {
      VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
  };
  VkDeviceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features2,
      .queueCreateInfoCount = 1u,
      .pQueueCreateInfos = &queue_create_info,
      .enabledExtensionCount = (uint32_t)V0_ARRAY_COUNT(device_extensions),
      .ppEnabledExtensionNames = device_extensions,
  };
  VkResult result = vkCreateDevice(candidate->physical_device, &create_info,
                                   NULL, &context->device);
  if (result != VK_SUCCESS) {
    char detail[128];
    snprintf(detail, sizeof(detail), "%s (%d)", v0_vk_result_name(result),
             result);
    v0_report_add(&candidate->report, V0_REPORT_DEVICE_CREATE, "vkCreateDevice",
                  true, false, detail);
    return false;
  }
  v0_report_add(&candidate->report, V0_REPORT_DEVICE_CREATE, "vkCreateDevice",
                true, true, "full feature floor enabled");
  vkGetDeviceQueue(context->device, candidate->queue_family_index, 0u,
                   &context->queue);
  if (!v0_load_descriptor_buffer_functions(context)) {
    v0_report_add(&candidate->report, V0_REPORT_DEVICE_CREATE,
                  "descriptor-buffer function table", true, false,
                  "required vkGetDeviceProcAddr result was NULL");
    return false;
  }
  v0_report_add(&candidate->report, V0_REPORT_DEVICE_CREATE,
                "descriptor-buffer function table", true, true,
                "all required entry points resolved");
  return v0_create_candidate_layouts(context, candidate);
}

static bool v0_select_device(V0Context *context) {
  bool attempted[V0_MAX_DEVICES] = {false};
  for (uint32_t attempt = 0; attempt < context->candidate_count; ++attempt) {
    uint32_t best_index = UINT32_MAX;
    uint32_t best_score = 0u;
    for (uint32_t i = 0; i < context->candidate_count; ++i) {
      const V0Candidate *candidate = &context->candidates[i];
      if (!attempted[i] && candidate->common_viable &&
          (best_index == UINT32_MAX || candidate->score > best_score)) {
        best_index = i;
        best_score = candidate->score;
      }
    }
    if (best_index == UINT32_MAX) {
      break;
    }
    attempted[best_index] = true;
    V0Candidate *candidate = &context->candidates[best_index];
    if (v0_try_create_candidate_device(context, candidate) &&
        v0_report_required_entries_present(&candidate->report)) {
      context->selected_candidate_index = best_index;
      context->selected = candidate;
      candidate->common_viable = true;
      return true;
    }
    candidate->common_viable = false;
    v0_destroy_candidate_device(context);
  }
  return false;
}

static bool v0_memory_type_has_flags(const V0Candidate *candidate,
                                     uint32_t index,
                                     VkMemoryPropertyFlags flags) {
  return (candidate->memory_properties.memoryTypes[index].propertyFlags &
          flags) == flags;
}

static bool v0_choose_memory_type(const V0Context *context,
                                  uint32_t memory_type_bits,
                                  VkMemoryPropertyFlags required,
                                  const VkMemoryPropertyFlags *preferences,
                                  uint32_t preference_count,
                                  uint32_t *out_index,
                                  VkMemoryPropertyFlags *out_properties) {
  const V0Candidate *candidate = context->selected;
  for (uint32_t preference = 0; preference < preference_count; ++preference) {
    const VkMemoryPropertyFlags wanted = required | preferences[preference];
    for (uint32_t i = 0; i < candidate->memory_properties.memoryTypeCount;
         ++i) {
      if ((memory_type_bits & (1u << i)) &&
          v0_memory_type_has_flags(candidate, i, wanted)) {
        *out_index = i;
        *out_properties =
            candidate->memory_properties.memoryTypes[i].propertyFlags;
        return true;
      }
    }
  }
  for (uint32_t i = 0; i < candidate->memory_properties.memoryTypeCount; ++i) {
    if ((memory_type_bits & (1u << i)) &&
        v0_memory_type_has_flags(candidate, i, required)) {
      *out_index = i;
      *out_properties =
          candidate->memory_properties.memoryTypes[i].propertyFlags;
      return true;
    }
  }
  return false;
}

static void v0_destroy_buffer(V0Context *context, V0Buffer *buffer) {
  if (buffer->allocation.mapped) {
    vkUnmapMemory(context->device, buffer->allocation.memory);
  }
  if (buffer->buffer) {
    vkDestroyBuffer(context->device, buffer->buffer, NULL);
  }
  if (buffer->allocation.memory) {
    vkFreeMemory(context->device, buffer->allocation.memory, NULL);
  }
  memset(buffer, 0, sizeof(*buffer));
}

static bool v0_create_buffer(V0Context *context, VkDeviceSize size,
                             VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags required_memory,
                             const VkMemoryPropertyFlags *preferences,
                             uint32_t preference_count, bool map,
                             V0Buffer *out_buffer) {
  memset(out_buffer, 0, sizeof(*out_buffer));
  out_buffer->size = size;
  VkBufferCreateInfo buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkResult result = vkCreateBuffer(context->device, &buffer_create_info, NULL,
                                   &out_buffer->buffer);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkCreateBuffer(size=%" PRIu64 ") failed: %s (%d)\n",
            (uint64_t)size, v0_vk_result_name(result), result);
    return false;
  }

  VkMemoryDedicatedRequirements dedicated_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
  };
  VkMemoryRequirements2 memory_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      .pNext = &dedicated_requirements,
  };
  VkBufferMemoryRequirementsInfo2 requirements_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
      .buffer = out_buffer->buffer,
  };
  vkGetBufferMemoryRequirements2(context->device, &requirements_info,
                                 &memory_requirements);
  V0Allocation *allocation = &out_buffer->allocation;
  if (!v0_choose_memory_type(
          context, memory_requirements.memoryRequirements.memoryTypeBits,
          required_memory, preferences, preference_count,
          &allocation->memory_type_index, &allocation->properties)) {
    fprintf(stderr,
            "no compatible memory type for buffer memoryTypeBits=0x%x\n",
            memory_requirements.memoryRequirements.memoryTypeBits);
    v0_destroy_buffer(context, out_buffer);
    return false;
  }

  const bool device_address =
      (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
  VkMemoryDedicatedAllocateInfo dedicated_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .buffer = out_buffer->buffer,
  };
  VkMemoryAllocateFlagsInfo allocate_flags = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
      .pNext = &dedicated_allocate_info,
      .flags = device_address ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0u,
  };
  VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &allocate_flags,
      .allocationSize = memory_requirements.memoryRequirements.size,
      .memoryTypeIndex = allocation->memory_type_index,
  };
  allocation->size = allocate_info.allocationSize;
  result = vkAllocateMemory(context->device, &allocate_info, NULL,
                            &allocation->memory);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkAllocateMemory(buffer) failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    v0_destroy_buffer(context, out_buffer);
    return false;
  }
  result = vkBindBufferMemory(context->device, out_buffer->buffer,
                              allocation->memory, 0u);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkBindBufferMemory failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    v0_destroy_buffer(context, out_buffer);
    return false;
  }
  if (map) {
    result = vkMapMemory(context->device, allocation->memory, 0u,
                         allocation->size, 0u, &allocation->mapped);
    if (result != VK_SUCCESS) {
      fprintf(stderr, "vkMapMemory(buffer) failed: %s (%d)\n",
              v0_vk_result_name(result), result);
      v0_destroy_buffer(context, out_buffer);
      return false;
    }
  }
  if (device_address) {
    VkBufferDeviceAddressInfo address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = out_buffer->buffer,
    };
    out_buffer->address =
        vkGetBufferDeviceAddress(context->device, &address_info);
    if (out_buffer->address == 0u) {
      fprintf(stderr, "vkGetBufferDeviceAddress returned zero\n");
      v0_destroy_buffer(context, out_buffer);
      return false;
    }
  }
  printf("BUFFER size=%" PRIu64 " alloc=%" PRIu64
         " memoryType=%u flags=0x%x address=0x%" PRIx64
         " dedicated-required=%u dedicated-preferred=%u\n",
         (uint64_t)size, (uint64_t)allocation->size,
         allocation->memory_type_index, allocation->properties,
         (uint64_t)out_buffer->address,
         dedicated_requirements.requiresDedicatedAllocation,
         dedicated_requirements.prefersDedicatedAllocation);
  return true;
}

static void v0_destroy_image(V0Context *context, V0Image *image) {
  if (image->view) {
    vkDestroyImageView(context->device, image->view, NULL);
  }
  if (image->image) {
    vkDestroyImage(context->device, image->image, NULL);
  }
  if (image->allocation.memory) {
    vkFreeMemory(context->device, image->allocation.memory, NULL);
  }
  memset(image, 0, sizeof(*image));
}

static bool v0_create_image(V0Context *context, uint32_t width, uint32_t height,
                            VkImageUsageFlags usage, V0Image *out_image) {
  memset(out_image, 0, sizeof(*out_image));
  VkImageCreateInfo image_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {.width = width, .height = height, .depth = 1u},
      .mipLevels = 1u,
      .arrayLayers = 1u,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  VkResult result = vkCreateImage(context->device, &image_create_info, NULL,
                                  &out_image->image);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkCreateImage failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }

  VkMemoryDedicatedRequirements dedicated_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
  };
  VkMemoryRequirements2 memory_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      .pNext = &dedicated_requirements,
  };
  VkImageMemoryRequirementsInfo2 requirements_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .image = out_image->image,
  };
  vkGetImageMemoryRequirements2(context->device, &requirements_info,
                                &memory_requirements);
  const VkMemoryPropertyFlags preferences[] = {
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
  };
  V0Allocation *allocation = &out_image->allocation;
  if (!v0_choose_memory_type(
          context, memory_requirements.memoryRequirements.memoryTypeBits,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, preferences,
          (uint32_t)V0_ARRAY_COUNT(preferences), &allocation->memory_type_index,
          &allocation->properties)) {
    fprintf(stderr,
            "no device-local memory type for image memoryTypeBits=0x%x\n",
            memory_requirements.memoryRequirements.memoryTypeBits);
    v0_destroy_image(context, out_image);
    return false;
  }
  VkMemoryDedicatedAllocateInfo dedicated_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = out_image->image,
  };
  VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated_allocate_info,
      .allocationSize = memory_requirements.memoryRequirements.size,
      .memoryTypeIndex = allocation->memory_type_index,
  };
  allocation->size = allocate_info.allocationSize;
  result = vkAllocateMemory(context->device, &allocate_info, NULL,
                            &allocation->memory);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkAllocateMemory(image) failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    v0_destroy_image(context, out_image);
    return false;
  }
  result = vkBindImageMemory(context->device, out_image->image,
                             allocation->memory, 0u);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkBindImageMemory failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    v0_destroy_image(context, out_image);
    return false;
  }
  VkImageViewCreateInfo view_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = out_image->image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0u,
              .levelCount = 1u,
              .baseArrayLayer = 0u,
              .layerCount = 1u,
          },
  };
  result = vkCreateImageView(context->device, &view_create_info, NULL,
                             &out_image->view);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkCreateImageView failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    v0_destroy_image(context, out_image);
    return false;
  }
  printf("IMAGE %ux%u alloc=%" PRIu64 " memoryType=%u flags=0x%x"
         " dedicated-required=%u dedicated-preferred=%u\n",
         width, height, (uint64_t)allocation->size,
         allocation->memory_type_index, allocation->properties,
         dedicated_requirements.requiresDedicatedAllocation,
         dedicated_requirements.prefersDedicatedAllocation);
  return true;
}

static VkDeviceSize v0_align_down(VkDeviceSize value, VkDeviceSize alignment) {
  return value & ~(alignment - 1u);
}

static VkDeviceSize v0_align_up(VkDeviceSize value, VkDeviceSize alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static bool v0_flush_range(V0Context *context, const V0Allocation *allocation,
                           VkDeviceSize offset, VkDeviceSize size) {
  if (allocation->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    return true;
  }
  const VkDeviceSize atom =
      context->selected->properties.properties.limits.nonCoherentAtomSize;
  const VkDeviceSize start = v0_align_down(offset, atom);
  VkDeviceSize end = v0_align_up(offset + size, atom);
  if (end > allocation->size) {
    end = allocation->size;
  }
  VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = allocation->memory,
      .offset = start,
      .size = end - start,
  };
  VkResult result = vkFlushMappedMemoryRanges(context->device, 1u, &range);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkFlushMappedMemoryRanges failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }
  return true;
}

static bool v0_invalidate_range(V0Context *context,
                                const V0Allocation *allocation,
                                VkDeviceSize offset, VkDeviceSize size) {
  if (allocation->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    return true;
  }
  const VkDeviceSize atom =
      context->selected->properties.properties.limits.nonCoherentAtomSize;
  const VkDeviceSize start = v0_align_down(offset, atom);
  VkDeviceSize end = v0_align_up(offset + size, atom);
  if (end > allocation->size) {
    end = allocation->size;
  }
  VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = allocation->memory,
      .offset = start,
      .size = end - start,
  };
  VkResult result = vkInvalidateMappedMemoryRanges(context->device, 1u, &range);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkInvalidateMappedMemoryRanges failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }
  return true;
}

static bool v0_write_upload_data(V0Context *context) {
  if (V0_TEXTURE_OFFSET + 4u > context->upload_buffer.size) {
    return false;
  }
  uint8_t *mapped = (uint8_t *)context->upload_buffer.allocation.mapped;
  memset(mapped, 0, (size_t)context->upload_buffer.size);

  const V0Vertex vertices[3] = {
      {{-1.0f, -1.0f}, {0.0f, 0.0f}},
      {{3.0f, -1.0f}, {2.0f, 0.0f}},
      {{-1.0f, 3.0f}, {0.0f, 2.0f}},
  };
  const uint16_t indices[3] = {0u, 1u, 2u};
  V0DrawRoot root;
  memset(&root, 0, sizeof(root));
  root.vertices = context->upload_buffer.address + V0_VERTEX_OFFSET;
  root.tint[0] = 1.0f;
  root.tint[1] = 1.0f;
  root.tint[2] = 1.0f;
  root.tint[3] = 1.0f;
  root.transform[0] = 1.0f;
  root.transform[5] = 1.0f;
  root.transform[10] = 1.0f;
  root.transform[15] = 1.0f;
  root.texture_index = 0u;
  root.sampler_index = 0u;
  const uint8_t texture_pixel[4] = {255u, 0u, 255u, 255u};

  memcpy(mapped + V0_ROOT_OFFSET, &root, sizeof(root));
  memcpy(mapped + V0_VERTEX_OFFSET, vertices, sizeof(vertices));
  memcpy(mapped + V0_INDEX_OFFSET, indices, sizeof(indices));
  memcpy(mapped + V0_TEXTURE_OFFSET, texture_pixel, sizeof(texture_pixel));
  return v0_flush_range(context, &context->upload_buffer.allocation, 0u,
                        V0_TEXTURE_OFFSET + sizeof(texture_pixel));
}

static bool v0_create_resources(V0Context *context) {
  const VkMemoryPropertyFlags upload_preferences[] = {
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      0u,
  };
  const VkMemoryPropertyFlags readback_preferences[] = {
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      0u,
  };
  const VkBufferUsageFlags resource_descriptor_usage =
      VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  const VkBufferUsageFlags sampler_descriptor_usage =
      VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (!v0_create_buffer(context, context->resource_layout.size,
                        resource_descriptor_usage,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, upload_preferences,
                        (uint32_t)V0_ARRAY_COUNT(upload_preferences), true,
                        &context->resource_descriptor_buffer) ||
      !v0_create_buffer(context, context->sampler_layout.size,
                        sampler_descriptor_usage,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, upload_preferences,
                        (uint32_t)V0_ARRAY_COUNT(upload_preferences), true,
                        &context->sampler_descriptor_buffer) ||
      !v0_create_buffer(context, V0_UPLOAD_SIZE,
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, upload_preferences,
                        (uint32_t)V0_ARRAY_COUNT(upload_preferences), true,
                        &context->upload_buffer) ||
      !v0_create_buffer(
          context, V0_TARGET_WIDTH * V0_TARGET_HEIGHT * 4u,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
          readback_preferences, (uint32_t)V0_ARRAY_COUNT(readback_preferences),
          true, &context->readback_buffer) ||
      !v0_create_image(context, 1u, 1u,
                       VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       &context->sampled_image) ||
      !v0_create_image(context, V0_TARGET_WIDTH, V0_TARGET_HEIGHT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       &context->target_image)) {
    return false;
  }

  const VkDeviceSize descriptor_alignment =
      context->selected->descriptor_properties.descriptorBufferOffsetAlignment;
  if ((context->resource_descriptor_buffer.address % descriptor_alignment) !=
          0u ||
      (context->sampler_descriptor_buffer.address % descriptor_alignment) !=
          0u) {
    fprintf(stderr,
            "descriptor buffer address alignment failed: alignment=%" PRIu64
            " resource=0x%" PRIx64 " sampler=0x%" PRIx64 "\n",
            (uint64_t)descriptor_alignment,
            (uint64_t)context->resource_descriptor_buffer.address,
            (uint64_t)context->sampler_descriptor_buffer.address);
    return false;
  }
  if ((context->upload_buffer.address % _Alignof(V0DrawRoot)) != 0u ||
      ((context->upload_buffer.address + V0_VERTEX_OFFSET) %
       _Alignof(V0Vertex)) != 0u) {
    fprintf(stderr,
            "upload buffer device addresses violate host ABI alignment\n");
    return false;
  }
  if (!v0_write_upload_data(context)) {
    return false;
  }

  VkSamplerCreateInfo sampler_create_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0f,
  };
  VkResult result = vkCreateSampler(context->device, &sampler_create_info, NULL,
                                    &context->sampler);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkCreateSampler failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }

  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      &context->selected->descriptor_properties;
  uint8_t *resource_base =
      (uint8_t *)context->resource_descriptor_buffer.allocation.mapped;
  uint8_t *sampler_base =
      (uint8_t *)context->sampler_descriptor_buffer.allocation.mapped;
  memset(resource_base, 0, (size_t)context->resource_layout.size);
  memset(sampler_base, 0, (size_t)context->sampler_layout.size);

  VkDescriptorImageInfo sampled_image_info = {
      .imageView = context->sampled_image.view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorGetInfoEXT sampled_get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .data.pSampledImage = &sampled_image_info,
  };
  context->get_descriptor(context->device, &sampled_get_info,
                          properties->sampledImageDescriptorSize,
                          resource_base +
                              context->resource_layout.sampled_image_offset);

  VkDescriptorGetInfoEXT sampler_get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLER,
      .data.pSampler = &context->sampler,
  };
  context->get_descriptor(
      context->device, &sampler_get_info, properties->samplerDescriptorSize,
      sampler_base + context->sampler_layout.sampler_offset);

  if (!v0_flush_range(context, &context->resource_descriptor_buffer.allocation,
                      0u, context->resource_layout.size) ||
      !v0_flush_range(context, &context->sampler_descriptor_buffer.allocation,
                      0u, context->sampler_layout.size)) {
    return false;
  }
  printf("DESCRIPTOR PUBLISH sampled-image[0] bytes=%zu sampler[0] bytes=%zu\n",
         properties->sampledImageDescriptorSize,
         properties->samplerDescriptorSize);
  printf("UPLOAD root-address=0x%" PRIx64 " vertex-address=0x%" PRIx64
         " index-offset=%u\n",
         (uint64_t)(context->upload_buffer.address + V0_ROOT_OFFSET),
         (uint64_t)(context->upload_buffer.address + V0_VERTEX_OFFSET),
         V0_INDEX_OFFSET);
  return true;
}

static bool v0_create_shader_module(V0Context *context, const char *path,
                                    VkShaderModule *out_shader) {
  uint8_t *bytes = NULL;
  size_t size = 0;
  if (!v0_read_file(path, &bytes, &size) || size % sizeof(uint32_t) != 0u) {
    free(bytes);
    return false;
  }
  VkShaderModuleCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size,
      .pCode = (const uint32_t *)bytes,
  };
  VkResult result =
      vkCreateShaderModule(context->device, &create_info, NULL, out_shader);
  free(bytes);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkCreateShaderModule(%s) failed: %s (%d)\n", path,
            v0_vk_result_name(result), result);
    return false;
  }
  return true;
}

static bool v0_create_pipeline(V0Context *context) {
  if (!v0_create_shader_module(context, VKR_V0_VERTEX_SPV,
                               &context->vertex_shader) ||
      !v0_create_shader_module(context, VKR_V0_FRAGMENT_SPV,
                               &context->fragment_shader)) {
    return false;
  }
  VkDescriptorSetLayout set_layouts[] = {
      context->resource_layout.layout,
      context->sampler_layout.layout,
  };
  VkPushConstantRange push_constant_range = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0u,
      .size = V0_ROOT_PUSH_CONSTANT_SIZE,
  };
  VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = (uint32_t)V0_ARRAY_COUNT(set_layouts),
      .pSetLayouts = set_layouts,
      .pushConstantRangeCount = 1u,
      .pPushConstantRanges = &push_constant_range,
  };
  VkResult result =
      vkCreatePipelineLayout(context->device, &pipeline_layout_create_info,
                             NULL, &context->pipeline_layout);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkCreatePipelineLayout failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }

  VkPipelineShaderStageCreateInfo shader_stages[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = context->vertex_shader,
          .pName = "vert_main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = context->fragment_shader,
          .pName = "frag_main",
      },
  };
  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1u,
      .scissorCount = 1u,
  };
  VkPipelineRasterizationStateCreateInfo rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineColorBlendAttachmentState blend_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  VkPipelineColorBlendStateCreateInfo blend_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1u,
      .pAttachments = &blend_attachment,
  };
  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  VkPipelineDynamicStateCreateInfo dynamic_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = (uint32_t)V0_ARRAY_COUNT(dynamic_states),
      .pDynamicStates = dynamic_states,
  };
  VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipelineRenderingCreateInfo rendering_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1u,
      .pColorAttachmentFormats = &color_format,
  };
  VkGraphicsPipelineCreateInfo pipeline_create_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_create_info,
      .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
      .stageCount = (uint32_t)V0_ARRAY_COUNT(shader_stages),
      .pStages = shader_stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport_state,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend_state,
      .pDynamicState = &dynamic_state,
      .layout = context->pipeline_layout,
  };
  result = vkCreateGraphicsPipelines(context->device, VK_NULL_HANDLE, 1u,
                                     &pipeline_create_info, NULL,
                                     &context->pipeline);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkCreateGraphicsPipelines failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }
  return true;
}

static bool v0_create_submission_objects(V0Context *context) {
  VkCommandPoolCreateInfo pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = context->selected->queue_family_index,
  };
  VkResult result = vkCreateCommandPool(context->device, &pool_create_info,
                                        NULL, &context->command_pool);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkCreateCommandPool failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }
  VkCommandBufferAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = context->command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1u,
  };
  result = vkAllocateCommandBuffers(context->device, &allocate_info,
                                    &context->command_buffer);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkAllocateCommandBuffers failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }
  VkSemaphoreTypeCreateInfo type_create_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue = 0u,
  };
  VkSemaphoreCreateInfo semaphore_create_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &type_create_info,
  };
  result = vkCreateSemaphore(context->device, &semaphore_create_info, NULL,
                             &context->timeline);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkCreateSemaphore(timeline) failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }
  return true;
}

static void v0_cmd_image_barrier(VkCommandBuffer command_buffer, VkImage image,
                                 VkPipelineStageFlags2 source_stage,
                                 VkAccessFlags2 source_access,
                                 VkPipelineStageFlags2 destination_stage,
                                 VkAccessFlags2 destination_access,
                                 VkImageLayout old_layout,
                                 VkImageLayout new_layout) {
  VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = source_stage,
      .srcAccessMask = source_access,
      .dstStageMask = destination_stage,
      .dstAccessMask = destination_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0u,
              .levelCount = 1u,
              .baseArrayLayer = 0u,
              .layerCount = 1u,
          },
  };
  VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1u,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command_buffer, &dependency);
}

static bool v0_record_commands(V0Context *context) {
  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkResult result = vkBeginCommandBuffer(context->command_buffer, &begin_info);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkBeginCommandBuffer failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }

  VkDescriptorBufferBindingInfoEXT descriptor_bindings[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
          .address = context->resource_descriptor_buffer.address,
          .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
      },
      {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
          .address = context->sampler_descriptor_buffer.address,
          .usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
      },
  };
  context->cmd_bind_descriptor_buffers(
      context->command_buffer, (uint32_t)V0_ARRAY_COUNT(descriptor_bindings),
      descriptor_bindings);

  v0_cmd_image_barrier(
      context->command_buffer, context->sampled_image.image,
      VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy2 texture_region = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
      .bufferOffset = V0_TEXTURE_OFFSET,
      .imageSubresource =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .mipLevel = 0u,
              .baseArrayLayer = 0u,
              .layerCount = 1u,
          },
      .imageExtent = {.width = 1u, .height = 1u, .depth = 1u},
  };
  VkCopyBufferToImageInfo2 texture_copy = {
      .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
      .srcBuffer = context->upload_buffer.buffer,
      .dstImage = context->sampled_image.image,
      .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .regionCount = 1u,
      .pRegions = &texture_region,
  };
  vkCmdCopyBufferToImage2(context->command_buffer, &texture_copy);
  v0_cmd_image_barrier(
      context->command_buffer, context->sampled_image.image,
      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  v0_cmd_image_barrier(context->command_buffer, context->target_image.image,
                       VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo color_attachment = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = context->target_image.view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}},
  };
  VkRenderingInfo rendering_info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea =
          {
              .offset = {.x = 0, .y = 0},
              .extent = {.width = V0_TARGET_WIDTH, .height = V0_TARGET_HEIGHT},
          },
      .layerCount = 1u,
      .colorAttachmentCount = 1u,
      .pColorAttachments = &color_attachment,
  };
  vkCmdBeginRendering(context->command_buffer, &rendering_info);
  vkCmdBindPipeline(context->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    context->pipeline);
  const uint32_t buffer_indices[2] = {0u, 1u};
  const VkDeviceSize descriptor_offsets[2] = {0u, 0u};
  context->cmd_set_descriptor_offsets(
      context->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
      context->pipeline_layout, 0u, 2u, buffer_indices, descriptor_offsets);
  VkViewport viewport = {
      .x = 0.0f,
      .y = 0.0f,
      .width = (float)V0_TARGET_WIDTH,
      .height = (float)V0_TARGET_HEIGHT,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  VkRect2D scissor = {
      .offset = {.x = 0, .y = 0},
      .extent = {.width = V0_TARGET_WIDTH, .height = V0_TARGET_HEIGHT},
  };
  vkCmdSetViewport(context->command_buffer, 0u, 1u, &viewport);
  vkCmdSetScissor(context->command_buffer, 0u, 1u, &scissor);
  V0PushConstants push_constants = {
      .root = context->upload_buffer.address + V0_ROOT_OFFSET,
      .material_index = 0u,
      .flags = 0u,
  };
  vkCmdPushConstants(context->command_buffer, context->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0u, sizeof(push_constants), &push_constants);
  vkCmdBindIndexBuffer2(context->command_buffer, context->upload_buffer.buffer,
                        V0_INDEX_OFFSET, sizeof(uint16_t) * 3u,
                        VK_INDEX_TYPE_UINT16);
  vkCmdDrawIndexed(context->command_buffer, 3u, 1u, 0u, 0, 0u);
  vkCmdEndRendering(context->command_buffer);

  v0_cmd_image_barrier(
      context->command_buffer, context->target_image.image,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
      VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy2 readback_region = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
      .bufferOffset = 0u,
      .imageSubresource =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .mipLevel = 0u,
              .baseArrayLayer = 0u,
              .layerCount = 1u,
          },
      .imageExtent = {.width = V0_TARGET_WIDTH,
                      .height = V0_TARGET_HEIGHT,
                      .depth = 1u},
  };
  VkCopyImageToBufferInfo2 readback_copy = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
      .srcImage = context->target_image.image,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .dstBuffer = context->readback_buffer.buffer,
      .regionCount = 1u,
      .pRegions = &readback_region,
  };
  vkCmdCopyImageToBuffer2(context->command_buffer, &readback_copy);
  VkBufferMemoryBarrier2 readback_barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = context->readback_buffer.buffer,
      .offset = 0u,
      .size = context->readback_buffer.size,
  };
  VkDependencyInfo readback_dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1u,
      .pBufferMemoryBarriers = &readback_barrier,
  };
  vkCmdPipelineBarrier2(context->command_buffer, &readback_dependency);

  result = vkEndCommandBuffer(context->command_buffer);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkEndCommandBuffer failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }
  printf(
      "COMMANDS descriptor-binds=1 graphics-offset-sets=1 indexed-draws=1\n");
  return true;
}

static bool v0_submit_and_wait(V0Context *context) {
  VkCommandBufferSubmitInfo command_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = context->command_buffer,
  };
  VkSemaphoreSubmitInfo signal_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = context->timeline,
      .value = V0_TIMELINE_VALUE,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  VkSubmitInfo2 submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .commandBufferInfoCount = 1u,
      .pCommandBufferInfos = &command_buffer_info,
      .signalSemaphoreInfoCount = 1u,
      .pSignalSemaphoreInfos = &signal_info,
  };
  VkResult result =
      vkQueueSubmit2(context->queue, 1u, &submit_info, VK_NULL_HANDLE);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkQueueSubmit2 failed: %s (%d)\n",
            v0_vk_result_name(result), result);
    return false;
  }
  context->submission_pending = true;
  VkSemaphoreWaitInfo wait_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1u,
      .pSemaphores = &context->timeline,
      .pValues = &(const uint64_t){V0_TIMELINE_VALUE},
  };
  result = vkWaitSemaphores(context->device, &wait_info, UINT64_MAX);
  if (result != VK_SUCCESS) {
    fprintf(stderr, "vkWaitSemaphores(value=%u) failed: %s (%d)\n",
            V0_TIMELINE_VALUE, v0_vk_result_name(result), result);
    return false;
  }
  context->submission_pending = false;
  uint64_t completed_value = 0u;
  result = vkGetSemaphoreCounterValue(context->device, context->timeline,
                                      &completed_value);
  if (result != VK_SUCCESS || completed_value < V0_TIMELINE_VALUE) {
    fprintf(stderr, "timeline counter=%" PRIu64 " expected-at-least=%u\n",
            completed_value, V0_TIMELINE_VALUE);
    return false;
  }
  printf("TIMELINE submitted=%u completed=%" PRIu64 "\n", V0_TIMELINE_VALUE,
         completed_value);
  return true;
}

static bool v0_validate_pixels(V0Context *context) {
  if (!v0_invalidate_range(context, &context->readback_buffer.allocation, 0u,
                           context->readback_buffer.size)) {
    return false;
  }
  const uint8_t expected[4] = {255u, 0u, 255u, 255u};
  const uint8_t *pixels =
      (const uint8_t *)context->readback_buffer.allocation.mapped;
  const uint32_t pixel_count = V0_TARGET_WIDTH * V0_TARGET_HEIGHT;
  for (uint32_t i = 0; i < pixel_count; ++i) {
    if (memcmp(pixels + i * 4u, expected, sizeof(expected)) != 0) {
      const uint8_t *actual = pixels + i * 4u;
      fprintf(stderr,
              "PIXEL FAIL index=%u actual=(%u,%u,%u,%u) "
              "expected=(255,0,255,255)\n",
              i, actual[0], actual[1], actual[2], actual[3]);
      return false;
    }
  }
  printf(
      "PIXELS PASS format=R8G8B8A8_UNORM extent=%ux%u exact=(255,0,255,255)\n",
      V0_TARGET_WIDTH, V0_TARGET_HEIGHT);
  return true;
}

static void v0_cleanup(V0Context *context) {
  if (context->device && context->submission_pending) {
    VkResult idle_result = vkDeviceWaitIdle(context->device);
    fprintf(stderr,
            "teardown fallback vkDeviceWaitIdle after unproven submission: %s "
            "(%d)\n",
            v0_vk_result_name(idle_result), idle_result);
    context->submission_pending = false;
  }
  if (context->device) {
    if (context->timeline) {
      vkDestroySemaphore(context->device, context->timeline, NULL);
    }
    if (context->command_pool) {
      vkDestroyCommandPool(context->device, context->command_pool, NULL);
    }
    if (context->pipeline) {
      vkDestroyPipeline(context->device, context->pipeline, NULL);
    }
    if (context->pipeline_layout) {
      vkDestroyPipelineLayout(context->device, context->pipeline_layout, NULL);
    }
    if (context->vertex_shader) {
      vkDestroyShaderModule(context->device, context->vertex_shader, NULL);
    }
    if (context->fragment_shader) {
      vkDestroyShaderModule(context->device, context->fragment_shader, NULL);
    }
    if (context->sampler) {
      vkDestroySampler(context->device, context->sampler, NULL);
    }
    v0_destroy_image(context, &context->target_image);
    v0_destroy_image(context, &context->sampled_image);
    v0_destroy_buffer(context, &context->readback_buffer);
    v0_destroy_buffer(context, &context->upload_buffer);
    v0_destroy_buffer(context, &context->sampler_descriptor_buffer);
    v0_destroy_buffer(context, &context->resource_descriptor_buffer);
    v0_destroy_candidate_device(context);
  }
  if (context->debug_messenger && context->instance) {
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug_messenger =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            context->instance, "vkDestroyDebugUtilsMessengerEXT");
    if (destroy_debug_messenger) {
      destroy_debug_messenger(context->instance, context->debug_messenger,
                              NULL);
    }
  }
  if (context->instance) {
    vkDestroyInstance(context->instance, NULL);
  }
}

static bool v0_parse_options(int argc, char **argv, V0Options *out_options) {
  memset(out_options, 0, sizeof(*out_options));
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--reflect-only") == 0) {
      out_options->reflect_only = true;
    } else if (strcmp(argv[i], "--validation") == 0) {
      out_options->validation = true;
      out_options->synchronization_validation = true;
    } else if (strcmp(argv[i], "--gpu-assisted") == 0) {
      out_options->validation = true;
      out_options->gpu_assisted = true;
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("usage: vkr_bindless_vulkan_v0 [--reflect-only] [--validation] "
             "[--gpu-assisted]\n");
      return false;
    } else {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      return false;
    }
  }
  return true;
}

int main(int argc, char **argv) {
  V0Context context;
  memset(&context, 0, sizeof(context));
  context.selected_candidate_index = UINT32_MAX;
  if (!v0_parse_options(argc, argv, &context.options)) {
    return 2;
  }
  printf("VKR BINDLESS VULKAN V0\n");
  printf("CONFIG validation=%s synchronization-validation=%s gpu-assisted=%s\n",
         context.options.validation ? "on" : "off",
         context.options.synchronization_validation ? "on" : "off",
         context.options.gpu_assisted ? "on" : "off");

  if (!v0_validate_shader_abi()) {
    fprintf(stderr, "V0 RESULT FAIL: shader ABI/reflection gate\n");
    return 1;
  }
  if (context.options.reflect_only) {
    printf("V0 RESULT PASS: offline reflection\n");
    return 0;
  }

  bool ok = v0_create_instance(&context) && v0_enumerate_candidates(&context) &&
            v0_select_device(&context);
  for (uint32_t i = 0; i < context.candidate_count; ++i) {
    v0_print_report(&context.candidates[i], i);
  }
  if (!ok) {
    fprintf(stderr, "V0 RESULT UNSUPPORTED: no device passed both phases\n");
    v0_cleanup(&context);
    return 3;
  }

  printf("\nSELECTED DEVICE[%u] %s\n", context.selected_candidate_index,
         context.selected->properties.properties.deviceName);
  ok = v0_create_resources(&context) && v0_create_pipeline(&context) &&
       v0_create_submission_objects(&context) && v0_record_commands(&context) &&
       v0_submit_and_wait(&context) && v0_validate_pixels(&context);
  const uint32_t validation_warnings = context.validation_warning_count;
  const uint32_t validation_errors = context.validation_error_count;
  const uint32_t validation_setup_notices =
      context.validation_setup_notice_count;
  const bool gpu_assisted_unavailable = context.gpu_assisted_unavailable;
  if (context.options.validation &&
      (validation_errors != 0u ||
       (validation_warnings != 0u && !gpu_assisted_unavailable))) {
    fprintf(stderr, "validation diagnostics warnings=%u errors=%u\n",
            validation_warnings, validation_errors);
    ok = false;
  }
  v0_cleanup(&context);
  printf("VALIDATION setup-notices=%u warnings=%u errors=%u\n",
         validation_setup_notices, validation_warnings, validation_errors);
  if (gpu_assisted_unavailable && validation_errors == 0u) {
    printf("GPU-ASSISTED UNAVAILABLE: installed validation layer disabled "
           "shader instrumentation for VK_EXT_descriptor_buffer\n");
    printf("V0 RESULT GPU_ASSISTED_UNAVAILABLE\n");
    return 4;
  }
  printf("V0 RESULT %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
