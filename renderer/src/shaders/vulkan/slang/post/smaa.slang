// Enhanced Subpixel Morphological Antialiasing (SMAA) for the desktop
// pipeline: a compute port of the reference SMAA.hlsl at iryoku/smaa commit
// 71c806a838bdd7d517df19192a20f0c61b3ca29d, HIGH preset, with luma edge
// detection, diagonal and corner detection. The three passes run on the
// display-linear image after the tonemap (ADR-043); `vendor/smaa/` holds the
// area and search textures.
//
// Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)
// Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
// Copyright (C) 2013 Belen Masia (bmasia@unizar.es)
// Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)
// Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to
// do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software. As clarification, there
// is no requirement that the copyright notice and permission be included in
// binary distributions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// The reference's HIGH preset.
#define VKR_SMAA_THRESHOLD 0.1f
#define VKR_SMAA_MAX_SEARCH_STEPS 16
#define VKR_SMAA_MAX_SEARCH_STEPS_DIAG 8
#define VKR_SMAA_CORNER_ROUNDING 25
#define VKR_SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR 2.0f

// The reference's non-configurable constants.
#define VKR_SMAA_AREATEX_MAX_DISTANCE 16.0f
#define VKR_SMAA_AREATEX_MAX_DISTANCE_DIAG 20.0f
#define VKR_SMAA_AREATEX_PIXEL_SIZE (1.0f / float2(160.0f, 560.0f))
#define VKR_SMAA_AREATEX_SUBTEX_SIZE (1.0f / 7.0f)
#define VKR_SMAA_SEARCHTEX_SIZE float2(66.0f, 33.0f)
#define VKR_SMAA_SEARCHTEX_PACKED_SIZE float2(64.0f, 16.0f)
#define VKR_SMAA_CORNER_ROUNDING_NORM (float(VKR_SMAA_CORNER_ROUNDING) / 100.0f)

// Mirrors VkrVulkanSmaaRoot. Each pass reads the fields it names.
struct VkrVkSmaaRoot
{
    // Edges pass: the display-linear colour. Blend pass: the same colour.
    uint source_texture;
    // Storage target of the pass: edges (RG8), weights (RGBA8) or the
    // antialiased colour (RGBA16F).
    uint destination_texture;
    // Weights pass: the edges. Blend pass: the blending weights.
    uint edges_texture;
    uint blend_texture;
    uint area_texture;
    uint search_texture;
    uint linear_sampler;
    uint point_sampler;
    uint2 extent;
    // Blend pass: the scene's motion, previous_uv - current_uv in UV units.
    uint motion_texture;
    uint reserved;
    // The area texture's subpixel rows for the frame's jitter (the
    // reference's @SUBSAMPLE_INDICES); zero without temporal jitter.
    float4 subsample_indices;
};

