/**
 * Phong.glsl - Blinn-Phong Lighting Shader
 * Spark Engine - Cross-platform shader (GLSL equivalent of HLSL Phong)
 *
 * GLSL 460 Core - Classic Blinn-Phong shading model.
 * Contains both vertex and fragment stages separated by preprocessor.
 */
#version 460 core

#ifdef VERTEX_SHADER

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragWorldNormal;
layout(location = 1) out vec3 fragViewDir;

layout(std140, binding = 1) uniform PhongConstants
{
    mat4 u_WorldMatrix;
    mat4 u_ViewProjection;
    vec3 u_LightDirection;
    float _phongPad1;
    vec3 u_LightColor;
    float u_SpecularPower;
};

void main()
{
    vec4 worldPos = u_WorldMatrix * vec4(inPosition, 1.0);
    gl_Position = u_ViewProjection * worldPos;
    fragWorldNormal = normalize(mat3(u_WorldMatrix) * inNormal);
    fragViewDir = normalize(-worldPos.xyz);
}

#endif // VERTEX_SHADER

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec3 fragWorldNormal;
layout(location = 1) in vec3 fragViewDir;

layout(location = 0) out vec4 outColor;

layout(std140, binding = 1) uniform PhongConstants
{
    mat4 u_WorldMatrix;
    mat4 u_ViewProjection;
    vec3 u_LightDirection;
    float _phongPad1;
    vec3 u_LightColor;
    float u_SpecularPower;
};

void main()
{
    vec3 N = normalize(fragWorldNormal);
    vec3 L = normalize(-u_LightDirection);
    vec3 V = normalize(fragViewDir);
    vec3 H = normalize(L + V);

    // Diffuse
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = NdotL * u_LightColor;

    // Specular (Blinn-Phong)
    float NdotH = max(dot(N, H), 0.0);
    float spec = pow(NdotH, u_SpecularPower);
    vec3 specular = spec * u_LightColor;

    outColor = vec4(diffuse + specular, 1.0);
}

#endif // FRAGMENT_SHADER
