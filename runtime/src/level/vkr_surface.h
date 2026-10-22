#pragma once

#include "containers/str.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

/* Surface tags and greybox looks (docs/proposals/artist-toolkit.md, part 1).
 * A surface tag names what a brush face is made of. Until the art pass binds
 * a material, the face shows its tag's greybox look: an engine material per
 * tag and face orientation, or per mark, that designers cannot change. Every
 * greybox look projects the same metric grid in world space, one repeat
 * per VKR_SURFACE_GREYBOX_REPEAT meters. The art pass binds tags to
 * materials in a theme (part 6). The module owns no state; the runtime,
 * Bakery's brush proxies and the lightmap baker resolve looks through it
 * alike. */

typedef enum VkrSurface {
  VKR_SURFACE_NONE = 0,
  VKR_SURFACE_CONCRETE,
  VKR_SURFACE_METAL,
  VKR_SURFACE_WOOD,
  VKR_SURFACE_TILE,
  VKR_SURFACE_PLASTER,
  VKR_SURFACE_BRICK,
  VKR_SURFACE_ROCK,
  VKR_SURFACE_DIRT,
  VKR_SURFACE_GRASS,
  VKR_SURFACE_GLASS,
  VKR_SURFACE_FABRIC,
  VKR_SURFACE_WATER,
  VKR_SURFACE_EMISSIVE,
  VKR_SURFACE_COUNT,
} VkrSurface;

/* A level-design accent that replaces the tag's tone in the greybox look:
   hazard stripes, or a colour for wayfinding. The art pass ignores marks. */
typedef enum VkrSurfaceMark {
  VKR_SURFACE_MARK_NONE = 0,
  VKR_SURFACE_MARK_HAZARD,
  VKR_SURFACE_MARK_ORANGE,
  VKR_SURFACE_MARK_BLUE,
  VKR_SURFACE_MARK_RED,
  VKR_SURFACE_MARK_GREEN,
  VKR_SURFACE_MARK_DARK,
  VKR_SURFACE_MARK_COUNT,
} VkrSurfaceMark;

/* Which way a face looks, from its world normal: the greybox look shades
   floors, walls and ceilings differently so they read apart unlit. */
typedef enum VkrSurfaceOrientation {
  VKR_SURFACE_FLOOR = 0,
  VKR_SURFACE_WALL,
  VKR_SURFACE_CEILING,
  VKR_SURFACE_ORIENTATION_COUNT,
} VkrSurfaceOrientation;

/* Meters one repeat of the greybox grid covers: 25 cm lines, 1 m lines and
   a 4 m border. */
#define VKR_SURFACE_GREYBOX_REPEAT 4.0f

/* Fixed looks of the clip and trigger brush roles. */
#define VKR_SURFACE_CLIP_MATERIAL "assets/materials/greybox/role_clip.mt"
#define VKR_SURFACE_TRIGGER_MATERIAL "assets/materials/greybox/role_trigger.mt"

/* A theme (`.surfaces`, JSON) binds surface tags to materials for the art
   pass: `{"version": 1, "materials": {"<tag>": "<.mt path>", ...}}`, paths
   content-root relative. An empty entry leaves the tag unbound. Version 2
   adds `"rules"`: a list of `{"tags": ["#lab", "#wall"], "surface": "<tag>",
   "mark": "<mark>", "faces": "floor"|"wall"|"ceiling", "material": "<.mt>"}`
   that bind faces by their brush's design tags (ADR-084), so one theme
   gives a lab wall and a station wall of the same surface tag different
   materials. A rule lists 1 to VKR_SURFACE_THEME_RULE_TAGS tags; `surface`,
   `mark` and `faces` are optional conditions. A theme without rules writes
   version 1. */
#define VKR_SURFACE_THEME_VERSION 1u
#define VKR_SURFACE_THEME_RULES_VERSION 2u
#define VKR_SURFACE_THEME_PATH_CAPACITY 128u
#define VKR_SURFACE_THEME_EXTENSION ".surfaces"
#define VKR_SURFACE_THEME_RULE_MAX 128u
#define VKR_SURFACE_THEME_RULE_TAGS 4u
/* A canonical design tag: '#', up to 32 characters and the terminator. */
#define VKR_SURFACE_THEME_TAG_CAPACITY 34u
#define VKR_SURFACE_THEME_RULE_WORDS ((VKR_SURFACE_THEME_RULE_MAX + 63u) / 64u)

/* One rule. `surface`, `mark` and `orientation` hold VKR_SURFACE_COUNT,
   VKR_SURFACE_MARK_COUNT and VKR_SURFACE_ORIENTATION_COUNT when the rule
   does not test them. */
typedef struct VkrSurfaceThemeRule {
  char tags[VKR_SURFACE_THEME_RULE_TAGS][VKR_SURFACE_THEME_TAG_CAPACITY];
  uint8_t tag_count;
  uint8_t surface;
  uint8_t mark;
  uint8_t orientation;
  char material[VKR_SURFACE_THEME_PATH_CAPACITY];
} VkrSurfaceThemeRule;

