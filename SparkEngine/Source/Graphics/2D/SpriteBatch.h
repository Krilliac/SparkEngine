/**
 * @file SpriteBatch.h
 * @brief Batched 2D sprite renderer with automatic atlas sorting and draw-call merging
 * @author Spark Engine Team
 * @date 2026
 *
 * SpriteBatch collects sprite draw requests, sorts them by texture and layer,
 * and flushes them in minimal draw calls. Supports:
 * - Per-sprite color tinting, UV sub-rects, rotation, scale, and flip
 * - Configurable blend modes (alpha, additive, multiply, premultiplied-alpha)
 * - Layer-based sorting for correct 2D depth ordering
 * - Nine-slice sprite rendering
 * - Line and rectangle primitive drawing for debug overlays
 */

#pragma once

#include "../../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace Spark::Graphics2D
{

    // =========================================================================
    // Blend modes
    // =========================================================================

    enum class BlendMode
    {
        Alpha,             ///< Standard alpha blending (src.a, 1-src.a)
        Additive,          ///< Additive blending (src.a, 1)
        Multiply,          ///< Multiply blending (dst, src)
        PremultipliedAlpha ///< Pre-multiplied alpha (1, 1-src.a)
    };

    // =========================================================================
    // Sprite vertex
    // =========================================================================

    struct SpriteVertex
    {
        DirectX::XMFLOAT3 position; ///< XY position + Z for depth sorting
        DirectX::XMFLOAT2 texCoord; ///< UV coordinates
        DirectX::XMFLOAT4 color;    ///< RGBA tint color
    };

    // =========================================================================
    // Draw command (internal)
    // =========================================================================

    struct SpriteDrawCommand
    {
        std::string texturePath;
        int sortKey = 0; ///< Combined from sortingLayer + orderInLayer
        DirectX::XMFLOAT3 position{0, 0, 0};
        DirectX::XMFLOAT2 size{1, 1};
        DirectX::XMFLOAT2 origin{0.5f, 0.5f};
        float rotation = 0.0f;                    ///< Rotation in radians
        DirectX::XMFLOAT4 sourceRect{0, 0, 1, 1}; ///< UV rect
        DirectX::XMFLOAT4 color{1, 1, 1, 1};
        bool flipX = false;
        bool flipY = false;
        BlendMode blendMode = BlendMode::Alpha;
    };

    // =========================================================================
    // SpriteBatch
    // =========================================================================

    class SpriteBatch
    {
      public:
        SpriteBatch() = default;

        /// Begin a new batch frame. Call before submitting sprites.
        void Begin(BlendMode defaultBlend = BlendMode::Alpha)
        {
            m_commands.clear();
            m_defaultBlend = defaultBlend;
            m_isBatching = true;
        }

        /// Submit a sprite for rendering
        void Draw(const std::string& texturePath, const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT2& size,
                  const DirectX::XMFLOAT4& sourceRect = {0, 0, 1, 1}, const DirectX::XMFLOAT4& color = {1, 1, 1, 1},
                  float rotation = 0.0f, const DirectX::XMFLOAT2& origin = {0.5f, 0.5f}, bool flipX = false,
                  bool flipY = false, int sortKey = 0, BlendMode blend = BlendMode::Alpha)
        {
            SpriteDrawCommand cmd;
            cmd.texturePath = texturePath;
            cmd.position = position;
            cmd.size = size;
            cmd.sourceRect = sourceRect;
            cmd.color = color;
            cmd.rotation = rotation;
            cmd.origin = origin;
            cmd.flipX = flipX;
            cmd.flipY = flipY;
            cmd.sortKey = sortKey;
            cmd.blendMode = blend;
            m_commands.push_back(std::move(cmd));
        }

        /// Draw a nine-slice sprite
        void DrawNineSlice(const std::string& texturePath, const DirectX::XMFLOAT3& position,
                           const DirectX::XMFLOAT2& size, float borderLeft, float borderTop, float borderRight,
                           float borderBottom, const DirectX::XMFLOAT4& color = {1, 1, 1, 1}, int sortKey = 0,
                           bool fillCenter = true)
        {
            // Generate 9 quads for the 9-slice grid
            // Border sizes normalized to texture coordinates
            float bl = borderLeft / size.x;
            float bt = borderTop / size.y;
            float br = borderRight / size.x;
            float bb = borderBottom / size.y;

            // Clamp borders
            bl = (std::min)(bl, 0.49f);
            bt = (std::min)(bt, 0.49f);
            br = (std::min)(br, 0.49f);
            bb = (std::min)(bb, 0.49f);

            float centerW = size.x - borderLeft - borderRight;
            float centerH = size.y - borderTop - borderBottom;

            // Corners
            auto addSlice = [&](float x, float y, float w, float h, float u0, float v0, float u1, float v1)
            {
                Draw(texturePath, {position.x + x, position.y + y, position.z}, {w, h}, {u0, v0, u1, v1}, color, 0.0f,
                     {0.0f, 0.0f}, false, false, sortKey);
            };

            // Top-left
            addSlice(0, 0, borderLeft, borderTop, 0, 0, bl, bt);
            // Top-center
            addSlice(borderLeft, 0, centerW, borderTop, bl, 0, 1.0f - br, bt);
            // Top-right
            addSlice(borderLeft + centerW, 0, borderRight, borderTop, 1.0f - br, 0, 1, bt);

            // Middle-left
            addSlice(0, borderTop, borderLeft, centerH, 0, bt, bl, 1.0f - bb);
            // Center
            if (fillCenter)
                addSlice(borderLeft, borderTop, centerW, centerH, bl, bt, 1.0f - br, 1.0f - bb);
            // Middle-right
            addSlice(borderLeft + centerW, borderTop, borderRight, centerH, 1.0f - br, bt, 1, 1.0f - bb);

            // Bottom-left
            addSlice(0, borderTop + centerH, borderLeft, borderBottom, 0, 1.0f - bb, bl, 1);
            // Bottom-center
            addSlice(borderLeft, borderTop + centerH, centerW, borderBottom, bl, 1.0f - bb, 1.0f - br, 1);
            // Bottom-right
            addSlice(borderLeft + centerW, borderTop + centerH, borderRight, borderBottom, 1.0f - br, 1.0f - bb, 1, 1);
        }

        /// Draw a debug line (rendered as a thin quad)
        void DrawLine(const DirectX::XMFLOAT2& start, const DirectX::XMFLOAT2& end,
                      const DirectX::XMFLOAT4& color = {1, 1, 1, 1}, float thickness = 0.02f, int sortKey = 9999)
        {
            float dx = end.x - start.x;
            float dy = end.y - start.y;
            float length = std::sqrt(dx * dx + dy * dy);
            float angle = std::atan2(dy, dx);
            float cx = (start.x + end.x) * 0.5f;
            float cy = (start.y + end.y) * 0.5f;

            Draw("__white__", {cx, cy, 0.0f}, {length, thickness}, {0, 0, 1, 1}, color, angle, {0.5f, 0.5f}, false,
                 false, sortKey);
        }

        /// Draw a debug rectangle outline
        void DrawRect(const DirectX::XMFLOAT2& pos, const DirectX::XMFLOAT2& size,
                      const DirectX::XMFLOAT4& color = {0, 1, 0, 1}, float thickness = 0.02f, int sortKey = 9999)
        {
            float x = pos.x;
            float y = pos.y;
            float w = size.x;
            float h = size.y;
            DrawLine({x, y}, {x + w, y}, color, thickness, sortKey);
            DrawLine({x + w, y}, {x + w, y + h}, color, thickness, sortKey);
            DrawLine({x + w, y + h}, {x, y + h}, color, thickness, sortKey);
            DrawLine({x, y + h}, {x, y}, color, thickness, sortKey);
        }

        /// Draw a filled rectangle
        void DrawFilledRect(const DirectX::XMFLOAT2& pos, const DirectX::XMFLOAT2& size,
                            const DirectX::XMFLOAT4& color = {1, 1, 1, 0.5f}, int sortKey = 0)
        {
            Draw("__white__", {pos.x + size.x * 0.5f, pos.y + size.y * 0.5f, 0.0f}, size, {0, 0, 1, 1}, color, 0.0f,
                 {0.5f, 0.5f}, false, false, sortKey);
        }

        /// End the batch and generate sorted vertex data
        void End()
        {
            m_isBatching = false;
            SortCommands();
            GenerateVertices();
        }

        /// Get the generated vertex data for rendering
        const std::vector<SpriteVertex>& GetVertices() const { return m_vertices; }

        /// Get the number of draw calls needed (one per texture change)
        size_t GetDrawCallCount() const { return m_drawCalls.size(); }

        struct DrawCall
        {
            std::string texturePath;
            uint32_t startVertex = 0;
            uint32_t vertexCount = 0;
            BlendMode blendMode = BlendMode::Alpha;
        };

        /// Get the draw call list for the GPU renderer
        const std::vector<DrawCall>& GetDrawCalls() const { return m_drawCalls; }

        /// Get total sprite count
        size_t GetSpriteCount() const { return m_commands.size(); }

        /// Check if currently batching
        bool IsBatching() const { return m_isBatching; }

      private:
        void SortCommands()
        {
            std::stable_sort(m_commands.begin(), m_commands.end(),
                             [](const SpriteDrawCommand& a, const SpriteDrawCommand& b)
                             {
                                 if (a.sortKey != b.sortKey)
                                     return a.sortKey < b.sortKey;
                                 return a.texturePath < b.texturePath;
                             });
        }

        void GenerateVertices()
        {
            m_vertices.clear();
            m_drawCalls.clear();

            if (m_commands.empty())
                return;

            m_vertices.reserve(m_commands.size() * 6); // 2 triangles per sprite

            std::string currentTexture;
            BlendMode currentBlend = BlendMode::Alpha;
            uint32_t batchStart = 0;

            for (const auto& cmd : m_commands)
            {
                if (cmd.texturePath != currentTexture || cmd.blendMode != currentBlend)
                {
                    // Flush previous batch
                    if (!currentTexture.empty() && m_vertices.size() > batchStart)
                    {
                        DrawCall dc;
                        dc.texturePath = currentTexture;
                        dc.startVertex = batchStart;
                        dc.vertexCount = static_cast<uint32_t>(m_vertices.size()) - batchStart;
                        dc.blendMode = currentBlend;
                        m_drawCalls.push_back(dc);
                    }
                    currentTexture = cmd.texturePath;
                    currentBlend = cmd.blendMode;
                    batchStart = static_cast<uint32_t>(m_vertices.size());
                }

                GenerateSpriteQuad(cmd);
            }

            // Flush last batch
            if (!currentTexture.empty() && m_vertices.size() > batchStart)
            {
                DrawCall dc;
                dc.texturePath = currentTexture;
                dc.startVertex = batchStart;
                dc.vertexCount = static_cast<uint32_t>(m_vertices.size()) - batchStart;
                dc.blendMode = currentBlend;
                m_drawCalls.push_back(dc);
            }
        }

        void GenerateSpriteQuad(const SpriteDrawCommand& cmd)
        {
            float halfW = cmd.size.x * 0.5f;
            float halfH = cmd.size.y * 0.5f;

            // Offset by origin
            float ox = (cmd.origin.x - 0.5f) * cmd.size.x;
            float oy = (cmd.origin.y - 0.5f) * cmd.size.y;

            // Corner positions before rotation (centered at origin)
            DirectX::XMFLOAT2 corners[4] = {
                {-halfW - ox, -halfH - oy}, // BL
                {halfW - ox, -halfH - oy},  // BR
                {halfW - ox, halfH - oy},   // TR
                {-halfW - ox, halfH - oy}   // TL
            };

            // Apply rotation
            float cosR = std::cos(cmd.rotation);
            float sinR = std::sin(cmd.rotation);
            for (auto& c : corners)
            {
                float rx = c.x * cosR - c.y * sinR;
                float ry = c.x * sinR + c.y * cosR;
                c.x = rx + cmd.position.x;
                c.y = ry + cmd.position.y;
            }

            // UV coordinates with flip support
            float u0 = cmd.sourceRect.x;
            float v0 = cmd.sourceRect.y;
            float u1 = cmd.sourceRect.z;
            float v1 = cmd.sourceRect.w;

            if (cmd.flipX)
                std::swap(u0, u1);
            if (cmd.flipY)
                std::swap(v0, v1);

            float z = cmd.position.z;

            // Two triangles (BL, BR, TR) and (BL, TR, TL)
            m_vertices.push_back({{corners[0].x, corners[0].y, z}, {u0, v1}, cmd.color});
            m_vertices.push_back({{corners[1].x, corners[1].y, z}, {u1, v1}, cmd.color});
            m_vertices.push_back({{corners[2].x, corners[2].y, z}, {u1, v0}, cmd.color});

            m_vertices.push_back({{corners[0].x, corners[0].y, z}, {u0, v1}, cmd.color});
            m_vertices.push_back({{corners[2].x, corners[2].y, z}, {u1, v0}, cmd.color});
            m_vertices.push_back({{corners[3].x, corners[3].y, z}, {u0, v0}, cmd.color});
        }

        std::vector<SpriteDrawCommand> m_commands;
        std::vector<SpriteVertex> m_vertices;
        std::vector<DrawCall> m_drawCalls;
        BlendMode m_defaultBlend = BlendMode::Alpha;
        bool m_isBatching = false;
    };

} // namespace Spark::Graphics2D
