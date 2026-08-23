#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum TimestampScope {
  TIMESTAMP_SCOPE_ENCODER,
  TIMESTAMP_SCOPE_COMMAND_BUFFER,
} TimestampScope;

typedef enum WorkKind {
  WORK_EMPTY,
  WORK_FILL,
  WORK_COPY,
  WORK_DISPATCH,
  WORK_DRAW,
  WORK_MIXED,
  WORK_MIXED_TRANSFER,
} WorkKind;

typedef enum EncoderWorkKind {
  ENCODER_WORK_COMPUTE,
  ENCODER_WORK_RENDER,
  ENCODER_WORK_TRANSFER,
} EncoderWorkKind;

typedef enum DependencyKind {
  DEPENDENCY_NONE,
  DEPENDENCY_CROSS_ENCODER,
  DEPENDENCY_INTRA_ENCODER,
} DependencyKind;

typedef enum ResidencyKind {
  RESIDENCY_NONE,
  RESIDENCY_SINGLE_SET,
  RESIDENCY_MULTIPLE_SETS,
} ResidencyKind;

typedef enum ComputeResourceKind {
  COMPUTE_RESOURCE_NONE,
  COMPUTE_RESOURCE_BUFFER,
} ComputeResourceKind;

typedef enum ResourceAllocationKind {
  RESOURCE_ALLOCATION_DIRECT,
  RESOURCE_ALLOCATION_DIRECT_UNTRACKED,
  RESOURCE_ALLOCATION_PLACEMENT,
} ResourceAllocationKind;

typedef enum DrawCommandKind {
  DRAW_COMMAND_DIRECT,
  DRAW_COMMAND_INDIRECT_CPU,
  DRAW_COMMAND_INDIRECT_GPU,
  DRAW_COMMAND_INDIRECT_GPU_INDEXED,
} DrawCommandKind;

typedef enum IcbRangeKind {
  ICB_RANGE_FIXED,
  ICB_RANGE_INDIRECT,
} IcbRangeKind;

typedef struct DiagnosticConfig {
  TimestampScope scope;
  MTL4TimestampGranularity granularity;
  WorkKind work;
  DependencyKind dependency;
  ComputeResourceKind compute_resource;
  ResourceAllocationKind resource_allocation;
  DrawCommandKind draw_command;
  IcbRangeKind icb_range;
  uint32_t encoder_count;
  uint32_t command_count;
  uint32_t transfer_bytes;
  uint32_t iteration_count;
  uint32_t slot_count;
  uint32_t timeout_ms;
  bool reuse_heaps;
  bool summary_only;
  ResidencyKind residency;
} DiagnosticConfig;

typedef struct DiagnosticSlot {
  id<MTL4CommandAllocator> allocator;
  id<MTL4CommandBuffer> command_buffer;
  id<MTL4CounterHeap> counter_heap;
  uint64_t submit_value;
  uint32_t iteration;
} DiagnosticSlot;

typedef struct DiagnosticTotals {
  uint64_t sample_count;
  uint64_t valid_interval_count;
  uint64_t zero_interval_count;
  uint64_t invalid_interval_count;
} DiagnosticTotals;

typedef struct DiagnosticRenderWorkload {
  id<MTLRenderPipelineState> pipeline;
  id<MTLTexture> *targets;
  MTL4RenderPassDescriptor **passes;
  uint32_t pass_count;
} DiagnosticRenderWorkload;

typedef struct DiagnosticComputeWorkload {
  id<MTLComputePipelineState> pipeline;
  id<MTLBuffer> *buffers;
  id<MTL4ArgumentTable> *argument_tables;
  uint32_t slot_count;
} DiagnosticComputeWorkload;

typedef struct DiagnosticTransferWorkload {
  id<MTLBuffer> *sources;
  id<MTLBuffer> *destinations;
  uint32_t source_count;
  uint32_t destination_count;
} DiagnosticTransferWorkload;

typedef struct DiagnosticResidencyWorkload {
  id<MTLBuffer> allocations[2];
  id<MTLResidencySet> sets[2];
  uint32_t set_count;
} DiagnosticResidencyWorkload;

typedef struct DiagnosticPlacementWorkload {
  id<MTLHeap> heap;
  NSUInteger size;
  NSUInteger cursor;
} DiagnosticPlacementWorkload;

typedef struct DiagnosticIndirectWorkload {
  id<MTLIndirectCommandBuffer> *buffers;
  id<MTLResidencySet> *residencies;
  id<MTLBuffer> *arguments;
  id<MTLBuffer> *range_buffers;
  id<MTLBuffer> *range_configs;
  id<MTLBuffer> *index_buffers;
  id<MTLBuffer> *vertex_roots;
  id<MTLBuffer> *fragment_roots;
  id<MTL4ArgumentTable> *argument_tables;
  id<MTLComputePipelineState> encode_pipeline;
  id<MTLArgumentEncoder> argument_encoder;
  uint32_t slot_count;
  uint32_t command_count;
} DiagnosticIndirectWorkload;

static bool work_uses_render(WorkKind work);
static bool work_uses_transfer(WorkKind work);

static bool draw_command_is_gpu(DrawCommandKind command) {
  return command == DRAW_COMMAND_INDIRECT_GPU ||
         command == DRAW_COMMAND_INDIRECT_GPU_INDEXED;
}

static void usage(const char *program) {
  fprintf(stderr,
          "Usage: %s [--scope encoder|command-buffer] "
          "[--granularity relaxed|precise] "
          "[--work empty|fill|copy|dispatch|draw|mixed|mixed-transfer] "
          "[--dependency none|cross|intra] [--encoders N] [--commands N] "
          "[--transfer-bytes N] "
          "[--iterations N] [--slots N] "
          "[--timeout-ms N] [--reuse-heaps] [--residency] "
          "[--residency-mode none|single|multiple] "
          "[--compute-resource none|buffer] "
          "[--resource-allocation direct|untracked|placement] "
          "[--draw-command direct|indirect|gpu-indirect|gpu-indexed] "
          "[--icb-range fixed|indirect] "
          "[--summary-only]\n",
          program);
}

static bool parse_u32(const char *text, uint32_t minimum, uint32_t maximum,
                      uint32_t *out_value) {
  if (!text || !out_value || text[0] == '\0')
    return false;
  errno = 0;
  char *end = NULL;
  const unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || !end || end[0] != '\0' || value < minimum ||
      value > maximum)
    return false;
  *out_value = (uint32_t)value;
  return true;
}

