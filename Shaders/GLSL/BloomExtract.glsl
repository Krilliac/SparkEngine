/**
 * BloomExtract.glsl - Bloom Bright-Pass Extraction
 * Spark Engine - Cross-platform shader (GLSL equivalent of HLSL BloomExtract)
 *
 * GLSL 460 Core - Extracts bright pixels above a luminance threshold.
 */
#version 460 core

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(std140, binding = 1) uniform BloomConstants
{
    float u_Threshold;
    float u_SoftThreshold;
    float u_Intensity;
    float _bloomPad;
};

layout(binding = 0) uniform sampler2D u_InputTexture;

void main()
{
    vec3 color = texture(u_InputTexture, fragTexCoord).rgb;
    float luminance = dot(color, vec3(0.299, 0.587, 0.114));

    // Soft knee threshold for smoother bloom
    float knee = u_Threshold * u_SoftThreshold;
    float soft = luminance - u_Threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.00001);

    float contribution = max(soft, luminance - u_Threshold);
    contribution /= max(luminance, 0.00001);

    outColor = vec4(color * contribution * u_Intensity, 1.0);
}
