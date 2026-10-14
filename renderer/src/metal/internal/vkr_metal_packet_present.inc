/* The editor's presenter (ADR-111): a second queue that composites the newest
 * finished Scene image with the UI into the drawable. The frame queue renders
 * Scenes at their own rate and copies each into one of the presenter's images;
 * presenter frames sample only finished copies, or wait on the GPU for the
 * copy of their own frame, so the UI presents at the display's rate. */

vkr_internal bool8_t vkr_metal_packet_presenter_create(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  id<MTLResidencySet> drawable_residency = renderer->layer.residencySet;
  if (!drawable_residency)
    return false_v;
  MTL4CommandQueueDescriptor *queue_descriptor =
      [MTL4CommandQueueDescriptor new];
  presenter->queue =
      [renderer->device newMTL4CommandQueueWithDescriptor:queue_descriptor
                                                    error:nil];
  [queue_descriptor release];
  presenter->completion = [renderer->device newSharedEvent];
  MTLResidencySetDescriptor *residency_descriptor =
      [MTLResidencySetDescriptor new];
  residency_descriptor.label = @"VKR presenter";
  residency_descriptor.initialCapacity = VKR_METAL_PACKET_PRESENT_IMAGE_COUNT +
                                         VKR_METAL_PACKET_PRESENT_SLOT_COUNT;
  presenter->residency =
      [renderer->device newResidencySetWithDescriptor:residency_descriptor
                                                error:nil];
  [residency_descriptor release];
  presenter->listener_queue = dispatch_queue_create(
      "vkr.metal.scene_completion", DISPATCH_QUEUE_SERIAL);
  presenter->listener = [[MTLSharedEventListener alloc]
      initWithDispatchQueue:presenter->listener_queue];
  presenter->render_pass = [MTL4RenderPassDescriptor new];
  bool8_t created = presenter->queue && presenter->completion &&
                    presenter->residency && presenter->listener_queue &&
                    presenter->listener && presenter->render_pass;
  for (uint32_t i = 0u; created && i < ArrayCount(presenter->slots); ++i) {
    VkrMetalPacketPresentSlot *slot = &presenter->slots[i];
    slot->allocator = [renderer->device newCommandAllocator];
    slot->buffer = [renderer->device newCommandBuffer];
    created = slot->allocator && slot->buffer;
  }
  if (!created)
    return false_v;
  /* Drawables, and the textures and rings both queues sample. */
  [presenter->queue addResidencySet:drawable_residency];
  presenter->write_image = UINT32_MAX;
  presenter->ready = true_v;
  return true_v;
}

/* Releases the presenter's objects. Every presenter frame and every Scene
   frame that copied into an image must have finished. */
vkr_internal void
vkr_metal_packet_presenter_destroy(VkrMetalPacketRenderer *renderer) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  /* Listener blocks read the presenter's counters; drain the ones queued. */
  if (presenter->listener_queue)
    dispatch_sync(presenter->listener_queue, ^{
                  });
  for (uint32_t i = 0u; i < ArrayCount(presenter->images); ++i)
    [presenter->images[i].texture release];
  for (uint32_t i = 0u; i < presenter->retired_count; ++i) {
    [presenter->retired[i].texture release];
    [presenter->retired[i].buffer release];
  }
  for (uint32_t i = 0u; i < ArrayCount(presenter->slots); ++i) {
    [presenter->slots[i].allocator release];
    [presenter->slots[i].buffer release];
    [presenter->slots[i].upload release];
  }
  [presenter->render_pass release];
  [presenter->listener release];
  if (presenter->listener_queue)
    dispatch_release(presenter->listener_queue);
  [presenter->residency release];
  [presenter->completion release];
  [presenter->queue release];
  MemZero(presenter, sizeof(*presenter));
}

/* Waits for every submitted presenter frame. */
vkr_internal void
vkr_metal_packet_presenter_wait_idle(VkrMetalPacketRenderer *renderer) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  if (!presenter->ready ||
      presenter->completion.signaledValue >= presenter->submit_value)
    return;
  if (![presenter->completion
          waitUntilSignaledValue:presenter->submit_value
                       timeoutMS:VKR_METAL_PACKET_TIMEOUT_MS])
    vkr_metal_packet_wait_timeout_exit(renderer, "present",
                                       presenter->submit_value);
}

