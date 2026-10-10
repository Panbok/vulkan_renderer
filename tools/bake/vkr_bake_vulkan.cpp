/* The Vulkan loader is opened at run time, so no prototype is linked. */
#define VK_NO_PROTOTYPES

#include "bake/vkr_bake_gpu.h"
#include "bake/vkr_bake_sh.h"

extern "C" {
#include "platform/vkr_platform.h"
#include "vkr_dfg_lut.h"
#include "vkr_ibl_math.h"
}

#include "vkr_bake_vulkan_spirv.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <utility>

/*
 * Vulkan ray-query lightmap transport (owner decision 2026-10-09): the Metal
 * transport of vkr_bake_metal.mm on a headless compute device with
 * VK_KHR_acceleration_structure and VK_KHR_ray_query. The kernels are
 * vkr_bake_lightmap.slang, compiled to SPIR-V at build time and embedded; the
 * scene upload, acceleration structure groups, light partition and dispatch
 * split mirror the Metal host code, so both backends gather the same
 * transport with the same seeds.
 *
 * The device also runs the BC6H and BC7 page encoders, DirectXTex's compute
 * kernels (vendor/directxtex/Shaders), with the dispatch sequence of
 * DirectXTex's BCDirectCompute.cpp; Metal has no counterpart.
 */

namespace {

/* Texels per command buffer, and samples per texel the gather takes per
   command buffer, as on Metal: each submission stays near a quarter second
   of a textured Bistro layer on the M1 Pro, so a long bake leaves the
   display GPU time and stays far under the Windows TDR limit (2 s) on a GPU
   that drives the desktop. */
constexpr uint32_t kTexelsPerDispatch = 16384u;
constexpr uint32_t kSamplesPerDispatch = 8u;
/* Paths per probe-gather submission, as a texel-gather submission takes
   (kTexelsPerDispatch * kSamplesPerDispatch); a probe takes
   6 * face_size^2 * samples of them, so a submission holds as many whole
   probes as fit, and at least one. */
constexpr uint32_t kProbePathsPerDispatch = 131072u;
/* The kernels' numthreads. */
constexpr uint32_t kWorkgroupSize = 64u;

/* Material textures are resampled to this edge for transport: bounce light
   needs their low frequencies only. */
constexpr uint32_t kTextureEdge = 128u;
constexpr uint32_t kSkyWidth = 512u;
constexpr uint32_t kSkyHeight = 256u;
constexpr float kPi = 3.14159265358979323846f;

/* Descriptor set 0 of vkr_bake_lightmap.slang. */
enum Binding : uint32_t {
  kBindingTexelPositions = 0u,
  kBindingTexelNormals = 1u,
  kBindingIndirect = 2u,
  kBindingHitFraction = 3u,
  kBindingScene = 4u,
  kBindingPositions = 5u,
  kBindingNormals = 6u,
  kBindingUvs = 7u,
  kBindingColors = 8u,
  kBindingTriangleMaterials = 9u,
  kBindingMaterials = 10u,
  kBindingLights = 11u,
  kBindingLayerLights = 12u,
  kBindingAsIndices = 13u,
  kBindingOcclusion = 14u,
  kBindingDirect = 15u,
  kBindingMoments = 16u,
  kBindingBackface = 17u,
  kBindingFootprints = 18u,
  kBindingTextures = 19u,
  kBindingSky = 20u,
  kBindingDfg = 21u,
  kBindingTextureSampler = 22u,
  kBindingSkySampler = 23u,
  kBindingDfgSampler = 24u,
  kBindingProbePositions = 25u,
  kBindingProbeSeeds = 26u,
  kBindingProbeRadiance = 27u,
  kBindingDirection = 28u,
  kBindingCount = 29u,
};

/* Descriptor set 0 of the BC kernels: DirectXTex's cbCS (b0), g_Input (t0),
   g_InBuff (t1) and g_OutBuff (u0) after slangc's register shifts in
   tools/CMakeLists.txt, and the decode kernel's texture and output. */
enum BcBinding : uint32_t {
  kBcBindingConstants = 0u,
  kBcBindingSource = 1u,
  kBcBindingInput = 2u,
  kBcBindingOutput = 3u,
  kBcBindingCount = 4u,
};

/* The kernels of the BC encoders and their decode. */
enum BcPipeline : uint32_t {
  kBc6hTryModeG10 = 0u,
  kBc6hTryModeLE10 = 1u,
  kBc6hEncodeBlock = 2u,
  kBc7TryMode456 = 3u,
  kBc7TryMode137 = 4u,
  kBc7EncodeBlock = 5u,
  kBcDecode = 6u,
  kBcPipelineCount = 7u,
};

/* One descriptor set per direction of the encoders' ping-pong between their
   two error buffers, one per error buffer the block pass reads, and the
   decode's. */
enum BcSet : uint32_t {
  kBcErr1ToErr2 = 0u,
  kBcErr2ToErr1 = 1u,
  kBcErr1ToBlocks = 2u,
  kBcErr2ToBlocks = 3u,
  kBcDecodeSet = 4u,
  kBcSetCount = 5u,
};

/* DirectXTex's ConstantsBC6HBC7, the kernels' cbCS. */
struct BcConstants {
  uint32_t tex_width;
  uint32_t num_block_x;
  uint32_t format;
  uint32_t mode_id;
  uint32_t start_block_id;
  uint32_t num_total_blocks;
  float alpha_weight;
  uint32_t reserved;
};
static_assert(sizeof(BcConstants) == 32u, "BcConstants matches cbCS");

/* The decode kernel's push constants. */
struct BcDecodeArgs {
  uint32_t width;
  uint32_t height;
};

/* DXGI_FORMAT_BC6H_UF16 and DXGI_FORMAT_BC7_UNORM, the values the kernels
   compare g_format with. */
constexpr uint32_t kDxgiBc6hUf16 = 95u;
constexpr uint32_t kDxgiBc7Unorm = 98u;
/* Blocks per submission: a multiple of 4, the most blocks a kernel group
   takes, and within the 65535 groups every device dispatches, since the
   BC7 two-subset pass runs one block per group. On the RX 6700 XT a
   submission stays far under the Windows TDR limit (2 s). */
constexpr uint32_t kBcBlocksPerSubmission = 32768u;
/* The most passes one submission records: BC6H's mode 11-14 pass, ten mode
   1-10 passes and its block pass. */
constexpr uint32_t kBcMaxPasses = 12u;

struct BenchmarkArgs {
  uint32_t first_texel;
  uint32_t texel_count;
  uint32_t samples;
  uint32_t seed;
};

/* The kernel's GatherArgs push constants: 32-bit scalars only. */
struct GatherArgs {
  uint32_t first_texel;
  uint32_t texel_count;
  uint32_t samples;
  uint32_t first_sample;
  uint32_t sample_count;
  uint32_t seed;
  uint32_t max_depth;
  uint32_t rr_start_depth;
  uint32_t light_count;
  uint32_t rectangle_first;
  uint32_t flags;
  uint32_t geometry_first0;
  uint32_t geometry_first1;
  float occlusion_radius;
  float indirect_clamp;
};
static_assert(sizeof(GatherArgs) == 60u, "GatherArgs matches the kernel");

/* The probe kernel's ProbeArgs push constants. */
struct ProbeArgs {
  uint32_t first_probe;
  uint32_t probe_count;
  uint32_t face_size;
  uint32_t samples;
  uint32_t max_depth;
  uint32_t rr_start_depth;
  uint32_t light_count;
  uint32_t flags;
  uint32_t geometry_first0;
  uint32_t geometry_first1;
};
static_assert(sizeof(ProbeArgs) == 40u, "ProbeArgs matches the kernel");

struct GpuMaterial {
  float base_color[4];
  float emissive_metallic[4];
  float misc[4];
  float specular[4];
  int32_t layers[4];
  float surface[4];
};
static_assert(sizeof(GpuMaterial) == 96u, "GpuMaterial matches the kernel");

struct GpuLight {
  float position_kind[4];
  float direction_range[4];
  float color_intensity[4];
  float attenuation[4];
  float right_half_width[4];
  float up_half_height[4];
  float cone[4];
};
static_assert(sizeof(GpuLight) == 112u, "GpuLight matches the kernel");

#define VKR_BAKE_VK_GLOBAL_FUNCTIONS(X)                                        \
  X(vkCreateInstance)                                                          \
  X(vkEnumerateInstanceVersion)

#define VKR_BAKE_VK_INSTANCE_FUNCTIONS(X)                                      \
  X(vkDestroyInstance)                                                         \
  X(vkEnumeratePhysicalDevices)                                                \
  X(vkGetPhysicalDeviceProperties)                                             \
  X(vkGetPhysicalDeviceProperties2)                                            \
  X(vkGetPhysicalDeviceFeatures2)                                              \
  X(vkGetPhysicalDeviceQueueFamilyProperties)                                  \
  X(vkGetPhysicalDeviceMemoryProperties)                                       \
  X(vkGetPhysicalDeviceFormatProperties)                                       \
  X(vkEnumerateDeviceExtensionProperties)                                      \
  X(vkCreateDevice)                                                            \
  X(vkGetDeviceProcAddr)

#define VKR_BAKE_VK_DEVICE_FUNCTIONS(X)                                        \
  X(vkDestroyDevice)                                                           \
  X(vkDeviceWaitIdle)                                                          \
  X(vkGetDeviceQueue)                                                          \
  X(vkCreateBuffer)                                                            \
  X(vkDestroyBuffer)                                                           \
  X(vkGetBufferMemoryRequirements)                                             \
  X(vkGetBufferDeviceAddress)                                                  \
  X(vkAllocateMemory)                                                          \
  X(vkFreeMemory)                                                              \
  X(vkBindBufferMemory)                                                        \
  X(vkMapMemory)                                                               \
  X(vkCreateImage)                                                             \
  X(vkDestroyImage)                                                            \
  X(vkGetImageMemoryRequirements)                                              \
  X(vkBindImageMemory)                                                         \
  X(vkCreateImageView)                                                         \
  X(vkDestroyImageView)                                                        \
  X(vkCreateSampler)                                                           \
  X(vkDestroySampler)                                                          \
  X(vkCreateCommandPool)                                                       \
  X(vkDestroyCommandPool)                                                      \
  X(vkAllocateCommandBuffers)                                                  \
  X(vkResetCommandBuffer)                                                      \
  X(vkBeginCommandBuffer)                                                      \
  X(vkEndCommandBuffer)                                                        \
  X(vkQueueSubmit)                                                             \
  X(vkCreateFence)                                                             \
  X(vkDestroyFence)                                                            \
  X(vkWaitForFences)                                                           \
  X(vkResetFences)                                                             \
  X(vkCreateQueryPool)                                                         \
  X(vkDestroyQueryPool)                                                        \
  X(vkGetQueryPoolResults)                                                     \
  X(vkCreateShaderModule)                                                      \
  X(vkDestroyShaderModule)                                                     \
  X(vkCreateDescriptorSetLayout)                                               \
  X(vkDestroyDescriptorSetLayout)                                              \
  X(vkCreatePipelineLayout)                                                    \
  X(vkDestroyPipelineLayout)                                                   \
  X(vkCreateComputePipelines)                                                  \
  X(vkDestroyPipeline)                                                         \
  X(vkCreateDescriptorPool)                                                    \
  X(vkDestroyDescriptorPool)                                                   \
  X(vkAllocateDescriptorSets)                                                  \
  X(vkUpdateDescriptorSets)                                                    \
  X(vkCmdResetQueryPool)                                                       \
  X(vkCmdWriteTimestamp)                                                       \
  X(vkCmdPipelineBarrier)                                                      \
  X(vkCmdCopyBuffer)                                                           \
  X(vkCmdCopyBufferToImage)                                                    \
  X(vkCmdBindPipeline)                                                         \
  X(vkCmdBindDescriptorSets)                                                   \
  X(vkCmdPushConstants)                                                        \
  X(vkCmdDispatch)                                                             \
  X(vkCreateAccelerationStructureKHR)                                          \
  X(vkDestroyAccelerationStructureKHR)                                         \
  X(vkGetAccelerationStructureBuildSizesKHR)                                   \
  X(vkGetAccelerationStructureDeviceAddressKHR)                                \
  X(vkCmdBuildAccelerationStructuresKHR)

#define VKR_BAKE_VK_MEMBER(name) PFN_##name name = nullptr;

/* Vulkan entry points from the loader opened at run time. */
struct VulkanApi {
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
  VKR_BAKE_VK_GLOBAL_FUNCTIONS(VKR_BAKE_VK_MEMBER)
  VKR_BAKE_VK_INSTANCE_FUNCTIONS(VKR_BAKE_VK_MEMBER)
  VKR_BAKE_VK_DEVICE_FUNCTIONS(VKR_BAKE_VK_MEMBER)
};

#undef VKR_BAKE_VK_MEMBER

const char *const kDeviceExtensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};

#if defined(_WIN32)
constexpr const char *kLoaderName = "vulkan-1.dll";
#else
constexpr const char *kLoaderName = "libvulkan.so.1";
#endif

/* A loader and an instance without any instance extension: the bake device
   presents nothing. */
struct VulkanInstance {
  VkrPlatformLibrary library = {};
  VulkanApi api = {};
  VkInstance instance = VK_NULL_HANDLE;
};

void close_instance(VulkanInstance *vulkan) {
  if (vulkan->instance != VK_NULL_HANDLE) {
    vulkan->api.vkDestroyInstance(vulkan->instance, nullptr);
    vulkan->instance = VK_NULL_HANDLE;
  }
  vkr_platform_library_close(&vulkan->library);
}