static bool parse_config(int argc, const char *const *argv,
                         DiagnosticConfig *config) {
  *config = (DiagnosticConfig){
      .scope = TIMESTAMP_SCOPE_ENCODER,
      .granularity = MTL4TimestampGranularityRelaxed,
      .work = WORK_EMPTY,
      .encoder_count = 1u,
      .command_count = 1u,
      .transfer_bytes = 4096u,
      .iteration_count = 1u,
      .slot_count = 1u,
      .timeout_ms = 5000u,
  };
  for (int i = 1; i < argc; ++i) {
    const char *argument = argv[i];
    if (strcmp(argument, "--reuse-heaps") == 0) {
      config->reuse_heaps = true;
    } else if (strcmp(argument, "--summary-only") == 0) {
      config->summary_only = true;
    } else if (strcmp(argument, "--residency") == 0) {
      config->residency = RESIDENCY_SINGLE_SET;
    } else if (strcmp(argument, "--help") == 0) {
      return false;
    } else if (i + 1 >= argc) {
      return false;
    } else if (strcmp(argument, "--scope") == 0) {
      const char *value = argv[++i];
      if (strcmp(value, "encoder") == 0)
        config->scope = TIMESTAMP_SCOPE_ENCODER;
      else if (strcmp(value, "command-buffer") == 0)
        config->scope = TIMESTAMP_SCOPE_COMMAND_BUFFER;
      else
        return false;
    } else if (strcmp(argument, "--granularity") == 0) {
      const char *value = argv[++i];
      if (strcmp(value, "relaxed") == 0)
        config->granularity = MTL4TimestampGranularityRelaxed;
      else if (strcmp(value, "precise") == 0)
        config->granularity = MTL4TimestampGranularityPrecise;
      else
        return false;
    } else if (strcmp(argument, "--work") == 0) {
      const char *value = argv[++i];
      if (strcmp(value, "empty") == 0)
        config->work = WORK_EMPTY;
      else if (strcmp(value, "fill") == 0)
        config->work = WORK_FILL;
      else if (strcmp(value, "copy") == 0)
        config->work = WORK_COPY;
      else if (strcmp(value, "dispatch") == 0)
        config->work = WORK_DISPATCH;
      else if (strcmp(value, "draw") == 0)
        config->work = WORK_DRAW;
      else if (strcmp(value, "mixed") == 0)
        config->work = WORK_MIXED;
      else if (strcmp(value, "mixed-transfer") == 0)
        config->work = WORK_MIXED_TRANSFER;
      else
        return false;
    } else if (strcmp(argument, "--dependency") == 0) {
      const char *value = argv[++i];
      if (strcmp(value, "none") == 0)
        config->dependency = DEPENDENCY_NONE;
      else if (strcmp(value, "cross") == 0)
        config->dependency = DEPENDENCY_CROSS_ENCODER;
      else if (strcmp(value, "intra") == 0)
        config->dependency = DEPENDENCY_INTRA_ENCODER;
      else
        return false;
    } else if (strcmp(argument, "--residency-mode") == 0) {
      const char *value = argv[++i];
      if (strcmp(value, "none") == 0)
        config->residency = RESIDENCY_NONE;
      else if (strcmp(value, "single") == 0)
        config->residency = RESIDENCY_SINGLE_SET;
      else if (strcmp(value, "multiple") == 0)
        config->residency = RESIDENCY_MULTIPLE_SETS;
      else
        return false;
    } else if (strcmp(argument, "--compute-resource") == 0) {
      const char *value = argv[++i];
      if (strcmp(value, "none") == 0)
        config->compute_resource = COMPUTE_RESOURCE_NONE;
      else if (strcmp(value, "buffer") == 0)
        config->compute_resource = COMPUTE_RESOURCE_BUFFER;
      else
        return false;
    } else if (strcmp(argument, "--resource-allocation") == 0) {
      const char *value = argv[++i];
      if (strcmp(value, "direct") == 0)
        config->resource_allocation = RESOURCE_ALLOCATION_DIRECT;
      else if (strcmp(value, "untracked") == 0)
        config->resource_allocation = RESOURCE_ALLOCATION_DIRECT_UNTRACKED;
      else if (strcmp(value, "placement") == 0)
        config->resource_allocation = RESOURCE_ALLOCATION_PLACEMENT;
      else
        return false;
    } else if (strcmp(argument, "--draw-command") == 0) {
      const char *value = argv[++i];
      if (strcmp(value, "direct") == 0)
        config->draw_command = DRAW_COMMAND_DIRECT;
      else if (strcmp(value, "indirect") == 0)
        config->draw_command = DRAW_COMMAND_INDIRECT_CPU;
      else if (strcmp(value, "gpu-indirect") == 0)
        config->draw_command = DRAW_COMMAND_INDIRECT_GPU;
      else if (strcmp(value, "gpu-indexed") == 0)
        config->draw_command = DRAW_COMMAND_INDIRECT_GPU_INDEXED;
      else
        return false;
    } else if (strcmp(argument, "--icb-range") == 0) {
      const char *value = argv[++i];
      if (strcmp(value, "fixed") == 0)
        config->icb_range = ICB_RANGE_FIXED;
      else if (strcmp(value, "indirect") == 0)
        config->icb_range = ICB_RANGE_INDIRECT;
      else
        return false;
    } else if (strcmp(argument, "--encoders") == 0) {
      if (!parse_u32(argv[++i], 1u, 64u, &config->encoder_count))
        return false;
    } else if (strcmp(argument, "--commands") == 0) {
      if (!parse_u32(argv[++i], 1u, 64u, &config->command_count))
        return false;
    } else if (strcmp(argument, "--transfer-bytes") == 0) {
      if (!parse_u32(argv[++i], 4u, 16u * 1024u * 1024u,
                     &config->transfer_bytes))
        return false;
    } else if (strcmp(argument, "--iterations") == 0) {
      if (!parse_u32(argv[++i], 1u, 100000u, &config->iteration_count))
        return false;
    } else if (strcmp(argument, "--slots") == 0) {
      if (!parse_u32(argv[++i], 1u, 64u, &config->slot_count))
        return false;
    } else if (strcmp(argument, "--timeout-ms") == 0) {
      if (!parse_u32(argv[++i], 1u, 60000u, &config->timeout_ms))
        return false;
    } else {
      return false;
    }
  }
  return config->slot_count <= config->iteration_count &&
         (config->dependency != DEPENDENCY_INTRA_ENCODER ||
          config->command_count >= 2u) &&
         (config->compute_resource == COMPUTE_RESOURCE_NONE ||
          ((config->work == WORK_DISPATCH || config->work == WORK_MIXED ||
            config->work == WORK_MIXED_TRANSFER) &&
           config->residency != RESIDENCY_NONE)) &&
         (config->resource_allocation == RESOURCE_ALLOCATION_DIRECT ||
          (config->residency != RESIDENCY_NONE &&
           (work_uses_render(config->work) ||
            work_uses_transfer(config->work) ||
            config->compute_resource == COMPUTE_RESOURCE_BUFFER ||
            config->icb_range == ICB_RANGE_INDIRECT))) &&
         (config->draw_command == DRAW_COMMAND_DIRECT ||
          (work_uses_render(config->work) &&
           config->residency != RESIDENCY_NONE)) &&
         (config->icb_range == ICB_RANGE_FIXED ||
          draw_command_is_gpu(config->draw_command)) &&
         (config->dependency != DEPENDENCY_INTRA_ENCODER ||
          config->draw_command == DRAW_COMMAND_DIRECT);
}

static const char *work_name(WorkKind work) {
  switch (work) {
  case WORK_EMPTY:
    return "empty";
  case WORK_FILL:
    return "fill";
  case WORK_COPY:
    return "copy";
  case WORK_DISPATCH:
    return "dispatch";
  case WORK_DRAW:
    return "draw";
  case WORK_MIXED:
    return "mixed";
  case WORK_MIXED_TRANSFER:
    return "mixed-transfer";
  }
  return "unknown";
}

static bool work_uses_compute(WorkKind work) {
  return work == WORK_DISPATCH || work == WORK_MIXED ||
         work == WORK_MIXED_TRANSFER;
}

static bool work_uses_render(WorkKind work) {
  return work == WORK_DRAW || work == WORK_MIXED || work == WORK_MIXED_TRANSFER;
}

static bool work_uses_transfer(WorkKind work) {
  return work == WORK_FILL || work == WORK_COPY || work == WORK_MIXED_TRANSFER;
}

static EncoderWorkKind encoder_work(WorkKind work, uint32_t encoder_index) {
  if (work == WORK_DRAW || (work == WORK_MIXED && (encoder_index & 1u) != 0u) ||
      (work == WORK_MIXED_TRANSFER && encoder_index % 3u == 1u))
    return ENCODER_WORK_RENDER;
  if (work == WORK_FILL || work == WORK_COPY ||
      (work == WORK_MIXED_TRANSFER && encoder_index % 3u == 2u))
    return ENCODER_WORK_TRANSFER;
  return ENCODER_WORK_COMPUTE;
}

static const char *dependency_name(DependencyKind dependency) {
  switch (dependency) {
  case DEPENDENCY_NONE:
    return "none";
  case DEPENDENCY_CROSS_ENCODER:
    return "cross";
  case DEPENDENCY_INTRA_ENCODER:
    return "intra";
  }
  return "unknown";
}

static const char *residency_name(ResidencyKind residency) {
  switch (residency) {
  case RESIDENCY_NONE:
    return "none";
  case RESIDENCY_SINGLE_SET:
    return "single";
  case RESIDENCY_MULTIPLE_SETS:
    return "multiple";
  }
  return "unknown";
}

static const char *compute_resource_name(ComputeResourceKind resource) {
  switch (resource) {
  case COMPUTE_RESOURCE_NONE:
    return "none";
  case COMPUTE_RESOURCE_BUFFER:
    return "buffer";
  }
  return "unknown";
}

static const char *resource_allocation_name(ResourceAllocationKind allocation) {
  switch (allocation) {
  case RESOURCE_ALLOCATION_DIRECT:
    return "direct";
  case RESOURCE_ALLOCATION_DIRECT_UNTRACKED:
    return "untracked";
  case RESOURCE_ALLOCATION_PLACEMENT:
    return "placement";
  }
  return "unknown";
}

static const char *draw_command_name(DrawCommandKind command) {
  switch (command) {
  case DRAW_COMMAND_DIRECT:
    return "direct";
  case DRAW_COMMAND_INDIRECT_CPU:
    return "indirect";
  case DRAW_COMMAND_INDIRECT_GPU:
    return "gpu-indirect";
  case DRAW_COMMAND_INDIRECT_GPU_INDEXED:
    return "gpu-indexed";
  }
  return "unknown";
}

static const char *icb_range_name(IcbRangeKind range) {
  switch (range) {
  case ICB_RANGE_FIXED:
    return "fixed";
  case ICB_RANGE_INDIRECT:
    return "indirect";
  }
  return "unknown";
}

static MTLResourceOptions resource_options(ResourceAllocationKind allocation) {
  MTLResourceOptions options = MTLResourceStorageModePrivate;
  if (allocation != RESOURCE_ALLOCATION_DIRECT)
    options |= MTLResourceHazardTrackingModeUntracked;
  return options;
}

static bool reserve_placement(NSUInteger size, NSUInteger alignment,
                              NSUInteger *cursor, NSUInteger *out_offset) {
  if (!cursor || !out_offset || alignment == 0u)
    return false;
  const NSUInteger remainder = *cursor % alignment;
  const NSUInteger padding = remainder == 0u ? 0u : alignment - remainder;
  if (*cursor > SIZE_MAX - padding || *cursor + padding > SIZE_MAX - size)
    return false;
  *out_offset = *cursor + padding;
  *cursor = *out_offset + size;
  return true;
}

static MTLTextureDescriptor *
create_target_descriptor(ResourceAllocationKind allocation) {
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:512u
                                  height:512u
                               mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.hazardTrackingMode = allocation == RESOURCE_ALLOCATION_DIRECT
                                      ? MTLHazardTrackingModeDefault
                                      : MTLHazardTrackingModeUntracked;
  descriptor.usage = MTLTextureUsageRenderTarget;
  return descriptor;
}

