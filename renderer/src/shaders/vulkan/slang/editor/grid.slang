// Editor ground grid: a full-screen pass over the Scene image that draws the
// plane through the world origin, hidden behind nearer opaque geometry unless
// the grid shows through it.
struct VkrVulkanEditorGridRoot
{
    float4x4 inverse_view_projection;
    // xyz camera position; w is 1 when the grid fades with distance.
    float4 camera_position;
    // x finest cell, y fade start, z fade end, w 1 when drawn through geometry.
    float4 params;
    uint2 extent;
    // 0 is the XZ plane (y = 0), 1 the ZY plane (x = 0), 2 the XY plane
    // (z = 0).
    uint plane;
    uint depth_texture;
    VkrDisplayOutputParams* display_output;
    uint64_t display_output_reserved;
};

struct VkrVulkanEditorGridPush
{
    VkrVulkanEditorGridRoot* root;
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVulkanEditorGridPush> g_editor_grid_push;

struct VkrVulkanEditorGridOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VkrVulkanEditorGridOutput editor_grid_vertex(uint vertex_id : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(3.0f, -1.0f),
        float2(-1.0f, 3.0f),
    };
    VkrVulkanEditorGridOutput output;
    output.position = float4(positions[vertex_id], 0.0f, 1.0f);
    return output;
}

float3 vkr_vk_editor_grid_unproject(VkrVulkanEditorGridRoot* root, float2 ndc,
                                    float depth)
{
    float4 world = mul(root.inverse_view_projection, float4(ndc, depth, 1.0f));
    return world.xyz / max(abs(world.w), 1e-7f) * sign(world.w);
}

[shader("fragment")]
float4 editor_grid_fragment(VkrVulkanEditorGridOutput input) : SV_Target0
{
    VkrVulkanEditorGridRoot* root = g_editor_grid_push.root;
    float2 uv = input.position.xy / float2(root.extent);
    float2 ndc = uv * 2.0f - 1.0f;
    float3 near_point = vkr_vk_editor_grid_unproject(root, ndc, 0.0f);
    float3 ray = vkr_vk_editor_grid_unproject(root, ndc, 1.0f) - near_point;
    // The plane's normal axis and its two in-plane coordinates.
    bool side = root.plane == 1u;
    bool front = root.plane == 2u;
    float origin_height =
        side ? near_point.x : (front ? near_point.z : near_point.y);
    float slope = side ? ray.x : (front ? ray.z : ray.y);
    float t = abs(slope) > 1e-9f ? -origin_height / slope : -1.0f;
    float3 hit = near_point + ray * t;
    float2 coord = side ? hit.zy : (front ? hit.xy : hit.xz);
    // Derivatives come before any discard.
    float2 footprint = fwidth(coord);
    float t_footprint = fwidth(t);
    if (t <= 0.0f || t >= 1.0f)
        discard;
    float visibility = 1.0f;
    if (root.params.w == 0.0f)
    {
        Texture2D<float> depth = g_depth_textures[root.depth_texture];
        uint depth_width = 0u;
        uint depth_height = 0u;
        depth.GetDimensions(depth_width, depth_height);
        int2 texel = clamp(int2(uv * float2(depth_width, depth_height)),
                           int2(0, 0),
                           int2(int(depth_width), int(depth_height)) - 1);
        float device_depth = depth.Load(int3(texel, 0));
        if (device_depth < 1.0f)
        {
            float3 surface = vkr_vk_editor_grid_unproject(root, ndc, device_depth);
            float s = dot(surface - near_point, ray) / max(dot(ray, ray), 1e-12f);
            // A floor on the plane stays under the grid: the depth texel is
            // render-scaled and jittered, so allow two of them plus a pixel.
            float texels = 2.0f * float(root.extent.x) /
                               max(float(depth_width), 1.0f) + 1.0f;
            visibility =
                vkr_editor_grid_depth_visibility(t, s, t_footprint, texels);
        }
    }
    float fade = 1.0f;
    if (root.camera_position.w > 0.0f)
    {
        float distance = length(hit - root.camera_position.xyz);
        fade = 1.0f - smoothstep(root.params.y, root.params.z, distance);
    }
    float3 x_color = float3(1.0f, 0.24f, 0.24f);
    float3 y_color = float3(0.35f, 0.85f, 0.35f);
    float3 z_color = float3(0.25f, 0.45f, 1.0f);
    float3 axis_u = side ? z_color : x_color;
    float3 axis_v = side || front ? y_color : z_color;
    float4 color = vkr_editor_grid_shade(coord, footprint, root.params.x,
                                         fade * visibility, axis_u, axis_v);
    if (color.a <= 0.0f)
        discard;
    color.rgb = vkr_display_output_ui(color.rgb, root.display_output[0]);
    return color;
}
