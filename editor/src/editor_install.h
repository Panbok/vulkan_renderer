#pragma once

#include "defines.h"
#include "platform/vkr_platform.h"

/* Where the editor's companion programs and per-user state live. A build tree
 * keeps them in the repository; an editor distribution
 * (docs/adr/078-project-build-and-packaging.md) places the programs beside
 * the editor, its engine content in `content/` (vkr_content_root()), and
 * writable state in per-user directories. */

#define VKR_EDITOR_INSTALL_PATH_CAPACITY 2048u

/** Companion programs the editor starts. */
typedef enum VkrEditorTool {
  VKR_EDITOR_TOOL_BAKERY = 0,
  VKR_EDITOR_TOOL_HARNESS,
  VKR_EDITOR_TOOL_ASSET_PREVIEW,
  VKR_EDITOR_TOOL_COUNT
} VkrEditorTool;

/** Resolves the companion programs and, outside the repository, the texture
 * transcode cache. Call once on the main thread before any job or renderer
 * starts; everything it resolves is read-only afterwards. */
void vkr_editor_install_resolve(void);
/** Absolute path of a companion program: beside the editor when it is
 * there, else where this build tree produced it. */
const char *vkr_editor_tool_path(VkrEditorTool tool);
/** The script SDK headers' include root (ADR-079): `sdk` beside the editor
 * when it holds sdk.h, else the build tree's staged copy. */
const char *vkr_editor_script_sdk_dir(void);
/** `<base>/VKR/<leaf>` below a per-user base directory; not created. */
bool8_t vkr_editor_user_path(VkrPlatformUserDirectory kind, const char *leaf,
                             char *out, uint32_t capacity);
