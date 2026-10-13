#include "assets/vkr_ktx_file.h"
#include "assets/vkr_probe_capture.h"
#include "filesystem/vkr_filesystem_cpp.h"
#include "vkr_tool_entry.h"
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <ktx.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kFaceCount = 6u;
constexpr uint32_t kMaxCubeSize = 2048u;
constexpr uint32_t kBytesPerTexel = 8u;
// Faces may be rendered at up to this many texels per output texel on each
// axis; the packer box-filters them down before building the mip chain.
constexpr uint32_t kMaxSupersample = 4u;

enum class ParseResult { kOk, kHelp, kError };

struct Config {
  uint32_t size = 0u;
  bool size_set = false;
  fs::path output;
  bool output_set = false;
  uint32_t supersample = 1u;
  bool supersample_set = false;
  fs::path probe_capture;
  bool probe_capture_set = false;
  std::vector<fs::path> faces;
};

void destroy_ktx_texture(ktxTexture2 *texture) {
  ktxTexture_Destroy(ktxTexture(texture));
}

bool parse_size(const std::string &text, uint32_t *out_size) {
  if (!out_size || text.empty()) {
    return false;
  }
  uint64_t value = 0u;
  for (const unsigned char ch : text) {
    if (ch < '0' || ch > '9' || value > UINT32_MAX / 10u) {
      return false;
    }
    value = value * 10u + (ch - '0');
  }
  if (value == 0u || value > kMaxCubeSize || (value & (value - 1u)) != 0u) {
    return false;
  }
  *out_size = static_cast<uint32_t>(value);
  return true;
}

void print_usage(const char *program) {
  std::cout << "Usage: " << program
            << " --size <1..2048 power-of-two> --output <path.vkt>"
               " [--supersample <1|2|4>] [--probe-capture <record>]"
               " --face <raw-rgba16f> --face <raw-rgba16f>"
               " --face <raw-rgba16f> --face <raw-rgba16f>"
               " --face <raw-rgba16f> --face <raw-rgba16f>\n"
               "Faces are required in KTX order: +X, -X, +Y, -Y, +Z, -Z. Inputs"
               " are tightly packed, little-endian, top-left RGBA16F, each"
               " size * supersample texels wide; supersampled faces are"
               " box-filtered to size. A probe capture record is stored as"
               " the vkr.probe_capture key/value entry.\n";
}

ParseResult parse_args(int argc, char **argv, Config *out_config) {
  if (!out_config) {
    return ParseResult::kError;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--help" || arg == "-h") {
      return ParseResult::kHelp;
    }
    if (arg == "--size") {
      if (out_config->size_set || index + 1 >= argc ||
          !parse_size(argv[++index], &out_config->size)) {
        std::cerr << "--size must be one power-of-two in [1, 2048]\n";
        return ParseResult::kError;
      }
      out_config->size_set = true;
      continue;
    }
    if (arg == "--output") {
      if (out_config->output_set || index + 1 >= argc) {
        std::cerr << "--output is required exactly once\n";
        return ParseResult::kError;
      }
      out_config->output = vkr_filesystem_native_utf8_path(argv[++index]);
      out_config->output_set = true;
      continue;
    }
    if (arg == "--supersample") {
      if (out_config->supersample_set || index + 1 >= argc ||
          !parse_size(argv[++index], &out_config->supersample) ||
          out_config->supersample > kMaxSupersample) {
        std::cerr << "--supersample must be 1, 2 or 4\n";
        return ParseResult::kError;
      }
      out_config->supersample_set = true;
      continue;
    }
    if (arg == "--probe-capture") {
      if (out_config->probe_capture_set || index + 1 >= argc) {
        std::cerr << "--probe-capture takes one record path\n";
        return ParseResult::kError;
      }
      out_config->probe_capture =
          vkr_filesystem_native_utf8_path(argv[++index]);
      out_config->probe_capture_set = true;
      continue;
    }
    if (arg == "--face") {
      if (index + 1 >= argc) {
        std::cerr << "Missing path after --face\n";
        return ParseResult::kError;
      }
      out_config->faces.emplace_back(vkr_filesystem_native_utf8_path(argv[++index]));
      continue;
    }
    std::cerr << "Unknown argument: " << arg << "\n";
    return ParseResult::kError;
  }
  if (!out_config->size_set || !out_config->output_set ||
      out_config->faces.size() != kFaceCount) {
    std::cerr << "Expected --size, --output, and exactly six --face paths\n";
    return ParseResult::kError;
  }
  if (static_cast<uint64_t>(out_config->size) * out_config->supersample >
      kMaxCubeSize) {
    std::cerr << "--size times --supersample must not exceed " << kMaxCubeSize
              << "\n";
    return ParseResult::kError;
  }
  if (out_config->output.extension() != ".vkt") {
    std::cerr << "--output must name a .vkt file\n";
    return ParseResult::kError;
  }
  return ParseResult::kOk;
}