/* Opens the loader and a Vulkan 1.2 instance; false, with nothing open, on a
   host without either. */
bool open_instance(VulkanInstance *vulkan) {
  char error[256] = {};
  if (!vkr_platform_library_open(kLoaderName, &vulkan->library, error,
                                 sizeof(error))) {
    return false;
  }
  VulkanApi &api = vulkan->api;
  api.vkGetInstanceProcAddr =
      (PFN_vkGetInstanceProcAddr)vkr_platform_library_symbol(
          &vulkan->library, "vkGetInstanceProcAddr");
  if (!api.vkGetInstanceProcAddr) {
    close_instance(vulkan);
    return false;
  }
#define VKR_BAKE_VK_LOAD_GLOBAL(name)                                          \
  api.name = (PFN_##name)api.vkGetInstanceProcAddr(VK_NULL_HANDLE, #name);
  VKR_BAKE_VK_GLOBAL_FUNCTIONS(VKR_BAKE_VK_LOAD_GLOBAL)
#undef VKR_BAKE_VK_LOAD_GLOBAL
  /* A Vulkan 1.0 loader has no vkEnumerateInstanceVersion. */
  uint32_t loader_version = VK_API_VERSION_1_0;
  if (!api.vkCreateInstance || !api.vkEnumerateInstanceVersion ||
      api.vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS ||
      loader_version < VK_API_VERSION_1_2) {
    close_instance(vulkan);
    return false;
  }
  const VkApplicationInfo application = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vkr_lightmap_baker",
      .applicationVersion = 1u,
      .pEngineName = "vkr",
      .engineVersion = 1u,
      /* 1.3 for the BC encoders' integer dot products; the transport needs
         1.2 only. */
      .apiVersion = VK_API_VERSION_1_3,
  };
  const VkInstanceCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &application,
  };
  if (api.vkCreateInstance(&create, nullptr, &vulkan->instance) != VK_SUCCESS) {
    vulkan->instance = VK_NULL_HANDLE;
    close_instance(vulkan);
    return false;
  }
  bool loaded = true;
#define VKR_BAKE_VK_LOAD_INSTANCE(name)                                        \
  api.name = (PFN_##name)api.vkGetInstanceProcAddr(vulkan->instance, #name);   \
  loaded = loaded && api.name != nullptr;
  VKR_BAKE_VK_INSTANCE_FUNCTIONS(VKR_BAKE_VK_LOAD_INSTANCE)
#undef VKR_BAKE_VK_LOAD_INSTANCE
  if (!loaded) {
    close_instance(vulkan);
    return false;
  }
  return true;
}

/* A device the transport can run on and the queue family it uses. */
struct DeviceChoice {
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  uint32_t queue_family = 0u;
  VkPhysicalDeviceProperties properties = {};
};

bool device_has_extensions(const VulkanApi &api, VkPhysicalDevice physical) {
  uint32_t count = 0u;
  if (api.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count,
                                               nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> extensions(count);
  if (api.vkEnumerateDeviceExtensionProperties(
          physical, nullptr, &count, extensions.data()) != VK_SUCCESS) {
    return false;
  }
  for (const char *wanted : kDeviceExtensions) {
    bool found = false;
    for (const VkExtensionProperties &extension : extensions) {
      found = found || std::strcmp(extension.extensionName, wanted) == 0;
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

bool device_has_features(const VulkanApi &api, VkPhysicalDevice physical) {
  VkPhysicalDeviceRayQueryFeaturesKHR ray_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
  };
  VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration = {
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
      .pNext = &ray_query,
  };
  VkPhysicalDeviceVulkan12Features vulkan12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = &acceleration,
  };
  VkPhysicalDeviceFeatures2 features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &vulkan12,
  };
  api.vkGetPhysicalDeviceFeatures2(physical, &features);
  return ray_query.rayQuery && acceleration.accelerationStructure &&
         vulkan12.bufferDeviceAddress;
}

/* A compute queue family with timestamps, preferring one without graphics,
   so the bake runs beside the desktop's graphics queue. */
bool choose_queue_family(const VulkanApi &api, VkPhysicalDevice physical,
                         uint32_t *out_family) {
  uint32_t count = 0u;
  api.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  api.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count,
                                               families.data());
  int64_t chosen = -1;
  for (uint32_t i = 0u; i < count; ++i) {
    const VkQueueFamilyProperties &family = families[i];
    if (!(family.queueFlags & VK_QUEUE_COMPUTE_BIT) ||
        family.timestampValidBits == 0u) {
      continue;
    }
    const bool graphics = (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u;
    if (chosen < 0 || !graphics) {
      chosen = i;
    }
    if (!graphics) {
      break;
    }
  }
  if (chosen < 0) {
    return false;
  }
  *out_family = (uint32_t)chosen;
  return true;
}

/* The first ray-query device, preferring a discrete GPU. */
bool choose_device(const VulkanInstance &vulkan, DeviceChoice *out_choice) {
  const VulkanApi &api = vulkan.api;
  uint32_t count = 0u;
  if (api.vkEnumeratePhysicalDevices(vulkan.instance, &count, nullptr) !=
          VK_SUCCESS ||
      count == 0u) {
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  if (api.vkEnumeratePhysicalDevices(vulkan.instance, &count, devices.data()) !=
      VK_SUCCESS) {
    return false;
  }
  bool found = false;
  for (VkPhysicalDevice physical : devices) {
    VkPhysicalDeviceProperties properties = {};
    api.vkGetPhysicalDeviceProperties(physical, &properties);
    uint32_t family = 0u;
    if (properties.apiVersion < VK_API_VERSION_1_2 ||
        !device_has_extensions(api, physical) ||
        !device_has_features(api, physical) ||
        !choose_queue_family(api, physical, &family)) {
      continue;
    }
    const bool discrete =
        properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    if (!found || (discrete && out_choice->properties.deviceType !=
                                   VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) {
      out_choice->physical = physical;
      out_choice->queue_family = family;
      out_choice->properties = properties;
      found = true;
    }
  }
  return found;
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  /* Persistently mapped when host visible. */
  void *mapped = nullptr;
  VkDeviceSize size = 0u;
  VkDeviceAddress address = 0u;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

uint32_t pack_unorm4(Vec4 color) {
  auto channel = [](float value) {
    const float clamped = std::fmin(std::fmax(value, 0.0f), 1.0f);
    return (uint32_t)std::lround(clamped * 255.0f);
  };
  return channel(color.x) | (channel(color.y) << 8u) |
         (channel(color.z) << 16u) | (channel(color.w) << 24u);
}

bool vk_ok(VkResult result, const char *what) {
  if (result != VK_SUCCESS) {
    std::fprintf(stderr, "Lightmap Vulkan %s failed (VkResult %d)\n", what,
                 (int)result);
    return false;
  }
  return true;
}

} // namespace

struct VkrBakeGpuContext {
  VulkanInstance vulkan;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_properties = {};
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queue_family = 0u;
  VkQueue queue = VK_NULL_HANDLE;
  /* Nanoseconds per timestamp tick and the mask of valid timestamp bits. */
  double timestamp_period = 1.0;
  uint64_t timestamp_mask = ~0ull;
  VkDeviceSize scratch_alignment = 1u;
  /* One command buffer, fence and timestamp pair, reused by every
     submission: each submission is waited for before the next records. */
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkQueryPool timestamps = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline benchmark = VK_NULL_HANDLE;
  VkPipeline gather = VK_NULL_HANDLE;
  VkPipeline probes = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkSampler texture_sampler = VK_NULL_HANDLE;
  VkSampler sky_sampler = VK_NULL_HANDLE;
  VkSampler dfg_sampler = VK_NULL_HANDLE;
  Buffer positions;
  Buffer normals;
  Buffer uvs;
  Buffer colors;
  Buffer triangle_materials;
  Buffer materials;
  Buffer lights;
  /* Corner indices of the acceleration structure, grouped by geometry. */
  Buffer as_indices;
  Buffer bottom_storage;
  Buffer top_storage;
  VkAccelerationStructureKHR bottom = VK_NULL_HANDLE;
  VkAccelerationStructureKHR top = VK_NULL_HANDLE;
  /* The uploaded lights, which the gather partitions by kind. */
  std::vector<GpuLight> host_lights;
  /* Per material, the lowest alpha its factor and base color texture give,
     as the kernel samples them. */
  std::vector<float> material_alpha_floor;
  uint32_t geometry_first[2] = {0u, 0u};
  Image textures;
  Image sky;
  Image dfg;
  /* The BC page encoders. bc_supported is set at device creation when the
     device has their features; the layouts, sets and pipelines are created
     by the first encode or decode. */
  bool bc_supported = false;
  VkDeviceSize uniform_alignment = 256u;
  VkDescriptorSetLayout bc_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout bc_pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool bc_descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet bc_sets[kBcSetCount] = {};
  VkPipeline bc_pipelines[kBcPipelineCount] = {};
};

namespace {

bool find_memory_type(const VkrBakeGpuContext *context, uint32_t type_bits,
                      VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred, uint32_t *out_type) {
  const VkPhysicalDeviceMemoryProperties &memory = context->memory_properties;
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    const VkMemoryPropertyFlags wanted =
        pass == 0u ? required | preferred : required;
    for (uint32_t i = 0u; i < memory.memoryTypeCount; ++i) {
      if ((type_bits & (1u << i)) &&
          (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
        *out_type = i;
        return true;
      }
    }
  }
  return false;
}

bool allocate_memory(VkrBakeGpuContext *context,
                     const VkMemoryRequirements &requirements,
                     VkMemoryPropertyFlags required,
                     VkMemoryPropertyFlags preferred, bool device_address,
                     VkDeviceMemory *out_memory) {
  uint32_t type = 0u;
  if (!find_memory_type(context, requirements.memoryTypeBits, required,
                        preferred, &type)) {
    std::fprintf(stderr, "Lightmap Vulkan memory type not found\n");
    return false;
  }
  const VkMemoryAllocateFlagsInfo flags = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
      .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
  };
  const VkMemoryAllocateInfo allocate = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = device_address ? &flags : nullptr,
      .allocationSize = requirements.size,
      .memoryTypeIndex = type,
  };
  return vk_ok(context->vulkan.api.vkAllocateMemory(context->device, &allocate,
                                                    nullptr, out_memory),
               "memory allocation");
}

void destroy_buffer(VkrBakeGpuContext *context, Buffer *buffer) {
  const VulkanApi &api = context->vulkan.api;
  if (buffer->buffer != VK_NULL_HANDLE) {
    api.vkDestroyBuffer(context->device, buffer->buffer, nullptr);
  }
  if (buffer->memory != VK_NULL_HANDLE) {
    api.vkFreeMemory(context->device, buffer->memory, nullptr);
  }
  *buffer = Buffer{};
}

/* A buffer of at least 16 bytes. Host-visible buffers are coherent and stay
   mapped; device-address usage also returns the address. */
bool create_buffer(VkrBakeGpuContext *context, VkDeviceSize size,
                   VkBufferUsageFlags usage, bool host_visible,
                   Buffer *out_buffer) {
  const VulkanApi &api = context->vulkan.api;
  *out_buffer = Buffer{};
  out_buffer->size = std::max<VkDeviceSize>(size, 16u);
  const VkBufferCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = out_buffer->size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  if (!vk_ok(api.vkCreateBuffer(context->device, &create, nullptr,
                                &out_buffer->buffer),
             "buffer creation")) {
    out_buffer->buffer = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements requirements = {};
  api.vkGetBufferMemoryRequirements(context->device, out_buffer->buffer,
                                    &requirements);
  const bool device_address =
      (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0u;
  const VkMemoryPropertyFlags required =
      host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                   : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  /* Host buffers are mostly read back. */
  const VkMemoryPropertyFlags preferred =
      host_visible ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0u;
  if (!allocate_memory(context, requirements, required, preferred,
                       device_address, &out_buffer->memory) ||
      !vk_ok(api.vkBindBufferMemory(context->device, out_buffer->buffer,
                                    out_buffer->memory, 0u),
             "buffer binding")) {
    destroy_buffer(context, out_buffer);
    return false;
  }
  if (host_visible &&
      !vk_ok(api.vkMapMemory(context->device, out_buffer->memory, 0u,
                             VK_WHOLE_SIZE, 0u, &out_buffer->mapped),
             "memory mapping")) {
    destroy_buffer(context, out_buffer);
    return false;
  }
  if (device_address) {
    const VkBufferDeviceAddressInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = out_buffer->buffer,
    };
    out_buffer->address = api.vkGetBufferDeviceAddress(context->device, &info);
  }
  return true;
}

void destroy_image(VkrBakeGpuContext *context, Image *image) {
  const VulkanApi &api = context->vulkan.api;
  if (image->view != VK_NULL_HANDLE) {
    api.vkDestroyImageView(context->device, image->view, nullptr);
  }
  if (image->image != VK_NULL_HANDLE) {
    api.vkDestroyImage(context->device, image->image, nullptr);
  }
  if (image->memory != VK_NULL_HANDLE) {
    api.vkFreeMemory(context->device, image->memory, nullptr);
  }
  *image = Image{};
}

bool begin_commands(VkrBakeGpuContext *context) {
  const VulkanApi &api = context->vulkan.api;
  const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  return vk_ok(api.vkResetCommandBuffer(context->command_buffer, 0u),
               "command buffer reset") &&
         vk_ok(api.vkBeginCommandBuffer(context->command_buffer, &begin),
               "command buffer begin");
}

/* Opens the timestamp pair around the commands recorded next. */
void begin_timing(VkrBakeGpuContext *context) {
  const VulkanApi &api = context->vulkan.api;
  api.vkCmdResetQueryPool(context->command_buffer, context->timestamps, 0u, 2u);
  api.vkCmdWriteTimestamp(context->command_buffer,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          context->timestamps, 0u);
}

void end_timing(VkrBakeGpuContext *context) {
  context->vulkan.api.vkCmdWriteTimestamp(context->command_buffer,
                                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                          context->timestamps, 1u);
}

/* Submits the recorded commands and waits for them. With
   `in_out_gpu_seconds`, the commands wrote the timestamp pair and its
   interval is added. A lost device (a GPU reset) fails the call. */
bool finish_commands(VkrBakeGpuContext *context, double *in_out_gpu_seconds) {
  const VulkanApi &api = context->vulkan.api;
  if (!vk_ok(api.vkEndCommandBuffer(context->command_buffer),
             "command buffer end")) {
    return false;
  }
  const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1u,
      .pCommandBuffers = &context->command_buffer,
  };
  if (!vk_ok(api.vkQueueSubmit(context->queue, 1u, &submit, context->fence),
             "submission") ||
      !vk_ok(api.vkWaitForFences(context->device, 1u, &context->fence, VK_TRUE,
                                 UINT64_MAX),
             "dispatch") ||
      !vk_ok(api.vkResetFences(context->device, 1u, &context->fence),
             "fence reset")) {
    return false;
  }
  if (!in_out_gpu_seconds) {
    return true;
  }
  uint64_t ticks[2] = {0u, 0u};
  if (!vk_ok(api.vkGetQueryPoolResults(
                 context->device, context->timestamps, 0u, 2u, sizeof(ticks),
                 ticks, sizeof(uint64_t),
                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
             "timestamp query")) {
    return false;
  }
  const uint64_t elapsed = ((ticks[1] & context->timestamp_mask) -
                            (ticks[0] & context->timestamp_mask)) &
                           context->timestamp_mask;
  *in_out_gpu_seconds += (double)elapsed * context->timestamp_period * 1.0e-9;
  return true;
}

/* A device-local buffer filled from `data` through a staging copy. */
bool upload_buffer(VkrBakeGpuContext *context, const void *data,
                   VkDeviceSize size, VkBufferUsageFlags usage,
                   Buffer *out_buffer) {
  Buffer staging;
  if (!create_buffer(context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                     &staging)) {
    return false;
  }
  if (size > 0u) {
    std::memcpy(staging.mapped, data, (size_t)size);
  }
  bool ok =
      create_buffer(context, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    false, out_buffer) &&
      begin_commands(context);
  if (ok) {
    const VkBufferCopy region = {.size = staging.size};
    context->vulkan.api.vkCmdCopyBuffer(context->command_buffer, staging.buffer,
                                        out_buffer->buffer, 1u, &region);
    ok = finish_commands(context, nullptr);
  }
  destroy_buffer(context, &staging);
  return ok;
}

/* A sampled 2D or 2D array image filled from tightly packed `data`, left in
   shader-read layout. The view type is the kernel's: a texture array stays
   an array view with one layer. `features` are the format features the
   kernel needs: linear filtering for a sampled texture, plain sampling for
   a fetched one. */
bool upload_image(VkrBakeGpuContext *context, VkFormat format, uint32_t width,
                  uint32_t height, uint32_t layers, VkImageViewType view_type,
                  VkFormatFeatureFlags features, const void *data,
                  VkDeviceSize size, Image *out_image) {
  const VulkanApi &api = context->vulkan.api;
  *out_image = Image{};
  VkFormatProperties format_properties = {};
  api.vkGetPhysicalDeviceFormatProperties(context->physical, format,
                                          &format_properties);
  if ((format_properties.optimalTilingFeatures & features) != features) {
    std::fprintf(stderr,
                 "Lightmap Vulkan device cannot sample format %d as needed\n",
                 (int)format);
    return false;
  }
  const VkImageCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {width, height, 1u},
      .mipLevels = 1u,
      .arrayLayers = layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  if (!vk_ok(api.vkCreateImage(context->device, &create, nullptr,
                               &out_image->image),
             "image creation")) {
    out_image->image = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements requirements = {};
  api.vkGetImageMemoryRequirements(context->device, out_image->image,
                                   &requirements);
  const VkImageSubresourceRange range = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .levelCount = 1u,
      .layerCount = layers,
  };
  const VkImageViewCreateInfo view = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = out_image->image,
      .viewType = view_type,
      .format = format,
      .subresourceRange = range,
  };
  Buffer staging;
  bool ok = allocate_memory(context, requirements,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0u, false,
                            &out_image->memory) &&
            vk_ok(api.vkBindImageMemory(context->device, out_image->image,
                                        out_image->memory, 0u),
                  "image binding") &&
            vk_ok(api.vkCreateImageView(context->device, &view, nullptr,
                                        &out_image->view),
                  "image view creation") &&
            create_buffer(context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                          &staging) &&
            begin_commands(context);
  if (ok) {
    std::memcpy(staging.mapped, data, (size_t)size);
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0u,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = out_image->image,
        .subresourceRange = range,
    };
    api.vkCmdPipelineBarrier(context->command_buffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr,
                             0u, nullptr, 1u, &barrier);
    const VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, layers},
        .imageExtent = {width, height, 1u},
    };
    api.vkCmdCopyBufferToImage(
        context->command_buffer, staging.buffer, out_image->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    api.vkCmdPipelineBarrier(context->command_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u,
                             nullptr, 0u, nullptr, 1u, &barrier);
    ok = finish_commands(context, nullptr);
  }
  destroy_buffer(context, &staging);
  if (!ok) {
    destroy_image(context, out_image);
  }
  return ok;
}

