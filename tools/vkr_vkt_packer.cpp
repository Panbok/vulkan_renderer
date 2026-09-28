#include "filesystem/vkr_filesystem_cpp.h"
#include "assets/vkr_ktx_file.h"
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <ktx-software/external/astc-encoder/Source/astcenc.h>
#include <ktx.h>
// Declarations only; basisu compiles the definitions once.
#define MINIZ_HEADER_FILE_ONLY
#include <ktx-software/external/basisu/encoder/basisu_miniz.h>
#include <vulkan/vulkan_core.h>

#include <stb_image.h>

#if defined(__APPLE__)
#include <AppleTextureEncoder.h>
#endif

#include "vkr_vkt_mips.h"
#include "vkr_vkt_normal_roughness.h"
#include "vkr_vkt_pack_contract.h"
#include "vkr_vkt_packer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
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

void destroy_ktx_texture(ktxTexture2 *texture) {
  ktxTexture_Destroy(ktxTexture(texture));
}

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

// One pack input: an image file, or RGBA8 pixels a caller converted, rows
// top first as an image decodes, named by their content hash in place of the
// file's bytes. A pixel source without pixels can only confirm a current
// output.
struct SourceImage {
  fs::path path;
  const uint8_t *pixels = nullptr;
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint64_t hash = 0u;
  bool converted = false;
};

std::vector<SourceImage> file_sources(const std::vector<fs::path> &paths) {
  std::vector<SourceImage> sources(paths.size());
  for (size_t i = 0; i < paths.size(); ++i) {
    sources[i].path = paths[i];
  }
  return sources;
}

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
  bool cutout = false;
  bool alpha_factor_explicit = false;
  float alpha_cutoff = 0.5f;
  float alpha_factor = 1.0f;
  bool normal_roughness = false;
  float normal_scale = 1.0f;
  float roughness_factor = 1.0f;
  bool roughness_source = false;
  // Preview tier: mip levels larger than this extent are not stored; 0 keeps
  // every level.
  uint32_t max_extent = 0u;
  // Native ASTC 4x4 blocks, which the runtime uploads without transcoding,
  // for hosts that sample them directly; transcodable UASTC otherwise.
  VkrVktEncoding encoding = VKR_VKT_ENCODING_UASTC;
};

