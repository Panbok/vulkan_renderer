// The three passes share main-stream transmitting indirect draws. Depth and RGB
// prefixes belong to one retained face and are refreshed in the same schedule.
struct VkrVkLocalShadowTransmissionRoot
{
    VkrVkRasterRoot raster;
    VkrVulkanTransmissionMaterial *transmission_materials;
    uint previous_color_texture;
    uint reserved;
    float4 light_position;
};

struct VkrVkLocalShadowTransmissionPush
{
    VkrVkLocalShadowTransmissionRoot *root;
    uint bucket;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<VkrVkLocalShadowTransmissionPush>
    g_local_shadow_transmission_push;

struct VkrVkLocalShadowTransmissionOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint visible_index : TEXCOORD1;
    float3 world : TEXCOORD2;
    float4 tangent : TEXCOORD3;
};

[shader("vertex")]
VkrVkLocalShadowTransmissionOutput vk_local_shadow_transmission_vertex(
    uint vertex_id : SV_VertexID, uint draw_index : SV_DrawIndex)
{
    VkrVkLocalShadowTransmissionRoot *root = g_local_shadow_transmission_push.root;
    VkrVkRasterRoot raster = root.raster;
    uint visible_instance = raster.states[raster.view_index]
        .execution_ranges[g_local_shadow_transmission_push.bucket].x + draw_index;
    VkrGpuVisibleDrawRow visible = raster.visible_rows[
        raster.view_index * raster.visible_capacity + visible_instance];
    VkrVkGeometryRow geometry = raster.geometry_rows[visible.geometry_index];
    VkrGpuDecodedVertex vertex = vkr_decode_packed_vertex(
        geometry.vertices[geometry.first_vertex + vertex_id],
        geometry.decode[visible.decode_index]);
    VkrVulkanInstance instance = raster.frame.instances[visible.instance_index];
    VkrVkLocalShadowTransmissionOutput output;
    output.world = mul(instance.model, float4(vertex.position, 1.0f)).xyz;
    output.position = mul(raster.frame.view_projection, float4(output.world, 1.0f));
    output.uv = vertex.texcoord;
    output.color = vertex.color;
    output.visible_index = visible_instance;
    output.tangent = float4(
        mul(instance.model, float4(vertex.tangent.xyz, 0.0f)).xyz,
        vertex.tangent.w * instance.normal_column0.w);
    return output;
}

float4 vkr_vk_local_shadow_material_sample(uint texture, uint sampler_index,
                                           float2 uv, float2 uv_dx, float2 uv_dy)
{
    return g_textures[NonUniformResourceIndex(texture)].SampleGrad(
        g_samplers[NonUniformResourceIndex(sampler_index)], uv, uv_dx, uv_dy);
}

void vkr_vk_local_shadow_peel(VkrVkRasterRoot raster, float4 position)
{
    if (raster.previous_depth_texture == 0xffffffffu)
        return;
    float previous = g_depth_array_textures[raster.previous_depth_texture].Load(
        int4(int2(position.xy), raster.previous_depth_layer, 0));
    // One D32 ULP rejects the already stored surface without merging distant
    // thin glass surfaces with a fixed perspective-depth epsilon.
    if (position.z <= asfloat(asuint(previous) + 1u))
        discard;
}

[shader("fragment")]
void vk_local_shadow_transmission_overflow_fragment(
    VkrVkLocalShadowTransmissionOutput input)
{
    VkrVkRasterRoot raster = g_local_shadow_transmission_push.root.raster;
    vkr_vk_local_shadow_peel(raster, input.position);
    VkrGpuVisibleDrawRow visible = raster.visible_rows[
        raster.view_index * raster.visible_capacity + input.visible_index];
    VkrVulkanMaterial material = raster.frame.materials[visible.material_index];
    if (material.alpha_mode == 1u)
    {
        float alpha = vkr_vk_local_shadow_material_sample(
            material.base_color_texture, material.base_color_sampler,
            input.uv, ddx(input.uv), ddy(input.uv)).a *
            material.tint.a * input.color.a;
        if (alpha < material.material_alpha.x)
            discard;
    }
}

