/**
 * DebugVisualize.glsl - Debug Visualization Shader
 * Spark Engine - Cross-platform shader (GLSL equivalent of HLSL DebugVisualize)
 *
 * GLSL 460 Core - Visualize various G-buffer channels, depth, normals,
 * wireframe, and other debug outputs.
 *
 * Select mode via MODE define:
 *   0 = Depth, 1 = Normals, 2 = UV, 3 = Wireframe, 4 = Overdraw
 */
#version 460 core

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(std140, binding = 1) uniform DebugConstants
{
    int u_DebugMode;       // 0=depth, 1=normals, 2=uv, 3=wireframe, 4=overdraw
    float u_NearPlane;
    float u_FarPlane;
    float u_DepthScale;
};

layout(binding = 0) uniform sampler2D u_DepthTexture;
layout(binding = 1) uniform sampler2D u_NormalTexture;
layout(binding = 2) uniform sampler2D u_AlbedoTexture;

// Linearize depth from non-linear depth buffer
float LinearizeDepth(float depth)
{
    float z = depth * 2.0 - 1.0; // Back to NDC
    return (2.0 * u_NearPlane * u_FarPlane) /
           (u_FarPlane + u_NearPlane - z * (u_FarPlane - u_NearPlane));
}

// Heat map color ramp
vec3 HeatMap(float value)
{
    value = clamp(value, 0.0, 1.0);
    vec3 color;
    if (value < 0.25)
        color = mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), value * 4.0);
    else if (value < 0.5)
        color = mix(vec3(0.0, 1.0, 1.0), vec3(0.0, 1.0, 0.0), (value - 0.25) * 4.0);
    else if (value < 0.75)
        color = mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), (value - 0.5) * 4.0);
    else
        color = mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), (value - 0.75) * 4.0);
    return color;
}

void main()
{
    // Depth visualization
    if (u_DebugMode == 0)
    {
        float depth = texture(u_DepthTexture, fragTexCoord).r;
        float linearDepth = LinearizeDepth(depth) / u_FarPlane;
        linearDepth = pow(linearDepth, u_DepthScale);
        outColor = vec4(vec3(linearDepth), 1.0);
    }
    // Normal visualization
    else if (u_DebugMode == 1)
    {
        vec3 normal = texture(u_NormalTexture, fragTexCoord).rgb;
        outColor = vec4(normal * 0.5 + 0.5, 1.0);
    }
    // UV visualization
    else if (u_DebugMode == 2)
    {
        outColor = vec4(fragTexCoord, 0.0, 1.0);
    }
    // Wireframe (checkerboard approximation)
    else if (u_DebugMode == 3)
    {
        vec2 grid = fract(fragTexCoord * 50.0);
        float line = step(0.98, grid.x) + step(0.98, grid.y);
        vec3 albedo = texture(u_AlbedoTexture, fragTexCoord).rgb;
        outColor = vec4(mix(albedo * 0.3, vec3(0.0, 1.0, 0.0), min(line, 1.0)), 1.0);
    }
    // Overdraw heat map
    else if (u_DebugMode == 4)
    {
        float overdraw = texture(u_AlbedoTexture, fragTexCoord).a;
        outColor = vec4(HeatMap(overdraw), 1.0);
    }
    else
    {
        outColor = texture(u_AlbedoTexture, fragTexCoord);
    }
}
