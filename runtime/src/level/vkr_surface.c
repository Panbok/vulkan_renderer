#include "level/vkr_surface.h"

#include "core/vkr_json.h"

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

const char *const vkr_surface_orientation_names[] = {"floor", "wall", "ceiling",
                                                     NULL};

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
_Static_assert(ArrayCount(vkr_surface_orientation_names) ==
                   VKR_SURFACE_ORIENTATION_COUNT + 1u,
               "one name per orientation");
_Static_assert(VKR_SURFACE_COUNT < 255u && VKR_SURFACE_MARK_COUNT < 255u,
               "rule conditions fit a byte");

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
    if (string_equals(names[i], name)) {
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
    string_format(error, capacity, format, name);
  }
}

/* The tag whose name is `text`, or VKR_SURFACE_COUNT. */
static uint32_t theme_tag(String8 text) {
  for (uint32_t tag = 0; tag < VKR_SURFACE_COUNT; ++tag) {
    const uint64_t length = string_length(vkr_surface_names[tag]);
    if (text.length == length &&
        MemCompare(text.str, vkr_surface_names[tag], length) == 0) {
      return tag;
    }
  }
  return VKR_SURFACE_COUNT;
}

bool8_t vkr_surface_rule_tag(String8 text, char *out) {
  uint64_t start = text.length && text.str[0] == '#' ? 1u : 0u;
  const uint64_t length = text.length - start;
  if (length == 0u || length > VKR_SURFACE_THEME_TAG_CAPACITY - 2u) {
    return false_v;
  }
  out[0] = '#';
  for (uint64_t i = 0; i < length; ++i) {
    const uint8_t c = text.str[start + i];
    const bool8_t allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                            c == '_' || c == '-';
    if (!allowed) {
      return false_v;
    }
    out[1u + i] = (char)c;
  }
  out[1u + length] = '\0';
  return true_v;
}

/* The optional string member `name` of rule object `rule`, found in
   `names` (NULL-terminated); `*out` keeps `any` when the member is
   absent. False for a member that is not a known name. */
static bool8_t theme_rule_condition(VkrJsonReader rule, const char *name,
                                    const char *const *names, uint8_t any,
                                    uint8_t *out) {
  *out = any;
  if (!vkr_json_find_root_field(&rule, name)) {
    return true_v;
  }
  String8 text = {0};
  if (!vkr_json_parse_string(&rule, &text)) {
    return false_v;
  }
  for (uint32_t i = 0; names[i]; ++i) {
    if (text.length == string_length(names[i]) &&
        MemCompare(text.str, names[i], text.length) == 0) {
      *out = (uint8_t)i;
      return true_v;
    }
  }
  return false_v;
}

/* Reads one rule object; false writes why to `error`. */
static bool8_t theme_rule_read(String8 object, uint32_t index,
                               VkrSurfaceThemeRule *out, char *error,
                               uint32_t capacity) {
  MemZero(out, sizeof(*out));
  char where[24];
  string_format(where, sizeof(where), "rules[%u]", index);
  const VkrJsonReader rule = vkr_json_reader_from_string(object);

  VkrJsonReader member = rule;
  if (!vkr_json_find_root_field(&member, "tags") ||
      member.pos >= member.length || member.data[member.pos] != '[') {
    theme_error(error, capacity, "%s.tags must list 1 to 4 tags", where);
    return false_v;
  }
  member.pos++;
  for (;;) {
    vkr_json_skip_whitespace(&member);
    if (member.pos >= member.length) {
      theme_error(error, capacity, "%s.tags is not closed", where);
      return false_v;
    }
    const uint8_t next = member.data[member.pos];
    if (next == ']') {
      break;
    }
    if (next == ',') {
      member.pos++;
      continue;
    }
    String8 text = {0};
    if (out->tag_count == VKR_SURFACE_THEME_RULE_TAGS ||
        !vkr_json_parse_string(&member, &text) ||
        !vkr_surface_rule_tag(text, out->tags[out->tag_count])) {
      theme_error(error, capacity,
                  "%s.tags must list 1 to 4 tags of [a-z0-9_-]", where);
      return false_v;
    }
    out->tag_count++;
  }
  if (out->tag_count == 0u) {
    theme_error(error, capacity, "%s.tags must list 1 to 4 tags", where);
    return false_v;
  }

  if (!theme_rule_condition(rule, "surface", vkr_surface_names,
                            VKR_SURFACE_COUNT, &out->surface)) {
    theme_error(error, capacity, "%s.surface names no surface tag", where);
    return false_v;
  }
  if (!theme_rule_condition(rule, "mark", vkr_surface_mark_names,
                            VKR_SURFACE_MARK_COUNT, &out->mark)) {
    theme_error(error, capacity, "%s.mark names no mark", where);
    return false_v;
  }
  if (!theme_rule_condition(rule, "faces", vkr_surface_orientation_names,
                            VKR_SURFACE_ORIENTATION_COUNT, &out->orientation)) {
    theme_error(error, capacity, "%s.faces is floor, wall or ceiling", where);
    return false_v;
  }

  member = rule;
  String8 path = {0};
  if (!vkr_json_find_root_field(&member, "material") ||
      !vkr_json_parse_string(&member, &path) || path.length == 0u ||
      path.length >= VKR_SURFACE_THEME_PATH_CAPACITY ||
      MemFindByte(path.str, '\\', path.length)) {
    theme_error(error, capacity,
                "%s.material must be a material path under 128 bytes", where);
    return false_v;
  }
  MemCopy(out->material, path.str, path.length);
  out->material[path.length] = '\0';
  return true_v;
}

