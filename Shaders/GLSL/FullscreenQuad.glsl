/**
 * FullscreenQuad.glsl - Fullscreen Triangle/Quad Vertex Shader
 * Spark Engine - Cross-platform shader
 *
 * GLSL 460 Core - Procedurally generates a fullscreen triangle
 * without requiring vertex buffer input. Used by all post-processing passes.
 */
#version 460 core

layout(location = 0) out vec2 fragTexCoord;

// glslangValidator -V (the Vulkan SPIR-V build) predefines VULKAN; the GL driver does not.
#ifdef VULKAN
#define SPARK_VERTEX_ID gl_VertexIndex
#else
#define SPARK_VERTEX_ID gl_VertexID
#endif

void main()
{
    // Generate fullscreen triangle from vertex ID (no vertex buffer needed)
    // Vertices: (-1,-1), (3,-1), (-1,3) - oversized triangle covers the screen
    fragTexCoord = vec2((SPARK_VERTEX_ID << 1) & 2, SPARK_VERTEX_ID & 2);
    gl_Position = vec4(fragTexCoord * 2.0 - 1.0, 0.0, 1.0);
#ifndef VULKAN
    // Flip Y for OpenGL convention (bottom-left origin). Vulkan clip-space Y
    // already points down, so v = 0 lands on the top row without a flip.
    fragTexCoord.y = 1.0 - fragTexCoord.y;
#endif
}