/* Whether a triangle needs its alpha tested during traversal: a cutout
   triangle whose lowest alpha (material floor times its lowest vertex alpha,
   which bounds the interpolated one) falls below the cutoff. Blended surfaces
   always stop a traversal and are resolved after it, so they gain nothing
   from testing. */
bool triangle_alpha_tested(const VkrBakeGpuContext *context,
                           const VkrBakeScene &scene, uint32_t triangle) {
  const VkrBakeTriangle &source = scene.triangles[triangle];
  const uint32_t material = source.material_index;
  if (material >= scene.materials.size() ||
      scene.materials[material].alpha_mode != VKR_BAKE_MATERIAL_ALPHA_CUTOUT) {
    return false;
  }
  float vertex_alpha = 1.0f;
  for (uint32_t c = 0u; c < 3u; ++c) {
    /* The kernel reads vertex colors as RGBA8. */
    const uint32_t packed = pack_unorm4(source.vertex[c].color);
    vertex_alpha = std::fmin(vertex_alpha, (float)(packed >> 24u) / 255.0f);
  }
  const float cutoff =
      std::fmin(std::fmax(scene.materials[material].alpha_cutoff, 0.0f), 1.0f);
  return context->material_alpha_floor[material] * vertex_alpha < cutoff;
}

/* A bottom-level structure of two geometries over the corner positions:
   opaque triangles, which commit during traversal, then cutout triangles
   that can pass, which the kernels test as candidates; an empty group gets
   no geometry. One top-level instance holds it. */
bool build_acceleration_structure(VkrBakeGpuContext *context,
                                  const VkrBakeScene &scene) {
  const VulkanApi &api = context->vulkan.api;
  const uint32_t triangle_count = (uint32_t)scene.triangles.size();
  std::vector<uint32_t> indices((size_t)triangle_count * 3u);
  uint32_t written = 0u;
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    context->geometry_first[pass] = written;
    for (uint32_t t = 0u; t < triangle_count; ++t) {
      const bool alpha = triangle_alpha_tested(context, scene, t);
      if (alpha != (pass == 1u)) {
        continue;
      }
      indices[3u * written + 0u] = 3u * t;
      indices[3u * written + 1u] = 3u * t + 1u;
      indices[3u * written + 2u] = 3u * t + 2u;
      ++written;
    }
  }
  const VkBufferUsageFlags input_usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  if (!upload_buffer(context, indices.data(),
                     (VkDeviceSize)indices.size() * sizeof(uint32_t),
                     input_usage, &context->as_indices)) {
    return false;
  }
  const uint32_t counts[2] = {context->geometry_first[1],
                              triangle_count - context->geometry_first[1]};
  VkAccelerationStructureGeometryKHR geometries[2] = {};
  VkAccelerationStructureBuildRangeInfoKHR ranges[2] = {};
  uint32_t primitive_counts[2] = {};
  uint32_t geometry_count = 0u;
  for (uint32_t g = 0u; g < 2u; ++g) {
    if (counts[g] == 0u) {
      continue;
    }
    VkAccelerationStructureGeometryKHR &geometry = geometries[geometry_count];
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.geometry.triangles = {
        .sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
        .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
        .vertexData = {.deviceAddress = context->positions.address},
        .vertexStride = 3u * sizeof(float),
        .maxVertex = 3u * triangle_count - 1u,
        .indexType = VK_INDEX_TYPE_UINT32,
        .indexData = {.deviceAddress = context->as_indices.address},
    };
    geometry.flags = g == 0u ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0u;
    ranges[geometry_count] = {
        .primitiveCount = counts[g],
        .primitiveOffset =
            context->geometry_first[g] * 3u * (uint32_t)sizeof(uint32_t),
    };
    primitive_counts[geometry_count] = counts[g];
    ++geometry_count;
  }
  /* With one group, geometry 0 is that group. */
  if (counts[0] == 0u) {
    context->geometry_first[0] = context->geometry_first[1];
  }

  VkAccelerationStructureBuildGeometryInfoKHR bottom_build = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
      .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
      .geometryCount = geometry_count,
      .pGeometries = geometries,
  };
  VkAccelerationStructureBuildSizesInfoKHR bottom_sizes = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
  };
  api.vkGetAccelerationStructureBuildSizesKHR(
      context->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
      &bottom_build, primitive_counts, &bottom_sizes);
  const VkBufferUsageFlags storage_usage =
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  VkAccelerationStructureCreateInfoKHR bottom_create = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
      .size = bottom_sizes.accelerationStructureSize,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
  };
  if (!create_buffer(context, bottom_sizes.accelerationStructureSize,
                     storage_usage, false, &context->bottom_storage)) {
    return false;
  }
  bottom_create.buffer = context->bottom_storage.buffer;
  if (!vk_ok(api.vkCreateAccelerationStructureKHR(
                 context->device, &bottom_create, nullptr, &context->bottom),
             "acceleration structure creation")) {
    context->bottom = VK_NULL_HANDLE;
    return false;
  }
  const VkAccelerationStructureDeviceAddressInfoKHR bottom_address_info = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
      .accelerationStructure = context->bottom,
  };
  const VkDeviceAddress bottom_address =
      api.vkGetAccelerationStructureDeviceAddressKHR(context->device,
                                                     &bottom_address_info);

  VkAccelerationStructureInstanceKHR instance = {};
  instance.transform.matrix[0][0] = 1.0f;
  instance.transform.matrix[1][1] = 1.0f;
  instance.transform.matrix[2][2] = 1.0f;
  instance.mask = 0xffu;
  instance.accelerationStructureReference = bottom_address;
  Buffer instances;
  if (!create_buffer(
          context, sizeof(instance),
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
          true, &instances)) {
    return false;
  }
  std::memcpy(instances.mapped, &instance, sizeof(instance));
  VkAccelerationStructureGeometryKHR top_geometry = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
  };
  top_geometry.geometry.instances = {
      .sType =
          VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
      .arrayOfPointers = VK_FALSE,
      .data = {.deviceAddress = instances.address},
  };
  VkAccelerationStructureBuildGeometryInfoKHR top_build = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
      .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
      .geometryCount = 1u,
      .pGeometries = &top_geometry,
  };
  const uint32_t instance_count = 1u;
  VkAccelerationStructureBuildSizesInfoKHR top_sizes = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
  };
  api.vkGetAccelerationStructureBuildSizesKHR(
      context->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
      &top_build, &instance_count, &top_sizes);
  VkAccelerationStructureCreateInfoKHR top_create = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
      .size = top_sizes.accelerationStructureSize,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
  };
  if (!create_buffer(context, top_sizes.accelerationStructureSize,
                     storage_usage, false, &context->top_storage)) {
    destroy_buffer(context, &instances);
    return false;
  }
  top_create.buffer = context->top_storage.buffer;
  if (!vk_ok(api.vkCreateAccelerationStructureKHR(context->device, &top_create,
                                                  nullptr, &context->top),
             "acceleration structure creation")) {
    context->top = VK_NULL_HANDLE;
    destroy_buffer(context, &instances);
    return false;
  }

  /* One scratch buffer serves both builds, which a barrier orders. */
  const VkDeviceSize alignment = context->scratch_alignment;
  const VkDeviceSize scratch_size =
      std::max(bottom_sizes.buildScratchSize, top_sizes.buildScratchSize) +
      alignment;
  Buffer scratch;
  bool ok = create_buffer(context, scratch_size,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          false, &scratch) &&
            begin_commands(context);
  double build_seconds = 0.0;
  if (ok) {
    const VkDeviceAddress scratch_address =
        (scratch.address + alignment - 1u) / alignment * alignment;
    bottom_build.dstAccelerationStructure = context->bottom;
    bottom_build.scratchData.deviceAddress = scratch_address;
    top_build.dstAccelerationStructure = context->top;
    top_build.scratchData.deviceAddress = scratch_address;
    const VkAccelerationStructureBuildRangeInfoKHR *bottom_ranges = ranges;
    const VkAccelerationStructureBuildRangeInfoKHR top_range = {
        .primitiveCount = 1u,
    };
    const VkAccelerationStructureBuildRangeInfoKHR *top_ranges = &top_range;
    const VkMemoryBarrier built = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                         VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    };
    begin_timing(context);
    api.vkCmdBuildAccelerationStructuresKHR(context->command_buffer, 1u,
                                            &bottom_build, &bottom_ranges);
    api.vkCmdPipelineBarrier(
        context->command_buffer,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0u, 1u, &built,
        0u, nullptr, 0u, nullptr);
    api.vkCmdBuildAccelerationStructuresKHR(context->command_buffer, 1u,
                                            &top_build, &top_ranges);
    const VkMemoryBarrier traced = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    };
    api.vkCmdPipelineBarrier(
        context->command_buffer,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &traced, 0u, nullptr, 0u,
        nullptr);
    end_timing(context);
    ok = finish_commands(context, &build_seconds);
  }
  destroy_buffer(context, &scratch);
  destroy_buffer(context, &instances);
  if (!ok) {
    return false;
  }
  std::printf("gpu_acceleration_structure_mb=%.1f build_ms=%.1f "
              "alpha_tested_triangles=%u\n",
              (bottom_sizes.accelerationStructureSize +
               top_sizes.accelerationStructureSize) /
                  1048576.0,
              build_seconds * 1000.0, counts[1]);
  return true;
}