/* Releases replaced images and upload buffers neither queue still uses. */
vkr_internal void
vkr_metal_packet_presenter_collect(VkrMetalPacketRenderer *renderer) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  const uint64_t scene_completed = renderer->completion.signaledValue;
  const uint64_t presented = presenter->completion.signaledValue;
  bool8_t residency_changed = false_v;
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < presenter->retired_count; ++i) {
    VkrMetalPacketPresentRetired *retired = &presenter->retired[i];
    if (retired->scene_value > scene_completed ||
        retired->present_value > presented) {
      presenter->retired[kept++] = *retired;
      continue;
    }
    if (retired->texture)
      [presenter->residency removeAllocation:retired->texture];
    if (retired->buffer)
      [presenter->residency removeAllocation:retired->buffer];
    [retired->texture release];
    [retired->buffer release];
    residency_changed = true_v;
  }
  presenter->retired_count = kept;
  if (residency_changed)
    [presenter->residency commit];
}

/* Queues an image or buffer for release once both queues pass the values.
   With no room left, waits for the oldest entry first. */
vkr_internal void vkr_metal_packet_presenter_retire(
    VkrMetalPacketRenderer *renderer, id<MTLTexture> texture,
    id<MTLBuffer> buffer, uint64_t scene_value, uint64_t present_value) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  if (presenter->retired_count == ArrayCount(presenter->retired)) {
    const VkrMetalPacketPresentRetired *oldest = &presenter->retired[0];
    if (oldest->scene_value > renderer->completion.signaledValue &&
        ![renderer->completion
            waitUntilSignaledValue:oldest->scene_value
                         timeoutMS:VKR_METAL_PACKET_TIMEOUT_MS])
      vkr_metal_packet_wait_timeout_exit(renderer, "frame",
                                         oldest->scene_value);
    if (oldest->present_value > presenter->completion.signaledValue &&
        ![presenter->completion
            waitUntilSignaledValue:oldest->present_value
                         timeoutMS:VKR_METAL_PACKET_TIMEOUT_MS])
      vkr_metal_packet_wait_timeout_exit(renderer, "present",
                                         oldest->present_value);
    vkr_metal_packet_presenter_collect(renderer);
  }
  presenter->retired[presenter->retired_count++] =
      (VkrMetalPacketPresentRetired){
          .texture = texture,
          .buffer = buffer,
          .scene_value = scene_value,
          .present_value = present_value,
      };
}

/* The image this Scene frame copies into: never written, or older than the
   newest image a presenter frame showed or may show next, and no longer being
   written. Presenter frames still sampling it hold the frame queue back
   through `out_present_wait`. */
vkr_internal uint32_t vkr_metal_packet_presenter_write_image(
    VkrMetalPacketRenderer *renderer, uint64_t *out_present_wait) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  for (;;) {
    const uint64_t scene_completed = renderer->completion.signaledValue;
    uint64_t newest_complete = 0u;
    uint64_t oldest_running = 0u;
    for (uint32_t i = 0u; i < ArrayCount(presenter->images); ++i) {
      const uint64_t value = presenter->images[i].scene_value;
      if (value == 0u)
        continue;
      if (value <= scene_completed)
        newest_complete = Max(newest_complete, value);
      else if (oldest_running == 0u || value < oldest_running)
        oldest_running = value;
    }
    const uint64_t kept = Max(newest_complete, presenter->shown_scene_value);
    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0u; i < ArrayCount(presenter->images); ++i) {
      const VkrMetalPacketPresentImage *image = &presenter->images[i];
      const bool8_t free =
          image->scene_value == 0u ||
          (image->scene_value <= scene_completed && image->scene_value < kept);
      if (free && (chosen == UINT32_MAX ||
                   image->scene_value < presenter->images[chosen].scene_value))
        chosen = i;
    }
    if (chosen != UINT32_MAX) {
      const uint64_t sampled = presenter->images[chosen].present_value;
      *out_present_wait =
          sampled > presenter->completion.signaledValue ? sampled : 0u;
      return chosen;
    }
    /* Two Scene frames still copy and a third image is shown: the older copy
       finishing makes the shown image older than it. */
    if (oldest_running == 0u ||
        ![renderer->completion
            waitUntilSignaledValue:oldest_running
                         timeoutMS:VKR_METAL_PACKET_TIMEOUT_MS])
      vkr_metal_packet_wait_timeout_exit(renderer, "frame", oldest_running);
  }
}

