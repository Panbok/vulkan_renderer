#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <ktx.h>
#include <vulkan/vulkan_core.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "vkr_vkt_pack_contract.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr float kAlphaMaskIntermediateRatio = 0.30f;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint32_t kMaxTextureDimension = 16384u;
constexpr uint32_t kMaxPhysicalLayers = 2048u;
constexpr uint32_t kMaxUploadRegions = 32768u;

struct AlphaAnalysis {
  bool has_transparency = false;
  bool alpha_mask = false;
  uint64_t transparent_count = 0u;
  uint64_t intermediate_count = 0u;
};

struct LevelImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;
};

struct PackStats {
  uint32_t discovered = 0;
  uint32_t packed = 0;
  uint32_t skipped = 0;
  uint32_t failed = 0;
};

enum class ParseResult { kOk, kHelp, kError };
enum class TextureClass {
  kColorSrgb = 0,
  kColorLinear,
  kNormalRg,
  kDataMask,
};
enum class TextureShape { k2D, k2DArray, kCube, kCubeArray };

struct PackConfig {
  fs::path input_dir;
  fs::path output;
  std::vector<fs::path> layers;
  TextureShape shape = TextureShape::k2D;
  TextureClass texture_class = TextureClass::kColorSrgb;
  bool layered_mode = false;
  bool shape_explicit = false;
  bool texture_class_explicit = false;
  bool strict = false;
  bool force = false;
  bool verbose = false;
  bool progress = true;
  uint32_t basis_threads = 0;
  ktx_pack_uastc_flags uastc_level = KTX_PACK_UASTC_LEVEL_FASTER;
  bool write_source_hash = true;
};

std::string to_lower_ascii(std::string value);

bool parse_texture_shape(const std::string &value, TextureShape *out) {
  const std::string normalized = to_lower_ascii(value);
  if (normalized == "2d") {
    *out = TextureShape::k2D;
    return true;
  }
  if (normalized == "2d-array") {
    *out = TextureShape::k2DArray;
    return true;
  }
  if (normalized == "cube") {
    *out = TextureShape::kCube;
    return true;
  }
  if (normalized == "cube-array") {
    *out = TextureShape::kCubeArray;
    return true;
  }
  return false;
}

bool parse_texture_class_option(const std::string &value, TextureClass *out) {
  const std::string normalized = to_lower_ascii(value);
  if (normalized == "color-srgb") {
    *out = TextureClass::kColorSrgb;
  } else if (normalized == "color-linear") {
    *out = TextureClass::kColorLinear;
  } else if (normalized == "normal-rg") {
    *out = TextureClass::kNormalRg;
  } else if (normalized == "data-mask") {
    *out = TextureClass::kDataMask;
  } else {
    return false;
  }
  return true;
}

