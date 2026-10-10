/*
 * Decodes a BC6H or BC7 lightmap page through the GPU's texture unit, so the
 * baker can report each page's encoding error (vkr_bake_gpu_decode_bc). The
 * encoders are DirectXTex's compute kernels in vendor/directxtex/Shaders;
 * vkr_bake_vulkan.cpp binds this kernel in their descriptor set layout,
 * where binding 1 is the source texture and binding 3 the output buffer.
 */

struct DecodeArgs
{
    uint width;
    uint height;
};

[[vk::binding(1, 0)]] Texture2D<float4> blocks;
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> texels_out;

[shader("compute")]
[numthreads(8, 8, 1)]
void bc_decode(uint3 id : SV_DispatchThreadID, uniform DecodeArgs args)
{
    // The dispatch rounds the page up to whole 8x8 groups.
    if (id.x >= args.width || id.y >= args.height)
    {
        return;
    }

    texels_out[id.y * args.width + id.x] = blocks.Load(int3(int2(id.xy), 0));
}
