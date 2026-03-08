/**
 * Water.glsl - Water Surface Shader
 * Spark Engine - Cross-platform shader (GLSL equivalent of HLSL Water)
 *
 * GLSL 460 Core - Animated water with Fresnel reflections and refraction.
 * Contains both vertex and fragment stages separated by preprocessor.
 */
#version 460 core

#ifdef VERTEX_SHADER

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 fragUV_Refraction;
layout(location = 1) out vec2 fragUV_Reflection;
layout(location = 2) out vec3 fragViewDir;
layout(location = 3) out vec2 fragBaseUV;

layout(std140, binding = 1) uniform WaterConstants
{
    mat4 u_WorldViewProjection;
    vec3 u_CameraPosition;
    float u_Time;
    vec2 u_WaveSpeed;
    float u_DistortionStrength;
    float u_FresnelPower;
    vec3 u_WaterColor;
    float u_WaterAlpha;
};

layout(binding = 2) uniform sampler2D u_NormalMap;

void main()
{
    vec4 worldPos = vec4(inPosition, 1.0);
    gl_Position = u_WorldViewProjection * worldPos;

    // Scrolling UV for normal map distortion
    vec2 scrollUV = inTexCoord + vec2(u_Time * u_WaveSpeed.x, u_Time * u_WaveSpeed.y);
    vec2 distortion = textureLod(u_NormalMap, scrollUV, 0.0).rg * 2.0 - 1.0;

    fragUV_Refraction = inTexCoord + distortion * u_DistortionStrength;
    fragUV_Reflection = inTexCoord - distortion * u_DistortionStrength;
    fragViewDir = normalize(u_CameraPosition - inPosition);
    fragBaseUV = inTexCoord;
}

#endif // VERTEX_SHADER

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec2 fragUV_Refraction;
layout(location = 1) in vec2 fragUV_Reflection;
layout(location = 2) in vec3 fragViewDir;
layout(location = 3) in vec2 fragBaseUV;

layout(location = 0) out vec4 outColor;

layout(std140, binding = 1) uniform WaterConstants
{
    mat4 u_WorldViewProjection;
    vec3 u_CameraPosition;
    float u_Time;
    vec2 u_WaveSpeed;
    float u_DistortionStrength;
    float u_FresnelPower;
    vec3 u_WaterColor;
    float u_WaterAlpha;
};

layout(binding = 0) uniform sampler2D u_RefractionMap;
layout(binding = 1) uniform sampler2D u_ReflectionMap;

void main()
{
    // Fresnel effect
    float fresnel = pow(1.0 - clamp(dot(fragViewDir, vec3(0.0, 1.0, 0.0)), 0.0, 1.0),
                        u_FresnelPower);

    // Sample refraction and reflection
    vec4 refraction = texture(u_RefractionMap, fragUV_Refraction);
    vec4 reflection = texture(u_ReflectionMap, fragUV_Reflection);

    // Blend based on Fresnel
    vec4 color = mix(refraction, reflection, fresnel);

    // Tint with water color
    color.rgb = mix(color.rgb, u_WaterColor, u_WaterAlpha);

    outColor = vec4(color.rgb, 1.0);
}

#endif // FRAGMENT_SHADER
