#pragma once

#include "renderer/vkr_renderer.h"

typedef enum VkrBootProfile {
  VKR_BOOT_PROFILE_FULL = 0,
  VKR_BOOT_PROFILE_AUTOMATION,
} VkrBootProfile;

/**
 * Stable initialization units used by dependency-resolved boot plans.
 *
 * The mandatory units come first and always initialize; the optional units
 * follow `VKR_RENDERER_SUBSYSTEM_WORLD` and are the only ones a plan may omit.
 * A new unit therefore belongs on the side of that boundary that matches
 * whether application initialization gates it.
 */
typedef enum VkrRendererSubsystem {
  VKR_RENDERER_SUBSYSTEM_CAMERA = 0,
  VKR_RENDERER_SUBSYSTEM_RENDER_GRAPH,
  VKR_RENDERER_SUBSYSTEM_FRAME_STREAMS,
  VKR_RENDERER_SUBSYSTEM_RESOURCES,
  VKR_RENDERER_SUBSYSTEM_GEOMETRY,
  VKR_RENDERER_SUBSYSTEM_TEXTURES,
  VKR_RENDERER_SUBSYSTEM_MATERIALS,
  VKR_RENDERER_SUBSYSTEM_MESHES,
  VKR_RENDERER_SUBSYSTEM_FONTS,
  VKR_RENDERER_SUBSYSTEM_LIGHTING,
  VKR_RENDERER_SUBSYSTEM_SHADOWS,
  VKR_RENDERER_SUBSYSTEM_WORLD,
  VKR_RENDERER_SUBSYSTEM_UI,
  VKR_RENDERER_SUBSYSTEM_SKYBOX,
  VKR_RENDERER_SUBSYSTEM_EDITOR,
  VKR_RENDERER_SUBSYSTEM_GIZMO,
  VKR_RENDERER_SUBSYSTEM_PICKING,
  VKR_RENDERER_SUBSYSTEM_COUNT,
} VkrRendererSubsystem;

typedef uint64_t VkrSubsystemMask;

#define VKR_RENDERER_SUBSYSTEM_BIT(SUBSYSTEM)                                  \
  ((VkrSubsystemMask)1u << (uint32_t)(SUBSYSTEM))
#define VKR_RENDERER_SUBSYSTEM_ALL                                             \
  (VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_COUNT) - 1u)
/** Units every plan contains; no boot profile can omit them. */
#define VKR_RENDERER_SUBSYSTEM_MANDATORY                                       \
  (VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_UI) - 1u)
/** The complement: the only units a workload may leave out. */
#define VKR_RENDERER_SUBSYSTEM_OPTIONAL                                        \
  (VKR_RENDERER_SUBSYSTEM_ALL & ~VKR_RENDERER_SUBSYSTEM_MANDATORY)

/* The two masks are derived from the enum order, so a unit inserted on the
   wrong side of the boundary would silently become excludable — or stop being
   excludable — without touching either definition. */
_Static_assert(VKR_RENDERER_SUBSYSTEM_UI == VKR_RENDERER_SUBSYSTEM_WORLD + 1,
               "Optional subsystems must directly follow the mandatory ones");
_Static_assert(VKR_RENDERER_SUBSYSTEM_COUNT ==
                   VKR_RENDERER_SUBSYSTEM_PICKING + 1,
               "Picking must remain the last optional subsystem");

/**
 * One immutable initialization contract. `requested_mask` describes workload
 * needs; `effective_mask` is their transitive dependency closure.
 */
typedef struct VkrSubsystemPlan {
  VkrBootProfile profile;
  VkrSubsystemMask requested_mask;
  VkrSubsystemMask excluded_mask;
  VkrSubsystemMask effective_mask;
} VkrSubsystemPlan;

/**
 * Builds and validates a plan before any application subsystem is initialized.
 * Dependencies that intersect `excluded_mask` make the request impossible.
 *
 * Callers describe intent only: `effective_mask` is always computed here, so a
 * zero-initialized request under `VKR_BOOT_PROFILE_FULL` resolves to
 * `VKR_RENDERER_SUBSYSTEM_ALL`.
 *
 * @param out_error Optional; receives the rejection reason when one is wanted.
 */
bool8_t vkr_subsystem_plan_build(VkrBootProfile profile,
                                 VkrSubsystemMask requested_mask,
                                 VkrSubsystemMask excluded_mask,
                                 VkrSubsystemPlan *out_plan,
                                 VkrRendererError *out_error);

bool8_t vkr_subsystem_plan_includes(const VkrSubsystemPlan *plan,
                                    VkrRendererSubsystem subsystem);