/* Corner attributes, triangle materials and positions, three corners per
   triangle in the scene's (BVH-partitioned) triangle order. */
bool upload_triangles(VkrBakeGpuContext *context, const VkrBakeScene &scene) {
  const size_t count = scene.triangles.size();
  std::vector<float> positions(count * 9u);
  std::vector<float> normals(count * 9u);
  std::vector<float> uvs(count * 6u);
  std::vector<uint32_t> colors(count * 3u);
  std::vector<uint32_t> materials(count);
  for (size_t t = 0u; t < count; ++t) {
    const VkrBakeTriangle &triangle = scene.triangles[t];
    for (uint32_t c = 0u; c < 3u; ++c) {
      const VkrBakeVertex &vertex = triangle.vertex[c];
      positions[9u * t + 3u * c + 0u] = vertex.position.x;
      positions[9u * t + 3u * c + 1u] = vertex.position.y;
      positions[9u * t + 3u * c + 2u] = vertex.position.z;
      normals[9u * t + 3u * c + 0u] = vertex.normal.x;
      normals[9u * t + 3u * c + 1u] = vertex.normal.y;
      normals[9u * t + 3u * c + 2u] = vertex.normal.z;
      uvs[6u * t + 2u * c + 0u] = vertex.uv.x;
      uvs[6u * t + 2u * c + 1u] = vertex.uv.y;
      colors[3u * t + c] = pack_unorm4(vertex.color);
    }
    materials[t] = triangle.material_index;
  }
  const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  return upload_buffer(
             context, positions.data(), positions.size() * sizeof(float),
             storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
             &context->positions) &&
         upload_buffer(context, normals.data(), normals.size() * sizeof(float),
                       storage, &context->normals) &&
         upload_buffer(context, uvs.data(), uvs.size() * sizeof(float), storage,
                       &context->uvs) &&
         upload_buffer(context, colors.data(), colors.size() * sizeof(uint32_t),
                       storage, &context->colors) &&
         upload_buffer(context, materials.data(),
                       materials.size() * sizeof(uint32_t), storage,
                       &context->triangle_materials);
}

/* Materials and their base color and emission textures, resampled into one
   RGBA16F array of kTextureEdge layers. */
bool upload_materials(VkrBakeGpuContext *context, const VkrBakeScene &scene) {
  std::map<std::pair<uint32_t, bool>, int32_t> layer_of;
  std::vector<VkrBakeMaterialTextureRef> layer_refs;
  auto layer_for = [&](VkrBakeMaterialTextureRef ref) -> int32_t {
    if (!ref.present) {
      return -1;
    }
    const auto key = std::make_pair(ref.texture_index, (bool)ref.srgb);
    const auto found = layer_of.find(key);
    if (found != layer_of.end()) {
      return found->second;
    }
    const int32_t layer = (int32_t)layer_refs.size();
    layer_of.emplace(key, layer);
    layer_refs.push_back(ref);
    return layer;
  };
  std::vector<GpuMaterial> gpu(std::max<size_t>(scene.materials.size(), 1u),
                               GpuMaterial{});
  for (size_t m = 0u; m < scene.materials.size(); ++m) {
    const VkrBakeMaterial &material = scene.materials[m];
    gpu[m] = {
        {material.base_color.x, material.base_color.y, material.base_color.z,
         material.base_color.w},
        {material.emissive_factor.x, material.emissive_factor.y,
         material.emissive_factor.z, material.metallic},
        {material.alpha_cutoff, material.transmission_factor,
         (float)material.alpha_mode,
         (material.transmission_factor > 0.0f &&
          material.thickness_factor <= 0.0f) ||
                 std::fabs(std::fmax(material.ior, 1.0f) - 1.0f) <= 1.0e-4f
             ? 1.0f
             : 0.0f},
        {material.dielectric_specular.x, material.dielectric_specular.y,
         material.dielectric_specular.z, material.roughness},
        {layer_for(material.textures[VKR_BAKE_MATERIAL_TEXTURE_BASE_COLOR]),
         layer_for(material.textures[VKR_BAKE_MATERIAL_TEXTURE_EMISSIVE]),
         layer_for(
             material.textures[VKR_BAKE_MATERIAL_TEXTURE_METALLIC_ROUGHNESS]),
         layer_for(material.textures[VKR_BAKE_MATERIAL_TEXTURE_TRANSMISSION])},
        {material.double_sided ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}};
  }
  if (!upload_buffer(context, gpu.data(), gpu.size() * sizeof(GpuMaterial),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &context->materials)) {
    return false;
  }

  const size_t layers = std::max<size_t>(layer_refs.size(), 1u);
  const size_t layer_values = 4u * kTextureEdge * kTextureEdge;
  std::vector<float> rgba(layer_values, 1.0f);
  std::vector<uint16_t> halves(layers * layer_values);
  std::vector<float> layer_min_alpha(layers, 1.0f);
  for (size_t layer = 0u; layer < layers; ++layer) {
    if (layer < layer_refs.size() &&
        !vkr_bake_texture_store_resample(scene.texture_store, layer_refs[layer],
                                         kTextureEdge, kTextureEdge,
                                         rgba.data())) {
      return false;
    }
    uint16_t *target = &halves[layer * layer_values];
    for (size_t i = 0u; i < layer_values; ++i) {
      target[i] = vkr_float32_to_float16(rgba[i]);
      if (i % 4u == 3u) {
        layer_min_alpha[layer] = std::fmin(layer_min_alpha[layer],
                                           vkr_float16_to_float32(target[i]));
      }
    }
  }
  if (!upload_image(context, VK_FORMAT_R16G16B16A16_SFLOAT, kTextureEdge,
                    kTextureEdge, (uint32_t)layers, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                    halves.data(), halves.size() * sizeof(uint16_t),
                    &context->textures)) {
    return false;
  }
  /* Bilinear filtering stays within its texels, so a layer's lowest texel
     alpha bounds every sample. */
  context->material_alpha_floor.assign(scene.materials.size(), 1.0f);
  for (size_t m = 0u; m < scene.materials.size(); ++m) {
    const int32_t layer = gpu[m].layers[0];
    context->material_alpha_floor[m] =
        std::fmax(scene.materials[m].base_color.w, 0.0f) *
        (layer >= 0 ? layer_min_alpha[(size_t)layer] : 1.0f);
  }
  std::printf("gpu_materials=%zu texture_layers=%zu\n", scene.materials.size(),
              layer_refs.size());
  return true;
}

bool upload_lights(VkrBakeGpuContext *context, const VkrBakeScene &scene) {
  destroy_buffer(context, &context->lights);
  context->host_lights.assign(std::max<size_t>(scene.lights.size(), 1u),
                              GpuLight{});
  for (size_t i = 0u; i < scene.lights.size(); ++i) {
    const VkrBakeSceneLight &light = scene.lights[i];
    context->host_lights[i] = {
        {light.position.x, light.position.y, light.position.z,
         (float)(uint8_t)light.kind},
        {light.direction.x, light.direction.y, light.direction.z, light.range},
        {light.color.x, light.color.y, light.color.z, light.intensity},
        {light.constant, light.linear, light.quadratic, light.radiance},
        {light.right.x, light.right.y, light.right.z, light.half_width},
        {light.up.x, light.up.y, light.up.z, light.half_height},
        // cone.z asks the kernel for shadow rays. Every baked light is
        // static and reaches the screen only through its bake, so it is
        // shadowed whatever casts_shadow says about runtime shadow maps;
        // an unshadowed lamp lit the far side of every wall.
        {std::cos(light.inner_cone_angle), std::cos(light.outer_cone_angle),
         1.0f, light.enabled ? 1.0f : 0.0f}};
  }
  return upload_buffer(context, context->host_lights.data(),
                       context->host_lights.size() * sizeof(GpuLight),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &context->lights);
}

/* The renderer's split-sum table, as the CPU BSDF reads it. */
bool upload_dfg(VkrBakeGpuContext *context) {
  return upload_image(
      context, VK_FORMAT_R16G16_SFLOAT, VKR_DFG_LUT_SIZE, VKR_DFG_LUT_SIZE, 1u,
      VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
      vkr_dfg_lut_pixels, sizeof(vkr_dfg_lut_pixels), &context->dfg);
}

/* Equirectangular sky from the CPU scene environment, so escaped paths see
   the same radiance as the reference integrator up to filtering. */
bool upload_sky(VkrBakeGpuContext *context, const VkrBakeScene &scene) {
  destroy_image(context, &context->sky);
  std::vector<float> rgba(4u * kSkyWidth * kSkyHeight, 0.0f);
  for (uint32_t y = 0u; y < kSkyHeight; ++y) {
    for (uint32_t x = 0u; x < kSkyWidth; ++x) {
      const float phi = ((x + 0.5f) / kSkyWidth - 0.5f) * 2.0f * kPi;
      const float theta = (y + 0.5f) / kSkyHeight * kPi;
      const Vec3 direction =
          vec3_new(std::sin(theta) * std::cos(phi), std::cos(theta),
                   std::sin(theta) * std::sin(phi));
      const Vec3 radiance =
          vkr_bake_scene_sample_environment(&scene, direction);
      float *texel = &rgba[4u * (y * kSkyWidth + x)];
      texel[0] = radiance.x;
      texel[1] = radiance.y;
      texel[2] = radiance.z;
      texel[3] = 1.0f;
    }
  }
  return upload_image(context, VK_FORMAT_R32G32B32A32_SFLOAT, kSkyWidth,
                      kSkyHeight, 1u, VK_IMAGE_VIEW_TYPE_2D,
                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                      rgba.data(), rgba.size() * sizeof(float), &context->sky);
}

bool create_sampler(VkrBakeGpuContext *context, VkSamplerAddressMode address_u,
                    VkSamplerAddressMode address_v, VkSampler *out_sampler) {
  const VkSamplerCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = address_u,
      .addressModeV = address_v,
      .addressModeW = address_v,
      .maxLod = 0.0f,
  };
  return vk_ok(context->vulkan.api.vkCreateSampler(context->device, &create,
                                                   nullptr, out_sampler),
               "sampler creation");
}

bool create_pipeline(VkrBakeGpuContext *context, VkPipelineLayout layout,
                     const uint8_t *spirv, size_t size, const char *entry,
                     VkPipeline *out_pipeline) {
  const VulkanApi &api = context->vulkan.api;
  /* The embedded bytes carry no alignment; the module wants words. */
  std::vector<uint32_t> words(size / sizeof(uint32_t));
  std::memcpy(words.data(), spirv, words.size() * sizeof(uint32_t));
  const VkShaderModuleCreateInfo module_create = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = words.size() * sizeof(uint32_t),
      .pCode = words.data(),
  };
  VkShaderModule module = VK_NULL_HANDLE;
  if (!vk_ok(api.vkCreateShaderModule(context->device, &module_create, nullptr,
                                      &module),
             "shader module creation")) {
    return false;
  }
  const VkComputePipelineCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
          {
              .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
              .module = module,
              .pName = entry,
          },
      .layout = layout,
  };
  const VkResult result = api.vkCreateComputePipelines(
      context->device, VK_NULL_HANDLE, 1u, &create, nullptr, out_pipeline);
  api.vkDestroyShaderModule(context->device, module, nullptr);
  if (result != VK_SUCCESS) {
    std::fprintf(stderr, "Lightmap pipeline %s failed (VkResult %d)\n", entry,
                 (int)result);
    *out_pipeline = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

/* The device, queue, command and timing objects, the descriptor set layout,
   both pipelines and the samplers. */
bool create_device_objects(VkrBakeGpuContext *context,
                           const DeviceChoice &choice) {
  VulkanApi &api = context->vulkan.api;
  context->physical = choice.physical;
  context->queue_family = choice.queue_family;
  api.vkGetPhysicalDeviceMemoryProperties(context->physical,
                                          &context->memory_properties);

  const float priority = 1.0f;
  const VkDeviceQueueCreateInfo queue_create = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = context->queue_family,
      .queueCount = 1u,
      .pQueuePriorities = &priority,
  };
  /* The BC encoders' kernels use 16-bit floats and integers (BC6H) and
     integer dot products (BC7), and their decode samples BC textures. The
     transport needs none of them, so a device without them still bakes. */
  const bool vulkan13 = choice.properties.apiVersion >= VK_API_VERSION_1_3;
  VkPhysicalDeviceVulkan13Features vulkan13_supported = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
  };
  VkPhysicalDeviceVulkan12Features vulkan12_supported = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = vulkan13 ? &vulkan13_supported : nullptr,
  };
  VkPhysicalDeviceFeatures2 supported = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &vulkan12_supported,
  };
  api.vkGetPhysicalDeviceFeatures2(context->physical, &supported);
  context->bc_supported =
      vulkan13 && vulkan13_supported.shaderIntegerDotProduct &&
      vulkan12_supported.shaderFloat16 && supported.features.shaderInt16 &&
      supported.features.textureCompressionBC;
  const VkBool32 bc = context->bc_supported ? VK_TRUE : VK_FALSE;

  VkPhysicalDeviceRayQueryFeaturesKHR ray_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
      .rayQuery = VK_TRUE,
  };
  VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration = {
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
      .pNext = &ray_query,
      .accelerationStructure = VK_TRUE,
  };
  VkPhysicalDeviceVulkan13Features vulkan13_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &acceleration,
      .shaderIntegerDotProduct = bc,
  };
  VkPhysicalDeviceVulkan12Features vulkan12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = vulkan13 ? (void *)&vulkan13_features : (void *)&acceleration,
      .shaderFloat16 = bc,
      .bufferDeviceAddress = VK_TRUE,
  };
  VkPhysicalDeviceFeatures2 features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &vulkan12,
  };
  features.features.shaderInt16 = bc;
  features.features.textureCompressionBC = bc;
  const VkDeviceCreateInfo device_create = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features,
      .queueCreateInfoCount = 1u,
      .pQueueCreateInfos = &queue_create,
      .enabledExtensionCount =
          (uint32_t)(sizeof(kDeviceExtensions) / sizeof(kDeviceExtensions[0])),
      .ppEnabledExtensionNames = kDeviceExtensions,
  };
  if (!vk_ok(api.vkCreateDevice(context->physical, &device_create, nullptr,
                                &context->device),
             "device creation")) {
    context->device = VK_NULL_HANDLE;
    return false;
  }
  bool loaded = true;