/* Editor.Present: copies the finished Scene image into a presenter image. */
vkr_internal bool8_t vkr_metal_packet_prepare_editor_present(
    VkrMetalPacketRenderer *renderer, const VkrRgPass *pass,
    VkrMetalPacketPreparedPass *prepared) API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  const VkrRgImageUse *read = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  id<MTLTexture> source =
      read ? vkr_metal_packet_texture(renderer, read->image) : nil;
  if (!presenter->ready || !source)
    return false_v;
  vkr_metal_packet_presenter_collect(renderer);
  uint64_t present_wait = 0u;
  const uint32_t index =
      vkr_metal_packet_presenter_write_image(renderer, &present_wait);
  VkrMetalPacketPresentImage *image = &presenter->images[index];
  if (!image->texture || image->texture.width != source.width ||
      image->texture.height != source.height ||
      image->texture.pixelFormat != source.pixelFormat) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:source.pixelFormat
                                     width:source.width
                                    height:source.height
                                 mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    id<MTLTexture> texture =
        [renderer->device newTextureWithDescriptor:descriptor];
    if (!texture)
      return false_v;
    texture.label = @"VKR presenter Scene image";
    if (image->texture)
      vkr_metal_packet_presenter_retire(renderer, image->texture, nil,
                                        image->scene_value,
                                        image->present_value);
    *image = (VkrMetalPacketPresentImage){.texture = texture};
    [presenter->residency addAllocation:texture];
    [presenter->residency commit];
    /* A new image has no sampler to wait for. */
    present_wait = 0u;
  }
  presenter->write_image = index;
  presenter->frame_present_wait = present_wait;
  prepared->editor_present.source = source;
  prepared->editor_present.destination = image->texture;
  prepared->active = true_v;
  return true_v;
}

/* Fits `width` x `height` inside `rect`, centred, keeping its aspect. */
vkr_internal Vec4 vkr_metal_packet_presenter_fit(Vec4 rect, uint32_t width,
                                                 uint32_t height) {
  if ((uint32_t)rect.z == width && (uint32_t)rect.w == height)
    return rect;
  const float32_t scale =
      Min(rect.z / (float32_t)width, rect.w / (float32_t)height);
  Vec4 output = rect;
  output.z = Max(1.0f, vkr_floor_f32((float32_t)width * scale));
  output.w = Max(1.0f, vkr_floor_f32((float32_t)height * scale));
  output.x += vkr_floor_f32((rect.z - output.z) * 0.5f);
  output.y += vkr_floor_f32((rect.w - output.w) * 0.5f);
  return output;
}

/* The image this presenter frame shows: the Scene of its own frame when the
   caller asks to wait for it, otherwise the newest finished one, but never one
   older than an earlier presenter frame showed. Returns UINT32_MAX before the
   first Scene copy. */
vkr_internal uint32_t vkr_metal_packet_presenter_shown_image(
    const VkrMetalPacketRenderer *renderer, uint64_t scene_value,
    bool8_t wait_scene) {
  const VkrMetalPacketPresenter *presenter = &renderer->presenter;
  const uint64_t scene_completed = renderer->completion.signaledValue;
  uint32_t chosen = UINT32_MAX;
  uint64_t chosen_value = 0u;
  uint32_t newest_written = UINT32_MAX;
  for (uint32_t i = 0u; i < ArrayCount(presenter->images); ++i) {
    const uint64_t value = presenter->images[i].scene_value;
    if (value == 0u)
      continue;
    if (newest_written == UINT32_MAX ||
        value > presenter->images[newest_written].scene_value)
      newest_written = i;
    const bool8_t eligible = value <= scene_completed ||
                             value == presenter->shown_scene_value ||
                             (wait_scene && value == scene_value);
    if (eligible && value > chosen_value) {
      chosen = i;
      chosen_value = value;
    }
  }
  /* Nothing finished or shown yet: wait for the first copy. */
  return chosen != UINT32_MAX ? chosen : newest_written;
}

/* Records, commits and presents one presenter frame into the acquired
   drawable. `scene_value` is the frame-queue value of the Scene this frame
   submitted, or zero. */
