#include "common/tonemap.slangh"

struct TonemapUniforms {
  float exposure;
};

[[vk::binding(0, 0)]]
ConstantBuffer<TonemapUniforms> tonemap;

[[vk::binding(1, 1)]]
Texture2D<float4> source_hdr;

[[vk::binding(2, 1)]]
SamplerState source_sampler;

struct VertexOutput {
  float4 position : SV_Position;
  float2 uv : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertex_id : SV_VertexID) {
  static const float2 positions[3] = {
      float2(-1.0f, -1.0f),
      float2(3.0f, -1.0f),
      float2(-1.0f, 3.0f),
  };
  VertexOutput output;
  output.position = float4(positions[vertex_id], 0.0f, 1.0f);
  output.uv = positions[vertex_id] * 0.5f + 0.5f;
  return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
  float4 hdr = source_hdr.Sample(source_sampler, input.uv);
  return float4(vkr_aces_fitted(hdr.rgb * tonemap.exposure), hdr.a);
}