#define VKR_BAKE_VK_LOAD_DEVICE(name)                                          \
  api.name = (PFN_##name)api.vkGetDeviceProcAddr(context->device, #name);      \
  loaded = loaded && api.name != nullptr;
  VKR_BAKE_VK_DEVICE_FUNCTIONS(VKR_BAKE_VK_LOAD_DEVICE)
#undef VKR_BAKE_VK_LOAD_DEVICE
  if (!loaded) {
    std::fprintf(stderr, "Lightmap Vulkan device functions are missing\n");
    return false;
  }
  api.vkGetDeviceQueue(context->device, context->queue_family, 0u,
                       &context->queue);

  uint32_t family_count = 0u;
  api.vkGetPhysicalDeviceQueueFamilyProperties(context->physical, &family_count,
                                               nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  api.vkGetPhysicalDeviceQueueFamilyProperties(context->physical, &family_count,
                                               families.data());
  const uint32_t valid_bits =
      families[context->queue_family].timestampValidBits;
  context->timestamp_mask =
      valid_bits >= 64u ? ~0ull : ((1ull << valid_bits) - 1ull);
  context->timestamp_period = choice.properties.limits.timestampPeriod;
  context->uniform_alignment = std::max<VkDeviceSize>(
      choice.properties.limits.minUniformBufferOffsetAlignment, 1u);

  VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties = {
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
  };
  VkPhysicalDeviceProperties2 properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &acceleration_properties,
  };
  api.vkGetPhysicalDeviceProperties2(context->physical, &properties);
  context->scratch_alignment = std::max<VkDeviceSize>(
      acceleration_properties.minAccelerationStructureScratchOffsetAlignment,
      1u);

  const VkCommandPoolCreateInfo pool_create = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = context->queue_family,
  };
  if (!vk_ok(api.vkCreateCommandPool(context->device, &pool_create, nullptr,
                                     &context->command_pool),
             "command pool creation")) {
    context->command_pool = VK_NULL_HANDLE;
    return false;
  }
  const VkCommandBufferAllocateInfo command_allocate = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = context->command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1u,
  };
  const VkFenceCreateInfo fence_create = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
  };
  const VkQueryPoolCreateInfo query_create = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = 2u,
  };
  if (!vk_ok(api.vkAllocateCommandBuffers(context->device, &command_allocate,
                                          &context->command_buffer),
             "command buffer allocation") ||
      !vk_ok(api.vkCreateFence(context->device, &fence_create, nullptr,
                               &context->fence),
             "fence creation") ||
      !vk_ok(api.vkCreateQueryPool(context->device, &query_create, nullptr,
                                   &context->timestamps),
             "query pool creation")) {
    return false;
  }

  VkDescriptorSetLayoutBinding bindings[kBindingCount] = {};
  for (uint32_t b = 0u; b < kBindingCount; ++b) {
    bindings[b].binding = b;
    bindings[b].descriptorCount = 1u;
    bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
  bindings[kBindingScene].descriptorType =
      VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  bindings[kBindingTextures].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[kBindingSky].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[kBindingDfg].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[kBindingTextureSampler].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[kBindingSkySampler].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[kBindingDfgSampler].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  const VkDescriptorSetLayoutCreateInfo set_create = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = kBindingCount,
      .pBindings = bindings,
  };
  if (!vk_ok(api.vkCreateDescriptorSetLayout(context->device, &set_create,
                                             nullptr, &context->set_layout),
             "descriptor set layout creation")) {
    context->set_layout = VK_NULL_HANDLE;
    return false;
  }
  /* Both kernels share the layout; the gather's arguments are the larger
     push constant block. */
  const VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0u,
      .size = sizeof(GatherArgs),
  };
  const VkPipelineLayoutCreateInfo layout_create = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1u,
      .pSetLayouts = &context->set_layout,
      .pushConstantRangeCount = 1u,
      .pPushConstantRanges = &push_range,
  };
  if (!vk_ok(api.vkCreatePipelineLayout(context->device, &layout_create,
                                        nullptr, &context->pipeline_layout),
             "pipeline layout creation")) {
    context->pipeline_layout = VK_NULL_HANDLE;
    return false;
  }
  const VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindingCount},
      {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3u},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 3u},
  };
  const VkDescriptorPoolCreateInfo descriptor_pool_create = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1u,
      .poolSizeCount = (uint32_t)(sizeof(pool_sizes) / sizeof(pool_sizes[0])),
      .pPoolSizes = pool_sizes,
  };
  if (!vk_ok(api.vkCreateDescriptorPool(context->device,
                                        &descriptor_pool_create, nullptr,
                                        &context->descriptor_pool),
             "descriptor pool creation")) {
    context->descriptor_pool = VK_NULL_HANDLE;
    return false;
  }
  const VkDescriptorSetAllocateInfo set_allocate = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = context->descriptor_pool,
      .descriptorSetCount = 1u,
      .pSetLayouts = &context->set_layout,
  };
  if (!vk_ok(api.vkAllocateDescriptorSets(context->device, &set_allocate,
                                          &context->descriptor_set),
             "descriptor set allocation")) {
    return false;
  }
  return create_pipeline(context, context->pipeline_layout,
                         k_lightmap_trace_benchmark_spirv,
                         sizeof(k_lightmap_trace_benchmark_spirv),
                         "lightmap_trace_benchmark", &context->benchmark) &&
         create_pipeline(context, context->pipeline_layout,
                         k_lightmap_gather_spirv,
                         sizeof(k_lightmap_gather_spirv), "lightmap_gather",
                         &context->gather) &&
         create_pipeline(context, context->pipeline_layout,
                         k_probe_gather_spirv, sizeof(k_probe_gather_spirv),
                         "probe_gather", &context->probes) &&
         create_sampler(context, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                        VK_SAMPLER_ADDRESS_MODE_REPEAT,
                        &context->texture_sampler) &&
         create_sampler(context, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        &context->sky_sampler) &&
         create_sampler(context, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        &context->dfg_sampler);
}

/* Releases everything a context holds, whatever its creation reached. */
void release_context(VkrBakeGpuContext *context) {
  const VulkanApi &api = context->vulkan.api;
  if (context->device != VK_NULL_HANDLE) {
    api.vkDeviceWaitIdle(context->device);
    Buffer *buffers[] = {&context->positions,
                         &context->normals,
                         &context->uvs,
                         &context->colors,
                         &context->triangle_materials,
                         &context->materials,
                         &context->lights,
                         &context->as_indices,
                         &context->bottom_storage,
                         &context->top_storage};
    if (context->top != VK_NULL_HANDLE) {
      api.vkDestroyAccelerationStructureKHR(context->device, context->top,
                                            nullptr);
    }
    if (context->bottom != VK_NULL_HANDLE) {
      api.vkDestroyAccelerationStructureKHR(context->device, context->bottom,
                                            nullptr);
    }
    for (Buffer *buffer : buffers) {
      destroy_buffer(context, buffer);
    }
    destroy_image(context, &context->textures);
    destroy_image(context, &context->sky);
    destroy_image(context, &context->dfg);
    VkSampler samplers[] = {context->texture_sampler, context->sky_sampler,
                            context->dfg_sampler};
    for (VkSampler sampler : samplers) {
      if (sampler != VK_NULL_HANDLE) {
        api.vkDestroySampler(context->device, sampler, nullptr);
      }
    }
    VkPipeline pipelines[] = {context->benchmark, context->gather,
                              context->probes};
    for (VkPipeline pipeline : pipelines) {
      if (pipeline != VK_NULL_HANDLE) {
        api.vkDestroyPipeline(context->device, pipeline, nullptr);
      }
    }
    for (VkPipeline pipeline : context->bc_pipelines) {
      if (pipeline != VK_NULL_HANDLE) {
        api.vkDestroyPipeline(context->device, pipeline, nullptr);
      }
    }
    if (context->bc_descriptor_pool != VK_NULL_HANDLE) {
      api.vkDestroyDescriptorPool(context->device, context->bc_descriptor_pool,
                                  nullptr);
    }
    if (context->bc_pipeline_layout != VK_NULL_HANDLE) {
      api.vkDestroyPipelineLayout(context->device, context->bc_pipeline_layout,
                                  nullptr);
    }
    if (context->bc_set_layout != VK_NULL_HANDLE) {
      api.vkDestroyDescriptorSetLayout(context->device, context->bc_set_layout,
                                       nullptr);
    }
    if (context->descriptor_pool != VK_NULL_HANDLE) {
      api.vkDestroyDescriptorPool(context->device, context->descriptor_pool,
                                  nullptr);
    }
    if (context->pipeline_layout != VK_NULL_HANDLE) {
      api.vkDestroyPipelineLayout(context->device, context->pipeline_layout,
                                  nullptr);
    }
    if (context->set_layout != VK_NULL_HANDLE) {
      api.vkDestroyDescriptorSetLayout(context->device, context->set_layout,
                                       nullptr);
    }
    if (context->timestamps != VK_NULL_HANDLE) {
      api.vkDestroyQueryPool(context->device, context->timestamps, nullptr);
    }
    if (context->fence != VK_NULL_HANDLE) {
      api.vkDestroyFence(context->device, context->fence, nullptr);
    }
    if (context->command_pool != VK_NULL_HANDLE) {
      api.vkDestroyCommandPool(context->device, context->command_pool, nullptr);
    }
    api.vkDestroyDevice(context->device, nullptr);
    context->device = VK_NULL_HANDLE;
  }
  close_instance(&context->vulkan);
}

} // namespace

bool vkr_bake_gpu_available() {
  VulkanInstance vulkan;
  if (!open_instance(&vulkan)) {
    return false;
  }
  DeviceChoice choice;
  const bool available = choose_device(vulkan, &choice);
  close_instance(&vulkan);
  return available;
}

VkrBakeGpuContext *vkr_bake_gpu_create(const VkrBakeScene &scene) {
  if (scene.triangles.empty()) {
    return nullptr;
  }
  VkrBakeGpuContext *context = new (std::nothrow) VkrBakeGpuContext();
  if (!context) {
    return nullptr;
  }
  DeviceChoice choice;
  if (!open_instance(&context->vulkan) ||
      !choose_device(context->vulkan, &choice)) {
    std::fprintf(stderr, "No Vulkan device with ray queries\n");
    release_context(context);
    delete context;
    return nullptr;
  }
  std::printf("gpu_device=\"%s\" vulkan=%u.%u\n", choice.properties.deviceName,
              VK_API_VERSION_MAJOR(choice.properties.apiVersion),
              VK_API_VERSION_MINOR(choice.properties.apiVersion));
  if (!create_device_objects(context, choice) ||
      !upload_triangles(context, scene) || !upload_materials(context, scene) ||
      !build_acceleration_structure(context, scene) ||
      !upload_lights(context, scene) || !upload_sky(context, scene) ||
      !upload_dfg(context)) {
    std::fprintf(stderr, "Lightmap GPU scene upload failed\n");
    release_context(context);
    delete context;
    return nullptr;
  }
  return context;
}

void vkr_bake_gpu_destroy(VkrBakeGpuContext *context) {
  if (!context) {
    return;
  }
  release_context(context);
  delete context;
}

bool vkr_bake_gpu_update_lighting(VkrBakeGpuContext *context,
                                  const VkrBakeScene &scene) {
  return context && upload_lights(context, scene) && upload_sky(context, scene);
}

namespace {

/* The host-visible buffers of one benchmark or gather call, released when
   the call returns; every submission has completed by then. */
struct CallBuffers {
  VkrBakeGpuContext *context = nullptr;
  Buffer positions;
  Buffer normals;
  Buffer footprints;
  Buffer hits;
  Buffer indirect;
  Buffer direct;
  Buffer moments;
  Buffer occlusion;
  Buffer backface;
  Buffer direction;
  Buffer layer_lights;