vkr_internal bool8_t vkr_metal_packet_present_frame(
    VkrMetalPacketRenderer *renderer, const VkrFrameInput *input,
    uint64_t scene_value, bool8_t wait_scene) API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  if (!presenter->ready || !input->editor)
    return false_v;
  /* A Scene frame takes its drawable only now, after its Scene. */
  if (!renderer->drawable &&
      !vkr_metal_packet_acquire_drawable(renderer, input->frame.window_width,
                                         input->frame.window_height)) {
    log_error("Metal presenter could not acquire the requested drawable "
              "extent");
    return false_v;
  }
  id<CAMetalDrawable> drawable = renderer->drawable;
  /* Textures the UI samples may still be in an open upload batch. */
  if (renderer->texture_upload_batch.enabled &&
      !vkr_metal_packet_texture_upload_batch_flush(renderer, true_v)) {
    log_error("Metal presenter could not finish pending texture uploads");
    return false_v;
  }
  vkr_metal_packet_presenter_collect(renderer);
  VkrMetalPacketPresentSlot *slot = &presenter->slots[presenter->next_slot];
  if (slot->present_value > presenter->completion.signaledValue &&
      ![presenter->completion
          waitUntilSignaledValue:slot->present_value
                       timeoutMS:VKR_METAL_PACKET_TIMEOUT_MS])
    vkr_metal_packet_wait_timeout_exit(renderer, "present",
                                       slot->present_value);
  [slot->allocator reset];

  const VkrPreparedUiDrawList *draw_list =
      input->ui ? &input->ui->draw_list : NULL;
  const uint32_t batch_count = draw_list ? draw_list->batch_count : 0u;
  const uint64_t vertex_bytes =
      draw_list ? (uint64_t)draw_list->vertex_count * sizeof(VkrUiVertex) : 0u;
  const uint64_t index_bytes =
      draw_list ? (uint64_t)draw_list->index_count * sizeof(uint32_t) : 0u;
  /* Display output, UI vertices and indices, then draw roots: one per UI
     batch and three composite records. */
  const uint64_t vertex_offset = 256u;
  const uint64_t index_offset =
      AlignPow2(vertex_offset + vertex_bytes, (uint64_t)256u);
  const uint64_t root_offset =
      AlignPow2(index_offset + index_bytes, (uint64_t)256u);
  const uint32_t root_count = batch_count + 3u;
  const uint64_t required =
      root_offset + (uint64_t)root_count * VKR_METAL_PACKET_DRAW_ROOT_STRIDE;
  if (!slot->upload || slot->upload.length < required) {
    uint64_t size = MB(1);
    while (size < required)
      size *= 2u;
    id<MTLBuffer> upload = [renderer->device
        newBufferWithLength:size
                    options:MTLResourceStorageModeShared |
                            MTLResourceHazardTrackingModeUntracked];
    if (!upload) {
      log_error("Metal presenter could not allocate %llu upload bytes",
                (unsigned long long)size);
      return false_v;
    }
    upload.label = @"VKR presenter upload";
    /* This slot's last frame finished; only it used the old buffer. */
    if (slot->upload) {
      [presenter->residency removeAllocation:slot->upload];
      [slot->upload release];
    }
    slot->upload = upload;
    [presenter->residency addAllocation:upload];
    [presenter->residency commit];
  }
  uint8_t *cpu = (uint8_t *)slot->upload.contents;
  const uint64_t gpu = slot->upload.gpuAddress;
  *(VkrDisplayOutputParams *)cpu = renderer->display_output;
  VkrMetalPacketFrameUpload upload = {
      .root_cpu = cpu + root_offset,
      .root_gpu = gpu + root_offset,
      .root_capacity = root_count,
      .display_output_gpu = gpu,
  };
  if (vertex_bytes > 0u) {
    MemCopy(cpu + vertex_offset, draw_list->vertices, vertex_bytes);
    upload.ui_vertices_gpu = gpu + vertex_offset;
  }
  if (index_bytes > 0u) {
    MemCopy(cpu + index_offset, draw_list->indices, index_bytes);
    upload.ui_indices_gpu = gpu + index_offset;
    upload.ui_indices_length = index_bytes;
  }

  const uint32_t target_width = (uint32_t)drawable.texture.width;
  const uint32_t target_height = (uint32_t)drawable.texture.height;
  const uint32_t shown =
      vkr_metal_packet_presenter_shown_image(renderer, scene_value, wait_scene);
  VkrMetalPacketPresentImage *image =
      shown != UINT32_MAX ? &presenter->images[shown] : NULL;
  uint64_t composite_root = 0u;
  Vec4 output = {0};
  if (image) {
    output = vkr_metal_packet_presenter_fit(input->editor->image_rect_px,
                                            (uint32_t)image->texture.width,
                                            (uint32_t)image->texture.height);
    VkrMetalPacketTonemapRoot *root =
        vkr_metal_packet_next_root(&upload, sizeof(*root), &composite_root);
    uint64_t exposure_gpu = 0u;
    VkrExposureGpuState *exposure =
        vkr_metal_packet_next_root(&upload, sizeof(*exposure), &exposure_gpu);
    uint64_t color_grading_gpu = 0u;
    VkrColorGradingGpu *color_grading = vkr_metal_packet_next_root(
        &upload, sizeof(*color_grading), &color_grading_gpu);
    if (!root || !exposure || !color_grading)
      return false_v;
    /* The copy is already output-encoded, as Editor.Composite samples it. */
    *exposure = (VkrExposureGpuState){.exposure_multiplier = 1.0f};
    *color_grading = (VkrColorGradingGpu){0};
    *root = (VkrMetalPacketTonemapRoot){
        .source_texture_id = image->texture.gpuResourceID._impl,
        .inverse_pre_exposure = 1.0f,
        .flags = VKR_METAL_PACKET_TONEMAP_FLAG_ALREADY_OUTPUT_ENCODED |
                 (input->editor->scene_backdrop_blur
                      ? VKR_METAL_PACKET_TONEMAP_FLAG_SCENE_BLUR
                      : 0u),
        .exposure_state = exposure_gpu,
        .output_extent = {(uint32_t)output.z, (uint32_t)output.w},
        .color_grading = color_grading_gpu,
        .display_output = gpu,
    };
  }
  VkrMetalPacketUiPass ui_pass = {0};
  VkrMetalPacketResult result = {0};
  if (draw_list && !vkr_metal_packet_prepare_ui_draw_list(
                       renderer, &ui_pass, draw_list, target_width,
                       target_height, NULL, &upload, &result)) {
    log_error("Metal presenter could not prepare %u UI batches", batch_count);
    return false_v;
  }
  /* Textures whose upload the frame queue has not finished. */
  uint64_t upload_wait = 0u;
  for (uint32_t i = 0u; i < batch_count; ++i) {
    const VkrUiDrawBatch *batch = &draw_list->batches[i];
    if (batch->texture.id == 0u || batch->texture.id == UINT32_MAX)
      continue;
    const VkrMetalPacketTexture *texture = vkr_metal_packet_resolve_texture(
        renderer, (VkrTextureHandle){.id = batch->texture.id,
                                     .generation = batch->texture.generation});
    if (texture)
      upload_wait = Max(upload_wait, texture->upload_submit_value);
  }
  /* Only committed uploads signal; the flush above committed the batch. */
  upload_wait = Min(upload_wait, renderer->submit_value);
  const uint64_t scene_wait =
      image && image->scene_value > renderer->completion.signaledValue
          ? image->scene_value
          : 0u;
  const uint64_t frame_wait = Max(scene_wait, upload_wait);

  [slot->buffer beginCommandBufferWithAllocator:slot->allocator];
  id<MTLResidencySet> residency_sets[] = {
      (id<MTLResidencySet>)vkr_metal_memory_device_residency_set(
          renderer->memory),
      (id<MTLResidencySet>)vkr_metal_material_table_device_residency_set(
          renderer->materials),
      presenter->residency,
  };
  [slot->buffer useResidencySets:residency_sets
                           count:ArrayCount(residency_sets)];
  MTL4RenderPassDescriptor *pass = presenter->render_pass;
  pass.colorAttachments[0].texture = drawable.texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
  id<MTL4RenderCommandEncoder> encoder =
      [slot->buffer renderCommandEncoderWithDescriptor:pass];
  if (!encoder) {
    [slot->buffer endCommandBuffer];
    [slot->allocator reset];
    return false_v;
  }
  encoder.label = @"Present.Editor";
  if (image) {
    [renderer->argument_table setAddress:composite_root atIndex:1u];
    [encoder setViewport:(MTLViewport){output.x, output.y, output.z, output.w,
                                       0.0, 1.0}];
    [encoder setScissorRect:(MTLScissorRect){
                                (NSUInteger)output.x, (NSUInteger)output.y,
                                (NSUInteger)output.z, (NSUInteger)output.w}];
    [encoder setRenderPipelineState:renderer->tonemap_pipeline];
    [encoder setArgumentTable:renderer->argument_table
                     atStages:MTLRenderStageFragment];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0u
                vertexCount:3u];
  }
  if (ui_pass.count)
    vkr_metal_packet_record_prepared_ui_draws(
        renderer, encoder, ui_pass.root_cpu, ui_pass.root_gpu, ui_pass.count,
        ui_pass.width, ui_pass.height);
  [encoder endEncoding];
  [slot->buffer endCommandBuffer];
  pass.colorAttachments[0].texture = nil;

  if (frame_wait)
    [presenter->queue waitForEvent:renderer->completion value:frame_wait];
  [presenter->queue waitForDrawable:drawable];
  id<MTL4CommandBuffer> submissions[] = {slot->buffer};
  [presenter->queue commit:submissions count:1];
  const uint64_t present_value = ++presenter->submit_value;
  [presenter->queue signalEvent:presenter->completion value:present_value];
  [presenter->queue signalDrawable:drawable];
  [drawable present];
  slot->present_value = present_value;
  slot->frame_mark = renderer->submit_value;
  presenter->texture_floor = renderer->submit_value + 1u;
  presenter->next_slot =
      (presenter->next_slot + 1u) % ArrayCount(presenter->slots);
  if (image) {
    image->present_value = present_value;
    presenter->shown_scene_value =
        Max(presenter->shown_scene_value, image->scene_value);
  }
  return true_v;
}

