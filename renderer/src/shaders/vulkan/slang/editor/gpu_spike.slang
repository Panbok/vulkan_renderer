// Debug.GpuSpike (VkrGpuDebugPayload.gpu_spike_iterations): a GPU cost with
// no CPU cost, for checking that GPU time lands on the frame that submitted
// it (docs/proposals/recordings.md, gate 4). One workgroup runs a dependent
// multiply-add chain per invocation; invocation 0 stores its last value so
// the chain is not removed.

struct VkrVkGpuSpikeRoot
{
    float *state;
    uint iterations;
    uint reserved;
};

struct VkrVkGpuSpikePush
{
    VkrVkGpuSpikeRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkGpuSpikePush> g_gpu_spike_push;

[shader("compute")]
[numthreads(64, 1, 1)]
void vk_gpu_spike(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkGpuSpikeRoot root = vkr_vk_uniform_root(*g_gpu_spike_push.root);
    float value = 1.0f + float(thread_id.x) * 1.0e-3f;
    for (uint i = 0u; i < root.iterations; ++i)
    {
        value = mad(value, 0.9999999f, 1.0e-7f);
    }
    if (thread_id.x == 0u)
    {
        root.state[0] = value;
    }
}
