#include "assets/vkr_mesh_encode.h"

#include "core/logger.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"
#include "assets/vkr_mesh_decode.h"
#include "assets/vkr_meshoptimizer_encode.h"

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
  String8 physical_path;
  uint64_t byte_size;
  uint8_t hash[32];
  uint64_t string_offset;
} VkrMeshCookedDependencyBuild;

typedef struct VkrMeshCookedRangeBuild {
  const VkrGeometryUploadRange *source;
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
    bool8_t ok = vkr_mesh_cooked_read_file(
        scratch_allocator, dependencies[i].physical_path, &bytes, &size);
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

static bool8_t vkr_mesh_cooked_source_validate(VkrAllocator *scratch_allocator,
                                               const VkrMeshSource *source,
                                               uint32_t range_count) {
  if (source->nodes.length > VKR_MESH_COOKED_MAX_RANGES ||
      source->meshes.length > VKR_MESH_COOKED_MAX_RANGES ||
      (source->nodes.length && !source->nodes.data) ||
      (source->meshes.length && !source->meshes.data))
    return false_v;
  uint32_t next_range = 0u;
  for (uint32_t i = 0; i < source->meshes.length; ++i) {
    const VkrMeshSourceMesh *mesh = &source->meshes.data[i];
    if (mesh->first_range != next_range ||
        mesh->range_count > range_count - next_range)
      return false_v;
    next_range += mesh->range_count;
  }
  if (source->meshes.length && next_range != range_count)
    return false_v;
  for (uint32_t i = 0; i < source->nodes.length; ++i) {
    const VkrMeshSourceNode *node = &source->nodes.data[i];
    if (node->name.length > VKR_MESH_COOKED_MAX_STRING_LENGTH ||
        (node->name.length && !node->name.str) || node->in_scene > 1u ||
        (node->parent != UINT32_MAX && node->parent >= source->nodes.length) ||
        (node->mesh_variant != UINT32_MAX &&
         node->mesh_variant >= source->meshes.length) ||
        (node->mesh == UINT32_MAX) != (node->mesh_variant == UINT32_MAX) ||
        node->punctual.kind > 3u || !isfinite(node->punctual.color.x) ||
        !isfinite(node->punctual.color.y) ||
        !isfinite(node->punctual.color.z) ||
        !isfinite(node->punctual.intensity) ||
        !isfinite(node->punctual.range) ||
        !isfinite(node->punctual.inner_cone) ||
        !isfinite(node->punctual.outer_cone))
      return false_v;
    for (uint32_t f = 0; f < 16u; ++f)
      if (!isfinite(node->local.elements[f]))
        return false_v;
    if ((node->mesh_variant != UINT32_MAX &&
         source->meshes.data[node->mesh_variant].source_mesh_index !=
             node->mesh) ||
        (node->parent != UINT32_MAX && node->in_scene &&
         !source->nodes.data[node->parent].in_scene))
      return false_v;
  }
  if (!source->nodes.length)
    return true_v;
  uint8_t *node_state = vkr_allocator_alloc(
      scratch_allocator, source->nodes.length, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (source->nodes.length && !node_state)
    return false_v;
  MemZero(node_state, source->nodes.length);
  for (uint32_t i = 0; i < source->nodes.length; ++i) {
    uint32_t node = i;
    while (node != UINT32_MAX && node_state[node] == 0u) {
      node_state[node] = 1u;
      node = source->nodes.data[node].parent;
    }
    if (node != UINT32_MAX && node_state[node] == 1u)
      return false_v;
    node = i;
    while (node != UINT32_MAX && node_state[node] == 1u) {
      node_state[node] = 2u;
      node = source->nodes.data[node].parent;
    }
  }
  return true_v;
}

static bool8_t
vkr_mesh_cooked_write_source_metadata(VkrMeshCookedWriter *writer,
                                      const VkrMeshSource *source) {
  bool8_t ok = true_v;
  ok = ok && vkr_mesh_cooked_writer_u64(writer, source->fingerprint);
  ok = ok && vkr_mesh_cooked_writer_u32(writer, source->animation_count);
  ok = ok && vkr_mesh_cooked_writer_u32(writer, 0u);
  for (uint64_t i = 0; ok && i < source->nodes.length; ++i) {
    const VkrMeshSourceNode *node = &source->nodes.data[i];
    ok = vkr_mesh_cooked_writer_u32(writer, node->parent) &&
         vkr_mesh_cooked_writer_u32(writer, node->mesh) &&
         vkr_mesh_cooked_writer_u32(writer, node->mesh_variant) &&
         vkr_mesh_cooked_writer_u32(writer, node->camera) &&
         vkr_mesh_cooked_writer_u32(writer, node->skin) &&
         vkr_mesh_cooked_writer_u32(writer, node->light) &&
         vkr_mesh_cooked_writer_u32(writer, node->in_scene);
    for (uint32_t f = 0; f < 16u; ++f) {
      ok = ok && vkr_mesh_cooked_writer_f32(writer, node->local.elements[f]);
    }
    ok = ok && vkr_mesh_cooked_writer_f32(writer, node->punctual.color.x) &&
         vkr_mesh_cooked_writer_f32(writer, node->punctual.color.y) &&
         vkr_mesh_cooked_writer_f32(writer, node->punctual.color.z) &&
         vkr_mesh_cooked_writer_f32(writer, node->punctual.intensity) &&
         vkr_mesh_cooked_writer_f32(writer, node->punctual.range) &&
         vkr_mesh_cooked_writer_f32(writer, node->punctual.inner_cone) &&
         vkr_mesh_cooked_writer_f32(writer, node->punctual.outer_cone) &&
         vkr_mesh_cooked_writer_u32(writer, node->punctual.kind);
    ok =
        ok && vkr_mesh_cooked_writer_u32(writer, (uint32_t)node->name.length) &&
        vkr_mesh_cooked_writer_bytes(writer, node->name.str, node->name.length);
  }
  for (uint64_t i = 0; ok && i < source->meshes.length; ++i) {
    ok = vkr_mesh_cooked_writer_u32(writer,
                                    source->meshes.data[i].source_mesh_index) &&
         vkr_mesh_cooked_writer_u32(writer,
                                    source->meshes.data[i].first_range) &&
         vkr_mesh_cooked_writer_u32(writer, source->meshes.data[i].range_count);
  }
  return ok;
}

bool8_t vkr_mesh_cooked_encode(VkrAllocator *scratch_allocator,
                               const VkrMeshCookedEncodeInfo *info,
                               uint8_t **out_data, uint64_t *out_size) {
  if (!scratch_allocator || !info || !out_data || !out_size ||
      !vkr_mesh_cooked_string_is_valid(info->source_path, false_v) ||
      !info->dependency_paths || info->dependency_count == 0 ||
      info->dependency_count > VKR_MESH_COOKED_MAX_DEPENDENCIES ||
      info->range_count > VKR_MESH_COOKED_MAX_RANGES ||
      (info->range_count &&
       (!info->ranges || info->mesh_buffer.vertex_size != sizeof(VkrVertex3d) ||
        info->mesh_buffer.index_size != sizeof(uint32_t) ||
        !info->mesh_buffer.vertex_count || !info->mesh_buffer.index_count ||
        !info->mesh_buffer.vertices || !info->mesh_buffer.indices)) ||
      (!info->range_count &&
       (!info->source.nodes.length || info->mesh_buffer.vertex_count ||
        info->mesh_buffer.index_count)) ||
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
  if (!vkr_mesh_cooked_source_validate(scratch_allocator, &info->source,
                                       info->range_count))
    return false_v;
  VkrMeshCookedDependencyBuild *dependencies = vkr_allocator_alloc(
      scratch_allocator,
      (uint64_t)info->dependency_count * sizeof(*dependencies),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrMeshCookedRangeBuild *ranges =
      info->range_count
          ? vkr_allocator_alloc(scratch_allocator,
                                (uint64_t)info->range_count * sizeof(*ranges),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
          : NULL;
  if (!dependencies || (info->range_count && !ranges)) {
    return false_v;
  }
  MemZero(dependencies,
          (uint64_t)info->dependency_count * sizeof(*dependencies));
  if (info->range_count)
    MemZero(ranges, (uint64_t)info->range_count * sizeof(*ranges));

  uint64_t string_size = info->source_path.length;
  for (uint32_t i = 0; i < info->dependency_count; ++i) {
    if (!vkr_mesh_cooked_string_is_valid(info->dependency_paths[i], false_v)) {
      return false_v;
    }
    dependencies[i].physical_path = info->dependency_paths[i];
    dependencies[i].path = info->dependency_references
                               ? info->dependency_references[i]
                               : info->dependency_paths[i];
    if (!vkr_mesh_cooked_string_is_valid(dependencies[i].path, false_v)) {
      return false_v;
    }
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
    const VkrGeometryUploadRange *range = &info->ranges[i];
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
  const VkrMeshSource *source = &info->source;
  uint64_t source_bytes =
      16u + source->nodes.length * 128u + source->meshes.length * 12u;
  for (uint64_t i = 0; i < source->nodes.length; ++i) {
    if (source->nodes.data[i].name.length > VKR_MESH_COOKED_MAX_STRING_LENGTH ||
        !vkr_mesh_cooked_add_u64(
            source_bytes, source->nodes.data[i].name.length, &source_bytes))
      return false_v;
  }
  const uint64_t directory_offset = VKR_MESH_COOKED_HEADER_SIZE;
  uint64_t dependency_offset = 0;
  uint64_t string_offset = 0;
  uint64_t stream_offset = 0;
  if (!vkr_mesh_cooked_add_u64(directory_offset, range_bytes,
                               &dependency_offset) ||
      !vkr_mesh_cooked_add_u64(
          dependency_offset, dependency_bytes + source_bytes, &string_offset) ||
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
  ok =
      ok && vkr_mesh_cooked_writer_u32(&writer, (uint32_t)source->nodes.length);
  ok = ok &&
       vkr_mesh_cooked_writer_u32(&writer, (uint32_t)source->meshes.length);
  if (!ok || writer.offset != VKR_MESH_COOKED_HEADER_SIZE) {
    return false_v;
  }

  writer.offset = directory_offset;
  for (uint32_t i = 0; ok && i < info->range_count; ++i) {
    const VkrMeshCookedRangeBuild *range = &ranges[i];
    const VkrGeometryUploadRange *source = range->source;
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
  ok = ok && vkr_mesh_cooked_write_source_metadata(&writer, source);
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

/* The caller decoded input successfully and only edits source-node values.
 * Preserve compressed geometry streams and every string/offset verbatim. */
bool8_t vkr_mesh_cooked_source_variant(VkrAllocator *allocator,
                                       const uint8_t *input, uint64_t size,
                                       const VkrMeshSource *source,
                                       uint8_t **out_data) {
  if (!allocator || !input || size < VKR_MESH_COOKED_HEADER_SIZE || !source ||
      !out_data) {
    return false_v;
  }
  uint64_t directory_offset = 0, dependency_offset = 0, string_offset = 0,
           stream_offset = 0;
  uint32_t dependency_count = 0, range_count = 0, node_count = 0,
           mesh_count = 0;
  MemCopy(&range_count, input + 40u, sizeof(range_count));
  MemCopy(&dependency_count, input + 44u, sizeof(dependency_count));
  MemCopy(&directory_offset, input + 48u, sizeof(directory_offset));
  MemCopy(&dependency_offset, input + 56u, sizeof(dependency_offset));
  MemCopy(&string_offset, input + 64u, sizeof(string_offset));
  MemCopy(&stream_offset, input + 80u, sizeof(stream_offset));
  MemCopy(&node_count, input + 264u, sizeof(node_count));
  MemCopy(&mesh_count, input + 268u, sizeof(mesh_count));
  range_count = vkr_mesh_cooked_to_le32(range_count);
  dependency_count = vkr_mesh_cooked_to_le32(dependency_count);
  node_count = vkr_mesh_cooked_to_le32(node_count);
  mesh_count = vkr_mesh_cooked_to_le32(mesh_count);
  directory_offset = vkr_mesh_cooked_to_le64(directory_offset);
  dependency_offset = vkr_mesh_cooked_to_le64(dependency_offset);
  string_offset = vkr_mesh_cooked_to_le64(string_offset);
  stream_offset = vkr_mesh_cooked_to_le64(stream_offset);
  uint64_t metadata_offset =
      dependency_offset +
      (uint64_t)dependency_count * VKR_MESH_COOKED_DEPENDENCY_SIZE;
  if (node_count != source->nodes.length ||
      mesh_count != source->meshes.length ||
      directory_offset > metadata_offset || metadata_offset > string_offset ||
      string_offset > stream_offset || stream_offset > size ||
      !vkr_mesh_cooked_source_validate(allocator, source, range_count)) {
    return false_v;
  }
  uint8_t *copy =
      vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!copy) {
    return false_v;
  }
  MemCopy(copy, input, size);
  VkrMeshCookedWriter writer = {
      .data = copy, .size = string_offset, .offset = metadata_offset};
  if (!vkr_mesh_cooked_write_source_metadata(&writer, source) ||
      writer.offset != string_offset) {
    return false_v;
  }
  uint32_t metadata_crc = vkr_mesh_cooked_to_le32(vkr_mesh_cooked_crc32(
      copy + directory_offset, stream_offset - directory_offset));
  MemCopy(copy + VKR_MESH_COOKED_METADATA_CRC_OFFSET, &metadata_crc,
          sizeof(metadata_crc));
  MemZero(copy + VKR_MESH_COOKED_HEADER_CRC_OFFSET, sizeof(uint32_t));
  uint32_t header_crc = vkr_mesh_cooked_to_le32(
      vkr_mesh_cooked_crc32(copy, VKR_MESH_COOKED_HEADER_SIZE));
  MemCopy(copy + VKR_MESH_COOKED_HEADER_CRC_OFFSET, &header_crc,
          sizeof(header_crc));
  *out_data = copy;
  return true_v;
}
