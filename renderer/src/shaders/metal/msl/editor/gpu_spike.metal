// Debug.GpuSpike (VkrGpuDebugPayload.gpu_spike_iterations): a GPU cost with
// no CPU cost, for checking that GPU time lands on the frame that submitted
// it (docs/proposals/recordings.md, gate 4). One threadgroup runs a
// dependent multiply-add chain per invocation; invocation 0 stores its last
// value so the chain is not removed.

struct VkrMetalPacketGpuSpikeRoot {
  device float *state;
  uint iterations;
  uint reserved;
};

kernel void vkr_metal_packet_gpu_spike(
    constant VkrMetalPacketGpuSpikeRoot &root [[buffer(0)]],
    uint invocation [[thread_position_in_grid]]) {
  float value = 1.0f + float(invocation) * 1.0e-3f;
  for (uint i = 0u; i < root.iterations; ++i) {
    value = fma(value, 0.9999999f, 1.0e-7f);
  }
  if (invocation == 0u) {
    root.state[0] = value;
  }
}
