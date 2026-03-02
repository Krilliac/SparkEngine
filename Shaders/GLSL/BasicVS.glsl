/**
 * BasicVS.glsl - Basic Vertex Shader (OpenGL/Vulkan GLSL equivalent of HLSL BasicVS)
 * Spark Engine - Cross-platform shader
 *
 * GLSL 460 Core with Vulkan SPIR-V compatibility.
 * Matches the HLSL BasicVS.hlsl constant buffer layout.
 */
#version 460 core

// ============================================================================
// VERTEX INPUT
// ============================================================================
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

// ============================================================================
// VERTEX OUTPUT
// ============================================================================
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec3 fragViewDir;
layout(location = 4) out vec4 fragPrevClipPos;

// ============================================================================
// UNIFORM BUFFERS (matching HLSL constant buffer slots)
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

    // Lighting parameters
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
    vec4 u_MaterialProperties;  // x: metallic, y: roughness, z: emissive, w: alpha
    vec4 u_UVTiling;            // xy: tiling, zw: offset
};

// ============================================================================
// MAIN VERTEX SHADER
// ============================================================================
void main()
{
    // Transform position to world space
    vec4 worldPos = u_WorldMatrix * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;

    // Transform position to clip space
    gl_Position = u_WorldViewProjectionMatrix * vec4(inPosition, 1.0);

    // Transform normal to world space (using inverse transpose for non-uniform scaling)
    fragNormal = normalize(mat3(u_WorldInverseTransposeMatrix) * inNormal);

    // Apply UV tiling and offset
    fragTexCoord = inTexCoord * u_UVTiling.xy + u_UVTiling.zw;

    // Calculate view direction
    fragViewDir = normalize(u_CameraPosition - worldPos.xyz);

    // Previous frame clip position for motion vectors / TAA
    fragPrevClipPos = u_ViewProjectionMatrix * u_PreviousWorldMatrix * vec4(inPosition, 1.0);
}
