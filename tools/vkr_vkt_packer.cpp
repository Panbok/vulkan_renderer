#include "assets/vkr_ktx_file.h"
#include "filesystem/vkr_filesystem_cpp.h"
extern "C" {
#include "assets/vkr_image_decode.h"
#include "core/vkr_hash.h"
}
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
#if defined(VKR_VKT_HAS_BC7E)
#include <bc7e_ispc.h>
#include <rgbcx.h>
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
#include <condition_variable>
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
#include <unordered_map>
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
// Paired bakes of at least this many base texels tabulate their base moments
// over the 65,536 normal X/Y byte pairs (build_normal_roughness_mips).
constexpr size_t kNormalMomentTableMinTexels = 512u * 512u;

void destroy_ktx_texture(ktxTexture2 *texture) {
  ktxTexture_Destroy(ktxTexture(texture));
}

struct AlphaAnalysis {
  bool has_transparency = false;
  bool alpha_mask = false;
  uint64_t transparent_count = 0u;
  uint64_t intermediate_count = 0u;
};

// Freed blocks of at least kReusedBlockMinBytes, kept for the next buffer
// of about their size. A texture's decoded image, mip levels and moments
// took fresh memory from the OS and gave it back each time, and Windows
// zeroes every page it hands out again: 15 million page faults and about a
// tenth of a Bistro finalization's CPU. Blocks stay mapped up to
// kReusedBlockCacheBytes in all; a freed block beyond that is released.
constexpr size_t kReusedBlockMinBytes = size_t(1) << 20u;
constexpr size_t kReusedBlockCacheBytes = size_t(1) << 30u;

class ReusedBlocks {
public:
  static void *allocate(size_t bytes) {
    if (bytes < kReusedBlockMinBytes) {
      return ::operator new(bytes);
    }
    {
      const std::lock_guard<std::mutex> lock(mutex());
      // The smallest cached block that holds `bytes` and wastes at most a
      // quarter of it.
      auto best = free_blocks().end();
      for (auto block = free_blocks().begin(); block != free_blocks().end();
           ++block) {
        if (block->bytes >= bytes && block->bytes - bytes <= bytes / 4u &&
            (best == free_blocks().end() || block->bytes < best->bytes)) {
          best = block;
        }
      }
      if (best != free_blocks().end()) {
        void *memory = best->memory;
        cached_bytes() -= best->bytes;
        live_blocks()[memory] = best->bytes;
        *best = free_blocks().back();
        free_blocks().pop_back();
        return memory;
      }
    }
    void *memory = ::operator new(bytes);
    const std::lock_guard<std::mutex> lock(mutex());
    live_blocks()[memory] = bytes;
    return memory;
  }

  static void release(void *memory, size_t bytes) {
    if (bytes < kReusedBlockMinBytes) {
      ::operator delete(memory);
      return;
    }
    std::unique_lock<std::mutex> lock(mutex());
    const auto live = live_blocks().find(memory);
    const size_t capacity = live->second;
    live_blocks().erase(live);
    if (cached_bytes() + capacity <= kReusedBlockCacheBytes) {
      free_blocks().push_back({memory, capacity});
      cached_bytes() += capacity;
      return;
    }
    lock.unlock();
    ::operator delete(memory);
  }

private:
  struct Block {
    void *memory;
    size_t bytes;
  };

  static std::mutex &mutex() {
    static std::mutex value;
    return value;
  }
  static std::vector<Block> &free_blocks() {
    static std::vector<Block> value;
    return value;
  }
  static std::unordered_map<void *, size_t> &live_blocks() {
    static std::unordered_map<void *, size_t> value;
    return value;
  }
  static size_t &cached_bytes() {
    static size_t value = 0u;
    return value;
  }
};

// The allocator of the packer's large buffers (ReusedBlocks).
template <typename T> struct ReusedAllocator {
  using value_type = T;

  ReusedAllocator() = default;
  template <typename U> ReusedAllocator(const ReusedAllocator<U> &) {}

  T *allocate(size_t count) {
    return static_cast<T *>(ReusedBlocks::allocate(count * sizeof(T)));
  }
  void deallocate(T *memory, size_t count) {
    ReusedBlocks::release(memory, count * sizeof(T));
  }

  template <typename U> bool operator==(const ReusedAllocator<U> &) const {
    return true;
  }
  template <typename U> bool operator!=(const ReusedAllocator<U> &) const {
    return false;
  }
};

using PixelBuffer = std::vector<uint8_t, ReusedAllocator<uint8_t>>;

// Block rows of every image being encoded share one set of threads, created
// once. Each mip level of each texture used to start its own threads: with a
// dozen material workers encoding at once that was over a hundred threads at
// a time and tens of thousands started per finalization, each faulting in a
// fresh stack. The submitting worker encodes rows of its own image too.
class EncodePool {
public:
  // Calls encode(row) once for each row in [0, rows), on the pool's threads
  // and the caller's, and returns when all have returned.
  static void run(uint32_t rows, const std::function<void(uint32_t)> &encode) {
    EncodePool &pool = instance();
    Job job = {&encode, rows, 0u, 0u};
    {
      const std::lock_guard<std::mutex> lock(pool.mutex_);
      pool.jobs_.push_back(&job);
    }
    pool.work_.notify_all();
    for (;;) {
      std::unique_lock<std::mutex> lock(pool.mutex_);
      if (job.next == job.rows) {
        pool.done_.wait(lock, [&] { return job.done == job.rows; });
        return;
      }
      const uint32_t row = pool.claim(&job);
      lock.unlock();
      encode(row);
      lock.lock();
      pool.finish(&job);
    }
  }

private:
  struct Job {
    const std::function<void(uint32_t)> *encode;
    uint32_t rows;
    uint32_t next;
    uint32_t done;
  };

  EncodePool() {
    const uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    for (uint32_t i = 0u; i < threads; ++i) {
      std::thread(&EncodePool::work, this).detach();
    }
  }

  // Never destroyed: its threads wait for work until the process exits.
  static EncodePool &instance() {
    static EncodePool *pool = new EncodePool();
    return *pool;
  }

  // Takes `job`'s next row; the last row taken leaves the queue. Caller
  // holds mutex_.
  uint32_t claim(Job *job) {
    const uint32_t row = job->next++;
    if (job->next == job->rows) {
      jobs_.erase(std::find(jobs_.begin(), jobs_.end(), job));
    }
    return row;
  }

  // Counts a finished row; the job's submitter may return once all are.
  // Caller holds mutex_, so the job outlives this call.
  void finish(Job *job) {
    if (++job->done == job->rows) {
      done_.notify_all();
    }
  }

