#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include <msdf-atlas-gen/TightAtlasPacker.h>
#include <msdf-atlas-gen/glyph-generators.h>
#include <msdf-atlas-gen/msdf-atlas-gen.h>
#include <msdfgen-ext.h>

extern "C" {
#include "containers/str.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/vkr_font_cooked.h"
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kCookerVersion = 5u;
#ifdef VKR_FONT_COOKER_MSDF_ATLAS_COMMIT
constexpr char kPinnedAtlasCommit[] = VKR_FONT_COOKER_MSDF_ATLAS_COMMIT;
#else
constexpr char kPinnedAtlasCommit[] =
    "2ede254314a2512252a225fa6c975948d6af559a";
#endif
#ifdef VKR_FONT_COOKER_MSDFGEN_COMMIT
constexpr char kPinnedMsdfgenCommit[] = VKR_FONT_COOKER_MSDFGEN_COMMIT;
#else
constexpr char kPinnedMsdfgenCommit[] =
    "1874bcf7d9624ccc85b4bc9a85d78116f690f35b";
#endif

struct Axis {
  std::string tag;
  double value = 0.0;
};

struct CodepointRange {
  uint32_t first = 0;
  uint32_t last = 0;
};

struct Config {
  std::string type;
  fs::path source;
  fs::path output;
  std::string face;
  uint32_t face_index = 0;
  std::vector<CodepointRange> ranges;
  std::vector<fs::path> charset_files;
  std::vector<Axis> axes;
  std::string field;
  uint32_t atlas_width = 0;
  uint32_t atlas_height = 0;
  double atlas_px_per_em = 0.0;
  double size = 0.0;
  bool has_size = false;
  double distance_range = 0.0;
  double inner_padding_px = 0.0;
  double outer_padding_px = 0.0;
  std::string edge_coloring;
  double edge_angle_degrees = 0.0;
  uint64_t edge_seed = 0;
  bool has_fallback = false;
  uint32_t fallback = 0;
  std::string fallback_policy;
  std::string pixel_format;
  double miter_limit = 0.0;
  bool scanline = false;
  uint32_t threads = 1;
};

struct Options {
  fs::path config_path;
  fs::path output_override;
  bool has_output_override = false;
  bool force = false;
  bool identity_self_test = false;
};

struct IdentityProvenance {
  std::string atlas_commit = kPinnedAtlasCommit;
  std::string msdfgen_commit = kPinnedMsdfgenCommit;
  uint32_t artifact_version = VKR_FONT_COOKED_VERSION;
  uint32_t cooker_version = kCookerVersion;
};

struct Sha256 {
  uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  uint64_t bits = 0;
  uint32_t length = 0;
  uint8_t block[64] = {};
};

uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }

void sha_transform(Sha256 &sha, const uint8_t *block) {
  static constexpr uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abb,  0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  uint32_t w[64];
  for (uint32_t i = 0; i < 16; ++i)
    w[i] = (uint32_t(block[i * 4u]) << 24u) |
           (uint32_t(block[i * 4u + 1u]) << 16u) |
           (uint32_t(block[i * 4u + 2u]) << 8u) | uint32_t(block[i * 4u + 3u]);
  for (uint32_t i = 16; i < 64; ++i) {
    uint32_t s0 =
        rotr(w[i - 15u], 7u) ^ rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
    uint32_t s1 =
        rotr(w[i - 2u], 17u) ^ rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
    w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
  }
  uint32_t a = sha.state[0], b = sha.state[1], c = sha.state[2],
           d = sha.state[3];
  uint32_t e = sha.state[4], f = sha.state[5], g = sha.state[6],
           h = sha.state[7];
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + ch + k[i] + w[i];
    uint32_t s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  sha.state[0] += a;
  sha.state[1] += b;
  sha.state[2] += c;
  sha.state[3] += d;
  sha.state[4] += e;
  sha.state[5] += f;
  sha.state[6] += g;
  sha.state[7] += h;
}

void sha_update(Sha256 &sha, const void *data, size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  while (length) {
    size_t n = std::min(length, size_t(64u - sha.length));
    std::memcpy(sha.block + sha.length, bytes, n);
    sha.length += static_cast<uint32_t>(n);
    bytes += n;
    length -= n;
    if (sha.length == 64u) {
      sha_transform(sha, sha.block);
      sha.bits += 512u;
      sha.length = 0;
    }
  }
}

std::array<uint8_t, 32> sha_final(Sha256 sha) {
  const uint32_t tail_length = sha.length;
  sha.bits += uint64_t(tail_length) * 8u;
  sha.block[sha.length++] = 0x80u;
  if (sha.length > 56u) {
    while (sha.length < 64u)
      sha.block[sha.length++] = 0;
    sha_transform(sha, sha.block);
    sha.length = 0;
  }
  while (sha.length < 56u)
    sha.block[sha.length++] = 0;
  for (uint32_t i = 0; i < 8; ++i)
    sha.block[63u - i] = uint8_t(sha.bits >> (i * 8u));
  sha_transform(sha, sha.block);
  std::array<uint8_t, 32> result{};
  for (uint32_t i = 0; i < 8; ++i) {
    result[i * 4u] = uint8_t(sha.state[i] >> 24u);
    result[i * 4u + 1u] = uint8_t(sha.state[i] >> 16u);
    result[i * 4u + 2u] = uint8_t(sha.state[i] >> 8u);
    result[i * 4u + 3u] = uint8_t(sha.state[i]);
  }
  return result;
}

