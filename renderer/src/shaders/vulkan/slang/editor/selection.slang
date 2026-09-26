// Selection outline: a full-screen pass over the Scene image that colors
// pixels just outside the selection mask. The mask shares the Scene image
// extent, so fragment and mask pixels coincide.
struct VkrVulkanSelectionOutlineRoot
{
    uint mask_texture;
    uint radius_px;
    uint2 reserved;
    float4 color;
    VkrDisplayOutputParams* display_output;
    uint64_t display_output_reserved;
};

struct VkrVulkanSelectionOutlinePush
{
    VkrVulkanSelectionOutlineRoot* root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanSelectionOutlinePush> g_selection_outline_push;

struct VkrVulkanSelectionOutlineOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VkrVulkanSelectionOutlineOutput selection_outline_vertex(uint vertex_id : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(3.0f, -1.0f),
        float2(-1.0f, 3.0f),
    };
    VkrVulkanSelectionOutlineOutput output;
    output.position = float4(positions[vertex_id], 0.0f, 1.0f);
    return output;
}

// Twelve taps reach two steps: the four axis neighbors at one and two steps
// plus the four diagonals. Inside pixels keep the scene unchanged.
[shader("fragment")]
float4 selection_outline_fragment(VkrVulkanSelectionOutlineOutput input) : SV_Target0
{
    VkrVulkanSelectionOutlineRoot* root = g_selection_outline_push.root;
    Texture2D<float4> mask = g_textures[root.mask_texture];
    uint width = 0u;
    uint height = 0u;
    mask.GetDimensions(width, height);
    const int2 extent = int2(int(width), int(height));
    const int2 pixel = int2(input.position.xy);
    if (any(pixel >= extent) || mask.Load(int3(pixel, 0)).r > 0.5f)
        discard;
    const int step = max(int(max(root.radius_px, 1u)) / 2, 1);
    static const int2 offsets[12] = {
        int2(1, 0),  int2(-1, 0), int2(0, 1),  int2(0, -1),
        int2(1, 1),  int2(-1, 1), int2(1, -1), int2(-1, -1),
        int2(2, 0),  int2(-2, 0), int2(0, 2),  int2(0, -2),
    };
    float coverage = 0.0f;
    for (uint i = 0u; i < 12u; ++i)
    {
        const int2 sample_pixel =
            clamp(pixel + offsets[i] * step, int2(0, 0), extent - 1);
        coverage = max(coverage, mask.Load(int3(sample_pixel, 0)).r);
    }
    if (coverage <= 0.0f)
        discard;
    float4 color = root.color;
    color.rgb = vkr_display_output_ui(color.rgb, root.display_output[0]);
    color.a *= coverage;
    return color;
}
