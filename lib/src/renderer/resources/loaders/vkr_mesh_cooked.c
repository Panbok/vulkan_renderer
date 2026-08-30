#include "renderer/resources/loaders/vkr_mesh_cooked.h"

#include "core/logger.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include "renderer/resources/loaders/vkr_meshoptimizer_bridge.h"

#include <math.h>

#define VKR_MESH_COOKED_HEADER_SIZE 272u
#define VKR_MESH_COOKED_DEPENDENCY_SIZE 64u
#define VKR_MESH_COOKED_RANGE_SIZE 200u
#define VKR_MESH_COOKED_MAX_RANGES 1048576u
#define VKR_MESH_COOKED_MAX_DEPENDENCIES 65536u
#define VKR_MESH_COOKED_MAX_STRING_LENGTH 65535u
#define VKR_MESH_COOKED_MAX_FILE_SIZE GB(8)

#define VKR_MESH_COOKED_HEADER_CRC_OFFSET 184u
#define VKR_MESH_COOKED_METADATA_CRC_OFFSET 188u

typedef struct VkrMeshCookedSha256 {
  uint32_t state[8];
  uint64_t bit_count;
  uint8_t block[64];
  uint32_t block_size;
} VkrMeshCookedSha256;

typedef struct VkrMeshCookedWriter {
  uint8_t *data;
  uint64_t size;
  uint64_t offset;
} VkrMeshCookedWriter;

typedef struct VkrMeshCookedReader {
  const uint8_t *data;
  uint64_t size;
  uint64_t offset;
} VkrMeshCookedReader;

typedef struct VkrMeshCookedDependencyBuild {
  String8 path;
  uint64_t byte_size;
  uint8_t hash[32];
  uint64_t string_offset;
} VkrMeshCookedDependencyBuild;

typedef struct VkrMeshCookedRangeBuild {
  const VkrMeshLoaderSubmeshRange *source;
  uint8_t *encoded_vertices;
  uint64_t encoded_vertex_size;
  uint8_t *encoded_indices;
  uint64_t encoded_index_size;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t first_index;
  uint64_t material_offset;
  uint64_t shader_offset;
  uint64_t vertex_stream_offset;
  uint64_t index_stream_offset;
  uint32_t vertex_crc;
  uint32_t index_crc;
  VkrGeometryQuantizationMetrics quantization;
  VkrGpuGeometryDecodeRecord decode;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
} VkrMeshCookedRangeBuild;

typedef struct VkrMeshCookedDependencyView {
  String8 path;
  uint64_t byte_size;
  uint8_t hash[32];
} VkrMeshCookedDependencyView;

typedef struct VkrMeshCookedRangeView {
  uint32_t range_id;
  uint32_t first_index;
  uint32_t index_count;
  uint32_t vertex_count;
  int32_t vertex_offset;
  VkrPipelineDomain pipeline_domain;
  String8 material_name;
  String8 shader_override;
  uint64_t vertex_stream_offset;
  uint64_t vertex_encoded_size;
  uint64_t vertex_decoded_size;
  uint32_t vertex_crc;
  uint32_t index_crc;
  uint64_t index_stream_offset;
  uint64_t index_encoded_size;
  uint64_t index_decoded_size;
  Vec3 center;
  Vec3 min_extents;
  Vec3 max_extents;
  VkrGeometryQuantizationMetrics quantization;
  VkrGpuGeometryDecodeRecord decode;
} VkrMeshCookedRangeView;

static uint32_t vkr_mesh_cooked_rotr32(uint32_t value, uint32_t amount) {
  return (value >> amount) | (value << (32u - amount));
}

static void vkr_mesh_cooked_sha256_transform(VkrMeshCookedSha256 *ctx,
                                             const uint8_t block[64]) {
  static const uint32_t constants[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
  };
  uint32_t words[64];
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t offset = i * 4u;
    words[i] = ((uint32_t)block[offset] << 24u) |
               ((uint32_t)block[offset + 1u] << 16u) |
               ((uint32_t)block[offset + 2u] << 8u) |
               (uint32_t)block[offset + 3u];
  }
  for (uint32_t i = 16; i < 64; ++i) {
    const uint32_t s0 = vkr_mesh_cooked_rotr32(words[i - 15u], 7u) ^
                        vkr_mesh_cooked_rotr32(words[i - 15u], 18u) ^
                        (words[i - 15u] >> 3u);
    const uint32_t s1 = vkr_mesh_cooked_rotr32(words[i - 2u], 17u) ^
                        vkr_mesh_cooked_rotr32(words[i - 2u], 19u) ^
                        (words[i - 2u] >> 10u);
    words[i] = words[i - 16u] + s0 + words[i - 7u] + s1;
  }

  uint32_t a = ctx->state[0];
  uint32_t b = ctx->state[1];
  uint32_t c = ctx->state[2];
  uint32_t d = ctx->state[3];
  uint32_t e = ctx->state[4];
  uint32_t f = ctx->state[5];
  uint32_t g = ctx->state[6];
  uint32_t h = ctx->state[7];
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t sum1 = vkr_mesh_cooked_rotr32(e, 6u) ^
                          vkr_mesh_cooked_rotr32(e, 11u) ^
                          vkr_mesh_cooked_rotr32(e, 25u);
    const uint32_t choice = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + sum1 + choice + constants[i] + words[i];
    const uint32_t sum0 = vkr_mesh_cooked_rotr32(a, 2u) ^
                          vkr_mesh_cooked_rotr32(a, 13u) ^
                          vkr_mesh_cooked_rotr32(a, 22u);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

static void vkr_mesh_cooked_sha256_init(VkrMeshCookedSha256 *ctx) {
  *ctx = (VkrMeshCookedSha256){
      .state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu,
                0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
  };
}

static void vkr_mesh_cooked_sha256_update(VkrMeshCookedSha256 *ctx,
                                          const void *data, uint64_t size) {
  const uint8_t *bytes = (const uint8_t *)data;
  ctx->bit_count += size * 8u;
  while (size > 0) {
    uint32_t available = 64u - ctx->block_size;
    uint32_t copy_size = size < available ? (uint32_t)size : available;
    MemCopy(ctx->block + ctx->block_size, bytes, copy_size);
    ctx->block_size += copy_size;
    bytes += copy_size;
    size -= copy_size;
    if (ctx->block_size == 64u) {
      vkr_mesh_cooked_sha256_transform(ctx, ctx->block);
      ctx->block_size = 0;
    }
  }
}

static void vkr_mesh_cooked_sha256_final(VkrMeshCookedSha256 *ctx,
                                         uint8_t out_hash[32]) {
  ctx->block[ctx->block_size++] = 0x80u;
  if (ctx->block_size > 56u) {
    MemZero(ctx->block + ctx->block_size, 64u - ctx->block_size);
    vkr_mesh_cooked_sha256_transform(ctx, ctx->block);
    ctx->block_size = 0;
  }
  MemZero(ctx->block + ctx->block_size, 56u - ctx->block_size);
  for (uint32_t i = 0; i < 8u; ++i) {
    ctx->block[63u - i] = (uint8_t)(ctx->bit_count >> (i * 8u));
  }
  vkr_mesh_cooked_sha256_transform(ctx, ctx->block);
  for (uint32_t i = 0; i < 8u; ++i) {
    const uint64_t offset = (uint64_t)i * 4u;
    out_hash[offset] = (uint8_t)(ctx->state[i] >> 24u);
    out_hash[offset + 1u] = (uint8_t)(ctx->state[i] >> 16u);
    out_hash[offset + 2u] = (uint8_t)(ctx->state[i] >> 8u);
    out_hash[offset + 3u] = (uint8_t)ctx->state[i];
  }
}

static uint32_t vkr_mesh_cooked_crc32(const uint8_t *data, uint64_t size) {
  uint32_t crc = 0xffffffffu;
  for (uint64_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint32_t bit = 0; bit < 8u; ++bit) {
      crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
  }
  return ~crc;
}

static uint32_t vkr_mesh_cooked_to_le32(uint32_t value) {
  const uint16_t endian = 1u;
  if (*(const uint8_t *)&endian == 1u) {
    return value;
  }
  return ((value & 0xff000000u) >> 24u) | ((value & 0x00ff0000u) >> 8u) |
         ((value & 0x0000ff00u) << 8u) | ((value & 0x000000ffu) << 24u);
}

static uint64_t vkr_mesh_cooked_to_le64(uint64_t value) {
  const uint16_t endian = 1u;
  if (*(const uint8_t *)&endian == 1u) {
    return value;
  }
  return ((value & 0xff00000000000000ull) >> 56u) |
         ((value & 0x00ff000000000000ull) >> 40u) |
         ((value & 0x0000ff0000000000ull) >> 24u) |
         ((value & 0x000000ff00000000ull) >> 8u) |
         ((value & 0x00000000ff000000ull) << 8u) |
         ((value & 0x0000000000ff0000ull) << 24u) |
         ((value & 0x000000000000ff00ull) << 40u) |
         ((value & 0x00000000000000ffull) << 56u);
}