static bool create_placement_workload(id<MTLDevice> device,
                                      const DiagnosticConfig *config,
                                      DiagnosticPlacementWorkload *workload) {
  if (config->resource_allocation != RESOURCE_ALLOCATION_PLACEMENT)
    return true;
  NSUInteger required_size = 0u;
  NSUInteger ignored_offset = 0u;
  const MTLResourceOptions options =
      resource_options(config->resource_allocation);
  if (work_uses_transfer(config->work)) {
    const MTLSizeAndAlign size_align =
        [device heapBufferSizeAndAlignWithLength:config->transfer_bytes
                                         options:options];
    const uint64_t destination_count =
        (uint64_t)config->slot_count * config->encoder_count;
    for (uint64_t destination = 0u; destination < destination_count;
         ++destination) {
      if (!reserve_placement(size_align.size, size_align.align, &required_size,
                             &ignored_offset))
        return false;
    }
  }
  if (config->compute_resource == COMPUTE_RESOURCE_BUFFER) {
    const MTLSizeAndAlign size_align =
        [device heapBufferSizeAndAlignWithLength:4096u options:options];
    for (uint32_t slot = 0u; slot < config->slot_count; ++slot) {
      if (!reserve_placement(size_align.size, size_align.align, &required_size,
                             &ignored_offset))
        return false;
    }
  }
  if (work_uses_render(config->work)) {
    MTLTextureDescriptor *descriptor =
        create_target_descriptor(config->resource_allocation);
    const MTLSizeAndAlign size_align =
        [device heapTextureSizeAndAlignWithDescriptor:descriptor];
    const uint64_t target_count =
        (uint64_t)config->slot_count * config->encoder_count;
    for (uint64_t target = 0u; target < target_count; ++target) {
      if (!reserve_placement(size_align.size, size_align.align, &required_size,
                             &ignored_offset))
        return false;
    }
  }
  if (config->icb_range == ICB_RANGE_INDIRECT ||
      config->draw_command == DRAW_COMMAND_INDIRECT_GPU_INDEXED) {
    const MTLSizeAndAlign range_size_align =
        config->icb_range == ICB_RANGE_INDIRECT
            ? [device heapBufferSizeAndAlignWithLength:
                          sizeof(MTLIndirectCommandBufferExecutionRange)
                                               options:options]
            : (MTLSizeAndAlign){0};
    const MTLSizeAndAlign index_size_align =
        config->draw_command == DRAW_COMMAND_INDIRECT_GPU_INDEXED
            ? [device heapBufferSizeAndAlignWithLength:3u * sizeof(uint32_t)
                                               options:options]
            : (MTLSizeAndAlign){0};
    const MTLSizeAndAlign root_size_align =
        config->draw_command == DRAW_COMMAND_INDIRECT_GPU_INDEXED
            ? [device heapBufferSizeAndAlignWithLength:sizeof(uint32_t)
                                               options:options]
            : (MTLSizeAndAlign){0};
    for (uint32_t slot = 0u; slot < config->slot_count; ++slot) {
      if (config->icb_range == ICB_RANGE_INDIRECT &&
          !reserve_placement(range_size_align.size, range_size_align.align,
                             &required_size, &ignored_offset))
        return false;
      if (config->draw_command == DRAW_COMMAND_INDIRECT_GPU_INDEXED) {
        if (!reserve_placement(index_size_align.size, index_size_align.align,
                               &required_size, &ignored_offset) ||
            !reserve_placement(root_size_align.size, root_size_align.align,
                               &required_size, &ignored_offset) ||
            !reserve_placement(root_size_align.size, root_size_align.align,
                               &required_size, &ignored_offset))
          return false;
      }
    }
  }
  if (required_size == 0u)
    return false;
  MTLHeapDescriptor *descriptor = [MTLHeapDescriptor new];
  descriptor.type = MTLHeapTypePlacement;
  descriptor.size = required_size;
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.hazardTrackingMode = MTLHazardTrackingModeUntracked;
  workload->heap = [device newHeapWithDescriptor:descriptor];
  [descriptor release];
  workload->size = required_size;
  return workload->heap != nil;
}

static id<MTLBuffer> create_buffer(id<MTLDevice> device, NSUInteger length,
                                   ResourceAllocationKind allocation,
                                   DiagnosticPlacementWorkload *placement) {
  const MTLResourceOptions options = resource_options(allocation);
  if (allocation != RESOURCE_ALLOCATION_PLACEMENT)
    return [device newBufferWithLength:length options:options];
  const MTLSizeAndAlign size_align =
      [device heapBufferSizeAndAlignWithLength:length options:options];
  NSUInteger offset = 0u;
  if (!placement->heap ||
      !reserve_placement(size_align.size, size_align.align, &placement->cursor,
                         &offset) ||
      placement->cursor > placement->size)
    return nil;
  return [placement->heap newBufferWithLength:length
                                      options:options
                                       offset:offset];
}

static id<MTLTexture> create_target(id<MTLDevice> device,
                                    MTLTextureDescriptor *descriptor,
                                    ResourceAllocationKind allocation,
                                    DiagnosticPlacementWorkload *placement) {
  if (allocation != RESOURCE_ALLOCATION_PLACEMENT)
    return [device newTextureWithDescriptor:descriptor];
  const MTLSizeAndAlign size_align =
      [device heapTextureSizeAndAlignWithDescriptor:descriptor];
  NSUInteger offset = 0u;
  if (!placement->heap ||
      !reserve_placement(size_align.size, size_align.align, &placement->cursor,
                         &offset) ||
      placement->cursor > placement->size)
    return nil;
  return [placement->heap newTextureWithDescriptor:descriptor offset:offset];
}

static bool create_transfer_workload(id<MTLDevice> device, WorkKind work,
                                     ResourceAllocationKind allocation,
                                     uint32_t transfer_bytes,
                                     uint32_t slot_count,
                                     uint32_t encoder_count,
                                     DiagnosticPlacementWorkload *placement,
                                     DiagnosticTransferWorkload *workload) {
  if (!work_uses_transfer(work))
    return true;
  workload->source_count = work == WORK_FILL ? 0u : slot_count;
  workload->destination_count = slot_count * encoder_count;
  if (workload->source_count > 0u)
    workload->sources =
        calloc(workload->source_count, sizeof(*workload->sources));
  workload->destinations =
      calloc(workload->destination_count, sizeof(*workload->destinations));
  if ((workload->source_count > 0u && !workload->sources) ||
      !workload->destinations)
    return false;
  bool valid = true;
  for (uint32_t source = 0u; valid && source < workload->source_count;
       ++source) {
    workload->sources[source] =
        [device newBufferWithLength:transfer_bytes
                            options:MTLResourceStorageModeShared |
                                    MTLResourceCPUCacheModeWriteCombined];
    valid = workload->sources[source] != nil;
    if (valid)
      memset(workload->sources[source].contents, (int)(source + 1u),
             transfer_bytes);
  }
  for (uint32_t destination = 0u;
       valid && destination < workload->destination_count; ++destination) {
    workload->destinations[destination] =
        create_buffer(device, transfer_bytes, allocation, placement);
    valid = workload->destinations[destination] != nil;
  }
  return valid;
}

static void destroy_transfer_workload(DiagnosticTransferWorkload *workload) {
  for (uint32_t source = 0u; source < workload->source_count; ++source)
    [workload->sources[source] release];
  for (uint32_t destination = 0u; destination < workload->destination_count;
       ++destination)
    [workload->destinations[destination] release];
  free(workload->sources);
  free(workload->destinations);
}

static MTLStages encoder_stages(WorkKind work, uint32_t encoder_index) {
  const EncoderWorkKind encoder = encoder_work(work, encoder_index);
  if (encoder == ENCODER_WORK_RENDER)
    return MTLStageFragment;
  return encoder == ENCODER_WORK_TRANSFER ? MTLStageBlit : MTLStageDispatch;
}

static void encode_cross_consumer(id<MTL4CommandEncoder> encoder,
                                  MTLStages producer_stages,
                                  MTLStages consumer_stages) {
  [encoder barrierAfterQueueStages:producer_stages
                      beforeStages:consumer_stages
                 visibilityOptions:MTL4VisibilityOptionDevice];
}

static void encode_cross_producer(id<MTL4CommandEncoder> encoder,
                                  MTLStages producer_stages,
                                  MTLStages consumer_stages) {
  [encoder barrierAfterStages:producer_stages
            beforeQueueStages:consumer_stages
            visibilityOptions:MTL4VisibilityOptionDevice];
}

static void encode_intra_dependency(id<MTL4CommandEncoder> encoder,
                                    MTLStages stages) {
  [encoder barrierAfterEncoderStages:stages
                 beforeEncoderStages:stages
                   visibilityOptions:MTL4VisibilityOptionDevice];
}