bool is_astc(const PackConfig &config) {
  return config.encoding != VKR_VKT_ENCODING_UASTC;
}

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
      out_config.input_dir = vkr_filesystem_native_utf8_path(argv[++index]);
      continue;
    }
    if (arg == "--output") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for --output\n";
        return ParseResult::kError;
      }
      out_config.output = vkr_filesystem_native_utf8_path(argv[++index]);
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
      out_config.layers.emplace_back(
          vkr_filesystem_native_utf8_path(argv[++index]));
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
    if (arg == "--alpha-cutoff" || arg == "--alpha-factor") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for " << arg << "\n";
        return ParseResult::kError;
      }
      const char *value = argv[++index];
      char *end = nullptr;
      const float parsed = std::strtof(value, &end);
      if (end == value || *end != '\0' || !std::isfinite(parsed) ||
          parsed < 0.0f || parsed > 1.0f) {
        std::cerr << "Invalid " << arg << " (expected a number in [0, 1])\n";
        return ParseResult::kError;
      }
      if (arg == "--alpha-cutoff") {
        out_config.cutout = true;
        out_config.alpha_cutoff = parsed == 0.0f ? 0.0f : parsed;
      } else {
        out_config.alpha_factor_explicit = true;
        out_config.alpha_factor = parsed == 0.0f ? 0.0f : parsed;
      }
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
    if (arg == "--encoding") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for --encoding\n";
        return ParseResult::kError;
      }
      const std::string value = to_lower_ascii(argv[++index]);
      if (!vkr_vkt_parse_encoding(value.c_str(), &out_config.encoding)) {
        std::cerr << "Invalid --encoding value '" << value
                  << "' (expected uastc|astc|astc-fast; astc-fast needs "
                     "Apple's system encoder)\n";
        return ParseResult::kError;
      }
      continue;
    }
    if (arg == "--max-extent") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for --max-extent\n";
        return ParseResult::kError;
      }
      uint32_t parsed = 0u;
      if (!parse_uint32_nonzero(argv[++index], &parsed)) {
        std::cerr << "Invalid --max-extent value\n";
        return ParseResult::kError;
      }
      out_config.max_extent = parsed;
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
  if ((out_config.alpha_factor_explicit && !out_config.cutout) ||
      (out_config.cutout &&
       (!out_config.layered_mode || !out_config.texture_class_explicit ||
        (out_config.texture_class != TextureClass::kColorSrgb &&
         out_config.texture_class != TextureClass::kColorLinear)))) {
    std::cerr << "Cutout filtering requires --output, an explicit color "
                 "--texture-class and --alpha-cutoff; --alpha-factor is "
                 "optional\n";
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
         " [--encoding <uastc|astc|astc-fast>] [--max-extent <pixels>]"
         " [--source-hash|--no-source-hash]\n";
  std::cout << "Cutout color mips: --alpha-cutoff <0..1>"
               " [--alpha-factor <0..1>] (explicit output/color class only)\n";
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
  std::string ext = to_lower_ascii(path.extension().u8string());
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
  const std::string name = to_lower_ascii(path.filename().u8string());
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
                                              uint32_t width, uint32_t height,
                                              TextureClass texture_class,
                                              const PackConfig &config) {
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

    vkr_vkt_downsample_rgba8(previous.pixels.data(), previous.width,
                             previous.height, next.pixels.data(), next_width,
                             next_height,
                             texture_class_prefers_srgb(texture_class),
                             config.cutout && config.alpha_cutoff > 0.0f);

    levels.push_back(std::move(next));
  }

  if (config.cutout) {
    const uint32_t pass_byte =
        vkr_vkt_alpha_pass_byte(config.alpha_cutoff, config.alpha_factor);
    const uint64_t base_count = static_cast<uint64_t>(width) * height;
    const uint64_t base_covered =
        vkr_vkt_alpha_covered(base_pixels, base_count, pass_byte);
    for (size_t mip = 1; mip < levels.size(); ++mip) {
      LevelImage &level = levels[mip];
      vkr_vkt_preserve_alpha_coverage(
          level.pixels.data(), static_cast<size_t>(level.width) * level.height,
          pass_byte, base_covered, base_count);
    }
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

bool combined_source_hash(const std::vector<SourceImage> &sources,
                          uint64_t *out_hash) {
  if (!out_hash || sources.empty()) {
    return false;
  }
  uint64_t combined = kFnvOffsetBasis;
  for (const SourceImage &source : sources) {
    bool ok = true;
    const uint64_t source_hash =
        source.converted ? source.hash : fnv1a_file_hash(source.path, &ok);
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
           << ";class=" << texture_class_metadata_value(texture_class);
  if (config.encoding == VKR_VKT_ENCODING_ASTC) {
    settings << ";encoding=astc-4x4-fastest";
    if (texture_class == TextureClass::kNormalRg) {
      settings << ";astc_rg=alpha-one-db39-c1-v2";
    }
  } else if (config.encoding == VKR_VKT_ENCODING_ASTC_FAST) {
    settings << ";encoding=astc-4x4-system-equal-t12-v1";
    if (texture_class == TextureClass::kNormalRg) {
      settings << ";astc_rg=alpha-one";
    }
  } else {
    settings << ";uastc=" << uastc_level_to_string(config.uastc_level);
  }
  settings << ";mips=rgba8-area-srgb-v2;flip=vertical";
  if (config.max_extent) {
    settings << ";max_extent=" << config.max_extent;
  }
  if (texture_class == TextureClass::kNormalRg) {
    settings << ";basis_rg=source-ra-v1";
  }
  if (config.cutout) {
    settings << ";cutout=weighted-coverage-v" << VKR_VKT_CUTOUT_POLICY_VERSION
             << ";cutoff=" << std::hexfloat << config.alpha_cutoff
             << ";factor=" << config.alpha_factor;
  }
  if (config.normal_roughness) {
    settings << ";normal_roughness=vmf-alpha2-v"
             << VKR_VKT_NORMAL_ROUGHNESS_POLICY_VERSION
             << ";normal_scale=" << std::hexfloat << config.normal_scale;
    // The encoded normal depends only on its source and scale, so one
    // normal output serves every roughness input paired with it.
    if (texture_class != TextureClass::kNormalRg) {
      settings << ";roughness_factor=" << config.roughness_factor
               << ";roughness_source=" << config.roughness_source;
    }
    settings << ";uv=matched-extents-repeat-linear;variance_cap=0.25";
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

// Output reads and replacements share one lock: material workers can target
// one output, and Windows cannot replace a file another thread holds open.
std::mutex output_mutex;

// Whether `dst` already holds the pack of sources hashing to `source_hash`
// with these settings. The caller holds output_mutex.
bool output_matches(const fs::path &dst, uint64_t source_hash,
                    TextureClass texture_class, TextureShape shape,
                    const PackConfig &config) {
  if (!fs::exists(dst)) {
    return false;
  }
  ktxTexture2 *texture = nullptr;
  if (vkr_ktx_read_file(dst.u8string().c_str(), KTX_TEXTURE_CREATE_NO_FLAGS,
                        &texture) != KTX_SUCCESS ||
      !texture) {
    return false;
  }
  const std::unique_ptr<ktxTexture2, decltype(&destroy_ktx_texture)> owner(
      texture, destroy_ktx_texture);
  const bool matches =
      texture_metadata_equals(texture, "vkr.source_hash",
                              to_hex_u64(source_hash)) &&
      texture_metadata_equals(
          texture, "vkr.pack_settings",
          pack_settings_identity(texture_class, shape, config));
  return matches;
}

bool should_skip_output(const std::vector<SourceImage> &sources,
                        const fs::path &dst, TextureClass texture_class,
                        TextureShape shape, const PackConfig &config) {
  if (config.force || !config.write_source_hash) {
    return false;
  }
  uint64_t source_hash = 0u;
  if (!combined_source_hash(sources, &source_hash)) {
    return false;
  }
  const std::lock_guard<std::mutex> lock(output_mutex);
  return output_matches(dst, source_hash, texture_class, shape, config);
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
  uint32_t width = 0u;
  uint32_t height = 0u;
  std::vector<LevelImage> levels;
  AlphaAnalysis alpha = {};
};

bool load_source_rgba8(const fs::path &path, LevelImage &image) {
  std::ifstream source_file(path, std::ios::binary | std::ios::ate);
  if (!source_file) {
    std::cerr << "Failed to open texture: " << path << "\n";
    return false;
  }
  const std::streamoff source_size = source_file.tellg();
  if (source_size <= 0 || source_size > INT_MAX) {
    std::cerr << "Texture source exceeds the decoder byte limit: " << path
              << "\n";
    return false;
  }
  std::vector<uint8_t> encoded(static_cast<size_t>(source_size));
  source_file.seekg(0);
  if (!source_file.read(reinterpret_cast<char *>(encoded.data()),
                        source_size)) {
    std::cerr << "Failed to read texture: " << path << "\n";
    return false;
  }
  int source_width = 0;
  int source_height = 0;
  int channels = 0;
  if (!stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                             &source_width, &source_height, &channels) ||
      source_width <= 0 || source_height <= 0 ||
      source_width > static_cast<int>(kMaxTextureDimension) ||
      source_height > static_cast<int>(kMaxTextureDimension)) {
    std::cerr << "Texture extent exceeds the runtime contract: " << path
              << "\n";
    return false;
  }
  // The glTF cooker also decodes images on this thread. Keep its unflipped
  // decode convention separate from the packed texture's vertical flip.
  stbi_set_flip_vertically_on_load_thread(1);
  const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> loaded(
      stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                            &source_width, &source_height, &channels, 4),
      stbi_image_free);
  stbi_set_flip_vertically_on_load_thread(0);
  if (!loaded || source_width <= 0 || source_height <= 0 ||
      source_width > static_cast<int>(kMaxTextureDimension) ||
      source_height > static_cast<int>(kMaxTextureDimension)) {
    std::cerr << "Failed to decode texture: " << path << "\n";
    return false;
  }
  image.width = static_cast<uint32_t>(source_width);
  image.height = static_cast<uint32_t>(source_height);
  image.pixels.assign(loaded.get(),
                      loaded.get() +
                          static_cast<size_t>(image.width) * image.height * 4u);
  return true;
}

// Loads a source as the packer stores it: files through the decoder, and
// converted pixels flipped the same way.
bool load_source(const SourceImage &source, LevelImage &image) {
  if (!source.converted) {
    return load_source_rgba8(source.path, image);
  }
  if (!source.pixels || source.width == 0u || source.height == 0u ||
      source.width > kMaxTextureDimension ||
      source.height > kMaxTextureDimension) {
    return false;
  }
  const size_t row = static_cast<size_t>(source.width) * 4u;
  image.width = source.width;
  image.height = source.height;
  image.pixels.resize(row * source.height);
  for (uint32_t y = 0u; y < source.height; ++y) {
    std::memcpy(image.pixels.data() + row * y,
                source.pixels + row * (source.height - 1u - y), row);
  }
  return true;
}

// Creates the ASTC 4x4 texture that receives the encoded images of `source`.
ktxTexture2 *create_astc_texture(ktxTexture2 *source,
                                 KTX_error_code *out_result) {
  const bool srgb = source->vkFormat == VK_FORMAT_R8G8B8A8_SRGB;
  ktxTextureCreateInfo create_info = {};
  create_info.vkFormat =
      srgb ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
  create_info.baseWidth = source->baseWidth;
  create_info.baseHeight = source->baseHeight;
  create_info.baseDepth = source->baseDepth;
  create_info.numDimensions = source->numDimensions;
  create_info.numLevels = source->numLevels;
  create_info.numLayers = source->numLayers;
  create_info.numFaces = source->numFaces;
  create_info.isArray = source->isArray;
  create_info.generateMipmaps = KTX_FALSE;
  ktxTexture2 *encoded = nullptr;
  *out_result = ktxTexture2_Create(&create_info,
                                   KTX_TEXTURE_CREATE_ALLOC_STORAGE, &encoded);
  return *out_result == KTX_SUCCESS ? encoded : nullptr;
}

// Calls encode(width, height, texels, blocks, block_size) for every image of
// `source`, stopping at the first that returns false.
template <typename Encode>
bool encode_astc_images(ktxTexture2 *source, ktxTexture2 *encoded,
                        Encode encode) {
  for (uint32_t level = 0u; level < source->numLevels; ++level) {
    const uint32_t width = std::max(1u, source->baseWidth >> level);
    const uint32_t height = std::max(1u, source->baseHeight >> level);
    const ktx_size_t block_size =
        ktxTexture_GetImageSize(ktxTexture(encoded), level);
    for (uint32_t layer = 0u; layer < source->numLayers; ++layer) {
      for (uint32_t face = 0u; face < source->numFaces; ++face) {
        ktx_size_t in_offset = 0u;
        ktx_size_t out_offset = 0u;
        if (ktxTexture_GetImageOffset(ktxTexture(source), level, layer, face,
                                      &in_offset) != KTX_SUCCESS ||
            ktxTexture_GetImageOffset(ktxTexture(encoded), level, layer, face,
                                      &out_offset) != KTX_SUCCESS ||
            !encode(width, height, source->pData + in_offset,
                    encoded->pData + out_offset, block_size)) {
          return false;
        }
      }
    }
  }
  return true;
}

// Encodes every image of an RGBA8 texture to ASTC 4x4 with astcenc, as
// ktxTexture2_CompressAstcEx does at its "fastest" level, and moves the
// key/value data to the returned texture. Normals (R, G; B zero, A one) aim
// at 39 dB with one candidate instead of the preset's 43: on Bistro's baked
// normals that encoded 1.8 times faster and still scored above UASTC
// "faster" (41.2 against 40.9 dB in RG), where the preset kept searching
// blocks that never reach its target. libktx exposes neither setting.
ktxTexture2 *compress_astc(ktxTexture2 *source, TextureClass texture_class,
                           uint32_t thread_count, KTX_error_code *out_result) {
  ktxTexture2 *encoded = create_astc_texture(source, out_result);
  if (!encoded) {
    return nullptr;
  }
  std::unique_ptr<ktxTexture2, decltype(&destroy_ktx_texture)> owner(
      encoded, destroy_ktx_texture);

  const bool srgb = source->vkFormat == VK_FORMAT_R8G8B8A8_SRGB;
  astcenc_config astc_config;
  if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4u, 4u,
                          1u, ASTCENC_PRE_FASTEST, 0u,
                          &astc_config) != ASTCENC_SUCCESS) {
    *out_result = KTX_INVALID_OPERATION;
    return nullptr;
  }
  if (texture_class == TextureClass::kNormalRg) {
    astc_config.tune_db_limit = 39.0f;
    astc_config.tune_candidate_limit = 1u;
  }
  const uint32_t threads = std::max(1u, thread_count);
  astcenc_context *context = nullptr;
  if (astcenc_context_alloc(&astc_config, threads, &context) !=
      ASTCENC_SUCCESS) {
    *out_result = KTX_OUT_OF_MEMORY;
    return nullptr;
  }
  const astcenc_swizzle swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B,
                                   ASTCENC_SWZ_A};
  const bool encoded_all = encode_astc_images(
      source, encoded,
      [&](uint32_t width, uint32_t height, uint8_t *texels, uint8_t *blocks,
          ktx_size_t block_size) {
        void *slice = texels;
        astcenc_image image = {width, height, 1u, ASTCENC_TYPE_U8, &slice};
        std::atomic<int> failed{0};
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (uint32_t thread = 0u; thread < threads; ++thread) {
          workers.emplace_back([&, thread] {
            if (astcenc_compress_image(context, &image, &swizzle, blocks,
                                       block_size, thread) != ASTCENC_SUCCESS) {
              failed = 1;
            }
          });
        }
        for (std::thread &worker : workers) {
          worker.join();
        }
        astcenc_compress_reset(context);
        return failed == 0;
      });
  astcenc_context_free(context);
  if (!encoded_all) {
    *out_result = KTX_INVALID_OPERATION;
    return nullptr;
  }
  std::swap(ktxTexture(encoded)->kvDataHead, ktxTexture(source)->kvDataHead);
  *out_result = KTX_SUCCESS;
  return owner.release();
}