/* A Scene frame presented without the presenter makes its images stale: the
   next presenter frame must not show a Scene older than that one. Presenter
   frames still sampling them keep their present values. */
vkr_internal void
vkr_metal_packet_presenter_forget(VkrMetalPacketRenderer *renderer) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  for (uint32_t i = 0u; i < ArrayCount(presenter->images); ++i)
    presenter->images[i].scene_value = 0u;
  presenter->shown_scene_value = 0u;
}

/* Counts this Scene frame once the frame queue finishes it. */
vkr_internal void
vkr_metal_packet_presenter_count_scene(VkrMetalPacketRenderer *renderer,
                                       uint64_t submit_value) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  if (!presenter->ready)
    return;
  [renderer->completion
      notifyListener:presenter->listener
             atValue:submit_value
               block:^(id<MTLSharedEvent> event, uint64_t value) {
                 (void)event;
                 (void)value;
                 vkr_atomic_uint64_store(
                     &presenter->scene_completed_ns,
                     (uint64_t)(vkr_platform_get_absolute_time() * 1e9),
                     VKR_MEMORY_ORDER_RELAXED);
                 vkr_atomic_uint64_fetch_add(&presenter->scene_frames_completed,
                                             1u, VKR_MEMORY_ORDER_RELEASE);
               }];
}

