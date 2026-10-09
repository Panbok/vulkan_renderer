#include "level/vkr_surface.h"

#include "core/vkr_json.h"

#include <stdio.h>
#include <string.h>

const char *const vkr_surface_names[] = {
    "none", "concrete", "metal", "wood",   "tile",  "plaster",  "brick", "rock",
    "dirt", "grass",    "glass", "fabric", "water", "emissive", NULL};

const char *const vkr_surface_labels[] = {
    "None", "Concrete", "Metal", "Wood",   "Tile",  "Plaster",  "Brick", "Rock",
    "Dirt", "Grass",    "Glass", "Fabric", "Water", "Emissive", NULL};

const char *const vkr_surface_mark_names[] = {
    "none", "hazard", "orange", "blue", "red", "green", "dark", NULL};

const char *const vkr_surface_mark_labels[] = {
    "None", "Hazard", "Orange", "Blue", "Red", "Green", "Dark", NULL};

_Static_assert(ArrayCount(vkr_surface_names) == VKR_SURFACE_COUNT + 1u,
               "one name per surface tag");
_Static_assert(ArrayCount(vkr_surface_labels) == VKR_SURFACE_COUNT + 1u,
               "one label per surface tag");
_Static_assert(ArrayCount(vkr_surface_mark_names) ==
                   VKR_SURFACE_MARK_COUNT + 1u,
               "one name per mark");
_Static_assert(ArrayCount(vkr_surface_mark_labels) ==
                   VKR_SURFACE_MARK_COUNT + 1u,
               "one label per mark");

#define SURFACE_GREYBOX_DIR "assets/materials/greybox/"
#define SURFACE_LOOKS(prefix)                                                  \
  {SURFACE_GREYBOX_DIR prefix "_floor.mt",                                     \
   SURFACE_GREYBOX_DIR prefix "_wall.mt",                                      \
   SURFACE_GREYBOX_DIR prefix "_ceiling.mt"}

/* The greybox looks, one per orientation; the files are engine content
   (cmake/vkr_engine_content.cmake). */
static const char
    *const s_surface_looks[VKR_SURFACE_COUNT][VKR_SURFACE_ORIENTATION_COUNT] = {
        SURFACE_LOOKS("none"),  SURFACE_LOOKS("concrete"),
        SURFACE_LOOKS("metal"), SURFACE_LOOKS("wood"),
        SURFACE_LOOKS("tile"),  SURFACE_LOOKS("plaster"),
        SURFACE_LOOKS("brick"), SURFACE_LOOKS("rock"),
        SURFACE_LOOKS("dirt"),  SURFACE_LOOKS("grass"),
        SURFACE_LOOKS("glass"), SURFACE_LOOKS("fabric"),
        SURFACE_LOOKS("water"), SURFACE_LOOKS("emissive"),
};

static const char *const
    s_mark_looks[VKR_SURFACE_MARK_COUNT][VKR_SURFACE_ORIENTATION_COUNT] = {
        SURFACE_LOOKS("none"),        SURFACE_LOOKS("mark_hazard"),
        SURFACE_LOOKS("mark_orange"), SURFACE_LOOKS("mark_blue"),
        SURFACE_LOOKS("mark_red"),    SURFACE_LOOKS("mark_green"),
        SURFACE_LOOKS("mark_dark"),
};

/* A face whose normal leans this far up or down is a floor or a ceiling;
   about 45 degrees, so ramps read as floors. */
#define SURFACE_LEVEL_NORMAL_Y 0.7f