/* Reads the version 2 `rules` list at `member`. */
static bool8_t theme_rules_read(VkrJsonReader member, VkrSurfaceTheme *out,
                                char *error, uint32_t capacity) {
  String8 list = {0};
  if (member.pos >= member.length || member.data[member.pos] != '[' ||
      !vkr_json_capture_composite(&member, &list)) {
    theme_error(error, capacity, "%s must be a list of rule objects", "rules");
    return false_v;
  }
  VkrJsonReader rules = vkr_json_reader_from_string(list);
  rules.pos = 1u;
  for (;;) {
    vkr_json_skip_whitespace(&rules);
    if (rules.pos >= rules.length) {
      break;
    }
    const uint8_t next = rules.data[rules.pos];
    if (next == ']') {
      break;
    }
    if (next == ',') {
      rules.pos++;
      continue;
    }
    String8 object = {0};
    if (next != '{' || !vkr_json_capture_composite(&rules, &object)) {
      theme_error(error, capacity, "%s must be a list of rule objects",
                  "rules");
      return false_v;
    }
    if (out->rule_count == VKR_SURFACE_THEME_RULE_MAX) {
      theme_error(error, capacity, "%s holds more than 128 rules", "rules");
      return false_v;
    }
    if (!theme_rule_read(object, out->rule_count, &out->rules[out->rule_count],
                         error, capacity)) {
      return false_v;
    }
    out->rule_count++;
  }
  return true_v;
}

