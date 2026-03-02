/**
 * Rim.glsl - Rim Lighting Shader
 * Spark Engine - Cross-platform shader (GLSL equivalent of HLSL Rim)
 *
 * GLSL 460 Core - Fresnel-based rim/edge lighting effect.
 * Contains both vertex and fragment stages separated by preprocessor.
 */
#version 460 core

#ifdef VERTEX_SHADER

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragViewDir;

layout(std140, binding = 1) uniform RimConstants
{
    mat4 u_WorldViewProjection;
    vec3 u_CameraPosition;
    float u_RimPower;
    vec3 u_RimColor;
    float u_RimIntensity;
};

void main()
{
    gl_Position = u_WorldViewProjection * vec4(inPosition, 1.0);
    fragNormal = normalize(inNormal);
    fragViewDir = normalize(u_CameraPosition - inPosition);
}

#endif // VERTEX_SHADER

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragViewDir;

layout(location = 0) out vec4 outColor;

layout(std140, binding = 1) uniform RimConstants
{
    mat4 u_WorldViewProjection;
    vec3 u_CameraPosition;
    float u_RimPower;
    vec3 u_RimColor;
    float u_RimIntensity;
};

void main()
{
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(fragViewDir);

    float rim = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), u_RimPower);
    vec3 color = u_RimColor * rim * u_RimIntensity;

    outColor = vec4(color, 1.0);
}

#endif // FRAGMENT_SHADER
