#pragma once

#include "defines.h"
#include "math/vec.h"
#include "memory/vkr_allocator.h"

/* Heightfield terrain data (ADR-084): a square grid of 16-bit heights and
 * four 8-bit layer weights per sample. A terrain of `cells` cells per side
 * has `cells + 1` samples per side, `spacing` metres apart, centred on its
 * entity: sample (0, 0) lies at local (-size / 2, -size / 2). A height of 0
 * is `height_min` and 65535 is `height_max`.
 *
 * Samples live in tiles of VKR_HEIGHTFIELD_TILE_SAMPLES squared, in memory as
 * in the file, which stores them row-major over tiles at fixed offsets so a
 * reader can take one tile without the rest. A tile is resident or not:
 * terrains up to VKR_HEIGHTFIELD_RESIDENT_CELLS keep every tile, larger ones
 * stream tiles (docs/proposals/world-partition.md). An overview of every
 * VKR_HEIGHTFIELD_OVERVIEW_STRIDE-th sample is always resident and follows
 * every write. The module owns no global state; region operations change
 * resident samples in place and return the rectangle they touched. */

#define VKR_HEIGHTFIELD_LAYERS 4u
/* Cells of one terrain tile per side: meshes, LOD and file chunks follow
   it. */
#define VKR_HEIGHTFIELD_TILE_CELLS 64u
/* Samples of one tile per side, in memory and in the file. */
#define VKR_HEIGHTFIELD_TILE_SAMPLES 64u
/* 8 km at 1 m spacing (docs/proposals/world-partition.md). */
#define VKR_HEIGHTFIELD_CELLS_MAX 8192u
/* Terrains up to this many cells a side (1 km at 1 m) keep every tile. */
#define VKR_HEIGHTFIELD_RESIDENT_CELLS 1024u
/* Samples between overview samples. */
#define VKR_HEIGHTFIELD_OVERVIEW_STRIDE 16u
/* Larger terrains stream; their size is a multiple of this many cells, one
   64-sample tile of the overview. */
#define VKR_HEIGHTFIELD_STREAMED_CELLS                                         \
  (VKR_HEIGHTFIELD_TILE_CELLS * VKR_HEIGHTFIELD_OVERVIEW_STRIDE)

/* One tile's samples, X fastest then Z; samples past the field's edge in
   its last tiles are unused. Weights hold layer 0 in the low byte. */
typedef struct VkrHeightfieldTile {
  uint16_t heights[VKR_HEIGHTFIELD_TILE_SAMPLES * VKR_HEIGHTFIELD_TILE_SAMPLES];
  uint32_t weights[VKR_HEIGHTFIELD_TILE_SAMPLES * VKR_HEIGHTFIELD_TILE_SAMPLES];
} VkrHeightfieldTile;

typedef struct VkrHeightfield {
  uint32_t cells;
  float32_t spacing;
  float32_t height_min;
  float32_t height_max;
  uint32_t tiles_per_side;
  /* tiles_per_side^2 tiles, NULL where not resident. */
  VkrHeightfieldTile **tiles;
  /* Per tile: changed since it was read or written. */
  uint8_t *tile_dirty;
  uint32_t resident_tiles;
  /* (cells / VKR_HEIGHTFIELD_OVERVIEW_STRIDE + 1)^2 samples. */
  uint16_t *overview_heights;
  uint32_t *overview_weights;
  /* Owns the tiles and arrays. */
  VkrAllocator *allocator;
  /* The open file of a streamed field (FILE *), NULL otherwise. */
  void *file;
} VkrHeightfield;

/* An inclusive rectangle of samples. */
typedef struct VkrHeightfieldRect {
  uint32_t x0;
  uint32_t z0;
  uint32_t x1;
  uint32_t z1;
} VkrHeightfieldRect;

/* A resident heightfield of `cells` (a multiple of
   VKR_HEIGHTFIELD_TILE_CELLS, at most VKR_HEIGHTFIELD_RESIDENT_CELLS) cells
   per side at `height`, fully on layer 0. */
bool8_t vkr_heightfield_create(VkrHeightfield *out, uint32_t cells,
                               float32_t spacing, float32_t height_min,
                               float32_t height_max, float32_t height,
                               VkrAllocator *allocator);
/* Writes a flat heightfield file of up to VKR_HEIGHTFIELD_CELLS_MAX cells a
   side tile by tile, without holding it. */
bool8_t vkr_heightfield_create_file(const char *path, uint32_t cells,
                                    float32_t spacing, float32_t height_min,
                                    float32_t height_max, float32_t height,
                                    char *error, uint32_t capacity);
/* Releases every tile and array and closes a streamed field's file;
   `allocator` must be the field's. */
void vkr_heightfield_destroy(VkrHeightfield *field, VkrAllocator *allocator);

static inline uint32_t vkr_heightfield_samples(const VkrHeightfield *field) {
  return field->cells + 1u;
}