struct VkrVkSmaaPush
{
    VkrVkSmaaRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkSmaaPush> g_smaa_push;

// The reference's SMAA_RT_METRICS: (1 / width, 1 / height, width, height).
float4 vkr_vk_smaa_metrics(VkrVkSmaaRoot root)
{
    float2 size = float2(root.extent);
    return float4(1.0f / size, size);
}

float4 vkr_vk_smaa_sample(VkrVkSmaaRoot root, uint texture_index, float2 uv)
{
    return g_textures[NonUniformResourceIndex(texture_index)].SampleLevel(
        g_samplers[NonUniformResourceIndex(root.linear_sampler)], uv, 0.0f);
}

float4 vkr_vk_smaa_sample_point(VkrVkSmaaRoot root, uint texture_index,
                                float2 uv)
{
    return g_textures[NonUniformResourceIndex(texture_index)].SampleLevel(
        g_samplers[NonUniformResourceIndex(root.point_sampler)], uv, 0.0f);
}

// The reference's SMAASampleLevelZeroOffset as its tex2Dlod form: a texel
// offset added to the coordinate, which is exact for whole texels and needs
// no compile-time constant.
float4 vkr_vk_smaa_sample_offset(VkrVkSmaaRoot root, uint texture_index,
                                 float2 uv, int2 offset)
{
    return vkr_vk_smaa_sample(
        root, texture_index, uv + float2(offset) * vkr_vk_smaa_metrics(root).xy);
}

void vkr_vk_smaa_movc(bool2 cond, inout float2 variable, float2 value)
{
    if (cond.x)
        variable.x = value.x;
    if (cond.y)
        variable.y = value.y;
}

void vkr_vk_smaa_movc(bool4 cond, inout float4 variable, float4 value)
{
    float2 xy = variable.xy;
    float2 zw = variable.zw;
    vkr_vk_smaa_movc(cond.xy, xy, value.xy);
    vkr_vk_smaa_movc(cond.zw, zw, value.zw);
    variable = float4(xy, zw);
}

// Luma edge detection needs gamma-encoded colour. The display-linear source
// takes a square root, as FXAA did, which keeps EDR highlights above one.
float vkr_vk_smaa_luma(VkrVkSmaaRoot root, float2 uv)
{
    float3 color = vkr_vk_smaa_sample_point(root, root.source_texture, uv).rgb;
    return dot(sqrt(max(color, 0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
}

// SMAALumaEdgeDetectionPS with SMAAEdgeDetectionVS's offsets. A pixel without
// an edge stores zero, where the reference discards into a cleared target.
float2 vkr_vk_smaa_luma_edges(VkrVkSmaaRoot root, float2 texcoord)
{
    float4 metrics = vkr_vk_smaa_metrics(root);
    float4 offset0 = metrics.xyxy * float4(-1.0f, 0.0f, 0.0f, -1.0f) +
                     texcoord.xyxy;
    float4 offset1 = metrics.xyxy * float4(1.0f, 0.0f, 0.0f, 1.0f) +
                     texcoord.xyxy;
    float4 offset2 = metrics.xyxy * float4(-2.0f, 0.0f, 0.0f, -2.0f) +
                     texcoord.xyxy;

    float2 threshold = float2(VKR_SMAA_THRESHOLD, VKR_SMAA_THRESHOLD);
    float luma = vkr_vk_smaa_luma(root, texcoord);
    float luma_left = vkr_vk_smaa_luma(root, offset0.xy);
    float luma_top = vkr_vk_smaa_luma(root, offset0.zw);

    float4 delta;
    delta.xy = abs(luma - float2(luma_left, luma_top));
    float2 edges = step(threshold, delta.xy);
    if (dot(edges, float2(1.0f, 1.0f)) == 0.0f)
        return 0.0f;

    float luma_right = vkr_vk_smaa_luma(root, offset1.xy);
    float luma_bottom = vkr_vk_smaa_luma(root, offset1.zw);
    delta.zw = abs(luma - float2(luma_right, luma_bottom));
    float2 max_delta = max(delta.xy, delta.zw);

    float luma_left_left = vkr_vk_smaa_luma(root, offset2.xy);
    float luma_top_top = vkr_vk_smaa_luma(root, offset2.zw);
    delta.zw = abs(float2(luma_left, luma_top) -
                   float2(luma_left_left, luma_top_top));
    max_delta = max(max_delta.xy, delta.zw);
    float final_delta = max(max_delta.x, max_delta.y);

    // Local contrast adaptation.
    edges.xy *= step(final_delta,
                     VKR_SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    return edges;
}

// Diagonal search (SMAADecodeDiagBilinearAccess to SMAACalculateDiagWeights).
float2 vkr_vk_smaa_decode_diag_bilinear_access(float2 e)
{
    e.r = e.r * abs(5.0f * e.r - 5.0f * 0.75f);
    return round(e);
}

float4 vkr_vk_smaa_decode_diag_bilinear_access(float4 e)
{
    e.rb = e.rb * abs(5.0f * e.rb - 5.0f * 0.75f);
    return round(e);
}

float2 vkr_vk_smaa_search_diag1(VkrVkSmaaRoot root, float2 texcoord,
                                float2 dir, out float2 e)
{
    float4 metrics = vkr_vk_smaa_metrics(root);
    float4 coord = float4(texcoord, -1.0f, 1.0f);
    float3 t = float3(metrics.xy, 1.0f);
    e = 0.0f;
    while (coord.z < float(VKR_SMAA_MAX_SEARCH_STEPS_DIAG - 1) &&
           coord.w > 0.9f)
    {
        coord.xyz = t * float3(dir, 1.0f) + coord.xyz;
        e = vkr_vk_smaa_sample(root, root.edges_texture, coord.xy).rg;
        coord.w = dot(e, float2(0.5f, 0.5f));
    }
    return coord.zw;
}

float2 vkr_vk_smaa_search_diag2(VkrVkSmaaRoot root, float2 texcoord,
                                float2 dir, out float2 e)
{
    float4 metrics = vkr_vk_smaa_metrics(root);
    float4 coord = float4(texcoord, -1.0f, 1.0f);
    coord.x += 0.25f * metrics.x;
    float3 t = float3(metrics.xy, 1.0f);
    e = 0.0f;
    while (coord.z < float(VKR_SMAA_MAX_SEARCH_STEPS_DIAG - 1) &&
           coord.w > 0.9f)
    {
        coord.xyz = t * float3(dir, 1.0f) + coord.xyz;
        // Fetch both edges at once using bilinear filtering.
        e = vkr_vk_smaa_sample(root, root.edges_texture, coord.xy).rg;
        e = vkr_vk_smaa_decode_diag_bilinear_access(e);
        coord.w = dot(e, float2(0.5f, 0.5f));
    }
    return coord.zw;
}

float2 vkr_vk_smaa_area_diag(VkrVkSmaaRoot root, float2 dist, float2 e,
                             float offset)
{
    float2 texcoord = float2(VKR_SMAA_AREATEX_MAX_DISTANCE_DIAG,
                             VKR_SMAA_AREATEX_MAX_DISTANCE_DIAG) * e +
                      dist;
    texcoord = VKR_SMAA_AREATEX_PIXEL_SIZE * texcoord +
               0.5f * VKR_SMAA_AREATEX_PIXEL_SIZE;
    // Diagonal areas are on the second half of the texture.
    texcoord.x += 0.5f;
    texcoord.y += VKR_SMAA_AREATEX_SUBTEX_SIZE * offset;
    return vkr_vk_smaa_sample(root, root.area_texture, texcoord).rg;
}

float2 vkr_vk_smaa_calculate_diag_weights(VkrVkSmaaRoot root, float2 texcoord,
                                          float2 e, float4 subsample_indices)
{
    float4 metrics = vkr_vk_smaa_metrics(root);
    float2 weights = float2(0.0f, 0.0f);

    // Search for the line ends.
    float4 d = 0.0f;
    float2 end;
    if (e.r > 0.0f)
    {
        d.xz = vkr_vk_smaa_search_diag1(root, texcoord, float2(-1.0f, 1.0f),
                                        end);
        d.x += float(end.y > 0.9f);
    }
    else
    {
        d.xz = float2(0.0f, 0.0f);
    }
    d.yw = vkr_vk_smaa_search_diag1(root, texcoord, float2(1.0f, -1.0f), end);

    if (d.x + d.y > 2.0f)
    {
        // Fetch the crossing edges.
        float4 coords = float4(-d.x + 0.25f, d.x, d.y, -d.y - 0.25f) *
                            metrics.xyxy +
                        texcoord.xyxy;
        float4 c;
        c.xy = vkr_vk_smaa_sample_offset(root, root.edges_texture, coords.xy,
                                         int2(-1, 0))
                   .rg;
        c.zw = vkr_vk_smaa_sample_offset(root, root.edges_texture, coords.zw,
                                         int2(1, 0))
                   .rg;
        c.yxwz = vkr_vk_smaa_decode_diag_bilinear_access(c.xyzw);

        // Merge crossing edges at each side into a single value.
        float2 cc = float2(2.0f, 2.0f) * c.xz + c.yw;
        // Remove the crossing edge if we didn't find the end of the line.
        vkr_vk_smaa_movc(bool2(step(0.9f, d.zw) != 0.0f), cc,
                         float2(0.0f, 0.0f));
        weights += vkr_vk_smaa_area_diag(root, d.xy, cc, subsample_indices.z);
    }

    // Search for the line ends.
    d.xz = vkr_vk_smaa_search_diag2(root, texcoord, float2(-1.0f, -1.0f), end);
    if (vkr_vk_smaa_sample_offset(root, root.edges_texture, texcoord,
                                  int2(1, 0))
            .r > 0.0f)
    {
        d.yw = vkr_vk_smaa_search_diag2(root, texcoord, float2(1.0f, 1.0f),
                                        end);
        d.y += float(end.y > 0.9f);
    }
    else
    {
        d.yw = float2(0.0f, 0.0f);
    }

    if (d.x + d.y > 2.0f)
    {
        // Fetch the crossing edges.
        float4 coords = float4(-d.x, -d.x, d.y, d.y) * metrics.xyxy +
                        texcoord.xyxy;
        float4 c;
        c.x = vkr_vk_smaa_sample_offset(root, root.edges_texture, coords.xy,
                                        int2(-1, 0))
                  .g;
        c.y = vkr_vk_smaa_sample_offset(root, root.edges_texture, coords.xy,
                                        int2(0, -1))
                  .r;
        c.zw = vkr_vk_smaa_sample_offset(root, root.edges_texture, coords.zw,
                                         int2(1, 0))
                   .gr;
        float2 cc = float2(2.0f, 2.0f) * c.xz + c.yw;
        vkr_vk_smaa_movc(bool2(step(0.9f, d.zw) != 0.0f), cc,
                         float2(0.0f, 0.0f));
        weights +=
            vkr_vk_smaa_area_diag(root, d.xy, cc, subsample_indices.w).gr;
    }
    return weights;
}

// Horizontal and vertical searches (SMAASearchLength to SMAASearchYDown).
float vkr_vk_smaa_search_length(VkrVkSmaaRoot root, float2 e, float offset)
{
    // The texture is flipped vertically, with left and right cases taking
    // half of the space horizontally.
    float2 scale = VKR_SMAA_SEARCHTEX_SIZE * float2(0.5f, -1.0f);
    float2 bias = VKR_SMAA_SEARCHTEX_SIZE * float2(offset, 1.0f);
    // Scale and bias to access texel centers.
    scale += float2(-1.0f, 1.0f);
    bias += float2(0.5f, -0.5f);
    // Convert from pixel coordinates to texcoords; the texture is cropped.
    scale *= 1.0f / VKR_SMAA_SEARCHTEX_PACKED_SIZE;
    bias *= 1.0f / VKR_SMAA_SEARCHTEX_PACKED_SIZE;
    return vkr_vk_smaa_sample(root, root.search_texture, scale * e + bias).r;
}

float vkr_vk_smaa_search_x_left(VkrVkSmaaRoot root, float2 texcoord,
                                float end)
{
    float4 metrics = vkr_vk_smaa_metrics(root);
    float2 e = float2(0.0f, 1.0f);
    while (texcoord.x > end && e.g > 0.8281f && e.r == 0.0f)
    {
        e = vkr_vk_smaa_sample(root, root.edges_texture, texcoord).rg;
        texcoord = -float2(2.0f, 0.0f) * metrics.xy + texcoord;
    }
    float offset =
        -(255.0f / 127.0f) * vkr_vk_smaa_search_length(root, e, 0.0f) + 3.25f;
    return metrics.x * offset + texcoord.x;
}

float vkr_vk_smaa_search_x_right(VkrVkSmaaRoot root, float2 texcoord,
                                 float end)
{
    float4 metrics = vkr_vk_smaa_metrics(root);
    float2 e = float2(0.0f, 1.0f);
    while (texcoord.x < end && e.g > 0.8281f && e.r == 0.0f)
    {
        e = vkr_vk_smaa_sample(root, root.edges_texture, texcoord).rg;
        texcoord = float2(2.0f, 0.0f) * metrics.xy + texcoord;
    }
    float offset =
        -(255.0f / 127.0f) * vkr_vk_smaa_search_length(root, e, 0.5f) + 3.25f;
    return -metrics.x * offset + texcoord.x;
}

float vkr_vk_smaa_search_y_up(VkrVkSmaaRoot root, float2 texcoord, float end)
{
    float4 metrics = vkr_vk_smaa_metrics(root);
    float2 e = float2(1.0f, 0.0f);
    while (texcoord.y > end && e.r > 0.8281f && e.g == 0.0f)
    {
        e = vkr_vk_smaa_sample(root, root.edges_texture, texcoord).rg;
        texcoord = -float2(0.0f, 2.0f) * metrics.xy + texcoord;
    }
    float offset =
        -(255.0f / 127.0f) * vkr_vk_smaa_search_length(root, e.gr, 0.0f) +
        3.25f;
    return metrics.y * offset + texcoord.y;
}

float vkr_vk_smaa_search_y_down(VkrVkSmaaRoot root, float2 texcoord,
                                float end)
{
    float4 metrics = vkr_vk_smaa_metrics(root);
    float2 e = float2(1.0f, 0.0f);
    while (texcoord.y < end && e.r > 0.8281f && e.g == 0.0f)
    {
        e = vkr_vk_smaa_sample(root, root.edges_texture, texcoord).rg;
        texcoord = float2(0.0f, 2.0f) * metrics.xy + texcoord;
    }
    float offset =
        -(255.0f / 127.0f) * vkr_vk_smaa_search_length(root, e.gr, 0.5f) +
        3.25f;
    return -metrics.y * offset + texcoord.y;
}

// The areas at each side of the current edge for a distance and crossings.
float2 vkr_vk_smaa_area(VkrVkSmaaRoot root, float2 dist, float e1, float e2,
                        float offset)
{
    // Rounding prevents precision errors of bilinear filtering.
    float2 texcoord = float2(VKR_SMAA_AREATEX_MAX_DISTANCE,
                             VKR_SMAA_AREATEX_MAX_DISTANCE) *
                          round(4.0f * float2(e1, e2)) +
                      dist;
    texcoord = VKR_SMAA_AREATEX_PIXEL_SIZE * texcoord +
               0.5f * VKR_SMAA_AREATEX_PIXEL_SIZE;
    texcoord.y = VKR_SMAA_AREATEX_SUBTEX_SIZE * offset + texcoord.y;
    return vkr_vk_smaa_sample(root, root.area_texture, texcoord).rg;
}

// Corner detection.
void vkr_vk_smaa_detect_horizontal_corner_pattern(VkrVkSmaaRoot root,
                                                  inout float2 weights,
                                                  float4 texcoord, float2 d)
{
    float2 left_right = step(d.xy, d.yx);
    float2 rounding = (1.0f - VKR_SMAA_CORNER_ROUNDING_NORM) * left_right;
    // Reduce blending for pixels in the center of a line.
    rounding /= left_right.x + left_right.y;

    float2 factor = float2(1.0f, 1.0f);
    factor.x -= rounding.x * vkr_vk_smaa_sample_offset(
                                 root, root.edges_texture, texcoord.xy,
                                 int2(0, 1))
                                 .r;
    factor.x -= rounding.y * vkr_vk_smaa_sample_offset(
                                 root, root.edges_texture, texcoord.zw,
                                 int2(1, 1))
                                 .r;
    factor.y -= rounding.x * vkr_vk_smaa_sample_offset(
                                 root, root.edges_texture, texcoord.xy,
                                 int2(0, -2))
                                 .r;
    factor.y -= rounding.y * vkr_vk_smaa_sample_offset(
                                 root, root.edges_texture, texcoord.zw,
                                 int2(1, -2))
                                 .r;
    weights *= saturate(factor);
}

void vkr_vk_smaa_detect_vertical_corner_pattern(VkrVkSmaaRoot root,
                                                inout float2 weights,
                                                float4 texcoord, float2 d)
{
    float2 left_right = step(d.xy, d.yx);
    float2 rounding = (1.0f - VKR_SMAA_CORNER_ROUNDING_NORM) * left_right;
    rounding /= left_right.x + left_right.y;

    float2 factor = float2(1.0f, 1.0f);
    factor.x -= rounding.x * vkr_vk_smaa_sample_offset(
                                 root, root.edges_texture, texcoord.xy,
                                 int2(1, 0))
                                 .g;
    factor.x -= rounding.y * vkr_vk_smaa_sample_offset(
                                 root, root.edges_texture, texcoord.zw,
                                 int2(1, 1))
                                 .g;
    factor.y -= rounding.x * vkr_vk_smaa_sample_offset(
                                 root, root.edges_texture, texcoord.xy,
                                 int2(-2, 0))
                                 .g;
    factor.y -= rounding.y * vkr_vk_smaa_sample_offset(
                                 root, root.edges_texture, texcoord.zw,
                                 int2(-2, 1))
                                 .g;
    weights *= saturate(factor);
}

// SMAABlendingWeightCalculationPS with SMAABlendingWeightCalculationVS's
// offsets.
float4 vkr_vk_smaa_blending_weights(VkrVkSmaaRoot root, float2 texcoord)
{
    float4 metrics = vkr_vk_smaa_metrics(root);
    float2 pixcoord = texcoord * metrics.zw;
    // Offsets for the searches (see the reference's @PSEUDO_GATHER4).
    float4 offset0 = metrics.xyxy * float4(-0.25f, -0.125f, 1.25f, -0.125f) +
                     texcoord.xyxy;
    float4 offset1 = metrics.xyxy * float4(-0.125f, -0.25f, -0.125f, 1.25f) +
                     texcoord.xyxy;
    // The ends of the search loops.
    float4 offset2 =
        metrics.xxyy * float4(-2.0f, 2.0f, -2.0f, 2.0f) *
            float(VKR_SMAA_MAX_SEARCH_STEPS) +
        float4(offset0.xz, offset1.yw);
    float4 subsample_indices = root.subsample_indices;

    float4 weights = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 e = vkr_vk_smaa_sample(root, root.edges_texture, texcoord).rg;

    if (e.g > 0.0f)
    {
        // Edge at north. Diagonals have both north and west edges, so
        // searching for them in one of the boundaries is enough.
        weights.rg =
            vkr_vk_smaa_calculate_diag_weights(root, texcoord, e,
                                               subsample_indices);
        // Diagonals take priority: a diagonal skips horizontal and vertical
        // processing.
        if (weights.r == -weights.g)
        {
            float2 d;
            // Find the distance to the left.
            float3 coords;
            coords.x = vkr_vk_smaa_search_x_left(root, offset0.xy, offset2.x);
            // offset1.y = texcoord.y - 0.25 * metrics.y (@CROSSING_OFFSET)
            coords.y = offset1.y;
            d.x = coords.x;
            // Fetch the left crossing edges, two at a time using bilinear
            // filtering; sampling at -0.25 tells each edge apart.
            float e1 =
                vkr_vk_smaa_sample(root, root.edges_texture, coords.xy).r;
            // Find the distance to the right.
            coords.z =
                vkr_vk_smaa_search_x_right(root, offset0.zw, offset2.y);
            d.y = coords.z;
            // The distances in pixel units.
            d = abs(round(metrics.zz * d - pixcoord.xx));
            // The areas texture is compressed quadratically.
            float2 sqrt_d = sqrt(d);
            // Fetch the right crossing edges.
            float e2 = vkr_vk_smaa_sample_offset(root, root.edges_texture,
                                                 coords.zy, int2(1, 0))
                           .r;
            weights.rg =
                vkr_vk_smaa_area(root, sqrt_d, e1, e2, subsample_indices.y);
            // Fix corners.
            coords.y = texcoord.y;
            float2 horizontal = weights.rg;
            vkr_vk_smaa_detect_horizontal_corner_pattern(root, horizontal,
                                                         coords.xyzy, d);
            weights.rg = horizontal;
        }
        else
        {
            // Skip vertical processing.
            e.r = 0.0f;
        }
    }

    if (e.r > 0.0f)
    {
        // Edge at west.
        float2 d;
        // Find the distance to the top.
        float3 coords;
        coords.y = vkr_vk_smaa_search_y_up(root, offset1.xy, offset2.z);
        // offset0.x = texcoord.x - 0.25 * metrics.x
        coords.x = offset0.x;
        d.x = coords.y;
        // Fetch the top crossing edges.
        float e1 = vkr_vk_smaa_sample(root, root.edges_texture, coords.xy).g;
        // Find the distance to the bottom.
        coords.z = vkr_vk_smaa_search_y_down(root, offset1.zw, offset2.w);
        d.y = coords.z;
        // The distances in pixel units.
        d = abs(round(metrics.ww * d - pixcoord.yy));
        float2 sqrt_d = sqrt(d);
        // Fetch the bottom crossing edges.
        float e2 = vkr_vk_smaa_sample_offset(root, root.edges_texture,
                                             coords.xz, int2(0, 1))
                       .g;
        weights.ba =
            vkr_vk_smaa_area(root, sqrt_d, e1, e2, subsample_indices.x);
        // Fix corners.
        coords.x = texcoord.x;
        float2 vertical = weights.ba;
        vkr_vk_smaa_detect_vertical_corner_pattern(root, vertical,
                                                   coords.xyxz, d);
        weights.ba = vertical;
    }
    return weights;
}

// SMAANeighborhoodBlendingPS with SMAANeighborhoodBlendingVS's offset.
float4 vkr_vk_smaa_neighborhood_blend(VkrVkSmaaRoot root, float2 texcoord)
{
    float4 metrics = vkr_vk_smaa_metrics(root);
    float4 offset = metrics.xyxy * float4(1.0f, 0.0f, 0.0f, 1.0f) +
                    texcoord.xyxy;
    // Fetch the blending weights for the current pixel.
    float4 a;
    a.x = vkr_vk_smaa_sample(root, root.blend_texture, offset.xy).a;  // Right
    a.y = vkr_vk_smaa_sample(root, root.blend_texture, offset.zw).g;  // Top
    a.wz = vkr_vk_smaa_sample(root, root.blend_texture, texcoord).xz; // Bottom / Left

    if (dot(a, float4(1.0f, 1.0f, 1.0f, 1.0f)) < 1e-5f)
    {
        float4 color = vkr_vk_smaa_sample(root, root.source_texture, texcoord);
        float2 velocity =
            vkr_vk_smaa_sample(root, root.motion_texture, texcoord).rg;
        // Pack velocity into the alpha channel for the T2x resolve.
        color.a = sqrt(5.0f * length(velocity));
        return color;
    }

    // max(horizontal) > max(vertical)
    bool h = max(a.x, a.z) > max(a.y, a.w);
    // Calculate the blending offsets.
    float4 blending_offset = float4(0.0f, a.y, 0.0f, a.w);
    float2 blending_weight = a.yw;
    vkr_vk_smaa_movc(bool4(h, h, h, h), blending_offset,
                     float4(a.x, 0.0f, a.z, 0.0f));
    vkr_vk_smaa_movc(bool2(h, h), blending_weight, a.xz);
    blending_weight /= dot(blending_weight, float2(1.0f, 1.0f));
    // Calculate the texture coordinates.
    float4 blending_coord =
        blending_offset * float4(metrics.xy, -metrics.xy) + texcoord.xyxy;
    // Bilinear filtering mixes the current pixel with the chosen neighbour.
    float4 color = blending_weight.x * vkr_vk_smaa_sample(
                                           root, root.source_texture,
                                           blending_coord.xy);
    color += blending_weight.y *
             vkr_vk_smaa_sample(root, root.source_texture, blending_coord.zw);
    // Antialias velocity for proper reprojection in the resolve.
    float2 velocity =
        blending_weight.x *
        vkr_vk_smaa_sample(root, root.motion_texture, blending_coord.xy).rg;
    velocity += blending_weight.y * vkr_vk_smaa_sample(root, root.motion_texture,
                                                       blending_coord.zw)
                                        .rg;
    color.a = sqrt(5.0f * length(velocity));
    return color;
}

// SMAA_REPROJECTION_WEIGHT_SCALE: velocity weighting removes the temporal
// supersample where two subsamples moved differently.
#define VKR_SMAA_REPROJECTION_WEIGHT_SCALE 30.0f

// Mirrors VkrVulkanSmaaResolveRoot.
struct VkrVkSmaaResolveRoot
{
    uint current_texture;
    uint previous_texture;
    uint motion_texture;
    uint accept_texture;
    uint destination_texture;
    uint linear_sampler;
    uint point_sampler;
    uint history_valid;
    uint2 extent;
    uint2 reserved;
};

struct VkrVkSmaaResolvePush
{
    VkrVkSmaaResolveRoot *root;
    uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<VkrVkSmaaResolvePush> g_smaa_resolve_push;

// SMAAResolvePS with SMAA_REPROJECTION: blends this frame's antialiased
// subsample with last frame's, reprojected. VKR's motion already points from
// the current to the previous position, so it is not negated as the
// reference's motion-blur velocity is. The scene-linear resolve's accept mask
// also rejects a different surface, which velocity alone misses on surfaces
// without motion vectors.
[shader("compute")]
[numthreads(8, 8, 1)]
void vk_smaa_resolve(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSmaaResolveRoot root = vkr_vk_uniform_root(*g_smaa_resolve_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    float2 texcoord = (float2(thread_id.xy) + 0.5f) / float2(root.extent);
    SamplerState point_sampler =
        g_samplers[NonUniformResourceIndex(root.point_sampler)];
    float4 current =
        g_textures[NonUniformResourceIndex(root.current_texture)].SampleLevel(
            point_sampler, texcoord, 0.0f);
    float4 resolved = current;
    if (root.history_valid != 0u)
    {
        float2 velocity =
            g_textures[NonUniformResourceIndex(root.motion_texture)]
                .SampleLevel(point_sampler, texcoord, 0.0f)
                .rg;
        float accept =
            g_textures[NonUniformResourceIndex(root.accept_texture)]
                .SampleLevel(point_sampler, texcoord, 0.0f)
                .r;
        float4 previous =
            g_textures[NonUniformResourceIndex(root.previous_texture)]
                .SampleLevel(point_sampler, texcoord + velocity, 0.0f);
        // Attenuate the previous pixel if the velocity is different.
        float delta = abs(current.a * current.a - previous.a * previous.a) / 5.0f;
        float weight =
            0.5f *
            saturate(1.0f - sqrt(delta) * VKR_SMAA_REPROJECTION_WEIGHT_SCALE) *
            accept;
        resolved = lerp(current, previous, weight);
    }
    g_storage_2d[NonUniformResourceIndex(root.destination_texture)]
        [thread_id.xy] = float4(resolved.rgb, 1.0f);
}

float2 vkr_vk_smaa_texcoord(uint2 pixel, uint2 extent)
{
    return (float2(pixel) + 0.5f) / float2(extent);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_smaa_edges(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSmaaRoot root = vkr_vk_uniform_root(*g_smaa_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    float2 edges = vkr_vk_smaa_luma_edges(
        root, vkr_vk_smaa_texcoord(thread_id.xy, root.extent));
    g_storage_2d[NonUniformResourceIndex(root.destination_texture)]
        [thread_id.xy] = float4(edges, 0.0f, 0.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_smaa_weights(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSmaaRoot root = vkr_vk_uniform_root(*g_smaa_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    g_storage_2d[NonUniformResourceIndex(root.destination_texture)]
        [thread_id.xy] = vkr_vk_smaa_blending_weights(
            root, vkr_vk_smaa_texcoord(thread_id.xy, root.extent));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void vk_smaa_blend(uint3 thread_id : SV_DispatchThreadID)
{
    VkrVkSmaaRoot root = vkr_vk_uniform_root(*g_smaa_push.root);
    if (any(thread_id.xy >= root.extent))
        return;
    g_storage_2d[NonUniformResourceIndex(root.destination_texture)]
        [thread_id.xy] = vkr_vk_smaa_neighborhood_blend(
            root, vkr_vk_smaa_texcoord(thread_id.xy, root.extent));
}