bool parse_uint32_nonzero(const std::string &value, uint32_t *out_value) {
  if (!out_value || value.empty()) {
    return false;
  }

  size_t consumed = 0;
  uint64_t parsed = 0;
  try {
    parsed = std::stoull(value, &consumed, 10);
  } catch (...) {
    return false;
  }

  if (consumed != value.size() || parsed == 0u || parsed > 0xFFFFFFFFull) {
    return false;
  }

  *out_value = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_uastc_level(const std::string &value, ktx_pack_uastc_flags *out) {
  if (!out) {
    return false;
  }

  const std::string normalized = to_lower_ascii(value);
  if (normalized == "0" || normalized == "fastest") {
    *out = KTX_PACK_UASTC_LEVEL_FASTEST;
    return true;
  }
  if (normalized == "1" || normalized == "faster") {
    *out = KTX_PACK_UASTC_LEVEL_FASTER;
    return true;
  }
  if (normalized == "2" || normalized == "default") {
    *out = KTX_PACK_UASTC_LEVEL_DEFAULT;
    return true;
  }
  if (normalized == "3" || normalized == "slower") {
    *out = KTX_PACK_UASTC_LEVEL_SLOWER;
    return true;
  }
  if (normalized == "4" || normalized == "veryslow") {
    *out = KTX_PACK_UASTC_LEVEL_VERYSLOW;
    return true;
  }
  return false;
}

const char *uastc_level_to_string(ktx_pack_uastc_flags level) {
  switch (level & KTX_PACK_UASTC_LEVEL_MASK) {
  case KTX_PACK_UASTC_LEVEL_FASTEST:
    return "fastest";
  case KTX_PACK_UASTC_LEVEL_FASTER:
    return "faster";
  case KTX_PACK_UASTC_LEVEL_DEFAULT:
    return "default";
  case KTX_PACK_UASTC_LEVEL_SLOWER:
    return "slower";
  case KTX_PACK_UASTC_LEVEL_VERYSLOW:
    return "veryslow";
  default:
    return "default";
  }
}

uint32_t resolve_basis_thread_count(uint32_t configured_threads) {
  if (configured_threads > 0u) {
    return configured_threads;
  }
  const uint32_t detected = std::thread::hardware_concurrency();
  return detected > 0u ? detected : 1u;
}

ParseResult parse_args(int argc, char **argv, PackConfig &out_config) {
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--input-dir") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for --input-dir\n";
        return ParseResult::kError;
      }
      out_config.input_dir = fs::path(argv[++index]);
      continue;
    }
    if (arg == "--output") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for --output\n";
        return ParseResult::kError;
      }
      out_config.output = fs::path(argv[++index]);
      out_config.layered_mode = true;
      continue;
    }
    if (arg == "--type") {
      if (index + 1 >= argc ||
          !parse_texture_shape(argv[++index], &out_config.shape)) {
        std::cerr << "Invalid --type (expected 2d|2d-array|cube|cube-array)\n";
        return ParseResult::kError;
      }
      out_config.shape_explicit = true;
      out_config.layered_mode = true;
      continue;
    }
    if (arg == "--layer") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for --layer\n";
        return ParseResult::kError;
      }
      out_config.layers.emplace_back(argv[++index]);
      out_config.layered_mode = true;
      continue;
    }
    if (arg == "--texture-class") {
      if (index + 1 >= argc || !parse_texture_class_option(
                                   argv[++index], &out_config.texture_class)) {
        std::cerr << "Invalid --texture-class\n";
        return ParseResult::kError;
      }
      out_config.texture_class_explicit = true;
      continue;
    }
    if (arg == "--strict") {
      out_config.strict = true;
      continue;
    }
    if (arg == "--force") {
      out_config.force = true;
      continue;
    }
    if (arg == "--verbose") {
      out_config.verbose = true;
      continue;
    }
    if (arg == "--progress") {
      out_config.progress = true;
      continue;
    }
    if (arg == "--no-progress") {
      out_config.progress = false;
      continue;
    }
    if (arg == "--basis-threads") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for --basis-threads\n";
        return ParseResult::kError;
      }
      const std::string value = to_lower_ascii(argv[++index]);
      if (value == "auto") {
        out_config.basis_threads = 0;
        continue;
      }
      uint32_t parsed = 0;
      if (!parse_uint32_nonzero(value, &parsed)) {
        std::cerr << "Invalid --basis-threads value '" << value
                  << "' (expected positive integer or 'auto')\n";
        return ParseResult::kError;
      }
      out_config.basis_threads = parsed;
      continue;
    }
    if (arg == "--uastc-level") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for --uastc-level\n";
        return ParseResult::kError;
      }
      const std::string value = argv[++index];
      if (!parse_uastc_level(value, &out_config.uastc_level)) {
        std::cerr
            << "Invalid --uastc-level value '" << value
            << "' (expected fastest|faster|default|slower|veryslow or 0..4)\n";
        return ParseResult::kError;
      }
      continue;
    }
    if (arg == "--source-hash") {
      out_config.write_source_hash = true;
      continue;
    }
    if (arg == "--no-source-hash") {
      out_config.write_source_hash = false;
      continue;
    }
    if (arg == "--help" || arg == "-h") {
      return ParseResult::kHelp;
    }
    std::cerr << "Unknown argument: " << arg << "\n";
    return ParseResult::kError;
  }

  if (out_config.layered_mode) {
    if (!out_config.input_dir.empty() || out_config.output.empty() ||
        !out_config.shape_explicit || out_config.layers.empty()) {
      std::cerr << "Layered mode requires --output, --type, and repeated "
                   "--layer; it cannot use --input-dir\n";
      return ParseResult::kError;
    }
  } else if (out_config.input_dir.empty()) {
    std::cerr << "Missing required argument --input-dir\n";
    return ParseResult::kError;
  }

  return ParseResult::kOk;
}

