/**
 * EnvReflection.glsl - Environment Cubemap Reflection Shader
 * Spark Engine - Cross-platform shader (GLSL equivalent of HLSL EnvReflection)
 *
 * GLSL 460 Core - Cubemap-based environment reflections with roughness LOD.
 * Contains both vertex and fragment stages separated by preprocessor.
 */
#version 460 core

#ifdef VERTEX_SHADER

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;

layout(std140, binding = 1) uniform EnvReflectionConstants
{
    mat4 u_WorldViewProjection;
    vec3 u_CameraPosition;
    float u_Roughness;
    float u_ReflectionIntensity;
    float u_FresnelBias;
    float u_FresnelScale;
    float u_FresnelPower;
};

void main()
{
    gl_Position = u_WorldViewProjection * vec4(inPosition, 1.0);
    fragWorldPos = inPosition;
    fragNormal = normalize(inNormal);
}

#endif // VERTEX_SHADER

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

layout(std140, binding = 1) uniform EnvReflectionConstants
{
    mat4 u_WorldViewProjection;
    vec3 u_CameraPosition;
    float u_Roughness;
    float u_ReflectionIntensity;
    float u_FresnelBias;
    float u_FresnelScale;
    float u_FresnelPower;
};

layout(binding = 0) uniform samplerCube u_EnvMap;

void main()
{
    vec3 V = normalize(fragWorldPos - u_CameraPosition);
    vec3 N = normalize(fragNormal);
    vec3 R = reflect(V, N);

    // Sample cubemap with roughness-based LOD
    float mipLevel = u_Roughness * 6.0; // Assuming 7 mip levels
    vec3 envColor = textureLod(u_EnvMap, R, mipLevel).rgb;

    // Fresnel
    float fresnel = u_FresnelBias + u_FresnelScale *
                    pow(1.0 - clamp(dot(-V, N), 0.0, 1.0), u_FresnelPower);

    outColor = vec4(envColor * u_ReflectionIntensity * fresnel, 1.0);
}

#endif // FRAGMENT_SHADER
