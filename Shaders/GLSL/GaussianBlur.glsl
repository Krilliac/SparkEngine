/**
 * GaussianBlur.glsl - Separable Gaussian Blur
 * Spark Engine - Cross-platform shader (GLSL equivalent of HLSL GaussianBlur)
 *
 * GLSL 460 Core - Two-pass separable Gaussian blur (horizontal + vertical).
 * Use BLUR_HORIZONTAL define for horizontal pass, otherwise vertical.
 */
#version 460 core

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(std140, binding = 1) uniform BlurConstants
{
    vec2 u_TexelSize;
    vec2 _blurPad;
};

layout(binding = 0) uniform sampler2D u_InputTexture;

// 9-tap Gaussian weights (sigma ~1.8)
const float weights[5] = float[5](
    0.227027, 0.194595, 0.121622, 0.054054, 0.016216
);

void main()
{
    vec4 color = texture(u_InputTexture, fragTexCoord) * weights[0];

#ifdef BLUR_HORIZONTAL
    for (int i = 1; i < 5; i++)
    {
        vec2 offset = vec2(u_TexelSize.x * float(i), 0.0);
        color += texture(u_InputTexture, fragTexCoord + offset) * weights[i];
        color += texture(u_InputTexture, fragTexCoord - offset) * weights[i];
    }
#else
    for (int i = 1; i < 5; i++)
    {
        vec2 offset = vec2(0.0, u_TexelSize.y * float(i));
        color += texture(u_InputTexture, fragTexCoord + offset) * weights[i];
        color += texture(u_InputTexture, fragTexCoord - offset) * weights[i];
    }
#endif

    outColor = color;
}