static inline uint32_t
vkr_heightfield_overview_samples(const VkrHeightfield *field) {
  return field->cells / VKR_HEIGHTFIELD_OVERVIEW_STRIDE + 1u;
}

/* Whether the field streams its tiles. */
static inline bool8_t vkr_heightfield_streamed(const VkrHeightfield *field) {
  return field->file != NULL;
}

static inline VkrHeightfieldTile *
vkr_heightfield_tile(const VkrHeightfield *field, uint32_t tx, uint32_t tz) {
  return field->tiles[tz * field->tiles_per_side + tx];
}

/* The tile holding sample (x, z), or NULL when it is not resident. */
static inline VkrHeightfieldTile *
vkr_heightfield_tile_of(const VkrHeightfield *field, uint32_t x, uint32_t z) {
  return vkr_heightfield_tile(field, x / VKR_HEIGHTFIELD_TILE_SAMPLES,
                              z / VKR_HEIGHTFIELD_TILE_SAMPLES);
}

/* Index of sample (x, z) inside its tile. */
static inline uint32_t vkr_heightfield_tile_index(uint32_t x, uint32_t z) {
  return (z % VKR_HEIGHTFIELD_TILE_SAMPLES) * VKR_HEIGHTFIELD_TILE_SAMPLES +
         x % VKR_HEIGHTFIELD_TILE_SAMPLES;
}

/* Raw height and weights of resident sample (x, z). */
static inline uint16_t vkr_heightfield_raw(const VkrHeightfield *field,
                                           uint32_t x, uint32_t z) {
  return vkr_heightfield_tile_of(field, x, z)
      ->heights[vkr_heightfield_tile_index(x, z)];
}
static inline uint32_t vkr_heightfield_weights_at(const VkrHeightfield *field,
                                                  uint32_t x, uint32_t z) {
  return vkr_heightfield_tile_of(field, x, z)
      ->weights[vkr_heightfield_tile_index(x, z)];
}

/* Half the side length in metres. */
static inline float32_t vkr_heightfield_half_size(const VkrHeightfield *field) {
  return 0.5f * field->spacing * (float32_t)field->cells;
}

float32_t vkr_heightfield_metres(const VkrHeightfield *field, uint16_t value);
uint16_t vkr_heightfield_quantize(const VkrHeightfield *field,
                                  float32_t metres);
/* Height in metres of resident sample (x, z). */
float32_t vkr_heightfield_at(const VkrHeightfield *field, uint32_t x,
                             uint32_t z);
/* Height in metres of overview sample (x, z), in overview samples. */
float32_t vkr_heightfield_overview_at(const VkrHeightfield *field, uint32_t x,
                                      uint32_t z);
/* Bilinear height in metres at local (x, z) metres from the centre, from
   resident samples or else the overview; false outside the terrain. */
bool8_t vkr_heightfield_sample(const VkrHeightfield *field, float32_t x,
                               float32_t z, float32_t *out_height);
/* Whether every tile `rect` reaches is resident. */
bool8_t vkr_heightfield_resident(const VkrHeightfield *field,
                                 VkrHeightfieldRect rect);

/* The samples within `radius` metres of local (x, z), clipped; false when
   none lie inside. */
bool8_t vkr_heightfield_rect_around(const VkrHeightfield *field, float32_t x,
                                    float32_t z, float32_t radius,
                                    VkrHeightfieldRect *out);
bool8_t vkr_heightfield_rect_union(const VkrHeightfield *field,
                                   VkrHeightfieldRect a, VkrHeightfieldRect b,
                                   VkrHeightfieldRect *out);
static inline uint32_t vkr_heightfield_rect_count(VkrHeightfieldRect rect) {
  return (rect.x1 - rect.x0 + 1u) * (rect.z1 - rect.z0 + 1u);
}

/* Copies the samples of `rect`, which must be resident, out of or into the
   field, rows of the rectangle packed. Writes mark tiles dirty and refresh
   the overview. */
void vkr_heightfield_read_rect(const VkrHeightfield *field,
                               VkrHeightfieldRect rect, uint16_t *heights,
                               uint32_t *weights);
void vkr_heightfield_write_rect(VkrHeightfield *field, VkrHeightfieldRect rect,
                                const uint16_t *heights,
                                const uint32_t *weights);

typedef enum VkrHeightfieldBrush {
  VKR_HEIGHTFIELD_RAISE = 0,
  VKR_HEIGHTFIELD_LOWER,
  VKR_HEIGHTFIELD_SMOOTH,
  /* Moves heights toward `height`. */
  VKR_HEIGHTFIELD_FLATTEN,
  /* Adds weight to `layer`, taking it from the others. */
  VKR_HEIGHTFIELD_PAINT,
  VKR_HEIGHTFIELD_BRUSH_COUNT,
} VkrHeightfieldBrush;

