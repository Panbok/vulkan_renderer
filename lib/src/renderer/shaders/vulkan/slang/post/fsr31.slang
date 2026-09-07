// FSR consumes raw jittered inputs. Keep its masks separate from TAA's
// reconstructed color/metadata and history rejection policy.
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

// This is a private post-SDK history. Alpha carries its exact static-sample
// age; FSR's output alpha is otherwise not propagated past this pass.
struct VkrVkFsr31StabilizeRoot
{
    uint output_texture;
    uint history_texture;
    uint reactive_texture;
    uint scene_stationary;
    uint2 output_extent;
    uint2 render_extent;
    float2 jitter_pixels;
    uint2 reserved;
};

struct VkrVkFsr31StabilizePush
{
    VkrVkFsr31StabilizeRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkFsr31StabilizePush>
    g_fsr31_stabilize_push;

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_fsr31_prepare(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkFsr31PrepareRoot *root = g_fsr31_push.root;
    uint2 pixel = thread_id.xy;
    if (any(pixel >= root.extent))
        return;

    float3 color = g_textures[root.scene_texture].Load(int3(pixel, 0)).rgb;
    float3 opaque = g_textures[root.pre_transmission_texture].Load(int3(pixel, 0)).rgb;
    float validity = g_textures[root.validity_texture].Load(int3(pixel, 0)).x;
    float depth = g_depth_textures[root.opaque_depth_texture].Load(int3(pixel, 0));
    // Transmission motion is written for the nearest peeled surface. Its
    // depth must accompany that motion rather than the opaque surface behind it.
    if (root.transmission_enabled != 0u && validity >= 2.0f)
        depth = min(depth, g_depth_array_textures[root.transmission_depth_texture]
                               .Load(int4(pixel, 0, 0)));

    float3 difference = abs(color - opaque);
    float optical = saturate(max(difference.x, max(difference.y, difference.z)) /
                             max(0.05f, max(max(color.x, color.y),
                                           max(color.z, max(opaque.x, max(opaque.y, opaque.z))))));
    // Optical contrast marks composition changes, not authored reactivity.
    // Feeding it to both masks over-rejects moving glass history. Suppress
    // composition contrast only with a full scene/revision match, as in TAA.
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

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_fsr31_stabilize(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkFsr31StabilizeRoot *root = g_fsr31_stabilize_push.root;
    uint2 pixel = thread_id.xy;
    if (any(pixel >= root.output_extent))
        return;

    float4 current = g_storage_2d[root.output_texture][pixel];
    // The submitted-history equality proof gates every history read. A change
    // preserves this frame's FSR RGB and restarts the private sample count.
    if (root.scene_stationary == 0u)
    {
        g_storage_2d[root.output_texture][pixel] = float4(current.rgb, 0.0f);
        return;
    }

    // `pixel` is in reconstructed-output pixels. Convert its center into raw
    // render-pixel coordinates, then apply the current raster jitter. The four
    // clamped bilinear-footprint texels conservatively retain authored and
    // missing-motion reactivity despite static optical-mask suppression.
    float2 raw_pixel = (float2(pixel) + 0.5f) * float2(root.render_extent) /
                           float2(root.output_extent) +
                       root.jitter_pixels - 0.5f;
    int2 raw_min = int2(floor(raw_pixel));
    int2 raw_max = raw_min + int2(1, 1);
    int2 raw_limit = int2(root.render_extent - 1u);
    uint2 reactive00 = uint2(clamp(raw_min, int2(0, 0), raw_limit));
    uint2 reactive10 = uint2(clamp(int2(raw_max.x, raw_min.y), int2(0, 0), raw_limit));
    uint2 reactive01 = uint2(clamp(int2(raw_min.x, raw_max.y), int2(0, 0), raw_limit));
    uint2 reactive11 = uint2(clamp(raw_max, int2(0, 0), raw_limit));
    float reactive = max(
        max(g_textures[root.reactive_texture].Load(int3(reactive00, 0)).x,
            g_textures[root.reactive_texture].Load(int3(reactive10, 0)).x),
        max(g_textures[root.reactive_texture].Load(int3(reactive01, 0)).x,
            g_textures[root.reactive_texture].Load(int3(reactive11, 0)).x));
    if (reactive > 0.0f)
    {
        g_storage_2d[root.output_texture][pixel] = float4(current.rgb, 0.0f);
        return;
    }

    // History and current are the same canonical output pixel. The history is
    // the previous submitted final result, so alpha can remain a private age.
    float4 previous = g_textures[root.history_texture].Load(int3(pixel, 0));
    float age = min(previous.a + 1.0f, VKR_TEMPORAL_STATIC_SAMPLE_COUNT);
    float weight = (age - 1.0f) / age;
    float4 resolved = previous.a >= VKR_TEMPORAL_STATIC_SAMPLE_COUNT
                          ? previous
                          : float4(lerp(current.rgb, previous.rgb, weight), age);
    g_storage_2d[root.output_texture][pixel] = resolved;
}