static uint64_t vkr_mesh_cooked_align(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static bool8_t vkr_mesh_cooked_add_u64(uint64_t lhs, uint64_t rhs,
                                       uint64_t *out) {
  if (UINT64_MAX - lhs < rhs) {
    return false_v;
  }
  *out = lhs + rhs;
  return true_v;
}

static bool8_t vkr_mesh_cooked_mul_u64(uint64_t lhs, uint64_t rhs,
                                       uint64_t *out) {
  if (lhs != 0 && rhs > UINT64_MAX / lhs) {
    return false_v;
  }
  *out = lhs * rhs;
  return true_v;
}

static bool8_t vkr_mesh_cooked_writer_bytes(VkrMeshCookedWriter *writer,
                                            const void *data, uint64_t size) {
  if (writer->offset > writer->size || size > writer->size - writer->offset) {
    return false_v;
  }
  if (size > 0) {
    MemCopy(writer->data + writer->offset, data, size);
  }
  writer->offset += size;
  return true_v;
}

static bool8_t vkr_mesh_cooked_writer_u32(VkrMeshCookedWriter *writer,
                                          uint32_t value) {
  value = vkr_mesh_cooked_to_le32(value);
  return vkr_mesh_cooked_writer_bytes(writer, &value, sizeof(value));
}

static bool8_t vkr_mesh_cooked_writer_i32(VkrMeshCookedWriter *writer,
                                          int32_t value) {
  return vkr_mesh_cooked_writer_u32(writer, (uint32_t)value);
}

static bool8_t vkr_mesh_cooked_writer_u64(VkrMeshCookedWriter *writer,
                                          uint64_t value) {
  value = vkr_mesh_cooked_to_le64(value);
  return vkr_mesh_cooked_writer_bytes(writer, &value, sizeof(value));
}

static bool8_t vkr_mesh_cooked_writer_f32(VkrMeshCookedWriter *writer,
                                          float32_t value) {
  union {
    float32_t f32;
    uint32_t u32;
  } bits = {.f32 = value};
  return vkr_mesh_cooked_writer_u32(writer, bits.u32);
}

static bool8_t vkr_mesh_cooked_reader_bytes(VkrMeshCookedReader *reader,
                                            void *out, uint64_t size) {
  if (reader->offset > reader->size || size > reader->size - reader->offset) {
    return false_v;
  }
  if (out && size > 0) {
    MemCopy(out, reader->data + reader->offset, size);
  }
  reader->offset += size;
  return true_v;
}

static bool8_t vkr_mesh_cooked_reader_u32(VkrMeshCookedReader *reader,
                                          uint32_t *out) {
  uint32_t value = 0;
  if (!vkr_mesh_cooked_reader_bytes(reader, &value, sizeof(value))) {
    return false_v;
  }
  *out = vkr_mesh_cooked_to_le32(value);
  return true_v;
}

static bool8_t vkr_mesh_cooked_reader_i32(VkrMeshCookedReader *reader,
                                          int32_t *out) {
  uint32_t value = 0;
  if (!vkr_mesh_cooked_reader_u32(reader, &value)) {
    return false_v;
  }
  *out = (int32_t)value;
  return true_v;
}

static bool8_t vkr_mesh_cooked_reader_u64(VkrMeshCookedReader *reader,
                                          uint64_t *out) {
  uint64_t value = 0;
  if (!vkr_mesh_cooked_reader_bytes(reader, &value, sizeof(value))) {
    return false_v;
  }
  *out = vkr_mesh_cooked_to_le64(value);
  return true_v;
}

static bool8_t vkr_mesh_cooked_reader_f32(VkrMeshCookedReader *reader,
                                          float32_t *out) {
  uint32_t bits = 0;
  if (!vkr_mesh_cooked_reader_u32(reader, &bits)) {
    return false_v;
  }
  union {
    uint32_t u32;
    float32_t f32;
  } value = {.u32 = bits};
  *out = value.f32;
  return true_v;
}

static bool8_t vkr_mesh_cooked_path_is_absolute(String8 path) {
  return (path.length > 0 && (path.str[0] == '/' || path.str[0] == '\\')) ||
         (path.length > 1 && path.str[1] == ':');
}

static bool8_t vkr_mesh_cooked_read_file(VkrAllocator *allocator, String8 path,
                                         uint8_t **out_data,
                                         uint64_t *out_size) {
  String8 owned_path = string8_duplicate(allocator, &path);
  if (!owned_path.str) {
    return false_v;
  }
  FilePath file_path = file_path_create(string8_cstr(&owned_path), allocator,
                                        vkr_mesh_cooked_path_is_absolute(path)
                                            ? FILE_PATH_TYPE_ABSOLUTE
                                            : FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&file_path, mode, &file) != FILE_ERROR_NONE) {
    return false_v;
  }
  FileError error = file_read_all(&file, allocator, out_data, out_size);
  file_close(&file);
  return error == FILE_ERROR_NONE;
}

static uint32_t vkr_mesh_cooked_float_bits(float32_t value) {
  union {
    float32_t f32;
    uint32_t u32;
  } bits = {.f32 = value};
  return bits.u32;
}

static void
vkr_mesh_cooked_hash_settings(const VkrGeometryQuantizationBudgets *budgets,
                              uint8_t out_hash[32]) {
  uint32_t settings[] = {
      vkr_mesh_cooked_to_le32(VKR_MESH_COOKED_VERSION),
      vkr_mesh_cooked_to_le32(VKR_MESH_COOKED_LAYOUT_STATIC_PACKED_V1),
      vkr_mesh_cooked_to_le32((uint32_t)sizeof(VkrPackedStaticVertex)),
      vkr_mesh_cooked_to_le32((uint32_t)sizeof(uint32_t)),
      vkr_mesh_cooked_to_le32(VKR_MESHOPT_VERTEX_CODEC_VERSION),
      vkr_mesh_cooked_to_le32(VKR_MESHOPT_INDEX_CODEC_VERSION),
      vkr_mesh_cooked_to_le32(vkr_meshopt_library_version()),
      vkr_mesh_cooked_to_le32(2u), /* vertex codec level */
      vkr_mesh_cooked_to_le32(0u), /* overdraw disabled */
      vkr_mesh_cooked_to_le32(
          vkr_mesh_cooked_float_bits(budgets->position_relative)),
      vkr_mesh_cooked_to_le32(
          vkr_mesh_cooked_float_bits(budgets->normal_degrees)),
      vkr_mesh_cooked_to_le32(
          vkr_mesh_cooked_float_bits(budgets->tangent_degrees)),
      vkr_mesh_cooked_to_le32(vkr_mesh_cooked_float_bits(budgets->uv_absolute)),
      vkr_mesh_cooked_to_le32(
          vkr_mesh_cooked_float_bits(budgets->color_absolute)),
  };
  VkrMeshCookedSha256 sha;
  vkr_mesh_cooked_sha256_init(&sha);
  vkr_mesh_cooked_sha256_update(&sha, settings, sizeof(settings));
  vkr_mesh_cooked_sha256_final(&sha, out_hash);
}

static bool8_t vkr_mesh_cooked_hash_dependencies(
    VkrAllocator *scratch_allocator, VkrMeshCookedDependencyBuild *dependencies,
    uint32_t dependency_count, uint8_t out_hash[32]) {
  VkrMeshCookedSha256 aggregate;
  vkr_mesh_cooked_sha256_init(&aggregate);
  for (uint32_t i = 0; i < dependency_count; ++i) {
    VkrAllocatorScope scope = vkr_allocator_begin_scope(scratch_allocator);
    if (!vkr_allocator_scope_is_valid(&scope)) {
      return false_v;
    }
    uint8_t *bytes = NULL;
    uint64_t size = 0;
    bool8_t ok = vkr_mesh_cooked_read_file(scratch_allocator,
                                           dependencies[i].path, &bytes, &size);
    if (ok) {
      VkrMeshCookedSha256 file_sha;
      vkr_mesh_cooked_sha256_init(&file_sha);
      vkr_mesh_cooked_sha256_update(&file_sha, bytes, size);
      vkr_mesh_cooked_sha256_final(&file_sha, dependencies[i].hash);
      dependencies[i].byte_size = size;

      uint64_t path_length_le =
          vkr_mesh_cooked_to_le64(dependencies[i].path.length);
      uint64_t byte_size_le = vkr_mesh_cooked_to_le64(size);
      vkr_mesh_cooked_sha256_update(&aggregate, &path_length_le,
                                    sizeof(path_length_le));
      vkr_mesh_cooked_sha256_update(&aggregate, dependencies[i].path.str,
                                    dependencies[i].path.length);
      vkr_mesh_cooked_sha256_update(&aggregate, &byte_size_le,
                                    sizeof(byte_size_le));
      vkr_mesh_cooked_sha256_update(&aggregate, bytes, size);
    }
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    if (!ok) {
      log_error("MeshCooked: failed to read dependency '%.*s'",
                (int32_t)dependencies[i].path.length, dependencies[i].path.str);
      return false_v;
    }
  }
  vkr_mesh_cooked_sha256_final(&aggregate, out_hash);
  return true_v;
}