void print_usage(const char *program_name) {
  std::cout
      << "Usage: " << program_name
      << " --input-dir <path> [options]\n"
         "   or: "
      << program_name
      << " --output <file.vkt> --type <2d|2d-array|cube|cube-array>"
         " --layer <image> [--layer <image> ...]"
         " [--texture-class <color-srgb|color-linear|normal-rg|data-mask>]"
         " [options]\n"
         "Options: [--strict] [--force] [--verbose]"
         " [--progress|--no-progress] [--basis-threads <auto|n>]"
         " [--uastc-level <fastest|faster|default|slower|veryslow>]"
         " [--source-hash|--no-source-hash]\n";
}

std::string format_duration(double seconds) {
  if (seconds < 0.0) {
    seconds = 0.0;
  }
  const int total = static_cast<int>(seconds + 0.5);
  const int mins = total / 60;
  const int secs = total % 60;
  std::ostringstream oss;
  oss << mins << "m" << std::setw(2) << std::setfill('0') << secs << "s";
  return oss.str();
}

void log_progress_line(bool enabled, const std::string &line) {
  if (!enabled) {
    return;
  }
  std::cout << line << std::endl;
}

std::string to_lower_ascii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool is_supported_source_extension(const fs::path &path) {
  static const std::array<const char *, 5> kExts = {".png", ".jpg", ".jpeg",
                                                    ".bmp", ".tga"};
  std::string ext = to_lower_ascii(path.extension().string());
  return std::find(kExts.begin(), kExts.end(), ext) != kExts.end();
}

