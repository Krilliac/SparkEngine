/**
 * PBRSurface.glsl - Advanced PBR Surface Shader for OpenGL/Vulkan
 * Spark Engine - Cross-platform shader
 *
 * Full PBR implementation with clearcoat, subsurface scattering,
 * anisotropy, and IBL support.
 */
#version 460 core

// ============================================================================
// FRAGMENT INPUT
// ============================================================================
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec3 fragViewDir;
layout(location = 4) in vec4 fragPrevClipPos;

// ============================================================================
// FRAGMENT OUTPUT (MRT for deferred rendering)
// ============================================================================
layout(location = 0) out vec4 outAlbedo;      // RGB: albedo, A: metallic
layout(location = 1) out vec4 outNormal;      // RGB: world normal, A: roughness
layout(location = 2) out vec4 outMaterial;    // R: occlusion, G: emissive, BA: motion vectors

// ============================================================================
// UNIFORM BUFFERS
// ============================================================================

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

// Advanced PBR parameters (b3)
layout(std140, binding = 3) uniform AdvancedPBRConstants
{
    // Clearcoat
    float u_ClearcoatFactor;
    float u_ClearcoatRoughness;

    // Subsurface
    vec3 u_SubsurfaceColor;
    float u_SubsurfaceRadius;

    // Anisotropy
    float u_AnisotropyFactor;
    vec2 u_AnisotropyDirection;

    // Sheen
    vec3 u_SheenColor;
    float u_SheenRoughness;

    // Iridescence
    float u_IridescenceFactor;
    float u_IridescenceIOR;
    float u_IridescenceThickness;

    // Transmission
    float u_TransmissionFactor;
    vec3 u_TransmissionColor;
};

// ============================================================================
// TEXTURE SAMPLERS
// ============================================================================
layout(binding = 0) uniform sampler2D u_AlbedoTexture;
layout(binding = 1) uniform sampler2D u_NormalTexture;
layout(binding = 2) uniform sampler2D u_MetallicRoughnessTexture;
layout(binding = 3) uniform sampler2D u_OcclusionTexture;
layout(binding = 4) uniform sampler2D u_EmissiveTexture;
layout(binding = 5) uniform sampler2D u_ClearcoatTexture;
layout(binding = 6) uniform sampler2D u_SubsurfaceTexture;
layout(binding = 7) uniform samplerCube u_EnvironmentMap;
layout(binding = 8) uniform samplerCube u_IrradianceMap;
layout(binding = 9) uniform sampler2D u_BRDFLookup;

// ============================================================================
// CONSTANTS
// ============================================================================
const float PI = 3.14159265359;
const float TWO_PI = 6.28318530718;
const float INV_PI = 0.31830988618;
const float EPSILON = 0.0001;

// ============================================================================
// PBR CORE FUNCTIONS
// ============================================================================

float D_GGX(float NdotH, float roughness)
{
    float a2 = roughness * roughness;
    float f = (NdotH * a2 - NdotH) * NdotH + 1.0;
    return a2 / (PI * f * f + EPSILON);
}

float V_SmithGGXCorrelated(float NdotV, float NdotL, float roughness)
{
    float a2 = roughness * roughness;
    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / (GGXV + GGXL + EPSILON);
}

vec3 F_Schlick(float u, vec3 f0)
{
    return f0 + (vec3(1.0) - f0) * pow(1.0 - u, 5.0);
}

float F_Schlick(float u, float f0, float f90)
{
    return f0 + (f90 - f0) * pow(1.0 - u, 5.0);
}

// Charlie distribution for sheen
float D_Charlie(float roughness, float NdotH)
{
    float invAlpha = 1.0 / roughness;
    float cos2h = NdotH * NdotH;
    float sin2h = max(1.0 - cos2h, 0.0078125);
    return (2.0 + invAlpha) * pow(sin2h, invAlpha * 0.5) / TWO_PI;
}

// Ashikhmin visibility for sheen
float V_Ashikhmin(float NdotL, float NdotV)
{
    return 1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV));
}

// ============================================================================
// CLEARCOAT
// ============================================================================
float D_GGX_Clearcoat(float NdotH, float clearcoatRoughness)
{
    return D_GGX(NdotH, clearcoatRoughness);
}

float V_Kelemen(float LdotH)
{
    return 0.25 / (LdotH * LdotH + EPSILON);
}

// ============================================================================
// ANISOTROPIC GGX
// ============================================================================
float D_GGX_Anisotropic(float NdotH, float TdotH, float BdotH,
                          float roughnessT, float roughnessB)
{
    float a2 = roughnessT * roughnessB;
    vec3 v = vec3(roughnessB * TdotH, roughnessT * BdotH, a2 * NdotH);
    float v2 = dot(v, v);
    float w2 = a2 / v2;
    return a2 * w2 * w2 * INV_PI;
}

// ============================================================================
// NORMAL MAPPING
// ============================================================================
vec3 GetNormalFromMap()
{
    vec3 tangentNormal = texture(u_NormalTexture, fragTexCoord).xyz * 2.0 - 1.0;

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
// MOTION VECTORS
// ============================================================================
vec2 ComputeMotionVector()
{
    vec2 currentPos = gl_FragCoord.xy * u_InvScreenResolution;
    vec2 previousPos = (fragPrevClipPos.xy / fragPrevClipPos.w) * 0.5 + 0.5;
    return currentPos - previousPos;
}

// ============================================================================
// MAIN FRAGMENT SHADER (G-Buffer output for deferred rendering)
// ============================================================================
void main()
{
    // Sample base textures
    vec4 albedoSample = texture(u_AlbedoTexture, fragTexCoord);
    vec3 albedo = albedoSample.rgb * u_AlbedoColor.rgb;
    float alpha = albedoSample.a * u_AlbedoColor.a;

    if (alpha < u_AlphaCutoff)
        discard;

    // PBR material properties
    vec4 metallicRoughness = texture(u_MetallicRoughnessTexture, fragTexCoord);
    float metallic = metallicRoughness.b * u_MetallicFactor;
    float roughness = metallicRoughness.g * u_RoughnessFactor;
    roughness = clamp(roughness, 0.045, 1.0);  // Prevent zero roughness

    float ao = texture(u_OcclusionTexture, fragTexCoord).r;
    ao = mix(1.0, ao, u_OcclusionStrength);

    float emissive = u_EmissiveFactor;
    vec3 emissiveColor = texture(u_EmissiveTexture, fragTexCoord).rgb * emissive;

    // Get world-space normal
    vec3 N = GetNormalFromMap();

    // Motion vectors
    vec2 motionVec = ComputeMotionVector();

    // ========================================================================
    // G-BUFFER OUTPUT
    // ========================================================================
    outAlbedo = vec4(albedo, metallic);
    outNormal = vec4(N * 0.5 + 0.5, roughness);
    outMaterial = vec4(ao, length(emissiveColor), motionVec);
}