bool paths_alias(const fs::path &source, const fs::path &output) {
  std::error_code error;
  if (fs::exists(output, error) && !error) {
    const bool equivalent = fs::equivalent(source, output, error);
    if (!error && equivalent) {
      return true;
    }
  }
  error.clear();
  const fs::path source_absolute =
      fs::absolute(source, error).lexically_normal();
  if (error) {
    return false;
  }
  const fs::path output_absolute =
      fs::absolute(output, error).lexically_normal();
  return !error && source_absolute == output_absolute;
}

bool half_bits_are_finite(uint16_t bits) { return (bits & 0x7c00u) != 0x7c00u; }

uint16_t read_u16_le(const uint8_t *bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8u);
}

bool validate_rgba16f(const std::vector<uint8_t> &bytes, const fs::path &path) {
  for (size_t texel = 0u; texel < bytes.size() / kBytesPerTexel; ++texel) {
    const uint8_t *const sample = bytes.data() + texel * kBytesPerTexel;
    for (uint32_t component = 0u; component < 4u; ++component) {
      if (!half_bits_are_finite(read_u16_le(sample + component * 2u))) {
        std::cerr << "Non-finite RGBA16F component in '" << path
                  << "' at texel " << texel << ", component " << component
                  << "\n";
        return false;
      }
    }
  }
  return true;
}

bool read_face(const fs::path &path, uint64_t expected_bytes,
               std::vector<uint8_t> *out_bytes) {
  if (!out_bytes) {
    return false;
  }
  std::error_code error;
  if (!fs::is_regular_file(path, error) || error) {
    std::cerr << "Face input is not a regular file: " << path << "\n";
    return false;
  }
  const uint64_t actual_bytes = fs::file_size(path, error);
  if (error || actual_bytes != expected_bytes) {
    std::cerr << "Face input has "
              << (error ? std::string("an unreadable size")
                        : std::to_string(actual_bytes) + " bytes")
              << "; expected " << expected_bytes << ": " << path << "\n";
    return false;
  }
  out_bytes->resize(static_cast<size_t>(expected_bytes));
  std::ifstream input(path, std::ios::binary);
  if (!input.read(reinterpret_cast<char *>(out_bytes->data()),
                  static_cast<std::streamsize>(out_bytes->size())) ||
      !validate_rgba16f(*out_bytes, path)) {
    std::cerr << "Failed to read RGBA16F face: " << path << "\n";
    return false;
  }
  return true;
}

float half_to_float(uint16_t bits) {
  const uint32_t sign = static_cast<uint32_t>(bits & 0x8000u) << 16u;
  const uint32_t exponent = (bits >> 10u) & 0x1fu;
  uint32_t mantissa = bits & 0x3ffu;
  uint32_t result = 0u;
  if (exponent == 0u) {
    if (mantissa != 0u) {
      // Subnormal: normalize into a float exponent.
      uint32_t shift = 0u;
      while ((mantissa & 0x400u) == 0u) {
        mantissa <<= 1u;
        ++shift;
      }
      mantissa &= 0x3ffu;
      result = sign | ((113u - shift) << 23u) | (mantissa << 13u);
    } else {
      result = sign;
    }
  } else {
    result = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
  }
  float value = 0.0f;
  std::memcpy(&value, &result, sizeof(value));
  return value;
}

