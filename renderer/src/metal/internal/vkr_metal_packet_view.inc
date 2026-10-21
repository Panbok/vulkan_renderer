/* View streaming (ADR-131): a frame resamples its finished Scene image into
 * a surface a video encoder reads (shaders/metal/msl/editor/view_export.metal)
 * and the UI draws a decoded remote Scene image. Surfaces are lib video
 * surfaces (vkr_video.h) whose IOSurfaces Metal wraps as textures; an entry
 * keeps its surface and texture, resident, until the GPU finished the last
 * frame that used them. */

vkr_internal bool8_t vkr_metal_packet_view_prepare(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketView *view = &renderer->view;
  if (view->ready) {
    return true_v;
  }
  /* Made once: a failed attempt does not repeat every frame. */
  if (view->residency) {
    return false_v;
  }
  MTLResidencySetDescriptor *descriptor = [MTLResidencySetDescriptor new];
  descriptor.label = @"VKR view streaming";
  descriptor.initialCapacity = VKR_METAL_PACKET_VIEW_TEXTURES;
  view->residency =
      [renderer->device newResidencySetWithDescriptor:descriptor error:nil];
  [descriptor release];
  view->listener_queue =
      dispatch_queue_create("vkr.metal.view_export", DISPATCH_QUEUE_SERIAL);
  view->listener = view->listener_queue
                       ? [[MTLSharedEventListener alloc]
                             initWithDispatchQueue:view->listener_queue]
                       : nil;
  view->export_pipeline = vkr_metal_packet_create_compute_pipeline(
      renderer, renderer->fragment_library, "vkr_metal_packet_view_export",
      VKR_METAL_PACKET_ABI_VIEW_EXPORT_ROOT, false_v,
      @"VKR Metal packet view export");
  view->ready = view->residency && view->listener && view->export_pipeline;
  if (!view->ready) {
    log_warn("Metal view streaming unavailable: no residency set, listener "
             "or export pipeline");
  }
  return view->ready;
}

/* The texture over `surface`'s IOSurface in `format`, wrapped once and kept
   while frames use it; nil when it cannot be wrapped. */
vkr_internal id<MTLTexture>
vkr_metal_packet_view_texture(VkrMetalPacketRenderer *renderer,
                              VkrVideoSurface *surface, MTLPixelFormat format)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketView *view = &renderer->view;
  IOSurfaceRef platform = (IOSurfaceRef)vkr_video_surface_platform(surface);
  if (!platform || !vkr_metal_packet_view_prepare(renderer)) {
    return nil;
  }
  const uint64_t use_value = renderer->submit_value + 1u;
  for (uint32_t i = 0u; i < VKR_METAL_PACKET_VIEW_TEXTURES; ++i) {
    VkrMetalPacketViewTexture *entry = &view->textures[i];
    if (entry->texture && entry->platform == platform &&
        entry->texture.pixelFormat == format) {
      entry->last_use_value = use_value;
      return entry->texture;
    }
  }
  /* A free entry, else the least recently used one the GPU is done with. */
  const uint64_t completed = vkr_metal_packet_completed(renderer);
  VkrMetalPacketViewTexture *chosen = NULL;
  for (uint32_t i = 0u; i < VKR_METAL_PACKET_VIEW_TEXTURES; ++i) {
    VkrMetalPacketViewTexture *entry = &view->textures[i];
    if (!entry->texture) {
      chosen = entry;
      break;
    }
    if (entry->last_use_value <= completed &&
        (!chosen || entry->last_use_value < chosen->last_use_value)) {
      chosen = entry;
    }
  }
  if (!chosen) {
    return nil;
  }
  if (chosen->texture) {
    [view->residency removeAllocation:chosen->texture];
    [chosen->texture release];
    vkr_video_surface_release(chosen->surface);
    *chosen = (VkrMetalPacketViewTexture){0};
  }
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:format
                                   width:vkr_video_surface_width(surface)
                                  height:vkr_video_surface_height(surface)
                               mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.hazardTrackingMode = MTLHazardTrackingModeUntracked;
  id<MTLTexture> texture = [renderer->device newTextureWithDescriptor:descriptor
                                                            iosurface:platform
                                                                plane:0];
  if (!texture) {
    return nil;
  }
  texture.label = @"VKR view streaming surface";
  vkr_video_surface_retain(surface);
  *chosen = (VkrMetalPacketViewTexture){.surface = surface,
                                        .platform = platform,
                                        .texture = texture,
                                        .last_use_value = use_value};
  [view->residency addAllocation:texture];
  [view->residency commit];
  return texture;
}

/* Reports an export the frame could not copy into, at once. */
vkr_internal void vkr_metal_packet_view_reject(const VkrViewExport *export) {
  if (export && export->done) {
    export->done(export->context, export->tag, false_v);
  }
}

/* First in a frame's submission: takes the frame's export, which
   frame_committed reports, and its import for the UI. An export a failed
   frame left unreported is reported unwritten first, and destroy reports
   the last one. */
