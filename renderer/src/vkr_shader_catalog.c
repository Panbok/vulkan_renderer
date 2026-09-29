#include "vkr_shader_catalog.h"

#include "core/logger.h"
#include "core/vkr_hash.h"
#include "core/vkr_json.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

vkr_internal char vkr_shader_catalog_override[4096];

void vkr_shader_catalog_set_root(const char *path) {
  (void)snprintf(vkr_shader_catalog_override,
                 sizeof(vkr_shader_catalog_override), "%s", path ? path : "");
}

vkr_internal bool8_t vkr_shader_catalog_is_file(const char *path) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return false_v;
  }
  fclose(file);
  return true_v;
}

vkr_internal bool8_t vkr_shader_catalog_has_manifest(const char *root) {
  static const char *const backends[] = {"metal", "vulkan"};
  for (uint32_t i = 0u; i < ArrayCount(backends); ++i) {
    char manifest[4096];
    if ((uint32_t)snprintf(manifest, sizeof(manifest),
                           "%s/%s/shader_manifest.json", root,
                           backends[i]) < sizeof(manifest) &&
        vkr_shader_catalog_is_file(manifest)) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_shader_catalog_root(char *out, uint32_t capacity) {
  if (vkr_shader_catalog_override[0]) {
    return (uint32_t)snprintf(out, capacity, "%s",
                              vkr_shader_catalog_override) < capacity;
  }
  const char *environment = getenv("VKR_SHADER_CATALOG");
  if (environment && environment[0]) {
    return (uint32_t)snprintf(out, capacity, "%s", environment) < capacity;
  }
  char executable[4096];
  if (vkr_platform_executable_path(executable, sizeof(executable))) {
    char *slash = strrchr(executable, '/');
    char *backslash = strrchr(executable, '\\');
    if (backslash && (!slash || backslash > slash)) {
      slash = backslash;
    }
    if (slash) {
      *slash = 0;
      char bundled[4096];
      if ((uint32_t)snprintf(bundled, sizeof(bundled), "%s/shaders",
                             executable) < sizeof(bundled) &&
          vkr_shader_catalog_has_manifest(bundled)) {
        return (uint32_t)snprintf(out, capacity, "%s", bundled) < capacity;
      }
      /* A macOS application bundle keeps it in Contents/Resources. */
      if ((uint32_t)snprintf(bundled, sizeof(bundled),
                             "%s/../Resources/shaders",
                             executable) < sizeof(bundled) &&
          vkr_shader_catalog_has_manifest(bundled)) {
        return (uint32_t)snprintf(out, capacity, "%s", bundled) < capacity;
      }
    }
  }
#if defined(VKR_SHADER_CATALOG_DEFAULT)
  return (uint32_t)snprintf(out, capacity, "%s", VKR_SHADER_CATALOG_DEFAULT) <
         capacity;
#else
  return (uint32_t)snprintf(out, capacity, "shaders") < capacity;
#endif
}

bool8_t vkr_shader_catalog_path(const char *backend, const char *file,
                                char *out, uint32_t capacity) {
  char root[4096];
  if (!backend || !file || !vkr_shader_catalog_root(root, sizeof(root))) {
    return false_v;
  }
  const int written = snprintf(out, capacity, "%s/%s/%s", root, backend, file);
  return written > 0 && (uint32_t)written < capacity;
}

vkr_internal bool8_t
vkr_shader_catalog_file_hash(const char *path, char out[VKR_SHA256_HEX_SIZE]) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return false_v;
  }
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  uint8_t chunk[64u * 1024u];
  size_t read = 0u;
  while ((read = fread(chunk, 1u, sizeof(chunk), file)) > 0u) {
    vkr_sha256_update(&hash, chunk, read);
  }
  const bool8_t ok = !ferror(file);
  fclose(file);
  if (!ok) {
    return false_v;
  }
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  vkr_sha256_final(&hash, digest);
  vkr_sha256_hex(digest, out);
  return true_v;
}

vkr_internal bool8_t vkr_shader_catalog_field(VkrJsonReader object,
                                              const char *name, char *out,
                                              uint32_t capacity) {
  String8 value = {0};
  if (!vkr_json_find_field(&object, name) ||
      !vkr_json_parse_string(&object, &value) || value.length >= capacity) {
    return false_v;
  }
  MemCopy(out, value.str, value.length);
  out[value.length] = 0;
  return true_v;
}

bool8_t vkr_shader_catalog_metallib(const char *library, char *out_metallib,
                                    char *out_source, uint32_t capacity) {
  out_metallib[0] = 0;
  char default_source[256];
  (void)snprintf(default_source, sizeof(default_source), "%s.metal", library);
  if (!vkr_shader_catalog_path("metal", default_source, out_source, capacity)) {
    return false_v;
  }
  char manifest_path[4096];
  if (!vkr_shader_catalog_path("metal", "shader_manifest.json", manifest_path,
                               sizeof(manifest_path))) {
    return false_v;
  }
  FILE *file = file_fopen(manifest_path, "rb");
  if (!file) {
    return false_v;
  }
  char text[64u * 1024u];
  const size_t length = fread(text, 1u, sizeof(text) - 1u, file);
  fclose(file);
  text[length] = 0;
  VkrJsonReader reader =
      vkr_json_reader_create((const uint8_t *)text, (uint64_t)length);
  if (!vkr_json_find_array(&reader, "libraries")) {
    return false_v;
  }
  while (vkr_json_next_array_element(&reader)) {
    VkrJsonReader object = {0};
    if (!vkr_json_enter_object(&reader, &object)) {
      return false_v;
    }
    char name[256];
    char source[256];
    char source_hash[VKR_SHA256_HEX_SIZE];
    char metallib[256];
    if (!vkr_shader_catalog_field(object, "name", name, sizeof(name)) ||
        strcmp(name, library) != 0) {
      continue;
    }
    if (!vkr_shader_catalog_field(object, "source", source, sizeof(source)) ||
        !vkr_shader_catalog_path("metal", source, out_source, capacity)) {
      return false_v;
    }
    if (!vkr_shader_catalog_field(object, "source_hash", source_hash,
                                  sizeof(source_hash)) ||
        !vkr_shader_catalog_field(object, "metallib", metallib,
                                  sizeof(metallib))) {
      return false_v; /* No metallib: the toolchain was unavailable. */
    }
    /* A failed rebuild can leave a newer source beside an older metallib;
     * only a metallib compiled from the current source is trusted. */
    char current[VKR_SHA256_HEX_SIZE];
    if (!vkr_shader_catalog_file_hash(out_source, current) ||
        strcmp(current, source_hash) != 0) {
      log_warn("Shader catalog: %s metallib is older than its source; "
               "compiling the source",
               library);
      return false_v;
    }
    char metallib_path[4096];
    if (!vkr_shader_catalog_path("metal", metallib, metallib_path,
                                 sizeof(metallib_path)) ||
        !vkr_shader_catalog_is_file(metallib_path) ||
        (uint32_t)snprintf(out_metallib, capacity, "%s", metallib_path) >=
            capacity) {
      out_metallib[0] = 0;
      return false_v;
    }
    return true_v;
  }
  return false_v;
}
