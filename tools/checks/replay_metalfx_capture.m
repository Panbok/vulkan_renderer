#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Standalone diagnostic: one owner, one submission at a time, no renderer
// state.
static void fail(NSString *message) {
  fprintf(stderr, "%s\n", message.UTF8String);
  exit(1);
}
static NSUInteger dimension(NSDictionary *m, NSString *key) {
  double n = [m[key] doubleValue];
  if (!isfinite(n) || n < 1 || n > 16384 || n != floor(n))
    fail([@"Invalid dimension: " stringByAppendingString:key]);
  return (NSUInteger)n;
}
static id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format,
                              NSUInteger w, NSUInteger h,
                              MTLTextureUsage usage) {
  MTLTextureDescriptor *d =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                         width:w
                                                        height:h
                                                     mipmapped:NO];
  d.storageMode = MTLStorageModePrivate;
  d.hazardTrackingMode = MTLHazardTrackingModeUntracked;
  d.usage = usage;
  id<MTLTexture> result = [device newTextureWithDescriptor:d];
  if (!result)
    fail(@"Texture allocation failed");
  return result;
}
static NSData *input(NSDictionary *frame, NSString *key, NSString *base,
                     NSUInteger size) {
  NSString *path = frame[key];
  if (![path isKindOfClass:NSString.class])
    fail(@"Missing input path");
  if (!path.isAbsolutePath)
    path = [base stringByAppendingPathComponent:path];
  NSError *error = nil;
  NSData *data = [NSData dataWithContentsOfFile:path options:0 error:&error];
  if (!data || data.length != size)
    fail([NSString
        stringWithFormat:@"Input %@: expected %lu bytes, got %lu (%@)", path,
                         (unsigned long)size, (unsigned long)data.length,
                         error]);
  return data;
}
static void upload(id<MTL4ComputeCommandEncoder> e, id<MTLBuffer> b,
                   id<MTLTexture> t, NSUInteger pitch) {
  [e copyFromBuffer:b
             sourceOffset:0
        sourceBytesPerRow:pitch
      sourceBytesPerImage:pitch * t.height
               sourceSize:MTLSizeMake(t.width, t.height, 1)
                toTexture:t
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
}
int main(int argc, const char **argv) {
  @autoreleasepool {
    if (argc != 5) {
      fprintf(stderr,
              "usage: %s manifest.json NEW_OUTPUT_DIR PADDING_RGB auto|fixed\n",
              argv[0]);
      return 2;
    }
    for (NSString *key in @[ @"MTL_DEBUG_LAYER", @"MTL_SHADER_VALIDATION" ])
      if (getenv(key.UTF8String))
        fail(@"Run with Metal validation variables unset");
    char *end = NULL;
    float padding = strtof(argv[3], &end);
    if (*end || end == argv[3] || !isfinite(padding) || padding < 0 ||
        padding > 65504)
      fail(@"Padding must be a finite nonnegative half-float value");
    bool automatic = strcmp(argv[4], "auto") == 0;
    if (!automatic && strcmp(argv[4], "fixed"))
      fail(@"Exposure mode: auto or fixed");
    NSString *manifestPath = [NSString stringWithUTF8String:argv[1]];
    if (!manifestPath.isAbsolutePath)
      manifestPath = [NSFileManager.defaultManager.currentDirectoryPath
          stringByAppendingPathComponent:manifestPath];
    NSString *base = manifestPath.stringByDeletingLastPathComponent;
    NSError *error = nil;
    NSData *manifestBytes = [NSData dataWithContentsOfFile:manifestPath];
    if (!manifestBytes)
      fail(@"Cannot read manifest");
    NSDictionary *m = [NSJSONSerialization JSONObjectWithData:manifestBytes
                                                      options:0
                                                        error:&error];
    if (![m isKindOfClass:NSDictionary.class])
      fail(@"Invalid manifest JSON");
    NSUInteger w = dimension(m, @"width"), h = dimension(m, @"height");
    NSUInteger ow = dimension(m, @"output_width"),
               oh = dimension(m, @"output_height");
    if (w > ow || h > oh)
      fail(@"Active content exceeds output-sized input allocation");
    NSArray *frames = m[@"frames"];
    if (![frames isKindOfClass:NSArray.class] || frames.count != 8)
      fail(@"Manifest needs exactly eight ordered phases");
    NSUInteger warmup =
        m[@"warmup_frames"] ? [m[@"warmup_frames"] unsignedIntegerValue] : 128;
    if (warmup < 128 || warmup > 8192 || warmup % 8)
      fail(@"warmup_frames must be a multiple of8 in [128,8192]");
    float fixed = m[@"fixed_exposure"] ? [m[@"fixed_exposure"] floatValue] : 1;
    if (!isfinite(fixed) || fixed <= 0 || fixed > 65504)
      fail(@"Invalid fixed_exposure");
    NSMutableArray *colors = [NSMutableArray new],
                   *depths = [NSMutableArray new],
                   *motions = [NSMutableArray new];
    for (NSDictionary *f in frames) {
      if (![f isKindOfClass:NSDictionary.class])
        fail(@"Invalid frame record");
      NSArray *j = f[@"jitter"];
      if (![j isKindOfClass:NSArray.class] || j.count != 2 ||
          !isfinite([j[0] floatValue]) || !isfinite([j[1] floatValue]))
        fail(@"Invalid jitter");
      [colors addObject:input(f, @"color", base, w * h * 8)];
      [depths addObject:input(f, @"depth", base, w * h * 4)];
      [motions addObject:input(f, @"motion", base, w * h * 4)];
    }
    NSString *out = [NSString stringWithUTF8String:argv[2]];
    if ([NSFileManager.defaultManager fileExistsAtPath:out])
      fail(@"Output directory already exists");
    if (![NSFileManager.defaultManager createDirectoryAtPath:out
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:&error])
      fail(error.description);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || ![MTLFXTemporalScalerDescriptor supportsMetal4FX:device])
      fail(@"Metal4FX unsupported");
    MTL4CompilerDescriptor *cd = [MTL4CompilerDescriptor new];
    id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:cd
                                                            error:&error];
    if (!compiler)
      fail(error.description);
    MTLFXTemporalScalerDescriptor *sd = [MTLFXTemporalScalerDescriptor new];
    sd.inputWidth = ow;
    sd.inputHeight = oh;
    sd.outputWidth = ow;
    sd.outputHeight = oh;
    sd.colorTextureFormat = MTLPixelFormatRGBA16Float;
    sd.depthTextureFormat = MTLPixelFormatDepth32Float;
    sd.motionTextureFormat = MTLPixelFormatRG16Float;
    sd.outputTextureFormat = MTLPixelFormatRGBA16Float;
    sd.autoExposureEnabled = automatic;
    sd.requiresSynchronousInitialization = YES;
    sd.inputContentPropertiesEnabled = YES;
    sd.inputContentMinScale = fminf((float)ow / w, (float)oh / h);
    sd.inputContentMaxScale = fmaxf((float)ow / w, (float)oh / h);
    sd.reactiveMaskTextureEnabled = NO;
    id<MTL4FXTemporalScaler> scaler = [sd newTemporalScalerWithDevice:device
                                                             compiler:compiler];
    if (!scaler)
      fail(@"MTL4FXTemporalScaler creation failed");
    id<MTLFence> fence = [device newFence];
    if (!fence)
      fail(@"Fence creation failed");
    scaler.fence = fence;
    id<MTLTexture> color = texture(device, sd.colorTextureFormat, ow, oh,
                                   scaler.colorTextureUsage);
    id<MTLTexture> depth = texture(device, sd.depthTextureFormat, ow, oh,
                                   scaler.depthTextureUsage);
    id<MTLTexture> motion = texture(device, sd.motionTextureFormat, ow, oh,
                                    scaler.motionTextureUsage);
    id<MTLTexture> output = texture(device, sd.outputTextureFormat, ow, oh,
                                    scaler.outputTextureUsage);
    id<MTLTexture> exposure = texture(device, MTLPixelFormatR16Float, 1, 1,
                                      MTLTextureUsageShaderRead);
    NSUInteger cp = (ow * 8 + 255) & ~(NSUInteger)255,
               dp = (ow * 4 + 255) & ~(NSUInteger)255;
    id<MTLBuffer> cb =
        [device newBufferWithLength:cp * oh
                            options:MTLResourceStorageModeShared];
    id<MTLBuffer> db =
        [device newBufferWithLength:dp * oh
                            options:MTLResourceStorageModeShared];
    id<MTLBuffer> mb =
        [device newBufferWithLength:dp * oh
                            options:MTLResourceStorageModeShared];
    id<MTLBuffer> eb =
        [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> rb =
        [device newBufferWithLength:cp * oh
                            options:MTLResourceStorageModeShared];
    if (!cb || !db || !mb || !eb || !rb)
      fail(@"Buffer allocation failed");
    memset(cb.contents, 0, cb.length);
    memset(db.contents, 0, db.length);
    memset(mb.contents, 0, mb.length);
    memset(eb.contents, 0, eb.length);
    *(_Float16 *)eb.contents = (_Float16)fixed;
    for (NSUInteger y = 0; y < oh; y++)
      for (NSUInteger x = 0; x < ow; x++) {
        _Float16 *p = (_Float16 *)((uint8_t *)cb.contents + y * cp) + x * 4;
        p[0] = p[1] = p[2] = (_Float16)padding;
        p[3] = 1;
        ((float *)((uint8_t *)db.contents + y * dp))[x] = 1;
      }
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
    rd.initialCapacity = 10;
    id<MTLResidencySet> residency =
        [device newResidencySetWithDescriptor:rd error:&error];
    if (!residency)
      fail(error.description);
    for (id<MTLAllocation> resource in
         @[ color, depth, motion, output, exposure, cb, db, mb, eb, rb ])
      [residency addAllocation:resource];
    [residency commit];
    MTL4CommandQueueDescriptor *qd = [MTL4CommandQueueDescriptor new];
    // Leave feedbackQueue nil: Metal owns its internal serial queue. A borrowed
    // custom queue must outlive the descriptor as well as the command queue.
    id<MTL4CommandQueue> queue =
        [device newMTL4CommandQueueWithDescriptor:qd error:&error];
    id<MTL4CommandAllocator> allocator = [device newCommandAllocator];
    id<MTL4CommandBuffer> commands = [device newCommandBuffer];
    id<MTLSharedEvent> completion = [device newSharedEvent];
    if (!queue || !allocator || !commands || !completion)
      fail(@"Command setup failed");
    [queue addResidencySet:residency];
    scaler.colorTexture = color;
    scaler.depthTexture = depth;
    scaler.motionTexture = motion;
    scaler.outputTexture = output;
    scaler.exposureTexture = automatic ? nil : exposure;
    scaler.inputContentWidth = w;
    scaler.inputContentHeight = h;
    scaler.preExposure = 1;
    scaler.motionVectorScaleX = w;
    scaler.motionVectorScaleY = h;
    scaler.depthReversed = NO;
    NSMutableArray *written = [NSMutableArray new];
    fprintf(stderr,
            "Metal4FX %s active%lux%lu allocation%lux%lu padding%.7g %s, %lu "
            "frames\n",
            device.name.UTF8String, (unsigned long)w, (unsigned long)h,
            (unsigned long)ow, (unsigned long)oh, padding, argv[4],
            (unsigned long)(warmup + 8));
    for (NSUInteger frame = 0; frame < warmup + 8; frame++)
      @autoreleasepool {
        NSUInteger phase = frame % 8;
        NSData *c = colors[phase], *d = depths[phase], *v = motions[phase];
        for (NSUInteger y = 0; y < h; y++) {
          memcpy((uint8_t *)cb.contents + y * cp,
                 (const uint8_t *)c.bytes + y * w * 8, w * 8);
          memcpy((uint8_t *)db.contents + y * dp,
                 (const uint8_t *)d.bytes + y * w * 4, w * 4);
          memcpy((uint8_t *)mb.contents + y * dp,
                 (const uint8_t *)v.bytes + y * w * 4, w * 4);
        }
        [allocator reset];
        [commands beginCommandBufferWithAllocator:allocator];
        id<MTL4ComputeCommandEncoder> stage = [commands computeCommandEncoder];
        if (!stage)
          fail(@"Upload encoder failed");
        upload(stage, cb, color, cp);
        upload(stage, db, depth, dp);
        upload(stage, mb, motion, dp);
        upload(stage, eb, exposure, 256);
        [stage updateFence:fence afterEncoderStages:MTLStageBlit];
        [stage endEncoding];
        NSArray *j = frames[phase][@"jitter"];
        scaler.jitterOffsetX = [j[0] floatValue];
        scaler.jitterOffsetY = [j[1] floatValue];
        scaler.reset = frame == 0;
        [scaler encodeToCommandBuffer:commands];
        if (frame >= warmup) {
          id<MTL4ComputeCommandEncoder> read = [commands computeCommandEncoder];
          if (!read)
            fail(@"Readback encoder failed");
          [read waitForFence:fence beforeEncoderStages:MTLStageBlit];
          [read copyFromTexture:output
                           sourceSlice:0
                           sourceLevel:0
                          sourceOrigin:MTLOriginMake(0, 0, 0)
                            sourceSize:MTLSizeMake(ow, oh, 1)
                              toBuffer:rb
                     destinationOffset:0
                destinationBytesPerRow:cp
              destinationBytesPerImage:cp * oh];
          [read endEncoding];
        }
        [commands endCommandBuffer];
        dispatch_semaphore_t feedbackDone = dispatch_semaphore_create(0);
        __block NSError *gpuError = nil;
        MTL4CommitOptions *options = [MTL4CommitOptions new];
        [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
          gpuError = feedback.error;
          dispatch_semaphore_signal(feedbackDone);
        }];
        id<MTL4CommandBuffer> submission[] = {commands};
        [queue commit:submission count:1 options:options];
        [queue signalEvent:completion value:frame + 1];
        if (![completion waitUntilSignaledValue:frame + 1 timeoutMS:30000] ||
            dispatch_semaphore_wait(
                feedbackDone,
                dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC))) {
          fprintf(stderr, "Completion/feedback timeout; exiting without "
                          "recycling GPU resources\n");
          _Exit(3);
        }
        if (gpuError)
          fail(gpuError.description);
        if (frame >= warmup) {
          NSMutableData *packed = [NSMutableData dataWithLength:ow * oh * 8];
          for (NSUInteger y = 0; y < oh; y++)
            memcpy((uint8_t *)packed.mutableBytes + y * ow * 8,
                   (uint8_t *)rb.contents + y * cp, ow * 8);
          NSString *name = [NSString
              stringWithFormat:@"frame_%03lu_phase_%lu.raw",
                               (unsigned long)frame, (unsigned long)phase];
          if (![packed writeToFile:[out stringByAppendingPathComponent:name]
                           options:NSDataWritingAtomic
                             error:&error])
            fail(error.description);
          [written addObject:@{
            @"file" : name,
            @"replay_frame" : @(frame),
            @"cycle_index" : @(phase),
            @"jitter" : j
          }];
        }
      }
    NSDictionary *report = @{
      @"mechanism" : @"MTL4FXTemporalScaler",
      @"device" : device.name,
      @"manifest" : manifestPath,
      @"width" : @(ow),
      @"height" : @(oh),
      @"active_width" : @(w),
      @"active_height" : @(h),
      @"encoding" : @"RGBA16_FLOAT_LE",
      @"origin" : @"top_left",
      @"row_pitch" : @(ow * 8),
      @"padding_rgb" : @(padding),
      @"auto_exposure" : @(automatic),
      @"fixed_exposure" : @(fixed),
      @"warmup_frames" : @(warmup),
      @"frames" : written
    };
    NSData *reportData =
        [NSJSONSerialization dataWithJSONObject:report
                                        options:NSJSONWritingPrettyPrinted
                                          error:&error];
    if (!reportData ||
        ![reportData
            writeToFile:[out stringByAppendingPathComponent:@"report.json"]
                options:NSDataWritingAtomic
                  error:&error])
      fail(error.description);
    [queue removeResidencySet:residency];
    // ARC releases all owned resources after the last proven completion.
  }
  fprintf(
      stderr,
      "Replay completed; all output frames written and resources released\n");
  return 0;
}
