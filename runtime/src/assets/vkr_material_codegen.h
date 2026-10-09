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

#ifdef __cplusplus
}
#endif