static bool8_t surface_name_index(const char *const *names, const char *name,
                                  uint32_t *out) {
  if (!name) {
    return false_v;
  }
  for (uint32_t i = 0; names[i]; ++i) {
    if (strcmp(names[i], name) == 0) {
      *out = i;
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_surface_find(const char *name, VkrSurface *out) {
  uint32_t index = 0u;
  if (!surface_name_index(vkr_surface_names, name, &index)) {
    return false_v;
  }
  *out = (VkrSurface)index;
  return true_v;
}

bool8_t vkr_surface_mark_find(const char *name, VkrSurfaceMark *out) {
  uint32_t index = 0u;
  if (!surface_name_index(vkr_surface_mark_names, name, &index)) {
    return false_v;
  }
  *out = (VkrSurfaceMark)index;
  return true_v;
}

VkrSurfaceOrientation vkr_surface_orientation(float32_t normal_y) {
  if (normal_y >= SURFACE_LEVEL_NORMAL_Y) {
    return VKR_SURFACE_FLOOR;
  }
  if (normal_y <= -SURFACE_LEVEL_NORMAL_Y) {
    return VKR_SURFACE_CEILING;
  }
  return VKR_SURFACE_WALL;
}

const char *vkr_surface_greybox_material(VkrSurface surface,
                                         VkrSurfaceMark mark,
                                         VkrSurfaceOrientation orientation) {
  const uint32_t side = (uint32_t)orientation < VKR_SURFACE_ORIENTATION_COUNT
                            ? (uint32_t)orientation
                            : VKR_SURFACE_WALL;
  if ((uint32_t)mark != VKR_SURFACE_MARK_NONE &&
      (uint32_t)mark < VKR_SURFACE_MARK_COUNT) {
    return s_mark_looks[mark][side];
  }
  const uint32_t tag =
      (uint32_t)surface < VKR_SURFACE_COUNT ? (uint32_t)surface : 0u;
  return s_surface_looks[tag][side];
}

// =============================================================================
// Themes
// =============================================================================

static void theme_error(char *error, uint32_t capacity, const char *format,
                        const char *name) {
  if (error && capacity > 0u) {
    snprintf(error, capacity, format, name);
  }
}

/* The tag whose name is `text`, or VKR_SURFACE_COUNT. */
static uint32_t theme_tag(String8 text) {
  for (uint32_t tag = 0; tag < VKR_SURFACE_COUNT; ++tag) {
    const uint64_t length = strlen(vkr_surface_names[tag]);
    if (text.length == length &&
        MemCompare(text.str, vkr_surface_names[tag], length) == 0) {
      return tag;
    }
  }
  return VKR_SURFACE_COUNT;
}

bool8_t vkr_surface_theme_read(String8 json, VkrSurfaceTheme *out, char *error,
                               uint32_t capacity) {
  MemZero(out, sizeof(*out));
  VkrJsonReader root = vkr_json_reader_from_string(json);
  VkrJsonReader member = root;
  float32_t version = 0.0f;
  if (!vkr_json_find_root_field(&member, "version") ||
      !vkr_json_parse_float(&member, &version) ||
      version != (float32_t)VKR_SURFACE_THEME_VERSION) {
    theme_error(error, capacity, "%s must be 1", "version");
    return false_v;
  }

  member = root;
  if (!vkr_json_find_root_field(&member, "materials")) {
    return true_v;
  }
  VkrJsonReader materials;
  if (!vkr_json_enter_object(&member, &materials)) {
    theme_error(error, capacity, "%s must be an object", "materials");
    return false_v;
  }

  /* Members in order: "tag": "path", separated by commas. */
  materials.pos = 1u;
  for (;;) {
    vkr_json_skip_whitespace(&materials);
    if (materials.pos >= materials.length) {
      break;
    }
    const uint8_t next = materials.data[materials.pos];
    if (next == '}') {
      break;
    }
    if (next == ',') {
      materials.pos++;
      continue;
    }
    String8 key = {0};
    String8 path = {0};
    if (!vkr_json_parse_string(&materials, &key)) {
      theme_error(error, capacity, "%s holds a member that is not a string",
                  "materials");
      return false_v;
    }
    vkr_json_skip_whitespace(&materials);
    if (materials.pos >= materials.length ||
        materials.data[materials.pos] != ':') {
      theme_error(error, capacity, "%s holds a member without a value",
                  "materials");
      return false_v;
    }
    materials.pos++;
    const uint32_t tag = theme_tag(key);
    char name[32] = {0};
    MemCopy(name, key.str, Min(key.length, (uint64_t)sizeof(name) - 1u));
    if (tag == VKR_SURFACE_COUNT || tag == VKR_SURFACE_NONE) {
      theme_error(error, capacity, "materials.%s names no surface tag", name);
      return false_v;
    }
    if (!vkr_json_parse_string(&materials, &path) ||
        path.length >= VKR_SURFACE_THEME_PATH_CAPACITY ||
        memchr(path.str, '\\', path.length)) {
      theme_error(error, capacity,
                  "materials.%s must be a material path under 128 bytes", name);
      return false_v;
    }
    MemCopy(out->materials[tag], path.str, path.length);
    out->materials[tag][path.length] = '\0';
  }
  return true_v;
}

bool8_t vkr_surface_theme_write(const VkrSurfaceTheme *theme,
                                VkrAllocator *allocator, String8 *out_json) {
  uint64_t capacity = 64u;
  for (uint32_t tag = 0; tag < VKR_SURFACE_COUNT; ++tag) {
    capacity +=
        strlen(vkr_surface_names[tag]) + strlen(theme->materials[tag]) + 16u;
  }
  char *text =
      vkr_allocator_alloc(allocator, capacity, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!text) {
    return false_v;
  }

  uint64_t length = (uint64_t)snprintf(
      text, capacity, "{\n  \"version\": %u,\n  \"materials\": {",
      VKR_SURFACE_THEME_VERSION);
  bool8_t first = true_v;
  for (uint32_t tag = 1; tag < VKR_SURFACE_COUNT; ++tag) {
    if (!theme->materials[tag][0]) {
      continue;
    }
    length += (uint64_t)snprintf(text + length, capacity - length,
                                 "%s\n    \"%s\": \"%s\"", first ? "" : ",",
                                 vkr_surface_names[tag], theme->materials[tag]);
    first = false_v;
  }
  length += (uint64_t)snprintf(text + length, capacity - length, "%s}\n}\n",
                               first ? "" : "\n  ");
  *out_json = (String8){.str = (uint8_t *)text, .length = length};
  return true_v;
}

const char *vkr_surface_theme_material(const VkrSurfaceTheme *theme,
                                       const VkrSurfaceTheme *fallback,
                                       VkrSurface surface) {
  if ((uint32_t)surface == VKR_SURFACE_NONE ||
      (uint32_t)surface >= VKR_SURFACE_COUNT) {
    return NULL;
  }
  if (theme && theme->materials[surface][0]) {
    return theme->materials[surface];
  }
  if (fallback && fallback->materials[surface][0]) {
    return fallback->materials[surface];
  }
  return NULL;
}

const char *vkr_surface_face_material(VkrSurface surface, VkrSurfaceMark mark,
                                      const char *material,
                                      const VkrSurfaceTheme *theme,
                                      const VkrSurfaceTheme *fallback,
                                      float32_t normal_y,
                                      bool8_t greybox_view) {
  if (!greybox_view) {
    if (material && material[0]) {
      return material;
    }
    const char *bound = vkr_surface_theme_material(theme, fallback, surface);
    if (bound) {
      return bound;
    }
  }
  return vkr_surface_greybox_material(surface, mark,
                                      vkr_surface_orientation(normal_y));
}

/* The retired dev palette, as the tag and mark each material became. */
static const struct {
  const char *name;
  VkrSurface surface;
  VkrSurfaceMark mark;
} s_legacy_materials[] = {
    {"grid", VKR_SURFACE_NONE, VKR_SURFACE_MARK_NONE},
    {"floor", VKR_SURFACE_NONE, VKR_SURFACE_MARK_NONE},
    {"wall", VKR_SURFACE_NONE, VKR_SURFACE_MARK_NONE},
    {"ceiling", VKR_SURFACE_NONE, VKR_SURFACE_MARK_NONE},
    {"clip", VKR_SURFACE_NONE, VKR_SURFACE_MARK_NONE},
    {"trigger", VKR_SURFACE_NONE, VKR_SURFACE_MARK_NONE},
    {"concrete", VKR_SURFACE_CONCRETE, VKR_SURFACE_MARK_NONE},
    {"metal", VKR_SURFACE_METAL, VKR_SURFACE_MARK_NONE},
    {"wood", VKR_SURFACE_WOOD, VKR_SURFACE_MARK_NONE},
    {"tile", VKR_SURFACE_TILE, VKR_SURFACE_MARK_NONE},
    {"light", VKR_SURFACE_EMISSIVE, VKR_SURFACE_MARK_NONE},
    {"hazard", VKR_SURFACE_NONE, VKR_SURFACE_MARK_HAZARD},
    {"orange", VKR_SURFACE_NONE, VKR_SURFACE_MARK_ORANGE},
    {"blue", VKR_SURFACE_NONE, VKR_SURFACE_MARK_BLUE},
    {"red", VKR_SURFACE_NONE, VKR_SURFACE_MARK_RED},
    {"green", VKR_SURFACE_NONE, VKR_SURFACE_MARK_GREEN},
    {"dark", VKR_SURFACE_NONE, VKR_SURFACE_MARK_DARK},
};

bool8_t vkr_surface_from_legacy_material(const char *material,
                                         VkrSurface *out_surface,
                                         VkrSurfaceMark *out_mark) {
  static const char prefix[] = "assets/materials/dev/dev_";
  static const char suffix[] = ".mt";
  if (!material || strncmp(material, prefix, sizeof(prefix) - 1u) != 0) {
    return false_v;
  }
  const char *name = material + sizeof(prefix) - 1u;
  const size_t length = strlen(name);
  if (length <= sizeof(suffix) - 1u ||
      strcmp(name + length - (sizeof(suffix) - 1u), suffix) != 0) {
    return false_v;
  }
  const size_t name_length = length - (sizeof(suffix) - 1u);
  for (uint32_t i = 0; i < ArrayCount(s_legacy_materials); ++i) {
    if (strlen(s_legacy_materials[i].name) == name_length &&
        strncmp(s_legacy_materials[i].name, name, name_length) == 0) {
      *out_surface = s_legacy_materials[i].surface;
      *out_mark = s_legacy_materials[i].mark;
      return true_v;
    }
  }
  return false_v;
}
