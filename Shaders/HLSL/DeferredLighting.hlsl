// D3D11 single-sample deferred resolve. Layout mirrors DeferredResolveConstants.
// Shadow visibility, area lights and IBL are not implemented by this pass.
struct ResolveLight
{
    float4 Position; // w: 0 directional, 1 point, 2 spot
    float4 Direction; // w: full cone angle in radians
    float4 Color; // w: intensity
    float4 Attenuation; // constant, linear, quadratic, range
};

cbuffer DeferredResolveConstants : register(b0)
{
    row_major float4x4 InverseViewProjection;
    float3 CameraPosition;
    uint LightCount;
    float4 AmbientColor;
    float4 TargetSize;
    ResolveLight Lights[64];
};

Texture2D<float4> AlbedoBuffer : register(t0);
Texture2D<float4> NormalBuffer : register(t1);
Texture2D<float4> MaterialBuffer : register(t2);
Texture2D<float> DepthBuffer : register(t3);

float4 VSMain(uint vertex : SV_VertexID) : SV_POSITION
{
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

float3 SafeNormalize(float3 value)
{
    return value * rsqrt(max(dot(value, value), 1e-12f));
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET
{
    int3 texel = int3(position.xy, 0);
    float depth = DepthBuffer.Load(texel);
    // Preserve the existing background, including sky rendered before this pass.
    if (depth >= 1.0f)
        discard;
    float2 uv = position.xy * TargetSize.zw;
    float4 world = mul(float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), depth, 1.0f),
                       InverseViewProjection);
    float3 worldPosition = world.xyz / world.w;
    float4 albedo = AlbedoBuffer.Load(texel);
    float4 material = MaterialBuffer.Load(texel);
    float3 normal = SafeNormalize(NormalBuffer.Load(texel).xyz * 2.0f - 1.0f);
    float3 view = SafeNormalize(CameraPosition - worldPosition);
    float metallic = saturate(material.x);
    float roughness = clamp(material.y, 0.04f, 1.0f);
    float3 f0 = lerp(0.04f.xxx, albedo.rgb, metallic);
    float3 color = albedo.rgb * (AmbientColor.rgb * (1.0f - metallic) + max(material.z, 0.0f));
    const float pi = 3.14159265f;

    for (uint i = 0; i < min(LightCount, 64u); ++i)
    {
        ResolveLight light = Lights[i];
        float3 toLight = -light.Direction.xyz;
        float attenuation = 1.0f;
        if (light.Position.w > 0.5f)
        {
            toLight = light.Position.xyz - worldPosition;
            float distance = length(toLight);
            if (light.Attenuation.w <= 0.0f || distance >= light.Attenuation.w)
                continue;
            attenuation = rcp(max(dot(light.Attenuation.xyz, float3(1.0f, distance, distance * distance)), 1e-4f));
            if (light.Position.w > 1.5f)
            {
                float cone = dot(SafeNormalize(-toLight), SafeNormalize(light.Direction.xyz));
                float cutoff = cos(light.Direction.w * 0.5f);
                attenuation *= saturate((cone - cutoff) / max(1.0f - cutoff, 1e-4f));
            }
        }
        float3 l = SafeNormalize(toLight);
        float3 h = SafeNormalize(l + view);
        float nl = saturate(dot(normal, l));
        float nv = max(saturate(dot(normal, view)), 1e-4f);
        float nh = saturate(dot(normal, h));
        float vh = saturate(dot(view, h));
        float a = roughness * roughness;
        float a2 = a * a;
        float denominator = nh * nh * (a2 - 1.0f) + 1.0f;
        float distribution = a2 / max(pi * denominator * denominator, 1e-6f);
        float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
        float geometry = (nv / (nv * (1.0f - k) + k)) * (nl / (nl * (1.0f - k) + k));
        float3 fresnel = f0 + (1.0f - f0) * pow(1.0f - vh, 5.0f);
        float3 specular = distribution * geometry * fresnel / max(4.0f * nv * nl, 1e-4f);
        float3 diffuse = (1.0f - fresnel) * (1.0f - metallic) * albedo.rgb / pi;
        color += (diffuse + specular) * max(light.Color.rgb, 0.0f) * max(light.Color.w, 0.0f) * attenuation * nl;
    }
    return float4(color, albedo.a * saturate(material.w));
}