/* One round brush stroke step at local (x, z): `strength` is metres (raise,
   lower) or a 0..1 fraction (smooth, flatten, paint) at the centre, falling
   off smoothly to zero at `radius`. Smoothing stages the touched heights in
   `scratch`. */
bool8_t vkr_heightfield_brush(VkrHeightfield *field, VkrHeightfieldBrush brush,
                              float32_t x, float32_t z, float32_t radius,
                              float32_t strength, float32_t height,
                              uint32_t layer, VkrAllocator *scratch,
                              VkrHeightfieldRect *touched);

/* Sets the rectangle of local [min, max] (x, z) to `height`, blending back to
   the existing ground over `falloff` metres outside it. */
bool8_t vkr_heightfield_flatten(VkrHeightfield *field, Vec2 min, Vec2 max,
                                float32_t height, float32_t falloff,
                                VkrHeightfieldRect *touched);

/* A straight slope from local point `a` to `b` (x, height, z), `width` metres
   wide, blending into the ground over `falloff` metres at its sides. */
bool8_t vkr_heightfield_ramp(VkrHeightfield *field, Vec3 a, Vec3 b,
                             float32_t width, float32_t falloff,
                             VkrHeightfieldRect *touched);

typedef enum VkrHeightfieldOpKind {
  VKR_HEIGHTFIELD_OP_BRUSH = 0,
  VKR_HEIGHTFIELD_OP_FLATTEN,
  VKR_HEIGHTFIELD_OP_RAMP,
  /* An image of heights over [`min`, `max`]: each sample takes `height` plus
     `strength` times the image value, or with `add` gains it. */
  VKR_HEIGHTFIELD_OP_STAMP,
  /* A road: a ramp `width` wide from each of `path_count` points, offset by
     `a`, to the next, blending over `falloff`. */
  VKR_HEIGHTFIELD_OP_ROAD,
  VKR_HEIGHTFIELD_OP_KIND_COUNT,
} VkrHeightfieldOpKind;

/* One edit in the field's local space (metres from its centre, heights
   relative to its origin): a brush step at `a` (x, z) of `radius`, a
   flattened rectangle [`min`, `max`] at `height`, or a ramp from `a` to `b`
   `width` wide. */
typedef struct VkrHeightfieldOp {
  VkrHeightfieldOpKind kind;
  VkrHeightfieldBrush brush;
  uint32_t layer;
  Vec3 a;
  Vec3 b;
  Vec2 min;
  Vec2 max;
  float32_t radius;
  float32_t strength;
  float32_t height;
  float32_t width;
  float32_t falloff;
  /* Stamps: `image_width` x `image_height` values in [0, 1], row by row from
     `min`, borrowed for the call. */
  const float32_t *image;
  uint32_t image_width;
  uint32_t image_height;
  bool8_t add;
  /* Roads: points of the centreline, borrowed for the call. */
  const Vec3 *path;
  uint32_t path_count;
} VkrHeightfieldOp;

/* The samples `op` may change, before it runs; false when none. */
bool8_t vkr_heightfield_op_rect(const VkrHeightfield *field,
                                const VkrHeightfieldOp *op,
                                VkrHeightfieldRect *out);
/* Runs `op`, first loading the tiles it reads; `touched` is the rectangle
   vkr_heightfield_op_rect gives. Marks tiles dirty and refreshes the
   overview. */
bool8_t vkr_heightfield_op_apply(VkrHeightfield *field,
                                 const VkrHeightfieldOp *op,
                                 VkrAllocator *scratch,
                                 VkrHeightfieldRect *touched);

/* Writes a resident field to `path` atomically; a streamed field writes its
   dirty tiles and overview in place. False with the reason in `error`. */
bool8_t vkr_heightfield_save(VkrHeightfield *field, const char *path,
                             char *error, uint32_t capacity);
/* Opens a field written by vkr_heightfield_save or _create_file: every tile
   of a field up to VKR_HEIGHTFIELD_RESIDENT_CELLS, or else only the
   overview, keeping the file open to stream tiles. */
bool8_t vkr_heightfield_load(VkrHeightfield *out, const char *path,
                             VkrAllocator *allocator, char *error,
                             uint32_t capacity);
/* Reads tile (tx, tz) of a streamed field; true when already resident. */
bool8_t vkr_heightfield_tile_load(VkrHeightfield *field, uint32_t tx,
                                  uint32_t tz);
/* Releases clean tile (tx, tz) of a streamed field. False, keeping the
   tile, when it holds unsaved samples or the field is not streamed. */
bool8_t vkr_heightfield_tile_release(VkrHeightfield *field, uint32_t tx,
                                     uint32_t tz);
/* Loads every tile `rect` reaches; false when one cannot be read. */
bool8_t vkr_heightfield_load_rect(VkrHeightfield *field,
                                  VkrHeightfieldRect rect);
