/**
 * BasicPS.glsl - Basic Pixel/Fragment Shader (OpenGL/Vulkan GLSL equivalent of HLSL BasicPS)
 * Spark Engine - Cross-platform shader
 *
 * GLSL 460 Core with PBR lighting support.
 * Matches the HLSL BasicPS.hlsl constant buffer layout.
 */
#version 460 core

// ============================================================================
// FRAGMENT INPUT (from vertex shader)
// ============================================================================
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec3 fragViewDir;
layout(location = 4) in vec4 fragPrevClipPos;

// ============================================================================
// FRAGMENT OUTPUT
// ============================================================================
layout(location = 0) out vec4 outColor;

// ============================================================================
// UNIFORM BUFFERS
// ============================================================================

// Per-Frame Constants (b0)
layout(std140, binding = 0) uniform PerFrameConstants
{
    mat4 u_ViewMatrix;
    mat4 u_ProjectionMatrix;
    mat4 u_ViewProjectionMatrix;
    vec3 u_CameraPosition;
    float u_Time;
    vec3 u_CameraDirection;
    float u_DeltaTime;
    vec2 u_ScreenResolution;
    vec2 u_InvScreenResolution;

    vec3 u_DirectionalLightDir;
    float u_DirectionalLightIntensity;
    vec3 u_DirectionalLightColor;
    float u_AmbientIntensity;
    vec3 u_AmbientColor;
    float _padding1;
};

// Per-Object Constants (b1)
layout(std140, binding = 1) uniform PerObjectConstants
{
    mat4 u_WorldMatrix;
    mat4 u_WorldViewProjectionMatrix;
    mat4 u_WorldInverseTransposeMatrix;
    mat4 u_PreviousWorldMatrix;
    vec3 u_ObjectPosition;
    float u_ObjectScale;
    vec4 u_ObjectColor;
    vec4 u_MaterialProperties;
    vec4 u_UVTiling;
};

// Per-Material Constants (b2)
layout(std140, binding = 2) uniform PerMaterialConstants
{
    vec4 u_AlbedoColor;
    float u_MetallicFactor;
    float u_RoughnessFactor;
    float u_NormalScale;
    float u_OcclusionStrength;
    float u_EmissiveFactor;
    float u_AlphaCutoff;
    vec2 _materialPadding;
};

// ============================================================================
// TEXTURE SAMPLERS
// ============================================================================
layout(binding = 0) uniform sampler2D u_AlbedoTexture;
layout(binding = 1) uniform sampler2D u_NormalTexture;
layout(binding = 2) uniform sampler2D u_MetallicRoughnessTexture;
layout(binding = 3) uniform sampler2D u_OcclusionTexture;
layout(binding = 4) uniform sampler2D u_EmissiveTexture;

// ============================================================================
// PBR CONSTANTS
// ============================================================================
const float PI = 3.14159265359;
const float EPSILON = 0.0001;
const vec3 F0_DIELECTRIC = vec3(0.04);

// ============================================================================
// PBR FUNCTIONS
// ============================================================================

// Normal Distribution Function (GGX/Trowbridge-Reitz)
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;

    return a2 / max(denom, EPSILON);
}

// Geometry Function (Schlick-GGX)
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith's method for geometry
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

// Fresnel (Schlick approximation)
vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel with roughness for IBL
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) *
           pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ============================================================================
// NORMAL MAPPING
// ============================================================================
vec3 GetNormalFromMap()
{
    vec3 tangentNormal = texture(u_NormalTexture, fragTexCoord).xyz * 2.0 - 1.0;

    // Calculate TBN matrix from derivatives
    vec3 Q1 = dFdx(fragWorldPos);
    vec3 Q2 = dFdy(fragWorldPos);
    vec2 st1 = dFdx(fragTexCoord);
    vec2 st2 = dFdy(fragTexCoord);

    vec3 N = normalize(fragNormal);
    vec3 T = normalize(Q1 * st2.t - Q2 * st1.t);
    vec3 B = -normalize(cross(N, T));
    mat3 TBN = mat3(T, B, N);

    return normalize(TBN * (tangentNormal * vec3(u_NormalScale, u_NormalScale, 1.0)));
}

// ============================================================================
// TONEMAPPING
// ============================================================================
vec3 ACESFilm(vec3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// ============================================================================
// MAIN FRAGMENT SHADER
// ============================================================================
void main()
{
    // Sample textures
    vec4 albedoSample = texture(u_AlbedoTexture, fragTexCoord);
    vec3 albedo = albedoSample.rgb * u_AlbedoColor.rgb * u_ObjectColor.rgb;
    float alpha = albedoSample.a * u_AlbedoColor.a * u_ObjectColor.a;

    // Alpha cutoff test
    if (alpha < u_AlphaCutoff)
        discard;

    // PBR properties
    float metallic = u_MetallicFactor * u_MaterialProperties.x;
    float roughness = u_RoughnessFactor * u_MaterialProperties.y;
    float ao = texture(u_OcclusionTexture, fragTexCoord).r * u_OcclusionStrength;

    // Normals
    vec3 N = GetNormalFromMap();
    vec3 V = normalize(fragViewDir);

    // Calculate F0 (reflectance at normal incidence)
    vec3 F0 = mix(F0_DIELECTRIC, albedo, metallic);

    // ========================================================================
    // DIRECT LIGHTING (Directional Light)
    // ========================================================================
    vec3 Lo = vec3(0.0);

    vec3 L = normalize(-u_DirectionalLightDir);
    vec3 H = normalize(V + L);
    vec3 radiance = u_DirectionalLightColor * u_DirectionalLightIntensity;

    // Cook-Torrance BRDF
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + EPSILON;
    vec3 specular = numerator / denominator;

    // Energy conservation
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);
    Lo += (kD * albedo / PI + specular) * radiance * NdotL;

    // ========================================================================
    // AMBIENT LIGHTING
    // ========================================================================
    vec3 ambient = u_AmbientColor * u_AmbientIntensity * albedo * ao;

    // Emissive
    vec3 emissive = texture(u_EmissiveTexture, fragTexCoord).rgb *
                    u_EmissiveFactor * u_MaterialProperties.z;

    // ========================================================================
    // FINAL COLOR
    // ========================================================================
    vec3 color = ambient + Lo + emissive;

    // Tone mapping (ACES)
    color = ACESFilm(color);

    // Gamma correction
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, alpha);
}
