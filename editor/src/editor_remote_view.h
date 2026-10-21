#pragma once

#include "vkr_sample_runtime.h"

/* View streaming (docs/adr/131-view-streaming.md). One editor serves its
 * Scene image over the network transport; another shows it in its Scene
 * panel and steers the serving camera with its own.
 *
 * The server resamples each Scene frame into one of a ring of video
 * surfaces (VkrViewExport), encodes it in hardware as it finishes and sends
 * it on a deadline channel. The client decodes every frame on a thread of
 * its own and shows the newest instead of rendering a Scene. It sends its
 * Scene camera pose every build; the server applies the newest before its
 * next frame and returns the pose's send time with that frame, so the
 * client measures pose-to-display latency on its own clock.
 *
 * One client at a time. Both sides need a renderer with
 * vkr_renderer_view_streaming_supported() and hardware H.264 coding
 * (vkr_video.h): macOS today. */

#define VKR_EDITOR_REMOTE_VIEW_PORT 7340u
#define VKR_EDITOR_REMOTE_VIEW_ALLOWED_MAX 16u

typedef struct VkrEditorRemoteView VkrEditorRemoteView;

VkrEditorRemoteView *vkr_editor_remote_view_create(VkrAllocator *allocator);
/* Stops serving or viewing; exports the renderer still holds report into
   memory of their own. */
void vkr_editor_remote_view_destroy(VkrEditorRemoteView *view);

/* Serves this editor's Scene on `bind` (a numeric address, default every
   interface on VKR_EDITOR_REMOTE_VIEW_PORT) to one client: with
   `allowed_count` identity keys only to those, else to anyone given the
   server key. `bitrate_max_bps` caps the encoder's rate (zero: 40 Mbit/s).
   False with the reason. */
bool8_t vkr_editor_remote_view_serve(
    VkrEditorRemoteView *view, const VkrSampleUiFrame *frame, const char *bind,
    const uint8_t (*allowed)[32], uint32_t allowed_count,
    uint32_t bitrate_max_bps, char *error, uint32_t capacity);
/* Shows the Scene of the server at `address` (numeric, default port
   VKR_EDITOR_REMOTE_VIEW_PORT) whose key is `key_hex`, connecting with
   this editor's identity key. */
bool8_t vkr_editor_remote_view_connect(VkrEditorRemoteView *view,
                                       const VkrSampleUiFrame *frame,
                                       const char *address, const char *key_hex,
                                       char *error, uint32_t capacity);
/* Stops serving or viewing and withdraws the export or import `frame`'s
   runtime holds. */
void vkr_editor_remote_view_stop(VkrEditorRemoteView *view,
                                 const VkrSampleUiFrame *frame);

/* Once per UI build, after every other request: the server applies the
   client's pose and hands out the next export; the client sends its pose
   and shows the newest decoded frame. */
void vkr_editor_remote_view_update(VkrEditorRemoteView *view,
                                   const VkrSampleUiFrame *frame);
/* The client's remote Scene image over the Scene panel. Build it before
   the Scene's overlays, which draw above it. */
void vkr_editor_remote_view_build(VkrEditorRemoteView *view,
                                  const VkrSampleUiFrame *frame);
/* Whether the Scene panel shows a remote Scene, so local Scene overlays
   stay off it. */
bool8_t vkr_editor_remote_view_showing(const VkrEditorRemoteView *view);

struct VkrBakeryJson;
struct Arena;

/* The role, address and key, the stream's size, rate and frame counts, and
   on the client the pose-to-display latency with its parts. */
struct VkrBakeryJson *
vkr_editor_remote_view_status_json(VkrEditorRemoteView *view,
                                   struct Arena *arena);
