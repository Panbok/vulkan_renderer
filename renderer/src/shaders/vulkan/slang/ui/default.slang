// UI draws carry a VkrVulkanUiRoot, not a world draw root. Keeping this
// physical pointer typed makes the compiled UI entries expose the same 64-byte
// record that the Vulkan host validates. The push payload remains 16 bytes.
struct VkrVulkanUiPush
{
    VkrVulkanUiRoot* root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanUiPush> g_ui_push;

static const uint VKR_VULKAN_UI_MODE_QUAD = 0u;
static const uint VKR_VULKAN_UI_MODE_MTSDF_TEXT = 1u;
static const uint VKR_VULKAN_UI_MODE_BOX = 3u;
static const uint VKR_VULKAN_UI_MODE_IMAGE = 4u;

// Signed distance to a rounded box; negative inside. `local` is Y-down.
float vkr_ui_box_distance(float2 local, float2 half_extent, float4 corner_radius)
{
    bool left = local.x < 0.0f;
    bool top = local.y < 0.0f;
    float radius = left ? (top ? corner_radius.x : corner_radius.w)
                        : (top ? corner_radius.y : corner_radius.z);
    float2 q = abs(local) - (half_extent - radius);
    return length(max(q, float2(0.0f, 0.0f))) + min(max(q.x, q.y), 0.0f) -
           radius;
}

float4 vkr_ui_display_color(float4 color, VkrVulkanUiRoot* root)
{
    color.rgb = vkr_display_output_ui(color.rgb, root.display_output[0]);
    return color;
}

[shader("vertex")]
VkrVulkanUiOutput ui_vertex(uint vertex_id : SV_VertexID)
{
    VkrVulkanUiRoot* root = g_ui_push.root;
    VkrVulkanUiVertex* vertices = (VkrVulkanUiVertex*)root.vertices;
    VkrVulkanUiVertex vertex_data = vertices[vertex_id];
    float2 target = max(root.target_unit_range.xy, float2(1.0f, 1.0f));
    VkrVulkanUiOutput output;
    output.position = float4(vertex_data.position.x / target.x * 2.0f - 1.0f,
                             1.0f - vertex_data.position.y / target.y * 2.0f,
                             0.0f, 1.0f);
    output.uv = vertex_data.texcoord;
    output.color = vertex_data.color;
    output.border_color = vertex_data.border_color;
    output.corner_radius = vertex_data.corner_radius;
    output.local = vertex_data.local;
    output.half_extent = vertex_data.half_extent;
    output.border = vertex_data.border;
    output.softness = vertex_data.softness;
    output.mode = vertex_data.mode;
    return output;
}

[shader("fragment")]
float4 ui_fragment(VkrVulkanUiOutput input) : SV_Target0
{
    VkrVulkanUiRoot* root = g_ui_push.root;
    if (input.mode == VKR_VULKAN_UI_MODE_QUAD)
        return vkr_ui_display_color(input.color, root);

    if (input.mode == VKR_VULKAN_UI_MODE_BOX)
    {
        float distance = vkr_ui_box_distance(input.local, input.half_extent,
                                             input.corner_radius);
        float edge = max(fwidth(distance), 1e-4f);
        // A feathered box ramps coverage across twice its softness.
        float coverage =
            input.softness > 0.0f
                ? 1.0f - smoothstep(-input.softness, input.softness, distance)
                : saturate(0.5f - distance / edge);
        float4 color = input.color;
        if (input.border > 0.0f)
        {
            float border_mix = saturate(0.5f + (distance + input.border) / edge);
            color = lerp(input.color, input.border_color, border_mix);
        }
        color.a *= coverage;
        return vkr_ui_display_color(color, root);
    }

    uint texture_index = NonUniformResourceIndex(root.texture);
    uint sampler_index = NonUniformResourceIndex(root.sampler);
    float4 texel =
        g_textures[texture_index].Sample(g_samplers[sampler_index], input.uv);
    if (input.mode == VKR_VULKAN_UI_MODE_IMAGE)
        return vkr_ui_display_color(input.color * texel, root);

    float alpha = texel.a;
    if (input.mode == VKR_VULKAN_UI_MODE_MTSDF_TEXT)
    {
        float2 dx = ddx(input.uv);
        float2 dy = ddy(input.uv);
        float2 gradient_squared =
            max(dx * dx + dy * dy, float2(1e-12f, 1e-12f));
        float2 screen_tex_size = rsqrt(gradient_squared);
        float range = max(0.5f * dot(root.target_unit_range.zw,
                                    screen_tex_size),
                          1.0f);
        float distance = max(min(texel.r, texel.g),
                             min(max(texel.r, texel.g), texel.b)) - 0.5f;
        alpha = saturate(range * distance + 0.5f);
    }
    return float4(vkr_display_output_ui(input.color.rgb,
                                        root.display_output[0]),
                  input.color.a * alpha);
}
