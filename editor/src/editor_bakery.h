#pragma once

#include "renderer/systems/vkr_ui_system.h"

typedef struct VkrEditorBakery VkrEditorBakery;

/** UI-thread owner. Allocator must support independent frees and outlive the
 * Bakery. Destroy cancels and joins its sole worker before releasing storage.
 */
VkrEditorBakery *vkr_editor_bakery_create(VkrAllocator *allocator);
void vkr_editor_bakery_destroy(VkrEditorBakery *bakery);
/** Poll every editor frame, including while the Bakery panel is hidden. */
void vkr_editor_bakery_update(VkrEditorBakery *bakery);
/** Build within a caller-owned panel. No renderer or asset mutation occurs. */
void vkr_editor_bakery_build(VkrEditorBakery *bakery, VkrUiSystem *ui,
                             VkrUiRect rect, VkrFontHandle heading);
