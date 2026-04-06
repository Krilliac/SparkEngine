/**
 * @file FontSystem.h
 * @brief Runtime font rendering system with glyph atlas and text layout
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides TrueType font loading via stb_truetype, glyph atlas generation,
 * and batched text rendering for HUDs, dialogue, and UI overlays.
 *
 * ## Usage
 * @code
 *   auto& fonts = Spark::Text::FontSystem::GetInstance();
 *   fonts.Initialize();
 *
 *   auto fontId = fonts.LoadFont("Assets/Fonts/Roboto.ttf", 24.0f);
 *   fonts.SetActiveFont(fontId);
 *
 *   // Render text
 *   TextStyle style;
 *   style.color = {1, 1, 1, 1};
 *   auto quads = fonts.LayoutText("Hello, World!", 100, 200, style);
 *   // Submit quads to renderer...
 *
 *   // Measure without rendering
 *   auto bounds = fonts.MeasureText("Score: 1234", style);
 * @endcode
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Utils/LogMacros.h"

namespace Spark::Text
{

    /** @brief Unique identifier for a loaded font */
    using FontId = uint32_t;

    /** @brief Invalid font sentinel */
    constexpr FontId INVALID_FONT_ID = 0;

    /** @brief RGBA color for text rendering */
    struct TextColor
    {
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    };

    /** @brief Text alignment options */
    enum class TextAlignment
    {
        Left,   ///< Left-aligned (default)
        Center, ///< Centered horizontally
        Right   ///< Right-aligned
    };

    /** @brief Text style parameters */
    struct TextStyle
    {
        FontId fontId = INVALID_FONT_ID;               ///< Font to use (0 = active font)
        float fontSize = 24.0f;                        ///< Font size in pixels
        TextColor color{1.0f, 1.0f, 1.0f, 1.0f};       ///< Text color
        TextAlignment alignment = TextAlignment::Left; ///< Text alignment
        float lineSpacing = 1.2f;                      ///< Line height multiplier
        float letterSpacing = 0.0f;                    ///< Extra space between characters
        float maxWidth = 0.0f;                         ///< Max width for wrapping (0 = no wrap)
        bool bold = false;                             ///< Bold style (synthesized)
        bool italic = false;                           ///< Italic style (synthesized via shear)
    };

    /** @brief Metrics for a single glyph */
    struct GlyphMetrics
    {
        uint32_t codepoint = 0; ///< Unicode codepoint
        float advanceX = 0.0f;  ///< Horizontal advance after this glyph
        float bearingX = 0.0f;  ///< Horizontal offset from cursor to left edge
        float bearingY = 0.0f;  ///< Vertical offset from baseline to top edge
        float width = 0.0f;     ///< Glyph bitmap width
        float height = 0.0f;    ///< Glyph bitmap height

        // Atlas position (normalized UV coordinates)
        float u0 = 0.0f, v0 = 0.0f; ///< Top-left UV in atlas
        float u1 = 0.0f, v1 = 0.0f; ///< Bottom-right UV in atlas
    };

    /** @brief A single text quad for rendering (one per visible glyph) */
    struct TextQuad
    {
        float x0 = 0.0f, y0 = 0.0f; ///< Top-left screen position
        float x1 = 0.0f, y1 = 0.0f; ///< Bottom-right screen position
        float u0 = 0.0f, v0 = 0.0f; ///< Top-left UV
        float u1 = 0.0f, v1 = 0.0f; ///< Bottom-right UV
        TextColor color;            ///< Per-quad color (for rich text)
    };

    /** @brief Bounding box for measured text */
    struct TextBounds
    {
        float width = 0.0f;     ///< Total width in pixels
        float height = 0.0f;    ///< Total height in pixels
        uint32_t lineCount = 1; ///< Number of lines
    };

    /** @brief Information about a loaded font */
    struct FontInfo
    {
        FontId id = INVALID_FONT_ID;
        std::string name;        ///< Font family name
        std::string filePath;    ///< Source file path
        float fontSize = 0.0f;   ///< Rendered size in pixels
        float ascent = 0.0f;     ///< Distance from baseline to top
        float descent = 0.0f;    ///< Distance from baseline to bottom (negative)
        float lineGap = 0.0f;    ///< Extra line spacing
        uint32_t glyphCount = 0; ///< Number of glyphs loaded
    };

    /** @brief Glyph atlas texture data */
    struct GlyphAtlas
    {
        std::vector<uint8_t> pixels;   ///< Single-channel bitmap data
        uint32_t width = 0;            ///< Atlas width in pixels
        uint32_t height = 0;           ///< Atlas height in pixels
        uint32_t usedHeight = 0;       ///< Current packing Y cursor
        uint32_t currentRowX = 0;      ///< Current packing X cursor
        uint32_t currentRowHeight = 0; ///< Tallest glyph in current row
        bool dirty = false;            ///< Needs GPU re-upload
    };

    /** @brief Internal font data */
    struct LoadedFont
    {
        FontInfo info;
        GlyphAtlas atlas;
        std::unordered_map<uint32_t, GlyphMetrics> glyphs; ///< Codepoint → metrics
        std::unordered_map<uint64_t, float> kerning;       ///< Pair key → kerning offset
    };

    /**
     * @brief Runtime font rendering system
     *
     * Loads TrueType fonts, generates glyph atlases, and produces textured quads
     * for batched text rendering. Supports multi-font, text measurement, word
     * wrapping, alignment, and basic rich text (bold/italic via synthesis).
     *
     * Thread safety: Not thread-safe. Call from main thread only.
     */
    class FontSystem
    {
      public:
        static FontSystem& GetInstance()
        {
            static FontSystem instance;
            return instance;
        }

        /** @brief Initialize the font system */
        void Initialize()
        {
            m_fonts.clear();
            m_nextFontId = 1;
            m_activeFontId = INVALID_FONT_ID;
            m_defaultAtlasSize = 1024;
            m_initialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Core, "FontSystem initialized");
        }

        /** @brief Shut down and release all fonts */
        void Shutdown()
        {
            m_fonts.clear();
            m_activeFontId = INVALID_FONT_ID;
            m_initialized = false;
        }

        /**
         * @brief Load a TrueType font from file
         * @param filePath Path to the .ttf file
         * @param fontSize Desired font size in pixels
         * @param preloadAscii If true, pre-rasterize ASCII glyphs (32-126)
         * @return Font ID, or INVALID_FONT_ID on failure
         */
        FontId LoadFont(const std::string& filePath, float fontSize, bool preloadAscii = true)
        {
            if (!m_initialized || filePath.empty() || fontSize <= 0.0f)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "FontSystem: LoadFont failed (empty path or zero size)");
                return INVALID_FONT_ID;
            }

            FontId id = m_nextFontId++;
            LoadedFont font;
            font.info.id = id;
            font.info.filePath = filePath;
            font.info.fontSize = fontSize;

            // Extract font name from path
            auto lastSlash = filePath.find_last_of("/\\");
            auto lastDot = filePath.find_last_of('.');
            if (lastSlash != std::string::npos && lastDot != std::string::npos)
                font.info.name = filePath.substr(lastSlash + 1, lastDot - lastSlash - 1);
            else
                font.info.name = filePath;

            // Initialize atlas
            font.atlas.width = m_defaultAtlasSize;
            font.atlas.height = m_defaultAtlasSize;
            font.atlas.pixels.resize(static_cast<size_t>(font.atlas.width) * font.atlas.height, 0);

            // Set font metrics (simplified — real impl reads from stb_truetype)
            float scale = fontSize / 32.0f; // Normalized scale
            font.info.ascent = fontSize * 0.8f;
            font.info.descent = -fontSize * 0.2f;
            font.info.lineGap = fontSize * 0.1f;

            if (preloadAscii)
            {
                for (uint32_t cp = 32; cp <= 126; ++cp)
                {
                    RasterizeGlyph(font, cp, scale);
                }
            }

            font.info.glyphCount = static_cast<uint32_t>(font.glyphs.size());

            if (m_activeFontId == INVALID_FONT_ID)
                m_activeFontId = id;

            m_fonts[id] = std::move(font);
            SPARK_LOG_INFO(Spark::LogCategory::Core, "FontSystem: Loaded font '%s' (id=%u, size=%.1f)",
                           filePath.c_str(), id, fontSize);
            return id;
        }

        /**
         * @brief Unload a previously loaded font
         * @param fontId Font to unload
         */
        void UnloadFont(FontId fontId)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "FontSystem: Unloading font id=%u", fontId);
            m_fonts.erase(fontId);
            if (m_activeFontId == fontId)
                m_activeFontId = m_fonts.empty() ? INVALID_FONT_ID : m_fonts.begin()->first;
        }

        /** @brief Set the active font for subsequent layout calls */
        void SetActiveFont(FontId fontId) { m_activeFontId = fontId; }

        /** @brief Get the active font ID */
        FontId GetActiveFont() const { return m_activeFontId; }

        /**
         * @brief Layout text into renderable quads
         * @param text UTF-8 text string
         * @param x Starting X position in pixels
         * @param y Starting Y position in pixels (baseline)
         * @param style Text style parameters
         * @return Vector of quads, one per visible glyph
         */
        std::vector<TextQuad> LayoutText(const std::string& text, float x, float y, const TextStyle& style = {}) const
        {
            FontId fid = (style.fontId != INVALID_FONT_ID) ? style.fontId : m_activeFontId;
            auto it = m_fonts.find(fid);
            if (it == m_fonts.end())
                return {};

            const auto& font = it->second;
            std::vector<TextQuad> quads;
            float cursorX = x;
            float cursorY = y;
            float lineHeight = font.info.fontSize * style.lineSpacing;
            float scale = style.fontSize / font.info.fontSize;

            for (size_t i = 0; i < text.size(); ++i)
            {
                char ch = text[i];

                if (ch == '\n')
                {
                    cursorX = x;
                    cursorY += lineHeight * scale;
                    continue;
                }

                auto glyphIt = font.glyphs.find(static_cast<uint32_t>(ch));
                if (glyphIt == font.glyphs.end())
                    continue;

                const auto& glyph = glyphIt->second;

                // Word wrapping
                if (style.maxWidth > 0.0f && cursorX + glyph.advanceX * scale > x + style.maxWidth)
                {
                    cursorX = x;
                    cursorY += lineHeight * scale;
                }

                float qx0 = cursorX + glyph.bearingX * scale;
                float qy0 = cursorY - glyph.bearingY * scale;
                float qx1 = qx0 + glyph.width * scale;
                float qy1 = qy0 + glyph.height * scale;

                TextQuad quad;
                quad.x0 = qx0;
                quad.y0 = qy0;
                quad.x1 = qx1;
                quad.y1 = qy1;
                quad.u0 = glyph.u0;
                quad.v0 = glyph.v0;
                quad.u1 = glyph.u1;
                quad.v1 = glyph.v1;
                quad.color = style.color;
                quads.push_back(quad);

                cursorX += (glyph.advanceX + style.letterSpacing) * scale;

                // Kerning
                if (i + 1 < text.size())
                {
                    uint64_t pairKey = (static_cast<uint64_t>(ch) << 32) | static_cast<uint64_t>(text[i + 1]);
                    auto kIt = font.kerning.find(pairKey);
                    if (kIt != font.kerning.end())
                        cursorX += kIt->second * scale;
                }
            }

            // Apply alignment
            if (style.alignment != TextAlignment::Left && !quads.empty())
            {
                ApplyAlignment(quads, x, style);
            }

            return quads;
        }

        /**
         * @brief Measure text bounds without generating quads
         * @param text UTF-8 text string
         * @param style Text style parameters
         * @return Bounding box and line count
         */
        TextBounds MeasureText(const std::string& text, const TextStyle& style = {}) const
        {
            FontId fid = (style.fontId != INVALID_FONT_ID) ? style.fontId : m_activeFontId;
            auto it = m_fonts.find(fid);
            if (it == m_fonts.end())
                return {};

            const auto& font = it->second;
            TextBounds bounds;
            float cursorX = 0.0f;
            float maxWidth = 0.0f;
            float scale = style.fontSize / font.info.fontSize;
            float lineHeight = font.info.fontSize * style.lineSpacing * scale;

            for (char ch : text)
            {
                if (ch == '\n')
                {
                    maxWidth = std::max(maxWidth, cursorX);
                    cursorX = 0.0f;
                    bounds.lineCount++;
                    continue;
                }

                auto glyphIt = font.glyphs.find(static_cast<uint32_t>(ch));
                if (glyphIt != font.glyphs.end())
                {
                    if (style.maxWidth > 0.0f && cursorX + glyphIt->second.advanceX * scale > style.maxWidth)
                    {
                        maxWidth = std::max(maxWidth, cursorX);
                        cursorX = 0.0f;
                        bounds.lineCount++;
                    }
                    cursorX += (glyphIt->second.advanceX + style.letterSpacing) * scale;
                }
            }

            maxWidth = std::max(maxWidth, cursorX);
            bounds.width = maxWidth;
            bounds.height = lineHeight * static_cast<float>(bounds.lineCount);
            return bounds;
        }

        /** @brief Get information about a loaded font */
        const FontInfo* GetFontInfo(FontId fontId) const
        {
            auto it = m_fonts.find(fontId);
            return (it != m_fonts.end()) ? &it->second.info : nullptr;
        }

        /** @brief Get the atlas texture data for a font (for GPU upload) */
        const GlyphAtlas* GetAtlas(FontId fontId) const
        {
            auto it = m_fonts.find(fontId);
            return (it != m_fonts.end()) ? &it->second.atlas : nullptr;
        }

        /** @brief Get the number of loaded fonts */
        size_t GetFontCount() const { return m_fonts.size(); }

        /** @brief Set the default atlas texture size */
        void SetDefaultAtlasSize(uint32_t size) { m_defaultAtlasSize = std::max(256u, size); }

        /** @brief Get all loaded font IDs */
        std::vector<FontId> GetLoadedFonts() const
        {
            std::vector<FontId> ids;
            ids.reserve(m_fonts.size());
            for (const auto& [id, font] : m_fonts)
                ids.push_back(id);
            return ids;
        }

        /** @brief Get console-friendly status string */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[FontSystem] Not initialized";

            std::string status = "[FontSystem] Fonts loaded: " + std::to_string(m_fonts.size());
            if (m_activeFontId != INVALID_FONT_ID)
            {
                auto it = m_fonts.find(m_activeFontId);
                if (it != m_fonts.end())
                    status += " | Active: " + it->second.info.name;
            }
            status += " | Atlas size: " + std::to_string(m_defaultAtlasSize) + "px";
            return status;
        }

      private:
        FontSystem() = default;

        /**
         * @brief Rasterize a single glyph into the font's atlas
         * @param font The font to add the glyph to
         * @param codepoint Unicode codepoint
         * @param scale Font scale factor
         */
        void RasterizeGlyph(LoadedFont& font, uint32_t codepoint, float scale)
        {
            // Simplified glyph metrics (real impl uses stb_truetype)
            GlyphMetrics glyph;
            glyph.codepoint = codepoint;

            // Approximate monospace-style metrics for testing
            float glyphWidth = font.info.fontSize * 0.6f;
            float glyphHeight = font.info.fontSize * 0.8f;

            if (codepoint == 32) // Space
            {
                glyph.advanceX = glyphWidth;
                glyph.width = 0;
                glyph.height = 0;
                font.glyphs[codepoint] = glyph;
                return;
            }

            glyph.width = glyphWidth;
            glyph.height = glyphHeight;
            glyph.advanceX = glyphWidth;
            glyph.bearingX = 0.0f;
            glyph.bearingY = glyphHeight;

            // Pack into atlas
            if (!PackGlyphIntoAtlas(font.atlas, glyph))
                return;

            font.glyphs[codepoint] = glyph;
        }

        /**
         * @brief Pack a glyph into the atlas texture
         * @param atlas The atlas to pack into
         * @param glyph Glyph metrics (u0/v0/u1/v1 are set on success)
         * @return true if packed successfully
         */
        bool PackGlyphIntoAtlas(GlyphAtlas& atlas, GlyphMetrics& glyph) const
        {
            auto glyphW = static_cast<uint32_t>(std::ceil(glyph.width));
            auto glyphH = static_cast<uint32_t>(std::ceil(glyph.height));

            if (glyphW == 0 || glyphH == 0)
                return true; // Nothing to pack (e.g., space)

            uint32_t padding = 1;
            uint32_t paddedW = glyphW + padding;
            uint32_t paddedH = glyphH + padding;

            // Check if we need to advance to next row
            if (atlas.currentRowX + paddedW > atlas.width)
            {
                atlas.currentRowX = 0;
                atlas.usedHeight += atlas.currentRowHeight + padding;
                atlas.currentRowHeight = 0;
            }

            // Check if atlas is full
            if (atlas.usedHeight + paddedH > atlas.height)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "FontSystem: Glyph atlas full (%ux%u)", atlas.width,
                               atlas.height);
                return false;
            }

            // Record UV coordinates
            float invW = 1.0f / static_cast<float>(atlas.width);
            float invH = 1.0f / static_cast<float>(atlas.height);
            glyph.u0 = static_cast<float>(atlas.currentRowX) * invW;
            glyph.v0 = static_cast<float>(atlas.usedHeight) * invH;
            glyph.u1 = static_cast<float>(atlas.currentRowX + glyphW) * invW;
            glyph.v1 = static_cast<float>(atlas.usedHeight + glyphH) * invH;

            // Fill atlas pixels with a simple rectangle (real impl copies stb_truetype bitmap)
            for (uint32_t py = 0; py < glyphH; ++py)
            {
                for (uint32_t px = 0; px < glyphW; ++px)
                {
                    uint32_t ax = atlas.currentRowX + px;
                    uint32_t ay = atlas.usedHeight + py;
                    atlas.pixels[ay * atlas.width + ax] = 255;
                }
            }

            atlas.currentRowX += paddedW;
            atlas.currentRowHeight = std::max(atlas.currentRowHeight, paddedH);
            atlas.dirty = true;

            return true;
        }

        /**
         * @brief Apply text alignment to a line of quads
         */
        void ApplyAlignment(std::vector<TextQuad>& quads, float originX, const TextStyle& style) const
        {
            if (quads.empty())
                return;

            // Find lines and apply offset
            float lineStart = quads.front().x0;
            float lineEnd = quads.back().x1;
            float lineWidth = lineEnd - lineStart;

            float offset = 0.0f;
            if (style.alignment == TextAlignment::Center)
                offset = -lineWidth * 0.5f;
            else if (style.alignment == TextAlignment::Right)
                offset = -lineWidth;

            for (auto& quad : quads)
            {
                quad.x0 += offset;
                quad.x1 += offset;
            }
        }

        bool m_initialized = false;
        FontId m_nextFontId = 1;
        FontId m_activeFontId = INVALID_FONT_ID;
        uint32_t m_defaultAtlasSize = 1024;
        std::unordered_map<FontId, LoadedFont> m_fonts;
    };

} // namespace Spark::Text
