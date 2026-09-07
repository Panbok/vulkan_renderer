#include "core/vkr_subsystem_plan.h"

vkr_global const VkrSubsystemMask
    vkr_renderer_subsystem_dependencies[VKR_RENDERER_SUBSYSTEM_COUNT] = {
        [VKR_RENDERER_SUBSYSTEM_CAMERA] = 0u,
        [VKR_RENDERER_SUBSYSTEM_RENDER_GRAPH] = 0u,
        [VKR_RENDERER_SUBSYSTEM_FRAME_STREAMS] = 0u,
        [VKR_RENDERER_SUBSYSTEM_RESOURCES] = 0u,
        [VKR_RENDERER_SUBSYSTEM_GEOMETRY] = 0u,
        [VKR_RENDERER_SUBSYSTEM_TEXTURES] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES),
        [VKR_RENDERER_SUBSYSTEM_MATERIALS] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES),
        [VKR_RENDERER_SUBSYSTEM_MESHES] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS),
        [VKR_RENDERER_SUBSYSTEM_FONTS] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES),
        [VKR_RENDERER_SUBSYSTEM_LIGHTING] = 0u,
        [VKR_RENDERER_SUBSYSTEM_SHADOWS] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RENDER_GRAPH) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES),
        [VKR_RENDERER_SUBSYSTEM_WORLD] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS),
        [VKR_RENDERER_SUBSYSTEM_UI] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_FONTS),
        [VKR_RENDERER_SUBSYSTEM_SKYBOX] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES),
        [VKR_RENDERER_SUBSYSTEM_EDITOR] = 0u,
        [VKR_RENDERER_SUBSYSTEM_GIZMO] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MESHES),
        [VKR_RENDERER_SUBSYSTEM_PICKING] =
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_RESOURCES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_GEOMETRY) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_TEXTURES) |
            VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_MATERIALS),
};

bool8_t vkr_subsystem_plan_build(VkrBootProfile profile,
                                 VkrSubsystemMask requested_mask,
                                 VkrSubsystemMask excluded_mask,
                                 VkrSubsystemPlan *out_plan,
                                 VkrRendererError *out_error) {
  VkrRendererError discarded_error = VKR_RENDERER_ERROR_NONE;
  if (!out_error) {
    out_error = &discarded_error;
  }
  if (!out_plan || profile > VKR_BOOT_PROFILE_AUTOMATION ||
      ((requested_mask | excluded_mask) & ~VKR_RENDERER_SUBSYSTEM_ALL) != 0u ||
      (profile == VKR_BOOT_PROFILE_FULL && excluded_mask != 0u)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrSubsystemMask effective =
      profile == VKR_BOOT_PROFILE_FULL
          ? VKR_RENDERER_SUBSYSTEM_ALL
          : VKR_RENDERER_SUBSYSTEM_MANDATORY | requested_mask;
  /* Iterated to a fixed point rather than swept once: the dependency table is
     ordered for readability, not topologically, so a later unit may pull in an
     earlier one that has dependencies of its own. The mask only ever grows
     within a bounded bit space, so this terminates. */
  VkrSubsystemMask prior = 0u;
  while (prior != effective) {
    prior = effective;
    for (uint32_t subsystem = 0u; subsystem < VKR_RENDERER_SUBSYSTEM_COUNT;
         ++subsystem) {
      if ((effective & VKR_RENDERER_SUBSYSTEM_BIT(subsystem)) != 0u) {
        effective |= vkr_renderer_subsystem_dependencies[subsystem];
      }
    }
  }
  if ((effective & excluded_mask) != 0u) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  *out_plan = (VkrSubsystemPlan){
      .profile = profile,
      .requested_mask = profile == VKR_BOOT_PROFILE_FULL
                            ? VKR_RENDERER_SUBSYSTEM_ALL
                            : requested_mask,
      .excluded_mask = excluded_mask,
      .effective_mask = effective,
  };
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

bool8_t vkr_subsystem_plan_includes(const VkrSubsystemPlan *plan,
                                    VkrRendererSubsystem subsystem) {
  return plan && subsystem < VKR_RENDERER_SUBSYSTEM_COUNT &&
         (plan->effective_mask & VKR_RENDERER_SUBSYSTEM_BIT(subsystem)) != 0u;
}
