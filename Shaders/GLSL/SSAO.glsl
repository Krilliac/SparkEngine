/**
 * SSAO.glsl - Screen Space Ambient Occlusion
 * Spark Engine - Cross-platform shader (GLSL equivalent of HLSL SSAO)
 *
 * GLSL 460 Core - 16-sample kernel SSAO pass.
 */
#version 460 core

// Fragment input (fullscreen quad)
layout(location = 0) in vec2 fragTexCoord;

// Fragment output
layout(location = 0) out vec4 outColor;

// Uniform buffer (matches HLSL register(b1))
layout(std140, binding = 1) uniform SSAOConstants
{
    mat4 u_InvProjection;
    vec2 u_ScreenSize;
    float u_Radius;
    float u_Bias;
};

// Depth texture
layout(binding = 0) uniform sampler2D u_DepthTexture;

// SSAO kernel samples
const vec2 samples[16] = vec2[16](
    vec2( 1.0,  0.0),   vec2(-1.0,  0.0),
    vec2( 0.0,  1.0),   vec2( 0.0, -1.0),
    vec2( 0.707,  0.707), vec2(-0.707,  0.707),
    vec2( 0.707, -0.707), vec2(-0.707, -0.707),
    vec2( 1.0,  1.0),   vec2(-1.0,  1.0),
    vec2( 1.0, -1.0),   vec2(-1.0, -1.0),
    vec2( 0.5,  0.0),   vec2(-0.5,  0.0),
    vec2( 0.0,  0.5),   vec2( 0.0, -0.5)
);

vec3 ReconstructViewPos(vec2 uv, float depth)
{
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 viewPos = u_InvProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

void main()
{
    float z = texture(u_DepthTexture, fragTexCoord).r;
    vec3 pos = ReconstructViewPos(fragTexCoord, z);

    float occlusion = 0.0;

    for (int i = 0; i < 16; i++)
    {
        vec2 offset = samples[i] / u_ScreenSize * u_Radius;
        float z2 = texture(u_DepthTexture, fragTexCoord + offset).r;
        vec3 pos2 = ReconstructViewPos(fragTexCoord + offset, z2);

        occlusion += (pos.z - pos2.z > u_Bias) ? 1.0 : 0.0;
    }

    occlusion /= 16.0;
    float ao = 1.0 - occlusion;

    outColor = vec4(ao, ao, ao, 1.0);
}