bool8_t vkr_surface_theme_read(String8 json, VkrSurfaceTheme *out, char *error,
                               uint32_t capacity) {
  MemZero(out, sizeof(*out));
  VkrJsonReader root = vkr_json_reader_from_string(json);
  VkrJsonReader member = root;
  float32_t version = 0.0f;
  if (!vkr_json_find_root_field(&member, "version") ||
      !vkr_json_parse_float(&member, &version) ||
      (version != (float32_t)VKR_SURFACE_THEME_VERSION &&
       version != (float32_t)VKR_SURFACE_THEME_RULES_VERSION)) {
    theme_error(error, capacity, "%s must be 1 or 2", "version");
    return false_v;
  }

  member = root;
  if (vkr_json_find_root_field(&member, "rules")) {
    if (version != (float32_t)VKR_SURFACE_THEME_RULES_VERSION) {
      theme_error(error, capacity, "%s need version 2", "rules");
      return false_v;
    }
    if (!theme_rules_read(member, out, error, capacity)) {
      MemZero(out, sizeof(*out));
      return false_v;
    }
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
        MemFindByte(path.str, '\\', path.length)) {
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
    capacity += string_length(vkr_surface_names[tag]) +
                string_length(theme->materials[tag]) + 16u;
  }
  const uint32_t rule_count =
      Min(theme->rule_count, VKR_SURFACE_THEME_RULE_MAX);
  /* A rule's line: its tags, its conditions' names and its path. */
  capacity +=
      (uint64_t)rule_count *
      (VKR_SURFACE_THEME_RULE_TAGS * (VKR_SURFACE_THEME_TAG_CAPACITY + 4u) +
       VKR_SURFACE_THEME_PATH_CAPACITY + 128u);
  char *text =
      vkr_allocator_alloc(allocator, capacity, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!text) {
    return false_v;
  }

  uint64_t length = (uint64_t)string_format(
      text, capacity, "{\n  \"version\": %u,\n  \"materials\": {",
      rule_count ? VKR_SURFACE_THEME_RULES_VERSION : VKR_SURFACE_THEME_VERSION);
  bool8_t first = true_v;
  for (uint32_t tag = 1; tag < VKR_SURFACE_COUNT; ++tag) {
    if (!theme->materials[tag][0]) {
      continue;
    }
    length += (uint64_t)string_format(
        text + length, capacity - length, "%s\n    \"%s\": \"%s\"",
        first ? "" : ",", vkr_surface_names[tag], theme->materials[tag]);
    first = false_v;
  }
  length += (uint64_t)string_format(text + length, capacity - length, "%s}",
                                    first ? "" : "\n  ");

  /* One rule a line, in their order, which settles ties. */
  if (rule_count) {
    length += (uint64_t)string_format(text + length, capacity - length,
                                      ",\n  \"rules\": [");
  }
  for (uint32_t i = 0; i < rule_count; ++i) {
    const VkrSurfaceThemeRule *rule = &theme->rules[i];
    length += (uint64_t)string_format(text + length, capacity - length,
                                      "%s\n    {\"tags\": [", i ? "," : "");
    for (uint32_t t = 0; t < rule->tag_count; ++t) {
      length +=
          (uint64_t)string_format(text + length, capacity - length, "%s\"%s\"",
                                  t ? ", " : "", rule->tags[t]);
    }
    length += (uint64_t)string_format(text + length, capacity - length, "]");
    if (rule->surface < VKR_SURFACE_COUNT) {
      length += (uint64_t)string_format(text + length, capacity - length,
                                        ", \"surface\": \"%s\"",
                                        vkr_surface_names[rule->surface]);
    }
    if (rule->mark < VKR_SURFACE_MARK_COUNT) {
      length += (uint64_t)string_format(text + length, capacity - length,
                                        ", \"mark\": \"%s\"",
                                        vkr_surface_mark_names[rule->mark]);
    }
    if (rule->orientation < VKR_SURFACE_ORIENTATION_COUNT) {
      length += (uint64_t)string_format(
          text + length, capacity - length, ", \"faces\": \"%s\"",
          vkr_surface_orientation_names[rule->orientation]);
    }
    length +=
        (uint64_t)string_format(text + length, capacity - length,
                                ", \"material\": \"%s\"}", rule->material);
  }
  if (rule_count) {
    length +=
        (uint64_t)string_format(text + length, capacity - length, "\n  ]");
  }
  length += (uint64_t)string_format(text + length, capacity - length, "\n}\n");
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

/* Whether the canonical tag text `tags` holds `tag` as a whole word. */
static bool8_t theme_tags_hold(String8 tags, const char *tag) {
  const uint64_t length = string_length(tag);
  uint64_t at = 0u;
  while (at < tags.length) {
    while (at < tags.length && tags.str[at] == ' ') {
      at++;
    }
    uint64_t end = at;
    while (end < tags.length && tags.str[end] != ' ') {
      end++;
    }
    if (end - at == length && MemCompare(tags.str + at, tag, length) == 0) {
      return true_v;
    }
    at = end;
  }
  return false_v;
}

void vkr_surface_theme_match_tags(const VkrSurfaceTheme *theme, String8 tags,
                                  VkrSurfaceRuleSet *out) {
  MemZero(out, sizeof(*out));
  if (!theme || !tags.length) {
    return;
  }
  const uint32_t count = Min(theme->rule_count, VKR_SURFACE_THEME_RULE_MAX);
  for (uint32_t i = 0; i < count; ++i) {
    const VkrSurfaceThemeRule *rule = &theme->rules[i];
    bool8_t holds = rule->tag_count > 0u;
    for (uint32_t t = 0; holds && t < rule->tag_count; ++t) {
      holds = theme_tags_hold(tags, rule->tags[t]);
    }
    if (holds) {
      out->bits[i / 64u] |= UINT64_C(1) << (i % 64u);
    }
  }
}

const VkrSurfaceThemeRule *
vkr_surface_theme_rule(const VkrSurfaceTheme *theme,
                       const VkrSurfaceRuleSet *matched, VkrSurface surface,
                       VkrSurfaceMark mark, VkrSurfaceOrientation orientation) {
  if (!theme || !matched) {
    return NULL;
  }
  const VkrSurfaceThemeRule *best = NULL;
  uint32_t best_score = 0u;
  const uint32_t count = Min(theme->rule_count, VKR_SURFACE_THEME_RULE_MAX);
  for (uint32_t i = 0; i < count; ++i) {
    if (!(matched->bits[i / 64u] & (UINT64_C(1) << (i % 64u)))) {
      continue;
    }
    const VkrSurfaceThemeRule *rule = &theme->rules[i];
    if ((rule->surface < VKR_SURFACE_COUNT && rule->surface != surface) ||
        (rule->mark < VKR_SURFACE_MARK_COUNT && rule->mark != mark) ||
        (rule->orientation < VKR_SURFACE_ORIENTATION_COUNT &&
         rule->orientation != orientation)) {
      continue;
    }
    const uint32_t score =
        rule->tag_count + (rule->surface < VKR_SURFACE_COUNT ? 1u : 0u) +
        (rule->mark < VKR_SURFACE_MARK_COUNT ? 1u : 0u) +
        (rule->orientation < VKR_SURFACE_ORIENTATION_COUNT ? 1u : 0u);
    /* Rules run in order, so a tie keeps the earlier one. */
    if (score > best_score) {
      best = rule;
      best_score = score;
    }
  }
  return best;
}

const char *vkr_surface_face_material(VkrSurface surface, VkrSurfaceMark mark,
                                      const char *material,
                                      const VkrSurfaceTheme *theme,
                                      const VkrSurfaceTheme *fallback,
                                      const VkrSurfaceRuleSet *matched,
                                      float32_t normal_y, bool8_t greybox_view,
                                      bool8_t *out_greybox) {
  const VkrSurfaceOrientation orientation = vkr_surface_orientation(normal_y);
  if (out_greybox) {
    *out_greybox = false_v;
  }
  if (!greybox_view) {
    if (material && material[0]) {
      return material;
    }
    if (matched) {
      const VkrSurfaceThemeRule *rule = vkr_surface_theme_rule(
          theme, &matched[0], surface, mark, orientation);
      if (!rule) {
        rule = vkr_surface_theme_rule(fallback, &matched[1], surface, mark,
                                      orientation);
      }
      if (rule) {
        return rule->material;
      }
    }
    const char *bound = vkr_surface_theme_material(theme, fallback, surface);
    if (bound) {
      return bound;
    }
  }
  if (out_greybox) {
    *out_greybox = true_v;
  }
  return vkr_surface_greybox_material(surface, mark, orientation);
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
  if (!material || !string_n_equals(material, prefix, sizeof(prefix) - 1u)) {
    return false_v;
  }
  const char *name = material + sizeof(prefix) - 1u;
  const size_t length = string_length(name);
  if (length <= sizeof(suffix) - 1u ||
      !string_equals(name + length - (sizeof(suffix) - 1u), suffix)) {
    return false_v;
  }
  const size_t name_length = length - (sizeof(suffix) - 1u);
  for (uint32_t i = 0; i < ArrayCount(s_legacy_materials); ++i) {
    if (string_length(s_legacy_materials[i].name) == name_length &&
        string_n_equals(s_legacy_materials[i].name, name, name_length)) {
      *out_surface = s_legacy_materials[i].surface;
      *out_mark = s_legacy_materials[i].mark;
      return true_v;
    }
  }
  return false_v;
}