// Round-to-nearest-even conversion of a finite value; values beyond the
// largest half clamp to it, so averaged finite faces stay finite.
uint16_t float_to_half(float value) {
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint16_t sign = static_cast<uint16_t>((bits >> 16u) & 0x8000u);
  const uint32_t magnitude = bits & 0x7fffffffu;
  if (magnitude >= 0x477ff000u) {
    return static_cast<uint16_t>(sign | 0x7bffu);
  }
  if (magnitude < 0x38800000u) {
    // Subnormal half or zero.
    if (magnitude < 0x33000000u) {
      return sign;
    }
    const uint32_t exponent = magnitude >> 23u;
    const uint32_t mantissa = (magnitude & 0x7fffffu) | 0x800000u;
    const uint32_t shift = 126u - exponent;
    uint32_t half = mantissa >> shift;
    const uint32_t remainder = mantissa & ((1u << shift) - 1u);
    const uint32_t halfway = 1u << (shift - 1u);
    if (remainder > halfway || (remainder == halfway && (half & 1u))) {
      ++half;
    }
    return static_cast<uint16_t>(sign | half);
  }
  uint32_t half = ((magnitude - 0x38000000u) >> 13u);
  const uint32_t remainder = magnitude & 0x1fffu;
  if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u))) {
    ++half;
  }
  return static_cast<uint16_t>(sign | half);
}

uint32_t cube_level_count(uint32_t size) {
  uint32_t levels = 1u;
  while ((size >> levels) != 0u) {
    ++levels;
  }
  return levels;
}

// One 2x2 box-filtered level of a face. The prefilter's filtered importance
// sampling reads these levels, so a rough lobe averages the face instead of
// aliasing over its sharp top level.
void downsample_face(const std::vector<uint8_t> &source, uint32_t size,
                     std::vector<uint8_t> *out_level) {
  const uint32_t half_size = size / 2u;
  out_level->resize(static_cast<size_t>(half_size) * half_size *
                    kBytesPerTexel);
  for (uint32_t y = 0u; y < half_size; ++y) {
    for (uint32_t x = 0u; x < half_size; ++x) {
      for (uint32_t component = 0u; component < 4u; ++component) {
        float sum = 0.0f;
        for (uint32_t dy = 0u; dy < 2u; ++dy) {
          for (uint32_t dx = 0u; dx < 2u; ++dx) {
            const size_t texel =
                static_cast<size_t>(2u * y + dy) * size + (2u * x + dx);
            sum += half_to_float(read_u16_le(
                source.data() + texel * kBytesPerTexel + component * 2u));
          }
        }
        const uint16_t half = float_to_half(0.25f * sum);
        uint8_t *const out =
            out_level->data() +
            (static_cast<size_t>(y) * half_size + x) * kBytesPerTexel +
            component * 2u;
        out[0] = static_cast<uint8_t>(half & 0xffu);
        out[1] = static_cast<uint8_t>(half >> 8u);
      }
    }
  }
}

void flip_rows(const std::vector<uint8_t> &source, uint32_t size,
               std::vector<uint8_t> *out_flipped) {
  const size_t row_bytes = static_cast<size_t>(size) * kBytesPerTexel;
  out_flipped->resize(source.size());
  for (uint32_t y = 0u; y < size; ++y) {
    const uint8_t *from =
        source.data() + static_cast<size_t>(size - 1u - y) * row_bytes;
    uint8_t *to = out_flipped->data() + static_cast<size_t>(y) * row_bytes;
    std::copy_n(from, row_bytes, to);
  }
}

bool publish_temporary_output(const fs::path &temporary,
                              const fs::path &destination,
                              std::error_code *out_error) {
  if (!out_error) {
    return false;
  }
  out_error->clear();
#if defined(_WIN32)
  // A scanner or a just-exited reader may hold the destination for a moment
  // after the capture that loaded it, so a denied replace is retried briefly.
  for (uint32_t attempt = 0u; attempt < 20u; ++attempt) {
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return true;
    }
    const DWORD failure = GetLastError();
    *out_error =
        std::error_code(static_cast<int>(failure), std::system_category());
    if (failure != ERROR_ACCESS_DENIED && failure != ERROR_SHARING_VIOLATION) {
      return false;
    }
    Sleep(100u);
  }
  return false;
#else
  fs::rename(temporary, destination, *out_error);
  return !*out_error;
#endif
}

fs::path temporary_output_path(const fs::path &output) {
  const uint64_t suffix =
      static_cast<uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()) ^
      static_cast<uint64_t>(
          std::hash<std::thread::id>{}(std::this_thread::get_id()));
  return vkr_filesystem_native_utf8_path(output.u8string() + ".tmp." + std::to_string(suffix));
}