[shader("fragment")]
float4 vk_local_shadow_transmission_fragment(
    VkrVkLocalShadowTransmissionOutput input) : SV_Target0
{
    VkrVkLocalShadowTransmissionRoot *root = g_local_shadow_transmission_push.root;
    VkrVkRasterRoot raster = root.raster;
    float2 uv_dx = ddx(input.uv);
    float2 uv_dy = ddy(input.uv);
    float3 normal = normalize(cross(ddx(input.world), ddy(input.world)));
    float3 light = normalize(root.light_position.xyz - input.world);
    normal = dot(normal, light) < 0.0f ? -normal : normal;
    vkr_vk_local_shadow_peel(raster, input.position);
    VkrGpuVisibleDrawRow visible = raster.visible_rows[
        raster.view_index * raster.visible_capacity + input.visible_index];
    VkrVulkanMaterial material = raster.frame.materials[visible.material_index];
    VkrVulkanInstance instance = raster.frame.instances[visible.instance_index];
    float4 base = vkr_vk_local_shadow_material_sample(
        material.base_color_texture, material.base_color_sampler,
        input.uv, uv_dx, uv_dy) * material.tint * input.color;
    if (material.alpha_mode == 1u && base.a < material.material_alpha.x)
        discard;

    VkrVulkanTransmissionMaterial transmitting =
        root.transmission_materials[visible.material_index];
    float transmission = saturate(material.material_alpha.y);
    if ((material.flags & 8u) != 0u)
        transmission *= vkr_vk_local_shadow_material_sample(
            transmitting.transmission_texture, transmitting.transmission_sampler,
            input.uv, uv_dx, uv_dy).r;
    float thickness = max(material.material_alpha.w, 0.0f);
    if ((material.flags & 16u) != 0u)
        thickness *= vkr_vk_local_shadow_material_sample(
            transmitting.thickness_texture, transmitting.thickness_sampler,
            input.uv, uv_dx, uv_dy).g;
    VkrTransmissionExit exit = vkr_transmission_exit_point(
        input.world, root.light_position.xyz, normal,
        mul(instance.model, float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz,
        mul(instance.model, float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz,
        mul(instance.model, float4(0.0f, 0.0f, 1.0f, 0.0f)).xyz,
        material.material_alpha.z, max(thickness, 0.0f));
    float metallic = saturate(material.material_surface.x);
    float roughness = saturate(material.material_surface.y);
    if ((material.flags & 2u) != 0u)
    {
        float3 orm = vkr_vk_local_shadow_material_sample(
            material.orm_texture, material.orm_sampler,
            input.uv, uv_dx, uv_dy).rgb;
        metallic = saturate(metallic * orm.b);
        roughness = saturate(roughness * orm.g);
    }
    roughness = clamp(roughness, 0.04f, 1.0f);
    float no_l = saturate(dot(normal, light));
    float3 f0 = lerp(saturate(material.material_dielectric_specular.rgb),
                     base.rgb, metallic);
    VkrGgxMaterialEnergy energy = packet_prepare_brdf(raster.frame, no_l, roughness, f0);
    float anisotropy_strength = material.material_anisotropy.x;
    if (anisotropy_strength > 0.0f)
    {
        float3 map = float3(1.0f, 0.5f, 1.0f);
        if ((material.flags & 1024u) != 0u)
            map = vkr_vk_local_shadow_material_sample(
                material.anisotropy_texture, material.anisotropy_sampler,
                input.uv, uv_dx, uv_dy).rgb;
        anisotropy_strength *= map.b;
        float3 axis = vkr_anisotropy_axis(normal, normal, input.tangent,
            map.rg * 2.0f - 1.0f, material.material_anisotropy.yz);
        if (anisotropy_strength > 0.0f)
            energy = packet_prepare_anisotropy(raster.frame, normal, axis, light,
                                               roughness, anisotropy_strength, f0);
    }
    float3 reflectance = energy.reflectance;
    float3 sheen_color = max(material.material_sheen.rgb, 0.0f);
    if (vkr_sheen_active(sheen_color) && (material.flags & 256u) != 0u)
        sheen_color *= vkr_vk_local_shadow_material_sample(
            material.sheen_color_texture, material.sheen_color_sampler,
            input.uv, uv_dx, uv_dy).rgb;
    if (vkr_sheen_active(sheen_color))
    {
        float sheen_roughness = material.material_sheen.w;
        if ((material.flags & 512u) != 0u)
            sheen_roughness *= vkr_vk_local_shadow_material_sample(
                material.sheen_roughness_texture, material.sheen_roughness_sampler,
                input.uv, uv_dx, uv_dy).a;
        VkrSheenLtcSample ltc = packet_sheen_ltc_sample(raster.frame, no_l, sheen_roughness);
        VkrSheenLayer sheen = packet_prepare_sheen(
            raster.frame, sheen_color, sheen_roughness, normal, light,
            ltc.allocation_reserve);
        reflectance = sheen.color * sheen.directional_albedo +
                      sheen.base_transmission * reflectance;
    }
    float coat_factor = saturate(material.material_clearcoat.x);
    if (coat_factor > 0.0f && (material.flags & 32u) != 0u)
        coat_factor *= vkr_vk_local_shadow_material_sample(
            material.clearcoat_texture, material.clearcoat_sampler,
            input.uv, uv_dx, uv_dy).r;
    if (vkr_clearcoat_active(coat_factor))
    {
        float coat_roughness = material.material_clearcoat.y;
        if ((material.flags & 64u) != 0u)
            coat_roughness *= vkr_vk_local_shadow_material_sample(
                material.clearcoat_roughness_texture, material.clearcoat_roughness_sampler,
                input.uv, uv_dx, uv_dy).g;
        VkrClearcoatLayer coat = packet_prepare_clearcoat(
            raster.frame, coat_factor,
            clamp(coat_roughness, VKR_CLEARCOAT_MIN_ROUGHNESS, 1.0f), normal, light);
        reflectance = coat.factor * coat.energy.reflectance +
                      coat.base_transmission * reflectance;
    }
    float3 weight = vkr_local_shadow_transmission_weight(
        base.rgb, metallic, transmission, reflectance,
        material.material_attenuation_color.rgb,
        material.material_attenuation_color.w, exit.path_length);
    if (root.previous_color_texture != 0xffffffffu)
        weight *= g_array_textures[root.previous_color_texture].Load(
            int4(int2(input.position.xy), raster.previous_depth_layer, 0)).rgb;
    return float4(weight, 1.0f);
}
