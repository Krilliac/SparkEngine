/**
 * FullscreenQuad.glsl - Fullscreen Triangle/Quad Vertex Shader
 * Spark Engine - Cross-platform shader
 *
 * GLSL 460 Core - Procedurally generates a fullscreen triangle
 * without requiring vertex buffer input. Used by all post-processing passes.
 */
#version 460 core

layout(location = 0) out vec2 fragTexCoord;

void main()
{
    // Generate fullscreen triangle from vertex ID (no vertex buffer needed)
    // Vertices: (-1,-1), (3,-1), (-1,3) - oversized triangle covers the screen
    fragTexCoord = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(fragTexCoord * 2.0 - 1.0, 0.0, 1.0);
    // Flip Y for OpenGL convention (bottom-left origin)
    fragTexCoord.y = 1.0 - fragTexCoord.y;
}