  void work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      work_.wait(lock, [&] { return !jobs_.empty(); });
      Job *job = jobs_.front();
      const uint32_t row = claim(job);
      lock.unlock();
      (*job->encode)(row);
      lock.lock();
      finish(job);
    }
  }

  std::mutex mutex_;
  std::condition_variable work_;
  std::condition_variable done_;
  std::vector<Job *> jobs_;
};

struct LevelImage {
  uint32_t width = 0;
  uint32_t height = 0;
  PixelBuffer pixels;
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
  uint32_t threads = 0;
  bool write_source_hash = true;
  bool cutout = false;
  // The texture of an opaque material: alpha is one before filtering.
  bool opaque = false;
  bool alpha_factor_explicit = false;
  float alpha_cutoff = 0.5f;
  float alpha_factor = 1.0f;
  bool normal_roughness = false;
  float normal_scale = 1.0f;
  float roughness_factor = 1.0f;
  bool roughness_source = false;
  // Command-line paired bake: the first layer is the normal map, written
  // here; the optional second is the metal-roughness map, written to output.
  fs::path paired_normal_output;
  // Preview tier: mip levels larger than this extent are not stored; 0 keeps
  // every level.
  uint32_t max_extent = 0u;
  // Native ASTC or BC7/BC5 blocks, which the runtime uploads as stored; the
  // host's encoding unless the command names one.
  VkrVktEncoding encoding = vkr_vkt_host_encoding();
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

uint32_t resolve_thread_count(uint32_t configured_threads) {
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
    if (arg == "--paired-normal") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for --paired-normal\n";
        return ParseResult::kError;
      }
      out_config.paired_normal_output =
          vkr_filesystem_native_utf8_path(argv[++index]);
      out_config.normal_roughness = true;
      out_config.layered_mode = true;
      continue;
    }
    if (arg == "--normal-scale" || arg == "--roughness-factor") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for " << arg << "\n";
        return ParseResult::kError;
      }
      const char *value = argv[++index];
      char *end = nullptr;
      const float parsed = std::strtof(value, &end);
      if (end == value || *end != '\0' || !std::isfinite(parsed) ||
          (arg == "--roughness-factor" && (parsed < 0.0f || parsed > 1.0f))) {
        std::cerr << "Invalid " << arg << "\n";
        return ParseResult::kError;
      }
      if (arg == "--normal-scale") {
        out_config.normal_scale = parsed;
      } else {
        out_config.roughness_factor = parsed;
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
    if (arg == "--threads") {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for --threads\n";
        return ParseResult::kError;
      }
      const std::string value = to_lower_ascii(argv[++index]);
      if (value == "auto") {
        out_config.threads = 0;
        continue;
      }
      uint32_t parsed = 0;
      if (!parse_uint32_nonzero(value, &parsed)) {
        std::cerr << "Invalid --threads value '" << value
                  << "' (expected positive integer or 'auto')\n";
        return ParseResult::kError;
      }
      out_config.threads = parsed;
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
                  << "' (expected astc|astc-fast|bc|bc-fast; astc-fast "
                     "needs Apple's system encoder, bc an x86-64 build)\n";
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
  if (out_config.normal_roughness &&
      (out_config.shape != TextureShape::k2D || out_config.layers.size() > 2u ||
       out_config.cutout || out_config.texture_class_explicit)) {
    std::cerr << "A paired bake takes --type 2d, the normal map and an "
                 "optional metal-roughness --layer, and no --texture-class "
                 "or cutout\n";
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
         " [--progress|--no-progress] [--threads <auto|n>]"
         " [--encoding <astc|astc-fast|bc|bc-fast>]"
         " [--max-extent <pixels>]"
         " [--source-hash|--no-source-hash]\n";
  std::cout << "Cutout color mips: --alpha-cutoff <0..1>"
               " [--alpha-factor <0..1>] (explicit output/color class only)\n";
  std::cout << "Paired normal/roughness: --paired-normal <normal.vkt>"
               " --output <metal-roughness.vkt> --type 2d --layer <normal>"
               " [--layer <metal-roughness>] [--normal-scale <s>]"
               " [--roughness-factor <0..1>]\n";
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

// The sRGB encode table every colour mip chain shares.
const VkrVktSrgbEncodeTable &srgb_encode_table() {
  static const VkrVktSrgbEncodeTable table = [] {
    VkrVktSrgbEncodeTable value = {};
    vkr_vkt_srgb_encode_table_init(&value);
    return value;
  }();
  return table;
}

// Level 0 is `base_pixels` itself, which the caller moves in when it no
// longer needs them.
std::vector<LevelImage> build_mip_chain_rgba8(PixelBuffer base_pixels,
                                              uint32_t width, uint32_t height,
                                              TextureClass texture_class,
                                              const PackConfig &config) {
  std::vector<LevelImage> levels;
  levels.reserve(calculate_mip_levels(width, height));

  LevelImage base = {};
  base.width = width;
  base.height = height;
  base.pixels = std::move(base_pixels);
  levels.push_back(std::move(base));
  const uint8_t *const base_texels = levels.front().pixels.data();

  while (levels.back().width > 1 || levels.back().height > 1) {
    const LevelImage &previous = levels.back();
    const uint32_t next_width = std::max(1u, previous.width >> 1u);
    const uint32_t next_height = std::max(1u, previous.height >> 1u);

    LevelImage next = {};
    next.width = next_width;
    next.height = next_height;
    next.pixels.resize(static_cast<size_t>(next_width) * next_height * 4u);

    vkr_vkt_downsample_rgba8_with_table(
        previous.pixels.data(), previous.width, previous.height,
        next.pixels.data(), next_width, next_height,
        texture_class_prefers_srgb(texture_class),
        config.cutout && config.alpha_cutoff > 0.0f, &srgb_encode_table());

    levels.push_back(std::move(next));
  }

  if (config.cutout) {
    const uint32_t pass_byte =
        vkr_vkt_alpha_pass_byte(config.alpha_cutoff, config.alpha_factor);
    const uint64_t base_count = static_cast<uint64_t>(width) * height;
    const uint64_t base_covered =
        vkr_vkt_alpha_covered(base_texels, base_count, pass_byte);
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

// FNV-1a over a file's bytes, in two variants read in one pass: `source`
// starts from kFnvOffsetBasis and is the vkr.source_hash packs record;
// `standard` starts from the standard offset basis and names the glTF cook's
// paired bakes (vkr_vkt_hash_file).
struct FileHashes {
  uint64_t source = 0u;
  uint64_t standard = 0u;
};
constexpr uint64_t kFnvStandardOffsetBasis = 14695981039346656037ull;

bool fnv1a_file_hashes(const fs::path &path, FileHashes *out) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }

  uint64_t source = kFnvOffsetBasis;
  uint64_t standard = kFnvStandardOffsetBasis;
  std::vector<char> buffer(MB(1));
  while (input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      const uint8_t byte = static_cast<uint8_t>(buffer[static_cast<size_t>(i)]);
      source = (source ^ byte) * kFnvPrime;
      standard = (standard ^ byte) * kFnvPrime;
    }
  }
  if (!input.eof() && !input.good()) {
    return false;
  }
  *out = FileHashes{source, standard};
  return true;
}

// fnv1a_file_hashes memoized by normalized path, size and modification time
// while a cook's scope is open (vkr_vkt_begin_file_hash_scope): a paired
// bake's normal map is otherwise read and hashed by the cook and again by
// each output's skip check. Sources do not change during one cook, but a file
// rewritten in place within one timestamp tick keeps its size and time, so
// the memo never outlives the scope. An entry is kept only when the file's
// size and time did not change while it was hashed.
struct FileHashEntry {
  uintmax_t size = 0u;
  fs::file_time_type modified;
  FileHashes hashes;
};
std::mutex file_hash_mutex;
std::unordered_map<std::string, FileHashEntry> file_hashes;
uint32_t file_hash_scopes = 0u;

bool memoized_file_hashes(const fs::path &path, FileHashes *out) {
  bool scoped = false;
  {
    const std::lock_guard<std::mutex> lock(file_hash_mutex);
    scoped = file_hash_scopes > 0u;
  }
  if (!scoped) {
    return fnv1a_file_hashes(path, out);
  }
  std::error_code error;
  const std::string key =
      fs::absolute(path, error).lexically_normal().u8string();
  const uintmax_t size = error ? 0u : fs::file_size(path, error);
  const fs::file_time_type modified =
      error ? fs::file_time_type{} : fs::last_write_time(path, error);
  if (error) {
    return false;
  }
  {
    const std::lock_guard<std::mutex> lock(file_hash_mutex);
    const auto found = file_hashes.find(key);
    if (found != file_hashes.end() && found->second.size == size &&
        found->second.modified == modified) {
      *out = found->second.hashes;
      return true;
    }
  }
  FileHashes hashes;
  if (!fnv1a_file_hashes(path, &hashes)) {
    return false;
  }
  if (fs::file_size(path, error) == size && !error &&
      fs::last_write_time(path, error) == modified && !error) {
    const std::lock_guard<std::mutex> lock(file_hash_mutex);
    file_hashes[key] = FileHashEntry{size, modified, hashes};
  }
  *out = hashes;
  return true;
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
    uint64_t source_hash = source.hash;
    FileHashes hashes;
    if (!source.converted) {
      if (!memoized_file_hashes(source.path, &hashes)) {
        return false;
      }
      source_hash = hashes.source;
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

// Encoder and profile of each class under the BC encodings (ADR-012). Bump
// the version when the vendored encoders or their settings change.
const char *bc_profile(TextureClass texture_class, VkrVktEncoding encoding) {
  if (texture_class == TextureClass::kNormalRg) {
    return "bc5-rgbcx-v1";
  }
  if (texture_class == TextureClass::kDataMask) {
    return encoding == VKR_VKT_ENCODING_BC_FAST ? "bc7-bc7e-m456-r023-v1"
                                                : "bc7-bc7e-default-v1";
  }
  return encoding == VKR_VKT_ENCODING_BC_FAST ? "bc7-bc7e-ultrafast-v1"
                                              : "bc7-bc7e-veryfast-v1";
}

// ASTC block side of each class under the `astc` encoding (ADR-012). Colours
// and data masks take 6x6, 3.56 bits per texel against 8: on one in six of
// Bistro's images astcenc "fastest" scored 43.8 against 51.5 dB on colours and
// 51.4 against 72.6 dB on metal-roughness. Normals and alpha-tested colours,
// whose alpha decides coverage, keep 4x4.
uint32_t astc_block_side(TextureClass texture_class, const PackConfig &config) {
  return texture_class == TextureClass::kNormalRg || config.cutout ? 4u : 6u;
}

std::string pack_settings_identity(TextureClass texture_class,
                                   TextureShape shape,
                                   const PackConfig &config) {
  std::ostringstream settings;
  settings << "asset=1;shape=" << texture_shape_metadata_value(shape)
           << ";class=" << texture_class_metadata_value(texture_class);
  if (config.encoding == VKR_VKT_ENCODING_ASTC) {
    const uint32_t side = astc_block_side(texture_class, config);
    settings << ";encoding=astc-" << side << "x" << side << "-fastest";
    if (texture_class == TextureClass::kNormalRg) {
      settings << ";astc_rg=alpha-one-db39-c1-v2";
    }
  } else if (config.encoding == VKR_VKT_ENCODING_ASTC_FAST) {
    settings << ";encoding=astc-4x4-system-equal-t12-v1";
    if (texture_class == TextureClass::kNormalRg) {
      settings << ";astc_rg=alpha-one";
    }
  } else {
    settings << ";encoding=" << bc_profile(texture_class, config.encoding);
  }
  settings << ";mips=rgba8-area-srgb-v2;flip=vertical";
  if (config.max_extent) {
    settings << ";max_extent=" << config.max_extent;
  }
  if (config.opaque) {
    settings << ";alpha=opaque-v1";
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

// Digests of the outputs this process published (vkr_vkt_output_digest),
// keyed by their normalized paths. Published outputs are content-named and
// replaced only by rename, so a recorded digest stays the file's.
struct OutputDigest {
  uint8_t digest[32];
  uint64_t size;
};

std::mutex &output_digests_mutex() {
  static std::mutex value;
  return value;
}

std::unordered_map<std::string, OutputDigest> &output_digests() {
  static std::unordered_map<std::string, OutputDigest> value;
  return value;
}

std::string output_digest_key(const fs::path &path) {
  return path.lexically_normal().generic_u8string();
}

// SHA-256 and size of a file's bytes.
bool hash_output(const fs::path &path, OutputDigest *out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  std::vector<char> chunk(size_t(1) << 20u);
  uint64_t size = 0u;
  while (file) {
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize read = file.gcount();
    if (read <= 0) {
      break;
    }
    vkr_sha256_update(&hash, reinterpret_cast<const uint8_t *>(chunk.data()),
                      static_cast<size_t>(read));
    size += static_cast<uint64_t>(read);
  }
  if (file.bad()) {
    return false;
  }
  vkr_sha256_final(&hash, out->digest);
  out->size = size;
  return true;
}

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

// Outputs a worker is packing now. Material workers often name one texture
// at the same time; a second worker waits for the first to publish and then
// finds the output current, rather than decoding, filtering and encoding the
// same texture only to discard it. A claim takes all its paths at once, and
// a worker never claims twice, so claims cannot deadlock.
class OutputClaim {
public:
  explicit OutputClaim(std::vector<fs::path> paths) : paths_(std::move(paths)) {
    std::unique_lock<std::mutex> lock(mutex());
    released().wait(lock, [&] {
      for (const fs::path &path : paths_) {
        if (std::find(claimed().begin(), claimed().end(), path) !=
            claimed().end()) {
          return false;
        }
      }
      return true;
    });
    claimed().insert(claimed().end(), paths_.begin(), paths_.end());
  }

  ~OutputClaim() {
    {
      const std::lock_guard<std::mutex> lock(mutex());
      for (const fs::path &path : paths_) {
        claimed().erase(std::find(claimed().begin(), claimed().end(), path));
      }
    }
    released().notify_all();
  }

  OutputClaim(const OutputClaim &) = delete;
  OutputClaim &operator=(const OutputClaim &) = delete;

private:
  static std::mutex &mutex() {
    static std::mutex value;
    return value;
  }
  static std::condition_variable &released() {
    static std::condition_variable value;
    return value;
  }
  static std::vector<fs::path> &claimed() {
    static std::vector<fs::path> value;
    return value;
  }

  std::vector<fs::path> paths_;
};

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
  PixelBuffer encoded(static_cast<size_t>(source_size));
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
  // The packed texture stores rows bottom first, decoded straight into the
  // level's reused buffer.
  const VkrImageDecodeTarget target = {
      .pixels =
          [](void *context, uint32_t width, uint32_t height) {
            LevelImage *level = static_cast<LevelImage *>(context);
            level->width = width;
            level->height = height;
            level->pixels.resize(static_cast<size_t>(width) * height * 4u);
            return level->pixels.data();
          },
      .allocate = [](void *,
                     size_t size) { return ReusedBlocks::allocate(size); },
      .release = [](void *, void *memory,
                    size_t size) { ReusedBlocks::release(memory, size); },
      .context = &image,
  };
  if (!vkr_image_decode_rgba8_into(encoded.data(), encoded.size(), 1,
                                   &target) ||
      image.width > kMaxTextureDimension ||
      image.height > kMaxTextureDimension) {
    std::cerr << "Failed to decode texture: " << path << "\n";
    return false;
  }
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

// Creates the 4x4-block texture that receives the encoded images of `source`,
// in `srgb_format` when the source is sRGB and `unorm_format` otherwise.
ktxTexture2 *create_block_texture(ktxTexture2 *source, VkFormat unorm_format,
                                  VkFormat srgb_format,
                                  KTX_error_code *out_result) {
  const bool srgb = source->vkFormat == VK_FORMAT_R8G8B8A8_SRGB;
  ktxTextureCreateInfo create_info = {};
  create_info.vkFormat = srgb ? srgb_format : unorm_format;
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

// The RGBA8 texels of one image (level, layer, face) of the texture being
// encoded, or null when it has none.
using ImageTexels =
    std::function<uint8_t *(uint32_t level, uint32_t layer, uint32_t face)>;

// Calls encode(width, height, texels, blocks, block_size) for every image of
// `source`, whose texels `texels` returns, stopping at the first that returns
// false.
template <typename Encode>
bool encode_block_images(ktxTexture2 *source, const ImageTexels &texels,
                         ktxTexture2 *encoded, Encode encode) {
  for (uint32_t level = 0u; level < source->numLevels; ++level) {
    const uint32_t width = std::max(1u, source->baseWidth >> level);
    const uint32_t height = std::max(1u, source->baseHeight >> level);
    const ktx_size_t block_size =
        ktxTexture_GetImageSize(ktxTexture(encoded), level);
    for (uint32_t layer = 0u; layer < source->numLayers; ++layer) {
      for (uint32_t face = 0u; face < source->numFaces; ++face) {
        ktx_size_t out_offset = 0u;
        uint8_t *image = texels(level, layer, face);
        if (!image ||
            ktxTexture_GetImageOffset(ktxTexture(encoded), level, layer, face,
                                      &out_offset) != KTX_SUCCESS ||
            !encode(width, height, image, encoded->pData + out_offset,
                    block_size)) {
          return false;
        }
      }
    }
  }
  return true;
}

// As above, for images held in `source`'s own storage.
template <typename Encode>
bool encode_block_images(ktxTexture2 *source, ktxTexture2 *encoded,
                         Encode encode) {
  return encode_block_images(
      source,
      [source](uint32_t level, uint32_t layer, uint32_t face) -> uint8_t * {
        ktx_size_t offset = 0u;
        return ktxTexture_GetImageOffset(ktxTexture(source), level, layer, face,
                                         &offset) == KTX_SUCCESS
                   ? source->pData + offset
                   : nullptr;
      },
      encoded, encode);
}

// Encodes every image of an RGBA8 texture to ASTC with square blocks of
// `block_side` (4 or 6) using astcenc, as ktxTexture2_CompressAstcEx does at
// its "fastest" level, and moves the
// key/value data to the returned texture. Normals (R, G; B zero, A one) aim
// at 39 dB with one candidate instead of the preset's 43: on Bistro's baked
// normals that encoded 1.8 times faster and still scored above UASTC
// "faster" (41.2 against 40.9 dB in RG), where the preset kept searching
// blocks that never reach its target. libktx exposes neither setting.
ktxTexture2 *compress_astc(ktxTexture2 *source, const ImageTexels &texels,
                           TextureClass texture_class, uint32_t block_side,
                           uint32_t thread_count, KTX_error_code *out_result) {
  const bool wide = block_side == 6u;
  ktxTexture2 *encoded = create_block_texture(
      source,
      wide ? VK_FORMAT_ASTC_6x6_UNORM_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
      wide ? VK_FORMAT_ASTC_6x6_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
      out_result);
  if (!encoded) {
    return nullptr;
  }
  std::unique_ptr<ktxTexture2, decltype(&destroy_ktx_texture)> owner(
      encoded, destroy_ktx_texture);

  const bool srgb = source->vkFormat == VK_FORMAT_R8G8B8A8_SRGB;
  astcenc_config astc_config;
  if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR,
                          block_side, block_side, 1u, ASTCENC_PRE_FASTEST, 0u,
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
  const bool encoded_all = encode_block_images(
      source, texels, encoded,
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
  ktxTexture2 *encoded =
      create_block_texture(source, VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
                           VK_FORMAT_ASTC_4x4_SRGB_BLOCK, out_result);
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
  const bool encoded_all = encode_block_images(
      source, encoded,
      [&](uint32_t width, uint32_t height, uint8_t *texels, uint8_t *blocks,
          ktx_size_t block_size) {
        const at_texel_region_t region = {texels,
                                          {width, height, 1u},
                                          static_cast<size_t>(width) * 4u,
                                          static_cast<size_t>(width) * height *
                                              4u};
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

#if defined(VKR_VKT_HAS_BC7E)
// bc7e encodes each block alone, so a row of blocks encodes to the same
// bytes when each distinct block is encoded once and copied to its repeats.
// Metallic-roughness rows repeat often: flat regions are solid, and texels of
// two or three values repeat whole blocks. Solid blocks go first, so a gang
// of four holds blocks of one kind rather than leaving lanes idle beside a
// full search. Bistro's masks encoded at 176 against 113 Mpx/s; colours
// repeat too rarely to repay the pass.
class DistinctBlocks {
public:
  uint32_t columns() const { return columns_; }

  explicit DistinctBlocks(uint32_t columns)
      : columns_(columns), slots_(slot_count(columns)), first_(columns),
        order_(columns), position_(columns),
        texels_(static_cast<size_t>(columns) * 16u),
        bits_(static_cast<size_t>(columns) * 2u) {}

  // Encodes the row's `columns` blocks of 16 RGBA8 texels into `bits`.
  void encode(const uint32_t *texels, uint64_t *bits,
              const ispc::bc7e_compress_block_params &params) {
    std::fill(slots_.begin(), slots_.end(), kEmpty);
    const uint32_t mask = static_cast<uint32_t>(slots_.size()) - 1u;
    uint32_t solid_count = 0u;
    uint32_t distinct_count = 0u;
    for (uint32_t column = 0u; column < columns_; ++column) {
      const uint32_t *block = texels + static_cast<size_t>(column) * 16u;
      uint32_t slot = block_key(block) & mask;
      while (slots_[slot] != kEmpty &&
             std::memcmp(texels + static_cast<size_t>(slots_[slot]) * 16u,
                         block, 64u) != 0) {
        slot = (slot + 1u) & mask;
      }
      if (slots_[slot] != kEmpty) {
        first_[column] = slots_[slot];
        continue;
      }
      slots_[slot] = column;
      first_[column] = column;
      order_[distinct_count++] = column;
      bool solid = true;
      for (uint32_t texel = 1u; texel < 16u && solid; ++texel) {
        solid = block[texel] == block[0];
      }
      if (solid) {
        std::swap(order_[solid_count++], order_[distinct_count - 1u]);
      }
    }

    for (uint32_t i = 0u; i < distinct_count; ++i) {
      std::memcpy(&texels_[static_cast<size_t>(i) * 16u],
                  texels + static_cast<size_t>(order_[i]) * 16u, 64u);
      position_[order_[i]] = i;
    }
    ispc::bc7e_compress_blocks(distinct_count, bits_.data(), texels_.data(),
                               &params);
    for (uint32_t column = 0u; column < columns_; ++column) {
      std::memcpy(bits + static_cast<size_t>(column) * 2u,
                  &bits_[static_cast<size_t>(position_[first_[column]]) * 2u],
                  16u);
    }
  }

private:
  static constexpr uint32_t kEmpty = UINT32_MAX;

  // A power of two at least twice `columns`.
  static size_t slot_count(uint32_t columns) {
    size_t count = 16u;
    while (count < static_cast<size_t>(columns) * 2u) {
      count *= 2u;
    }
    return count;
  }

  static uint32_t block_key(const uint32_t *block) {
    uint64_t key = kFnvOffsetBasis;
    for (uint32_t texel = 0u; texel < 16u; ++texel) {
      key = (key ^ block[texel]) * kFnvPrime;
    }
    return static_cast<uint32_t>(key ^ (key >> 32u));
  }

  uint32_t columns_;
  std::vector<uint32_t> slots_;
  std::vector<uint32_t> first_;
  std::vector<uint32_t> order_;
  std::vector<uint32_t> position_;
  std::vector<uint32_t> texels_;
  std::vector<uint64_t> bits_;
};
#endif

// Encodes every image of an RGBA8 texture as BC7 with bc7e, or normals as BC5
// from R and G with rgbcx, and moves the key/value data to the returned
// texture. Channels weigh equally, as in astcenc. On one in six of Bistro's
// pre-encode images, against UASTC "faster" at 4.1, 16.5 and 2.4 Mpx/s
// (ADR-012): colours with bc7e's "veryfast" profile scored 54.1 against 51.1
// dB at 13 Mpx/s, metallic-roughness with its default profile, which keeps
// the channel-rotation modes uncorrelated channels need, 70.2 against 66.6 dB
// mean per image at 31 Mpx/s, and normals 48.5 against 41.7 dB in RG at 566
// Mpx/s, since BC5 keeps the two channels apart. The fast speed's
// "ultrafast" colours scored 51.4 dB at 54 Mpx/s.
ktxTexture2 *compress_bc(ktxTexture2 *source, const ImageTexels &texels,
                         TextureClass texture_class, VkrVktEncoding encoding,
                         KTX_error_code *out_result) {
#if defined(VKR_VKT_HAS_BC7E)
  static std::once_flag encoders_initialized;
  std::call_once(encoders_initialized, [] {
    rgbcx::init();
    ispc::bc7e_compress_block_init();
  });
  const bool normal = texture_class == TextureClass::kNormalRg;
  ktxTexture2 *encoded = create_block_texture(
      source, normal ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK,
      normal ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC7_SRGB_BLOCK,
      out_result);
  if (!encoded) {
    return nullptr;
  }
  std::unique_ptr<ktxTexture2, decltype(&destroy_ktx_texture)> owner(
      encoded, destroy_ktx_texture);

  ispc::bc7e_compress_block_params params = {};
  if (texture_class == TextureClass::kDataMask) {
    ispc::bc7e_compress_block_params_init(&params, false);
    if (encoding == VKR_VKT_ENCODING_BC_FAST) {
      // The fast speed keeps only the modes metallic-roughness needs: 4 and
      // 5 give one channel its own indices (rotations 2 and 3 roughness or
      // metallic), 6 covers the rest. On one in six of Bistro's masks this
      // encoded 1.8 times faster than the default (63 against 36 Mpx/s) at
      // 64.7 against 65.9 dB, still above UASTC "faster" (63.5 dB), and
      // keeps constant masks exact.
      for (uint32_t mode = 0u; mode < 7u; ++mode) {
        params.m_opaque_settings.m_use_mode[mode] =
            mode == 4u || mode == 5u || mode == 6u;
      }
      params.m_mode4_rotation_mask = 1u | 4u | 8u;
      params.m_mode5_rotation_mask = 1u | 4u | 8u;
    }
  } else if (encoding == VKR_VKT_ENCODING_BC_FAST) {
    ispc::bc7e_compress_block_params_init_ultrafast(&params, false);
  } else {
    ispc::bc7e_compress_block_params_init_veryfast(&params, false);
  }
  const bool encoded_all = encode_block_images(
      source, texels, encoded,
      [&](uint32_t width, uint32_t height, uint8_t *image, uint8_t *blocks,
          ktx_size_t block_size) {
        const uint8_t *texels = image;
        const uint32_t columns = (width + 3u) / 4u;
        const uint32_t rows = (height + 3u) / 4u;
        if (block_size != static_cast<ktx_size_t>(columns) * rows * 16u) {
          return false;
        }
        // The pool's threads take block rows in turn, each with scratch it
        // keeps between images. Edge blocks repeat the last column and row.
        const std::function<void(uint32_t)> encode_row = [&](uint32_t row) {
          thread_local std::vector<uint32_t> row_texels;
          thread_local std::vector<uint64_t> row_bits;
          thread_local std::unique_ptr<DistinctBlocks> distinct;
          row_texels.resize(static_cast<size_t>(columns) * 16u);
          row_bits.resize(static_cast<size_t>(columns) * 2u);
          if (texture_class == TextureClass::kDataMask &&
              (!distinct || distinct->columns() != columns)) {
            distinct = std::make_unique<DistinctBlocks>(columns);
          }
          for (uint32_t column = 0u; column < columns; ++column) {
            for (uint32_t y = 0u; y < 4u; ++y) {
              const uint32_t source_y = std::min(row * 4u + y, height - 1u);
              for (uint32_t x = 0u; x < 4u; ++x) {
                const uint32_t source_x = std::min(column * 4u + x, width - 1u);
                std::memcpy(
                    &row_texels[column * 16u + y * 4u + x],
                    texels +
                        (static_cast<size_t>(source_y) * width + source_x) * 4u,
                    4u);
              }
            }
          }

          uint8_t *row_blocks =
              blocks + static_cast<size_t>(row) * columns * 16u;
          if (normal) {
            for (uint32_t column = 0u; column < columns; ++column) {
              rgbcx::encode_bc5(
                  row_blocks + column * 16u,
                  reinterpret_cast<const uint8_t *>(&row_texels[column * 16u]),
                  0u, 1u, 4u);
            }
          } else if (texture_class == TextureClass::kDataMask) {
            distinct->encode(row_texels.data(), row_bits.data(), params);
            std::memcpy(row_blocks, row_bits.data(),
                        static_cast<size_t>(columns) * 16u);
          } else {
            ispc::bc7e_compress_blocks(columns, row_bits.data(),
                                       row_texels.data(), &params);
            std::memcpy(row_blocks, row_bits.data(),
                        static_cast<size_t>(columns) * 16u);
          }
        };
        // Every block row of a uniform image, such as the metallic-roughness
        // of a paired bake without a roughness map, encodes to the same
        // bytes: encode one row and repeat it.
        const size_t texel_count = static_cast<size_t>(width) * height;
        bool uniform = true;
        for (size_t texel = 1u; texel < texel_count && uniform; ++texel) {
          uniform = std::memcmp(texels + texel * 4u, texels, 4u) == 0;
        }
        if (uniform) {
          encode_row(rows - 1u);
          const size_t row_bytes = static_cast<size_t>(columns) * 16u;
          const uint8_t *last =
              blocks + static_cast<size_t>(rows - 1u) * row_bytes;
          for (uint32_t row = 0u; row + 1u < rows; ++row) {
            std::memcpy(blocks + static_cast<size_t>(row) * row_bytes, last,
                        row_bytes);
          }
          return true;
        }
        EncodePool::run(rows, encode_row);
        return true;
      });
  if (!encoded_all) {
    *out_result = KTX_INVALID_OPERATION;
    return nullptr;
  }
  std::swap(ktxTexture(encoded)->kvDataHead, ktxTexture(source)->kvDataHead);
  *out_result = KTX_SUCCESS;
  return owner.release();
#else
  (void)source;
  (void)texels;
  (void)texture_class;
  (void)encoding;
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
                          std::vector<PackedSource> &sources,
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

  // bc7e, rgbcx and astcenc read each level where the packer built it; only
  // Apple's encoder reads the texture's storage, so only it pays for a copy of
  // every level into fresh memory.
  const bool levels_in_place = config.encoding != VKR_VKT_ENCODING_ASTC_FAST;
  const ImageTexels level_texels = [&](uint32_t level, uint32_t layer,
                                       uint32_t face) {
    return sources[layer * face_count + face]
        .levels[first_level + level]
        .pixels.data();
  };
  ktxTexture2 *texture = nullptr;
  KTX_error_code result =
      ktxTexture2_Create(&create_info,
                         levels_in_place ? KTX_TEXTURE_CREATE_NO_STORAGE
                                         : KTX_TEXTURE_CREATE_ALLOC_STORAGE,
                         &texture);
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
    for (uint32_t layer = 0u; layer < layer_count && !levels_in_place;
         ++layer) {
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
    // Normals keep X and Y in R and G, which shaders sample. A constant alpha
    // encodes ASTC faster and more accurately (Bistro pair normals: 1.4x,
    // +2 dB in RG), and BC5 stores no alpha.
    if (result == KTX_SUCCESS && texture_class == TextureClass::kNormalRg &&
        config.encoding == VKR_VKT_ENCODING_ASTC) {
      for (PackedSource &source : sources) {
        for (size_t mip = first_level; mip < source.levels.size(); ++mip) {
          PixelBuffer &pixels = source.levels[mip].pixels;
          for (size_t i = 3u; i < pixels.size(); i += 4u) {
            pixels[i] = 255u;
          }
        }
      }
    } else if (result == KTX_SUCCESS &&
               texture_class == TextureClass::kNormalRg &&
               config.encoding == VKR_VKT_ENCODING_ASTC_FAST) {
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

    if (config.encoding == VKR_VKT_ENCODING_ASTC) {
      // astcenc "fastest" beat UASTC "faster", the former final tier, on
      // Bistro's colours, paired normals and metal-roughness (52.6/40.1/49.2
      // against 52.2/38.5/47.4 dB) at 4 to 9 times its speed (ADR-077).
      encoded.reset(compress_astc(texture, level_texels, texture_class,
                                  astc_block_side(texture_class, config),
                                  config.threads, &result));
    } else if (config.encoding == VKR_VKT_ENCODING_ASTC_FAST) {
      encoded.reset(compress_astc_system(texture, &result));
    } else {
      encoded.reset(compress_bc(texture, level_texels, texture_class,
                                config.encoding, &result));
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

    // The file's digest, taken while the material worker is in the parallel
    // stretch of a cook and its bytes are cached, spares the cook reading it
    // again once every material is done (vkr_vkt_output_digest).
    OutputDigest written = {};
    const bool hashed = hash_output(tmp_path, &written);

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
    if (hashed) {
      const std::lock_guard<std::mutex> digests(output_digests_mutex());
      output_digests()[output_digest_key(dst_path)] = written;
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
    // Bistro's opaque base colours carry stray alpha of 251-254 in a few
    // percent of texels; each such block took bc7e's alpha modes, which cost
    // 2.5 times mode 6 and spend bits no shader reads.
    if (config.opaque) {
      for (size_t i = 3u; i < image.pixels.size(); i += 4u) {
        image.pixels[i] = 255u;
      }
    }
    PackedSource source;
    source.width = image.width;
    source.height = image.height;
    source.alpha =
        analyze_alpha(image.pixels.data(), source.width, source.height);
    source.levels = build_mip_chain_rgba8(std::move(image.pixels), source.width,
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
std::vector<VkrVktMaterialMoment, ReusedAllocator<VkrVktMaterialMoment>>
reduce_material_moments(uint32_t width, uint32_t height, uint32_t next_width,
                        uint32_t next_height, Sample sample) {
  std::vector<VkrVktMaterialMoment, ReusedAllocator<VkrVktMaterialMoment>>
      result(static_cast<size_t>(next_width) * next_height);
  const double inverse_area = 1.0 / (static_cast<double>(width) * height);
  // An exactly halved extent covers source texels 2i and 2i + 1, which the
  // divisions would also give.
  const bool halved_x = width == next_width * 2u;
  const bool halved_y = height == next_height * 2u;
  for (uint32_t y = 0; y < next_height; ++y) {
    const uint32_t top = y * height;
    const uint32_t bottom = top + height;
    const uint32_t first_y = halved_y ? y * 2u : top / next_height;
    const uint32_t end_y =
        halved_y ? y * 2u + 2u : (bottom + next_height - 1u) / next_height;
    for (uint32_t x = 0; x < next_width; ++x) {
      const uint32_t left = x * width;
      const uint32_t right = left + width;
      const uint32_t first_x = halved_x ? x * 2u : left / next_width;
      const uint32_t end_x =
          halved_x ? x * 2u + 2u : (right + next_width - 1u) / next_width;
      VkrVktMaterialMoment sum = {};
      for (uint32_t sy = first_y; sy < end_y; ++sy) {
        const uint32_t overlap_y = std::min(bottom, (sy + 1u) * next_height) -
                                   std::max(top, sy * next_height);
        for (uint32_t sx = first_x; sx < end_x; ++sx) {
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
      build_mip_chain_rgba8(roughness_image.pixels, roughness.width,
                            roughness.height, TextureClass::kDataMask, config);
  normal.levels.reserve(roughness.levels.size());
  std::vector<VkrVktMaterialMoment, ReusedAllocator<VkrVktMaterialMoment>>
      moments;
  const size_t first_level = first_stored_level(
      normal.width, normal.height, roughness.levels.size(), config);

  // A base texel's moment depends only on its normal's X and Y bytes and its
  // roughness byte. A large image reads moments and their mip-0 encodings
  // from tables instead of repeating double-precision square roots and
  // divides per texel, which Zen+ cores run slowest; the values are the
  // same.
  const size_t base_count =
      static_cast<size_t>(normal.width) * static_cast<size_t>(normal.height);
  const bool tabulated = base_count >= kNormalMomentTableMinTexels;
  std::vector<VkrVktMaterialMoment, ReusedAllocator<VkrVktMaterialMoment>>
      normal_moments;
  std::vector<uint8_t> normal_encodings;
  std::vector<double> normal_variances;
  double roughness_fourth[256];
  if (tabulated) {
    normal_moments.resize(65536u);
    normal_encodings.resize(65536u * 4u);
    normal_variances.resize(65536u);
    for (uint32_t key = 0u; key < 65536u; ++key) {
      const uint8_t texel[4] = {static_cast<uint8_t>(key & 0xffu),
                                static_cast<uint8_t>(key >> 8u), 0u, 0u};
      normal_moments[key] = vkr_vkt_material_moment(
          texel, 0u, config.normal_scale, config.roughness_factor);
      normal_variances[key] = vkr_vkt_encode_material_normal(
          normal_moments[key], normal_encodings.data() + key * 4u);
    }
    const uint8_t flat[4] = {128u, 128u, 0u, 0u};
    for (uint32_t value = 0u; value < 256u; ++value) {
      roughness_fourth[value] =
          vkr_vkt_material_moment(flat, static_cast<uint8_t>(value),
                                  config.normal_scale, config.roughness_factor)
              .roughness_fourth;
    }
  }
  const auto normal_key = [&](size_t index) {
    const uint8_t *texel = normal_image.pixels.data() + index * 4u;
    return static_cast<uint32_t>(texel[0]) |
           (static_cast<uint32_t>(texel[1]) << 8u);
  };
  const auto base_sample = [&](size_t index) {
    const uint8_t roughness_value = roughness_image.pixels[index * 4u + 1u];
    if (!tabulated) {
      return vkr_vkt_material_moment(normal_image.pixels.data() + index * 4u,
                                     roughness_value, config.normal_scale,
                                     config.roughness_factor);
    }
    VkrVktMaterialMoment moment = normal_moments[normal_key(index)];
    moment.roughness_fourth = roughness_fourth[roughness_value];
    return moment;
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
    } else if (mip == 0u && tabulated) {
      for (size_t index = 0; index < count; ++index) {
        const uint32_t key = normal_key(index);
        std::memcpy(normal_level.pixels.data() + index * 4u,
                    normal_encodings.data() + key * 4u, 4u);
        roughness_level.pixels[index * 4u + 1u] =
            vkr_vkt_encode_material_roughness(
                roughness_fourth[roughness_image.pixels[index * 4u + 1u]],
                normal_variances[key]);
      }
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
                                 texture_class, TextureShape::k2D, config);
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

void vkr_vkt_begin_file_hash_scope(void) {
  const std::lock_guard<std::mutex> lock(file_hash_mutex);
  ++file_hash_scopes;
}

void vkr_vkt_end_file_hash_scope(void) {
  const std::lock_guard<std::mutex> lock(file_hash_mutex);
  if (file_hash_scopes > 0u && --file_hash_scopes == 0u) {
    file_hashes.clear();
  }
}

int vkr_vkt_hash_file(const char *path, unsigned long long *out_hash) {
  if (!path || !path[0] || !out_hash) {
    return 0;
  }
  try {
    FileHashes hashes;
    if (!memoized_file_hashes(vkr_filesystem_native_utf8_path(path), &hashes)) {
      return 0;
    }
    *out_hash = hashes.standard;
    return 1;
  } catch (const std::exception &) {
    return 0;
  }
}

int vkr_vkt_parse_encoding(const char *name, VkrVktEncoding *out) {
  if (!name || !out) {
    return 0;
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
#if defined(VKR_VKT_HAS_BC7E)
  if (std::strcmp(name, "bc") == 0) {
    *out = VKR_VKT_ENCODING_BC;
    return 1;
  }
  if (std::strcmp(name, "bc-fast") == 0) {
    *out = VKR_VKT_ENCODING_BC_FAST;
    return 1;
  }
#endif
  return 0;
}

VkrVktEncoding vkr_vkt_host_encoding(void) {
#if defined(VKR_VKT_HAS_BC7E)
  return VKR_VKT_ENCODING_BC;
#else
  return VKR_VKT_ENCODING_ASTC;
#endif
}

static std::atomic<int> g_vkr_vkt_encoding{vkr_vkt_host_encoding()};

void vkr_vkt_set_encoding(VkrVktEncoding encoding) {
  g_vkr_vkt_encoding.store(encoding);
}

VkrVktEncoding vkr_vkt_encoding(void) {
  return static_cast<VkrVktEncoding>(g_vkr_vkt_encoding.load());
}

const char *vkr_vkt_variant_suffix(void) {
  // Each host has one final encoding, so its outputs share host-neutral
  // names; settings identities keep hosts apart. The editor-only fast
  // encodings coexist with them under their own names.
  static const char *const suffixes[8] = {
      "", ".preview", ".astc-fast", ".astc-fast.preview",
      "", ".preview", ".bc-fast",   ".bc-fast.preview"};
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

int vkr_vkt_output_digest(const char *path, unsigned char out_digest[32],
                          unsigned long long *out_size) {
  if (!path || !path[0] || !out_digest || !out_size) {
    return 0;
  }
  try {
    const std::string key =
        output_digest_key(vkr_filesystem_native_utf8_path(path));
    const std::lock_guard<std::mutex> lock(output_digests_mutex());
    const auto found = output_digests().find(key);
    if (found == output_digests().end()) {
      return 0;
    }
    std::memcpy(out_digest, found->second.digest, 32u);
    *out_size = found->second.size;
    return 1;
  } catch (const std::exception &) {
    return 0;
  }
}

void *vkr_vkt_reused_allocate(size_t size) {
  try {
    return ReusedBlocks::allocate(size);
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
}

void vkr_vkt_reused_release(void *memory, size_t size) {
  if (memory) {
    ReusedBlocks::release(memory, size);
  }
}

namespace {

// Settings of the in-process packs: the process tier and encoding.
PackConfig process_pack_config() {
  PackConfig config;
  if (vkr_vkt_preview_tier()) {
    config.max_extent = VKR_VKT_PREVIEW_MAX_EXTENT;
  }
  config.encoding = vkr_vkt_encoding();
  config.threads = resolve_thread_count(config.threads);
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
  const OutputClaim claim({destination});
  if (should_skip_output(sources, destination, texture_class, TextureShape::k2D,
                         config)) {
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

VkrVktPackResult vkr_vkt_pack_opaque(const VkrVktSource *source,
                                     const char *output) {
  if (!source_valid(source) || !output || !output[0]) {
    return VKR_VKT_PACK_FAILED;
  }
  try {
    PackConfig config = process_pack_config();
    config.opaque = true;
    return pack_single(source, vkr_filesystem_native_utf8_path(output),
                       TextureClass::kColorSrgb, config);
  } catch (const std::exception &error) {
    std::cerr << "Opaque packing failed: " << error.what() << "\n";
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
    const char *normal_output, const char *roughness_output, float normal_scale,
    float roughness_factor) {
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
    const OutputClaim claim({normal_path, roughness_path});
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
    // Normal alpha is not surface transparency.
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

  config.threads = resolve_thread_count(config.threads);
  if (config.layered_mode) {
    for (const fs::path &layer : config.layers) {
      if (!fs::exists(layer) || !fs::is_regular_file(layer)) {
        std::cerr << "Layer source does not exist: " << layer << "\n";
        return 1;
      }
    }
    if (config.normal_roughness) {
      // The library bake the mesh cooker runs, at this command's encoding.
      vkr_vkt_set_encoding(config.encoding);
      const std::string normal = config.layers.front().u8string();
      const std::string roughness_path =
          config.layers.size() > 1u ? config.layers.back().u8string() : "";
      VkrVktSource roughness = {};
      roughness.path = roughness_path.c_str();
      const VkrVktPackResult result = vkr_vkt_pack_normal_roughness(
          normal.c_str(), config.layers.size() > 1u ? &roughness : nullptr,
          config.paired_normal_output.u8string().c_str(),
          config.output.u8string().c_str(), config.normal_scale,
          config.roughness_factor);
      if (result == VKR_VKT_PACK_INCOMPATIBLE) {
        std::cerr << "The paired sources differ in extent\n";
      }
      std::cout << "vkt paired pack: normal=" << config.paired_normal_output
                << " metal_roughness=" << config.output << " status="
                << (result == VKR_VKT_PACK_SUCCESS ? "packed" : "failed")
                << "\n";
      return result == VKR_VKT_PACK_SUCCESS ? 0 : 1;
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
    static const char *const encodings[] = {
        "astc-6x6-4x4-fastest", "astc-4x4-system", "bc7-bc5", "bc7-bc5-fast"};
    encode_config_line << "Encode config: encoding="
                       << encodings[config.encoding]
                       << " threads=" << config.threads << " source_hash="
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