static bool8_t vkr_mesh_cooked_string_view(const uint8_t *data,
                                           uint64_t string_offset,
                                           uint64_t string_size,
                                           uint64_t relative_offset,
                                           uint32_t length, bool8_t allow_empty,
                                           String8 *out) {
  if ((!allow_empty && length == 0) ||
      length > VKR_MESH_COOKED_MAX_STRING_LENGTH ||
      relative_offset > string_size || length > string_size - relative_offset) {
    return false_v;
  }
  String8 view = {
      .str = (uint8_t *)data + string_offset + relative_offset,
      .length = length,
  };
  for (uint32_t i = 0; i < length; ++i) {
    if (view.str[i] == '\0') {
      return false_v;
    }
  }
  *out = view;
  return true_v;
}

static bool8_t vkr_mesh_cooked_string_is_valid(String8 value,
                                               bool8_t allow_empty) {
  if (value.length == 0) {
    return allow_empty;
  }
  if (!value.str || value.length > VKR_MESH_COOKED_MAX_STRING_LENGTH) {
    return false_v;
  }
  for (uint64_t i = 0; i < value.length; ++i) {
    if (value.str[i] == '\0') {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t vkr_mesh_cooked_vertex_is_finite(const VkrVertex3d *vertex) {
  return isfinite(vertex->position.x) && isfinite(vertex->position.y) &&
         isfinite(vertex->position.z) && isfinite(vertex->normal.x) &&
         isfinite(vertex->normal.y) && isfinite(vertex->normal.z) &&
         isfinite(vertex->texcoord.x) && isfinite(vertex->texcoord.y) &&
         isfinite(vertex->colour.x) && isfinite(vertex->colour.y) &&
         isfinite(vertex->colour.z) && isfinite(vertex->colour.w) &&
         isfinite(vertex->tangent.x) && isfinite(vertex->tangent.y) &&
         isfinite(vertex->tangent.z) && isfinite(vertex->tangent.w);
}

static bool8_t
vkr_mesh_cooked_decode_equal(const VkrGpuGeometryDecodeRecord *a,
                             const VkrGpuGeometryDecodeRecord *b) {
  return a->position_bias[0] == b->position_bias[0] &&
         a->position_bias[1] == b->position_bias[1] &&
         a->position_bias[2] == b->position_bias[2] && a->flags == b->flags &&
         a->position_scale[0] == b->position_scale[0] &&
         a->position_scale[1] == b->position_scale[1] &&
         a->position_scale[2] == b->position_scale[2] &&
         a->reserved == b->reserved;
}

static bool8_t
vkr_mesh_cooked_quantization_equal(const VkrGeometryQuantizationMetrics *a,
                                   const VkrGeometryQuantizationMetrics *b) {
  return a->position_max == b->position_max &&
         a->normal_degrees_max == b->normal_degrees_max &&
         a->tangent_degrees_max == b->tangent_degrees_max &&
         a->uv_max == b->uv_max && a->color_max == b->color_max;
}

static bool8_t vkr_mesh_cooked_compute_range_bounds(const VkrVertex3d *vertices,
                                                    uint32_t count,
                                                    Vec3 *out_center,
                                                    Vec3 *out_min,
                                                    Vec3 *out_max) {
  if (!vertices || count == 0 || !out_center || !out_min || !out_max) {
    return false_v;
  }
  Vec3 min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
  Vec3 max = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
  for (uint32_t i = 0; i < count; ++i) {
    if (!vkr_mesh_cooked_vertex_is_finite(&vertices[i])) {
      return false_v;
    }
    const Vec3 position = vkr_vertex_unpack_vec3(vertices[i].position);
    min.x = Min(min.x, position.x);
    min.y = Min(min.y, position.y);
    min.z = Min(min.z, position.z);
    max.x = Max(max.x, position.x);
    max.y = Max(max.y, position.y);
    max.z = Max(max.z, position.z);
  }
  *out_min = min;
  *out_max = max;
  *out_center = vec3_scale(vec3_add(min, max), 0.5f);
  return true_v;
}

static bool8_t vkr_mesh_cooked_validate_packed_range_vertices(
    const VkrPackedStaticVertex *vertices, uint32_t count,
    const VkrGpuGeometryDecodeRecord *decode, Vec3 expected_center,
    Vec3 expected_min, Vec3 expected_max, float32_t tolerance) {
  if (!vkr_packed_geometry_vertices_are_valid(vertices, count, decode) ||
      !isfinite(tolerance) || tolerance < 0.0f) {
    return false_v;
  }
  Vec3 min = vec3_new(VKR_FLOAT_MAX, VKR_FLOAT_MAX, VKR_FLOAT_MAX);
  Vec3 max = vec3_new(-VKR_FLOAT_MAX, -VKR_FLOAT_MAX, -VKR_FLOAT_MAX);
  for (uint32_t i = 0; i < count; ++i) {
    VkrVertex3d unpacked = {0};
    vkr_packed_geometry_unpack(&vertices[i], 1u, decode, &unpacked);
    const Vec3 position = vkr_vertex_unpack_vec3(unpacked.position);
    min.x = Min(min.x, position.x);
    min.y = Min(min.y, position.y);
    min.z = Min(min.z, position.z);
    max.x = Max(max.x, position.x);
    max.y = Max(max.y, position.y);
    max.z = Max(max.z, position.z);
  }
  const Vec3 center = vec3_scale(vec3_add(min, max), 0.5f);
  const float32_t epsilon = tolerance + 1.0e-6f;
  return fabsf(min.x - expected_min.x) <= epsilon &&
         fabsf(min.y - expected_min.y) <= epsilon &&
         fabsf(min.z - expected_min.z) <= epsilon &&
         fabsf(max.x - expected_max.x) <= epsilon &&
         fabsf(max.y - expected_max.y) <= epsilon &&
         fabsf(max.z - expected_max.z) <= epsilon &&
         fabsf(center.x - expected_center.x) <= epsilon &&
         fabsf(center.y - expected_center.y) <= epsilon &&
         fabsf(center.z - expected_center.z) <= epsilon;
}

bool8_t vkr_mesh_cooked_encode(VkrAllocator *scratch_allocator,
                               const VkrMeshCookedEncodeInfo *info,
                               uint8_t **out_data, uint64_t *out_size) {
  if (!scratch_allocator || !info || !out_data || !out_size ||
      !vkr_mesh_cooked_string_is_valid(info->source_path, false_v) ||
      !info->dependency_paths || info->dependency_count == 0 ||
      info->dependency_count > VKR_MESH_COOKED_MAX_DEPENDENCIES ||
      !info->ranges || info->range_count == 0 ||
      info->range_count > VKR_MESH_COOKED_MAX_RANGES ||
      info->mesh_buffer.vertex_size != sizeof(VkrVertex3d) ||
      info->mesh_buffer.index_size != sizeof(uint32_t) ||
      info->mesh_buffer.vertex_count == 0 ||
      info->mesh_buffer.index_count == 0 || !info->mesh_buffer.vertices ||
      !info->mesh_buffer.indices ||
      !isfinite(info->budgets.position_relative) ||
      !isfinite(info->budgets.normal_degrees) ||
      !isfinite(info->budgets.tangent_degrees) ||
      !isfinite(info->budgets.uv_absolute) ||
      !isfinite(info->budgets.color_absolute) ||
      info->budgets.position_relative <= 0.0f ||
      info->budgets.normal_degrees <= 0.0f ||
      info->budgets.tangent_degrees <= 0.0f ||
      info->budgets.uv_absolute <= 0.0f ||
      info->budgets.color_absolute <= 0.0f) {
    return false_v;
  }

  *out_data = NULL;
  *out_size = 0;
  VkrMeshCookedDependencyBuild *dependencies = vkr_allocator_alloc(
      scratch_allocator,
      (uint64_t)info->dependency_count * sizeof(*dependencies),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrMeshCookedRangeBuild *ranges = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)info->range_count * sizeof(*ranges),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!dependencies || !ranges) {
    return false_v;
  }
  MemZero(dependencies,
          (uint64_t)info->dependency_count * sizeof(*dependencies));
  MemZero(ranges, (uint64_t)info->range_count * sizeof(*ranges));

  uint64_t string_size = info->source_path.length;
  for (uint32_t i = 0; i < info->dependency_count; ++i) {
    if (!vkr_mesh_cooked_string_is_valid(info->dependency_paths[i], false_v)) {
      return false_v;
    }
    dependencies[i].path = info->dependency_paths[i];
    dependencies[i].string_offset = string_size;
    if (!vkr_mesh_cooked_add_u64(string_size, dependencies[i].path.length,
                                 &string_size)) {
      return false_v;
    }
  }

  const VkrVertex3d *source_vertices =
      (const VkrVertex3d *)info->mesh_buffer.vertices;
  const uint32_t *source_indices = (const uint32_t *)info->mesh_buffer.indices;
  for (uint32_t i = 0; i < info->mesh_buffer.vertex_count; ++i) {
    if (!vkr_mesh_cooked_vertex_is_finite(&source_vertices[i])) {
      return false_v;
    }
  }
  VkrGpuGeometryDecodeRecord geometry_decode = {0};
  VkrGeometryQuantizationMetrics quantization_max = {0};
  uint64_t total_vertices = 0;
  uint64_t total_indices = 0;
  for (uint32_t i = 0; i < info->range_count; ++i) {
    const VkrMeshLoaderSubmeshRange *range = &info->ranges[i];
    if (range->range_id != i || range->index_count == 0 ||
        range->index_count % 3u != 0 ||
        range->first_index > info->mesh_buffer.index_count ||
        range->index_count >
            info->mesh_buffer.index_count - range->first_index ||
        range->pipeline_domain >= VKR_PIPELINE_DOMAIN_COUNT ||
        !vkr_mesh_cooked_string_is_valid(range->material_name, true_v) ||
        !vkr_mesh_cooked_string_is_valid(range->shader_override, true_v)) {
      return false_v;
    }

    uint32_t min_vertex = UINT32_MAX;
    uint32_t max_vertex = 0;
    for (uint32_t j = 0; j < range->index_count; ++j) {
      uint32_t index = source_indices[range->first_index + j];
      if (index >= info->mesh_buffer.vertex_count) {
        return false_v;
      }
      min_vertex = index < min_vertex ? index : min_vertex;
      max_vertex = index > max_vertex ? index : max_vertex;
    }
    uint64_t span64 = (uint64_t)max_vertex - min_vertex + 1u;
    if (span64 > UINT32_MAX) {
      return false_v;
    }
    uint32_t span = (uint32_t)span64;
    uint32_t *source_local_indices = vkr_allocator_alloc(
        scratch_allocator, (uint64_t)range->index_count * sizeof(uint32_t),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *optimized_indices = vkr_allocator_alloc(
        scratch_allocator, (uint64_t)range->index_count * sizeof(uint32_t),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrVertex3d *optimized_vertices = vkr_allocator_alloc(
        scratch_allocator, (uint64_t)span * sizeof(VkrVertex3d),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!source_local_indices || !optimized_indices || !optimized_vertices) {
      return false_v;
    }
    for (uint32_t j = 0; j < range->index_count; ++j) {
      source_local_indices[j] =
          source_indices[range->first_index + j] - min_vertex;
    }

    size_t optimized_vertex_count = vkr_meshopt_optimize_range(
        optimized_vertices, optimized_indices, source_vertices + min_vertex,
        source_local_indices, span, range->index_count, sizeof(VkrVertex3d));
    if (optimized_vertex_count == 0 || optimized_vertex_count > UINT32_MAX) {
      log_error("MeshCooked: range %u meshoptimizer locality pass failed", i);
      return false_v;
    }
    Vec3 optimized_center = vec3_zero();
    Vec3 optimized_min = vec3_zero();
    Vec3 optimized_max = vec3_zero();
    if (!vkr_mesh_cooked_compute_range_bounds(
            optimized_vertices, (uint32_t)optimized_vertex_count,
            &optimized_center, &optimized_min, &optimized_max)) {
      log_error("MeshCooked: range %u has invalid optimized bounds", i);
      return false_v;
    }

    VkrPackedStaticVertex *packed_vertices = vkr_allocator_alloc(
        scratch_allocator,
        optimized_vertex_count * sizeof(VkrPackedStaticVertex),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrGpuGeometryDecodeRecord range_decode = {0};
    VkrGeometryQuantizationMetrics range_quantization = {0};
    if (!packed_vertices) {
      return false_v;
    }
    if (!vkr_packed_geometry_pack(
            optimized_vertices, (uint32_t)optimized_vertex_count, optimized_min,
            optimized_max, &info->budgets, packed_vertices, &range_decode,
            &range_quantization)) {
      log_error("MeshCooked: range %u exceeds quantization budgets "
                "(position=%g normal=%g tangent=%g uv=%g color=%g)",
                i, range_quantization.position_max,
                range_quantization.normal_degrees_max,
                range_quantization.tangent_degrees_max,
                range_quantization.uv_max, range_quantization.color_max);
      return false_v;
    }
    if (i == 0)
      geometry_decode = range_decode;
    quantization_max.position_max =
        Max(quantization_max.position_max, range_quantization.position_max);
    quantization_max.normal_degrees_max =
        Max(quantization_max.normal_degrees_max,
            range_quantization.normal_degrees_max);
    quantization_max.tangent_degrees_max =
        Max(quantization_max.tangent_degrees_max,
            range_quantization.tangent_degrees_max);
    quantization_max.uv_max =
        Max(quantization_max.uv_max, range_quantization.uv_max);
    quantization_max.color_max =
        Max(quantization_max.color_max, range_quantization.color_max);

    size_t vertex_bound = vkr_meshopt_vertex_encode_bound(
        optimized_vertex_count, sizeof(VkrPackedStaticVertex));
    size_t index_bound = vkr_meshopt_index_encode_bound(range->index_count,
                                                        optimized_vertex_count);
    uint8_t *encoded_vertices = vkr_allocator_alloc(
        scratch_allocator, vertex_bound, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    uint8_t *encoded_indices = vkr_allocator_alloc(
        scratch_allocator, index_bound, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    if (!encoded_vertices || !encoded_indices) {
      return false_v;
    }
    size_t vertex_encoded_size = vkr_meshopt_encode_vertices(
        encoded_vertices, vertex_bound, packed_vertices, optimized_vertex_count,
        sizeof(VkrPackedStaticVertex));
    size_t index_encoded_size = vkr_meshopt_encode_indices(
        encoded_indices, index_bound, optimized_indices, range->index_count,
        optimized_vertex_count);
    if (vertex_encoded_size == 0 || index_encoded_size == 0 ||
        vkr_meshopt_vertex_codec_version(encoded_vertices,
                                         vertex_encoded_size) !=
            (int)VKR_MESHOPT_VERTEX_CODEC_VERSION ||
        vkr_meshopt_index_codec_version(encoded_indices, index_encoded_size) !=
            (int)VKR_MESHOPT_INDEX_CODEC_VERSION) {
      return false_v;
    }

    ranges[i] = (VkrMeshCookedRangeBuild){
        .source = range,
        .encoded_vertices = encoded_vertices,
        .encoded_vertex_size = vertex_encoded_size,
        .encoded_indices = encoded_indices,
        .encoded_index_size = index_encoded_size,
        .vertex_count = (uint32_t)optimized_vertex_count,
        .index_count = range->index_count,
        .first_index = (uint32_t)total_indices,
        .material_offset = string_size,
        .vertex_crc =
            vkr_mesh_cooked_crc32(encoded_vertices, vertex_encoded_size),
        .index_crc = vkr_mesh_cooked_crc32(encoded_indices, index_encoded_size),
        .quantization = range_quantization,
        .decode = range_decode,
        .center = optimized_center,
        .min_extents = optimized_min,
        .max_extents = optimized_max,
    };
    if (!vkr_mesh_cooked_add_u64(string_size, range->material_name.length,
                                 &string_size)) {
      return false_v;
    }
    ranges[i].shader_offset = string_size;
    if (!vkr_mesh_cooked_add_u64(string_size, range->shader_override.length,
                                 &string_size)) {
      return false_v;
    }
    total_vertices += optimized_vertex_count;
    total_indices += range->index_count;
    if (total_vertices > UINT32_MAX || total_indices > UINT32_MAX) {
      return false_v;
    }
  }

  uint8_t source_hash[32];
  if (!vkr_mesh_cooked_hash_dependencies(scratch_allocator, dependencies,
                                         info->dependency_count, source_hash)) {
    return false_v;
  }
  uint8_t settings_hash[32];
  vkr_mesh_cooked_hash_settings(&info->budgets, settings_hash);

  uint64_t range_bytes = 0;
  uint64_t dependency_bytes = 0;
  if (!vkr_mesh_cooked_mul_u64(info->range_count, VKR_MESH_COOKED_RANGE_SIZE,
                               &range_bytes) ||
      !vkr_mesh_cooked_mul_u64(info->dependency_count,
                               VKR_MESH_COOKED_DEPENDENCY_SIZE,
                               &dependency_bytes)) {
    return false_v;
  }
  const uint64_t directory_offset = VKR_MESH_COOKED_HEADER_SIZE;
  uint64_t dependency_offset = 0;
  uint64_t string_offset = 0;
  uint64_t stream_offset = 0;
  if (!vkr_mesh_cooked_add_u64(directory_offset, range_bytes,
                               &dependency_offset) ||
      !vkr_mesh_cooked_add_u64(dependency_offset, dependency_bytes,
                               &string_offset) ||
      !vkr_mesh_cooked_add_u64(string_offset, string_size, &stream_offset)) {
    return false_v;
  }
  stream_offset =
      vkr_mesh_cooked_align(stream_offset, VKR_MESH_COOKED_STREAM_ALIGNMENT);
  uint64_t file_size = stream_offset;
  for (uint32_t i = 0; i < info->range_count; ++i) {
    ranges[i].vertex_stream_offset = file_size;
    if (!vkr_mesh_cooked_add_u64(file_size, ranges[i].encoded_vertex_size,
                                 &file_size)) {
      return false_v;
    }
    file_size =
        vkr_mesh_cooked_align(file_size, VKR_MESH_COOKED_STREAM_ALIGNMENT);
    ranges[i].index_stream_offset = file_size;
    if (!vkr_mesh_cooked_add_u64(file_size, ranges[i].encoded_index_size,
                                 &file_size)) {
      return false_v;
    }
    file_size =
        vkr_mesh_cooked_align(file_size, VKR_MESH_COOKED_STREAM_ALIGNMENT);
  }
  if (file_size > VKR_MESH_COOKED_MAX_FILE_SIZE || file_size > SIZE_MAX) {
    return false_v;
  }

  uint8_t *artifact = vkr_allocator_alloc(scratch_allocator, file_size,
                                          VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!artifact) {
    return false_v;
  }
  MemZero(artifact, file_size);
  VkrMeshCookedWriter writer = {
      .data = artifact, .size = file_size, .offset = 0};
  bool8_t ok = true_v;
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, VKR_MESH_COOKED_MAGIC);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, VKR_MESH_COOKED_VERSION);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, VKR_MESH_COOKED_ENDIAN_TAG);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, VKR_MESH_COOKED_HEADER_SIZE);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, vkr_meshopt_library_version());
  ok = ok &&
       vkr_mesh_cooked_writer_u32(&writer, VKR_MESHOPT_VERTEX_CODEC_VERSION);
  ok = ok &&
       vkr_mesh_cooked_writer_u32(&writer, VKR_MESHOPT_INDEX_CODEC_VERSION);
  ok = ok && vkr_mesh_cooked_writer_u32(
                 &writer, VKR_MESH_COOKED_LAYOUT_STATIC_PACKED_V1);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, sizeof(VkrPackedStaticVertex));
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, sizeof(uint32_t));
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, info->range_count);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, info->dependency_count);
  ok = ok && vkr_mesh_cooked_writer_u64(&writer, directory_offset);
  ok = ok && vkr_mesh_cooked_writer_u64(&writer, dependency_offset);
  ok = ok && vkr_mesh_cooked_writer_u64(&writer, string_offset);
  ok = ok && vkr_mesh_cooked_writer_u64(&writer, string_size);
  ok = ok && vkr_mesh_cooked_writer_u64(&writer, stream_offset);
  ok = ok && vkr_mesh_cooked_writer_u64(&writer, file_size);
  ok = ok && vkr_mesh_cooked_writer_u64(&writer, 0u);
  ok = ok &&
       vkr_mesh_cooked_writer_u32(&writer, (uint32_t)info->source_path.length);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, 0u);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, (uint32_t)total_vertices);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, (uint32_t)total_indices);
  ok = ok && vkr_mesh_cooked_writer_bytes(&writer, source_hash, 32u);
  ok = ok && vkr_mesh_cooked_writer_bytes(&writer, settings_hash, 32u);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, 0u);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, 0u);
  for (uint32_t i = 0; i < 3u; ++i)
    ok = ok &&
         vkr_mesh_cooked_writer_f32(&writer, geometry_decode.position_bias[i]);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, geometry_decode.flags);
  for (uint32_t i = 0; i < 3u; ++i)
    ok = ok &&
         vkr_mesh_cooked_writer_f32(&writer, geometry_decode.position_scale[i]);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, geometry_decode.reserved);
  ok = ok &&
       vkr_mesh_cooked_writer_f32(&writer, info->budgets.position_relative);
  ok = ok && vkr_mesh_cooked_writer_f32(&writer, info->budgets.normal_degrees);
  ok = ok && vkr_mesh_cooked_writer_f32(&writer, info->budgets.tangent_degrees);
  ok = ok && vkr_mesh_cooked_writer_f32(&writer, info->budgets.uv_absolute);
  ok = ok && vkr_mesh_cooked_writer_f32(&writer, info->budgets.color_absolute);
  ok = ok && vkr_mesh_cooked_writer_f32(&writer, quantization_max.position_max);
  ok = ok &&
       vkr_mesh_cooked_writer_f32(&writer, quantization_max.normal_degrees_max);
  ok = ok && vkr_mesh_cooked_writer_f32(&writer,
                                        quantization_max.tangent_degrees_max);
  ok = ok && vkr_mesh_cooked_writer_f32(&writer, quantization_max.uv_max);
  ok = ok && vkr_mesh_cooked_writer_f32(&writer, quantization_max.color_max);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, 0u);
  ok = ok && vkr_mesh_cooked_writer_u32(&writer, 0u);
  if (!ok || writer.offset != VKR_MESH_COOKED_HEADER_SIZE) {
    return false_v;
  }

  writer.offset = directory_offset;
  for (uint32_t i = 0; ok && i < info->range_count; ++i) {
    const VkrMeshCookedRangeBuild *range = &ranges[i];
    const VkrMeshLoaderSubmeshRange *source = range->source;
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, i);
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, range->first_index);
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, range->index_count);
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, range->vertex_count);
    ok = ok && vkr_mesh_cooked_writer_i32(&writer, 0);
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, source->pipeline_domain);
    ok = ok && vkr_mesh_cooked_writer_u64(&writer, range->material_offset);
    ok = ok && vkr_mesh_cooked_writer_u32(
                   &writer, (uint32_t)source->material_name.length);
    ok = ok && vkr_mesh_cooked_writer_u32(
                   &writer, (uint32_t)source->shader_override.length);
    ok = ok && vkr_mesh_cooked_writer_u64(&writer, range->shader_offset);
    ok = ok && vkr_mesh_cooked_writer_u64(&writer, range->vertex_stream_offset);
    ok = ok && vkr_mesh_cooked_writer_u64(&writer, range->encoded_vertex_size);
    ok = ok &&
         vkr_mesh_cooked_writer_u64(&writer, (uint64_t)range->vertex_count *
                                                 sizeof(VkrPackedStaticVertex));
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, range->vertex_crc);
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, range->index_crc);
    ok = ok && vkr_mesh_cooked_writer_u64(&writer, range->index_stream_offset);
    ok = ok && vkr_mesh_cooked_writer_u64(&writer, range->encoded_index_size);
    ok = ok && vkr_mesh_cooked_writer_u64(
                   &writer, (uint64_t)range->index_count * sizeof(uint32_t));
    ok = ok && vkr_mesh_cooked_writer_f32(&writer, range->center.x);
    ok = ok && vkr_mesh_cooked_writer_f32(&writer, range->center.y);
    ok = ok && vkr_mesh_cooked_writer_f32(&writer, range->center.z);
    ok = ok && vkr_mesh_cooked_writer_f32(&writer, range->min_extents.x);
    ok = ok && vkr_mesh_cooked_writer_f32(&writer, range->min_extents.y);
    ok = ok && vkr_mesh_cooked_writer_f32(&writer, range->min_extents.z);
    ok = ok && vkr_mesh_cooked_writer_f32(&writer, range->max_extents.x);
    ok = ok && vkr_mesh_cooked_writer_f32(&writer, range->max_extents.y);
    ok = ok && vkr_mesh_cooked_writer_f32(&writer, range->max_extents.z);
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, 0u);
    ok = ok &&
         vkr_mesh_cooked_writer_f32(&writer, range->quantization.position_max);
    ok = ok && vkr_mesh_cooked_writer_f32(
                   &writer, range->quantization.normal_degrees_max);
    ok = ok && vkr_mesh_cooked_writer_f32(
                   &writer, range->quantization.tangent_degrees_max);
    ok = ok && vkr_mesh_cooked_writer_f32(&writer, range->quantization.uv_max);
    ok = ok &&
         vkr_mesh_cooked_writer_f32(&writer, range->quantization.color_max);
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, 0u);
    for (uint32_t axis = 0; axis < 3u; ++axis)
      ok = ok && vkr_mesh_cooked_writer_f32(&writer,
                                            range->decode.position_bias[axis]);
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, range->decode.flags);
    for (uint32_t axis = 0; axis < 3u; ++axis)
      ok = ok && vkr_mesh_cooked_writer_f32(&writer,
                                            range->decode.position_scale[axis]);
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, range->decode.reserved);
  }
  if (!ok || writer.offset != dependency_offset) {
    return false_v;
  }

  writer.offset = dependency_offset;
  for (uint32_t i = 0; ok && i < info->dependency_count; ++i) {
    ok = ok &&
         vkr_mesh_cooked_writer_u64(&writer, dependencies[i].string_offset);
    ok = ok && vkr_mesh_cooked_writer_u32(
                   &writer, (uint32_t)dependencies[i].path.length);
    ok = ok && vkr_mesh_cooked_writer_u32(&writer, 0u);
    ok = ok && vkr_mesh_cooked_writer_u64(&writer, dependencies[i].byte_size);
    ok = ok && vkr_mesh_cooked_writer_bytes(&writer, dependencies[i].hash, 32u);
    ok = ok && vkr_mesh_cooked_writer_u64(&writer, 0u);
  }
  if (!ok || writer.offset != string_offset) {
    return false_v;
  }

  writer.offset = string_offset;
  ok = vkr_mesh_cooked_writer_bytes(&writer, info->source_path.str,
                                    info->source_path.length);
  for (uint32_t i = 0; ok && i < info->dependency_count; ++i) {
    ok = vkr_mesh_cooked_writer_bytes(&writer, dependencies[i].path.str,
                                      dependencies[i].path.length);
  }
  for (uint32_t i = 0; ok && i < info->range_count; ++i) {
    ok = vkr_mesh_cooked_writer_bytes(&writer,
                                      ranges[i].source->material_name.str,
                                      ranges[i].source->material_name.length);
    ok = ok && vkr_mesh_cooked_writer_bytes(
                   &writer, ranges[i].source->shader_override.str,
                   ranges[i].source->shader_override.length);
  }
  if (!ok || writer.offset != string_offset + string_size) {
    return false_v;
  }

  for (uint32_t i = 0; i < info->range_count; ++i) {
    MemCopy(artifact + ranges[i].vertex_stream_offset,
            ranges[i].encoded_vertices, ranges[i].encoded_vertex_size);
    MemCopy(artifact + ranges[i].index_stream_offset, ranges[i].encoded_indices,
            ranges[i].encoded_index_size);
  }
  uint32_t metadata_crc = vkr_mesh_cooked_crc32(
      artifact + directory_offset, stream_offset - directory_offset);
  uint32_t metadata_crc_le = vkr_mesh_cooked_to_le32(metadata_crc);
  MemCopy(artifact + VKR_MESH_COOKED_METADATA_CRC_OFFSET, &metadata_crc_le,
          sizeof(metadata_crc_le));
  uint32_t header_crc =
      vkr_mesh_cooked_crc32(artifact, VKR_MESH_COOKED_HEADER_SIZE);
  uint32_t header_crc_le = vkr_mesh_cooked_to_le32(header_crc);
  MemCopy(artifact + VKR_MESH_COOKED_HEADER_CRC_OFFSET, &header_crc_le,
          sizeof(header_crc_le));

  *out_data = artifact;
  *out_size = file_size;
  return true_v;
}