bool contains_any_token(const std::string &value,
                        const std::array<const char *, 14> &tokens) {
  for (const char *token : tokens) {
    if (value.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

TextureClass infer_texture_class(const fs::path &path) {
  const std::string name = to_lower_ascii(path.filename().string());
  if (vkr_vkt_filename_is_normal_rg(name.data(), name.size())) {
    return TextureClass::kNormalRg;
  }

  static const std::array<const char *, 14> kDataTokens = {
      "roughness", "metallic", "metalness", "occlusion", "ao.",
      "orm",       "rma",      "mask",      "height",    "displace",
      "specular",  "gloss",    "data",      "utility"};
  if (contains_any_token(name, kDataTokens)) {
    return TextureClass::kDataMask;
  }

  return TextureClass::kColorSrgb;
}

bool texture_class_prefers_srgb(TextureClass texture_class) {
  return texture_class == TextureClass::kColorSrgb;
}

const char *texture_class_metadata_value(TextureClass texture_class) {
  switch (texture_class) {
  case TextureClass::kColorSrgb:
    return "color_srgb";
  case TextureClass::kColorLinear:
    return "color_linear";
  case TextureClass::kNormalRg:
    return "normal_rg";
  case TextureClass::kDataMask:
    return "data_mask";
  default:
    return "color_linear";
  }
}

uint32_t calculate_mip_levels(uint32_t width, uint32_t height) {
  uint32_t levels = 1;
  while (width > 1 || height > 1) {
    width = std::max(1u, width >> 1u);
    height = std::max(1u, height >> 1u);
    ++levels;
  }
  return levels;
}

std::vector<LevelImage> build_mip_chain_rgba8(const uint8_t *base_pixels,
                                              uint32_t width, uint32_t height) {
  std::vector<LevelImage> levels;
  levels.reserve(calculate_mip_levels(width, height));

  LevelImage base = {};
  base.width = width;
  base.height = height;
  base.pixels.assign(base_pixels, base_pixels + (width * height * 4u));
  levels.push_back(std::move(base));

  while (levels.back().width > 1 || levels.back().height > 1) {
    const LevelImage &previous = levels.back();
    const uint32_t next_width = std::max(1u, previous.width >> 1u);
    const uint32_t next_height = std::max(1u, previous.height >> 1u);

    LevelImage next = {};
    next.width = next_width;
    next.height = next_height;
    next.pixels.resize(static_cast<size_t>(next_width) * next_height * 4u);

    for (uint32_t y = 0; y < next_height; ++y) {
      for (uint32_t x = 0; x < next_width; ++x) {
        std::array<uint32_t, 4> accum = {0, 0, 0, 0};
        for (uint32_t oy = 0; oy < 2; ++oy) {
          const uint32_t sy = std::min(previous.height - 1, (y * 2u) + oy);
          for (uint32_t ox = 0; ox < 2; ++ox) {
            const uint32_t sx = std::min(previous.width - 1, (x * 2u) + ox);
            const size_t src_index =
                (static_cast<size_t>(sy) * previous.width + sx) * 4u;
            for (uint32_t channel = 0; channel < 4; ++channel) {
              accum[channel] += previous.pixels[src_index + channel];
            }
          }
        }

        const size_t dst_index = (static_cast<size_t>(y) * next_width + x) * 4u;
        for (uint32_t channel = 0; channel < 4; ++channel) {
          next.pixels[dst_index + channel] =
              static_cast<uint8_t>(accum[channel] / 4u);
        }
      }
    }

    levels.push_back(std::move(next));
  }

  return levels;
}

AlphaAnalysis analyze_alpha(const uint8_t *pixels, uint32_t width,
                            uint32_t height) {
  AlphaAnalysis analysis = {};
  if (!pixels || width == 0 || height == 0) {
    return analysis;
  }

  const uint64_t pixel_count = static_cast<uint64_t>(width) * height;
  uint64_t transparent_count = 0;
  uint64_t intermediate_count = 0;
  for (uint64_t index = 0; index < pixel_count; ++index) {
    const uint8_t alpha = pixels[index * 4u + 3u];
    if (alpha < 255u) {
      ++transparent_count;
      if (alpha > 0u && alpha < 255u) {
        ++intermediate_count;
      }
    }
  }

  if (transparent_count == 0) {
    return analysis;
  }

  analysis.transparent_count = transparent_count;
  analysis.intermediate_count = intermediate_count;
  analysis.has_transparency = true;
  const float ratio = static_cast<float>(intermediate_count) /
                      static_cast<float>(transparent_count);
  analysis.alpha_mask = ratio <= kAlphaMaskIntermediateRatio;
  return analysis;
}

uint64_t fnv1a_file_hash(const fs::path &path, bool *ok) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    *ok = false;
    return 0;
  }

  uint64_t hash = kFnvOffsetBasis;
  std::array<char, 4096> buffer = {};
  while (input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count <= 0) {
      continue;
    }
    for (std::streamsize i = 0; i < count; ++i) {
      hash ^= static_cast<uint8_t>(buffer[static_cast<size_t>(i)]);
      hash *= kFnvPrime;
    }
  }

  *ok = input.eof() || input.good();
  return hash;
}

std::string to_hex_u64(uint64_t value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int index = 15; index >= 0; --index) {
    out[static_cast<size_t>(index)] = kHex[value & 0xFu];
    value >>= 4u;
  }
  return out;
}

bool combined_source_hash(const std::vector<fs::path> &source_paths,
                          uint64_t *out_hash) {
  if (!out_hash || source_paths.empty()) {
    return false;
  }
  uint64_t combined = kFnvOffsetBasis;
  for (const fs::path &path : source_paths) {
    bool ok = true;
    const uint64_t source_hash = fnv1a_file_hash(path, &ok);
    if (!ok) {
      return false;
    }
    combined ^= source_hash;
    combined *= kFnvPrime;
  }
  *out_hash = combined;
  return true;
}

const char *texture_shape_metadata_value(TextureShape shape) {
  switch (shape) {
  case TextureShape::k2D:
    return "2d";
  case TextureShape::k2DArray:
    return "2d-array";
  case TextureShape::kCube:
    return "cube";
  case TextureShape::kCubeArray:
    return "cube-array";
  }
  return "invalid";
}

