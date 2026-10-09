#pragma once

#include "assets/vkr_material_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Custom-tier code generation (docs/proposals/artist-toolkit.md, part 3). A
 * graph the Standard tier cannot lower becomes a surface function of its
 * own: a struct named after the hash of its generated source, so equal
 * graphs share one function, whose `evaluate` maps the shaded point's inputs
 * to the material model's core outputs. Exposed constants read the
 * material's parameters and every texture reads one of the material's
 * Custom texture slots, so an instance changes row data only. The module
 * owns no state; callers own the graph and the text it writes.
 */

#define VKR_MATERIAL_CUSTOM_TEXTURE_MAX 8u
#define VKR_MATERIAL_CUSTOM_PARAM_MAX 8u
/* Custom graphs a project may hold: each adds its own pipelines, so the
   editor warns past it and packaging refuses (proposal, part 3). */
#define VKR_MATERIAL_CUSTOM_BUDGET 32u
/* The tiled pipeline's forward variants a Custom graph gets: by lighting
   (none, punctual, shadowed, all), probes and decals, plus the editor's
   inspection, for opaque and alpha-tested draws. */
#define VKR_MATERIAL_CUSTOM_FORWARD_VARIANTS 17u
/* Pipeline states a Custom graph adds on Metal, once for all its instances:
   forward and alpha-tested, by lighting and inspection (5), probes (2) and
   decals (2). */
#define VKR_MATERIAL_CUSTOM_PIPELINES 40u

/* How a Custom graph's nodes bind to a material's row: the texture node of
   each Custom texture slot, with the class its uses need, and the exposed
   constant of each parameter slot. Node indices index the graph. */
typedef struct VkrMaterialCustomBinding {
  uint32_t texture_nodes[VKR_MATERIAL_CUSTOM_TEXTURE_MAX];
  /* The texture feeds a normal map; else it is colour (sRGB) when its RGB
     feeds a colour and data (linear) otherwise, unless its node names a
     colour space. */
  bool8_t texture_normal[VKR_MATERIAL_CUSTOM_TEXTURE_MAX];
  bool8_t texture_srgb[VKR_MATERIAL_CUSTOM_TEXTURE_MAX];
  uint32_t texture_count;
  uint32_t param_nodes[VKR_MATERIAL_CUSTOM_PARAM_MAX];
  uint32_t param_count;
} VkrMaterialCustomBinding;

/* Generates `graph`'s surface function in Metal Shading Language for the
   tiled pipeline:

     struct <function> {
       static VkrMetalCustomOutput evaluate(
           thread const VkrMetalCustomInput &in,
           const device VkrMetalPacketCustomMaterial &custom);
     };

   `function` is `vkr_custom_<16 hex digits>` of the source. False with
   `out->reason` and `out->node` when the graph has no Custom form. */
bool8_t vkr_material_codegen_msl(const VkrMaterialGraph *graph,
                                 VkrAllocator *allocator, String8 *out_source,
                                 VkrMaterialCustomBinding *out_binding,
                                 VkrMaterialLowering *out);

/* The Metal fragment entry points of Custom function `function`: one per
   forward variant of the tiled pipeline, opaque and alpha-tested, each named
   `<function>_<the Standard entry's name>` so the renderer finds a graph's
   pipelines through the Standard entries' table. */
bool8_t vkr_material_codegen_msl_entries(const char *function,
                                         VkrAllocator *allocator,
                                         String8 *out_source);

#ifdef __cplusplus
}
#endif