void hash_u32(Sha256 &sha, uint32_t value) {
  uint8_t b[4] = {uint8_t(value), uint8_t(value >> 8u), uint8_t(value >> 16u),
                  uint8_t(value >> 24u)};
  sha_update(sha, b, 4);
}
void hash_u64(Sha256 &sha, uint64_t value) {
  hash_u32(sha, uint32_t(value));
  hash_u32(sha, uint32_t(value >> 32u));
}
void hash_double(Sha256 &sha, double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  hash_u64(sha, bits);
}
void hash_string(Sha256 &sha, const std::string &value) {
  hash_u64(sha, value.size());
  sha_update(sha, value.data(), value.size());
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1u);
}

bool parse_u32(const std::string &text, uint32_t *out, int base = 10) {
  if (text.empty())
    return false;
  size_t consumed = 0;
  try {
    unsigned long long value = std::stoull(text, &consumed, base);
    if (consumed != text.size() || value > std::numeric_limits<uint32_t>::max())
      return false;
    *out = static_cast<uint32_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_u64(const std::string &text, uint64_t *out) {
  if (text.empty())
    return false;
  size_t consumed = 0;
  try {
    unsigned long long value = std::stoull(text, &consumed, 0);
    if (consumed != text.size())
      return false;
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_double(const std::string &text, double *out) {
  if (text.empty())
    return false;
  size_t consumed = 0;
  try {
    double value = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value))
      return false;
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_codepoint(std::string text, uint32_t *out) {
  if (text.size() < 3u || (text[0] != 'U' && text[0] != 'u') || text[1] != '+')
    return false;
  text.erase(0, 2);
  if (text.empty() || text.size() > 6u)
    return false;
  for (char c : text)
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return false;
  return parse_u32(text, out, 16) && *out <= 0x10ffffu &&
         !(*out >= 0xd800u && *out <= 0xdfffu);
}

bool parse_range(const std::string &text, CodepointRange *out) {
  const size_t dash = text.find('-');
  std::string first = dash == std::string::npos ? text : text.substr(0, dash);
  std::string last = dash == std::string::npos ? text : text.substr(dash + 1u);
  if (!parse_codepoint(first, &out->first) ||
      !parse_codepoint(last, &out->last) || out->first > out->last)
    return false;
  return true;
}

bool is_scalar_key(const std::string &key) {
  return key != "charset" && key != "charset_file" && key != "axis";
}

bool parse_config(const fs::path &path, Config *out, std::string *error) {
  std::ifstream input(path);
  if (!input) {
    *error = "unable to read config";
    return false;
  }
  std::set<std::string> seen;
  std::string line;
  uint32_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    line = trim(line);
    if (line.empty() || line[0] == '#')
      continue;
    const size_t equals = line.find('=');
    if (equals == std::string::npos ||
        line.find('=', equals + 1u) != std::string::npos) {
      *error = "line " + std::to_string(line_number) + ": expected key=value";
      return false;
    }
    std::string key = trim(line.substr(0, equals));
    std::string value = trim(line.substr(equals + 1u));
    if (key.empty() || value.empty() ||
        (is_scalar_key(key) && !seen.insert(key).second)) {
      *error = "line " + std::to_string(line_number) +
               ": duplicate or empty setting";
      return false;
    }
    if (key == "charset") {
      CodepointRange range;
      if (!parse_range(value, &range)) {
        *error = "invalid charset";
        return false;
      }
      out->ranges.push_back(range);
    } else if (key == "charset_file")
      out->charset_files.emplace_back(value);
    else if (key == "axis") {
      const size_t colon = value.find(':');
      if (colon != 4u || value.find(':', colon + 1u) != std::string::npos) {
        *error = "invalid axis";
        return false;
      }
      Axis axis{value.substr(0, colon)};
      if (!parse_double(value.substr(colon + 1u), &axis.value)) {
        *error = "invalid axis value";
        return false;
      }
      for (char c : axis.tag)
        if (!std::isalnum(static_cast<unsigned char>(c))) {
          *error = "invalid axis tag";
          return false;
        }
      out->axes.push_back(axis);
    } else if (key == "type")
      out->type = value;
    else if (key == "source")
      out->source = value;
    else if (key == "file")
      out->output = value;
    else if (key == "face")
      out->face = value;
    else if (key == "face_index") {
      if (!parse_u32(value, &out->face_index)) {
        *error = "invalid face_index";
        return false;
      }
    } else if (key == "field")
      out->field = value;
    else if (key == "atlas_width") {
      if (!parse_u32(value, &out->atlas_width)) {
        *error = "invalid atlas_width";
        return false;
      }
    } else if (key == "atlas_height") {
      if (!parse_u32(value, &out->atlas_height)) {
        *error = "invalid atlas_height";
        return false;
      }
    } else if (key == "atlas_px_per_em") {
      if (!parse_double(value, &out->atlas_px_per_em)) {
        *error = "invalid atlas_px_per_em";
        return false;
      }
    } else if (key == "size") {
      if (!parse_double(value, &out->size)) {
        *error = "invalid size";
        return false;
      }
      out->has_size = true;
    } else if (key == "distance_range") {
      if (!parse_double(value, &out->distance_range)) {
        *error = "invalid distance_range";
        return false;
      }
    } else if (key == "inner_padding_px") {
      if (!parse_double(value, &out->inner_padding_px)) {
        *error = "invalid inner_padding_px";
        return false;
      }
    } else if (key == "outer_padding_px") {
      if (!parse_double(value, &out->outer_padding_px)) {
        *error = "invalid outer_padding_px";
        return false;
      }
    } else if (key == "edge_coloring")
      out->edge_coloring = value;
    else if (key == "edge_angle_degrees") {
      if (!parse_double(value, &out->edge_angle_degrees)) {
        *error = "invalid edge_angle_degrees";
        return false;
      }
    } else if (key == "edge_seed") {
      if (!parse_u64(value, &out->edge_seed)) {
        *error = "invalid edge_seed";
        return false;
      }
    } else if (key == "fallback") {
      if (!parse_codepoint(value, &out->fallback)) {
        *error = "invalid fallback";
        return false;
      }
      out->has_fallback = true;
    } else if (key == "fallback_policy")
      out->fallback_policy = value;
    else if (key == "pixel_format")
      out->pixel_format = value;
    else if (key == "miter_limit") {
      if (!parse_double(value, &out->miter_limit)) {
        *error = "invalid miter_limit";
        return false;
      }
    } else if (key == "scanline") {
      if (value == "true")
        out->scanline = true;
      else if (value == "false")
        out->scanline = false;
      else {
        *error = "scanline must be true or false";
        return false;
      }
    } else if (key == "threads") {
      if (!parse_u32(value, &out->threads)) {
        *error = "invalid threads";
        return false;
      }
    } else {
      *error = "unknown setting: " + key;
      return false;
    }
  }
  const std::set<std::string> required = {"type",
                                          "source",
                                          "file",
                                          "face",
                                          "face_index",
                                          "field",
                                          "atlas_width",
                                          "atlas_height",
                                          "atlas_px_per_em",
                                          "distance_range",
                                          "inner_padding_px",
                                          "outer_padding_px",
                                          "edge_coloring",
                                          "edge_angle_degrees",
                                          "edge_seed",
                                          "fallback",
                                          "fallback_policy",
                                          "pixel_format",
                                          "miter_limit",
                                          "scanline",
                                          "threads"};
  for (const auto &key : required)
    if (!seen.count(key)) {
      *error = "missing setting: " + key;
      return false;
    }
  if (out->ranges.empty() && out->charset_files.empty()) {
    *error = "at least one charset or charset_file is required";
    return false;
  }
  if (out->source.is_absolute() || out->output.is_absolute()) {
    *error = "source and file must be relative to the config";
    return false;
  }
  for (const fs::path &charset_file : out->charset_files)
    if (charset_file.is_absolute()) {
      *error = "charset_file must be relative to the config";
      return false;
    }
  if (out->type != "cooked_mtsdf" || out->field != "mtsdf" ||
      out->pixel_format != "rgba8_unorm" ||
      (out->edge_coloring != "simple" && out->edge_coloring != "inktrap" &&
       out->edge_coloring != "distance") ||
      (out->fallback_policy != "reject" && out->fallback_policy != "map")) {
    *error = "unsupported semantic setting";
    return false;
  }
  if (out->fallback_policy == "map" && !out->has_fallback) {
    *error = "map fallback policy requires fallback";
    return false;
  }
  if (out->has_size && (!(out->size > 0.0) || out->size > 4096.0)) {
    *error = "size out of range";
    return false;
  }
  if (out->atlas_width == 0 || out->atlas_height == 0 ||
      out->atlas_width > 16384u || out->atlas_height > 16384u ||
      !(out->atlas_px_per_em > 0.0 && out->atlas_px_per_em <= 4096.0) ||
      !(out->distance_range > 0.0 && out->distance_range <= 4096.0) ||
      out->inner_padding_px < 0.0 || out->outer_padding_px < 0.0 ||
      out->edge_angle_degrees <= 0.0 || out->edge_angle_degrees > 180.0 ||
      out->miter_limit < 0.0 || out->threads == 0 || out->threads > 256u ||
      out->face.size() > VKR_FONT_COOKED_MAX_FACE_BYTES) {
    *error = "setting out of range";
    return false;
  }
  std::sort(out->axes.begin(), out->axes.end(),
            [](const Axis &a, const Axis &b) { return a.tag < b.tag; });
  for (size_t i = 1; i < out->axes.size(); ++i)
    if (out->axes[i - 1u].tag == out->axes[i].tag) {
      *error = "duplicate axis";
      return false;
    }
  return true;
}

bool load_charset_file(const fs::path &path,
                       std::vector<CodepointRange> *ranges,
                       std::string *error) {
  std::ifstream input(path);
  if (!input) {
    *error = "unable to read charset_file: " + path.string();
    return false;
  }
  std::string line;
  while (std::getline(input, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream words(line);
    std::string word;
    while (words >> word) {
      CodepointRange range;
      if (!parse_range(word, &range)) {
        *error = "invalid charset_file codepoint: " + word;
        return false;
      }
      ranges->push_back(range);
    }
  }
  return true;
}

bool read_bytes(const fs::path &path, std::vector<uint8_t> *bytes) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    return false;
  const auto end = input.tellg();
  if (end < 0)
    return false;
  bytes->resize(static_cast<size_t>(end));
  input.seekg(0);
  return input.read(reinterpret_cast<char *>(bytes->data()), end).good() ||
         input.eof();
}

uint64_t glyph_seed(uint64_t seed, uint32_t glyph_id) {
  uint64_t z = seed + 0x9e3779b97f4a7c15ull * (uint64_t(glyph_id) + 1u);
  z = (z ^ (z >> 30u)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27u)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31u);
}

void add_identity(Sha256 &sha, const Config &config,
                  const std::vector<uint8_t> &source,
                  const std::vector<uint32_t> &codepoints,
                  const IdentityProvenance &provenance = {}) {
  hash_string(sha, "VKR_FONT_COOKED_IDENTITY_V1");
  hash_string(sha, provenance.atlas_commit);
  hash_string(sha, provenance.msdfgen_commit);
  hash_u32(sha, provenance.artifact_version);
  hash_u32(sha, provenance.cooker_version);
  sha_update(sha, source.data(), source.size());
  hash_string(sha, config.face);
  hash_u32(sha, config.face_index);
  hash_u32(sha, uint32_t(codepoints.size()));
  for (uint32_t cp : codepoints)
    hash_u32(sha, cp);
  hash_u32(sha, uint32_t(config.axes.size()));
  for (const Axis &axis : config.axes) {
    hash_string(sha, axis.tag);
    hash_double(sha, axis.value);
  }
  hash_string(sha, config.type);
  hash_string(sha, config.field);
  hash_u32(sha, config.atlas_width);
  hash_u32(sha, config.atlas_height);
  hash_double(sha, config.atlas_px_per_em);
  // Runtime font size does not change the em-normalized cooked artifact.
  hash_double(sha, config.distance_range);
  hash_double(sha, config.inner_padding_px);
  hash_double(sha, config.outer_padding_px);
  hash_string(sha, config.edge_coloring);
  hash_double(sha, config.edge_angle_degrees);
  hash_u64(sha, config.edge_seed);
  hash_u32(sha, config.has_fallback ? config.fallback : 0xffffffffu);
  hash_string(sha, config.fallback_policy);
  hash_string(sha, config.pixel_format);
  hash_double(sha, config.miter_limit);
  hash_u32(sha, config.scanline ? 1u : 0u);
  hash_u32(sha, config.threads);
}

std::array<uint8_t, 32>
font_identity(const Config &config, const std::vector<uint8_t> &source,
              const std::vector<uint32_t> &codepoints,
              const IdentityProvenance &provenance = {}) {
  Sha256 hash;
  add_identity(hash, config, source, codepoints, provenance);
  return sha_final(hash);
}

bool identity_self_test(void) {
  Config base;
  base.type = "cooked_mtsdf";
  base.face = "Fixture/Regular";
  base.face_index = 0u;
  base.axes = {{"wght", 400.0}};
  base.field = "mtsdf";
  base.atlas_width = 1024u;
  base.atlas_height = 1024u;
  base.atlas_px_per_em = 64.0;
  base.size = 32.0;
  base.has_size = true;
  base.distance_range = 8.0;
  base.inner_padding_px = 0.0;
  base.outer_padding_px = 0.0;
  base.edge_coloring = "inktrap";
  base.edge_angle_degrees = 3.0;
  base.edge_seed = 1u;
  base.has_fallback = true;
  base.fallback = '?';
  base.fallback_policy = "reject";
  base.pixel_format = "rgba8_unorm";
  base.miter_limit = 1.0;
  base.scanline = true;
  base.threads = 1u;
  const std::vector<uint8_t> source = {0x00u, 0x01u, 0x02u, 0x03u};
  const std::vector<uint32_t> codepoints = {'?', 'A', 'V'};
  const IdentityProvenance provenance;
  const auto expected = font_identity(base, source, codepoints, provenance);

  auto require_changed = [&](const char *label, const Config &config,
                             const std::vector<uint8_t> &font_bytes,
                             const std::vector<uint32_t> &charset,
                             const IdentityProvenance &versions) {
    if (font_identity(config, font_bytes, charset, versions) == expected) {
      std::cerr << "vkr_font_cooker: identity did not change for " << label
                << "\n";
      return false;
    }
    return true;
  };

  std::vector<uint8_t> changed_source = source;
  changed_source.back() ^= 0x80u;
  if (!require_changed("source bytes", base, changed_source, codepoints,
                       provenance))
    return false;
  Config changed = base;
  changed.face_index++;
  if (!require_changed("face index", changed, source, codepoints, provenance))
    return false;
  std::vector<uint32_t> changed_charset = codepoints;
  changed_charset.push_back('W');
  if (!require_changed("charset", base, source, changed_charset, provenance))
    return false;
  changed = base;
  changed.axes[0].value = 401.0;
  if (!require_changed("axis", changed, source, codepoints, provenance))
    return false;
  changed = base;
  changed.distance_range = 9.0;
  if (!require_changed("generator setting", changed, source, codepoints,
                       provenance))
    return false;
  changed = base;
  changed.size = 48.0;
  if (font_identity(changed, source, codepoints, provenance) != expected) {
    std::cerr << "vkr_font_cooker: runtime size changed artifact identity\n";
    return false;
  }
  IdentityProvenance changed_versions = provenance;
  changed_versions.cooker_version++;
  if (!require_changed("cooker version", base, source, codepoints,
                       changed_versions))
    return false;
  changed_versions = provenance;
  changed_versions.atlas_commit[0] ^= 1;
  if (!require_changed("msdf-atlas-gen commit", base, source, codepoints,
                       changed_versions))
    return false;
  changed_versions = provenance;
  changed_versions.msdfgen_commit[0] ^= 1;
  if (!require_changed("msdfgen commit", base, source, codepoints,
                       changed_versions))
    return false;
  std::cout << "status=passed test=font_identity_inputs\n";
  return true;
}

bool validate_selected_face(const Config &config,
                            const std::vector<uint8_t> &source,
                            std::string *error) {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
  bool valid = false;
  if (FT_Init_FreeType(&library) != 0 ||
      FT_New_Memory_Face(library, source.data(),
                         static_cast<FT_Long>(source.size()),
                         static_cast<FT_Long>(config.face_index), &face) != 0) {
    *error = "unable to load configured font face";
    goto cleanup;
  }
  if (!face->family_name || !face->style_name) {
    *error = "selected font face has no family/style identity";
    goto cleanup;
  }
  {
    const std::string actual =
        std::string(face->family_name) + "/" + face->style_name;
    if (config.face != actual) {
      *error = "face must match selected family/style '" + actual + "'";
      goto cleanup;
    }
  }
  valid = true;
cleanup:
  if (face)
    FT_Done_Face(face);
  if (library)
    FT_Done_FreeType(library);
  return valid;
}

bool configure_axes(FT_Library library, FT_Face face,
                    const std::vector<Axis> &axes, std::string *error) {
  if (axes.empty())
    return true;
  FT_MM_Var *variation = nullptr;
  if (FT_Get_MM_Var(face, &variation) != 0 || !variation) {
    *error = "axes requested for a non-variable font";
    return false;
  }
  std::vector<FT_Fixed> coordinates(variation->num_axis);
  if (FT_Get_Var_Design_Coordinates(face, variation->num_axis,
                                    coordinates.data()) != 0) {
    FT_Done_MM_Var(library, variation);
    *error = "unable to read variable font axes";
    return false;
  }
  for (const Axis &axis : axes) {
    uint32_t tag =
        FT_MAKE_TAG(axis.tag[0], axis.tag[1], axis.tag[2], axis.tag[3]);
    bool found = false;
    for (FT_UInt i = 0; i < variation->num_axis; ++i)
      if (variation->axis[i].tag == tag) {
        const double min = variation->axis[i].minimum / 65536.0;
        const double max = variation->axis[i].maximum / 65536.0;
        if (axis.value < min || axis.value > max) {
          FT_Done_MM_Var(library, variation);
          *error = "axis value outside font range";
          return false;
        }
        coordinates[i] =
            static_cast<FT_Fixed>(std::llround(axis.value * 65536.0));
        found = true;
        break;
      }
    if (!found) {
      FT_Done_MM_Var(library, variation);
      *error = "axis tag not present in font";
      return false;
    }
  }
  const FT_Error result = FT_Set_Var_Design_Coordinates(
      face, variation->num_axis, coordinates.data());
  FT_Done_MM_Var(library, variation);
  if (result != 0) {
    *error = "unable to apply variable font axes";
    return false;
  }
  return true;
}

using GlyphList = std::vector<msdf_atlas::GlyphGeometry>;

bool cook(const Config &config, const fs::path &output,
          const std::vector<uint8_t> &source,
          const std::array<uint8_t, 32> &identity, VkrAllocator *allocator,
          uint64_t *format_temp_bytes, uint32_t *glyph_count,
          uint32_t *codepoint_count, uint32_t *kerning_count, uint64_t *bytes,
          uint64_t *gpu_bytes, std::string *error) {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
  msdfgen::FontHandle *font = nullptr;
  uint8_t *encoded = nullptr;
  uint64_t encoded_size = 0;
  const bool success = [&]() {
    if (FT_Init_FreeType(&library) != 0 ||
        FT_New_Memory_Face(
            library, source.data(), static_cast<FT_Long>(source.size()),
            static_cast<FT_Long>(config.face_index), &face) != 0) {
      *error = "unable to load font face";
      return false;
    }
    if (!configure_axes(library, face, config.axes, error))
      return false;
    font = msdfgen::adoptFreetypeFont(face);
    if (!font) {
      *error = "unable to adopt FreeType face";
      return false;
    }

    std::vector<uint32_t> requested;
    for (const auto &range : config.ranges)
      for (uint32_t cp = range.first;; ++cp) {
        requested.push_back(cp);
        if (cp == range.last)
          break;
      }
    for (const fs::path &charset_file : config.charset_files) {
      std::vector<CodepointRange> ranges;
      if (!load_charset_file(charset_file, &ranges, error))
        return false;
      for (const auto &range : ranges)
        for (uint32_t cp = range.first;; ++cp) {
          requested.push_back(cp);
          if (cp == range.last)
            break;
        }
    }
    std::sort(requested.begin(), requested.end());
    requested.erase(std::unique(requested.begin(), requested.end()),
                    requested.end());

    std::map<uint32_t, uint32_t> mapping;
    std::set<uint32_t> glyph_ids;
    for (uint32_t cp : requested) {
      FT_UInt gid = FT_Get_Char_Index(face, cp);
      if (gid == 0u) {
        if (config.fallback_policy == "reject") {
          *error = "charset contains a missing glyph";
          return false;
        }
        gid =
            config.has_fallback ? FT_Get_Char_Index(face, config.fallback) : 0u;
        if (gid == 0u) {
          *error = "fallback glyph is missing";
          return false;
        }
      }
      mapping[cp] = gid;
      glyph_ids.insert(gid);
    }
    if (config.has_fallback) {
      const uint32_t fallback_gid = FT_Get_Char_Index(face, config.fallback);
      if (fallback_gid == 0u) {
        *error = "fallback glyph is missing";
        return false;
      }
      glyph_ids.insert(fallback_gid);
      mapping.emplace(config.fallback, fallback_gid);
    }
    if (glyph_ids.empty() || glyph_ids.size() > VKR_FONT_COOKED_MAX_GLYPHS ||
        mapping.size() > VKR_FONT_COOKED_MAX_CODEPOINTS) {
      *error = "font coverage exceeds VKFA limits";
      return false;
    }

    msdf_atlas::Charset glyphset;
    for (uint32_t gid : glyph_ids)
      glyphset.add(gid);
    GlyphList glyphs;
    msdf_atlas::FontGeometry geometry(&glyphs);
    // FontGeometry divides fontScale by the source em size internally. A
    // fontScale of one therefore produces the VKFA contract where one em is
    // 1.0; pre-dividing here would normalize twice and collapse every glyph.
    if (geometry.loadGlyphset(font, 1.0, glyphset, true, true) !=
        static_cast<int>(glyph_ids.size())) {
      *error = "unable to load glyph geometry";
      return false;
    }
    if (std::abs(geometry.getMetrics().emSize - 1.0) > 1e-9) {
      *error = "generator metrics are not normalized to one em";
      return false;
    }
    void (*coloring)(msdfgen::Shape &, double, unsigned long long) =
        &msdfgen::edgeColoringSimple;
    if (config.edge_coloring == "inktrap")
      coloring = &msdfgen::edgeColoringInkTrap;
    if (config.edge_coloring == "distance")
      coloring = &msdfgen::edgeColoringByDistance;
    const double angle =
        config.edge_angle_degrees * 3.14159265358979323846 / 180.0;
    for (auto &glyph : glyphs)
      glyph.edgeColoring(coloring, angle,
                         glyph_seed(config.edge_seed, glyph.getIndex()));

    msdf_atlas::TightAtlasPacker packer;
    packer.setDimensions(static_cast<int>(config.atlas_width),
                         static_cast<int>(config.atlas_height));
    packer.setScale(config.atlas_px_per_em);
    packer.setPixelRange(msdfgen::Range(config.distance_range));
    packer.setMiterLimit(config.miter_limit);
    packer.setSpacing(0);
    packer.setInnerPixelPadding(msdf_atlas::Padding(config.inner_padding_px));
    packer.setOuterPixelPadding(msdf_atlas::Padding(config.outer_padding_px));
    if (packer.pack(glyphs.data(), static_cast<int>(glyphs.size())) != 0) {
      *error = "glyphs do not fit fixed atlas dimensions";
      return false;
    }

    msdf_atlas::ImmediateAtlasGenerator<
        float, 4, msdf_atlas::mtsdfGenerator,
        msdf_atlas::BitmapAtlasStorage<uint8_t, 4>>
        generator(config.atlas_width, config.atlas_height);
    msdf_atlas::GeneratorAttributes attributes;
    attributes.scanlinePass = config.scanline;
    generator.setAttributes(attributes);
    generator.setThreadCount(static_cast<int>(config.threads));
    generator.generate(glyphs.data(), static_cast<int>(glyphs.size()));
    const msdfgen::BitmapConstSection<uint8_t, 4> bitmap =
        generator.atlasStorage();
    std::vector<uint8_t> pixels(size_t(config.atlas_width) *
                                config.atlas_height * 4u);
    for (uint32_t y = 0; y < config.atlas_height; ++y)
      std::memcpy(pixels.data() + size_t(y) * config.atlas_width * 4u,
                  bitmap(0, static_cast<int>(y)),
                  size_t(config.atlas_width) * 4u);

    std::vector<VkrFontCookedGlyph> records;
    records.reserve(glyphs.size());
    for (const auto &glyph : glyphs) {
      VkrFontCookedGlyph record{};
      double l, b, r, t, ul, ub, ur, ut;
      glyph.getQuadPlaneBounds(l, b, r, t);
      glyph.getQuadAtlasBounds(ul, ub, ur, ut);
      record.glyph_id = glyph.getIndex();
      record.flags =
          glyph.isWhitespace() ? 0u : VKR_FONT_COOKED_GLYPH_HAS_GEOMETRY;
      record.advance = static_cast<float>(glyph.getAdvance());
      record.plane_left = static_cast<float>(l);
      record.plane_bottom = static_cast<float>(b);
      record.plane_right = static_cast<float>(r);
      record.plane_top = static_cast<float>(t);
      record.uv_left = static_cast<float>(ul / config.atlas_width);
      record.uv_right = static_cast<float>(ur / config.atlas_width);
      record.uv_bottom = static_cast<float>(ub / config.atlas_height);
      record.uv_top = static_cast<float>(ut / config.atlas_height);
      records.push_back(record);
    }
    std::vector<VkrFontCookedCodepoint> codepoints;
    for (const auto &item : mapping)
      codepoints.push_back({item.first, item.second});
    std::vector<VkrFontCookedKerning> kernings;
    for (const auto &item : geometry.getKerning())
      kernings.push_back({static_cast<uint32_t>(item.first.first),
                          static_cast<uint32_t>(item.first.second),
                          static_cast<float>(item.second)});
    if (kernings.size() > VKR_FONT_COOKED_MAX_KERNINGS) {
      *error = "kerning table exceeds VKFA limit";
      return false;
    }

    VkrFontCookedPage page{};
    page.width = config.atlas_width;
    page.height = config.atlas_height;
    page.row_stride = config.atlas_width * 4u;
    page.pixel_format = VKR_FONT_COOKED_PIXEL_RGBA8_UNORM;
    page.pixels = pixels.data();
    page.pixel_size = pixels.size();
    VkrFontCookedMetrics metrics{};
    const auto &font_metrics = geometry.getMetrics();
    metrics.line_height = static_cast<float>(font_metrics.lineHeight);
    metrics.ascender = static_cast<float>(font_metrics.ascenderY);
    metrics.descender = static_cast<float>(font_metrics.descenderY);
    metrics.underline_y = static_cast<float>(font_metrics.underlineY);
    metrics.underline_thickness =
        static_cast<float>(font_metrics.underlineThickness);
    metrics.distance_range = static_cast<float>(config.distance_range);
    metrics.atlas_px_per_em = static_cast<float>(config.atlas_px_per_em);
    metrics.units_per_em = face->units_per_EM;

    VkrFontCookedEncodeInfo info{};
    info.face = string8_create(
        reinterpret_cast<uint8_t *>(const_cast<char *>(config.face.data())),
        config.face.size());
    std::memcpy(info.identity, identity.data(), identity.size());
    info.cooker_version = kCookerVersion;
    info.field_kind = VKR_FONT_COOKED_FIELD_MTSDF;
    info.fallback_glyph_id =
        config.has_fallback ? FT_Get_Char_Index(face, config.fallback) : 0u;
    info.metrics = metrics;
    info.glyphs = records.data();
    info.glyph_count = records.size();
    info.codepoints = codepoints.data();
    info.codepoint_count = codepoints.size();
    info.kernings = kernings.data();
    info.kerning_count = kernings.size();
    info.pages = &page;
    info.page_count = 1u;
    const std::string output_string = output.string();
    VkrAllocatorScope format_scope = vkr_allocator_begin_scope(allocator);
    const uint64_t format_temp_start =
        arena_pos(static_cast<Arena *>(allocator->ctx));
    const bool encoded_ok =
        vkr_font_cooked_encode(allocator, &info, &encoded, &encoded_size) &&
        vkr_font_cooked_write_atomic(
            allocator,
            string8_create(reinterpret_cast<uint8_t *>(
                               const_cast<char *>(output_string.data())),
                           output_string.size()),
            encoded, encoded_size);
    const uint64_t format_temp_end =
        arena_pos(static_cast<Arena *>(allocator->ctx));
    if (format_temp_bytes)
      *format_temp_bytes = format_temp_end - format_temp_start;
    vkr_allocator_end_scope(&format_scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
    if (!encoded_ok) {
      *error = "unable to encode or publish VKFA";
      return false;
    }
    *glyph_count = records.size();
    *codepoint_count = codepoints.size();
    *kerning_count = kernings.size();
    *bytes = encoded_size;
    *gpu_bytes = pixels.size();
    return true;
  }();
  if (font)
    msdfgen::destroyFont(font);
  if (face)
    FT_Done_Face(face);
  if (library)
    FT_Done_FreeType(library);
  return success;
}

bool parse_args(int argc, char **argv, Options *out) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc)
      out->config_path = argv[++i];
    else if (arg == "--output" && i + 1 < argc) {
      out->output_override = argv[++i];
      out->has_output_override = true;
    } else if (arg == "--force")
      out->force = true;
    else if (arg == "--identity-self-test")
      out->identity_self_test = true;
    else if (arg == "--help" || arg == "-h")
      return false;
    else
      return false;
  }
  return out->identity_self_test || !out->config_path.empty();
}

