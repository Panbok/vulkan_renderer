[shader("vertex")]
VkrVulkanTextOutput text_vertex(uint vertex_id : SV_VertexID)
{
    VkrVulkanPacketUtilityRoot* root =
        (VkrVulkanPacketUtilityRoot*)g_push.root;
    VkrVulkanTextVertex* vertices =
        (VkrVulkanTextVertex*)root.vertices;
    VkrVulkanTextVertex glyph = vertices[vertex_id];
    float4 position = mul(root.view, float4(glyph.position, 0.0f, 1.0f));
    VkrVulkanTextOutput output;
    if ((root.flags & 1u) != 0u)
    {
        float2 target = max(root.point_light_grid_origin_cell_size.xy,
                            float2(1.0f, 1.0f));
        output.position = float4(position.x / target.x * 2.0f - 1.0f,
                                 1.0f - position.y / target.y * 2.0f,
                                 position.z, position.w);
    }
    else
    {
        output.position = mul(root.view_projection, position);
    }
    output.uv = glyph.texcoord;
    output.color = glyph.color;
    output.object_id = root.first_instance;
    return output;
}

float text_alpha(VkrVulkanTextOutput input)
{
    VkrVulkanPacketUtilityRoot* root =
        (VkrVulkanPacketUtilityRoot*)g_push.root;
    uint texture_index = NonUniformResourceIndex(root.transmission_texture);
    uint sampler_index = NonUniformResourceIndex(root.transmission_sampler);
    float4 atlas =
        g_textures[texture_index].Sample(g_samplers[sampler_index], input.uv);
    if (root.material_flags == 0u)
        return atlas.a;
    float2 dx = ddx(input.uv);
    float2 dy = ddy(input.uv);
    float2 gradient_squared =
        max(dx * dx + dy * dy, float2(1e-12f, 1e-12f));
    float2 screen_tex_size = rsqrt(gradient_squared);
    float range =
        max(0.5f * dot(root.material_alpha.xy, screen_tex_size), 1.0f);
    float distance = max(min(atlas.r, atlas.g),
                         min(max(atlas.r, atlas.g), atlas.b)) - 0.5f;
    return saturate(range * distance + 0.5f);
}

[shader("fragment")]
float4 text_fragment(VkrVulkanTextOutput input) : SV_Target0
{
    float alpha = text_alpha(input);
    return float4(input.color.rgb, input.color.a * alpha);
}

[shader("fragment")]
uint text_picking_fragment(VkrVulkanTextOutput input) : SV_Target0
{
    if (text_alpha(input) <= 0.01f)
        discard;
    return input.object_id;
}