static id<MTLLibrary> create_shader_library(id<MTLDevice> device,
                                            id<MTL4Compiler> *out_compiler) {
  static NSString *const source =
      @"#include <metal_stdlib>\n"
       "using namespace metal;\n"
       "kernel void diagnostic_compute(\n"
       "    uint local_index [[thread_index_in_threadgroup]],\n"
       "    uint3 group [[threadgroup_position_in_grid]]) {\n"
       "  threadgroup volatile uint scratch[64];\n"
       "  uint value = local_index ^ (group.x * 747796405u);\n"
       "  for (uint round = 0; round < 16; ++round) {\n"
       "    value = value * 1664525u + 1013904223u;\n"
       "    scratch[local_index] = value;\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "    value ^= scratch[(local_index + 1u) & 63u];\n"
       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
       "  }\n"
       "  scratch[local_index] = value;\n"
       "}\n"
       "kernel void diagnostic_compute_buffer(\n"
       "    device atomic_uint *output [[buffer(0)]],\n"
       "    uint thread_index [[thread_position_in_grid]]) {\n"
       "  uint value = thread_index * 747796405u + 2891336453u;\n"
       "  for (uint round = 0; round < 16; ++round)\n"
       "    value = value * 1664525u + 1013904223u;\n"
       "  atomic_fetch_xor_explicit(\n"
       "      &output[thread_index & 1023u], value, memory_order_relaxed);\n"
       "}\n"
       "struct DiagnosticIcbContainer {\n"
       "  command_buffer commands [[id(0)]];\n"
       "};\n"
       "kernel void diagnostic_encode_icb(\n"
       "    constant DiagnosticIcbContainer *icb [[buffer(0)]],\n"
       "    device uint2 *execution_range [[buffer(1)]],\n"
       "    constant uint *command_count [[buffer(2)]],\n"
       "    uint command_index [[thread_position_in_grid]]) {\n"
       "  if (icb == nullptr)\n"
       "    return;\n"
       "  if (command_index == 0u && execution_range != nullptr &&\n"
       "      command_count != nullptr)\n"
       "    execution_range[0] = uint2(0u, command_count[0]);\n"
       "  render_command command(icb->commands, command_index);\n"
       "  command.draw_primitives(primitive_type::triangle, 0u, 3u, 1u, 0u);\n"
       "}\n"
       "kernel void diagnostic_encode_indexed_icb(\n"
       "    constant DiagnosticIcbContainer *icb [[buffer(0)]],\n"
       "    device uint2 *execution_range [[buffer(1)]],\n"
       "    constant uint *command_count [[buffer(2)]],\n"
       "    device uint *indices [[buffer(3)]],\n"
       "    device uint *vertex_root [[buffer(4)]],\n"
       "    device uint *fragment_root [[buffer(5)]],\n"
       "    uint command_index [[thread_position_in_grid]]) {\n"
       "  if (icb == nullptr || indices == nullptr ||\n"
       "      vertex_root == nullptr || fragment_root == nullptr)\n"
       "    return;\n"
       "  if (command_index == 0u) {\n"
       "    indices[0] = 0u;\n"
       "    indices[1] = 1u;\n"
       "    indices[2] = 2u;\n"
       "    vertex_root[0] = 1u;\n"
       "    fragment_root[0] = 37u;\n"
       "    if (execution_range != nullptr && command_count != nullptr)\n"
       "      execution_range[0] = uint2(0u, command_count[0]);\n"
       "  }\n"
       "  render_command command(icb->commands, command_index);\n"
       "  command.set_vertex_buffer(vertex_root, 0u);\n"
       "  command.set_fragment_buffer(fragment_root, 1u);\n"
       "  command.draw_indexed_primitives(\n"
       "      primitive_type::triangle, 3u, indices, 1u, 0, command_index);\n"
       "}\n"
       "struct DiagnosticVertex {\n"
       "  float4 position [[position]];\n"
       "  float2 uv;\n"
       "};\n"
       "vertex DiagnosticVertex diagnostic_vertex(\n"
       "    uint vertex_id [[vertex_id]]) {\n"
       "  float2 position = vertex_id == 0u ? float2(-1.0, -1.0) :\n"
       "                    vertex_id == 1u ? float2( 3.0, -1.0) :\n"
       "                                      float2(-1.0,  3.0);\n"
       "  return {float4(position, 0.0, 1.0), position * 0.5 + 0.5};\n"
       "}\n"
       "fragment float4 diagnostic_fragment(\n"
       "    DiagnosticVertex input [[stage_in]]) {\n"
       "  float value = input.uv.x * 0.754877666 + input.uv.y;\n"
       "  for (uint round = 0; round < 16; ++round)\n"
       "    value = fract(value * 1.618033989 + float(round) * 0.414213562);\n"
       "  return float4(value, value * value, 1.0 - value, 1.0);\n"
       "}\n"
       "vertex DiagnosticVertex diagnostic_indexed_vertex(\n"
       "    uint vertex_id [[vertex_id]],\n"
       "    constant uint *vertex_root [[buffer(0)]]) {\n"
       "  float2 position = vertex_id == 0u ? float2(-1.0, -1.0) :\n"
       "                    vertex_id == 1u ? float2( 3.0, -1.0) :\n"
       "                                      float2(-1.0,  3.0);\n"
       "  position.x += float(vertex_root[0] & 1u) * 0.0001;\n"
       "  return {float4(position, 0.0, 1.0), position * 0.5 + 0.5};\n"
       "}\n"
       "fragment float4 diagnostic_indexed_fragment(\n"
       "    DiagnosticVertex input [[stage_in]],\n"
       "    constant uint *fragment_root [[buffer(1)]]) {\n"
       "  float value = input.uv.x * 0.754877666 + input.uv.y +\n"
       "                float(fragment_root[0] & 255u) * 0.000001;\n"
       "  for (uint round = 0; round < 16; ++round)\n"
       "    value = fract(value * 1.618033989 + float(round) * 0.414213562);\n"
       "  return float4(value, value * value, 1.0 - value, 1.0);\n"
       "}\n";

  NSError *error = nil;
  MTL4CompilerDescriptor *compiler_descriptor = [MTL4CompilerDescriptor new];
  compiler_descriptor.label = @"VKR timestamp diagnostic";
  id<MTL4Compiler> compiler =
      [device newCompilerWithDescriptor:compiler_descriptor error:&error];
  [compiler_descriptor release];
  if (!compiler) {
    fprintf(stderr, "Metal compiler creation failed: %s\n",
            error.localizedDescription.UTF8String);
    return nil;
  }

  MTL4LibraryDescriptor *library_descriptor = [MTL4LibraryDescriptor new];
  library_descriptor.name = @"VKR timestamp diagnostic shaders";
  library_descriptor.source = source;
  id<MTLLibrary> library = [compiler newLibraryWithDescriptor:library_descriptor
                                                        error:&error];
  [library_descriptor release];
  if (!library) {
    fprintf(stderr, "Metal diagnostic library creation failed: %s\n",
            error.localizedDescription.UTF8String);
    [compiler release];
    return nil;
  }
  *out_compiler = compiler;
  return library;
}

static MTL4LibraryFunctionDescriptor *
create_function_descriptor(id<MTLLibrary> library, NSString *name) {
  MTL4LibraryFunctionDescriptor *function = [MTL4LibraryFunctionDescriptor new];
  function.library = library;
  function.name = name;
  return function;
}

static id<MTLComputePipelineState>
create_compute_pipeline(id<MTLDevice> device, ComputeResourceKind resource) {
  id<MTL4Compiler> compiler = nil;
  id<MTLLibrary> library = create_shader_library(device, &compiler);
  if (!library)
    return nil;
  MTL4LibraryFunctionDescriptor *function =
      create_function_descriptor(library, resource == COMPUTE_RESOURCE_NONE
                                              ? @"diagnostic_compute"
                                              : @"diagnostic_compute_buffer");
  MTL4ComputePipelineDescriptor *pipeline_descriptor =
      [MTL4ComputePipelineDescriptor new];
  pipeline_descriptor.label = @"VKR timestamp diagnostic compute";
  pipeline_descriptor.computeFunctionDescriptor = function;
  NSError *pipeline_error = nil;
  id<MTLComputePipelineState> pipeline =
      [compiler newComputePipelineStateWithDescriptor:pipeline_descriptor
                                  compilerTaskOptions:nil
                                                error:&pipeline_error];
  [pipeline_descriptor release];
  [function release];
  [library release];
  [compiler release];
  if (!pipeline)
    fprintf(stderr, "Metal diagnostic compute pipeline creation failed: %s\n",
            pipeline_error.localizedDescription.UTF8String);
  return pipeline;
}

static id<MTLComputePipelineState>
create_icb_encode_pipeline(id<MTLDevice> device, DrawCommandKind draw_command,
                           id<MTLArgumentEncoder> *out_argument_encoder) {
  id<MTL4Compiler> compiler = nil;
  id<MTLLibrary> library = create_shader_library(device, &compiler);
  if (!library)
    return nil;
  NSString *function_name = draw_command == DRAW_COMMAND_INDIRECT_GPU_INDEXED
                                ? @"diagnostic_encode_indexed_icb"
                                : @"diagnostic_encode_icb";
  id<MTLFunction> legacy_function = [library newFunctionWithName:function_name];
  id<MTLArgumentEncoder> argument_encoder =
      [legacy_function newArgumentEncoderWithBufferIndex:0u];
  MTL4LibraryFunctionDescriptor *function =
      create_function_descriptor(library, function_name);
  MTL4ComputePipelineDescriptor *descriptor =
      [MTL4ComputePipelineDescriptor new];
  descriptor.label = @"VKR timestamp diagnostic ICB encode";
  descriptor.computeFunctionDescriptor = function;
  descriptor.supportIndirectCommandBuffers =
      MTL4IndirectCommandBufferSupportStateEnabled;
  NSError *error = nil;
  id<MTLComputePipelineState> pipeline =
      [compiler newComputePipelineStateWithDescriptor:descriptor
                                  compilerTaskOptions:nil
                                                error:&error];
  [descriptor release];
  [function release];
  [legacy_function release];
  [library release];
  [compiler release];
  if (!pipeline || !argument_encoder) {
    fprintf(stderr, "Metal diagnostic ICB encode pipeline failed: %s\n",
            error.localizedDescription.UTF8String);
    [pipeline release];
    [argument_encoder release];
    return nil;
  }
  *out_argument_encoder = argument_encoder;
  return pipeline;
}

