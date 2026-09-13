struct VkrSkinningInfluence
{
    uint4 joints;
    float4 weights;
};

struct VkrSkinningRoot
{
    VkrVulkanVertex *bind_vertices;
    VkrSkinningInfluence *influences;
    column_major float4x4 *palette;
    VkrGpuDeformedVertex *output;
    uint vertex_count;
    uint palette_count;
    uint reserved0;
    uint reserved1;
};

struct VkrSkinningPush
{
    VkrSkinningRoot *root;
};
[[vk::push_constant]] ConstantBuffer<VkrSkinningPush> g_skinning_push;

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_skinning(uint3 dispatch_id : SV_DispatchThreadID)
{
    VkrSkinningRoot *root = g_skinning_push.root;
    uint index = dispatch_id.x;
    if (index >= root.vertex_count)
    {
        return;
    }
    VkrVulkanVertex vertex = root.bind_vertices[index];
    VkrSkinningInfluence influence = root.influences[index];
    float3 column0 = float3(0.0f);
    float3 column1 = float3(0.0f);
    float3 column2 = float3(0.0f);
    float3 translation = float3(0.0f);
    float weight_sum = 0.0f;
    for (uint lane = 0u; lane < 4u; ++lane)
    {
        float weight = influence.weights[lane];
        if (weight <= 0.0f || influence.joints[lane] >= root.palette_count)
        {
            continue;
        }
        float4x4 matrix = root.palette[influence.joints[lane]];
        column0 += mul(matrix, float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz * weight;
        column1 += mul(matrix, float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz * weight;
        column2 += mul(matrix, float4(0.0f, 0.0f, 1.0f, 0.0f)).xyz * weight;
        translation += mul(matrix, float4(0.0f, 0.0f, 0.0f, 1.0f)).xyz * weight;
        weight_sum += weight;
    }
    if (weight_sum == 0.0f)
    {
        column0 = float3(1.0f, 0.0f, 0.0f);
        column1 = float3(0.0f, 1.0f, 0.0f);
        column2 = float3(0.0f, 0.0f, 1.0f);
    }
    VkrSkinningFrame frame = vkr_skinning_frame(
        vertex.position, vertex.normal, vertex.tangent,
        column0, column1, column2, translation);
    VkrGpuDeformedVertex output;
    output.position = frame.position;
    output.normal_packed = vkr_skinning_oct_encode(frame.normal);
    output.tangent_packed = vkr_skinning_oct_encode(frame.tangent.xyz);
    output.tangent_sign = frame.tangent.w;
    output.reserved0 = 0u;
    output.reserved1 = 0u;
    root.output[index] = output;
}