std::string pack_settings_identity(TextureClass texture_class,
                                   TextureShape shape,
                                   const PackConfig &config) {
  std::ostringstream settings;
  settings << "asset=1;shape=" << texture_shape_metadata_value(shape)
           << ";class=" << texture_class_metadata_value(texture_class)
           << ";uastc=" << uastc_level_to_string(config.uastc_level)
           << ";mips=rgba8-box-v1;flip=vertical";
  if (texture_class == TextureClass::kNormalRg) {
    settings << ";basis_rg=source-ra-v1";
  }
  return settings.str();
}

bool texture_metadata_equals(ktxTexture2 *texture, const char *key,
                             const std::string &expected) {
  if (!texture || !key) {
    return false;
  }
  unsigned int value_length = 0u;
  void *value = nullptr;
  return ktxHashList_FindValue(&texture->kvDataHead, key, &value_length,
                               &value) == KTX_SUCCESS &&
         value && value_length == expected.size() + 1u &&
         std::equal(expected.begin(), expected.end(),
                    static_cast<const char *>(value)) &&
         static_cast<const char *>(value)[expected.size()] == '\0';
}

bool add_kv_string(ktxTexture2 *texture, const char *key,
                   const std::string &value) {
  if (!texture || !key) {
    return false;
  }

  return ktxHashList_AddKVPair(&texture->kvDataHead, key,
                               static_cast<unsigned int>(value.size() + 1u),
                               value.c_str()) == KTX_SUCCESS;
}

bool add_kv_bool(ktxTexture2 *texture, const char *key, bool value) {
  const char encoded[2] = {value ? '1' : '0', '\0'};
  return ktxHashList_AddKVPair(&texture->kvDataHead, key, 2u, encoded) ==
         KTX_SUCCESS;
}