static bool create_compute_workload(id<MTLDevice> device,
                                    ComputeResourceKind resource,
                                    ResourceAllocationKind allocation,
                                    uint32_t slot_count,
                                    DiagnosticPlacementWorkload *placement,
                                    DiagnosticComputeWorkload *workload) {
  workload->pipeline = create_compute_pipeline(device, resource);
  if (!workload->pipeline)
    return false;
  if (resource == COMPUTE_RESOURCE_NONE)
    return true;
  workload->slot_count = slot_count;
  workload->buffers = calloc(slot_count, sizeof(*workload->buffers));
  workload->argument_tables =
      calloc(slot_count, sizeof(*workload->argument_tables));
  if (!workload->buffers || !workload->argument_tables)
    return false;
  MTL4ArgumentTableDescriptor *descriptor = [MTL4ArgumentTableDescriptor new];
  descriptor.label = @"VKR timestamp diagnostic compute resources";
  descriptor.maxBufferBindCount = 1u;
  descriptor.initializeBindings = YES;
  bool valid = true;
  for (uint32_t slot = 0u; valid && slot < slot_count; ++slot) {
    workload->buffers[slot] =
        create_buffer(device, 4096u, allocation, placement);
    workload->argument_tables[slot] =
        [device newArgumentTableWithDescriptor:descriptor error:nil];
    valid = workload->buffers[slot] && workload->argument_tables[slot];
    if (valid)
      [workload->argument_tables[slot]
          setAddress:workload->buffers[slot].gpuAddress
             atIndex:0u];
  }
  [descriptor release];
  return valid;
}

static void destroy_compute_workload(DiagnosticComputeWorkload *workload) {
  for (uint32_t slot = 0u; slot < workload->slot_count; ++slot) {
    if (workload->argument_tables)
      [workload->argument_tables[slot] release];
    if (workload->buffers)
      [workload->buffers[slot] release];
  }
  free(workload->argument_tables);
  free(workload->buffers);
  [workload->pipeline release];
}

static id<MTLRenderPipelineState>
create_render_pipeline(id<MTLDevice> device, DrawCommandKind draw_command) {
  id<MTL4Compiler> compiler = nil;
  id<MTLLibrary> library = create_shader_library(device, &compiler);
  if (!library)
    return nil;
  const bool indexed = draw_command == DRAW_COMMAND_INDIRECT_GPU_INDEXED;
  MTL4LibraryFunctionDescriptor *vertex = create_function_descriptor(
      library, indexed ? @"diagnostic_indexed_vertex" : @"diagnostic_vertex");
  MTL4LibraryFunctionDescriptor *fragment = create_function_descriptor(
      library,
      indexed ? @"diagnostic_indexed_fragment" : @"diagnostic_fragment");
  MTL4RenderPipelineDescriptor *descriptor = [MTL4RenderPipelineDescriptor new];
  descriptor.label = @"VKR timestamp diagnostic render";
  descriptor.vertexFunctionDescriptor = vertex;
  descriptor.fragmentFunctionDescriptor = fragment;
  descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
  descriptor.rasterSampleCount = 1u;
  descriptor.supportIndirectCommandBuffers =
      draw_command == DRAW_COMMAND_DIRECT
          ? MTL4IndirectCommandBufferSupportStateDisabled
          : MTL4IndirectCommandBufferSupportStateEnabled;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
  NSError *error = nil;
  id<MTLRenderPipelineState> pipeline =
      [compiler newRenderPipelineStateWithDescriptor:descriptor
                                 compilerTaskOptions:nil
                                               error:&error];
  [descriptor release];
  [fragment release];
  [vertex release];
  [library release];
  [compiler release];
  if (!pipeline)
    fprintf(stderr, "Metal diagnostic render pipeline creation failed: %s\n",
            error.localizedDescription.UTF8String);
  return pipeline;
}

static bool create_render_workload(id<MTLDevice> device, uint32_t slot_count,
                                   uint32_t encoder_count,
                                   ResourceAllocationKind allocation,
                                   DrawCommandKind draw_command,
                                   DiagnosticPlacementWorkload *placement,
                                   DiagnosticRenderWorkload *workload) {
  workload->pass_count = slot_count * encoder_count;
  workload->pipeline = create_render_pipeline(device, draw_command);
  workload->targets = calloc(workload->pass_count, sizeof(*workload->targets));
  workload->passes = calloc(workload->pass_count, sizeof(*workload->passes));
  bool valid = workload->pipeline && workload->targets && workload->passes;
  MTLTextureDescriptor *target_descriptor =
      create_target_descriptor(allocation);
  for (uint32_t i = 0u; valid && i < workload->pass_count; ++i) {
    workload->targets[i] =
        create_target(device, target_descriptor, allocation, placement);
    workload->passes[i] = [MTL4RenderPassDescriptor new];
    MTLRenderPassColorAttachmentDescriptor *color =
        workload->passes[i].colorAttachments[0];
    color.texture = workload->targets[i];
    color.loadAction = MTLLoadActionClear;
    color.storeAction = MTLStoreActionStore;
    color.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    valid = workload->targets[i] && workload->passes[i];
  }
  return valid;
}

static void destroy_render_workload(DiagnosticRenderWorkload *workload) {
  for (uint32_t i = 0u; i < workload->pass_count; ++i) {
    if (workload->passes)
      [workload->passes[i] release];
    if (workload->targets)
      [workload->targets[i] release];
  }
  free(workload->passes);
  free(workload->targets);
  [workload->pipeline release];
}