  explicit CallBuffers(VkrBakeGpuContext *owner) : context(owner) {}

  ~CallBuffers() {
    Buffer *buffers[] = {&positions, &normals,   &footprints,  &hits,
                         &indirect,  &direct,    &moments,     &occlusion,
                         &backface,  &direction, &layer_lights};
    for (Buffer *buffer : buffers) {
      destroy_buffer(context, buffer);
    }
  }
};

/* Texel centers and normals as float4 arrays, and when `footprints` is set
   the two footprint steps of each texel. */
bool upload_texels(CallBuffers *call,
                   const std::vector<VkrBakeLightmapTexel> &texels,
                   bool footprints) {
  const VkDeviceSize count = texels.size();
  const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!create_buffer(call->context, count * 4u * sizeof(float), storage, true,
                     &call->positions) ||
      !create_buffer(call->context, count * 4u * sizeof(float), storage, true,
                     &call->normals)) {
    return false;
  }
  if (footprints) {
    if (!create_buffer(call->context, count * 8u * sizeof(float), storage, true,
                       &call->footprints)) {
      return false;
    }
    float *f = static_cast<float *>(call->footprints.mapped);
    for (size_t i = 0u; i < texels.size(); ++i) {
      const VkrBakeLightmapTexel &texel = texels[i];
      const Vec3 steps[2] = {texel.step_x, texel.step_y};
      for (uint32_t k = 0u; k < 2u; ++k) {
        f[8u * i + 4u * k + 0u] = steps[k].x;
        f[8u * i + 4u * k + 1u] = steps[k].y;
        f[8u * i + 4u * k + 2u] = steps[k].z;
        f[8u * i + 4u * k + 3u] = 0.0f;
      }
    }
  }
  float *p = static_cast<float *>(call->positions.mapped);
  float *n = static_cast<float *>(call->normals.mapped);
  for (size_t i = 0u; i < texels.size(); ++i) {
    const VkrBakeLightmapTexel &texel = texels[i];
    p[4u * i + 0u] = texel.position.x;
    p[4u * i + 1u] = texel.position.y;
    p[4u * i + 2u] = texel.position.z;
    p[4u * i + 3u] = 1.0f;
    n[4u * i + 0u] = texel.normal.x;
    n[4u * i + 1u] = texel.normal.y;
    n[4u * i + 2u] = texel.normal.z;
    n[4u * i + 3u] = 0.0f;
  }
  return true;
}

/* Descriptor writes for one call, gathered before one update. */
struct DescriptorWrites {
  VkDescriptorBufferInfo buffers[kBindingCount] = {};
  VkDescriptorImageInfo images[kBindingCount] = {};
  VkWriteDescriptorSetAccelerationStructureKHR scene = {};
  VkWriteDescriptorSet writes[kBindingCount] = {};
  uint32_t count = 0u;
};

void write_buffer(DescriptorWrites *writes, VkDescriptorSet set,
                  uint32_t binding, const Buffer &buffer) {
  writes->buffers[binding] = {buffer.buffer, 0u, VK_WHOLE_SIZE};
  writes->writes[writes->count++] = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = binding,
      .descriptorCount = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &writes->buffers[binding],
  };
}

void write_image(DescriptorWrites *writes, VkDescriptorSet set,
                 uint32_t binding, VkImageView view) {
  writes->images[binding] = {VK_NULL_HANDLE, view,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  writes->writes[writes->count++] = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = binding,
      .descriptorCount = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .pImageInfo = &writes->images[binding],
  };
}

void write_sampler(DescriptorWrites *writes, VkDescriptorSet set,
                   uint32_t binding, VkSampler sampler) {
  writes->images[binding] = {sampler, VK_NULL_HANDLE,
                             VK_IMAGE_LAYOUT_UNDEFINED};
  writes->writes[writes->count++] = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = binding,
      .descriptorCount = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
      .pImageInfo = &writes->images[binding],
  };
}

void write_scene(DescriptorWrites *writes, const VkrBakeGpuContext *context) {
  writes->scene = {
      .sType =
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
      .accelerationStructureCount = 1u,
      .pAccelerationStructures = &context->top,
  };
  writes->writes[writes->count++] = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .pNext = &writes->scene,
      .dstSet = context->descriptor_set,
      .dstBinding = kBindingScene,
      .descriptorCount = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
  };
}

/* Records, submits and waits for one dispatch of `texel_count` threads with
   these push constants, adding its GPU time. The dispatch first sees the
   previous submission's writes, and the host sees its own afterwards. */
bool dispatch(VkrBakeGpuContext *context, VkPipeline pipeline,
              const void *arguments, uint32_t argument_size,
              uint32_t texel_count, double *in_out_gpu_seconds) {
  const VulkanApi &api = context->vulkan.api;
  if (!begin_commands(context)) {
    return false;
  }
  const VkCommandBuffer command = context->command_buffer;
  const VkMemoryBarrier previous = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
  };
  api.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u,
                           &previous, 0u, nullptr, 0u, nullptr);
  begin_timing(context);
  api.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  api.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                              context->pipeline_layout, 0u, 1u,
                              &context->descriptor_set, 0u, nullptr);
  api.vkCmdPushConstants(command, context->pipeline_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0u, argument_size,
                         arguments);
  api.vkCmdDispatch(
      command, (texel_count + kWorkgroupSize - 1u) / kWorkgroupSize, 1u, 1u);
  end_timing(context);
  const VkMemoryBarrier readback = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
  };
  api.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0u, 1u, &readback, 0u,
                           nullptr, 0u, nullptr);
  return finish_commands(context, in_out_gpu_seconds);
}

} // namespace

bool vkr_bake_gpu_trace_benchmark(
    VkrBakeGpuContext *context, const std::vector<VkrBakeLightmapTexel> &texels,
    uint32_t samples, uint32_t seed, std::vector<float32_t> *out_hit_fraction,
    double *out_gpu_seconds) {
  if (!context || !out_hit_fraction || !out_gpu_seconds || samples == 0u) {
    return false;
  }
  const size_t count = texels.size();
  out_hit_fraction->assign(count, 0.0f);
  *out_gpu_seconds = 0.0;
  if (count == 0u) {
    return true;
  }
  CallBuffers call(context);
  if (!create_buffer(context, count * sizeof(float),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, &call.hits) ||
      !upload_texels(&call, texels, false)) {
    return false;
  }
  DescriptorWrites writes;
  const VkDescriptorSet set = context->descriptor_set;
  write_buffer(&writes, set, kBindingTexelPositions, call.positions);
  write_buffer(&writes, set, kBindingTexelNormals, call.normals);
  write_buffer(&writes, set, kBindingHitFraction, call.hits);
  write_scene(&writes, context);
  context->vulkan.api.vkUpdateDescriptorSets(context->device, writes.count,
                                             writes.writes, 0u, nullptr);
  for (size_t first = 0u; first < count; first += kTexelsPerDispatch) {
    const BenchmarkArgs args = {
        (uint32_t)first,
        (uint32_t)std::min<size_t>(kTexelsPerDispatch, count - first), samples,
        seed};
    if (!dispatch(context, context->benchmark, &args, sizeof(args),
                  args.texel_count, out_gpu_seconds)) {
      return false;
    }
  }
  const float *result = static_cast<const float *>(call.hits.mapped);
  std::copy(result, result + count, out_hit_fraction->begin());
  return true;
}

bool vkr_bake_gpu_gather(VkrBakeGpuContext *context,
                         const std::vector<VkrBakeLightmapTexel> &texels,
                         const VkrBakeGpuLayer &layer,
                         const VkrBakeGpuGatherSettings &settings,
                         VkrBakeGpuGatherResult *out_result) {
  if (!context || !out_result || settings.samples == 0u ||
      settings.max_depth == 0u ||
      (settings.occlusion && !(settings.occlusion_radius > 0.0f)) ||
      !(settings.indirect_clamp >= 0.0f)) {
    return false;
  }
  const size_t count = texels.size();
  VkrBakeGpuGatherResult &result = *out_result;
  result.direct.assign(count, vec3_zero());
  result.indirect.assign(count, vec3_zero());
  result.indirect_variance.assign(count, 0.0f);
  result.occlusion.assign(settings.occlusion ? count : 0u, 1.0f);
  result.backface.assign(settings.backface ? count : 0u, 0.0f);
  result.direction.assign(settings.direction ? count : 0u, vec4_zero());
  result.gpu_seconds = 0.0;
  if (count == 0u) {
    return true;
  }
  for (uint32_t light : layer.lights) {
    if (light >= context->host_lights.size()) {
      return false;
    }
  }
  /* Lights the texel evaluates once come first, then the rectangles it
     samples per sample. */
  std::vector<uint32_t> ordered = layer.lights;
  const auto rectangles = std::stable_partition(
      ordered.begin(), ordered.end(), [&](uint32_t light) {
        return (uint32_t)context->host_lights[light].position_kind[3] !=
               (uint32_t)VkrBakeSceneLightKind::Rectangle;
      });
  const uint32_t rectangle_first = (uint32_t)(rectangles - ordered.begin());

  CallBuffers call(context);
  const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!create_buffer(context, count * 4u * sizeof(float), storage, true,
                     &call.indirect) ||
      !create_buffer(context, count * 4u * sizeof(float), storage, true,
                     &call.direct) ||
      !create_buffer(context, count * 2u * sizeof(float), storage, true,
                     &call.moments) ||
      (settings.occlusion && !create_buffer(context, count * sizeof(float),
                                            storage, true, &call.occlusion)) ||
      (settings.backface && !create_buffer(context, count * sizeof(float),
                                           storage, true, &call.backface)) ||
      (settings.direction && !create_buffer(context, count * 4u * sizeof(float),
                                            storage, true, &call.direction)) ||
      !upload_buffer(context, ordered.data(), ordered.size() * sizeof(uint32_t),
                     storage, &call.layer_lights) ||
      !upload_texels(&call, texels, true)) {
    return false;
  }
  /* A buffer the gather does not fill is bound to a placeholder. */
  const Buffer &occlusion = settings.occlusion ? call.occlusion : call.moments;
  const Buffer &backface = settings.backface ? call.backface : call.moments;
  const Buffer &direction = settings.direction ? call.direction : call.moments;

  DescriptorWrites writes;
  const VkDescriptorSet set = context->descriptor_set;
  write_buffer(&writes, set, kBindingTexelPositions, call.positions);
  write_buffer(&writes, set, kBindingTexelNormals, call.normals);
  write_buffer(&writes, set, kBindingIndirect, call.indirect);
  write_scene(&writes, context);
  write_buffer(&writes, set, kBindingPositions, context->positions);
  write_buffer(&writes, set, kBindingNormals, context->normals);
  write_buffer(&writes, set, kBindingUvs, context->uvs);
  write_buffer(&writes, set, kBindingColors, context->colors);
  write_buffer(&writes, set, kBindingTriangleMaterials,
               context->triangle_materials);
  write_buffer(&writes, set, kBindingMaterials, context->materials);
  write_buffer(&writes, set, kBindingLights, context->lights);
  write_buffer(&writes, set, kBindingLayerLights, call.layer_lights);
  write_buffer(&writes, set, kBindingAsIndices, context->as_indices);
  write_buffer(&writes, set, kBindingOcclusion, occlusion);
  write_buffer(&writes, set, kBindingDirect, call.direct);
  write_buffer(&writes, set, kBindingMoments, call.moments);
  write_buffer(&writes, set, kBindingBackface, backface);
  write_buffer(&writes, set, kBindingDirection, direction);
  write_buffer(&writes, set, kBindingFootprints, call.footprints);
  write_image(&writes, set, kBindingTextures, context->textures.view);
  write_image(&writes, set, kBindingSky, context->sky.view);
  write_image(&writes, set, kBindingDfg, context->dfg.view);
  write_sampler(&writes, set, kBindingTextureSampler, context->texture_sampler);
  write_sampler(&writes, set, kBindingSkySampler, context->sky_sampler);
  write_sampler(&writes, set, kBindingDfgSampler, context->dfg_sampler);
  context->vulkan.api.vkUpdateDescriptorSets(context->device, writes.count,
                                             writes.writes, 0u, nullptr);

  const uint32_t flags =
      (layer.sky ? 1u : 0u) | (layer.emission ? 2u : 0u) |
      (layer.texel_direct ? 4u : 0u) | (settings.occlusion ? 8u : 0u) |
      (settings.backface ? 16u : 0u) | (settings.direction ? 32u : 0u);
  /* Each batch of texels takes its samples in runs of kSamplesPerDispatch, a
     submission each. */
  const uint32_t runs =
      (settings.samples + kSamplesPerDispatch - 1u) / kSamplesPerDispatch;
  const size_t batches = (count + kTexelsPerDispatch - 1u) / kTexelsPerDispatch;
  for (size_t index = 0u; index < batches * runs; ++index) {
    const size_t first = (index / runs) * kTexelsPerDispatch;
    const uint32_t first_sample =
        (uint32_t)(index % runs) * kSamplesPerDispatch;
    const GatherArgs args = {
        (uint32_t)first,
        (uint32_t)std::min<size_t>(kTexelsPerDispatch, count - first),
        settings.samples,
        first_sample,
        std::min(kSamplesPerDispatch, settings.samples - first_sample),
        settings.seed,
        settings.max_depth,
        settings.rr_start_depth,
        (uint32_t)layer.lights.size(),
        rectangle_first,
        flags,
        context->geometry_first[0],
        context->geometry_first[1],
        settings.occlusion_radius,
        settings.indirect_clamp};
    if (!dispatch(context, context->gather, &args, sizeof(args),
                  args.texel_count, &result.gpu_seconds)) {
      return false;
    }
  }
  const float *indirect_values =
      static_cast<const float *>(call.indirect.mapped);
  const float *direct_values = static_cast<const float *>(call.direct.mapped);
  const float *moment_values = static_cast<const float *>(call.moments.mapped);
  for (size_t i = 0u; i < count; ++i) {
    result.indirect[i] =
        vec3_new(indirect_values[4u * i], indirect_values[4u * i + 1u],
                 indirect_values[4u * i + 2u]);
    result.direct[i] =
        vec3_new(direct_values[4u * i], direct_values[4u * i + 1u],
                 direct_values[4u * i + 2u]);
    /* The variance of a mean of n samples is the sample variance over n. */
    const float mean = moment_values[2u * i];
    const float mean_square = moment_values[2u * i + 1u];
    result.indirect_variance[i] =
        std::fmax(mean_square - mean * mean, 0.0f) / (float)settings.samples;
  }
  if (settings.occlusion) {
    const float *visibility = static_cast<const float *>(call.occlusion.mapped);
    for (size_t i = 0u; i < count; ++i) {
      result.occlusion[i] = std::clamp(visibility[i], 0.0f, 1.0f);
    }
  }
  if (settings.backface) {
    const float *share = static_cast<const float *>(call.backface.mapped);
    for (size_t i = 0u; i < count; ++i) {
      result.backface[i] = std::clamp(share[i], 0.0f, 1.0f);
    }
  }
  if (settings.direction) {
    const float *sums = static_cast<const float *>(call.direction.mapped);
    for (size_t i = 0u; i < count; ++i) {
      result.direction[i] = vec4_new(sums[4u * i], sums[4u * i + 1u],
                                     sums[4u * i + 2u], sums[4u * i + 3u]);
    }
  }
  return true;
}