// Encodes every image of an RGBA8 texture to ASTC 4x4 with Apple's system
// encoder, which threads each image itself, and moves the key/value data to
// the returned texture. Channels weigh equally, as in astcenc, rather than
// as luminance, and blocks stop searching below a mean square error of
// 2^-12. On Bistro that encoded source colours at 3 times astcenc's
// "fastest" speed (52.8 against 55.3 dB) and paired normals at 2.2 times
// (36.6 against 40.4 dB in RG, where lower thresholds gain nothing); ADR-012
// records the classes. An opaque texture encodes opaque blocks, which spend
// no bits on alpha.
ktxTexture2 *compress_astc_system(ktxTexture2 *source,
                                  KTX_error_code *out_result) {
#if defined(__APPLE__)
  constexpr float kErrorThreshold = 1.0f / 4096.0f;
  ktxTexture2 *encoded = create_astc_texture(source, out_result);
  if (!encoded) {
    return nullptr;
  }
  std::unique_ptr<ktxTexture2, decltype(&destroy_ktx_texture)> owner(
      encoded, destroy_ktx_texture);

  bool opaque = true;
  const ktx_size_t data_size = ktxTexture_GetDataSize(ktxTexture(source));
  for (ktx_size_t i = 3u; i < data_size && opaque; i += 4u) {
    opaque = source->pData[i] == 255u;
  }
  at_encoder_t encoder = at_encoder_create(
      at_texel_format_rgba8_unorm, at_alpha_not_premultiplied,
      at_block_format_astc_4x4_ldr,
      opaque ? at_alpha_opaque : at_alpha_not_premultiplied, nullptr);
  if (!encoder) {
    *out_result = KTX_INVALID_OPERATION;
    return nullptr;
  }
  const bool encoded_all = encode_astc_images(
      source, encoded,
      [&](uint32_t width, uint32_t height, uint8_t *texels, uint8_t *blocks,
          ktx_size_t block_size) {
        const at_texel_region_t region = {
            texels,
            {width, height, 1u},
            static_cast<size_t>(width) * 4u,
            static_cast<size_t>(width) * height * 4u};
        const at_block_buffer_t destination = {
            blocks, static_cast<size_t>((width + 3u) / 4u) * 16u, block_size};
        return at_encoder_compress_texels(
                   encoder, &region, &destination, kErrorThreshold,
                   static_cast<at_flags_t>(at_flags_skip_error_calculation |
                                           at_flags_weight_channels_equally)) >=
               0.0f;
      });
  os_release(encoder);
  if (!encoded_all) {
    *out_result = KTX_INVALID_OPERATION;
    return nullptr;
  }
  std::swap(ktxTexture(encoded)->kvDataHead, ktxTexture(source)->kvDataHead);
  *out_result = KTX_SUCCESS;
  return owner.release();
#else
  (void)source;
  *out_result = KTX_UNSUPPORTED_FEATURE;
  return nullptr;
#endif
}