/* Orders a frame-queue commit against the presenter (ADR-111): a Scene's
   copy waits for presenter frames still sampling its image, and a frame that
   presents itself presents behind earlier presenter frames. */
vkr_internal void
vkr_metal_packet_order_frame_commit(VkrMetalPacketRenderer *renderer,
                                    const VkrPreparedFrame *packet)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  if (packet->editor_presenter) {
    if (presenter->frame_present_wait > presenter->completion.signaledValue)
      [renderer->queue waitForEvent:presenter->completion
                              value:presenter->frame_present_wait];
    return;
  }
  if (!renderer->drawable)
    return;
  if (presenter->ready &&
      presenter->submit_value > presenter->completion.signaledValue)
    [renderer->queue waitForEvent:presenter->completion
                            value:presenter->submit_value];
  [renderer->queue waitForDrawable:renderer->drawable];
}

/* Records a committed frame-queue frame: editor Scene frames pace the next
   one by their completion, and the image Editor.Present filled now holds the
   newest Scene. A Scene presented without the presenter makes its images
   stale. */
vkr_internal void
vkr_metal_packet_presenter_frame_committed(VkrMetalPacketRenderer *renderer,
                                           const VkrPreparedFrame *packet,
                                           uint64_t submit_value) {
  VkrMetalPacketPresenter *presenter = &renderer->presenter;
  if (!packet->scene_rendering || !packet->input.editor)
    return;
  vkr_metal_packet_presenter_count_scene(renderer, submit_value);
  if (!packet->editor_presenter)
    vkr_metal_packet_presenter_forget(renderer);
  else if (presenter->write_image < ArrayCount(presenter->images))
    presenter->images[presenter->write_image].scene_value = submit_value;
}