typedef struct VkrSurfaceTheme {
  char materials[VKR_SURFACE_COUNT][VKR_SURFACE_THEME_PATH_CAPACITY];
  VkrSurfaceThemeRule rules[VKR_SURFACE_THEME_RULE_MAX];
  uint32_t rule_count;
} VkrSurfaceTheme;

/* The rules of one theme whose tags a brush carries, one bit per rule;
   vkr_surface_theme_match_tags fills it once per brush, so faces test only
   their surface, mark and orientation. */
typedef struct VkrSurfaceRuleSet {
  uint64_t bits[VKR_SURFACE_THEME_RULE_WORDS];
} VkrSurfaceRuleSet;

/* Tag and mark names as documents store them, NULL-terminated and indexed
   by VkrSurface and VkrSurfaceMark, with display labels parallel. */
extern const char *const vkr_surface_names[];
extern const char *const vkr_surface_labels[];
extern const char *const vkr_surface_mark_names[];
extern const char *const vkr_surface_mark_labels[];
/* Orientation names as theme rules store them (`faces`), indexed by
   VkrSurfaceOrientation, NULL-terminated. */
extern const char *const vkr_surface_orientation_names[];

/* The tag or mark named `name`; false for an unknown name. */
bool8_t vkr_surface_find(const char *name, VkrSurface *out);
bool8_t vkr_surface_mark_find(const char *name, VkrSurfaceMark *out);

/* The orientation of a face whose unit world normal has `normal_y`. */
VkrSurfaceOrientation vkr_surface_orientation(float32_t normal_y);

/* The greybox material of a tag and mark seen from `orientation`: the
   mark's look when there is a mark, else the tag's. Out-of-range values
   take the untagged look. */
const char *vkr_surface_greybox_material(VkrSurface surface,
                                         VkrSurfaceMark mark,
                                         VkrSurfaceOrientation orientation);

/* Reads a theme document; false names the member at fault in `error`. A
   key that names no tag fails, so a typo does not unbind a tag silently. */
bool8_t vkr_surface_theme_read(String8 json, VkrSurfaceTheme *out, char *error,
                               uint32_t capacity);
/* A rule's canonical tag from `text` into `out`
   (VKR_SURFACE_THEME_TAG_CAPACITY bytes): '#', added when missing, and 1
   to 32 of [a-z0-9_-], as SceneTags stores tags. False for other text. */
bool8_t vkr_surface_rule_tag(String8 text, char *out);
/* Writes `theme` with one bound tag, or one rule, a line into
   `allocator`. */
bool8_t vkr_surface_theme_write(const VkrSurfaceTheme *theme,
                                VkrAllocator *allocator, String8 *out_json);

/* The material `theme` binds to `surface`, else the one `fallback` binds
   (a container's theme over the World's); NULL when neither binds it or
   the face is untagged. Either theme may be NULL. */
const char *vkr_surface_theme_material(const VkrSurfaceTheme *theme,
                                       const VkrSurfaceTheme *fallback,
                                       VkrSurface surface);

/* Sets in `out` the rules of `theme` (NULL: none) whose every tag the
   canonical tag text `tags` holds ("#lab #wall", as SceneTags stores it). */
void vkr_surface_theme_match_tags(const VkrSurfaceTheme *theme, String8 tags,
                                  VkrSurfaceRuleSet *out);

/* The most specific rule of `theme` among `matched` whose surface, mark and
   orientation conditions hold: the one with the most conditions, tags
   counted one each, the earlier on a tie. NULL when none holds or either
   pointer is NULL. */
const VkrSurfaceThemeRule *
vkr_surface_theme_rule(const VkrSurfaceTheme *theme,
                       const VkrSurfaceRuleSet *matched, VkrSurface surface,
                       VkrSurfaceMark mark, VkrSurfaceOrientation orientation);

/* The material a face draws, in this order: with `greybox_view` on, its
   greybox look; its art-owned `material`; the best rule of `theme`, then
   of `fallback`, for its brush's tags (`matched` holds the two themes'
   rule sets, NULL for a brush without tags); its surface tag's binding in
   `theme`, then `fallback`; its greybox look. `out_greybox` (may be NULL)
   is true when the answer is a greybox look, which projects the greybox
   grid instead of the face's UVs. */
const char *vkr_surface_face_material(VkrSurface surface, VkrSurfaceMark mark,
                                      const char *material,
                                      const VkrSurfaceTheme *theme,
                                      const VkrSurfaceTheme *fallback,
                                      const VkrSurfaceRuleSet *matched,
                                      float32_t normal_y, bool8_t greybox_view,
                                      bool8_t *out_greybox);

/* Upgrades a material path that older documents stored for a retired dev
   palette material (assets/materials/dev/dev_<name>.mt) to the tag and mark
   that replace it. False when `material` names another material; the
   caller then keeps it. */
bool8_t vkr_surface_from_legacy_material(const char *material,
                                         VkrSurface *out_surface,
                                         VkrSurfaceMark *out_mark);
