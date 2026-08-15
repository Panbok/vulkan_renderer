[shader("fragment")]
float4 skybox_fragment(VkrBindlessVertexOutput input) : SV_Target0
{
    VkrBindlessPacketUtilityRoot* root =
        (VkrBindlessPacketUtilityRoot*)g_push.root;
    float2 ndc = input.uv * 2.0f - 1.0f;
    float4 world = mul(root.view_projection,
                       float4(ndc, 1.0f, 1.0f));
    float3 direction = normalize(world.xyz / world.w -
                                  root.view_position.xyz);
    uint texture_index =
        NonUniformResourceIndex(root.transmission_texture);
    uint sampler_index =
        NonUniformResourceIndex(root.transmission_sampler);
    return g_cube_textures[texture_index].Sample(g_samplers[sampler_index],
                                                  direction);
}