// A preview keeps the chain from the first level within its extent; levels
// before it are computed only to filter the ones kept.
size_t first_stored_level(uint32_t width, uint32_t height, size_t level_count,
                          const PackConfig &config) {
  size_t first_level = 0u;
  while (config.max_extent && first_level + 1u < level_count &&
         std::max(std::max(1u, width >> first_level),
                  std::max(1u, height >> first_level)) > config.max_extent) {
    ++first_level;
  }
  return first_level;
}

bool write_packed_sources(const std::vector<SourceImage> &source_images,
                          const std::vector<PackedSource> &sources,
                          const fs::path &dst_path, TextureClass texture_class,
                          TextureShape shape, const PackConfig &config,
                          AlphaAnalysis aggregate_alpha) {
  const size_t source_count = sources.size();
  const size_t first_level =
      first_stored_level(sources.front().levels.front().width,
                         sources.front().levels.front().height,
                         sources.front().levels.size(), config);
  const uint32_t width = sources.front().levels[first_level].width;
  const uint32_t height = sources.front().levels[first_level].height;
  const size_t mip_count = sources.front().levels.size() - first_level;
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
  const std::unique_ptr<ktxTexture2, decltype(&destroy_ktx_texture)> owner(
      texture, destroy_ktx_texture);
  // The native ASTC texture replaces `texture` for writing, when encoded.
  std::unique_ptr<ktxTexture2, decltype(&destroy_ktx_texture)> encoded(
      nullptr, destroy_ktx_texture);

  bool success = false;
  do {
    for (uint32_t layer = 0u; layer < layer_count; ++layer) {
      for (uint32_t face = 0u; face < face_count; ++face) {
        const PackedSource &source = sources[layer * face_count + face];
        for (uint32_t mip = 0u; mip < mip_count; ++mip) {
          const LevelImage &level = source.levels[first_level + mip];
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
    // Native ASTC normals keep X and Y in R and G, which shaders sample. The
    // alpha copy of G serves only Basis two-channel transcodes; a constant
    // alpha encodes faster and more accurately (Bistro pair normals: 1.4x,
    // +2 dB in RG).
    if (result == KTX_SUCCESS && is_astc(config) &&
        texture_class == TextureClass::kNormalRg) {
      uint8_t *data = ktxTexture_GetData(ktxTexture(texture));
      const ktx_size_t data_size = ktxTexture_GetDataSize(ktxTexture(texture));
      for (ktx_size_t i = 3u; i < data_size; i += 4u) {
        data[i] = 255u;
      }
    }
    if (result != KTX_SUCCESS ||
        !add_kv_string(texture, "vkr.colorspace_hint",
                       srgb_colorspace ? "srgb" : "linear") ||
        !add_kv_string(texture, "vkr.texture_class",
                       texture_class_metadata_value(texture_class)) ||
        !add_kv_bool(texture, "vkr.has_transparency",
                     aggregate_alpha.has_transparency) ||
        !add_kv_bool(texture, "vkr.alpha_mask",
                     config.cutout || aggregate_alpha.alpha_mask) ||
        !add_kv_string(texture, "vkr.asset_version", "1") ||
        !add_kv_string(texture, "vkr.pack_settings",
                       pack_settings_identity(texture_class, shape, config))) {
      break;
    }

    uint64_t combined_hash = 0u;
    if (config.write_source_hash) {
      if (!combined_source_hash(source_images, &combined_hash) ||
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
    if (config.encoding == VKR_VKT_ENCODING_ASTC) {
      // astcenc "fastest" beat UASTC "faster", the former final tier, on
      // Bistro's colours, paired normals and metal-roughness (52.6/40.1/49.2
      // against 52.2/38.5/47.4 dB) at 4 to 9 times its speed (ADR-077).
      encoded.reset(
          compress_astc(texture, texture_class, config.basis_threads, &result));
    } else if (config.encoding == VKR_VKT_ENCODING_ASTC_FAST) {
      encoded.reset(compress_astc_system(texture, &result));
    } else {
      result = ktxTexture2_CompressBasisEx(texture, &basis_params);
    }
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
    result = vkr_ktx_write_file(ktxTexture((encoded ? encoded.get() : texture)),
                                tmp_path.u8string().c_str());
    if (result != KTX_SUCCESS) {
      std::cerr << "Failed to write temporary output '" << tmp_path
                << "': " << ktxErrorString(result) << "\n";
      fs::remove(tmp_path, ec);
      break;
    }

    // Workers encode in parallel but publish in turn. Another worker may have
    // published this identity since the skip check; its output stays.
    const std::lock_guard<std::mutex> lock(output_mutex);
    if (!config.force && config.write_source_hash &&
        output_matches(dst_path, combined_hash, texture_class, shape, config)) {
      fs::remove(tmp_path, ec);
      success = true;
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

  return success;
}

bool pack_texture_set_to_vkt(const std::vector<SourceImage> &source_images,
                             const fs::path &dst_path,
                             TextureClass texture_class, TextureShape shape,
                             const PackConfig &config) {
  const size_t source_count = source_images.size();
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
  for (const SourceImage &source_image : source_images) {
    LevelImage image;
    if (!load_source(source_image, image)) {
      return false;
    }
    PackedSource source;
    source.width = image.width;
    source.height = image.height;
    source.alpha =
        analyze_alpha(image.pixels.data(), source.width, source.height);
    if (texture_class == TextureClass::kNormalRg) {
      vkr_vkt_prepare_normal_rg_for_basis(image.pixels.data(),
                                          static_cast<size_t>(source.width) *
                                              source.height);
    }
    source.levels = build_mip_chain_rgba8(image.pixels.data(), source.width,
                                          source.height, texture_class, config);
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

  return write_packed_sources(source_images, sources, dst_path, texture_class,
                              shape, config, aggregate_alpha);
}

// Both callers reduce the same area footprint: original decoded texels for
// mip 1, retained moments thereafter. No full-resolution moment buffer is
// needed.
template <typename Sample>
std::vector<VkrVktMaterialMoment>
reduce_material_moments(uint32_t width, uint32_t height, uint32_t next_width,
                        uint32_t next_height, Sample sample) {
  std::vector<VkrVktMaterialMoment> result(static_cast<size_t>(next_width) *
                                           next_height);
  const double inverse_area = 1.0 / (static_cast<double>(width) * height);
  for (uint32_t y = 0; y < next_height; ++y) {
    const uint32_t top = y * height;
    const uint32_t bottom = top + height;
    for (uint32_t x = 0; x < next_width; ++x) {
      const uint32_t left = x * width;
      const uint32_t right = left + width;
      VkrVktMaterialMoment sum = {};
      for (uint32_t sy = top / next_height;
           sy < (bottom + next_height - 1u) / next_height; ++sy) {
        const uint32_t overlap_y = std::min(bottom, (sy + 1u) * next_height) -
                                   std::max(top, sy * next_height);
        for (uint32_t sx = left / next_width;
             sx < (right + next_width - 1u) / next_width; ++sx) {
          const uint32_t overlap_x = std::min(right, (sx + 1u) * next_width) -
                                     std::max(left, sx * next_width);
          const double weight = static_cast<double>(overlap_x) * overlap_y;
          const VkrVktMaterialMoment value =
              sample(static_cast<size_t>(sy) * width + sx);
          sum.x += value.x * weight;
          sum.y += value.y * weight;
          sum.z += value.z * weight;
          sum.roughness_fourth += value.roughness_fourth * weight;
        }
      }
      sum.x *= inverse_area;
      sum.y *= inverse_area;
      sum.z *= inverse_area;
      sum.roughness_fourth *= inverse_area;
      result[static_cast<size_t>(y) * next_width + x] = sum;
    }
  }
  return result;
}

void build_normal_roughness_mips(const LevelImage &normal_image,
                                 const LevelImage &roughness_image,
                                 const PackConfig &config, PackedSource &normal,
                                 PackedSource &roughness) {
  normal.width = roughness.width = normal_image.width;
  normal.height = roughness.height = normal_image.height;
  // Keep the existing area-filtered R/B/A contract. Only roughness G is
  // coupled.
  roughness.levels =
      build_mip_chain_rgba8(roughness_image.pixels.data(), roughness.width,
                            roughness.height, TextureClass::kDataMask, config);
  normal.levels.reserve(roughness.levels.size());
  std::vector<VkrVktMaterialMoment> moments;
  const size_t first_level = first_stored_level(
      normal.width, normal.height, roughness.levels.size(), config);
  const auto base_sample = [&](size_t index) {
    return vkr_vkt_material_moment(normal_image.pixels.data() + index * 4u,
                                   roughness_image.pixels[index * 4u + 1u],
                                   config.normal_scale,
                                   config.roughness_factor);
  };
  for (size_t mip = 0; mip < roughness.levels.size(); ++mip) {
    LevelImage &roughness_level = roughness.levels[mip];
    LevelImage normal_level;
    normal_level.width = roughness_level.width;
    normal_level.height = roughness_level.height;
    if (mip >= first_level) {
      normal_level.pixels.resize(roughness_level.pixels.size());
    }
    if (mip == 1u) {
      moments = reduce_material_moments(normal.width, normal.height,
                                        normal_level.width, normal_level.height,
                                        base_sample);
    } else if (mip > 1u) {
      const LevelImage &previous = normal.levels.back();
      auto next = reduce_material_moments(
          previous.width, previous.height, normal_level.width,
          normal_level.height, [&](size_t index) { return moments[index]; });
      moments = std::move(next);
    }
    const size_t count =
        static_cast<size_t>(normal_level.width) * normal_level.height;
    if (mip < first_level) {
      // Dropped by the preview extent: its moments still feed the next
      // level, but its encoded texels are never stored.
    } else if (mip == 0u) {
      for (size_t index = 0; index < count; ++index) {
        vkr_vkt_encode_material_moment(
            base_sample(index), normal_level.pixels.data() + index * 4u,
            &roughness_level.pixels[index * 4u + 1u]);
      }
    } else {
      for (size_t index = 0; index < count; ++index) {
        vkr_vkt_encode_material_moment(
            moments[index], normal_level.pixels.data() + index * 4u,
            &roughness_level.pixels[index * 4u + 1u]);
      }
    }
    normal.levels.push_back(std::move(normal_level));
  }
}

bool pack_texture_to_vkt(const fs::path &src_path, const fs::path &dst_path,
                         TextureClass texture_class, const PackConfig &config) {
  return pack_texture_set_to_vkt(file_sources({src_path}), dst_path,
                                 texture_class,
                                 TextureShape::k2D, config);
}

bool discover_source_textures(const fs::path &root_dir,
                              std::vector<fs::path> *files,
                              std::error_code *error) {
  fs::recursive_directory_iterator it(root_dir, *error), end;
  while (!*error && it != end) {
    const bool regular = it->is_regular_file(*error);
    if (*error) {
      break;
    }
    if (regular && is_supported_source_extension(it->path())) {
      files->push_back(it->path());
    }
    it.increment(*error);
  }
  if (*error) {
    return false;
  }
  std::sort(files->begin(), files->end(),
            [](const fs::path &a, const fs::path &b) {
              return a.generic_string() < b.generic_string();
            });
  return true;
}

} // namespace

static std::atomic<int> g_vkr_vkt_preview_tier{0};

void vkr_vkt_set_preview_tier(int preview) {
  g_vkr_vkt_preview_tier.store(preview ? 1 : 0);
}

int vkr_vkt_preview_tier(void) { return g_vkr_vkt_preview_tier.load(); }

int vkr_vkt_parse_encoding(const char *name, VkrVktEncoding *out) {
  if (!name || !out) {
    return 0;
  }
  if (std::strcmp(name, "uastc") == 0) {
    *out = VKR_VKT_ENCODING_UASTC;
    return 1;
  }
  if (std::strcmp(name, "astc") == 0) {
    *out = VKR_VKT_ENCODING_ASTC;
    return 1;
  }
#if defined(__APPLE__)
  if (std::strcmp(name, "astc-fast") == 0) {
    *out = VKR_VKT_ENCODING_ASTC_FAST;
    return 1;
  }
#endif
  return 0;
}

static std::atomic<int> g_vkr_vkt_encoding{VKR_VKT_ENCODING_UASTC};

void vkr_vkt_set_encoding(VkrVktEncoding encoding) {
  g_vkr_vkt_encoding.store(encoding);
}

VkrVktEncoding vkr_vkt_encoding(void) {
  return static_cast<VkrVktEncoding>(g_vkr_vkt_encoding.load());
}

const char *vkr_vkt_variant_suffix(void) {
  static const char *const suffixes[6] = {
      "",      ".preview",         ".astc", ".astc.preview",
      ".astc-fast", ".astc-fast.preview"};
  return suffixes[vkr_vkt_encoding() * 2 + (vkr_vkt_preview_tier() ? 1 : 0)];
}

void *vkr_vkt_encode_png_rgba8(const void *pixels, int width, int height,
                               size_t *out_size) {
  // The writer stores 16-bit extents. Level 2 is greedy parsing with six
  // probes: about three times the speed of stb_image_write and smaller output
  // on converted Bistro images.
  constexpr buminiz::mz_uint kPngLevel = 2u;
  if (!out_size) {
    return nullptr;
  }
  *out_size = 0u;
  if (!pixels || width <= 0 || height <= 0 || width > 65535 || height > 65535) {
    return nullptr;
  }
  return buminiz::tdefl_write_image_to_png_file_in_memory_ex(
      pixels, width, height, 4, out_size, kPngLevel, MZ_FALSE);
}

void vkr_vkt_free_png(void *bytes) { buminiz::mz_free(bytes); }

namespace {

// Settings of the in-process packs: the process tier and encoding.
PackConfig process_pack_config() {
  PackConfig config;
  if (vkr_vkt_preview_tier()) {
    config.uastc_level = KTX_PACK_UASTC_LEVEL_FASTEST;
    config.max_extent = VKR_VKT_PREVIEW_MAX_EXTENT;
  }
  config.encoding = vkr_vkt_encoding();
  config.basis_threads = resolve_basis_thread_count(config.basis_threads);
  return config;
}

bool source_valid(const VkrVktSource *source) {
  return source && ((source->path && source->path[0] && !source->pixels) ||
                    (!source->path && source->width && source->height));
}

SourceImage source_image(const VkrVktSource &source) {
  SourceImage image;
  if (source.path) {
    image.path = vkr_filesystem_native_utf8_path(source.path);
    return image;
  }
  image.pixels = source.pixels;
  image.width = source.width;
  image.height = source.height;
  image.hash = source.hash;
  image.converted = true;
  return image;
}

// Packs one 2D texture from `source` unless `destination` is current.
VkrVktPackResult pack_single(const VkrVktSource *source,
                             const fs::path &destination,
                             TextureClass texture_class,
                             const PackConfig &config) {
  const std::vector<SourceImage> sources = {source_image(*source)};
  if (should_skip_output(sources, destination, texture_class,
                         TextureShape::k2D, config)) {
    return VKR_VKT_PACK_SUCCESS;
  }
  if (sources.front().converted && !sources.front().pixels) {
    return VKR_VKT_PACK_STALE;
  }
  return pack_texture_set_to_vkt(sources, destination, texture_class,
                                 TextureShape::k2D, config)
             ? VKR_VKT_PACK_SUCCESS
             : VKR_VKT_PACK_FAILED;
}

} // namespace

VkrVktPackResult vkr_vkt_pack_image(const VkrVktSource *source,
                                    const char *texture_class,
                                    const char *output) {
  TextureClass parsed_class = TextureClass::kColorSrgb;
  if (!source_valid(source) || !texture_class || !output || !output[0] ||
      !parse_texture_class_option(texture_class, &parsed_class)) {
    return VKR_VKT_PACK_FAILED;
  }
  try {
    return pack_single(source, vkr_filesystem_native_utf8_path(output),
                       parsed_class, process_pack_config());
  } catch (const std::exception &error) {
    std::cerr << "Texture packing failed: " << error.what() << "\n";
    return VKR_VKT_PACK_FAILED;
  }
}

VkrVktPackResult vkr_vkt_pack_cutout(const VkrVktSource *source,
                                     const char *output, float cutoff,
                                     float factor) {
  if (!source_valid(source) || !output || !output[0] ||
      !std::isfinite(cutoff) || !std::isfinite(factor) || cutoff < 0.0f ||
      cutoff > 1.0f || factor < 0.0f || factor > 1.0f) {
    return VKR_VKT_PACK_FAILED;
  }
  try {
    PackConfig config = process_pack_config();
    config.cutout = true;
    config.alpha_cutoff = cutoff == 0.0f ? 0.0f : cutoff;
    config.alpha_factor = factor == 0.0f ? 0.0f : factor;
    return pack_single(source, vkr_filesystem_native_utf8_path(output),
                       TextureClass::kColorSrgb, config);
  } catch (const std::exception &error) {
    std::cerr << "Cutout packing failed: " << error.what() << "\n";
    return VKR_VKT_PACK_FAILED;
  }
}

VkrVktPackResult vkr_vkt_pack_normal_roughness(
    const char *normal_source, const VkrVktSource *roughness_source,
    const char *normal_output, const char *roughness_output,
    float normal_scale, float roughness_factor) {
  if (!normal_source || !normal_source[0] || !normal_output ||
      !normal_output[0] || !roughness_output || !roughness_output[0] ||
      (roughness_source && !source_valid(roughness_source)) ||
      !std::isfinite(normal_scale) || !std::isfinite(roughness_factor) ||
      roughness_factor < 0.0f || roughness_factor > 1.0f) {
    return VKR_VKT_PACK_FAILED;
  }
  try {
    const fs::path normal_path = vkr_filesystem_native_utf8_path(normal_output);
    const fs::path roughness_path =
        vkr_filesystem_native_utf8_path(roughness_output);
    std::vector<SourceImage> sources(1);
    sources.front().path = vkr_filesystem_native_utf8_path(normal_source);
    if (roughness_source) {
      sources.push_back(source_image(*roughness_source));
    }
    // Publishing a pair must never overwrite either source or the other output.
    const fs::path normal_absolute =
        fs::absolute(normal_path).lexically_normal();
    const fs::path roughness_absolute =
        fs::absolute(roughness_path).lexically_normal();
    if (normal_absolute == roughness_absolute) {
      return VKR_VKT_PACK_FAILED;
    }
    for (const SourceImage &source : sources) {
      if (source.converted) {
        continue;
      }
      const fs::path absolute = fs::absolute(source.path).lexically_normal();
      if (absolute == normal_absolute || absolute == roughness_absolute) {
        return VKR_VKT_PACK_FAILED;
      }
    }
    PackConfig config = process_pack_config();
    config.normal_roughness = true;
    config.normal_scale = normal_scale == 0.0f ? 0.0f : normal_scale;
    config.roughness_factor =
        roughness_factor == 0.0f ? 0.0f : roughness_factor;
    config.roughness_source = roughness_source != nullptr;
    // The normal output records only its own source; see
    // pack_settings_identity.
    const std::vector<SourceImage> normal_sources = {sources.front()};
    const bool normal_cached =
        should_skip_output(normal_sources, normal_path, TextureClass::kNormalRg,
                           TextureShape::k2D, config);
    const bool roughness_cached =
        should_skip_output(sources, roughness_path, TextureClass::kDataMask,
                           TextureShape::k2D, config);
    if (normal_cached && roughness_cached) {
      return VKR_VKT_PACK_SUCCESS;
    }
    if (roughness_source && sources.back().converted &&
        !sources.back().pixels) {
      return VKR_VKT_PACK_STALE;
    }
    LevelImage normal_image;
    LevelImage roughness_image;
    if (!load_source(sources[0], normal_image)) {
      return VKR_VKT_PACK_FAILED;
    }
    if (roughness_source) {
      if (!load_source(sources[1], roughness_image)) {
        return VKR_VKT_PACK_FAILED;
      }
      if (normal_image.width != roughness_image.width ||
          normal_image.height != roughness_image.height) {
        return VKR_VKT_PACK_INCOMPATIBLE;
      }
    } else {
      roughness_image.width = normal_image.width;
      roughness_image.height = normal_image.height;
      roughness_image.pixels.assign(normal_image.pixels.size(), 255u);
    }
    std::vector<PackedSource> normal(1);
    std::vector<PackedSource> roughness(1);
    build_normal_roughness_mips(normal_image, roughness_image, config,
                                normal.front(), roughness.front());
    // Normal alpha is the Basis RG carrier, not surface transparency.
    const AlphaAnalysis roughness_alpha =
        analyze_alpha(roughness_image.pixels.data(), roughness_image.width,
                      roughness_image.height);
    if ((!normal_cached &&
         !write_packed_sources(normal_sources, normal, normal_path,
                               TextureClass::kNormalRg, TextureShape::k2D,
                               config, {})) ||
        (!roughness_cached &&
         !write_packed_sources(sources, roughness, roughness_path,
                               TextureClass::kDataMask, TextureShape::k2D,
                               config, roughness_alpha))) {
      return VKR_VKT_PACK_FAILED;
    }
    return VKR_VKT_PACK_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "Normal/roughness packing failed: " << error.what() << "\n";
    return VKR_VKT_PACK_FAILED;
  }
}

const char *vkr_vkt_infer_texture_class(const char *path) {
  switch (infer_texture_class(fs::u8path(path))) {
  case TextureClass::kNormalRg:
    return "normal-rg";
  case TextureClass::kDataMask:
    return "data-mask";
  case TextureClass::kColorLinear:
    return "color-linear";
  default:
    return "color-srgb";
  }
}

int vkr_vkt_is_supported_source(const char *path) {
  return is_supported_source_extension(fs::u8path(path)) ? 1 : 0;
}

int vkr_vkt_packer_main(int argc, char **argv) {
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
    const std::vector<SourceImage> layers = file_sources(config.layers);
    if (should_skip_output(layers, config.output, texture_class, config.shape,
                           config)) {
      std::cout << "Layered output is up to date: " << config.output << "\n";
      return 0;
    }
    const bool packed = pack_texture_set_to_vkt(
        layers, config.output, texture_class, config.shape, config);
    std::cout << "vkt layered pack: output=" << config.output
              << " sources=" << config.layers.size()
              << " status=" << (packed ? "packed" : "failed") << "\n";
    return packed ? 0 : 1;
  }

  if (!fs::exists(config.input_dir) || !fs::is_directory(config.input_dir)) {
    std::cerr << "Input directory does not exist: " << config.input_dir << "\n";
    return config.strict ? 1 : 0;
  }

  std::vector<fs::path> sources;
  std::error_code discovery_error;
  if (!discover_source_textures(config.input_dir, &sources, &discovery_error)) {
    std::cerr << "Unable to enumerate textures under " << config.input_dir
              << ": " << discovery_error.message() << "\n";
    return 1;
  }
  if (sources.empty()) {
    std::cout << "No source textures found under " << config.input_dir << "\n";
    return 0;
  }

  PackStats stats = {};
  stats.discovered = static_cast<uint32_t>(sources.size());
  log_progress_line(config.progress, "Discovered " +
                                         std::to_string(stats.discovered) +
                                         " source textures under " +
                                         config.input_dir.u8string());
  {
    std::ostringstream encode_config_line;
    encode_config_line << "Encode config: "
                       << (is_astc(config)
                               ? std::string("encoding=") +
                                     (config.encoding ==
                                              VKR_VKT_ENCODING_ASTC_FAST
                                          ? "astc-4x4-system"
                                          : "astc-4x4-fastest")
                               : std::string("uastc_level=") +
                                     uastc_level_to_string(config.uastc_level))
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

    const fs::path dst_path =
        vkr_filesystem_native_utf8_path(src_path.u8string() + ".vkt");
    const TextureClass texture_class = infer_texture_class(src_path);
    if (should_skip_output(file_sources({src_path}), dst_path, texture_class,
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
