/**
 * @file MSDFTextRenderer.h
 * @brief Multi-channel Signed Distance Field text rendering system
 * @author Spark Engine Team
 * @date 2025
 *
 * Renders crisp text at any size from a single pre-generated MSDF atlas texture.
 * MSDF fonts use RGB channels to encode distance fields, producing sharp edges
 * at all resolutions with a simple median-of-three fragment shader operation.
 *
 * Supports: per-glyph positioning, kerning, colored text, outlines, shadows,
 * and instanced batch rendering for efficiency.
 *
 * Atlas generation is done offline by msdf-atlas-gen (not included in engine).
 * Runtime only needs the atlas texture + glyph metrics JSON.
 *
 * @see UISystem.h, SpriteBatch.h
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    // =========================================================================
    // Glyph & Font Data
    // =========================================================================

    /**
     * @brief Metrics for a single glyph in the MSDF atlas
     */
    struct MSDFGlyph
    {
        uint32_t codepoint = 0;

        // Atlas position (UV coordinates, normalized 0-1)
        float atlasLeft = 0.0f;
        float atlasTop = 0.0f;
        float atlasRight = 0.0f;
        float atlasBottom = 0.0f;

        // Glyph bounds relative to baseline (in em units, scaled by font size)
        float planeBoundsLeft = 0.0f;
        float planeBoundsTop = 0.0f;
        float planeBoundsRight = 0.0f;
        float planeBoundsBottom = 0.0f;

        float advance = 0.0f; ///< Horizontal advance after this glyph (in em)
    };

    /**
     * @brief Kerning pair adjustment
     */
    struct MSDFKerningPair
    {
        uint32_t first = 0;
        uint32_t second = 0;
        float advance = 0.0f;
    };

    /**
     * @brief MSDF font atlas data
     */
    struct MSDFFont
    {
        std::string name;
        float lineHeight = 1.0f; ///< Line height in em units
        float ascender = 0.8f;   ///< Distance from baseline to top
        float descender = -0.2f; ///< Distance from baseline to bottom
        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        float distanceRange = 4.0f; ///< SDF distance range in atlas pixels

        std::unordered_map<uint32_t, MSDFGlyph> glyphs;
        std::vector<MSDFKerningPair> kerning;

        /** @brief Get glyph for a codepoint, nullptr if not found */
        const MSDFGlyph* GetGlyph(uint32_t codepoint) const
        {
            auto it = glyphs.find(codepoint);
            return it != glyphs.end() ? &it->second : nullptr;
        }

        /** @brief Get kerning advance between two codepoints */
        float GetKerning(uint32_t first, uint32_t second) const
        {
            for (const auto& k : kerning)
            {
                if (k.first == first && k.second == second)
                    return k.advance;
            }
            return 0.0f;
        }
    };

    // =========================================================================
    // Text Render Request
    // =========================================================================

    /**
     * @brief A single text draw request
     */
    struct TextDrawRequest
    {
        std::string text;
        float x = 0.0f;
        float y = 0.0f;
        float fontSize = 24.0f;
        XMFLOAT4 color = {1.0f, 1.0f, 1.0f, 1.0f};

        // Optional effects
        bool hasOutline = false;
        XMFLOAT4 outlineColor = {0.0f, 0.0f, 0.0f, 1.0f};
        float outlineWidth = 0.1f; ///< Outline width in SDF units

        bool hasShadow = false;
        XMFLOAT4 shadowColor = {0.0f, 0.0f, 0.0f, 0.5f};
        float shadowOffsetX = 2.0f;
        float shadowOffsetY = 2.0f;
    };

    // =========================================================================
    // MSDF Text Renderer
    // =========================================================================

    /**
     * @brief GPU-accelerated text renderer using MSDF font atlases
     *
     * Renders text by generating instanced quads for each glyph, using a
     * fragment shader that computes the median of the RGB SDF channels.
     */
    class MSDFTextRenderer
    {
      public:
        static constexpr uint32_t kMaxGlyphsPerBatch = 4096;

        MSDFTextRenderer() = default;
        ~MSDFTextRenderer() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t screenWidth,
                        uint32_t screenHeight);
        void Shutdown();

        /**
         * @brief Load an MSDF font from atlas texture and metrics
         *
         * @param fontName    Unique font identifier
         * @param atlasData   Raw RGBA atlas pixel data
         * @param atlasWidth  Atlas width in pixels
         * @param atlasHeight Atlas height in pixels
         * @param font        Glyph metrics and kerning data
         * @return true on success
         */
        bool LoadFont(const std::string& fontName, const uint8_t* atlasData, uint32_t atlasWidth, uint32_t atlasHeight,
                      const MSDFFont& font);

        /** @brief Set the active font for rendering */
        bool SetActiveFont(const std::string& fontName);

        /** @brief Queue text for rendering (batched, rendered on Flush) */
        void DrawText(const TextDrawRequest& request);

        /** @brief Render all queued text */
        void Flush();

        /** @brief Handle screen resize */
        void OnResize(uint32_t width, uint32_t height);

        /** @brief Measure text dimensions without rendering */
        XMFLOAT2 MeasureText(const std::string& text, float fontSize) const;

      private:
        bool CompileShaders();
        bool CreateBuffers();
        void GenerateGlyphQuads(const TextDrawRequest& request);

        // Per-glyph vertex
        struct GlyphVertex
        {
            XMFLOAT2 position; // Screen-space position
            XMFLOAT2 texCoord; // Atlas UV
            XMFLOAT4 color;    // Text color
            XMFLOAT4 params;   // x=outlineWidth, y=hasShadow, z=distRange, w=fontSize
        };

        // Constant buffer
        struct TextCB
        {
            XMFLOAT4X4 projection; // Orthographic projection
            XMFLOAT4 screenSize;   // x=width, y=height, z=1/width, w=1/height
        };

        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        uint32_t m_screenWidth = 0;
        uint32_t m_screenHeight = 0;

        // Fonts
        struct LoadedFont
        {
            MSDFFont metrics;
            ComPtr<ID3D11Texture2D> atlasTexture;
            ComPtr<ID3D11ShaderResourceView> atlasSRV;
        };
        std::unordered_map<std::string, LoadedFont> m_fonts;
        const LoadedFont* m_activeFont = nullptr;

        // Glyph batch
        std::vector<GlyphVertex> m_glyphVertices;

        // GPU resources
        ComPtr<ID3D11VertexShader> m_vertexShader;
        ComPtr<ID3D11PixelShader> m_pixelShader;
        ComPtr<ID3D11InputLayout> m_inputLayout;
        ComPtr<ID3D11Buffer> m_vertexBuffer;
        ComPtr<ID3D11Buffer> m_constantBuffer;
        ComPtr<ID3D11SamplerState> m_linearSampler;
        ComPtr<ID3D11BlendState> m_alphaBlendState;
        ComPtr<ID3D11RasterizerState> m_rasterizerState;
        ComPtr<ID3D11DepthStencilState> m_depthStencilState;

        bool m_initialized = false;
    };

} // namespace Spark::Graphics
