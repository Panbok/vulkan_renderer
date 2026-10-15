// FSR 3.1 native antialiasing consumes raw jittered inputs. Keep its masks
// separate from the SMAA T2x resolve's accept mask and history rejection.

// Mirrors VkrVulkanFsr31PrepareRoot.
struct VkrVkFsr31PrepareRoot
{
    uint scene_texture;
    uint pre_transmission_texture;
    uint validity_texture;
    uint opaque_depth_texture;
    uint transmission_depth_texture;
    uint output_depth_texture;
    uint reactive_texture;
    uint composition_texture;
    uint2 extent;
    uint transmission_enabled;
    uint scene_stationary;
};

struct VkrVkFsr31Push
{
    VkrVkFsr31PrepareRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkFsr31Push> g_fsr31_push;

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_fsr31_prepare(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkFsr31PrepareRoot root = vkr_vk_uniform_root(*g_fsr31_push.root);
    uint2 pixel = thread_id.xy;
    if (any(pixel >= root.extent))
        return;

    float3 color = g_textures[root.scene_texture].Load(int3(pixel, 0)).rgb;
    float3 opaque = g_textures[root.pre_transmission_texture].Load(int3(pixel, 0)).rgb;
    // Validity x: 0 without motion, 1 for opaque and sky motion, 2 plus the
    // material's temporal reactivity for a transmissive surface's motion.
    float validity = g_textures[root.validity_texture].Load(int3(pixel, 0)).x;
    float depth = g_depth_textures[root.opaque_depth_texture].Load(int3(pixel, 0));
    // Transmission motion is written for the nearest peeled surface. Its
    // depth must accompany that motion rather than the opaque surface behind it.
    // Reversed camera depth (ADR-109): the nearer surface has the larger depth.
    if (root.transmission_enabled != 0u && validity >= 2.0f)
        depth = max(depth, g_depth_array_textures[root.transmission_depth_texture]
                               .Load(int4(pixel, 0, 0)));

    float3 difference = abs(color - opaque);
    float optical = saturate(max(difference.x, max(difference.y, difference.z)) /
                             max(0.05f, max(max(color.x, color.y),
                                           max(color.z, max(opaque.x, max(opaque.y, opaque.z))))));
    // Optical contrast marks composition changes, not authored reactivity.
    // Feeding it to both masks over-rejects moving glass history. Suppress
    // composition contrast only with a full scene/revision match.
    if (root.scene_stationary != 0u)
        optical = 0.0f;
    float authored = validity >= 2.0f ? saturate(validity - 2.0f) : 0.0f;
    // Newly published or unsupported-motion surfaces must not acquire locks
    // from a zero vector. A whole-frame reset handles the first frame.
    float missing_motion = validity < 0.5f ? 1.0f : 0.0f;
    g_storage_2d[root.output_depth_texture][pixel] = float4(depth, 0.0f, 0.0f, 0.0f);
    g_storage_2d[root.reactive_texture][pixel] = float4(max(authored, missing_motion), 0.0f, 0.0f, 0.0f);
    g_storage_2d[root.composition_texture][pixel] = float4(max(optical, missing_motion), 0.0f, 0.0f, 0.0f);
}