static bool create_indirect_workload(
    id<MTLDevice> device, DrawCommandKind draw_command, IcbRangeKind icb_range,
    ResourceAllocationKind allocation, uint32_t slot_count,
    uint32_t command_count, DiagnosticPlacementWorkload *placement,
    DiagnosticIndirectWorkload *workload) {
  if (draw_command == DRAW_COMMAND_DIRECT)
    return true;
  workload->slot_count = slot_count;
  workload->command_count = command_count;
  workload->buffers = calloc(slot_count, sizeof(*workload->buffers));
  workload->residencies = calloc(slot_count, sizeof(*workload->residencies));
  if (draw_command_is_gpu(draw_command)) {
    workload->arguments = calloc(slot_count, sizeof(*workload->arguments));
    workload->argument_tables =
        calloc(slot_count, sizeof(*workload->argument_tables));
    if (icb_range == ICB_RANGE_INDIRECT) {
      workload->range_buffers =
          calloc(slot_count, sizeof(*workload->range_buffers));
      workload->range_configs =
          calloc(slot_count, sizeof(*workload->range_configs));
    }
    if (draw_command == DRAW_COMMAND_INDIRECT_GPU_INDEXED) {
      workload->index_buffers =
          calloc(slot_count, sizeof(*workload->index_buffers));
      workload->vertex_roots =
          calloc(slot_count, sizeof(*workload->vertex_roots));
      workload->fragment_roots =
          calloc(slot_count, sizeof(*workload->fragment_roots));
    }
    workload->encode_pipeline = create_icb_encode_pipeline(
        device, draw_command, &workload->argument_encoder);
  }
  if (!workload->buffers || !workload->residencies ||
      (draw_command_is_gpu(draw_command) &&
       (!workload->arguments || !workload->argument_tables ||
        !workload->encode_pipeline || !workload->argument_encoder)))
    return false;
  if (icb_range == ICB_RANGE_INDIRECT &&
      (!workload->range_buffers || !workload->range_configs))
    return false;
  if (draw_command == DRAW_COMMAND_INDIRECT_GPU_INDEXED &&
      (!workload->index_buffers || !workload->vertex_roots ||
       !workload->fragment_roots))
    return false;
  MTLIndirectCommandBufferDescriptor *descriptor =
      [MTLIndirectCommandBufferDescriptor new];
  const bool indexed = draw_command == DRAW_COMMAND_INDIRECT_GPU_INDEXED;
  descriptor.commandTypes =
      indexed ? MTLIndirectCommandTypeDrawIndexed : MTLIndirectCommandTypeDraw;
  descriptor.inheritPipelineState = YES;
  descriptor.inheritBuffers = indexed ? NO : YES;
  descriptor.inheritDepthStencilState = YES;
  descriptor.inheritCullMode = YES;
  if (indexed) {
    descriptor.maxVertexBufferBindCount = 1u;
    descriptor.maxFragmentBufferBindCount = 3u;
  }
  MTL4ArgumentTableDescriptor *argument_table_descriptor = nil;
  if (draw_command_is_gpu(draw_command)) {
    argument_table_descriptor = [MTL4ArgumentTableDescriptor new];
    argument_table_descriptor.label =
        @"VKR timestamp diagnostic ICB encode resources";
    argument_table_descriptor.maxBufferBindCount =
        indexed                           ? 6u
        : icb_range == ICB_RANGE_INDIRECT ? 3u
                                          : 1u;
    argument_table_descriptor.initializeBindings = YES;
  }
  bool valid = true;
  for (uint32_t slot = 0u; valid && slot < slot_count; ++slot) {
    workload->buffers[slot] = [device
        newIndirectCommandBufferWithDescriptor:descriptor
                               maxCommandCount:command_count
                                       options:MTLResourceStorageModePrivate];
    valid = workload->buffers[slot] != nil;
    if (draw_command == DRAW_COMMAND_INDIRECT_CPU) {
      for (uint32_t command = 0u; valid && command < command_count; ++command) {
        id<MTLIndirectRenderCommand> indirect =
            [workload->buffers[slot] indirectRenderCommandAtIndex:command];
        valid = indirect != nil;
        if (valid)
          [indirect drawPrimitives:MTLPrimitiveTypeTriangle
                       vertexStart:0u
                       vertexCount:3u
                     instanceCount:1u
                      baseInstance:0u];
      }
    } else if (valid) {
      workload->arguments[slot] =
          [device newBufferWithLength:workload->argument_encoder.encodedLength
                              options:MTLResourceStorageModeShared |
                                      MTLResourceCPUCacheModeWriteCombined];
      workload->argument_tables[slot] =
          [device newArgumentTableWithDescriptor:argument_table_descriptor
                                           error:nil];
      valid = workload->arguments[slot] && workload->argument_tables[slot];
      if (valid) {
        [workload->argument_encoder setArgumentBuffer:workload->arguments[slot]
                                               offset:0u];
        [workload->argument_encoder
            setIndirectCommandBuffer:workload->buffers[slot]
                             atIndex:0u];
        [workload->argument_tables[slot]
            setAddress:workload->arguments[slot].gpuAddress
               atIndex:0u];
        if (icb_range == ICB_RANGE_INDIRECT) {
          workload->range_buffers[slot] = create_buffer(
              device, sizeof(MTLIndirectCommandBufferExecutionRange),
              allocation, placement);
          workload->range_configs[slot] =
              [device newBufferWithLength:sizeof(uint32_t)
                                  options:MTLResourceStorageModeShared |
                                          MTLResourceCPUCacheModeWriteCombined];
          valid =
              workload->range_buffers[slot] && workload->range_configs[slot];
          if (valid) {
            *(uint32_t *)workload->range_configs[slot].contents = command_count;
            [workload->argument_tables[slot]
                setAddress:workload->range_buffers[slot].gpuAddress
                   atIndex:1u];
            [workload->argument_tables[slot]
                setAddress:workload->range_configs[slot].gpuAddress
                   atIndex:2u];
          }
        }
        if (valid && indexed) {
          workload->index_buffers[slot] = create_buffer(
              device, 3u * sizeof(uint32_t), allocation, placement);
          workload->vertex_roots[slot] =
              create_buffer(device, sizeof(uint32_t), allocation, placement);
          workload->fragment_roots[slot] =
              create_buffer(device, sizeof(uint32_t), allocation, placement);
          valid = workload->index_buffers[slot] &&
                  workload->vertex_roots[slot] &&
                  workload->fragment_roots[slot];
          if (valid) {
            [workload->argument_tables[slot]
                setAddress:workload->index_buffers[slot].gpuAddress
                   atIndex:3u];
            [workload->argument_tables[slot]
                setAddress:workload->vertex_roots[slot].gpuAddress
                   atIndex:4u];
            [workload->argument_tables[slot]
                setAddress:workload->fragment_roots[slot].gpuAddress
                   atIndex:5u];
          }
        }
      }
    }
    if (!valid)
      break;
    MTLResidencySetDescriptor *residency_descriptor =
        [MTLResidencySetDescriptor new];
    residency_descriptor.label = @"VKR timestamp diagnostic indirect commands";
    residency_descriptor.initialCapacity = 1u;
    if (draw_command_is_gpu(draw_command))
      residency_descriptor.initialCapacity += 1u;
    if (icb_range == ICB_RANGE_INDIRECT)
      residency_descriptor.initialCapacity +=
          allocation == RESOURCE_ALLOCATION_PLACEMENT ? 1u : 2u;
    if (indexed && allocation != RESOURCE_ALLOCATION_PLACEMENT)
      residency_descriptor.initialCapacity += 3u;
    workload->residencies[slot] =
        [device newResidencySetWithDescriptor:residency_descriptor error:nil];
    [residency_descriptor release];
    valid = workload->residencies[slot] != nil;
    if (valid) {
      [workload->residencies[slot]
          addAllocation:(id<MTLAllocation>)workload->buffers[slot]];
      if (workload->arguments)
        [workload->residencies[slot]
            addAllocation:(id<MTLAllocation>)workload->arguments[slot]];
      if (workload->range_buffers &&
          allocation != RESOURCE_ALLOCATION_PLACEMENT)
        [workload->residencies[slot]
            addAllocation:(id<MTLAllocation>)workload->range_buffers[slot]];
      if (workload->range_configs)
        [workload->residencies[slot]
            addAllocation:(id<MTLAllocation>)workload->range_configs[slot]];
      if (workload->index_buffers &&
          allocation != RESOURCE_ALLOCATION_PLACEMENT) {
        [workload->residencies[slot]
            addAllocation:(id<MTLAllocation>)workload->index_buffers[slot]];
        [workload->residencies[slot]
            addAllocation:(id<MTLAllocation>)workload->vertex_roots[slot]];
        [workload->residencies[slot]
            addAllocation:(id<MTLAllocation>)workload->fragment_roots[slot]];
      }
      [workload->residencies[slot] commit];
      [workload->residencies[slot] requestResidency];
    }
  }
  [argument_table_descriptor release];
  [descriptor release];
  return valid;
}

static void destroy_indirect_workload(DiagnosticIndirectWorkload *workload) {
  for (uint32_t slot = 0u; slot < workload->slot_count; ++slot) {
    if (workload->residencies)
      [workload->residencies[slot] endResidency];
    if (workload->residencies)
      [workload->residencies[slot] release];
    if (workload->buffers)
      [workload->buffers[slot] release];
    if (workload->argument_tables)
      [workload->argument_tables[slot] release];
    if (workload->arguments)
      [workload->arguments[slot] release];
    if (workload->range_configs)
      [workload->range_configs[slot] release];
    if (workload->range_buffers)
      [workload->range_buffers[slot] release];
    if (workload->index_buffers)
      [workload->index_buffers[slot] release];
    if (workload->vertex_roots)
      [workload->vertex_roots[slot] release];
    if (workload->fragment_roots)
      [workload->fragment_roots[slot] release];
  }
  [workload->argument_encoder release];
  [workload->encode_pipeline release];
  free(workload->argument_tables);
  free(workload->arguments);
  free(workload->range_configs);
  free(workload->range_buffers);
  free(workload->index_buffers);
  free(workload->vertex_roots);
  free(workload->fragment_roots);
  free(workload->residencies);
  free(workload->buffers);
}

static bool
create_residency_workload(id<MTLDevice> device, ResidencyKind kind,
                          id<MTLHeap> placement_heap,
                          const DiagnosticTransferWorkload *transfer,
                          const DiagnosticComputeWorkload *compute,
                          const DiagnosticRenderWorkload *render,
                          DiagnosticResidencyWorkload *workload) {
  if (kind == RESIDENCY_NONE)
    return true;
  const uint32_t wanted_set_count = kind == RESIDENCY_SINGLE_SET ? 1u : 2u;
  for (uint32_t set_index = 0u; set_index < wanted_set_count; ++set_index) {
    workload->allocations[set_index] =
        [device newBufferWithLength:4096u
                            options:MTLResourceStorageModePrivate];
    if (!workload->allocations[set_index])
      return false;
    NSUInteger target_count = 0u;
    for (uint32_t target = set_index; target < render->pass_count;
         target += wanted_set_count)
      target_count++;
    NSUInteger compute_buffer_count = 0u;
    for (uint32_t slot = set_index; slot < compute->slot_count;
         slot += wanted_set_count)
      compute_buffer_count++;
    NSUInteger transfer_destination_count = 0u;
    for (uint32_t destination = set_index;
         destination < transfer->destination_count;
         destination += wanted_set_count)
      transfer_destination_count++;
    MTLResidencySetDescriptor *descriptor = [MTLResidencySetDescriptor new];
    descriptor.label = set_index == 0u
                           ? @"VKR timestamp diagnostic residency primary"
                           : @"VKR timestamp diagnostic residency secondary";
    const bool placement_set = placement_heap && set_index == 0u;
    descriptor.initialCapacity =
        1u +
        (placement_heap ? (placement_set ? 1u : 0u)
                        : target_count + compute_buffer_count +
                              transfer_destination_count) +
        (set_index == 0u ? transfer->source_count : 0u);
    workload->sets[set_index] = [device newResidencySetWithDescriptor:descriptor
                                                                error:nil];
    [descriptor release];
    if (!workload->sets[set_index])
      return false;
    [workload->sets[set_index]
        addAllocation:(id<MTLAllocation>)workload->allocations[set_index]];
    if (placement_set)
      [workload->sets[set_index]
          addAllocation:(id<MTLAllocation>)placement_heap];
    if (set_index == 0u) {
      for (uint32_t source = 0u; source < transfer->source_count; ++source)
        [workload->sets[set_index]
            addAllocation:(id<MTLAllocation>)transfer->sources[source]];
    }
    if (!placement_heap) {
      for (uint32_t target = set_index; target < render->pass_count;
           target += wanted_set_count)
        [workload->sets[set_index]
            addAllocation:(id<MTLAllocation>)render->targets[target]];
      for (uint32_t slot = set_index; slot < compute->slot_count;
           slot += wanted_set_count)
        [workload->sets[set_index]
            addAllocation:(id<MTLAllocation>)compute->buffers[slot]];
      for (uint32_t destination = set_index;
           destination < transfer->destination_count;
           destination += wanted_set_count)
        [workload->sets[set_index]
            addAllocation:(id<MTLAllocation>)
                              transfer->destinations[destination]];
    }
    [workload->sets[set_index] commit];
    [workload->sets[set_index] requestResidency];
    workload->set_count++;
  }
  return true;
}

static void destroy_residency_workload(DiagnosticResidencyWorkload *workload) {
  for (uint32_t i = 0u; i < workload->set_count; ++i) {
    [workload->sets[i] endResidency];
    [workload->sets[i] release];
  }
  for (uint32_t i = 0u; i < 2u; ++i)
    [workload->allocations[i] release];
}