bool validate_written_ktx(const fs::path &path, uint32_t size,
                          uint64_t image_bytes) {
  ktxTexture2 *texture = nullptr;
  const KTX_error_code result = vkr_ktx_read_file(
      path.u8string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture);
  if (result != KTX_SUCCESS || !texture) {
    std::cerr << "Temporary KTX2 cannot be reopened: " << ktxErrorString(result)
              << "\n";
    return false;
  }
  const std::unique_ptr<ktxTexture2, decltype(&destroy_ktx_texture)> owner(
      texture, destroy_ktx_texture);
  ktxTexture *const base = ktxTexture(texture);
  if (texture->vkFormat != VK_FORMAT_R16G16B16A16_SFLOAT ||
      texture->supercompressionScheme != KTX_SS_NONE ||
      ktxTexture2_NeedsTranscoding(texture) || base->numDimensions != 2u ||
      base->baseWidth != size || base->baseHeight != size ||
      base->baseDepth != 1u || base->numLevels != cube_level_count(size) ||
      base->numLayers != 1u ||
      base->numFaces != kFaceCount || !base->isCubemap) {
    std::cerr << "Temporary KTX2 does not satisfy the direct HDR cubemap "
                 "contract\n";
    return false;
  }
  const uint8_t *data = ktxTexture_GetData(base);
  const ktx_size_t data_size = ktxTexture_GetDataSize(base);
  uint64_t expected_bytes = 0u;
  for (uint32_t level = 0u; level < base->numLevels; ++level) {
    const uint64_t extent = size >> level;
    expected_bytes += extent * extent * kBytesPerTexel * kFaceCount;
  }
  if (!data || data_size != expected_bytes ||
      ktxTexture_GetImageSize(base, 0u) != image_bytes) {
    std::cerr << "Temporary KTX2 has an unexpected payload size\n";
    return false;
  }
  for (uint32_t level = 0u; level < base->numLevels; ++level) {
    const uint64_t extent = size >> level;
    const uint64_t level_bytes = extent * extent * kBytesPerTexel;
    for (uint32_t face = 0u; face < kFaceCount; ++face) {
      ktx_size_t offset = 0u;
      if (ktxTexture_GetImageOffset(base, level, 0u, face, &offset) !=
              KTX_SUCCESS ||
          ktxTexture_GetImageSize(base, level) != level_bytes ||
          offset > data_size || level_bytes > data_size - offset) {
        std::cerr << "Temporary KTX2 has an invalid face layout\n";
        return false;
      }
    }
  }
  return true;
}

bool read_probe_capture(const fs::path &path, std::vector<uint8_t> *out_bytes) {
  std::error_code error;
  const uint64_t bytes = fs::file_size(path, error);
  if (error || bytes == 0u || bytes > VKR_PROBE_CAPTURE_MAX_BYTES) {
    std::cerr << "Probe capture record is missing or larger than "
              << VKR_PROBE_CAPTURE_MAX_BYTES << " bytes: " << path << "\n";
    return false;
  }
  out_bytes->resize(static_cast<size_t>(bytes));
  std::ifstream input(path, std::ios::binary);
  if (!input.read(reinterpret_cast<char *>(out_bytes->data()),
                  static_cast<std::streamsize>(out_bytes->size()))) {
    std::cerr << "Failed to read probe capture record: " << path << "\n";
    return false;
  }
  VkrProbeCapture capture = {};
  if (!vkr_probe_capture_decode(out_bytes->data(), out_bytes->size(),
                                &capture)) {
    std::cerr << "Probe capture record is invalid: " << path << "\n";
    return false;
  }
  return true;
}