bool should_skip_output(const std::vector<fs::path> &source_paths,
                        const fs::path &dst, TextureClass texture_class,
                        TextureShape shape, const PackConfig &config) {
  if (config.force || !config.write_source_hash || !fs::exists(dst)) {
    return false;
  }
  uint64_t source_hash = 0u;
  if (!combined_source_hash(source_paths, &source_hash)) {
    return false;
  }
  ktxTexture2 *texture = nullptr;
  if (ktxTexture2_CreateFromNamedFile(dst.string().c_str(),
                                      KTX_TEXTURE_CREATE_NO_FLAGS,
                                      &texture) != KTX_SUCCESS ||
      !texture) {
    return false;
  }
  const bool matches =
      texture_metadata_equals(texture, "vkr.source_hash",
                              to_hex_u64(source_hash)) &&
      texture_metadata_equals(
          texture, "vkr.pack_settings",
          pack_settings_identity(texture_class, shape, config));
  ktxTexture_Destroy(ktxTexture(texture));
  return matches;
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

struct PackedSource {
  fs::path path;
  uint32_t width = 0u;
  uint32_t height = 0u;
  std::vector<LevelImage> levels;
  AlphaAnalysis alpha = {};
};

bool pack_texture_set_to_vkt(const std::vector<fs::path> &source_paths,
                             const fs::path &dst_path,
                             TextureClass texture_class, TextureShape shape,
                             const PackConfig &config) {
  const size_t source_count = source_paths.size();
  const bool valid_count =
      (shape == TextureShape::k2D && source_count == 1u) ||
      (shape == TextureShape::k2DArray && source_count > 1u) ||
      (shape == TextureShape::kCube && source_count == 6u) ||
      (shape == TextureShape::kCubeArray && source_count > 6u &&
       source_count % 6u == 0u);
  if (!valid_count || source_count > kMaxPhysicalLayers) {
    std::cerr << "Invalid source count " << source_count
              << " for the requested texture shape or runtime layer limit\n";
    return false;
  }

  std::vector<PackedSource> sources;
  sources.reserve(source_count);
  uint32_t width = 0u;
  uint32_t height = 0u;
  size_t mip_count = 0u;
  AlphaAnalysis aggregate_alpha = {};
  for (const fs::path &path : source_paths) {
    int source_width = 0;
    int source_height = 0;
    int channels = 0;
    if (!stbi_info(path.string().c_str(), &source_width, &source_height,
                   &channels) ||
        source_width <= 0 || source_height <= 0 ||
        source_width > static_cast<int>(kMaxTextureDimension) ||
        source_height > static_cast<int>(kMaxTextureDimension)) {
      std::cerr << "Texture extent exceeds the runtime contract: " << path
                << "\n";
      return false;
    }
    stbi_uc *loaded = stbi_load(path.string().c_str(), &source_width,
                                &source_height, &channels, 4);
    if (!loaded || source_width <= 0 || source_height <= 0 ||
        source_width > static_cast<int>(kMaxTextureDimension) ||
        source_height > static_cast<int>(kMaxTextureDimension)) {
      std::cerr << "Failed to decode texture: " << path << "\n";
      if (loaded) {
        stbi_image_free(loaded);
      }
      return false;
    }
    PackedSource source;
    source.path = path;
    source.width = static_cast<uint32_t>(source_width);
    source.height = static_cast<uint32_t>(source_height);
    source.alpha = analyze_alpha(loaded, source.width, source.height);
    if (texture_class == TextureClass::kNormalRg) {
      vkr_vkt_prepare_normal_rg_for_basis(
          loaded, static_cast<size_t>(source.width) * source.height);
    }
    source.levels = build_mip_chain_rgba8(loaded, source.width, source.height);
    stbi_image_free(loaded);
    if (sources.empty()) {
      width = source.width;
      height = source.height;
      mip_count = source.levels.size();
    } else if (source.width != width || source.height != height ||
               source.levels.size() != mip_count) {
      std::cerr << "Layered texture sources must have identical extents\n";
      return false;
    }
    aggregate_alpha.transparent_count += source.alpha.transparent_count;
    aggregate_alpha.intermediate_count += source.alpha.intermediate_count;
    sources.push_back(std::move(source));
  }
  aggregate_alpha.has_transparency = aggregate_alpha.transparent_count > 0u;
  aggregate_alpha.alpha_mask =
      aggregate_alpha.has_transparency &&
      static_cast<double>(aggregate_alpha.intermediate_count) /
              static_cast<double>(aggregate_alpha.transparent_count) <=
          kAlphaMaskIntermediateRatio;
  if (mip_count == 0u ||
      mip_count * source_count > static_cast<size_t>(kMaxUploadRegions)) {
    std::cerr << "Layered texture exceeds the runtime upload-region limit\n";
    return false;
  }
  if ((shape == TextureShape::kCube || shape == TextureShape::kCubeArray) &&
      width != height) {
    std::cerr << "Cubemap sources must be square\n";
    return false;
  }

  const bool cube =
      shape == TextureShape::kCube || shape == TextureShape::kCubeArray;
  const uint32_t face_count = cube ? 6u : 1u;
  const uint32_t layer_count = cube ? static_cast<uint32_t>(source_count / 6u)
                                    : static_cast<uint32_t>(source_count);
  const bool srgb_colorspace = texture_class_prefers_srgb(texture_class);
  ktxTextureCreateInfo create_info = {};
  create_info.vkFormat =
      srgb_colorspace ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
  create_info.baseWidth = width;
  create_info.baseHeight = height;
  create_info.baseDepth = 1u;
  create_info.numDimensions = 2u;
  create_info.numLevels = static_cast<uint32_t>(mip_count);
  create_info.numLayers = layer_count;
  create_info.numFaces = face_count;
  create_info.isArray = layer_count > 1u ? KTX_TRUE : KTX_FALSE;
  create_info.generateMipmaps = KTX_FALSE;

  ktxTexture2 *texture = nullptr;
  KTX_error_code result = ktxTexture2_Create(
      &create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
  if (result != KTX_SUCCESS || !texture) {
    std::cerr << "Failed to create KTX2 object: " << ktxErrorString(result)
              << "\n";
    return false;
  }

  bool success = false;
  do {
    for (uint32_t layer = 0u; layer < layer_count; ++layer) {
      for (uint32_t face = 0u; face < face_count; ++face) {
        const PackedSource &source = sources[layer * face_count + face];
        for (uint32_t mip = 0u; mip < source.levels.size(); ++mip) {
          const LevelImage &level = source.levels[mip];
          result = ktxTexture_SetImageFromMemory(
              ktxTexture(texture), mip, layer, face, level.pixels.data(),
              static_cast<ktx_size_t>(level.pixels.size()));
          if (result != KTX_SUCCESS) {
            break;
          }
        }
        if (result != KTX_SUCCESS) {
          break;
        }
      }
      if (result != KTX_SUCCESS) {
        break;
      }
    }
    if (result != KTX_SUCCESS ||
        !add_kv_string(texture, "vkr.colorspace_hint",
                       srgb_colorspace ? "srgb" : "linear") ||
        !add_kv_string(texture, "vkr.texture_class",
                       texture_class_metadata_value(texture_class)) ||
        !add_kv_bool(texture, "vkr.has_transparency",
                     aggregate_alpha.has_transparency) ||
        !add_kv_bool(texture, "vkr.alpha_mask", aggregate_alpha.alpha_mask) ||
        !add_kv_string(texture, "vkr.asset_version", "1") ||
        !add_kv_string(texture, "vkr.pack_settings",
                       pack_settings_identity(texture_class, shape, config))) {
      break;
    }

    if (config.write_source_hash) {
      uint64_t combined_hash = 0u;
      if (!combined_source_hash(source_paths, &combined_hash) ||
          !add_kv_string(texture, "vkr.source_hash",
                         to_hex_u64(combined_hash))) {
        break;
      }
    }

    ktxBasisParams basis_params = {};
    basis_params.structSize = sizeof(basis_params);
    basis_params.compressionLevel = KTX_ETC1S_DEFAULT_COMPRESSION_LEVEL;
    basis_params.uastc = KTX_TRUE;
    basis_params.threadCount = config.basis_threads;
    basis_params.uastcFlags = config.uastc_level;
    basis_params.uastcRDO = KTX_FALSE;
    result = ktxTexture2_CompressBasisEx(texture, &basis_params);
    if (result != KTX_SUCCESS) {
      std::cerr << "Failed to compress layered texture: "
                << ktxErrorString(result) << "\n";
      break;
    }

    std::error_code ec;
    if (!dst_path.parent_path().empty()) {
      fs::create_directories(dst_path.parent_path(), ec);
      if (ec) {
        std::cerr << "Failed to create output directory: " << ec.message()
                  << "\n";
        break;
      }
    }
    fs::path tmp_path = dst_path;
    const uint64_t unique_suffix =
        static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^
        static_cast<uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    tmp_path += ".tmp." + to_hex_u64(unique_suffix);
    result = ktxTexture_WriteToNamedFile(ktxTexture(texture),
                                         tmp_path.string().c_str());
    if (result != KTX_SUCCESS) {
      std::cerr << "Failed to write temporary output '" << tmp_path
                << "': " << ktxErrorString(result) << "\n";
      fs::remove(tmp_path, ec);
      break;
    }
    if (!publish_temporary_output(tmp_path, dst_path, &ec)) {
      std::cerr << "Failed to publish layered output '" << dst_path
                << "': " << ec.message() << "\n";
      fs::remove(tmp_path, ec);
      break;
    }
    success = true;
  } while (false);

  ktxTexture_Destroy(ktxTexture(texture));
  return success;
}

bool pack_texture_to_vkt(const fs::path &src_path, const fs::path &dst_path,
                         TextureClass texture_class, const PackConfig &config) {
  return pack_texture_set_to_vkt({src_path}, dst_path, texture_class,
                                 TextureShape::k2D, config);
}

std::vector<fs::path> discover_source_textures(const fs::path &root_dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root_dir, ec), end; it != end;
       it.increment(ec)) {
    if (ec) {
      continue;
    }
    if (!it->is_regular_file()) {
      continue;
    }
    const fs::path path = it->path();
    if (is_supported_source_extension(path)) {
      files.push_back(path);
    }
  }

  std::sort(files.begin(), files.end(),
            [](const fs::path &a, const fs::path &b) {
              return a.generic_string() < b.generic_string();
            });
  return files;
}

} // namespace

