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
} WorkKind;

typedef struct DiagnosticConfig {
  TimestampScope scope;
  MTL4TimestampGranularity granularity;
  WorkKind work;
  uint32_t encoder_count;
  uint32_t iteration_count;
  uint32_t slot_count;
  uint32_t timeout_ms;
  bool reuse_heaps;
  bool residency;
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

static void usage(const char *program) {
  fprintf(stderr,
          "Usage: %s [--scope encoder|command-buffer] "
          "[--granularity relaxed|precise] [--work empty|fill] "
          "[--encoders N] [--iterations N] [--slots N] "
          "[--timeout-ms N] [--reuse-heaps] [--residency]\n",
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
      .iteration_count = 1u,
      .slot_count = 1u,
      .timeout_ms = 5000u,
  };
  for (int i = 1; i < argc; ++i) {
    const char *argument = argv[i];
    if (strcmp(argument, "--reuse-heaps") == 0) {
      config->reuse_heaps = true;
    } else if (strcmp(argument, "--residency") == 0) {
      config->residency = true;
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
      else
        return false;
    } else if (strcmp(argument, "--encoders") == 0) {
      if (!parse_u32(argv[++i], 1u, 64u, &config->encoder_count))
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
  return config->slot_count <= config->iteration_count;
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
    printf("{\"status\":\"sample\",\"iteration\":%u,"
           "\"encoder\":%u,\"begin\":%llu,\"end\":%llu,"
           "\"ticks\":%llu,\"duration_ns\":%llu,"
           "\"duration_valid\":%s}\n",
           slot->iteration, encoder, (unsigned long long)begin,
           (unsigned long long)end, (unsigned long long)ticks,
           (unsigned long long)duration_ns, duration_valid ? "true" : "false");
  }
  slot->submit_value = 0u;
  if (!config->reuse_heaps) {
    [slot->counter_heap release];
    slot->counter_heap = nil;
  }
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

  id<MTLBuffer> fill_buffer = nil;
  id<MTLResidencySet> residency_set = nil;
  if (setup && config->work == WORK_FILL) {
    fill_buffer = [device newBufferWithLength:4096u
                                      options:MTLResourceStorageModePrivate];
    setup = fill_buffer != nil;
  }
  if (setup && config->residency) {
    MTLResidencySetDescriptor *descriptor = [MTLResidencySetDescriptor new];
    descriptor.label = @"VKR timestamp diagnostic";
    descriptor.initialCapacity = fill_buffer ? 1u : 0u;
    residency_set = [device newResidencySetWithDescriptor:descriptor error:nil];
    [descriptor release];
    if (residency_set && fill_buffer) {
      [residency_set addAllocation:(id<MTLAllocation>)fill_buffer];
      [residency_set commit];
      [residency_set requestResidency];
    }
    setup = residency_set != nil;
  }
  if (!setup) {
    fprintf(stderr, "Metal timestamp diagnostic setup failed.\n");
  } else {
    printf("{\"status\":\"start\",\"scope\":\"%s\","
           "\"granularity\":\"%s\",\"work\":\"%s\","
           "\"encoders\":%u,\"iterations\":%u,\"slots\":%u,"
           "\"reuse_heaps\":%s,\"residency\":%s}\n",
           config->scope == TIMESTAMP_SCOPE_ENCODER ? "encoder"
                                                    : "command-buffer",
           config->granularity == MTL4TimestampGranularityRelaxed ? "relaxed"
                                                                  : "precise",
           config->work == WORK_EMPTY ? "empty" : "fill", config->encoder_count,
           config->iteration_count, config->slot_count,
           config->reuse_heaps ? "true" : "false",
           config->residency ? "true" : "false");
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
      if (residency_set)
        [slot->command_buffer useResidencySet:residency_set];
      for (uint32_t encoder_index = 0u;
           valid && encoder_index < config->encoder_count; ++encoder_index) {
        const NSUInteger begin_index = (NSUInteger)encoder_index * 2u;
        if (config->scope == TIMESTAMP_SCOPE_COMMAND_BUFFER)
          [slot->command_buffer writeTimestampIntoHeap:slot->counter_heap
                                               atIndex:begin_index];
        id<MTL4ComputeCommandEncoder> encoder =
            [slot->command_buffer computeCommandEncoder];
        valid = encoder != nil;
        if (!valid)
          continue;
        if (config->scope == TIMESTAMP_SCOPE_ENCODER)
          [encoder writeTimestampWithGranularity:config->granularity
                                        intoHeap:slot->counter_heap
                                         atIndex:begin_index];
        if (fill_buffer)
          [encoder fillBuffer:fill_buffer
                        range:NSMakeRange(0u, fill_buffer.length)
                        value:(uint8_t)iteration];
        if (config->scope == TIMESTAMP_SCOPE_ENCODER)
          [encoder writeTimestampWithGranularity:config->granularity
                                        intoHeap:slot->counter_heap
                                         atIndex:begin_index + 1u];
        [encoder endEncoding];
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

  if (residency_set)
    [residency_set endResidency];
  [residency_set release];
  [fill_buffer release];
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
