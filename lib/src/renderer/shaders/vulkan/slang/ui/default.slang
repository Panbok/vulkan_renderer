VkrVulkanUiOutput vkr_ui_vertex_output(uint vertex_id)
{
    VkrVulkanUiRoot* root = (VkrVulkanUiRoot*)g_push.root;
    VkrVulkanTextVertex* vertices = (VkrVulkanTextVertex*)root.vertices;
    VkrVulkanTextVertex vertex_data = vertices[vertex_id];
    float2 target = max(root.target_unit_range.xy, float2(1.0f, 1.0f));
    VkrVulkanUiOutput output;
    output.position = float4(vertex_data.position.x / target.x * 2.0f - 1.0f,
                             1.0f - vertex_data.position.y / target.y * 2.0f,
                             0.0f, 1.0f);
    output.uv = vertex_data.texcoord;
    output.color = vertex_data.color;
    return output;
}

[shader("vertex")]
VkrVulkanUiOutput ui_vertex(uint vertex_id : SV_VertexID)
{
    return vkr_ui_vertex_output(vertex_id);
}

[shader("fragment")]
float4 ui_fragment(VkrVulkanUiOutput input) : SV_Target0
{
    VkrVulkanUiRoot* root = (VkrVulkanUiRoot*)g_push.root;
    if (root.mode == 0u)
    {
        if ((root.flags & 1u) == 0u)
            return input.color;
        uint texture_index = NonUniformResourceIndex(root.texture_index);
        uint sampler_index = NonUniformResourceIndex(root.sampler_index);
        return input.color *
               g_textures[texture_index].Sample(g_samplers[sampler_index],
                                                input.uv);
    }

    uint texture_index = NonUniformResourceIndex(root.texture_index);
    uint sampler_index = NonUniformResourceIndex(root.sampler_index);
    float4 atlas =
        g_textures[texture_index].Sample(g_samplers[sampler_index], input.uv);
    float alpha = atlas.a;
    if (root.mode == 1u)
    {
        float2 dx = ddx(input.uv);
        float2 dy = ddy(input.uv);
        float2 gradient_squared =
            max(dx * dx + dy * dy, float2(1e-12f, 1e-12f));
        float2 screen_tex_size = rsqrt(gradient_squared);
        float range = max(0.5f * dot(root.target_unit_range.zw,
                                    screen_tex_size),
                          1.0f);
        float distance = max(min(atlas.r, atlas.g),
                             min(max(atlas.r, atlas.g), atlas.b)) - 0.5f;
        alpha = saturate(range * distance + 0.5f);
    }
    return float4(input.color.rgb, input.color.a * alpha);
}

[shader("vertex")]
VkrVulkanUiOutput ui_rect_vertex(uint vertex_id : SV_VertexID)
{
    return vkr_ui_vertex_output(vertex_id);
}

[shader("fragment")]
float4 ui_rect_fragment(VkrVulkanUiOutput input) : SV_Target0
{
    VkrVulkanUiRoot* root = (VkrVulkanUiRoot*)g_push.root;
    float2 local = input.uv * root.rect_extent;
    float2 centered = local - root.rect_extent * 0.5f;
    bool left = centered.x < 0.0f;
    bool top = centered.y < 0.0f;
    float radius = left ? (top ? root.corner_radii.x : root.corner_radii.w)
                        : (top ? root.corner_radii.y : root.corner_radii.z);
    float2 q = abs(centered) - (root.rect_extent * 0.5f - radius);
    float distance = length(max(q, float2(0.0f, 0.0f))) +
                     min(max(q.x, q.y), 0.0f) - radius;
    float edge = max(fwidth(distance), 1e-4f);
    float alpha = saturate(0.5f - distance / edge);
    return float4(input.color.rgb, input.color.a * alpha);
}