void print_usage(const char *program) {
  std::cerr << "Usage: " << program
            << " --config <file.fontcfg> [--output <file.vkfa>] [--force]\n"
            << "       " << program << " --identity-self-test\n";
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_args(argc, argv, &options)) {
    print_usage(argv[0]);
    return 2;
  }
  if (options.identity_self_test) {
    return identity_self_test() ? 0 : 1;
  }
  Config config;
  std::string error;
  if (!parse_config(options.config_path, &config, &error)) {
    std::cerr << "vkr_font_cooker: " << error << "\n";
    return 1;
  }
  const fs::path config_dir = options.config_path.parent_path();
  config.source = config_dir / config.source;
  for (auto &path : config.charset_files)
    path = config_dir / path;
  fs::path output = options.has_output_override ? options.output_override
                                                : config_dir / config.output;
  std::vector<uint8_t> source;
  if (!read_bytes(config.source, &source) ||
      source.size() > std::numeric_limits<FT_Long>::max()) {
    std::cerr << "vkr_font_cooker: unable to read source font\n";
    return 1;
  }
  if (!validate_selected_face(config, source, &error)) {
    std::cerr << "vkr_font_cooker: " << error << "\n";
    return 1;
  }
  std::vector<CodepointRange> all_ranges = config.ranges;
  for (const auto &path : config.charset_files)
    if (!load_charset_file(path, &all_ranges, &error)) {
      std::cerr << "vkr_font_cooker: " << error << "\n";
      return 1;
    }
  std::vector<uint32_t> semantic_codepoints;
  for (const auto &range : all_ranges)
    for (uint32_t cp = range.first;; ++cp) {
      semantic_codepoints.push_back(cp);
      if (cp == range.last)
        break;
    }
  std::sort(semantic_codepoints.begin(), semantic_codepoints.end());
  semantic_codepoints.erase(
      std::unique(semantic_codepoints.begin(), semantic_codepoints.end()),
      semantic_codepoints.end());
  const auto identity = font_identity(config, source, semantic_codepoints);
  if (!output.parent_path().empty())
    fs::create_directories(output.parent_path());
  const auto start = std::chrono::steady_clock::now();
  VkrFontCookedInspection inspection{};
  std::vector<uint8_t> existing;
  if (!options.force && read_bytes(output, &existing) &&
      vkr_font_cooked_inspect(existing.data(), existing.size(), &inspection) &&
      std::memcmp(inspection.identity, identity.data(), identity.size()) == 0) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::cout << "status=skipped cook_ms=" << ms << " bytes=" << existing.size()
              << " format_temp_bytes=0 gpu_bytes="
              << (uint64_t(config.atlas_width) * config.atlas_height * 4u)
              << " glyphs=" << inspection.glyph_count
              << " codepoints=" << inspection.codepoint_count
              << " kernings=" << inspection.kerning_count << " identity=";
    for (uint8_t byte : identity)
      std::cout << std::hex << std::setw(2) << std::setfill('0')
                << unsigned(byte);
    std::cout << std::dec << "\n";
    return 0;
  }
  Arena *scratch_arena = arena_create(GB(2), MB(64));
  if (!scratch_arena) {
    std::cerr << "vkr_font_cooker: unable to create scratch arena\n";
    return 1;
  }
  uint64_t format_temp_bytes = 0, bytes = 0, gpu_bytes = 0;
  uint32_t glyphs = 0, codepoints = 0, kernings = 0;
  VkrAllocator allocator = {};
  allocator.ctx = scratch_arena;
  if (!vkr_allocator_arena(&allocator)) {
    arena_destroy(scratch_arena);
    std::cerr << "vkr_font_cooker: unable to initialize allocator\n";
    return 1;
  }
  const bool success =
      cook(config, output, source, identity, &allocator, &format_temp_bytes,
           &glyphs, &codepoints, &kernings, &bytes, &gpu_bytes, &error);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(scratch_arena);
  if (!success) {
    std::cerr << "vkr_font_cooker: " << error << "\n";
    return 1;
  }
  std::cout << "status=cooked cook_ms=" << ms << " bytes=" << bytes
            << " format_temp_bytes=" << format_temp_bytes
            << " gpu_bytes=" << gpu_bytes << " glyphs=" << glyphs
            << " codepoints=" << codepoints << " kernings=" << kernings
            << " identity=";
  for (uint8_t byte : identity)
    std::cout << std::hex << std::setw(2) << std::setfill('0')
              << unsigned(byte);
  std::cout << std::dec << "\n";
  return 0;
}