static id<MTL4CounterHeap> create_counter_heap(id<MTLDevice> device,
                                               uint32_t encoder_count) {
  MTL4CounterHeapDescriptor *descriptor = [MTL4CounterHeapDescriptor new];
  descriptor.type = MTL4CounterHeapTypeTimestamp;
  descriptor.count = (NSUInteger)encoder_count * 2u;
  NSError *error = nil;
  id<MTL4CounterHeap> heap = [device newCounterHeapWithDescriptor:descriptor
                                                            error:&error];
  [descriptor release];
  if (!heap)
    fprintf(stderr, "Counter heap creation failed: %s\n",
            error.localizedDescription.UTF8String);
  return heap;
}

static bool complete_slot(const DiagnosticConfig *config,
                          id<MTLSharedEvent> completion,
                          uint64_t timestamp_frequency, DiagnosticSlot *slot,
                          DiagnosticTotals *totals) {
  if (slot->submit_value == 0u)
    return true;
  if (![completion waitUntilSignaledValue:slot->submit_value
                                timeoutMS:config->timeout_ms]) {
    fprintf(stderr,
            "{\"status\":\"timeout\",\"iteration\":%u,"
            "\"submit_value\":%llu,\"timeout_ms\":%u}\n",
            slot->iteration, (unsigned long long)slot->submit_value,
            config->timeout_ms);
    fflush(NULL);
    _Exit(4);
  }
  const NSUInteger entry_count = (NSUInteger)config->encoder_count * 2u;
  NSData *resolved =
      [slot->counter_heap resolveCounterRange:NSMakeRange(0u, entry_count)];
  if (!resolved ||
      resolved.length < entry_count * sizeof(MTL4TimestampHeapEntry)) {
    fprintf(stderr, "{\"status\":\"resolve_failed\",\"iteration\":%u}\n",
            slot->iteration);
    return false;
  }
  const MTL4TimestampHeapEntry *entries = resolved.bytes;
  for (uint32_t encoder = 0u; encoder < config->encoder_count; ++encoder) {
    const uint64_t begin = entries[encoder * 2u].timestamp;
    const uint64_t end = entries[encoder * 2u + 1u].timestamp;
    const bool duration_valid = begin > 0u && end > begin;
    totals->sample_count++;
    if (duration_valid)
      totals->valid_interval_count++;
    else if (begin > 0u && end == begin)
      totals->zero_interval_count++;
    else
      totals->invalid_interval_count++;
    const uint64_t ticks = end >= begin ? end - begin : 0u;
    const uint64_t duration_ns =
        timestamp_frequency > 0u
            ? (uint64_t)((long double)ticks * 1000000000.0L /
                         (long double)timestamp_frequency)
            : 0u;
    if (!config->summary_only)
      printf("{\"status\":\"sample\",\"iteration\":%u,"
             "\"encoder\":%u,\"begin\":%llu,\"end\":%llu,"
             "\"ticks\":%llu,\"duration_ns\":%llu,"
             "\"duration_valid\":%s}\n",
             slot->iteration, encoder, (unsigned long long)begin,
             (unsigned long long)end, (unsigned long long)ticks,
             (unsigned long long)duration_ns,
             duration_valid ? "true" : "false");
  }
  slot->submit_value = 0u;
  if (!config->reuse_heaps) {
    [slot->counter_heap release];
    slot->counter_heap = nil;
  }
  return true;
}

static void bind_residency_sets(id<MTL4CommandBuffer> command_buffer,
                                const DiagnosticResidencyWorkload *residency,
                                const DiagnosticIndirectWorkload *indirect,
                                uint32_t slot_index) {
  id<MTLResidencySet> active[3] = {nil};
  NSUInteger count = 0u;
  for (uint32_t set = 0u; set < residency->set_count; ++set)
    active[count++] = residency->sets[set];
  if (indirect->residencies)
    active[count++] = indirect->residencies[slot_index];
  if (count == 1u)
    [command_buffer useResidencySet:active[0]];
  else if (count > 1u)
    [command_buffer useResidencySets:active count:count];
}

static bool
encode_gpu_indirect_commands(id<MTL4CommandBuffer> command_buffer,
                             const DiagnosticIndirectWorkload *indirect,
                             uint32_t slot_index) {
  id<MTL4ComputeCommandEncoder> encoder =
      [command_buffer computeCommandEncoder];
  if (!encoder)
    return false;
  [encoder resetCommandsInBuffer:indirect->buffers[slot_index]
                       withRange:NSMakeRange(0u, indirect->command_count)];
  [encoder setComputePipelineState:indirect->encode_pipeline];
  [encoder setArgumentTable:indirect->argument_tables[slot_index]];
  [encoder dispatchThreads:MTLSizeMake(indirect->command_count, 1u, 1u)
      threadsPerThreadgroup:MTLSizeMake(indirect->command_count, 1u, 1u)];
  [encoder barrierAfterStages:MTLStageDispatch
            beforeQueueStages:MTLStageVertex | MTLStageFragment
            visibilityOptions:MTL4VisibilityOptionDevice];
  [encoder endEncoding];
  return true;
}

static bool encode_render_work(const DiagnosticConfig *config,
                               DiagnosticSlot *slot,
                               const DiagnosticRenderWorkload *render,
                               const DiagnosticIndirectWorkload *indirect,
                               uint32_t slot_index, uint32_t encoder_index,
                               bool *gpu_icb_wait_pending) {
  const NSUInteger begin_index = (NSUInteger)encoder_index * 2u;
  const MTLStages stages = encoder_stages(config->work, encoder_index);
  const uint32_t render_pass_index =
      slot_index * config->encoder_count + encoder_index;
  id<MTL4RenderCommandEncoder> encoder = [slot->command_buffer
      renderCommandEncoderWithDescriptor:render->passes[render_pass_index]];
  if (!encoder)
    return false;
  if (config->scope == TIMESTAMP_SCOPE_ENCODER)
    [encoder writeTimestampWithGranularity:config->granularity
                                afterStage:MTLRenderStageFragment
                                  intoHeap:slot->counter_heap
                                   atIndex:begin_index];
  if (*gpu_icb_wait_pending) {
    [encoder barrierAfterQueueStages:MTLStageDispatch
                        beforeStages:MTLStageVertex | MTLStageFragment
                   visibilityOptions:MTL4VisibilityOptionDevice];
    *gpu_icb_wait_pending = false;
  }
  if (config->dependency == DEPENDENCY_CROSS_ENCODER && encoder_index > 0u)
    encode_cross_consumer(
        encoder, encoder_stages(config->work, encoder_index - 1u), stages);
  [encoder setViewport:(MTLViewport){0.0, 0.0, 512.0, 512.0, 0.0, 1.0}];
  [encoder setRenderPipelineState:render->pipeline];
  if (config->draw_command == DRAW_COMMAND_DIRECT) {
    for (uint32_t command = 0u; command < config->command_count; ++command) {
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                  vertexStart:0u
                  vertexCount:3u];
      if (config->dependency == DEPENDENCY_INTRA_ENCODER &&
          command + 1u == config->command_count / 2u)
        encode_intra_dependency(encoder, stages);
    }
  } else if (config->icb_range == ICB_RANGE_INDIRECT) {
    [encoder
        executeCommandsInBuffer:indirect->buffers[slot_index]
                 indirectBuffer:indirect->range_buffers[slot_index].gpuAddress];
  } else {
    [encoder executeCommandsInBuffer:indirect->buffers[slot_index]
                           withRange:NSMakeRange(0u, indirect->command_count)];
  }
  if (config->dependency == DEPENDENCY_CROSS_ENCODER &&
      encoder_index + 1u < config->encoder_count)
    encode_cross_producer(encoder, stages,
                          encoder_stages(config->work, encoder_index + 1u));
  if (config->scope == TIMESTAMP_SCOPE_ENCODER)
    [encoder writeTimestampWithGranularity:config->granularity
                                afterStage:MTLRenderStageFragment
                                  intoHeap:slot->counter_heap
                                   atIndex:begin_index + 1u];
  [encoder endEncoding];
  return true;
}

static bool encode_compute_or_transfer_work(
    const DiagnosticConfig *config, DiagnosticSlot *slot,
    const DiagnosticComputeWorkload *compute,
    const DiagnosticTransferWorkload *transfer, uint32_t iteration,
    uint32_t slot_index, uint32_t encoder_index, EncoderWorkKind operation) {
  const NSUInteger begin_index = (NSUInteger)encoder_index * 2u;
  const MTLStages stages = encoder_stages(config->work, encoder_index);
  id<MTL4ComputeCommandEncoder> encoder =
      [slot->command_buffer computeCommandEncoder];
  if (!encoder)
    return false;
  if (config->scope == TIMESTAMP_SCOPE_ENCODER)
    [encoder writeTimestampWithGranularity:config->granularity
                                  intoHeap:slot->counter_heap
                                   atIndex:begin_index];
  if (config->dependency == DEPENDENCY_CROSS_ENCODER && encoder_index > 0u)
    encode_cross_consumer(
        encoder, encoder_stages(config->work, encoder_index - 1u), stages);
  if (operation == ENCODER_WORK_COMPUTE && compute->pipeline) {
    [encoder setComputePipelineState:compute->pipeline];
    if (compute->argument_tables)
      [encoder setArgumentTable:compute->argument_tables[slot_index]];
  }
  const uint32_t destination_index =
      slot_index * config->encoder_count + encoder_index;
  for (uint32_t command = 0u; command < config->command_count; ++command) {
    if (operation == ENCODER_WORK_TRANSFER && config->work == WORK_FILL)
      [encoder fillBuffer:transfer->destinations[destination_index]
                    range:NSMakeRange(0u, config->transfer_bytes)
                    value:(uint8_t)(iteration + command)];
    if (operation == ENCODER_WORK_TRANSFER && config->work != WORK_FILL)
      [encoder copyFromBuffer:transfer->sources[slot_index]
                 sourceOffset:0u
                     toBuffer:transfer->destinations[destination_index]
            destinationOffset:0u
                         size:config->transfer_bytes];
    if (operation == ENCODER_WORK_COMPUTE && compute->pipeline)
      [encoder dispatchThreadgroups:MTLSizeMake(256u, 1u, 1u)
              threadsPerThreadgroup:MTLSizeMake(64u, 1u, 1u)];
    if (config->dependency == DEPENDENCY_INTRA_ENCODER &&
        command + 1u == config->command_count / 2u)
      encode_intra_dependency(encoder, stages);
  }
  if (config->dependency == DEPENDENCY_CROSS_ENCODER &&
      encoder_index + 1u < config->encoder_count)
    encode_cross_producer(encoder, stages,
                          encoder_stages(config->work, encoder_index + 1u));
  if (config->scope == TIMESTAMP_SCOPE_ENCODER)
    [encoder writeTimestampWithGranularity:config->granularity
                                  intoHeap:slot->counter_heap
                                   atIndex:begin_index + 1u];
  [encoder endEncoding];
  return true;
}