vkr_internal void
vkr_metal_packet_view_begin_frame(VkrMetalPacketRenderer *renderer,
                                  const VkrPreparedFrame *packet)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketView *view = &renderer->view;
  if (view->has_pending) {
    view->has_pending = false_v;
    vkr_metal_packet_view_reject(&view->pending);
  }
  view->import_texture = nil;
  const VkrEditorPassPayload *editor = packet->input.editor;
  if (editor && editor->view_export) {
    view->pending = *editor->view_export;
    view->has_pending = true_v;
    view->pending_written = false_v;
    /* The frame attaches the residency set before its passes record, so
       it exists before the export's surface joins it. */
    (void)vkr_metal_packet_view_prepare(renderer);
  }
  if (editor && editor->view_import) {
    view->import_texture = vkr_metal_packet_view_texture(
        renderer, editor->view_import, MTLPixelFormatBGRA8Unorm_sRGB);
  }
}

/* Whether the frame's passes use the view streaming residency set. */
vkr_internal bool8_t vkr_metal_packet_view_frame_uses(
    const VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet) {
  const VkrEditorPassPayload *editor = packet->input.editor;
  return renderer->view.ready && editor &&
         (editor->view_import || editor->view_export);
}

/* After the frame's passes are prepared: the dispatch that resamples the
   finished Scene image into the export's surface, when the frame rendered a
   Scene. Without one the export is reported unwritten. */
vkr_internal void vkr_metal_packet_view_prepare_export(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    VkrMetalPacketFrameUpload *upload) API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketView *view = &renderer->view;
  view->export_active = false_v;
  if (!view->has_pending || !view->ready || !view->pending.surface ||
      !packet->scene_rendering || packet->input.editor->present_only) {
    return;
  }
  id<MTLTexture> scene =
      vkr_metal_packet_named_texture(renderer, "editor_scene_image");
  id<MTLTexture> destination =
      scene ? vkr_metal_packet_view_texture(renderer, view->pending.surface,
                                            MTLPixelFormatBGRA8Unorm)
            : nil;
  uint64_t root_gpu = 0u;
  VkrMetalPacketViewExportRoot *root =
      destination ? vkr_metal_packet_next_root(upload, sizeof(*root), &root_gpu)
                  : NULL;
  if (!root) {
    return;
  }
  const uint32_t width = (uint32_t)destination.width;
  const uint32_t height = (uint32_t)destination.height;
  *root = (VkrMetalPacketViewExportRoot){
      .source = scene.gpuResourceID._impl,
      .destination = destination.gpuResourceID._impl,
      .extent = {width, height},
      /* 8-bit UNORM Scene images hold display-encoded values. */
      .source_encoded = scene.pixelFormat == MTLPixelFormatBGRA8Unorm ||
                        scene.pixelFormat == MTLPixelFormatRGBA8Unorm,
  };
  view->export_root = root_gpu;
  view->export_width = width;
  view->export_height = height;
  view->export_active = true_v;
}

/* In the frame's readback encoder, after the barrier that waits for the
   Scene image's last writer: records the prepared export. */
vkr_internal void
vkr_metal_packet_view_record_export(VkrMetalPacketRenderer *renderer,
                                    id<MTL4ComputeCommandEncoder> encoder)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketView *view = &renderer->view;
  if (!view->export_active) {
    return;
  }
  view->export_active = false_v;
  const VkrMetalPacketDispatch dispatch = {
      .pipeline = view->export_pipeline,
      .root = view->export_root,
      .threads = MTLSizeMake(view->export_width, view->export_height, 1u),
      .group_size = MTLSizeMake(8u, 8u, 1u),
  };
  vkr_metal_packet_record_dispatch(renderer, encoder, &dispatch);
  view->pending_written = true_v;
}

/* After the frame's commit and completion signal: reports the frame's
   export, once the GPU passes `submit_value` when the frame copied it. */
vkr_internal void
vkr_metal_packet_view_frame_committed(VkrMetalPacketRenderer *renderer,
                                      uint64_t submit_value)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketView *view = &renderer->view;
  if (!view->has_pending) {
    return;
  }
  view->has_pending = false_v;
  const VkrViewExport export = view->pending;
  if (!view->pending_written || !view->listener) {
    vkr_metal_packet_view_reject(&export);
    return;
  }
  [renderer->completion
      notifyListener:view->listener
             atValue:submit_value
               block:^(id<MTLSharedEvent> event, uint64_t value) {
                 (void)event;
                 (void)value;
                 if (export.done) {
                   export.done(export.context, export.tag, true_v);
                 }
               }];
}

/* Releases every wrapped surface; the GPU must have finished every frame
   and the listener queue its blocks. */
vkr_internal void
vkr_metal_packet_view_destroy(VkrMetalPacketRenderer *renderer) {
  VkrMetalPacketView *view = &renderer->view;
  if (view->has_pending) {
    view->has_pending = false_v;
    vkr_metal_packet_view_reject(&view->pending);
  }
  /* Listener blocks report exports; drain the ones queued. */
  if (view->listener_queue) {
    dispatch_sync(view->listener_queue, ^{
                  });
  }
  for (uint32_t i = 0u; i < VKR_METAL_PACKET_VIEW_TEXTURES; ++i) {
    VkrMetalPacketViewTexture *entry = &view->textures[i];
    if (entry->texture) {
      [entry->texture release];
      vkr_video_surface_release(entry->surface);
    }
  }
  [view->export_pipeline release];
  [view->listener release];
  if (view->listener_queue) {
    dispatch_release(view->listener_queue);
  }
  [view->residency release];
  *view = (VkrMetalPacketView){0};
}
