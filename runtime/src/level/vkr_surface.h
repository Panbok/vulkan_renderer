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
   content-root relative. An empty entry leaves the tag unbound. */
#define VKR_SURFACE_THEME_VERSION 1u
#define VKR_SURFACE_THEME_PATH_CAPACITY 128u
#define VKR_SURFACE_THEME_EXTENSION ".surfaces"

typedef struct VkrSurfaceTheme {
  char materials[VKR_SURFACE_COUNT][VKR_SURFACE_THEME_PATH_CAPACITY];
} VkrSurfaceTheme;

/* Tag and mark names as documents store them, NULL-terminated and indexed
   by VkrSurface and VkrSurfaceMark, with display labels parallel. */
extern const char *const vkr_surface_names[];
extern const char *const vkr_surface_labels[];
extern const char *const vkr_surface_mark_names[];
extern const char *const vkr_surface_mark_labels[];

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
/* Writes `theme` with one bound tag a line, into `allocator`. */
bool8_t vkr_surface_theme_write(const VkrSurfaceTheme *theme,
                                VkrAllocator *allocator, String8 *out_json);

/* The material `theme` binds to `surface`, else the one `fallback` binds
   (a container's theme over the World's); NULL when neither binds it or
   the face is untagged. Either theme may be NULL. */
const char *vkr_surface_theme_material(const VkrSurfaceTheme *theme,
                                       const VkrSurfaceTheme *fallback,
                                       VkrSurface surface);

/* The material a face draws: with `greybox_view` off, its art-owned
   `material`, else its tag's theme binding; with the view on or nothing
   bound, its greybox look. */
const char *vkr_surface_face_material(VkrSurface surface, VkrSurfaceMark mark,
                                      const char *material,
                                      const VkrSurfaceTheme *theme,
                                      const VkrSurfaceTheme *fallback,
                                      float32_t normal_y, bool8_t greybox_view);

/* Upgrades a material path that older documents stored for a retired dev
   palette material (assets/materials/dev/dev_<name>.mt) to the tag and mark
   that replace it. False when `material` names another material; the
   caller then keeps it. */
bool8_t vkr_surface_from_legacy_material(const char *material,
                                         VkrSurface *out_surface,
                                         VkrSurfaceMark *out_mark);