/* Presents a committed frame-queue frame: through the presenter, or its own
   drawable. A presenter failure drops this presentation only; the Scene was
   submitted and the next frame presents again. */
vkr_internal void vkr_metal_packet_present_committed_frame(
    VkrMetalPacketRenderer *renderer, const VkrPreparedFrame *packet,
    uint64_t submit_value) API_AVAILABLE(macos(26.0)) {
  if (packet->editor_presenter) {
    if (!vkr_metal_packet_present_frame(
            renderer, &packet->input, submit_value,
            packet->input.editor->present_wait_scene))
      log_error("Metal presenter could not present frame %llu",
                (unsigned long long)packet->input.frame.frame_index);
    return;
  }
  if (!renderer->drawable)
    return;
  [renderer->queue signalDrawable:renderer->drawable];
  [renderer->drawable present];
  VKR_METAL_DIAGNOSTIC(renderer, "drawable_present", "frame=%llu",
                       (unsigned long long)packet->input.frame.frame_index);
}

bool8_t vkr_metal_packet_renderer_presenter_supported(
    const VkrMetalPacketRenderer *renderer) {
  return renderer && renderer->presenter.ready;
}

uint64_t vkr_metal_packet_renderer_scene_frames_completed(
    const VkrMetalPacketRenderer *renderer, uint64_t *out_completed_ns) {
  if (!renderer) {
    if (out_completed_ns)
      *out_completed_ns = 0u;
    return 0u;
  }
  VkrMetalPacketRenderer *mutable_renderer = (VkrMetalPacketRenderer *)renderer;
  const uint64_t completed = vkr_atomic_uint64_load(
      &mutable_renderer->presenter.scene_frames_completed,
      VKR_MEMORY_ORDER_ACQUIRE);
  if (out_completed_ns)
    *out_completed_ns =
        vkr_atomic_uint64_load(&mutable_renderer->presenter.scene_completed_ns,
                               VKR_MEMORY_ORDER_RELAXED);
  return completed;
}

bool8_t vkr_metal_packet_renderer_prepare_present(
    VkrMetalPacketRenderer *renderer,
    const VkrRenderGraphFrameInfo *frame_info) {
  @autoreleasepool {
    if (!renderer || !renderer->presenter.ready || renderer->frame_prepared ||
        renderer->present_prepared || renderer->reserved_frame_slot ||
        frame_info->target_width == 0u || frame_info->target_height == 0u)
      return false_v;
    if (vkr_metal_packet_pixel_format(frame_info->target_color_format) !=
        renderer->layer.pixelFormat) {
      log_error("Metal present preparation target format mismatch");
      return false_v;
    }
    if (!vkr_metal_packet_acquire_drawable(renderer, frame_info->target_width,
                                           frame_info->target_height)) {
      log_error("Metal present preparation could not acquire the requested "
                "drawable extent");
      return false_v;
    }
    renderer->present_prepared = true_v;
    return true_v;
  }
}

bool8_t vkr_metal_packet_renderer_present(VkrMetalPacketRenderer *renderer,
                                          const VkrFrameInput *input) {
  @autoreleasepool {
    if (!renderer || !input || !renderer->present_prepared)
      return false_v;
    if (!@available(macOS 26.0, *)) {
      vkr_metal_packet_finish_frame(renderer, false_v, false_v);
      return false_v;
    }
    const bool8_t presented =
        vkr_metal_packet_present_frame(renderer, input, 0u, false_v);
    vkr_metal_packet_finish_frame(renderer, false_v, presented);
    return presented;
  }
}