static int run_diagnostic(const DiagnosticConfig *config) {
  if (!@available(macOS 26.0, *)) {
    fprintf(stderr, "Metal 4 timestamp diagnostics require macOS 26.\n");
    return 3;
  }
  id<MTLDevice> device = [MTLCreateSystemDefaultDevice() retain];
  if (!device || ![device supportsFamily:MTLGPUFamilyMetal4]) {
    fprintf(stderr, "A Metal 4 device is unavailable.\n");
    [device release];
    return 3;
  }
  id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
  id<MTLSharedEvent> completion = [device newSharedEvent];
  DiagnosticSlot *slots = calloc(config->slot_count, sizeof(*slots));
  bool setup = queue && completion && slots;
  for (uint32_t i = 0u; setup && i < config->slot_count; ++i) {
    slots[i].allocator = [device newCommandAllocator];
    slots[i].command_buffer = [device newCommandBuffer];
    if (config->reuse_heaps)
      slots[i].counter_heap =
          create_counter_heap(device, config->encoder_count);
    setup = slots[i].allocator && slots[i].command_buffer &&
            (!config->reuse_heaps || slots[i].counter_heap);
  }

  DiagnosticTransferWorkload transfer = {0};
  DiagnosticComputeWorkload compute = {0};
  DiagnosticRenderWorkload render = {0};
  DiagnosticIndirectWorkload indirect = {0};
  DiagnosticResidencyWorkload residency = {0};
  DiagnosticPlacementWorkload placement = {0};
  if (setup)
    setup = create_placement_workload(device, config, &placement);
  if (setup && work_uses_transfer(config->work))
    setup = create_transfer_workload(
        device, config->work, config->resource_allocation,
        config->transfer_bytes, config->slot_count, config->encoder_count,
        &placement, &transfer);
  if (setup && work_uses_compute(config->work))
    setup = create_compute_workload(device, config->compute_resource,
                                    config->resource_allocation,
                                    config->slot_count, &placement, &compute);
  if (setup && work_uses_render(config->work))
    setup = create_render_workload(
        device, config->slot_count, config->encoder_count,
        config->resource_allocation, config->draw_command, &placement, &render);
  if (setup)
    setup = create_indirect_workload(
        device, config->draw_command, config->icb_range,
        config->resource_allocation, config->slot_count, config->command_count,
        &placement, &indirect);
  if (setup)
    setup = create_residency_workload(device, config->residency, placement.heap,
                                      &transfer, &compute, &render, &residency);
  if (!setup) {
    fprintf(stderr, "Metal timestamp diagnostic setup failed.\n");
  } else {
    printf(
        "{\"status\":\"start\",\"scope\":\"%s\","
        "\"granularity\":\"%s\",\"work\":\"%s\","
        "\"dependency\":\"%s\",\"encoders\":%u,\"commands\":%u,"
        "\"transfer_bytes\":%u,\"iterations\":%u,\"slots\":%u,"
        "\"reuse_heaps\":%s,\"residency\":\"%s\","
        "\"compute_resource\":\"%s\","
        "\"resource_allocation\":\"%s\","
        "\"draw_command\":\"%s\","
        "\"icb_range\":\"%s\","
        "\"placement_bytes\":%llu}\n",
        config->scope == TIMESTAMP_SCOPE_ENCODER ? "encoder" : "command-buffer",
        config->granularity == MTL4TimestampGranularityRelaxed ? "relaxed"
                                                               : "precise",
        work_name(config->work), dependency_name(config->dependency),
        config->encoder_count, config->command_count, config->transfer_bytes,
        config->iteration_count, config->slot_count,
        config->reuse_heaps ? "true" : "false",
        residency_name(config->residency),
        compute_resource_name(config->compute_resource),
        resource_allocation_name(config->resource_allocation),
        draw_command_name(config->draw_command),
        icb_range_name(config->icb_range), (unsigned long long)placement.size);
  }

  bool valid = setup;
  DiagnosticTotals totals = {0};
  uint64_t next_submit_value = 0u;
  const uint64_t timestamp_frequency = [device queryTimestampFrequency];
  for (uint32_t iteration = 0u; valid && iteration < config->iteration_count;
       ++iteration) {
    @autoreleasepool {
      DiagnosticSlot *slot = &slots[iteration % config->slot_count];
      valid =
          complete_slot(config, completion, timestamp_frequency, slot, &totals);
      if (!valid)
        continue;
      [slot->allocator reset];
      if (!slot->counter_heap)
        slot->counter_heap = create_counter_heap(device, config->encoder_count);
      valid = slot->counter_heap != nil;
      if (!valid)
        continue;
      [slot->counter_heap
          invalidateCounterRange:NSMakeRange(0u,
                                             (NSUInteger)config->encoder_count *
                                                 2u)];
      [slot->command_buffer beginCommandBufferWithAllocator:slot->allocator];
      const uint32_t slot_index = iteration % config->slot_count;
      bind_residency_sets(slot->command_buffer, &residency, &indirect,
                          slot_index);
      bool gpu_icb_wait_pending = false;
      if (draw_command_is_gpu(config->draw_command)) {
        valid = encode_gpu_indirect_commands(slot->command_buffer, &indirect,
                                             slot_index);
        gpu_icb_wait_pending = valid;
      }
      for (uint32_t encoder_index = 0u;
           valid && encoder_index < config->encoder_count; ++encoder_index) {
        const NSUInteger begin_index = (NSUInteger)encoder_index * 2u;
        const EncoderWorkKind operation =
            encoder_work(config->work, encoder_index);
        if (config->scope == TIMESTAMP_SCOPE_COMMAND_BUFFER)
          [slot->command_buffer writeTimestampIntoHeap:slot->counter_heap
                                               atIndex:begin_index];
        valid = operation == ENCODER_WORK_RENDER
                    ? encode_render_work(config, slot, &render, &indirect,
                                         slot_index, encoder_index,
                                         &gpu_icb_wait_pending)
                    : encode_compute_or_transfer_work(
                          config, slot, &compute, &transfer, iteration,
                          slot_index, encoder_index, operation);
        if (config->scope == TIMESTAMP_SCOPE_COMMAND_BUFFER)
          [slot->command_buffer writeTimestampIntoHeap:slot->counter_heap
                                               atIndex:begin_index + 1u];
      }
      [slot->command_buffer endCommandBuffer];
      if (!valid)
        continue;
      id<MTL4CommandBuffer> submissions[] = {slot->command_buffer};
      [queue commit:submissions count:1u];
      slot->submit_value = ++next_submit_value;
      slot->iteration = iteration;
      [queue signalEvent:completion value:slot->submit_value];
    }
  }
  for (uint32_t i = 0u; valid && i < config->slot_count; ++i)
    valid = complete_slot(config, completion, timestamp_frequency, &slots[i],
                          &totals);

  destroy_residency_workload(&residency);
  destroy_indirect_workload(&indirect);
  destroy_render_workload(&render);
  destroy_compute_workload(&compute);
  destroy_transfer_workload(&transfer);
  [placement.heap release];
  if (slots) {
    for (uint32_t i = 0u; i < config->slot_count; ++i) {
      [slots[i].counter_heap release];
      [slots[i].command_buffer release];
      [slots[i].allocator release];
    }
  }
  free(slots);
  [completion release];
  [queue release];
  [device release];
  const uint64_t expected_samples =
      (uint64_t)config->encoder_count * config->iteration_count;
  const bool samples_valid = totals.sample_count == expected_samples &&
                             totals.valid_interval_count == expected_samples;
  valid = valid && samples_valid;
  printf("{\"status\":\"%s\",\"samples\":%llu,"
         "\"valid_intervals\":%llu,\"zero_intervals\":%llu,"
         "\"invalid_intervals\":%llu}\n",
         valid ? "pass" : "invalid_sample",
         (unsigned long long)totals.sample_count,
         (unsigned long long)totals.valid_interval_count,
         (unsigned long long)totals.zero_interval_count,
         (unsigned long long)totals.invalid_interval_count);
  return valid ? 0 : (setup ? 5 : 3);
}

int main(int argc, const char *argv[]) {
  DiagnosticConfig config;
  if (!parse_config(argc, argv, &config)) {
    usage(argv[0]);
    return 2;
  }
  @autoreleasepool {
    return run_diagnostic(&config);
  }
}