int main(int argc, char **argv) {
  PackConfig config = {};
  ParseResult parse_result = parse_args(argc, argv, config);
  if (parse_result == ParseResult::kHelp) {
    print_usage(argv[0]);
    return 0;
  }
  if (parse_result == ParseResult::kError) {
    print_usage(argv[0]);
    return 1;
  }

  config.basis_threads = resolve_basis_thread_count(config.basis_threads);
  stbi_set_flip_vertically_on_load(1);
  if (config.layered_mode) {
    for (const fs::path &layer : config.layers) {
      if (!fs::exists(layer) || !fs::is_regular_file(layer)) {
        std::cerr << "Layer source does not exist: " << layer << "\n";
        return 1;
      }
    }
    const TextureClass texture_class =
        config.texture_class_explicit
            ? config.texture_class
            : infer_texture_class(config.layers.front());
    if (should_skip_output(config.layers, config.output, texture_class,
                           config.shape, config)) {
      std::cout << "Layered output is up to date: " << config.output << "\n";
      return 0;
    }
    const bool packed = pack_texture_set_to_vkt(
        config.layers, config.output, texture_class, config.shape, config);
    std::cout << "vkt layered pack: output=" << config.output
              << " sources=" << config.layers.size()
              << " status=" << (packed ? "packed" : "failed") << "\n";
    return packed ? 0 : 1;
  }

  if (!fs::exists(config.input_dir) || !fs::is_directory(config.input_dir)) {
    std::cerr << "Input directory does not exist: " << config.input_dir << "\n";
    return config.strict ? 1 : 0;
  }

  const std::vector<fs::path> sources =
      discover_source_textures(config.input_dir);
  if (sources.empty()) {
    std::cout << "No source textures found under " << config.input_dir << "\n";
    return 0;
  }

  PackStats stats = {};
  stats.discovered = static_cast<uint32_t>(sources.size());
  log_progress_line(config.progress,
                    "Discovered " + std::to_string(stats.discovered) +
                        " source textures under " + config.input_dir.string());
  {
    std::ostringstream encode_config_line;
    encode_config_line << "Encode config: uastc_level="
                       << uastc_level_to_string(config.uastc_level)
                       << " basis_threads=" << config.basis_threads
                       << " source_hash="
                       << (config.write_source_hash ? "enabled" : "disabled");
    log_progress_line(config.progress, encode_config_line.str());
  }

  const auto start_time = std::chrono::steady_clock::now();

  for (size_t index = 0; index < sources.size(); ++index) {
    const fs::path &src_path = sources[index];
    const uint32_t current = static_cast<uint32_t>(index + 1u);

    std::error_code rel_ec;
    fs::path rel_path = fs::relative(src_path, config.input_dir, rel_ec);
    const std::string label =
        (rel_ec ? src_path.generic_string() : rel_path.generic_string());

    if (config.progress) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsed =
          std::chrono::duration<double>(now - start_time).count();
      const double avg =
          (current > 1u) ? (elapsed / double(current - 1u)) : 0.0;
      const double eta = avg * double(stats.discovered - (current - 1u));

      std::ostringstream header;
      header << "[" << current << "/" << stats.discovered << "] " << std::fixed
             << std::setprecision(1)
             << (100.0 * double(current) / double(stats.discovered)) << "% "
             << "packed=" << stats.packed << " skipped=" << stats.skipped
             << " failed=" << stats.failed
             << " elapsed=" << format_duration(elapsed)
             << " eta=" << format_duration(eta) << " :: " << label;
      log_progress_line(true, header.str());
    }

    const fs::path dst_path = src_path.string() + ".vkt";
    const TextureClass texture_class = infer_texture_class(src_path);
    if (should_skip_output({src_path}, dst_path, texture_class,
                           TextureShape::k2D, config)) {
      ++stats.skipped;
      log_progress_line(config.progress, "  - skip: content/settings match");
      continue;
    }

    if (pack_texture_to_vkt(src_path, dst_path, texture_class, config)) {
      ++stats.packed;
      log_progress_line(config.progress, "  - ok");
    } else {
      ++stats.failed;
      log_progress_line(config.progress, "  - failed");
    }
  }

  std::cout << "vkt pack summary: discovered=" << stats.discovered
            << " packed=" << stats.packed << " skipped=" << stats.skipped
            << " failed=" << stats.failed << "\n";

  if (config.strict && stats.failed > 0) {
    return 1;
  }
  return 0;
}
