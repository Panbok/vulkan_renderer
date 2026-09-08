#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <ktx.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
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

enum class ParseResult { kOk, kHelp, kError };

struct Config {
  uint32_t size = 0u;
  bool size_set = false;
  fs::path output;
  bool output_set = false;
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
               " --face <raw-rgba16f> --face <raw-rgba16f>"
               " --face <raw-rgba16f> --face <raw-rgba16f>"
               " --face <raw-rgba16f> --face <raw-rgba16f>\n"
               "Faces are required in KTX order: +X, -X, +Y, -Y, +Z, -Z. Inputs"
               " are tightly packed, little-endian, top-left RGBA16F.\n";
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
      out_config->output = fs::path(argv[++index]);
      out_config->output_set = true;
      continue;
    }
    if (arg == "--face") {
      if (index + 1 >= argc) {
        std::cerr << "Missing path after --face\n";
        return ParseResult::kError;
      }
      out_config->faces.emplace_back(argv[++index]);
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
  if (MoveFileExW(temporary.c_str(), destination.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  *out_error =
      std::error_code(static_cast<int>(GetLastError()), std::system_category());
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
  return fs::path(output.string() + ".tmp." + std::to_string(suffix));
}

bool validate_written_ktx(const fs::path &path, uint32_t size,
                          uint64_t image_bytes) {
  ktxTexture2 *texture = nullptr;
  const KTX_error_code result = ktxTexture2_CreateFromNamedFile(
      path.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture);
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
      base->baseDepth != 1u || base->numLevels != 1u || base->numLayers != 1u ||
      base->numFaces != kFaceCount || !base->isCubemap) {
    std::cerr << "Temporary KTX2 does not satisfy the direct HDR cubemap "
                 "contract\n";
    return false;
  }
  const uint8_t *data = ktxTexture_GetData(base);
  const ktx_size_t data_size = ktxTexture_GetDataSize(base);
  if (!data || data_size != image_bytes * kFaceCount) {
    std::cerr << "Temporary KTX2 has an unexpected payload size\n";
    return false;
  }
  for (uint32_t face = 0u; face < kFaceCount; ++face) {
    ktx_size_t offset = 0u;
    if (ktxTexture_GetImageOffset(base, 0u, 0u, face, &offset) != KTX_SUCCESS ||
        ktxTexture_GetImageSize(base, 0u) != image_bytes ||
        offset > data_size || image_bytes > data_size - offset) {
      std::cerr << "Temporary KTX2 has an invalid face layout\n";
      return false;
    }
  }
  return true;
}

bool pack_hdr_cube(const Config &config) {
  const uint64_t texel_count = static_cast<uint64_t>(config.size) * config.size;
  const uint64_t image_bytes = texel_count * kBytesPerTexel;
  if (image_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
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
  create_info.numLevels = 1u;
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

  std::vector<uint8_t> source;
  std::vector<uint8_t> flipped;
  for (uint32_t face = 0u; face < kFaceCount; ++face) {
    if (!read_face(config.faces[face], image_bytes, &source)) {
      return false;
    }
    flip_rows(source, config.size, &flipped);
    result = ktxTexture_SetImageFromMemory(
        ktxTexture(texture), 0u, 0u, face, flipped.data(),
        static_cast<ktx_size_t>(flipped.size()));
    if (result != KTX_SUCCESS) {
      std::cerr << "Failed to set KTX2 face " << face << ": "
                << ktxErrorString(result) << "\n";
      return false;
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
  result = ktxTexture_WriteToNamedFile(ktxTexture(texture),
                                       temporary.string().c_str());
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
            << " size=" << config.size << " faces=" << kFaceCount
            << " format=R16G16B16A16_SFLOAT row_flip=vertical\n";
  return true;
}

} // namespace

int main(int argc, char **argv) {
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
