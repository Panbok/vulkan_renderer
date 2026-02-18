#include <ktx.h>
#include <vulkan/vulkan_core.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr float kAlphaMaskIntermediateRatio = 0.30f;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct AlphaAnalysis {
  bool has_transparency = false;
  bool alpha_mask = false;
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

struct PackConfig {
  fs::path input_dir;
  bool strict = false;
  bool force = false;
  bool verbose = false;
  bool progress = true;
  uint32_t basis_threads = 0;
  ktx_pack_uastc_flags uastc_level = KTX_PACK_UASTC_LEVEL_FASTER;
  bool write_source_hash = true;
};

enum class ParseResult { kOk, kHelp, kError };
enum class TextureClass {
  kColorSrgb = 0,
  kColorLinear,
  kNormalRg,
  kDataMask,
};

std::string to_lower_ascii(std::string value);

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

  if (out_config.input_dir.empty()) {
    std::cerr << "Missing required argument --input-dir\n";
    return ParseResult::kError;
  }

  return ParseResult::kOk;
}

void print_usage(const char *program_name) {
  std::cout << "Usage: " << program_name
            << " --input-dir <path> [--strict] [--force] [--verbose]"
               " [--progress|--no-progress]"
               " [--basis-threads <auto|n>]"
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
  static const std::array<const char *, 3> kNormalTokens = {"normal", "_n.",
                                                            "norm"};
  for (const char *token : kNormalTokens) {
    if (name.find(token) != std::string::npos) {
      return TextureClass::kNormalRg;
    }
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

bool should_skip_output(const fs::path &src, const fs::path &dst, bool force) {
  if (force || !fs::exists(dst)) {
    return false;
  }

  std::error_code ec_src;
  std::error_code ec_dst;
  const auto src_time = fs::last_write_time(src, ec_src);
  const auto dst_time = fs::last_write_time(dst, ec_dst);
  if (ec_src || ec_dst) {
    return false;
  }
  return dst_time >= src_time;
}

bool pack_texture_to_vkt(const fs::path &src_path, const fs::path &dst_path,
                         TextureClass texture_class, const PackConfig &config) {
  const bool srgb_colorspace = texture_class_prefers_srgb(texture_class);
  log_progress_line(config.progress,
                    "  - decode: " + src_path.generic_string());
  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc *loaded =
      stbi_load(src_path.string().c_str(), &width, &height, &channels, 4);
  if (!loaded || width <= 0 || height <= 0) {
    std::cerr << "Failed to decode texture: " << src_path << "\n";
    if (loaded) {
      stbi_image_free(loaded);
    }
    return false;
  }

  log_progress_line(config.progress, "  - mips: build chain");
  std::vector<LevelImage> levels = build_mip_chain_rgba8(
      loaded, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
  const AlphaAnalysis alpha = analyze_alpha(
      loaded, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
  stbi_image_free(loaded);

  ktxTextureCreateInfo create_info = {};
  create_info.vkFormat =
      srgb_colorspace ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
  create_info.baseWidth = static_cast<ktx_uint32_t>(width);
  create_info.baseHeight = static_cast<ktx_uint32_t>(height);
  create_info.baseDepth = 1;
  create_info.numDimensions = 2;
  create_info.numLevels = static_cast<ktx_uint32_t>(levels.size());
  create_info.numLayers = 1;
  create_info.numFaces = 1;
  create_info.isArray = KTX_FALSE;
  create_info.generateMipmaps = KTX_FALSE;

  ktxTexture2 *texture = nullptr;
  KTX_error_code result = ktxTexture2_Create(
      &create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
  if (result != KTX_SUCCESS || !texture) {
    std::cerr << "Failed to create KTX2 object for '" << src_path
              << "': " << ktxErrorString(result) << "\n";
    return false;
  }

  bool success = false;
  do {
    log_progress_line(config.progress, "  - ktx2: write mip payloads");
    for (uint32_t level_index = 0; level_index < levels.size(); ++level_index) {
      const LevelImage &level = levels[level_index];
      result = ktxTexture_SetImageFromMemory(
          ktxTexture(texture), level_index, 0, 0, level.pixels.data(),
          static_cast<ktx_size_t>(level.pixels.size()));
      if (result != KTX_SUCCESS) {
        std::cerr << "Failed to set mip level " << level_index << " for '"
                  << src_path << "': " << ktxErrorString(result) << "\n";
        break;
      }
    }
    if (result != KTX_SUCCESS) {
      break;
    }

    if (!add_kv_string(texture, "vkr.colorspace_hint",
                       srgb_colorspace ? "srgb" : "linear") ||
        !add_kv_string(texture, "vkr.texture_class",
                       texture_class_metadata_value(texture_class)) ||
        !add_kv_bool(texture, "vkr.has_transparency", alpha.has_transparency) ||
        !add_kv_bool(texture, "vkr.alpha_mask", alpha.alpha_mask) ||
        !add_kv_string(texture, "vkr.asset_version", "1")) {
      std::cerr << "Failed to set metadata for '" << src_path << "'\n";
      break;
    }

    if (config.write_source_hash) {
      bool hash_ok = false;
      const uint64_t source_hash = fnv1a_file_hash(src_path, &hash_ok);
      if (hash_ok) {
        if (!add_kv_string(texture, "vkr.source_hash",
                           to_hex_u64(source_hash))) {
          std::cerr << "Failed to set source hash metadata for '" << src_path
                    << "'\n";
          break;
        }
      } else if (config.verbose) {
        std::cerr << "Failed to hash source '" << src_path
                  << "' (continuing without vkr.source_hash)\n";
      }
    }

    {
      std::ostringstream compress_line;
      compress_line << "  - compress: UASTC (basis, level="
                    << uastc_level_to_string(config.uastc_level)
                    << ", threads=" << config.basis_threads << ")";
      log_progress_line(config.progress, compress_line.str());
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
      std::cerr << "Failed to compress texture '" << src_path
                << "' as UASTC: " << ktxErrorString(result) << "\n";
      break;
    }

    log_progress_line(config.progress,
                      "  - write: " + dst_path.generic_string());
    fs::path tmp_path = dst_path;
    tmp_path += ".tmp";
    result = ktxTexture_WriteToNamedFile(ktxTexture(texture),
                                         tmp_path.string().c_str());
    if (result != KTX_SUCCESS) {
      std::cerr << "Failed to write temporary output '" << tmp_path
                << "': " << ktxErrorString(result) << "\n";
      break;
    }

    std::error_code ec_remove;
    fs::remove(dst_path, ec_remove);
    std::error_code ec_rename;
    fs::rename(tmp_path, dst_path, ec_rename);
    if (ec_rename) {
      std::cerr << "Failed to move temporary output to destination '"
                << dst_path << "': " << ec_rename.message() << "\n";
      fs::remove(tmp_path, ec_remove);
      break;
    }

    if (config.verbose) {
      std::cout << "Packed " << src_path << " -> " << dst_path << " ("
                << levels.size()
                << " mips, colorspace=" << (srgb_colorspace ? "srgb" : "linear")
                << ", class=" << texture_class_metadata_value(texture_class)
                << ", uastc_level=" << uastc_level_to_string(config.uastc_level)
                << ", basis_threads=" << config.basis_threads
                << ", source_hash="
                << (config.write_source_hash ? "enabled" : "disabled") << ")\n";
    }
    success = true;
  } while (false);

  ktxTexture_Destroy(ktxTexture(texture));
  return success;
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

  if (!fs::exists(config.input_dir) || !fs::is_directory(config.input_dir)) {
    std::cerr << "Input directory does not exist: " << config.input_dir << "\n";
    return config.strict ? 1 : 0;
  }

  config.basis_threads = resolve_basis_thread_count(config.basis_threads);

  stbi_set_flip_vertically_on_load(1);

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
    if (should_skip_output(src_path, dst_path, config.force)) {
      ++stats.skipped;
      log_progress_line(config.progress, "  - skip: up-to-date");
      continue;
    }

    const TextureClass texture_class = infer_texture_class(src_path);
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