bool vkr_bake_gpu_probe_gather_available() { return vkr_bake_gpu_available(); }

bool vkr_bake_gpu_gather_probes(VkrBakeGpuContext *context,
                                const std::vector<Vec3> &positions,
                                const std::vector<uint32_t> &seeds,
                                const VkrBakeGpuLayer &layer,
                                const VkrBakeGpuProbeSettings &settings,
                                std::vector<float32_t> *out_sh,
                                double *out_gpu_seconds) {
  if (!context || !out_sh || !out_gpu_seconds ||
      seeds.size() != positions.size() || settings.face_size == 0u ||
      settings.face_size > VKR_SH_PROJECTION_MAX_FACE_SIZE ||
      settings.samples == 0u || settings.max_depth == 0u ||
      !std::isfinite(settings.deringing) || settings.deringing < 0.0f) {
    return false;
  }
  const size_t count = positions.size();
  out_sh->assign(count * 12u, 0.0f);
  *out_gpu_seconds = 0.0;
  if (count == 0u) {
    return true;
  }
  for (uint32_t light : layer.lights) {
    if (light >= context->host_lights.size()) {
      return false;
    }
  }
  const uint32_t pixels = 6u * settings.face_size * settings.face_size;
  const uint64_t probe_paths = (uint64_t)pixels * settings.samples;
  const uint32_t probes_per_dispatch =
      (uint32_t)std::max<uint64_t>(1u, kProbePathsPerDispatch / probe_paths);

  /* Probe inputs for the whole layer, and one dispatch's radiance, which the
     host projects before the next dispatch overwrites it. */
  CallBuffers call(context);
  Buffer seed_buffer;
  Buffer radiance;
  const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  bool ok = create_buffer(context, count * 4u * sizeof(float), storage, true,
                          &call.positions) &&
            create_buffer(context, count * sizeof(uint32_t), storage, true,
                          &seed_buffer) &&
            create_buffer(context,
                          (VkDeviceSize)probes_per_dispatch * pixels * 4u *
                              sizeof(float),
                          storage, true, &radiance) &&
            upload_buffer(context, layer.lights.data(),
                          layer.lights.size() * sizeof(uint32_t), storage,
                          &call.layer_lights);
  if (ok) {
    float *p = static_cast<float *>(call.positions.mapped);
    for (size_t i = 0u; i < count; ++i) {
      p[4u * i + 0u] = positions[i].x;
      p[4u * i + 1u] = positions[i].y;
      p[4u * i + 2u] = positions[i].z;
      p[4u * i + 3u] = 1.0f;
    }
    std::memcpy(seed_buffer.mapped, seeds.data(), count * sizeof(uint32_t));

    DescriptorWrites writes;
    const VkDescriptorSet set = context->descriptor_set;
    write_scene(&writes, context);
    write_buffer(&writes, set, kBindingPositions, context->positions);
    write_buffer(&writes, set, kBindingNormals, context->normals);
    write_buffer(&writes, set, kBindingUvs, context->uvs);
    write_buffer(&writes, set, kBindingColors, context->colors);
    write_buffer(&writes, set, kBindingTriangleMaterials,
                 context->triangle_materials);
    write_buffer(&writes, set, kBindingMaterials, context->materials);
    write_buffer(&writes, set, kBindingLights, context->lights);
    write_buffer(&writes, set, kBindingLayerLights, call.layer_lights);
    write_buffer(&writes, set, kBindingAsIndices, context->as_indices);
    write_buffer(&writes, set, kBindingProbePositions, call.positions);
    write_buffer(&writes, set, kBindingProbeSeeds, seed_buffer);
    write_buffer(&writes, set, kBindingProbeRadiance, radiance);
    write_image(&writes, set, kBindingTextures, context->textures.view);
    write_image(&writes, set, kBindingSky, context->sky.view);
    write_image(&writes, set, kBindingDfg, context->dfg.view);
    write_sampler(&writes, set, kBindingTextureSampler,
                  context->texture_sampler);
    write_sampler(&writes, set, kBindingSkySampler, context->sky_sampler);
    write_sampler(&writes, set, kBindingDfgSampler, context->dfg_sampler);
    context->vulkan.api.vkUpdateDescriptorSets(context->device, writes.count,
                                               writes.writes, 0u, nullptr);
  }

  const uint32_t flags = (layer.sky ? 1u : 0u) | (layer.emission ? 2u : 0u);
  std::vector<Vec3> cube(pixels);
  double longest_dispatch = 0.0;
  uint32_t dispatches = 0u;
  for (size_t first = 0u; ok && first < count; first += probes_per_dispatch) {
    const ProbeArgs args = {
        (uint32_t)first,
        (uint32_t)std::min<size_t>(probes_per_dispatch, count - first),
        settings.face_size,
        settings.samples,
        settings.max_depth,
        settings.rr_start_depth,
        (uint32_t)layer.lights.size(),
        flags,
        context->geometry_first[0],
        context->geometry_first[1]};
    double seconds = 0.0;
    ok = dispatch(context, context->probes, &args, sizeof(args),
                  args.probe_count * pixels, &seconds);
    *out_gpu_seconds += seconds;
    longest_dispatch = std::max(longest_dispatch, seconds);
    ++dispatches;
    const float *values = static_cast<const float *>(radiance.mapped);
    for (uint32_t local = 0u; ok && local < args.probe_count; ++local) {
      const float *source = values + (size_t)local * pixels * 4u;
      for (uint32_t pixel = 0u; pixel < pixels; ++pixel) {
        cube[pixel] = vec3_new(source[4u * pixel], source[4u * pixel + 1u],
                               source[4u * pixel + 2u]);
      }
      const size_t probe = first + local;
      float32_t sh[3][4];
      ok = vkr_bake_sh_project_l1(cube.data(), settings.face_size,
                                  settings.deringing, sh);
      if (!ok) {
        std::fprintf(stderr, "Probe %zu gathered non-finite radiance\n", probe);
        break;
      }
      std::memcpy(out_sh->data() + probe * 12u, sh, sizeof(sh));
    }
  }
  destroy_buffer(context, &seed_buffer);
  destroy_buffer(context, &radiance);
  if (ok) {
    std::printf("gpu_probe_gather probes=%zu dispatches=%u "
                "probes_per_dispatch=%u gpu_s=%.3f longest_dispatch_ms=%.1f\n",
                count, dispatches, probes_per_dispatch, *out_gpu_seconds,
                longest_dispatch * 1000.0);
  }
  return ok;
}

namespace {

/* Creates the BC kernels' set layout, pipeline layout, sets and pipelines
   on the first encode or decode. Once creation has started its outcome
   stands: a failed creation is released with the context. */
bool prepare_bc(VkrBakeGpuContext *context) {
  if (context->bc_set_layout != VK_NULL_HANDLE) {
    return context->bc_pipelines[kBcPipelineCount - 1u] != VK_NULL_HANDLE;
  }
  if (!context->bc_supported) {
    std::fprintf(stderr,
                 "The Vulkan device cannot run the BC encoders: they need "
                 "Vulkan 1.3 integer dot products, 16-bit shader floats and "
                 "integers, and BC textures\n");
    return false;
  }
  const VulkanApi &api = context->vulkan.api;
  VkDescriptorSetLayoutBinding bindings[kBcBindingCount] = {};
  for (uint32_t b = 0u; b < kBcBindingCount; ++b) {
    bindings[b].binding = b;
    bindings[b].descriptorCount = 1u;
    bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
  bindings[kBcBindingConstants].descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  bindings[kBcBindingSource].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  const VkDescriptorSetLayoutCreateInfo set_create = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = kBcBindingCount,
      .pBindings = bindings,
  };
  if (!vk_ok(api.vkCreateDescriptorSetLayout(context->device, &set_create,
                                             nullptr, &context->bc_set_layout),
             "BC descriptor set layout creation")) {
    context->bc_set_layout = VK_NULL_HANDLE;
    return false;
  }

  /* Only the decode kernel takes push constants. */
  const VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0u,
      .size = sizeof(BcDecodeArgs),
  };
  const VkPipelineLayoutCreateInfo layout_create = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1u,
      .pSetLayouts = &context->bc_set_layout,
      .pushConstantRangeCount = 1u,
      .pPushConstantRanges = &push_range,
  };
  if (!vk_ok(api.vkCreatePipelineLayout(context->device, &layout_create,
                                        nullptr, &context->bc_pipeline_layout),
             "BC pipeline layout creation")) {
    context->bc_pipeline_layout = VK_NULL_HANDLE;
    return false;
  }

  const VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kBcSetCount},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kBcSetCount},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2u * kBcSetCount},
  };
  const VkDescriptorPoolCreateInfo pool_create = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = kBcSetCount,
      .poolSizeCount = (uint32_t)(sizeof(pool_sizes) / sizeof(pool_sizes[0])),
      .pPoolSizes = pool_sizes,
  };
  if (!vk_ok(api.vkCreateDescriptorPool(context->device, &pool_create, nullptr,
                                        &context->bc_descriptor_pool),
             "BC descriptor pool creation")) {
    context->bc_descriptor_pool = VK_NULL_HANDLE;
    return false;
  }
  VkDescriptorSetLayout set_layouts[kBcSetCount];
  for (VkDescriptorSetLayout &layout : set_layouts) {
    layout = context->bc_set_layout;
  }
  const VkDescriptorSetAllocateInfo set_allocate = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = context->bc_descriptor_pool,
      .descriptorSetCount = kBcSetCount,
      .pSetLayouts = set_layouts,
  };
  if (!vk_ok(api.vkAllocateDescriptorSets(context->device, &set_allocate,
                                          context->bc_sets),
             "BC descriptor set allocation")) {
    return false;
  }

  struct Kernel {
    const uint8_t *spirv;
    size_t size;
    const char *entry;
  };
  const Kernel kernels[kBcPipelineCount] = {
      {k_bc6h_try_mode_g10_spirv, sizeof(k_bc6h_try_mode_g10_spirv),
       "TryModeG10CS"},
      {k_bc6h_try_mode_le10_spirv, sizeof(k_bc6h_try_mode_le10_spirv),
       "TryModeLE10CS"},
      {k_bc6h_encode_block_spirv, sizeof(k_bc6h_encode_block_spirv),
       "EncodeBlockCS"},
      {k_bc7_try_mode_456_spirv, sizeof(k_bc7_try_mode_456_spirv),
       "TryMode456CS"},
      {k_bc7_try_mode_137_spirv, sizeof(k_bc7_try_mode_137_spirv),
       "TryMode137CS"},
      {k_bc7_encode_block_spirv, sizeof(k_bc7_encode_block_spirv),
       "EncodeBlockCS"},
      {k_bc_decode_spirv, sizeof(k_bc_decode_spirv), "bc_decode"},
  };
  /* A driver without these pipelines in its cache compiles them for
     about a minute (BC6H's kernels on the RX 6700 XT), so the time is
     reported apart from the encodes. */
  const auto start = std::chrono::steady_clock::now();
  for (uint32_t p = 0u; p < kBcPipelineCount; ++p) {
    if (!create_pipeline(context, context->bc_pipeline_layout, kernels[p].spirv,
                         kernels[p].size, kernels[p].entry,
                         &context->bc_pipelines[p])) {
      return false;
    }
  }
  const std::chrono::duration<double> elapsed =
      std::chrono::steady_clock::now() - start;
  std::printf("gpu_bc_pipelines_s=%.2f\n", elapsed.count());
  return true;
}

/* Points one BC set at a call's resources. The decode set has no input
   buffer. */
