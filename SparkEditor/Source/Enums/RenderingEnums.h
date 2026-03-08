/**
 * @file RenderingEnums.h
 * @brief Enumerations for the rendering and graphics system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file contains all enumerations related to graphics rendering,
 * including pipeline types, texture formats, shader types, and rendering states.
 */

#pragma once

namespace SparkEditor
{

    /**
 * @brief Rendering pipeline types
 */
    enum class RenderingPipeline
    {
        FORWARD = 0,      ///< Forward rendering pipeline
        DEFERRED = 1,     ///< Deferred rendering pipeline
        FORWARD_PLUS = 2, ///< Forward+ rendering pipeline
        CLUSTERED = 3     ///< Clustered rendering pipeline
    };

    /**
 * @brief Shader types
 */
    enum class ShaderType
    {
        VERTEX = 0,   ///< Vertex shader
        PIXEL = 1,    ///< Pixel/Fragment shader
        GEOMETRY = 2, ///< Geometry shader
        HULL = 3,     ///< Hull/Tessellation control shader
        DOMAIN = 4,   ///< Domain/Tessellation evaluation shader
        COMPUTE = 5   ///< Compute shader
    };

    /**
 * @brief Texture formats
 */
    enum class TextureFormat
    {
        R8_UNORM = 0,           ///< 8-bit red channel
        R8G8_UNORM = 1,         ///< 8-bit red and green channels
        R8G8B8A8_UNORM = 2,     ///< 8-bit RGBA
        R16G16B16A16_FLOAT = 3, ///< 16-bit float RGBA
        R32G32B32A32_FLOAT = 4, ///< 32-bit float RGBA
        BC1_UNORM = 5,          ///< DXT1 compression
        BC3_UNORM = 6,          ///< DXT5 compression
        BC7_UNORM = 7,          ///< BC7 compression
        D24_UNORM_S8_UINT = 8,  ///< 24-bit depth, 8-bit stencil
        D32_FLOAT = 9           ///< 32-bit float depth
    };

    /**
 * @brief Texture types
 */
    enum class TextureType
    {
        TEXTURE_1D = 0,   ///< 1D texture
        TEXTURE_2D = 1,   ///< 2D texture
        TEXTURE_3D = 2,   ///< 3D texture
        TEXTURE_CUBE = 3, ///< Cube map texture
        TEXTURE_ARRAY = 4 ///< Texture array
    };

    /**
 * @brief Blend modes
 */
    enum class BlendMode
    {
        OPAQUE = 0,       ///< No blending (opaque)
        ALPHA_BLEND = 1,  ///< Alpha blending
        ADDITIVE = 2,     ///< Additive blending
        MULTIPLY = 3,     ///< Multiplicative blending
        PREMULTIPLIED = 4 ///< Premultiplied alpha
    };

    /**
 * @brief Cull modes
 */
    enum class CullMode
    {
        NONE = 0,  ///< No culling
        FRONT = 1, ///< Cull front faces
        BACK = 2   ///< Cull back faces
    };

    /**
 * @brief Fill modes
 */
    enum class FillMode
    {
        WIREFRAME = 0, ///< Wireframe rendering
        SOLID = 1      ///< Solid rendering
    };

    /**
 * @brief Filter modes
 */
    enum class FilterMode
    {
        POINT = 0,      ///< Point filtering
        LINEAR = 1,     ///< Linear filtering
        ANISOTROPIC = 2 ///< Anisotropic filtering
    };

    /**
 * @brief Address modes
 */
    enum class AddressMode
    {
        WRAP = 0,   ///< Wrap addressing
        CLAMP = 1,  ///< Clamp addressing
        MIRROR = 2, ///< Mirror addressing
        BORDER = 3  ///< Border addressing
    };

    /**
 * @brief Primitive topology types
 */
    enum class PrimitiveTopology
    {
        POINT_LIST = 0,    ///< Point list
        LINE_LIST = 1,     ///< Line list
        LINE_STRIP = 2,    ///< Line strip
        TRIANGLE_LIST = 3, ///< Triangle list
        TRIANGLE_STRIP = 4 ///< Triangle strip
    };

    /**
 * @brief Render target formats
 */
    enum class RenderTargetFormat
    {
        R8G8B8A8_UNORM = 0,     ///< 8-bit RGBA
        R16G16B16A16_FLOAT = 1, ///< 16-bit float RGBA (HDR)
        R32G32B32A32_FLOAT = 2, ///< 32-bit float RGBA (HDR)
        R11G11B10_FLOAT = 3,    ///< Packed 11:11:10 float RGB
        R10G10B10A2_UNORM = 4   ///< Packed 10:10:10:2 RGBA
    };

    /**
 * @brief Anti-aliasing types
 */
    enum class AntiAliasingType
    {
        NONE = 0,    ///< No anti-aliasing
        MSAA_2X = 1, ///< 2x MSAA
        MSAA_4X = 2, ///< 4x MSAA
        MSAA_8X = 3, ///< 8x MSAA
        FXAA = 4,    ///< FXAA
        TAA = 5      ///< Temporal anti-aliasing
    };

    /**
 * @brief Shadow map types
 */
    enum class ShadowMapType
    {
        STANDARD = 0,   ///< Standard shadow mapping
        CASCADED = 1,   ///< Cascaded shadow maps
        VARIANCE = 2,   ///< Variance shadow maps
        EXPONENTIAL = 3 ///< Exponential shadow maps
    };

    /**
 * @brief Post-processing effects
 */
    enum class PostProcessEffect
    {
        TONE_MAPPING = 0,             ///< Tone mapping
        BLOOM = 1,                    ///< Bloom effect
        DEPTH_OF_FIELD = 2,           ///< Depth of field
        MOTION_BLUR = 3,              ///< Motion blur
        SCREEN_SPACE_AO = 4,          ///< Screen space ambient occlusion
        SCREEN_SPACE_REFLECTIONS = 5, ///< Screen space reflections
        COLOR_GRADING = 6,            ///< Color grading
        CHROMATIC_ABERRATION = 7,     ///< Chromatic aberration
        VIGNETTE = 8,                 ///< Vignette effect
        FILM_GRAIN = 9                ///< Film grain effect
    };

} // namespace SparkEditor