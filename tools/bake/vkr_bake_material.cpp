#include "bake/vkr_bake_material.h"

#include <ktx.h>
#include <vulkan/vulkan_core.h>

#include <stb_image.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct VkrBakeTextureEntry {
  char *path;
  char sha256[72];
  uint64_t byte_count;
  uint32_t width;
  uint32_t height;
  uint32_t face_count;
  uint8_t *rgba8;
  float32_t *rgba32;
  uint64_t texel_bytes;
  bool8_t is_float;
  bool8_t srgb_hint;
  bool8_t source_ldr;
  bool8_t ldr_vertical_flip;
  bool8_t has_transparency;
  bool8_t alpha_mask;
};

struct VkrBakeTextureStore {
  VkrAllocator *allocator;
  VkrBakeTextureEntry *entries;
  uint32_t count;
  uint32_t capacity;
};

namespace {

constexpr uint32_t k_texture_store_initial_capacity = 64u;
constexpr uint32_t k_texture_max_dimension = 16384u;
constexpr uint32_t k_sha256_text_length = 72u;

struct Sha256 {
  uint32_t state[8];
  uint64_t bit_length;
  uint8_t block[64];
  uint32_t block_length;
};

uint32_t rotr32(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32u - bits));
}

void sha256_transform(Sha256 *hash, const uint8_t block[64]) {
  static const uint32_t constants[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
      0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
      0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
      0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
      0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0x81a664bcu, 0xc24b8b70u, 0xc76c51a3u,
      0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
      0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  uint32_t words[64];
  for (uint32_t i = 0u; i < 16u; ++i) {
    words[i] = ((uint32_t)block[i * 4u] << 24u) |
               ((uint32_t)block[i * 4u + 1u] << 16u) |
               ((uint32_t)block[i * 4u + 2u] << 8u) |
               (uint32_t)block[i * 4u + 3u];
  }
  for (uint32_t i = 16u; i < 64u; ++i) {
    const uint32_t s0 = rotr32(words[i - 15u], 7u) ^
                        rotr32(words[i - 15u], 18u) ^
                        (words[i - 15u] >> 3u);
    const uint32_t s1 = rotr32(words[i - 2u], 17u) ^
                        rotr32(words[i - 2u], 19u) ^
                        (words[i - 2u] >> 10u);
    words[i] = words[i - 16u] + s0 + words[i - 7u] + s1;
  }
  uint32_t a = hash->state[0], b = hash->state[1], c = hash->state[2];
  uint32_t d = hash->state[3], e = hash->state[4], f = hash->state[5];
  uint32_t g = hash->state[6], h = hash->state[7];
  for (uint32_t i = 0u; i < 64u; ++i) {
    const uint32_t sum1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
    const uint32_t choose = (e & f) ^ ((~e) & g);
    const uint32_t t1 = h + sum1 + choose + constants[i] + words[i];
    const uint32_t sum0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = sum0 + majority;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  hash->state[0] += a; hash->state[1] += b; hash->state[2] += c;
  hash->state[3] += d; hash->state[4] += e; hash->state[5] += f;
  hash->state[6] += g; hash->state[7] += h;
}

void sha256_begin(Sha256 *hash) {
  *hash = {{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
           0u, {}, 0u};
}

void sha256_update(Sha256 *hash, const uint8_t *bytes, uint64_t length) {
  while (length > 0u) {
    const uint32_t available = 64u - hash->block_length;
    const uint32_t copied = (uint32_t)((length < available) ? length : available);
    MemCopy(hash->block + hash->block_length, bytes, copied);
    hash->block_length += copied;
    bytes += copied;
    length -= copied;
    if (hash->block_length == 64u) {
      sha256_transform(hash, hash->block);
      hash->bit_length += 512u;
      hash->block_length = 0u;
    }
  }
}

void sha256_end(Sha256 *hash, char out[k_sha256_text_length]) {
  uint32_t index = hash->block_length;
  hash->block[index++] = 0x80u;
  if (index > 56u) {
    while (index < 64u) hash->block[index++] = 0u;
    sha256_transform(hash, hash->block);
    index = 0u;
  }
  while (index < 56u) hash->block[index++] = 0u;
  hash->bit_length += (uint64_t)hash->block_length * 8u;
  for (uint32_t byte = 0u; byte < 8u; ++byte)
    hash->block[63u - byte] = (uint8_t)(hash->bit_length >> (byte * 8u));
  sha256_transform(hash, hash->block);
  static const char hex[] = "0123456789abcdef";
  MemCopy(out, "sha256:", 7u);
  for (uint32_t word = 0u; word < 8u; ++word) {
    for (uint32_t byte = 0u; byte < 4u; ++byte) {
      const uint8_t value = (uint8_t)(hash->state[word] >> (24u - byte * 8u));
      out[7u + (word * 4u + byte) * 2u] = hex[value >> 4u];
      out[8u + (word * 4u + byte) * 2u] = hex[value & 15u];
    }
  }
  out[71] = '\0';
}

bool8_t finite_vec3(Vec3 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

bool8_t finite_vec4(Vec4 value) {
  return isfinite(value.x) && isfinite(value.y) && isfinite(value.z) &&
         isfinite(value.w);
}

float32_t saturate(float32_t value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

float32_t srgb_to_linear(float32_t value) {
  value = saturate(value);
  return value <= 0.04045f ? value / 12.92f
                            : powf((value + 0.055f) / 1.055f, 2.4f);
}

bool8_t checked_texel_bytes(uint32_t width, uint32_t height,
                            uint64_t bytes_per_texel, uint64_t *out_bytes) {
  if (width == 0u || height == 0u || width > k_texture_max_dimension ||
      height > k_texture_max_dimension ||
      width > UINT64_MAX / height ||
      (uint64_t)width * height > UINT64_MAX / bytes_per_texel) {
    return false_v;
  }
  *out_bytes = (uint64_t)width * height * bytes_per_texel;
  return true_v;
}

void texture_entry_release(VkrAllocator *allocator, VkrBakeTextureEntry *entry) {
  if (entry->path) {
    vkr_allocator_free(allocator, entry->path, strlen(entry->path) + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  if (entry->rgba8)
    vkr_allocator_free(allocator, entry->rgba8, entry->texel_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (entry->rgba32)
    vkr_allocator_free(allocator, entry->rgba32, entry->texel_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  *entry = {};
}

bool8_t store_grow(VkrBakeTextureStore *store) {
  if (store->count < store->capacity) return true_v;
  if (store->capacity > UINT32_MAX / 2u) return false_v;
  const uint32_t new_capacity = store->capacity ? store->capacity * 2u
                                                 : k_texture_store_initial_capacity;
  VkrBakeTextureEntry *new_entries = (VkrBakeTextureEntry *)vkr_allocator_alloc(
      store->allocator, (uint64_t)new_capacity * sizeof(*new_entries),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!new_entries) return false_v;
  MemZero(new_entries, (uint64_t)new_capacity * sizeof(*new_entries));
  if (store->entries) {
    MemCopy(new_entries, store->entries,
            (uint64_t)store->count * sizeof(*new_entries));
    vkr_allocator_free(store->allocator, store->entries,
                       (uint64_t)store->capacity * sizeof(*new_entries),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  store->entries = new_entries;
  store->capacity = new_capacity;
  return true_v;
}

std::string ascii_lower(std::string text) {
  for (char &ch : text)
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + ('a' - 'A'));
  return text;
}

std::string trim(std::string text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1u);
}

bool8_t parse_float(const std::string &text, float32_t *out_value) {
  if (!out_value || text.empty()) return false_v;
  char *end = nullptr;
  const float value = strtof(text.c_str(), &end);
  if (!end || *end != '\0' || !isfinite(value)) return false_v;
  *out_value = value;
  return true_v;
}

bool8_t parse_vec3(const std::string &text, Vec3 *out_value) {
  if (!out_value) return false_v;
  const size_t first = text.find(',');
  const size_t second = first == std::string::npos ? std::string::npos
                                                     : text.find(',', first + 1u);
  if (second == std::string::npos || text.find(',', second + 1u) != std::string::npos)
    return false_v;
  Vec3 value = {};
  if (!parse_float(trim(text.substr(0u, first)), &value.x) ||
      !parse_float(trim(text.substr(first + 1u, second - first - 1u)), &value.y) ||
      !parse_float(trim(text.substr(second + 1u)), &value.z))
    return false_v;
  *out_value = value;
  return true_v;
}

bool8_t parse_vec4(const std::string &text, Vec4 *out_value) {
  if (!out_value) return false_v;
  std::array<float32_t, 4u> components = {};
  size_t begin = 0u;
  for (uint32_t i = 0u; i < components.size(); ++i) {
    const size_t end = i + 1u == components.size() ? std::string::npos
                                                     : text.find(',', begin);
    if (end == std::string::npos && i + 1u != components.size()) return false_v;
    const std::string token = trim(text.substr(begin, end - begin));
    if (!parse_float(token, &components[i])) return false_v;
    begin = end == std::string::npos ? end : end + 1u;
  }
  if (begin != std::string::npos) return false_v;
  *out_value = vec4_new(components[0], components[1], components[2], components[3]);
  return true_v;
}

bool8_t parse_bool(const std::string &text, bool8_t *out_value) {
  const std::string value = ascii_lower(trim(text));
  if (value == "true" || value == "1" || value == "yes") { *out_value = true_v; return true_v; }
  if (value == "false" || value == "0" || value == "no") { *out_value = false_v; return true_v; }
  return false_v;
}

bool8_t parse_colorspace(const std::string &text, bool8_t *out_srgb) {
  const std::string value = ascii_lower(trim(text));
  if (value == "srgb") { *out_srgb = true_v; return true_v; }
  if (value == "linear") { *out_srgb = false_v; return true_v; }
  return false_v;
}

int32_t texture_slot_from_key(const std::string &key, bool8_t colorspace_key) {
  struct Alias { const char *name; VkrBakeMaterialTextureSlot slot; };
  static const Alias aliases[] = {
      {"base_color", VKR_BAKE_MATERIAL_TEXTURE_BASE_COLOR},
      {"diffuse", VKR_BAKE_MATERIAL_TEXTURE_BASE_COLOR},
      {"normal", VKR_BAKE_MATERIAL_TEXTURE_NORMAL},
      {"norm", VKR_BAKE_MATERIAL_TEXTURE_NORMAL},
      {"specular", VKR_BAKE_MATERIAL_TEXTURE_SPECULAR},
      {"emissive", VKR_BAKE_MATERIAL_TEXTURE_EMISSIVE},
      {"emission", VKR_BAKE_MATERIAL_TEXTURE_EMISSIVE},
      {"metallic_roughness", VKR_BAKE_MATERIAL_TEXTURE_METALLIC_ROUGHNESS},
      {"occlusion", VKR_BAKE_MATERIAL_TEXTURE_OCCLUSION},
      {"transmission", VKR_BAKE_MATERIAL_TEXTURE_TRANSMISSION},
      {"thickness", VKR_BAKE_MATERIAL_TEXTURE_THICKNESS},
      {"sheen_color", VKR_BAKE_MATERIAL_TEXTURE_SHEEN_COLOR},
      {"sheen_roughness", VKR_BAKE_MATERIAL_TEXTURE_SHEEN_ROUGHNESS},
      {"anisotropy", VKR_BAKE_MATERIAL_TEXTURE_ANISOTROPY},
      {"clearcoat", VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT},
      {"clearcoat_roughness", VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT_ROUGHNESS},
      {"clearcoat_normal", VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT_NORMAL},
  };
  const std::string suffix = colorspace_key ? "_colorspace" : "_texture";
  if (key.size() <= suffix.size() || key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0)
    return -1;
  const std::string prefix = key.substr(0u, key.size() - suffix.size());
  for (const Alias &alias : aliases)
    if (prefix == alias.name) return (int32_t)alias.slot;
  return -1;
}

std::string strip_resource_prefix(std::string path) {
  for (uint32_t pass = 0u; pass < 4u; ++pass) {
    size_t cursor = 0u;
    bool8_t removed = false_v;
    while (cursor < path.size()) {
      const bool8_t segment = cursor == 0u || path[cursor - 1u] == '/' || path[cursor - 1u] == '\\';
      if (!segment) { ++cursor; continue; }
      size_t digits_end = cursor;
      while (digits_end < path.size() && path[digits_end] >= '0' && path[digits_end] <= '9') ++digits_end;
      if (digits_end > cursor && digits_end < path.size() && path[digits_end] == '|') {
        path.erase(cursor, digits_end - cursor + 1u);
        removed = true_v;
        break;
      }
      ++cursor;
    }
    if (!removed) break;
  }
  return path;
}

struct TextureRequest {
  std::string path;
  bool8_t srgb;
  bool8_t colorspace_explicit;
  bool8_t source_only;
};

TextureRequest parse_texture_request(const std::string &value, bool8_t srgb) {
  TextureRequest request = {strip_resource_prefix(value), srgb, false_v, false_v};
  const size_t query = request.path.find('?');
  if (query == std::string::npos) return request;
  const std::string parameters = request.path.substr(query + 1u);
  request.path.erase(query);
  size_t cursor = 0u;
  while (cursor < parameters.size()) {
    const size_t end = parameters.find('&', cursor);
    const std::string pair = parameters.substr(cursor, end - cursor);
    const size_t equals = pair.find('=');
    if (equals != std::string::npos) {
      const std::string key = ascii_lower(pair.substr(0u, equals));
      const std::string field = ascii_lower(pair.substr(equals + 1u));
      if (key == "cs" && (field == "srgb" || field == "linear")) {
        request.srgb = field == "srgb";
        request.colorspace_explicit = true_v;
      }
      if (key == "source" && field == "only") request.source_only = true_v;
    }
    if (end == std::string::npos) break;
    cursor = end + 1u;
  }
  return request;
}

bool8_t layer_texture_intent_valid(const std::string &value, uint32_t slot) {
  const size_t query = value.find('?');
  if (query == std::string::npos) return true_v;
  size_t cursor = query + 1u;
  while (cursor < value.size()) {
    const size_t end = value.find('&', cursor);
    const std::string pair = ascii_lower(value.substr(cursor, end - cursor));
    const size_t equals = pair.find('=');
    if (equals != std::string::npos) {
      const std::string key = pair.substr(0u, equals);
      const std::string field = pair.substr(equals + 1u);
      if (key == "cs" && field != (slot == VKR_BAKE_MATERIAL_TEXTURE_SHEEN_COLOR ? "srgb" : "linear")) return false_v;
      if (key == "tc" && field != (slot == VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT_NORMAL
                                       ? "normal_rg" : slot == VKR_BAKE_MATERIAL_TEXTURE_SHEEN_COLOR ? "color_srgb" : "data_mask")) return false_v;
    }
    if (end == std::string::npos) break;
    cursor = end + 1u;
  }
  return true_v;
}

bool8_t read_file(const fs::path &path, std::vector<uint8_t> *out_bytes) {
  std::error_code error;
  if (!fs::is_regular_file(path, error) || error) return false_v;
  const uintmax_t size = fs::file_size(path, error);
  if (error || size == 0u || size > INT_MAX || size > SIZE_MAX) return false_v;
  out_bytes->resize((size_t)size);
  std::ifstream file(path, std::ios::binary);
  return file && file.read((char *)out_bytes->data(), (std::streamsize)out_bytes->size()) ? true_v : false_v;
}

float32_t half_to_float(uint16_t value) {
  const uint32_t sign = (uint32_t)(value & 0x8000u) << 16u;
  uint32_t exponent = (value >> 10u) & 31u;
  uint32_t mantissa = value & 1023u;
  uint32_t bits = 0u;
  if (exponent == 0u) {
    if (mantissa != 0u) {
      exponent = 113u;
      while ((mantissa & 1024u) == 0u) { mantissa <<= 1u; --exponent; }
      mantissa &= 1023u;
      bits = sign | (exponent << 23u) | (mantissa << 13u);
    } else bits = sign;
  } else if (exponent == 31u) {
    bits = sign | 0x7f800000u | (mantissa << 13u);
  } else bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
  float32_t result = 0.0f;
  MemCopy(&result, &bits, sizeof(result));
  return result;
}

bool8_t decode_source_image(VkrBakeTextureStore *store,
                            const std::vector<uint8_t> &encoded,
                            bool8_t ldr_vertical_flip,
                            VkrBakeTextureEntry *entry) {
  int width = 0, height = 0, channels = 0;
  if (stbi_is_hdr_from_memory(encoded.data(), (int)encoded.size())) {
    float *pixels = stbi_loadf_from_memory(encoded.data(), (int)encoded.size(),
                                           &width, &height, &channels, 4);
    uint64_t bytes = 0u;
    if (!pixels || width <= 0 || height <= 0 ||
        !checked_texel_bytes((uint32_t)width, (uint32_t)height, 16u, &bytes)) {
      stbi_image_free(pixels);
      return false_v;
    }
    entry->rgba32 = (float32_t *)vkr_allocator_alloc(store->allocator, bytes,
                                                       VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    if (!entry->rgba32) { stbi_image_free(pixels); return false_v; }
    for (uint64_t i = 0u; i < bytes / sizeof(float32_t); ++i) {
      if (!isfinite(pixels[i])) { stbi_image_free(pixels); return false_v; }
      entry->rgba32[i] = pixels[i];
    }
    stbi_image_free(pixels);
    entry->width = (uint32_t)width; entry->height = (uint32_t)height;
    entry->face_count = 1u;
    entry->texel_bytes = bytes; entry->is_float = true_v;
    return true_v;
  }
  stbi_set_flip_vertically_on_load_thread(ldr_vertical_flip ? 1 : 0);
  stbi_uc *pixels = stbi_load_from_memory(encoded.data(), (int)encoded.size(),
                                          &width, &height, &channels, 4);
  stbi_set_flip_vertically_on_load_thread(0);
  uint64_t bytes = 0u;
  if (!pixels || width <= 0 || height <= 0 ||
      !checked_texel_bytes((uint32_t)width, (uint32_t)height, 4u, &bytes)) {
    stbi_image_free(pixels);
    return false_v;
  }
  entry->rgba8 = (uint8_t *)vkr_allocator_alloc(store->allocator, bytes,
                                                  VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!entry->rgba8) { stbi_image_free(pixels); return false_v; }
  MemCopy(entry->rgba8, pixels, bytes);
  stbi_image_free(pixels);
  entry->width = (uint32_t)width; entry->height = (uint32_t)height;
  entry->face_count = 1u;
  entry->source_ldr = true_v;
  entry->ldr_vertical_flip = ldr_vertical_flip;
  entry->texel_bytes = bytes;
  return true_v;
}

void destroy_ktx(ktxTexture2 *texture) { ktxTexture_Destroy(ktxTexture(texture)); }

bool8_t decode_ktx2(VkrBakeTextureStore *store, const fs::path &path,
                    VkrBakeTextureEntry *entry) {
  ktxTexture2 *texture = nullptr;
  if (ktxTexture2_CreateFromNamedFile(path.string().c_str(),
                                      KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                      &texture) != KTX_SUCCESS || !texture)
    return false_v;
  ktxTexture *base = ktxTexture(texture);
  bool8_t success = false_v;
  do {
    if (base->numDimensions != 2u || base->numLayers != 1u || base->numFaces != 1u ||
        base->baseDepth != 1u || base->baseWidth == 0u || base->baseHeight == 0u ||
        base->baseWidth > k_texture_max_dimension || base->baseHeight > k_texture_max_dimension)
      break;
    unsigned int hint_size = 0u;
    void *hint = nullptr;
    if (ktxHashList_FindValue(&texture->kvDataHead, "vkr.colorspace_hint",
                              &hint_size, &hint) == KTX_SUCCESS && hint &&
        hint_size == 5u && memcmp(hint, "srgb", 5u) == 0)
      entry->srgb_hint = true_v;
    if (ktxTexture2_NeedsTranscoding(texture) &&
        ktxTexture2_TranscodeBasis(texture, KTX_TTF_RGBA32, 0u) != KTX_SUCCESS)
      break;
    ktx_size_t offset = 0u;
    if (ktxTexture_GetImageOffset(base, 0u, 0u, 0u, &offset) != KTX_SUCCESS)
      break;
    const uint8_t *data = ktxTexture_GetData(base);
    const ktx_size_t data_size = ktxTexture_GetDataSize(base);
    const ktx_size_t image_size = ktxTexture_GetImageSize(base, 0u);
    if (!data || offset > data_size || image_size > data_size - offset) break;
    uint64_t expected = 0u;
    if (texture->vkFormat == VK_FORMAT_R16G16B16A16_SFLOAT) {
      if (!checked_texel_bytes(base->baseWidth, base->baseHeight, 16u, &expected) || image_size != expected)
        break;
      entry->rgba32 = (float32_t *)vkr_allocator_alloc(store->allocator, expected,
                                                         VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
      if (!entry->rgba32) break;
      const uint8_t *source = data + offset;
      bool8_t finite = true_v;
      for (uint64_t i = 0u; i < expected / 2u; ++i) {
        const uint16_t half = (uint16_t)source[i * 2u] | ((uint16_t)source[i * 2u + 1u] << 8u);
        entry->rgba32[i] = half_to_float(half);
        if (!isfinite(entry->rgba32[i])) { finite = false_v; break; }
      }
      if (!finite) break;
      entry->is_float = true_v;
      entry->texel_bytes = expected;
    } else {
      if (texture->vkFormat != VK_FORMAT_R8G8B8A8_UNORM &&
          texture->vkFormat != VK_FORMAT_R8G8B8A8_SRGB &&
          texture->vkFormat != 0u)
        break;
      if (!checked_texel_bytes(base->baseWidth, base->baseHeight, 4u, &expected) || image_size != expected)
        break;
      entry->rgba8 = (uint8_t *)vkr_allocator_alloc(store->allocator, expected,
                                                      VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
      if (!entry->rgba8) break;
      MemCopy(entry->rgba8, data + offset, expected);
      entry->texel_bytes = expected;
    }
    entry->width = base->baseWidth;
    entry->height = base->baseHeight;
    entry->face_count = 1u;
    success = true_v;
  } while (false);
  destroy_ktx(texture);
  return success;
}

bool8_t decode_ktx2_cube_rgba16f(VkrBakeTextureStore *store,
                                      const fs::path &path,
                                      VkrBakeTextureEntry *entry) {
  ktxTexture2 *texture = nullptr;
  if (ktxTexture2_CreateFromNamedFile(path.string().c_str(),
                                      KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                      &texture) != KTX_SUCCESS || !texture)
    return false_v;
  ktxTexture *base = ktxTexture(texture);
  bool8_t success = false_v;
  do {
    if (base->numDimensions != 2u || base->numLayers != 1u ||
        base->numFaces != 6u || !base->isCubemap || base->baseDepth != 1u ||
        base->baseWidth == 0u || base->baseWidth != base->baseHeight ||
        base->baseWidth > k_texture_max_dimension ||
        texture->vkFormat != VK_FORMAT_R16G16B16A16_SFLOAT ||
        ktxTexture2_NeedsTranscoding(texture))
      break;
    uint64_t face_bytes = 0u;
    if (!checked_texel_bytes(base->baseWidth, base->baseHeight, 16u,
                             &face_bytes) || face_bytes > UINT64_MAX / 6u)
      break;
    entry->rgba32 = (float32_t *)vkr_allocator_alloc(
        store->allocator, face_bytes * 6u, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
    if (!entry->rgba32)
      break;
    const uint8_t *data = ktxTexture_GetData(base);
    const ktx_size_t data_size = ktxTexture_GetDataSize(base);
    const uint64_t source_face_bytes = face_bytes / 2u;
    if (!data)
      break;
    bool8_t finite = true_v;
    const uint64_t component_count = face_bytes / sizeof(float32_t);
    for (uint32_t face = 0u; face < 6u && finite; ++face) {
      ktx_size_t offset = 0u;
      const ktx_size_t image_size = ktxTexture_GetImageSize(base, 0u);
      if (ktxTexture_GetImageOffset(base, 0u, 0u, face, &offset) != KTX_SUCCESS ||
          image_size != source_face_bytes || offset > data_size ||
          source_face_bytes > data_size - offset) {
        finite = false_v;
        break;
      }
      const uint8_t *source = data + offset;
      for (uint64_t component = 0u; component < component_count; ++component) {
        const uint16_t half = (uint16_t)source[component * 2u] |
                              ((uint16_t)source[component * 2u + 1u] << 8u);
        const float32_t value = half_to_float(half);
        if (!isfinite(value)) {
          finite = false_v;
          break;
        }
        entry->rgba32[(uint64_t)face * component_count + component] = value;
      }
    }
    if (!finite)
      break;
    entry->width = base->baseWidth;
    entry->height = base->baseHeight;
    entry->face_count = 6u;
    entry->texel_bytes = face_bytes * 6u;
    entry->is_float = true_v;
    success = true_v;
  } while (false);
  destroy_ktx(texture);
  return success;
}

bool8_t texture_store_load_cube_rgba16f(VkrBakeTextureStore *store,
                                        const char *path,
                                        uint32_t *out_index,
                                        VkrBakeMaterialError *out_error) {
  std::error_code error;
  const fs::path selected(path);
  if (!fs::is_regular_file(selected, error) || error) {
    *out_error = VKR_BAKE_MATERIAL_ERROR_IO;
    return false_v;
  }
  const std::string canonical = fs::absolute(selected, error).lexically_normal().string();
  if (error || canonical.empty()) {
    *out_error = VKR_BAKE_MATERIAL_ERROR_IO;
    return false_v;
  }
  for (uint32_t i = 0u; i < store->count; ++i) {
    if (strcmp(store->entries[i].path, canonical.c_str()) == 0 &&
        store->entries[i].face_count == 6u) {
      *out_index = i;
      return true_v;
    }
  }
  if (!store_grow(store)) {
    *out_error = VKR_BAKE_MATERIAL_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  VkrBakeTextureEntry entry = {};
  entry.path = (char *)vkr_allocator_alloc(store->allocator,
                                            canonical.size() + 1u,
                                            VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!entry.path) {
    *out_error = VKR_BAKE_MATERIAL_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemCopy(entry.path, canonical.c_str(), canonical.size() + 1u);
  std::vector<uint8_t> bytes;
  if (!read_file(selected, &bytes)) {
    texture_entry_release(store->allocator, &entry);
    *out_error = VKR_BAKE_MATERIAL_ERROR_IO;
    return false_v;
  }
  Sha256 hash = {};
  sha256_begin(&hash);
  sha256_update(&hash, bytes.data(), bytes.size());
  sha256_end(&hash, entry.sha256);
  entry.byte_count = bytes.size();
  if (!decode_ktx2_cube_rgba16f(store, selected, &entry)) {
    texture_entry_release(store->allocator, &entry);
    *out_error = VKR_BAKE_MATERIAL_ERROR_TEXTURE;
    return false_v;
  }
  store->entries[store->count] = entry;
  *out_index = store->count++;
  return true_v;
}

void analyze_alpha(VkrBakeTextureEntry *entry) {
  entry->has_transparency = false_v;
  entry->alpha_mask = true_v;
  const uint64_t pixels = (uint64_t)entry->width * entry->height *
                          entry->face_count;
  for (uint64_t i = 0u; i < pixels; ++i) {
    const float32_t alpha = entry->is_float ? entry->rgba32[i * 4u + 3u]
                                            : entry->rgba8[i * 4u + 3u] / 255.0f;
    if (alpha < 0.999f) entry->has_transparency = true_v;
    if (alpha > 0.001f && alpha < 0.999f) entry->alpha_mask = false_v;
  }
}

bool8_t texture_store_find_or_load(VkrBakeTextureStore *store,
                                   const TextureRequest &request,
                                   const fs::path &material_directory,
                                   bool8_t ldr_vertical_flip,
                                   bool8_t source_srgb_hint,
                                   uint32_t *out_index,
                                   VkrBakeMaterialError *out_error) {
  fs::path source(request.path);
  std::error_code error;
  if (!source.is_absolute() && !fs::exists(source, error)) {
    error.clear();
    const fs::path local = material_directory / source;
    if (fs::exists(local, error)) source = local;
  }
  error.clear();
  fs::path selected = source;
  const bool8_t direct_vkt = ascii_lower(source.extension().string()) == ".vkt";
  if (!direct_vkt && !request.source_only) {
    const fs::path sidecar = source.string() + ".vkt";
    if (fs::is_regular_file(sidecar, error) && !error) selected = sidecar;
  }
  if (!fs::is_regular_file(selected, error) || error) {
    *out_error = VKR_BAKE_MATERIAL_ERROR_IO;
    return false_v;
  }
  const std::string canonical = fs::absolute(selected, error).lexically_normal().string();
  if (error || canonical.empty()) { *out_error = VKR_BAKE_MATERIAL_ERROR_IO; return false_v; }
  for (uint32_t i = 0u; i < store->count; ++i) {
    if (strcmp(store->entries[i].path, canonical.c_str()) == 0 &&
        store->entries[i].face_count == 1u &&
        (!store->entries[i].source_ldr ||
         store->entries[i].ldr_vertical_flip == ldr_vertical_flip)) {
      *out_index = i;
      return true_v;
    }
  }
  if (!store_grow(store)) { *out_error = VKR_BAKE_MATERIAL_ERROR_OUT_OF_MEMORY; return false_v; }
  VkrBakeTextureEntry entry = {};
  entry.path = (char *)vkr_allocator_alloc(store->allocator, canonical.size() + 1u,
                                            VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!entry.path) { *out_error = VKR_BAKE_MATERIAL_ERROR_OUT_OF_MEMORY; return false_v; }
  MemCopy(entry.path, canonical.c_str(), canonical.size() + 1u);
  std::vector<uint8_t> bytes;
  if (!read_file(selected, &bytes)) { texture_entry_release(store->allocator, &entry); *out_error = VKR_BAKE_MATERIAL_ERROR_IO; return false_v; }
  Sha256 hash = {};
  sha256_begin(&hash); sha256_update(&hash, bytes.data(), bytes.size()); sha256_end(&hash, entry.sha256);
  entry.byte_count = bytes.size();
  const bool8_t decoded = direct_vkt || ascii_lower(selected.extension().string()) == ".vkt"
                              ? decode_ktx2(store, selected, &entry)
                              : decode_source_image(store, bytes,
                                                    ldr_vertical_flip, &entry);
  if (!(direct_vkt || ascii_lower(selected.extension().string()) == ".vkt"))
    entry.srgb_hint = source_srgb_hint;
  if (!decoded) { texture_entry_release(store->allocator, &entry); *out_error = VKR_BAKE_MATERIAL_ERROR_TEXTURE; return false_v; }
  analyze_alpha(&entry);
  store->entries[store->count] = entry;
  *out_index = store->count++;
  return true_v;
}

Vec4 texture_fetch(const VkrBakeTextureEntry &entry, uint32_t face,
                   uint32_t x, uint32_t y, bool8_t srgb) {
  const uint64_t index = (((uint64_t)face * entry.height + y) * entry.width + x) * 4u;
  Vec4 value = entry.is_float
                   ? vec4_new(entry.rgba32[index], entry.rgba32[index + 1u], entry.rgba32[index + 2u], entry.rgba32[index + 3u])
                   : vec4_new(entry.rgba8[index] / 255.0f, entry.rgba8[index + 1u] / 255.0f,
                              entry.rgba8[index + 2u] / 255.0f, entry.rgba8[index + 3u] / 255.0f);
  if (srgb) { value.x = srgb_to_linear(value.x); value.y = srgb_to_linear(value.y); value.z = srgb_to_linear(value.z); }
  return value;
}

Vec4 texture_sample(const VkrBakeTextureStore *store, VkrBakeMaterialTextureRef ref, Vec2 uv) {
  const VkrBakeTextureEntry &entry = store->entries[ref.texture_index];
  const float32_t u = isfinite(uv.x) ? uv.x - floorf(uv.x) : 0.0f;
  const float32_t v = isfinite(uv.y) ? uv.y - floorf(uv.y) : 0.0f;
  const float32_t x = u * entry.width - 0.5f;
  const float32_t y = v * entry.height - 0.5f;
  const int32_t x0 = (int32_t)floorf(x), y0 = (int32_t)floorf(y);
  const float32_t tx = x - x0, ty = y - y0;
  const uint32_t x00 = (uint32_t)((x0 % (int32_t)entry.width + (int32_t)entry.width) % (int32_t)entry.width);
  const uint32_t y00 = (uint32_t)((y0 % (int32_t)entry.height + (int32_t)entry.height) % (int32_t)entry.height);
  const uint32_t x10 = (x00 + 1u) % entry.width, y01 = (y00 + 1u) % entry.height;
  const Vec4 a = texture_fetch(entry, 0u, x00, y00, ref.srgb);
  const Vec4 b = texture_fetch(entry, 0u, x10, y00, ref.srgb);
  const Vec4 c = texture_fetch(entry, 0u, x00, y01, ref.srgb);
  const Vec4 d = texture_fetch(entry, 0u, x10, y01, ref.srgb);
  const Vec4 top = vec4_new(a.x + (b.x-a.x)*tx, a.y + (b.y-a.y)*tx, a.z + (b.z-a.z)*tx, a.w + (b.w-a.w)*tx);
  const Vec4 bottom = vec4_new(c.x + (d.x-c.x)*tx, c.y + (d.y-c.y)*tx, c.z + (d.z-c.z)*tx, c.w + (d.w-c.w)*tx);
  return vec4_new(top.x + (bottom.x-top.x)*ty, top.y + (bottom.y-top.y)*ty,
                  top.z + (bottom.z-top.z)*ty, top.w + (bottom.w-top.w)*ty);
}

Vec4 texture_sample_environment_entry(const VkrBakeTextureEntry &entry,
                                      uint32_t face, Vec2 uv,
                                      bool8_t repeat_u) {
  if (face >= entry.face_count || !isfinite(uv.x) || !isfinite(uv.y))
    return vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
  const float32_t u = repeat_u ? uv.x - floorf(uv.x)
                               : fmaxf(0.0f, fminf(uv.x, 1.0f));
  const float32_t v = fmaxf(0.0f, fminf(uv.y, 1.0f));
  const float32_t x = u * entry.width - 0.5f;
  const float32_t y = v * entry.height - 0.5f;
  const int32_t x0 = (int32_t)floorf(x);
  const int32_t y0 = (int32_t)floorf(y);
  const float32_t tx = x - x0;
  const float32_t ty = y - y0;
  const int32_t width = (int32_t)entry.width;
  const int32_t height = (int32_t)entry.height;
  const uint32_t x00 = repeat_u ? (uint32_t)((x0 % width + width) % width)
                                : (uint32_t)std::max(0, std::min(x0, width - 1));
  const uint32_t x10 = repeat_u ? (x00 + 1u) % entry.width
                                : (uint32_t)std::min((int32_t)x00 + 1, width - 1);
  const uint32_t y00 = (uint32_t)std::max(0, std::min(y0, height - 1));
  const uint32_t y01 = (uint32_t)std::min((int32_t)y00 + 1, height - 1);
  const Vec4 a = texture_fetch(entry, face, x00, y00, entry.srgb_hint);
  const Vec4 b = texture_fetch(entry, face, x10, y00, entry.srgb_hint);
  const Vec4 c = texture_fetch(entry, face, x00, y01, entry.srgb_hint);
  const Vec4 d = texture_fetch(entry, face, x10, y01, entry.srgb_hint);
  const Vec4 top = vec4_new(a.x + (b.x - a.x) * tx, a.y + (b.y - a.y) * tx,
                            a.z + (b.z - a.z) * tx, a.w + (b.w - a.w) * tx);
  const Vec4 bottom = vec4_new(c.x + (d.x - c.x) * tx,
                               c.y + (d.y - c.y) * tx,
                               c.z + (d.z - c.z) * tx,
                               c.w + (d.w - c.w) * tx);
  return vec4_new(top.x + (bottom.x - top.x) * ty,
                  top.y + (bottom.y - top.y) * ty,
                  top.z + (bottom.z - top.z) * ty,
                  top.w + (bottom.w - top.w) * ty);
}

void material_defaults(VkrBakeMaterial *material) {
  *material = {};
  material->alpha_mode = VKR_BAKE_MATERIAL_ALPHA_OPAQUE;
  material->base_color = vec4_new(1, 1, 1, 1);
  material->metallic = 1.0f; material->roughness = 1.0f;
  material->normal_scale = 1.0f; material->occlusion_strength = 1.0f;
  material->clearcoat_normal_scale = 1.0f;
  material->diffuse_transmission_color = vec3_new(1, 1, 1);
  material->dielectric_specular = vec3_new(0.04f, 0.04f, 0.04f);
  material->ior = 1.5f; material->attenuation_color = vec3_new(1, 1, 1);
}

} // namespace

extern "C" VkrBakeTextureStore *vkr_bake_texture_store_create(VkrAllocator *allocator) {
  if (!allocator) return nullptr;
  VkrBakeTextureStore *store = (VkrBakeTextureStore *)vkr_allocator_alloc(
      allocator, sizeof(*store), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!store) return nullptr;
  *store = {.allocator = allocator};
  return store;
}

extern "C" void vkr_bake_texture_store_release(VkrBakeTextureStore *store) {
  if (!store) return;
  for (uint32_t i = 0u; i < store->count; ++i) texture_entry_release(store->allocator, &store->entries[i]);
  if (store->entries) vkr_allocator_free(store->allocator, store->entries,
                                         (uint64_t)store->capacity * sizeof(*store->entries),
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_free(store->allocator, store, sizeof(*store), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
}

extern "C" uint32_t vkr_bake_texture_store_count(const VkrBakeTextureStore *store) {
  return store ? store->count : 0u;
}

extern "C" bool8_t vkr_bake_texture_store_dependency(const VkrBakeTextureStore *store,
                                                        uint32_t texture_index,
                                                        const char **out_path,
                                                        const char **out_sha256,
                                                        uint64_t *out_byte_count) {
  if (!store || texture_index >= store->count || !out_path || !out_sha256 || !out_byte_count) return false_v;
  const VkrBakeTextureEntry &entry = store->entries[texture_index];
  *out_path = entry.path; *out_sha256 = entry.sha256; *out_byte_count = entry.byte_count;
  return true_v;
}

extern "C" bool8_t vkr_bake_texture_store_load_environment_2d(
    VkrBakeTextureStore *store, const char *path, bool8_t srgb,
    uint32_t *out_texture_index, VkrBakeMaterialError *out_error) {
  if (out_error)
    *out_error = VKR_BAKE_MATERIAL_ERROR_NONE;
  if (!store || !path || path[0] == '\0' || !out_texture_index) {
    if (out_error)
      *out_error = VKR_BAKE_MATERIAL_ERROR_INVALID_ARGUMENT;
    return false_v;
  }
  VkrBakeMaterialError error = VKR_BAKE_MATERIAL_ERROR_NONE;
  const TextureRequest request = {strip_resource_prefix(path), srgb, true_v,
                                  true_v};
  if (!texture_store_find_or_load(store, request, {}, false_v, srgb,
                                  out_texture_index, &error)) {
    if (out_error)
      *out_error = error;
    return false_v;
  }
  return true_v;
}

extern "C" bool8_t vkr_bake_texture_store_load_environment_equirect(
    VkrBakeTextureStore *store, const char *path, uint32_t *out_texture_index,
    VkrBakeMaterialError *out_error) {
  if (!vkr_bake_texture_store_load_environment_2d(
          store, path, false_v, out_texture_index, out_error))
    return false_v;
  const VkrBakeTextureEntry &entry = store->entries[*out_texture_index];
  if (entry.face_count != 1u || !entry.is_float ||
      entry.width != entry.height * 2u) {
    if (out_error)
      *out_error = VKR_BAKE_MATERIAL_ERROR_UNSUPPORTED;
    return false_v;
  }
  return true_v;
}

extern "C" bool8_t vkr_bake_texture_store_load_environment_cube_rgba16f(
    VkrBakeTextureStore *store, const char *path, uint32_t *out_texture_index,
    VkrBakeMaterialError *out_error) {
  if (out_error)
    *out_error = VKR_BAKE_MATERIAL_ERROR_NONE;
  if (!store || !path || path[0] == '\0' || !out_texture_index) {
    if (out_error)
      *out_error = VKR_BAKE_MATERIAL_ERROR_INVALID_ARGUMENT;
    return false_v;
  }
  VkrBakeMaterialError error = VKR_BAKE_MATERIAL_ERROR_NONE;
  const bool8_t success = texture_store_load_cube_rgba16f(
      store, path, out_texture_index, &error);
  if (!success && out_error)
    *out_error = error;
  return success;
}

extern "C" Vec4 vkr_bake_texture_store_sample_environment_2d(
    const VkrBakeTextureStore *store, uint32_t texture_index, Vec2 uv,
    bool8_t repeat_u) {
  if (!store || texture_index >= store->count ||
      store->entries[texture_index].face_count != 1u)
    return vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
  return texture_sample_environment_entry(store->entries[texture_index], 0u,
                                          uv, repeat_u);
}

extern "C" Vec4 vkr_bake_texture_store_sample_environment_cube_face(
    const VkrBakeTextureStore *store, uint32_t texture_index, uint32_t face,
    Vec2 uv) {
  if (!store || texture_index >= store->count ||
      store->entries[texture_index].face_count != 6u)
    return vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
  return texture_sample_environment_entry(store->entries[texture_index], face,
                                          uv, false_v);
}

extern "C" bool8_t vkr_bake_material_load(VkrBakeTextureStore *store,
                                            const char *material_path,
                                            VkrBakeMaterial *out_material,
                                            VkrBakeMaterialError *out_error) {
  if (out_error) *out_error = VKR_BAKE_MATERIAL_ERROR_NONE;
  if (!store || !material_path || !out_material) { if (out_error) *out_error = VKR_BAKE_MATERIAL_ERROR_INVALID_ARGUMENT; return false_v; }
  std::ifstream file(material_path);
  if (!file) { if (out_error) *out_error = VKR_BAKE_MATERIAL_ERROR_IO; return false_v; }
  VkrBakeMaterial material = {};
  material_defaults(&material);
  std::array<std::string, VKR_BAKE_MATERIAL_TEXTURE_COUNT> texture_paths;
  std::array<bool8_t, VKR_BAKE_MATERIAL_TEXTURE_COUNT> texture_srgb = {};
  texture_srgb[VKR_BAKE_MATERIAL_TEXTURE_SHEEN_COLOR] = true_v;
  std::array<bool8_t, VKR_BAKE_MATERIAL_TEXTURE_COUNT> texture_colorspace_explicit = {};
  // Preserve legacy PBR-field normalization while tracking the final type
  // selected by key order for the diffuse-sheet material contract.
  bool8_t cutout_enabled = false_v, pbr = false_v, final_pbr = false_v;
  std::string line;
  while (std::getline(file, line)) {
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    const size_t equal = trimmed.find('=');
    if (equal == std::string::npos || equal == 0u || equal + 1u >= trimmed.size()) continue;
    const std::string key = ascii_lower(trim(trimmed.substr(0u, equal)));
    const std::string value = trim(trimmed.substr(equal + 1u));
    const int32_t colorspace_slot = texture_slot_from_key(key, true_v);
    if (colorspace_slot >= 0) { if (!parse_colorspace(value, &texture_srgb[(uint32_t)colorspace_slot])) goto parse_error; texture_colorspace_explicit[(uint32_t)colorspace_slot] = true_v; continue; }
    const int32_t texture_slot = texture_slot_from_key(key, false_v);
    if (texture_slot >= 0) { if (value.empty()) goto parse_error; texture_paths[(uint32_t)texture_slot] = value; if ((uint32_t)texture_slot >= VKR_BAKE_MATERIAL_TEXTURE_METALLIC_ROUGHNESS) { pbr = true_v; final_pbr = true_v; } continue; }
    if (key == "name" || key == "shader" || key == "pipeline") continue;
    if (key == "type") { const std::string type = ascii_lower(value); if (type == "pbr") { pbr = true_v; final_pbr = true_v; } else if (type == "phong") final_pbr = false_v; else goto parse_error; continue; }
    if (key == "base_color" || key.find("diffuse_color") != std::string::npos) { if (!parse_vec4(value, &material.base_color)) goto parse_error; if (key == "base_color") { pbr = true_v; final_pbr = true_v; } continue; }
    if (key.find("specular_color") != std::string::npos) { Vec4 ignored = {}; if (!parse_vec4(value, &ignored)) goto parse_error; continue; }
    if (key.find("shininess") != std::string::npos) { float32_t ignored = 0.0f; if (!parse_float(value, &ignored)) goto parse_error; continue; }
    if (key == "emissive_factor" || key.find("emission_color") != std::string::npos) { if (!parse_vec3(value, &material.emissive_factor)) goto parse_error; if (key == "emissive_factor") { pbr = true_v; final_pbr = true_v; } continue; }
    if (key == "metallic") { if (!parse_float(value, &material.metallic)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "roughness") { if (!parse_float(value, &material.roughness)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "normal_scale") { if (!parse_float(value, &material.normal_scale)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "occlusion_strength") { if (!parse_float(value, &material.occlusion_strength)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "dielectric_specular") { if (!parse_vec3(value, &material.dielectric_specular)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "anisotropy_strength") { if (!parse_float(value, &material.anisotropy_strength)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "anisotropy_rotation") { if (!parse_float(value, &material.anisotropy_rotation)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "subsurface_strength") { if (!parse_float(value, &material.subsurface_strength)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "subsurface_profile") {
      float32_t index;
      if (!parse_float(value, &index) || !isfinite(index) || index < 0 || index >= 8 || floorf(index) != index) goto parse_error;
      material.subsurface_profile = (uint32_t)index; pbr = true_v; final_pbr = true_v; continue;
    }
    if (key.rfind("subsurface_", 0) == 0) goto parse_error;
    if (key == "diffuse_transmission_strength") { if (!parse_float(value, &material.diffuse_transmission_strength)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "diffuse_transmission_color") { if (!parse_vec3(value, &material.diffuse_transmission_color)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "sheen_color") { if (!parse_vec3(value, &material.sheen_color)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "sheen_roughness") { if (!parse_float(value, &material.sheen_roughness)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "clearcoat_factor") { if (!parse_float(value, &material.clearcoat_factor)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "clearcoat_roughness") { if (!parse_float(value, &material.clearcoat_roughness)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "clearcoat_normal_scale") { if (!parse_float(value, &material.clearcoat_normal_scale)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "transmission_factor") { if (!parse_float(value, &material.transmission_factor)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "ior") { if (!parse_float(value, &material.ior)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "thickness_factor") { if (!parse_float(value, &material.thickness_factor)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "attenuation_color") { if (!parse_vec3(value, &material.attenuation_color)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "attenuation_distance") { if (!parse_float(value, &material.attenuation_distance)) goto parse_error; pbr = true_v; final_pbr = true_v; continue; }
    if (key == "temporal_reactivity") { float32_t ignored = 0.0f; if (!parse_float(value, &ignored)) goto parse_error; continue; }
    if (key == "alpha_mode") { const std::string mode = ascii_lower(value); if (mode == "opaque") material.alpha_mode = VKR_BAKE_MATERIAL_ALPHA_OPAQUE; else if (mode == "mask" || mode == "cutout") material.alpha_mode = VKR_BAKE_MATERIAL_ALPHA_CUTOUT; else if (mode == "blend") material.alpha_mode = VKR_BAKE_MATERIAL_ALPHA_BLEND; else goto parse_error; material.alpha_mode_explicit = true_v; continue; }
    if (key == "double_sided") { if (!parse_bool(value, &material.double_sided)) goto parse_error; continue; }
    if (key.find("alpha_cutoff") != std::string::npos) { if (!parse_float(value, &material.alpha_cutoff)) goto parse_error; material.alpha_cutoff = material.alpha_cutoff < 0.0f ? 0.0f : material.alpha_cutoff; continue; }
    if (key.find("cutout") != std::string::npos) { if (!parse_bool(value, &cutout_enabled)) goto parse_error; continue; }
    if (out_error) *out_error = VKR_BAKE_MATERIAL_ERROR_UNSUPPORTED;
    return false_v;
  }
  if (material.alpha_mode == VKR_BAKE_MATERIAL_ALPHA_CUTOUT && material.alpha_cutoff <= 0.0f)
    material.alpha_cutoff = 0.1f;
  (void)cutout_enabled;
  for (uint32_t slot = 0u; slot < texture_paths.size(); ++slot) {
    if (texture_paths[slot].empty()) continue;
    TextureRequest request = parse_texture_request(texture_paths[slot], texture_srgb[slot]);
    if (texture_colorspace_explicit[slot]) request.colorspace_explicit = true_v;
    if (slot >= VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT) {
      const bool8_t expected_srgb = slot == VKR_BAKE_MATERIAL_TEXTURE_SHEEN_COLOR;
      if (request.srgb != expected_srgb || texture_srgb[slot] != expected_srgb ||
          !layer_texture_intent_valid(texture_paths[slot], slot)) goto parse_error;
      request.colorspace_explicit = true_v;
    }
    uint32_t index = 0u;
    VkrBakeMaterialError error = VKR_BAKE_MATERIAL_ERROR_NONE;
    if (!texture_store_find_or_load(store, request, fs::path(material_path).parent_path(),
                                    true_v, false_v, &index, &error)) {
      if (out_error) *out_error = error;
      return false_v;
    }
    material.textures[slot] = {.texture_index = index, .present = true_v,
                               .srgb = request.colorspace_explicit ? request.srgb : store->entries[index].srgb_hint};
  }
  if (!pbr) {
    /* The native publisher copies these two Phong values over PBR defaults. */
    material.metallic = 1.0f;
    material.roughness = 1.0f;
  }
  if (!material.alpha_mode_explicit) {
    const VkrBakeMaterialTextureRef base = material.textures[VKR_BAKE_MATERIAL_TEXTURE_BASE_COLOR];
    if (material.base_color.w < 0.999f) material.alpha_mode = VKR_BAKE_MATERIAL_ALPHA_BLEND;
    else if (base.present && store->entries[base.texture_index].has_transparency && material.alpha_cutoff > 0.0f)
      material.alpha_mode = store->entries[base.texture_index].alpha_mask ? VKR_BAKE_MATERIAL_ALPHA_CUTOUT : VKR_BAKE_MATERIAL_ALPHA_BLEND;
  }
  if (!finite_vec4(material.base_color) || !finite_vec3(material.emissive_factor) ||
      !finite_vec3(material.dielectric_specular) || !finite_vec3(material.attenuation_color) ||
      !isfinite(material.metallic) || !isfinite(material.roughness) || !isfinite(material.normal_scale) ||
      !isfinite(material.occlusion_strength) || !isfinite(material.transmission_factor) ||
      !isfinite(material.subsurface_strength) || material.subsurface_strength < 0.0f || material.subsurface_strength > 1.0f ||
      material.subsurface_profile >= 8u ||
      !isfinite(material.diffuse_transmission_strength) || material.diffuse_transmission_strength < 0.0f || material.diffuse_transmission_strength > 1.0f ||
      !finite_vec3(material.diffuse_transmission_color) ||
      material.diffuse_transmission_color.x < 0.0f || material.diffuse_transmission_color.x > 1.0f ||
      material.diffuse_transmission_color.y < 0.0f || material.diffuse_transmission_color.y > 1.0f ||
      material.diffuse_transmission_color.z < 0.0f || material.diffuse_transmission_color.z > 1.0f ||
      !isfinite(material.anisotropy_strength) || material.anisotropy_strength < 0.0f || material.anisotropy_strength > 1.0f ||
      !isfinite(material.anisotropy_rotation) ||
      !isfinite(material.clearcoat_factor) || material.clearcoat_factor < 0.0f || material.clearcoat_factor > 1.0f ||
      !isfinite(material.clearcoat_roughness) || material.clearcoat_roughness < 0.0f || material.clearcoat_roughness > 1.0f ||
      !isfinite(material.clearcoat_normal_scale) ||
      !isfinite(material.sheen_color.x) || material.sheen_color.x < 0.0f || material.sheen_color.x > 1.0f ||
      !isfinite(material.sheen_color.y) || material.sheen_color.y < 0.0f || material.sheen_color.y > 1.0f ||
      !isfinite(material.sheen_color.z) || material.sheen_color.z < 0.0f || material.sheen_color.z > 1.0f ||
      !isfinite(material.sheen_roughness) || material.sheen_roughness < 0.0f || material.sheen_roughness > 1.0f ||
      !isfinite(material.ior) || !isfinite(material.thickness_factor) || !isfinite(material.attenuation_distance)) goto parse_error;
  if (material.anisotropy_strength > 0.0f && material.transmission_factor > 0.0f) {
    if (out_error) *out_error = VKR_BAKE_MATERIAL_ERROR_UNSUPPORTED;
    return false_v;
  }
  if (material.subsurface_strength > 0.0f &&
      (!final_pbr || material.alpha_mode == VKR_BAKE_MATERIAL_ALPHA_BLEND ||
       material.transmission_factor > 0.0f || material.thickness_factor > 0.0f ||
       material.diffuse_transmission_strength > 0.0f)) {
    if (out_error) *out_error = VKR_BAKE_MATERIAL_ERROR_UNSUPPORTED;
    return false_v;
  }
  if (material.diffuse_transmission_strength > 0.0f &&
      (!final_pbr || material.alpha_mode == VKR_BAKE_MATERIAL_ALPHA_BLEND ||
       material.transmission_factor > 0.0f || material.thickness_factor > 0.0f)) {
    if (out_error) *out_error = VKR_BAKE_MATERIAL_ERROR_UNSUPPORTED;
    return false_v;
  }
  material.anisotropy_rotation = remainderf(material.anisotropy_rotation, 6.283185307179586f);
  *out_material = material;
  return true_v;

parse_error:
  if (out_error) *out_error = VKR_BAKE_MATERIAL_ERROR_PARSE;
  return false_v;
}

extern "C" void vkr_bake_material_sample(const VkrBakeTextureStore *store,
                                           const VkrBakeMaterial *material,
                                           Vec2 uv, Vec4 vertex_color,
                                           VkrBakeMaterialSample *out_sample) {
  VkrBakeMaterialSample result = {
      .base_color = vec4_new(material->base_color.x * vertex_color.x, material->base_color.y * vertex_color.y,
                             material->base_color.z * vertex_color.z, material->base_color.w * vertex_color.w),
      .emissive = material->emissive_factor,
      .dielectric_specular = material->dielectric_specular,
      .tangent_normal = vec3_new(0.0f, 0.0f, 1.0f),
      .attenuation_color = material->attenuation_color,
      .metallic = saturate(material->metallic),
      .roughness = fmaxf(0.04f, fminf(material->roughness, 1.0f)),
      .occlusion = saturate(material->occlusion_strength),
      .transmission = saturate(material->transmission_factor),
      .ior = fmaxf(material->ior, 1.0f),
      .thickness = fmaxf(material->thickness_factor, 0.0f),
      .attenuation_distance = material->attenuation_distance,
      .alpha_mode = material->alpha_mode,
      .alpha_cutoff = material->alpha_cutoff,
      .double_sided = material->double_sided,
      .clearcoat_factor = material->clearcoat_factor,
      .clearcoat_roughness = material->clearcoat_roughness,
      .clearcoat_tangent_normal = vec3_new(0.0f, 0.0f, 1.0f),
      .sheen_color = material->sheen_color,
      .sheen_roughness = material->sheen_roughness,
      .subsurface_strength = material->subsurface_strength,
      .subsurface_profile = material->subsurface_profile,
      .diffuse_transmission_strength = material->diffuse_transmission_strength,
      .diffuse_transmission_color = material->diffuse_transmission_color,
      .anisotropy_strength = material->anisotropy_strength,
      .anisotropy_direction = vec2_new(cosf(material->anisotropy_rotation), sinf(material->anisotropy_rotation)),
  };
  if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_BASE_COLOR].present) {
    const Vec4 sample = texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_BASE_COLOR], uv);
    result.base_color.x *= sample.x; result.base_color.y *= sample.y; result.base_color.z *= sample.z; result.base_color.w *= sample.w;
  }
  if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_NORMAL].present) {
    const Vec4 sample = texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_NORMAL], uv);
    float32_t x = (sample.x * 2.0f - 1.0f) * material->normal_scale;
    float32_t y = -(sample.y * 2.0f - 1.0f) * material->normal_scale;
    result.tangent_normal = vec3_new(x, y, sqrtf(saturate(1.0f - x * x - y * y)));
  }
  if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_METALLIC_ROUGHNESS].present) {
    const Vec4 orm = texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_METALLIC_ROUGHNESS], uv);
    result.occlusion *= orm.x;
    result.roughness = fmaxf(0.04f, fminf(result.roughness * orm.y, 1.0f));
    result.metallic = saturate(result.metallic * orm.z);
  }
  /* The published world material has no separate occlusion sampler. Its AO
     contract is the metallic-roughness texture's red channel. */
  if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_EMISSIVE].present) {
    const Vec4 sample = texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_EMISSIVE], uv);
    result.emissive.x *= sample.x; result.emissive.y *= sample.y; result.emissive.z *= sample.z;
  }
  if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_TRANSMISSION].present)
    result.transmission = saturate(result.transmission * texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_TRANSMISSION], uv).x);
  if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_THICKNESS].present)
    result.thickness = fmaxf(0.0f, result.thickness * texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_THICKNESS], uv).y);
  if (result.clearcoat_factor > 0.0f) {
    if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT].present)
      result.clearcoat_factor *= texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT], uv).x;
    if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT_ROUGHNESS].present)
      result.clearcoat_roughness *= texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT_ROUGHNESS], uv).y;
    result.clearcoat_roughness = fmaxf(0.04f, result.clearcoat_roughness);
    if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT_NORMAL].present) {
      const Vec4 sample = texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_CLEARCOAT_NORMAL], uv);
      const float32_t x = (sample.x * 2.0f - 1.0f) * material->clearcoat_normal_scale;
      const float32_t y = -(sample.y * 2.0f - 1.0f) * material->clearcoat_normal_scale;
      result.clearcoat_tangent_normal = vec3_new(x, y, sqrtf(saturate(1.0f - x * x - y * y)));
    }
  }
  if (result.sheen_color.x > 0.0f || result.sheen_color.y > 0.0f || result.sheen_color.z > 0.0f) {
    if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_SHEEN_COLOR].present) {
      const Vec4 sample = texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_SHEEN_COLOR], uv);
      result.sheen_color.x *= sample.x; result.sheen_color.y *= sample.y; result.sheen_color.z *= sample.z;
    }
    if (material->textures[VKR_BAKE_MATERIAL_TEXTURE_SHEEN_ROUGHNESS].present)
      result.sheen_roughness *= texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_SHEEN_ROUGHNESS], uv).w;
    result.sheen_roughness = fmaxf(0.04f, result.sheen_roughness);
  }
  if (result.anisotropy_strength > 0.0f && material->textures[VKR_BAKE_MATERIAL_TEXTURE_ANISOTROPY].present) {
    const Vec4 sample = texture_sample(store, material->textures[VKR_BAKE_MATERIAL_TEXTURE_ANISOTROPY], uv);
    float32_t x = sample.x * 2.0f - 1.0f, y = sample.y * 2.0f - 1.0f;
    const float32_t length_sq = x*x + y*y;
    if (length_sq > 1.0e-12f) { const float32_t inverse_length = 1.0f / sqrtf(length_sq); x *= inverse_length; y *= inverse_length; }
    else { x = 1.0f; y = 0.0f; }
    const Vec2 rotation = result.anisotropy_direction;
    result.anisotropy_direction = vec2_new(rotation.x*x - rotation.y*y, rotation.y*x + rotation.x*y);
    result.anisotropy_strength *= sample.z;
  }
  /* VKR flips image V and tangent handedness on import. */
  result.anisotropy_direction.y = -result.anisotropy_direction.y;
  *out_sample = result;
}