bool pack_hdr_cube(const Config &config) {
  const uint64_t texel_count = static_cast<uint64_t>(config.size) * config.size;
  const uint64_t image_bytes = texel_count * kBytesPerTexel;
  const uint32_t face_size = config.size * config.supersample;
  const uint64_t face_bytes =
      static_cast<uint64_t>(face_size) * face_size * kBytesPerTexel;
  if (face_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    std::cerr << "Requested cubemap exceeds addressable memory\n";
    return false;
  }
  for (const fs::path &face : config.faces) {
    if (paths_alias(face, config.output)) {
      std::cerr << "Output aliases a face input: " << face << "\n";
      return false;
    }
  }

  ktxTextureCreateInfo create_info = {};
  create_info.vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  create_info.baseWidth = config.size;
  create_info.baseHeight = config.size;
  create_info.baseDepth = 1u;
  create_info.numDimensions = 2u;
  create_info.numLevels = cube_level_count(config.size);
  create_info.numLayers = 1u;
  create_info.numFaces = kFaceCount;
  create_info.isArray = KTX_FALSE;
  create_info.generateMipmaps = KTX_FALSE;

  ktxTexture2 *texture = nullptr;
  KTX_error_code result = ktxTexture2_Create(
      &create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
  if (result != KTX_SUCCESS || !texture) {
    std::cerr << "Failed to create KTX2: " << ktxErrorString(result) << "\n";
    return false;
  }
  const std::unique_ptr<ktxTexture2, decltype(&destroy_ktx_texture)> owner(
      texture, destroy_ktx_texture);

  if (config.probe_capture_set) {
    std::vector<uint8_t> record;
    if (!read_probe_capture(config.probe_capture, &record)) {
      return false;
    }
    result = ktxHashList_AddKVPair(
        &ktxTexture(texture)->kvDataHead, VKR_PROBE_CAPTURE_KEY,
        static_cast<unsigned int>(record.size()), record.data());
    if (result != KTX_SUCCESS) {
      std::cerr << "Failed to add the probe capture record: "
                << ktxErrorString(result) << "\n";
      return false;
    }
  }

  std::vector<uint8_t> source;
  std::vector<uint8_t> flipped;
  for (uint32_t face = 0u; face < kFaceCount; ++face) {
    if (!read_face(config.faces[face], face_bytes, &source)) {
      return false;
    }
    flip_rows(source, face_size, &flipped);
    // Each 2x2 halving of a power-of-two supersample is one box filter of
    // the whole supersample footprint.
    for (uint32_t extent = face_size; extent > config.size; extent /= 2u) {
      downsample_face(flipped, extent, &source);
      flipped.swap(source);
    }
    uint32_t extent = config.size;
    for (uint32_t level = 0u; level < create_info.numLevels; ++level) {
      if (level > 0u) {
        downsample_face(flipped, extent, &source);
        extent /= 2u;
        flipped.swap(source);
      }
      result = ktxTexture_SetImageFromMemory(
          ktxTexture(texture), level, 0u, face, flipped.data(),
          static_cast<ktx_size_t>(flipped.size()));
      if (result != KTX_SUCCESS) {
        std::cerr << "Failed to set KTX2 face " << face << " level " << level
                  << ": " << ktxErrorString(result) << "\n";
        return false;
      }
    }
  }

  std::error_code error;
  if (!config.output.parent_path().empty()) {
    fs::create_directories(config.output.parent_path(), error);
    if (error) {
      std::cerr << "Cannot create output directory: " << error.message()
                << "\n";
      return false;
    }
  }
  const fs::path temporary = temporary_output_path(config.output);
  result = vkr_ktx_write_file(ktxTexture(texture),
                                       temporary.u8string().c_str());
  if (result != KTX_SUCCESS) {
    std::cerr << "Failed to write temporary KTX2: " << ktxErrorString(result)
              << "\n";
    fs::remove(temporary, error);
    return false;
  }
  if (!validate_written_ktx(temporary, config.size, image_bytes)) {
    fs::remove(temporary, error);
    return false;
  }
  if (!publish_temporary_output(temporary, config.output, &error)) {
    std::cerr << "Failed to replace output atomically: " << error.message()
              << "\n";
    fs::remove(temporary, error);
    return false;
  }
  std::cout << "vkr_hdr_cube_packer: status=packed output=" << config.output
            << " size=" << config.size << " supersample=" << config.supersample
            << " faces=" << kFaceCount
            << " levels=" << cube_level_count(config.size)
            << " format=R16G16B16A16_SFLOAT row_flip=vertical\n";
  return true;
}

} // namespace

VKR_TOOL_ENTRY(vkr_hdr_cube_packer_tool_main) {
  Config config;
  const ParseResult parsed = parse_args(argc, argv, &config);
  if (parsed == ParseResult::kHelp) {
    print_usage(argv[0]);
    return 0;
  }
  if (parsed != ParseResult::kOk) {
    print_usage(argv[0]);
    return 1;
  }
  try {
    return pack_hdr_cube(config) ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << "HDR cubemap packing failed: " << error.what() << "\n";
    return 1;
  }
}
