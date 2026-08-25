#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <math.h>
#include <stdio.h>

static bool close4(const float *value, float r, float g, float b, float a) {
  return fabsf(value[0] - r) < 0.01f && fabsf(value[1] - g) < 0.01f &&
         fabsf(value[2] - b) < 0.01f && fabsf(value[3] - a) < 0.01f;
}

int main(void) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      fprintf(stderr, "Metal device unavailable\n");
      return 3;
    }
    static NSString *source =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "kernel void sample_layered(\n"
         "  texture2d_array<float, access::read> array_texture "
         "[[texture(0)]],\n"
         "  texturecube_array<float, access::sample> cube_texture "
         "[[texture(1)]],\n"
         "  device float4 *output [[buffer(0)]]) {\n"
         "  constexpr sampler cube_sampler(coord::normalized, filter::nearest, "
         "address::clamp_to_edge);\n"
         "  output[0] = array_texture.read(uint2(0), 1);\n"
         "  output[1] = cube_texture.sample(cube_sampler, float3(1.0, 0.0, "
         "0.0), 1);\n"
         "}\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source
                                                  options:nil
                                                    error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"sample_layered"];
    id<MTLComputePipelineState> pipeline =
        function
            ? [device newComputePipelineStateWithFunction:function error:&error]
            : nil;
    if (!pipeline) {
      fprintf(stderr, "Layered sampling pipeline failed: %s\n",
              error.localizedDescription.UTF8String ?: "unknown");
      return 1;
    }

    MTLTextureDescriptor *array_desc = [MTLTextureDescriptor new];
    array_desc.textureType = MTLTextureType2DArray;
    array_desc.pixelFormat = MTLPixelFormatRGBA8Unorm;
    array_desc.width = 1;
    array_desc.height = 1;
    array_desc.arrayLength = 2;
    array_desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> array_texture = [device newTextureWithDescriptor:array_desc];

    MTLTextureDescriptor *cube_desc = [MTLTextureDescriptor new];
    cube_desc.textureType = MTLTextureTypeCubeArray;
    cube_desc.pixelFormat = MTLPixelFormatRGBA8Unorm;
    cube_desc.width = 1;
    cube_desc.height = 1;
    cube_desc.arrayLength = 2;
    cube_desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> cube_texture = [device newTextureWithDescriptor:cube_desc];
    if (!array_texture || !cube_texture) {
      fprintf(stderr, "Layered texture allocation failed\n");
      return 1;
    }

    const MTLRegion pixel_region = MTLRegionMake2D(0, 0, 1, 1);
    const uint8_t array_pixels[2][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}};
    for (NSUInteger layer = 0; layer < 2; ++layer) {
      [array_texture replaceRegion:pixel_region
                       mipmapLevel:0
                             slice:layer
                         withBytes:array_pixels[layer]
                       bytesPerRow:4
                     bytesPerImage:4];
    }
    for (NSUInteger slice = 0; slice < 12; ++slice) {
      const uint8_t pixel[4] = {slice == 6 ? 0 : 255, slice == 6 ? 255 : 0, 0,
                                255};
      [cube_texture replaceRegion:pixel_region
                      mipmapLevel:0
                            slice:slice
                        withBytes:pixel
                      bytesPerRow:4
                    bytesPerImage:4];
    }

    id<MTLBuffer> output =
        [device newBufferWithLength:sizeof(float) * 8
                            options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:array_texture atIndex:0];
    [encoder setTexture:cube_texture atIndex:1];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
      fprintf(stderr, "Layered sampling command failed: %s\n",
              command.error.localizedDescription.UTF8String ?: "unknown");
      return 1;
    }
    const float *values = output.contents;
    if (!close4(values, 0, 1, 0, 1) || !close4(values + 4, 0, 1, 0, 1)) {
      fprintf(stderr,
              "Layered sampling mismatch: array=(%.3f %.3f %.3f %.3f) "
              "cube=(%.3f %.3f %.3f %.3f)\n",
              values[0], values[1], values[2], values[3], values[4], values[5],
              values[6], values[7]);
      return 1;
    }
    printf("Metal layered texture sampling passed\n");
    return 0;
  }
}
