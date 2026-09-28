#pragma once

// Bump when a cutout bake changes filtering or fixed encoding settings. Both
// material variant paths and packed metadata use this identity.
#define VKR_VKT_CUTOUT_POLICY_VERSION 1u
#define VKR_VKT_NORMAL_ROUGHNESS_POLICY_VERSION 2u

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// An image an in-process pack reads: a file (`path`), or RGBA8 pixels the
// caller converted, rows top first as an image decodes, which `hash` names in
// place of a file's bytes (the caller's FNV-1a over the pixels). A converted
// source may leave `pixels` null: the pack then only confirms that its
// outputs are current and returns VKR_VKT_PACK_STALE when they are not.
// Borrowed until return.
typedef struct VkrVktSource {
  const char *path;
  const unsigned char *pixels;
  unsigned int width;
  unsigned int height;
  unsigned long long hash;
} VkrVktSource;

typedef enum VkrVktPackResult {
  VKR_VKT_PACK_FAILED = 0,
  VKR_VKT_PACK_SUCCESS = 1,
  // A pair's sources differ in extent.
  VKR_VKT_PACK_INCOMPATIBLE = 2,
  // A converted source without pixels whose outputs are missing or stale.
  VKR_VKT_PACK_STALE = 3
} VkrVktPackResult;

// Synchronously packs one 2D texture of class `texture_class` ("color-srgb",
// "color-linear", "normal-rg" or "data-mask"), skipping a current output.
// Publication is atomic.
VkrVktPackResult vkr_vkt_pack_image(const VkrVktSource *source,
                                    const char *texture_class,
                                    const char *output);

// Synchronously cook one sRGB cutout texture; cutoff/factor are finite
// material values in [0, 1]. Scratch images are owned and released by the
// cooker. A verified cache hit succeeds. Publication is atomic.
VkrVktPackResult vkr_vkt_pack_cutout(const VkrVktSource *source,
                                     const char *output, float cutoff,
                                     float factor);

// Cook matched UV-space normal/MR images, folding normal strength and roughness
// factor into the pair. A null roughness source synthesizes AO/metallic = 1.
// Inputs are borrowed until return. Differing source extents are incompatible.
// Each file publishes atomically; callers publish material references only
// after both succeed. Recipe-addressed files may survive a later failure.
VkrVktPackResult vkr_vkt_pack_normal_roughness(
    const char *normal_source, const VkrVktSource *roughness_source,
    const char *normal_output, const char *roughness_output, float normal_scale,
    float roughness_factor);

// Largest mip extent a preview-tier texture stores (the spec's mip floor).
#define VKR_VKT_PREVIEW_MAX_EXTENT 1024u

// Process setting for the in-process packs above: nonzero selects the
// "preview" tier (UASTC fastest, levels up to VKR_VKT_PREVIEW_MAX_EXTENT).
// Settings identities record the level, and callers name preview outputs apart
// from final ones, so neither ever substitutes for the other.
void vkr_vkt_set_preview_tier(int preview);
int vkr_vkt_preview_tier(void);

// Block encoding of packed textures. Identities record it.
typedef enum VkrVktEncoding {
  // Transcodable UASTC, for every host.
  VKR_VKT_ENCODING_UASTC = 0,
  // Native ASTC 4x4 from astcenc at its "fastest" preset.
  VKR_VKT_ENCODING_ASTC = 1,
  // Native ASTC 4x4 from Apple's system encoder: several times faster than
  // astcenc and a few dB below it, for textures only the editor shows.
  // Available on Apple platforms only.
  VKR_VKT_ENCODING_ASTC_FAST = 2
} VkrVktEncoding;

// Parses "uastc", "astc" or "astc-fast". Returns 0 for any other name, and
// for "astc-fast" where the system encoder is unavailable.
int vkr_vkt_parse_encoding(const char *name, VkrVktEncoding *out);

// Process setting for the in-process packs above (UASTC by default).
void vkr_vkt_set_encoding(VkrVktEncoding encoding);
VkrVktEncoding vkr_vkt_encoding(void);

// File-name suffix that keeps in-process pack outputs of each setting apart:
// "", ".preview", ".astc", ".astc.preview", ".astc-fast" or
// ".astc-fast.preview".
const char *vkr_vkt_variant_suffix(void);

int vkr_vkt_packer_main(int argc, char **argv);

// Encodes tightly packed RGBA8 pixels (at most 65535 on a side) as a lossless
// PNG with a fast deflate level, for intermediates the packer reads back.
// Returns heap bytes the caller releases with vkr_vkt_free_png, or null.
void *vkr_vkt_encode_png_rgba8(const void *pixels, int width, int height,
                               size_t *out_size);
void vkr_vkt_free_png(void *bytes);

// Texture class the directory mode infers from a file name, as its
// --texture-class spelling ("color-srgb", "normal-rg", "data-mask").
const char *vkr_vkt_infer_texture_class(const char *path);

// Returns 1 when the directory mode packs files with this path's extension.
int vkr_vkt_is_supported_source(const char *path);

#ifdef __cplusplus
}
#endif