void write_bc_set(VkrBakeGpuContext *context, BcSet set,
                  const Buffer &constants, VkImageView source,
                  const Buffer *input, const Buffer &output) {
  const VkDescriptorBufferInfo constants_info = {constants.buffer, 0u,
                                                 sizeof(BcConstants)};
  const VkDescriptorImageInfo source_info = {
      VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  const VkDescriptorBufferInfo input_info = {
      input ? input->buffer : VK_NULL_HANDLE, 0u, VK_WHOLE_SIZE};
  const VkDescriptorBufferInfo output_info = {output.buffer, 0u, VK_WHOLE_SIZE};
  const VkDescriptorSet target = context->bc_sets[set];
  const VkWriteDescriptorSet writes[] = {
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = target,
          .dstBinding = kBcBindingConstants,
          .descriptorCount = 1u,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo = &constants_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = target,
          .dstBinding = kBcBindingSource,
          .descriptorCount = 1u,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .pImageInfo = &source_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = target,
          .dstBinding = kBcBindingOutput,
          .descriptorCount = 1u,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &output_info,
      },
      {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = target,
          .dstBinding = kBcBindingInput,
          .descriptorCount = 1u,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &input_info,
      },
  };
  const uint32_t count = input ? 4u : 3u;
  context->vulkan.api.vkUpdateDescriptorSets(context->device, count, writes, 0u,
                                             nullptr);
}

/* A shader-to-shader dependency between consecutive dispatches. */
void bc_barrier(VkrBakeGpuContext *context, VkPipelineStageFlags destination,
                VkAccessFlags access) {
  const VkMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = access,
  };
  context->vulkan.api.vkCmdPipelineBarrier(
      context->command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      destination, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
}

/* One kernel dispatch of an encoder: its mode, the error buffer it reads
   and writes, and how many blocks one 64-thread group encodes. */
struct BcPass {
  BcPipeline pipeline;
  uint32_t mode;
  BcSet set;
  uint32_t blocks_per_group;
};

/* BCDirectCompute.cpp's BC6H sequence: modes 11-14 into err1, modes 1-10
   alternating between the error buffers, ending in err1, then the blocks
   from err1. */
constexpr BcPass kBc6hPasses[] = {
    {kBc6hTryModeG10, 0u, kBcErr2ToErr1, 4u},
    {kBc6hTryModeLE10, 0u, kBcErr1ToErr2, 2u},
    {kBc6hTryModeLE10, 1u, kBcErr2ToErr1, 2u},
    {kBc6hTryModeLE10, 2u, kBcErr1ToErr2, 2u},
    {kBc6hTryModeLE10, 3u, kBcErr2ToErr1, 2u},
    {kBc6hTryModeLE10, 4u, kBcErr1ToErr2, 2u},
    {kBc6hTryModeLE10, 5u, kBcErr2ToErr1, 2u},
    {kBc6hTryModeLE10, 6u, kBcErr1ToErr2, 2u},
    {kBc6hTryModeLE10, 7u, kBcErr2ToErr1, 2u},
    {kBc6hTryModeLE10, 8u, kBcErr1ToErr2, 2u},
    {kBc6hTryModeLE10, 9u, kBcErr2ToErr1, 2u},
    {kBc6hEncodeBlock, 0u, kBcErr1ToBlocks, 2u},
};
static_assert(sizeof(kBc6hPasses) / sizeof(kBc6hPasses[0]) <= kBcMaxPasses,
              "kBcMaxPasses holds the BC6H sequence");

/* Its default BC7 sequence: modes 4-6 into err1, then modes 1, 3 and 7
   alternating, ending in err2, then the blocks from err2. Modes 0 and 2
   (TEX_COMPRESS_BC7_USE_3SUBSETS) are left out, as DirectXTex does unless
   asked. */
constexpr BcPass kBc7Passes[] = {
    {kBc7TryMode456, 0u, kBcErr2ToErr1, 4u},
    {kBc7TryMode137, 1u, kBcErr1ToErr2, 1u},
    {kBc7TryMode137, 3u, kBcErr2ToErr1, 1u},
    {kBc7TryMode137, 7u, kBcErr1ToErr2, 1u},
    {kBc7EncodeBlock, 0u, kBcErr2ToBlocks, 4u},
};

/* Whether a page's dimensions are encodable: nonzero multiples of 4 whose
   texel bytes fit the host's sizes. */
bool bc_page_valid(uint32_t width, uint32_t height) {
  return width > 0u && height > 0u && width % 4u == 0u && height % 4u == 0u &&
         (uint64_t)width * height <= (uint64_t)UINT32_MAX / 16u;
}

/* The kernels have no tail guard (DirectXTex's REF_DEVICE build): a group
   encodes up to 4 blocks past the last one. The error and block buffers
   hold that many more blocks, and the source image enough more rows of
   zero texels, so those accesses stay in bounds. */
uint32_t bc_padded_blocks(uint32_t width, uint32_t height) {
  const uint32_t blocks = (width / 4u) * (height / 4u);
  return (blocks + 3u) / 4u * 4u;
}

uint32_t bc_source_height(uint32_t width, uint32_t height) {
  const uint32_t blocks_x = width / 4u;
  return (bc_padded_blocks(width, height) + blocks_x - 1u) / blocks_x * 4u;
}

/* Encodes a page from its padded source texels with `passes`, one
   submission per kBcBlocksPerSubmission blocks. */
bool encode_bc(VkrBakeGpuContext *context, VkFormat source_format,
               const void *source, VkDeviceSize source_size, uint32_t width,
               uint32_t height, uint32_t dxgi_format, const BcPass *passes,
               uint32_t pass_count, std::vector<uint8_t> *out_blocks,
               double *out_gpu_seconds) {
  const VulkanApi &api = context->vulkan.api;
  const uint32_t blocks_x = width / 4u;
  const uint32_t total = blocks_x * (height / 4u);
  const VkDeviceSize block_bytes =
      (VkDeviceSize)bc_padded_blocks(width, height) * 16u;
  const VkDeviceSize slot =
      (sizeof(BcConstants) + context->uniform_alignment - 1u) /
      context->uniform_alignment * context->uniform_alignment;
  const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  Image image;
  Buffer err1;
  Buffer err2;
  Buffer blocks;
  Buffer constants;
  bool ok = upload_image(
                context, source_format, width, bc_source_height(width, height),
                1u, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                source, source_size, &image) &&
            create_buffer(context, block_bytes, storage, false, &err1) &&
            create_buffer(context, block_bytes, storage, false, &err2) &&
            create_buffer(context, block_bytes, storage, true, &blocks) &&
            create_buffer(context, slot * kBcMaxPasses,
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, &constants);
  if (ok) {
    write_bc_set(context, kBcErr1ToErr2, constants, image.view, &err1, err2);
    write_bc_set(context, kBcErr2ToErr1, constants, image.view, &err2, err1);
    write_bc_set(context, kBcErr1ToBlocks, constants, image.view, &err1,
                 blocks);
    write_bc_set(context, kBcErr2ToBlocks, constants, image.view, &err2,
                 blocks);
  }

  for (uint32_t first = 0u; ok && first < total;
       first += kBcBlocksPerSubmission) {
    const uint32_t count = std::min(kBcBlocksPerSubmission, total - first);
    /* Every submission is waited for, so the slots are free to rewrite. */
    for (uint32_t p = 0u; p < pass_count; ++p) {
      const BcConstants values = {
          .tex_width = width,
          .num_block_x = blocks_x,
          .format = dxgi_format,
          .mode_id = passes[p].mode,
          .start_block_id = first,
          .num_total_blocks = total,
          .alpha_weight = 1.0f,
          .reserved = 0u,
      };
      std::memcpy(static_cast<uint8_t *>(constants.mapped) + p * slot, &values,
                  sizeof(values));
    }
    ok = begin_commands(context);
    if (!ok) {
      break;
    }
    const VkCommandBuffer command = context->command_buffer;
    /* The previous submission's writes to the error buffers. */
    bc_barrier(context, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    begin_timing(context);
    for (uint32_t p = 0u; p < pass_count; ++p) {
      const BcPass &pass = passes[p];
      const uint32_t offset = (uint32_t)(p * slot);
      api.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            context->bc_pipelines[pass.pipeline]);
      api.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  context->bc_pipeline_layout, 0u, 1u,
                                  &context->bc_sets[pass.set], 1u, &offset);
      api.vkCmdDispatch(
          command, (count + pass.blocks_per_group - 1u) / pass.blocks_per_group,
          1u, 1u);
      bc_barrier(context, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    end_timing(context);
    bc_barrier(context, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    ok = finish_commands(context, out_gpu_seconds);
  }

  if (ok) {
    const uint8_t *encoded = static_cast<const uint8_t *>(blocks.mapped);
    out_blocks->assign(encoded, encoded + (size_t)total * 16u);
  }
  destroy_buffer(context, &constants);
  destroy_buffer(context, &blocks);
  destroy_buffer(context, &err2);
  destroy_buffer(context, &err1);
  destroy_image(context, &image);
  return ok;
}

} // namespace

bool vkr_bake_gpu_bc_available(const VkrBakeGpuContext *context) {
  return context && context->bc_supported;
}

bool vkr_bake_gpu_encode_bc6h(VkrBakeGpuContext *context, const float32_t *rgba,
                              uint32_t width, uint32_t height,
                              std::vector<uint8_t> *out_blocks,
                              double *out_gpu_seconds) {
  *out_gpu_seconds = 0.0;
  if (!context || !rgba || !out_blocks || !bc_page_valid(width, height) ||
      !prepare_bc(context)) {
    return false;
  }
  /* The kernel reads RGBA32F; alpha is unused. Clamping here keeps the
     kernel's half conversion finite and NaN out of its search. */
  const size_t texels = (size_t)width * height;
  std::vector<float32_t> source(
      (size_t)width * bc_source_height(width, height) * 4u, 0.0f);
  for (size_t i = 0u; i < texels; ++i) {
    for (uint32_t c = 0u; c < 3u; ++c) {
      const float32_t value = rgba[i * 4u + c];
      source[i * 4u + c] = value > 0.0f ? std::fmin(value, 65504.0f) : 0.0f;
    }
    source[i * 4u + 3u] = 1.0f;
  }
  return encode_bc(context, VK_FORMAT_R32G32B32A32_SFLOAT, source.data(),
                   source.size() * sizeof(float32_t), width, height,
                   kDxgiBc6hUf16, kBc6hPasses,
                   (uint32_t)(sizeof(kBc6hPasses) / sizeof(kBc6hPasses[0])),
                   out_blocks, out_gpu_seconds);
}

bool vkr_bake_gpu_encode_bc7(VkrBakeGpuContext *context, const uint8_t *rgba8,
                             uint32_t width, uint32_t height,
                             std::vector<uint8_t> *out_blocks,
                             double *out_gpu_seconds) {
  *out_gpu_seconds = 0.0;
  if (!context || !rgba8 || !out_blocks || !bc_page_valid(width, height) ||
      !prepare_bc(context)) {
    return false;
  }
  std::vector<uint8_t> source(
      (size_t)width * bc_source_height(width, height) * 4u, 0u);
  std::memcpy(source.data(), rgba8, (size_t)width * height * 4u);
  return encode_bc(context, VK_FORMAT_R8G8B8A8_UNORM, source.data(),
                   source.size(), width, height, kDxgiBc7Unorm, kBc7Passes,
                   (uint32_t)(sizeof(kBc7Passes) / sizeof(kBc7Passes[0])),
                   out_blocks, out_gpu_seconds);
}

bool vkr_bake_gpu_decode_bc(VkrBakeGpuContext *context,
                            VkrBakeGpuBcFormat format, const uint8_t *blocks,
                            uint32_t width, uint32_t height,
                            std::vector<float32_t> *out_rgba) {
  if (!context || !blocks || !out_rgba || !bc_page_valid(width, height) ||
      !prepare_bc(context)) {
    return false;
  }
  const VulkanApi &api = context->vulkan.api;
  const VkFormat image_format = format == VkrBakeGpuBcFormat::Bc6hUfloat
                                    ? VK_FORMAT_BC6H_UFLOAT_BLOCK
                                    : VK_FORMAT_BC7_UNORM_BLOCK;
  const size_t texels = (size_t)width * height;
  Image image;
  Buffer decoded;
  /* The decode kernel reads no constants, but its set binds them like the
     encoders' sets. */
  Buffer constants;
  bool ok =
      upload_image(context, image_format, width, height, 1u,
                   VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                   blocks, (VkDeviceSize)texels, &image) &&
      create_buffer(context, (VkDeviceSize)texels * 16u,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, &decoded) &&
      create_buffer(context, sizeof(BcConstants),
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, &constants) &&
      begin_commands(context);
  if (ok) {
    write_bc_set(context, kBcDecodeSet, constants, image.view, nullptr,
                 decoded);
    const VkCommandBuffer command = context->command_buffer;
    const BcDecodeArgs args = {width, height};
    const uint32_t offset = 0u;
    api.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                          context->bc_pipelines[kBcDecode]);
    api.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                context->bc_pipeline_layout, 0u, 1u,
                                &context->bc_sets[kBcDecodeSet], 1u, &offset);
    api.vkCmdPushConstants(command, context->bc_pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(args),
                           &args);
    api.vkCmdDispatch(command, (width + 7u) / 8u, (height + 7u) / 8u, 1u);
    bc_barrier(context, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    ok = finish_commands(context, nullptr);
  }
  if (ok) {
    const float32_t *values = static_cast<const float32_t *>(decoded.mapped);
    out_rgba->assign(values, values + texels * 4u);
  }
  destroy_buffer(context, &constants);
  destroy_buffer(context, &decoded);
  destroy_image(context, &image);
  return ok;
}
