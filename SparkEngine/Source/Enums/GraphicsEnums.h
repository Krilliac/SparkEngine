/**
 * @file GraphicsEnums.h
 * @brief Core graphics system enumerations for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file contains fundamental graphics enumerations used throughout
 * the Spark Engine graphics system.
 */

#pragma once

namespace SparkEngine {

/**
 * @brief Graphics API types
 */
enum class GraphicsAPI {
    DIRECTX_11 = 0,         ///< DirectX 11
    DIRECTX_12 = 1,         ///< DirectX 12
    OPENGL = 2,             ///< OpenGL
    VULKAN = 3,             ///< Vulkan
    METAL = 4               ///< Metal (macOS/iOS)
};

/**
 * @brief Buffer usage types
 */
enum class BufferUsage {
    STATIC = 0,             ///< Static buffer (rarely updated)
    DYNAMIC = 1,            ///< Dynamic buffer (frequently updated)
    STREAMING = 2           ///< Streaming buffer (updated every frame)
};

/**
 * @brief Buffer types
 */
enum class BufferType {
    VERTEX = 0,             ///< Vertex buffer
    INDEX = 1,              ///< Index buffer
    CONSTANT = 2,           ///< Constant buffer
    STRUCTURED = 3,         ///< Structured buffer
    TEXTURE = 4             ///< Texture buffer
};

/**
 * @brief Mesh topology types
 */
enum class MeshTopology {
    TRIANGLES = 0,          ///< Triangle list
    TRIANGLE_STRIP = 1,     ///< Triangle strip
    LINES = 2,              ///< Line list
    LINE_STRIP = 3,         ///< Line strip
    POINTS = 4              ///< Point list
};

/**
 * @brief Resource states
 */
enum class ResourceState {
    UNINITIALIZED = 0,      ///< Resource not initialized
    LOADING = 1,            ///< Resource is loading
    READY = 2,              ///< Resource is ready for use
    ERROR = 3,              ///< Error loading resource
    DISPOSED = 4            ///< Resource has been disposed
};

/**
 * @brief Viewport clear flags
 */
enum class ClearFlags {
    NONE = 0,               ///< Clear nothing
    COLOR = 1,              ///< Clear color buffer
    DEPTH = 2,              ///< Clear depth buffer
    STENCIL = 4,            ///< Clear stencil buffer
    ALL = 7                 ///< Clear all buffers
};

/**
 * @brief Comparison functions
 */
enum class ComparisonFunction {
    NEVER = 0,              ///< Never pass
    LESS = 1,               ///< Pass if less
    EQUAL = 2,              ///< Pass if equal
    LESS_EQUAL = 3,         ///< Pass if less or equal
    GREATER = 4,            ///< Pass if greater
    NOT_EQUAL = 5,          ///< Pass if not equal
    GREATER_EQUAL = 6,      ///< Pass if greater or equal
    ALWAYS = 7              ///< Always pass
};

/**
 * @brief Stencil operations
 */
enum class StencilOperation {
    KEEP = 0,               ///< Keep existing value
    ZERO = 1,               ///< Set to zero
    REPLACE = 2,            ///< Replace with reference value
    INCREMENT = 3,          ///< Increment and clamp
    DECREMENT = 4,          ///< Decrement and clamp
    INVERT = 5,             ///< Bitwise invert
    INCREMENT_WRAP = 6,     ///< Increment and wrap
    DECREMENT_WRAP = 7      ///< Decrement and wrap
};

/**
 * @brief Render queue types
 */
enum class RenderQueue {
    BACKGROUND = 1000,      ///< Background objects (skybox, etc.)
    OPAQUE = 2000,          ///< Opaque objects
    ALPHA_TEST = 2450,      ///< Alpha-tested objects
    TRANSPARENT = 3000,     ///< Transparent objects
    OVERLAY = 4000          ///< Overlay objects (UI, etc.)
};

} // namespace SparkEngine
