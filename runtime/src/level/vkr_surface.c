#include "level/vkr_surface.h"

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

const char *vkr_surface_face_material(VkrSurface surface, VkrSurfaceMark mark,
                                      const char *material, float32_t normal_y,
                                      bool8_t greybox_view) {
  if (!greybox_view && material && material[0]) {
    return material;
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