bool8_t vkr_mesh_cooked_decode(VkrAllocator *result_allocator,
                               VkrAllocator *scratch_allocator,
                               const uint8_t *data, uint64_t size,
                               VkrMeshCookedDecoded *out_decoded) {
  if (!result_allocator || !scratch_allocator || !data || !out_decoded ||
      size < VKR_MESH_COOKED_HEADER_SIZE ||
      size > VKR_MESH_COOKED_MAX_FILE_SIZE) {
    return false_v;
  }
  *out_decoded = (VkrMeshCookedDecoded){0};

  uint8_t header_copy[VKR_MESH_COOKED_HEADER_SIZE];
  MemCopy(header_copy, data, sizeof(header_copy));
  uint32_t stored_crc_le = 0;
  MemCopy(&stored_crc_le, header_copy + VKR_MESH_COOKED_HEADER_CRC_OFFSET,
          sizeof(stored_crc_le));
  MemZero(header_copy + VKR_MESH_COOKED_HEADER_CRC_OFFSET,
          sizeof(stored_crc_le));
  if (vkr_mesh_cooked_to_le32(stored_crc_le) !=
      vkr_mesh_cooked_crc32(header_copy, sizeof(header_copy))) {
    return false_v;
  }

  VkrMeshCookedReader reader = {.data = data, .size = size, .offset = 0};
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t endian_tag = 0;
  uint32_t header_size = 0;
  uint32_t library_version = 0;
  uint32_t vertex_codec_version = 0;
  uint32_t index_codec_version = 0;
  uint32_t layout = 0;
  uint32_t vertex_stride = 0;
  uint32_t index_stride = 0;
  uint32_t range_count = 0;
  uint32_t dependency_count = 0;
  uint64_t directory_offset = 0;
  uint64_t dependency_offset = 0;
  uint64_t string_offset = 0;
  uint64_t string_size = 0;
  uint64_t stream_offset = 0;
  uint64_t file_size = 0;
  uint64_t source_path_offset = 0;
  uint32_t source_path_length = 0;
  uint32_t flags = 0;
  uint32_t total_vertex_count = 0;
  uint32_t total_index_count = 0;
  uint8_t source_hash[32];
  uint8_t settings_hash[32];
  uint32_t header_crc = 0;
  uint32_t metadata_crc = 0;
  VkrGpuGeometryDecodeRecord geometry_decode = {0};
  VkrGeometryQuantizationBudgets budgets = {0};
  VkrGeometryQuantizationMetrics quantization = {0};
  uint32_t header_reserved[2] = {0};
  bool8_t ok = true_v;
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &magic);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &version);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &endian_tag);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &header_size);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &library_version);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &vertex_codec_version);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &index_codec_version);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &layout);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &vertex_stride);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &index_stride);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &range_count);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &dependency_count);
  ok = ok && vkr_mesh_cooked_reader_u64(&reader, &directory_offset);
  ok = ok && vkr_mesh_cooked_reader_u64(&reader, &dependency_offset);
  ok = ok && vkr_mesh_cooked_reader_u64(&reader, &string_offset);
  ok = ok && vkr_mesh_cooked_reader_u64(&reader, &string_size);
  ok = ok && vkr_mesh_cooked_reader_u64(&reader, &stream_offset);
  ok = ok && vkr_mesh_cooked_reader_u64(&reader, &file_size);
  ok = ok && vkr_mesh_cooked_reader_u64(&reader, &source_path_offset);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &source_path_length);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &flags);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &total_vertex_count);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &total_index_count);
  ok = ok && vkr_mesh_cooked_reader_bytes(&reader, source_hash, 32u);
  ok = ok && vkr_mesh_cooked_reader_bytes(&reader, settings_hash, 32u);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &header_crc);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &metadata_crc);
  for (uint32_t i = 0; i < 3u; ++i)
    ok = ok &&
         vkr_mesh_cooked_reader_f32(&reader, &geometry_decode.position_bias[i]);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &geometry_decode.flags);
  for (uint32_t i = 0; i < 3u; ++i)
    ok = ok && vkr_mesh_cooked_reader_f32(&reader,
                                          &geometry_decode.position_scale[i]);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &geometry_decode.reserved);
  ok = ok && vkr_mesh_cooked_reader_f32(&reader, &budgets.position_relative);
  ok = ok && vkr_mesh_cooked_reader_f32(&reader, &budgets.normal_degrees);
  ok = ok && vkr_mesh_cooked_reader_f32(&reader, &budgets.tangent_degrees);
  ok = ok && vkr_mesh_cooked_reader_f32(&reader, &budgets.uv_absolute);
  ok = ok && vkr_mesh_cooked_reader_f32(&reader, &budgets.color_absolute);
  ok = ok && vkr_mesh_cooked_reader_f32(&reader, &quantization.position_max);
  ok = ok &&
       vkr_mesh_cooked_reader_f32(&reader, &quantization.normal_degrees_max);
  ok = ok &&
       vkr_mesh_cooked_reader_f32(&reader, &quantization.tangent_degrees_max);
  ok = ok && vkr_mesh_cooked_reader_f32(&reader, &quantization.uv_max);
  ok = ok && vkr_mesh_cooked_reader_f32(&reader, &quantization.color_max);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &header_reserved[0]);
  ok = ok && vkr_mesh_cooked_reader_u32(&reader, &header_reserved[1]);
  if (!ok || magic != VKR_MESH_COOKED_MAGIC ||
      version != VKR_MESH_COOKED_VERSION ||
      endian_tag != VKR_MESH_COOKED_ENDIAN_TAG ||
      header_size != VKR_MESH_COOKED_HEADER_SIZE ||
      library_version != vkr_meshopt_library_version() ||
      vertex_codec_version != VKR_MESHOPT_VERTEX_CODEC_VERSION ||
      index_codec_version != VKR_MESHOPT_INDEX_CODEC_VERSION ||
      layout != VKR_MESH_COOKED_LAYOUT_STATIC_PACKED_V1 ||
      vertex_stride != sizeof(VkrPackedStaticVertex) ||
      index_stride != sizeof(uint32_t) || range_count == 0 ||
      range_count > VKR_MESH_COOKED_MAX_RANGES || dependency_count == 0 ||
      dependency_count > VKR_MESH_COOKED_MAX_DEPENDENCIES ||
      total_vertex_count == 0 || total_index_count == 0 || flags != 0 ||
      !vkr_packed_geometry_decode_is_valid(&geometry_decode) ||
      header_reserved[0] != 0 || header_reserved[1] != 0 || file_size != size ||
      directory_offset != VKR_MESH_COOKED_HEADER_SIZE) {
    return false_v;
  }
  if (!isfinite(budgets.position_relative) ||
      !isfinite(budgets.normal_degrees) || !isfinite(budgets.tangent_degrees) ||
      !isfinite(budgets.uv_absolute) || !isfinite(budgets.color_absolute) ||
      !isfinite(quantization.position_max) ||
      !isfinite(quantization.normal_degrees_max) ||
      !isfinite(quantization.tangent_degrees_max) ||
      !isfinite(quantization.uv_max) || !isfinite(quantization.color_max) ||
      budgets.position_relative <= 0.0f || budgets.normal_degrees <= 0.0f ||
      budgets.tangent_degrees <= 0.0f || budgets.uv_absolute <= 0.0f ||
      budgets.color_absolute <= 0.0f || quantization.position_max < 0.0f ||
      quantization.normal_degrees_max < 0.0f ||
      quantization.tangent_degrees_max < 0.0f || quantization.uv_max < 0.0f ||
      quantization.color_max < 0.0f ||
      quantization.normal_degrees_max > budgets.normal_degrees ||
      quantization.tangent_degrees_max > budgets.tangent_degrees ||
      quantization.uv_max > budgets.uv_absolute ||
      quantization.color_max > budgets.color_absolute) {
    return false_v;
  }

  uint8_t expected_settings_hash[32];
  vkr_mesh_cooked_hash_settings(&budgets, expected_settings_hash);
  if (MemCompare(settings_hash, expected_settings_hash,
                 sizeof(settings_hash)) != 0) {
    return false_v;
  }

  uint64_t range_bytes = 0;
  uint64_t dependency_bytes = 0;
  uint64_t expected_dependency_offset = 0;
  uint64_t expected_string_offset = 0;
  uint64_t expected_stream_offset = 0;
  if (!vkr_mesh_cooked_mul_u64(range_count, VKR_MESH_COOKED_RANGE_SIZE,
                               &range_bytes) ||
      !vkr_mesh_cooked_mul_u64(dependency_count,
                               VKR_MESH_COOKED_DEPENDENCY_SIZE,
                               &dependency_bytes) ||
      !vkr_mesh_cooked_add_u64(directory_offset, range_bytes,
                               &expected_dependency_offset) ||
      !vkr_mesh_cooked_add_u64(dependency_offset, dependency_bytes,
                               &expected_string_offset) ||
      !vkr_mesh_cooked_add_u64(string_offset, string_size,
                               &expected_stream_offset) ||
      dependency_offset != expected_dependency_offset ||
      string_offset != expected_string_offset ||
      expected_stream_offset > size ||
      stream_offset !=
          vkr_mesh_cooked_align(expected_stream_offset,
                                VKR_MESH_COOKED_STREAM_ALIGNMENT) ||
      stream_offset > size || source_path_length == 0 ||
      source_path_offset > string_size ||
      source_path_length > string_size - source_path_offset) {
    return false_v;
  }
  if (metadata_crc != vkr_mesh_cooked_crc32(data + directory_offset,
                                            stream_offset - directory_offset)) {
    return false_v;
  }

  VkrMeshCookedDependencyView *dependencies = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)dependency_count * sizeof(*dependencies),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrMeshCookedRangeView *ranges = vkr_allocator_alloc(
      scratch_allocator, (uint64_t)range_count * sizeof(*ranges),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!dependencies || !ranges) {
    return false_v;
  }
  MemZero(dependencies, (uint64_t)dependency_count * sizeof(*dependencies));
  MemZero(ranges, (uint64_t)range_count * sizeof(*ranges));

  uint64_t source_bytes = 0;
  reader.offset = dependency_offset;
  for (uint32_t i = 0; i < dependency_count; ++i) {
    uint64_t path_offset = 0;
    uint32_t path_length = 0;
    uint32_t dep_reserved = 0;
    uint64_t tail_reserved = 0;
    if (!vkr_mesh_cooked_reader_u64(&reader, &path_offset) ||
        !vkr_mesh_cooked_reader_u32(&reader, &path_length) ||
        !vkr_mesh_cooked_reader_u32(&reader, &dep_reserved) ||
        !vkr_mesh_cooked_reader_u64(&reader, &dependencies[i].byte_size) ||
        !vkr_mesh_cooked_reader_bytes(&reader, dependencies[i].hash, 32u) ||
        !vkr_mesh_cooked_reader_u64(&reader, &tail_reserved) ||
        dep_reserved != 0 || tail_reserved != 0 || path_length == 0 ||
        !vkr_mesh_cooked_string_view(data, string_offset, string_size,
                                     path_offset, path_length, false_v,
                                     &dependencies[i].path)) {
      return false_v;
    }
    if (UINT64_MAX - source_bytes < dependencies[i].byte_size) {
      return false_v;
    }
    source_bytes += dependencies[i].byte_size;
  }
  if (reader.offset != string_offset) {
    return false_v;
  }

  reader.offset = directory_offset;
  uint64_t accumulated_vertices = 0;
  uint64_t accumulated_indices = 0;
  uint64_t prior_stream_end = stream_offset;
  VkrGeometryQuantizationMetrics range_quantization_max = {0};
  for (uint32_t i = 0; i < range_count; ++i) {
    VkrMeshCookedRangeView *range = &ranges[i];
    uint32_t pipeline_domain = 0;
    uint64_t material_offset = 0;
    uint32_t material_length = 0;
    uint32_t shader_length = 0;
    uint64_t shader_offset = 0;
    uint32_t range_reserved = 0;
    uint32_t quantization_reserved = 0;
    if (!vkr_mesh_cooked_reader_u32(&reader, &range->range_id) ||
        !vkr_mesh_cooked_reader_u32(&reader, &range->first_index) ||
        !vkr_mesh_cooked_reader_u32(&reader, &range->index_count) ||
        !vkr_mesh_cooked_reader_u32(&reader, &range->vertex_count) ||
        !vkr_mesh_cooked_reader_i32(&reader, &range->vertex_offset) ||
        !vkr_mesh_cooked_reader_u32(&reader, &pipeline_domain) ||
        !vkr_mesh_cooked_reader_u64(&reader, &material_offset) ||
        !vkr_mesh_cooked_reader_u32(&reader, &material_length) ||
        !vkr_mesh_cooked_reader_u32(&reader, &shader_length) ||
        !vkr_mesh_cooked_reader_u64(&reader, &shader_offset) ||
        !vkr_mesh_cooked_reader_u64(&reader, &range->vertex_stream_offset) ||
        !vkr_mesh_cooked_reader_u64(&reader, &range->vertex_encoded_size) ||
        !vkr_mesh_cooked_reader_u64(&reader, &range->vertex_decoded_size) ||
        !vkr_mesh_cooked_reader_u32(&reader, &range->vertex_crc) ||
        !vkr_mesh_cooked_reader_u32(&reader, &range->index_crc) ||
        !vkr_mesh_cooked_reader_u64(&reader, &range->index_stream_offset) ||
        !vkr_mesh_cooked_reader_u64(&reader, &range->index_encoded_size) ||
        !vkr_mesh_cooked_reader_u64(&reader, &range->index_decoded_size) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->center.x) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->center.y) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->center.z) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->min_extents.x) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->min_extents.y) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->min_extents.z) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->max_extents.x) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->max_extents.y) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->max_extents.z) ||
        !vkr_mesh_cooked_reader_u32(&reader, &range_reserved) ||
        !vkr_mesh_cooked_reader_f32(&reader,
                                    &range->quantization.position_max) ||
        !vkr_mesh_cooked_reader_f32(&reader,
                                    &range->quantization.normal_degrees_max) ||
        !vkr_mesh_cooked_reader_f32(&reader,
                                    &range->quantization.tangent_degrees_max) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->quantization.uv_max) ||
        !vkr_mesh_cooked_reader_f32(&reader, &range->quantization.color_max) ||
        !vkr_mesh_cooked_reader_u32(&reader, &quantization_reserved)) {
      return false_v;
    }
    for (uint32_t axis = 0; axis < 3u; ++axis)
      ok = ok && vkr_mesh_cooked_reader_f32(&reader,
                                            &range->decode.position_bias[axis]);
    ok = ok && vkr_mesh_cooked_reader_u32(&reader, &range->decode.flags);
    for (uint32_t axis = 0; axis < 3u; ++axis)
      ok = ok && vkr_mesh_cooked_reader_f32(
                     &reader, &range->decode.position_scale[axis]);
    ok = ok && vkr_mesh_cooked_reader_u32(&reader, &range->decode.reserved);
    if (!ok) {
      return false_v;
    }
    range->pipeline_domain = (VkrPipelineDomain)pipeline_domain;
    uint64_t expected_vertex_bytes = 0;
    uint64_t expected_index_bytes = 0;
    uint64_t vertex_stream_end = 0;
    uint64_t index_stream_end = 0;
    const float32_t range_sx = range->decode.position_scale[0];
    const float32_t range_sy = range->decode.position_scale[1];
    const float32_t range_sz = range->decode.position_scale[2];
    const float32_t range_position_budget =
        Max(hypotf(hypotf(range_sx, range_sy), range_sz) *
                budgets.position_relative,
            1.0e-7f);
    if ((i == 0u &&
         !vkr_mesh_cooked_decode_equal(&geometry_decode, &range->decode)) ||
        range->range_id != i || range->first_index != accumulated_indices ||
        range->index_count == 0 || range->index_count % 3u != 0 ||
        range->vertex_count == 0 || range->vertex_offset != 0 ||
        pipeline_domain >= VKR_PIPELINE_DOMAIN_COUNT || range_reserved != 0 ||
        quantization_reserved != 0 ||
        !vkr_packed_geometry_decode_is_valid(&range->decode) ||
        !isfinite(range_position_budget) ||
        !isfinite(range->quantization.position_max) ||
        !isfinite(range->quantization.normal_degrees_max) ||
        !isfinite(range->quantization.tangent_degrees_max) ||
        !isfinite(range->quantization.uv_max) ||
        !isfinite(range->quantization.color_max) ||
        range->quantization.position_max > range_position_budget ||
        range->quantization.normal_degrees_max > budgets.normal_degrees ||
        range->quantization.tangent_degrees_max > budgets.tangent_degrees ||
        range->quantization.uv_max > budgets.uv_absolute ||
        range->quantization.color_max > budgets.color_absolute ||
        !vkr_mesh_cooked_mul_u64(range->vertex_count, vertex_stride,
                                 &expected_vertex_bytes) ||
        !vkr_mesh_cooked_mul_u64(range->index_count, index_stride,
                                 &expected_index_bytes) ||
        range->vertex_decoded_size != expected_vertex_bytes ||
        range->index_decoded_size != expected_index_bytes ||
        range->vertex_encoded_size == 0 || range->index_encoded_size == 0 ||
        range->vertex_stream_offset % VKR_MESH_COOKED_STREAM_ALIGNMENT != 0 ||
        range->index_stream_offset % VKR_MESH_COOKED_STREAM_ALIGNMENT != 0 ||
        range->vertex_stream_offset != prior_stream_end ||
        !vkr_mesh_cooked_add_u64(range->vertex_stream_offset,
                                 range->vertex_encoded_size,
                                 &vertex_stream_end) ||
        vertex_stream_end > size ||
        range->index_stream_offset !=
            vkr_mesh_cooked_align(vertex_stream_end,
                                  VKR_MESH_COOKED_STREAM_ALIGNMENT) ||
        !vkr_mesh_cooked_add_u64(range->index_stream_offset,
                                 range->index_encoded_size,
                                 &index_stream_end) ||
        index_stream_end > size ||
        !vkr_mesh_cooked_string_view(data, string_offset, string_size,
                                     material_offset, material_length, true_v,
                                     &range->material_name) ||
        !vkr_mesh_cooked_string_view(data, string_offset, string_size,
                                     shader_offset, shader_length, true_v,
                                     &range->shader_override) ||
        !isfinite(range->center.x) || !isfinite(range->center.y) ||
        !isfinite(range->center.z) || !isfinite(range->min_extents.x) ||
        !isfinite(range->min_extents.y) || !isfinite(range->min_extents.z) ||
        !isfinite(range->max_extents.x) || !isfinite(range->max_extents.y) ||
        !isfinite(range->max_extents.z) ||
        range->min_extents.x > range->max_extents.x ||
        range->min_extents.y > range->max_extents.y ||
        range->min_extents.z > range->max_extents.z ||
        vkr_mesh_cooked_crc32(data + range->vertex_stream_offset,
                              range->vertex_encoded_size) !=
            range->vertex_crc ||
        vkr_mesh_cooked_crc32(data + range->index_stream_offset,
                              range->index_encoded_size) != range->index_crc ||
        vkr_meshopt_vertex_codec_version(data + range->vertex_stream_offset,
                                         range->vertex_encoded_size) !=
            (int)vertex_codec_version ||
        vkr_meshopt_index_codec_version(data + range->index_stream_offset,
                                        range->index_encoded_size) !=
            (int)index_codec_version) {
      return false_v;
    }
    range_quantization_max.position_max = Max(
        range_quantization_max.position_max, range->quantization.position_max);
    range_quantization_max.normal_degrees_max =
        Max(range_quantization_max.normal_degrees_max,
            range->quantization.normal_degrees_max);
    range_quantization_max.tangent_degrees_max =
        Max(range_quantization_max.tangent_degrees_max,
            range->quantization.tangent_degrees_max);
    range_quantization_max.uv_max =
        Max(range_quantization_max.uv_max, range->quantization.uv_max);
    range_quantization_max.color_max =
        Max(range_quantization_max.color_max, range->quantization.color_max);
    accumulated_vertices += range->vertex_count;
    accumulated_indices += range->index_count;
    if (accumulated_vertices > UINT32_MAX || accumulated_indices > UINT32_MAX) {
      return false_v;
    }
    prior_stream_end = vkr_mesh_cooked_align(index_stream_end,
                                             VKR_MESH_COOKED_STREAM_ALIGNMENT);
  }
  if (reader.offset != dependency_offset ||
      accumulated_vertices != total_vertex_count ||
      accumulated_indices != total_index_count || prior_stream_end != size) {
    return false_v;
  }
  if (!vkr_mesh_cooked_quantization_equal(&range_quantization_max,
                                          &quantization)) {
    return false_v;
  }

  String8 source_path_view = {0};
  if (!vkr_mesh_cooked_string_view(data, string_offset, string_size,
                                   source_path_offset, source_path_length,
                                   false_v, &source_path_view)) {
    return false_v;
  }
  uint64_t vertex_bytes = 0;
  uint64_t index_bytes = 0;
  if (!vkr_mesh_cooked_mul_u64(total_vertex_count, vertex_stride,
                               &vertex_bytes) ||
      !vkr_mesh_cooked_mul_u64(total_index_count, index_stride, &index_bytes)) {
    return false_v;
  }
  VkrPackedStaticVertex *vertices = vkr_allocator_alloc(
      result_allocator, vertex_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices = vkr_allocator_alloc(result_allocator, index_bytes,
                                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  Array_VkrMeshLoaderSubmeshRange output_ranges =
      array_create_VkrMeshLoaderSubmeshRange(result_allocator, range_count);
  VkrGpuGeometryDecodeRecord *decodes = vkr_allocator_alloc(
      result_allocator,
      (uint64_t)range_count * sizeof(VkrGpuGeometryDecodeRecord),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!vertices || !indices || !output_ranges.data || !decodes) {
    return false_v;
  }

  uint32_t vertex_base = 0;
  for (uint32_t i = 0; i < range_count; ++i) {
    const VkrMeshCookedRangeView *range = &ranges[i];
    VkrPackedStaticVertex *range_vertices = vertices + vertex_base;
    uint32_t *range_indices = indices + range->first_index;
    if (vkr_meshopt_decode_vertices(range_vertices, range->vertex_count,
                                    vertex_stride,
                                    data + range->vertex_stream_offset,
                                    range->vertex_encoded_size) != 0 ||
        vkr_meshopt_decode_indices(range_indices, range->index_count,
                                   data + range->index_stream_offset,
                                   range->index_encoded_size) != 0 ||
        !vkr_mesh_cooked_validate_packed_range_vertices(
            range_vertices, range->vertex_count, &range->decode, range->center,
            range->min_extents, range->max_extents,
            range->quantization.position_max)) {
      return false_v;
    }
    for (uint32_t j = 0; j < range->index_count; ++j) {
      if (range_indices[j] >= range->vertex_count) {
        return false_v;
      }
      range_indices[j] += vertex_base;
    }
    VkrMeshLoaderSubmeshRange output_range = {
        .range_id = i,
        .first_index = range->first_index,
        .index_count = range->index_count,
        .vertex_offset = 0,
        .decode_index = i,
        .center = range->center,
        .min_extents = range->min_extents,
        .max_extents = range->max_extents,
        .material_name =
            string8_duplicate(result_allocator, &range->material_name),
        .shader_override =
            string8_duplicate(result_allocator, &range->shader_override),
        .pipeline_domain = range->pipeline_domain,
        .material_handle = VKR_MATERIAL_HANDLE_INVALID,
    };
    array_set_VkrMeshLoaderSubmeshRange(&output_ranges, i, output_range);
    decodes[i] = range->decode;
    vertex_base += range->vertex_count;
  }

  out_decoded->mesh_buffer = (VkrMeshLoaderBuffer){
      .vertex_size = vertex_stride,
      .vertex_count = total_vertex_count,
      .vertices = vertices,
      .index_size = index_stride,
      .index_count = total_index_count,
      .indices = indices,
      .vertex_layout = VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1,
      .decodes = decodes,
      .decode_count = range_count,
      .quantization = quantization,
  };
  out_decoded->ranges = output_ranges;
  out_decoded->source_bytes = source_bytes;
  out_decoded->cooked_bytes = size;
  out_decoded->decoded_bytes =
      vertex_bytes + index_bytes +
      (uint64_t)range_count * sizeof(VkrGpuGeometryDecodeRecord);
  return true_v;
}

bool8_t vkr_mesh_cooked_write_atomic(VkrAllocator *scratch_allocator,
                                     String8 output_path, const uint8_t *data,
                                     uint64_t size) {
  if (!scratch_allocator || !output_path.str || output_path.length == 0 ||
      !data || size == 0) {
    return false_v;
  }
  String8 output_directory =
      file_path_get_directory(scratch_allocator, output_path);
  if (output_directory.length > 0 &&
      !file_ensure_directory(scratch_allocator, &output_directory)) {
    return false_v;
  }
  String8 temporary_path = string8_create_formatted(
      scratch_allocator, "%.*s.tmp.%u", (int32_t)output_path.length,
      output_path.str, vkr_platform_get_process_id());
  FilePathType path_type = vkr_mesh_cooked_path_is_absolute(output_path)
                               ? FILE_PATH_TYPE_ABSOLUTE
                               : FILE_PATH_TYPE_RELATIVE;
  FilePath output = file_path_create(string8_cstr(&output_path),
                                     scratch_allocator, path_type);
  FilePath temporary = file_path_create(string8_cstr(&temporary_path),
                                        scratch_allocator, path_type);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  uint64_t written = 0;
  if (file_open(&temporary, mode, &file) != FILE_ERROR_NONE ||
      file_write(&file, size, data, &written) != FILE_ERROR_NONE ||
      written != size || file_sync(&file) != FILE_ERROR_NONE) {
    file_close(&file);
    (void)file_remove(&temporary);
    return false_v;
  }
  file_close(&file);
  if (file_rename(&temporary, &output, true_v) != FILE_ERROR_NONE) {
    (void)file_remove(&temporary);
    return false_v;
  }
  return true_v;
}
