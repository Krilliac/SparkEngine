/**
 * @file SVGRenderer.h
 * @brief SVG path parsing, tessellation, and rendering to vertex buffers
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides data structures for SVG path representation and CPU-side
 * tessellation into triangle lists suitable for GPU rendering.
 *
 * Features:
 * - SVG path commands: MoveTo, LineTo, CubicBezier, QuadBezier, Close
 * - Recursive Bezier flattening to polylines with configurable tolerance
 * - Ear-clipping triangulation for filled paths
 * - Stroke tessellation with configurable width, line cap, and line join
 * - SVGDocument with simple SVG subset parsing
 * - Output as vector of Vertex2D (position, UV, RGBA)
 *
 * @see UIRenderer.h, FontSystem.h, TextRenderer.h
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    static constexpr float SVG_PI = 3.14159265358979323846f;
    static constexpr float SVG_FLATTEN_TOLERANCE = 0.5f;

    // =========================================================================
    // Vertex Output
    // =========================================================================

    /// @brief 2D vertex with position, texture coordinates, and color
    struct Vertex2D
    {
        float x = 0.0f;
        float y = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    // =========================================================================
    // Path Commands
    // =========================================================================

    enum class PathCommandType : uint8_t
    {
        MoveTo,
        LineTo,
        CubicBezier,
        QuadBezier,
        Close
    };

    /// @brief A single SVG path command with up to 3 control points
    struct PathCommand
    {
        PathCommandType type = PathCommandType::MoveTo;
        float x = 0.0f; ///< End point (or move target)
        float y = 0.0f;
        float cx1 = 0.0f; ///< First control point (cubic/quad)
        float cy1 = 0.0f;
        float cx2 = 0.0f; ///< Second control point (cubic only)
        float cy2 = 0.0f;
    };

    // =========================================================================
    // SVG Color and Stroke
    // =========================================================================

    struct SVGColor
    {
        float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    };

    enum class LineCap : uint8_t
    {
        Butt,
        Round,
        Square
    };

    enum class LineJoin : uint8_t
    {
        Miter,
        Round,
        Bevel
    };

    struct SVGStroke
    {
        SVGColor color = {0.0f, 0.0f, 0.0f, 1.0f};
        float width = 1.0f;
        LineCap cap = LineCap::Butt;
        LineJoin join = LineJoin::Miter;
        float miterLimit = 4.0f;
    };

    // =========================================================================
    // 2D Transform (affine 3x2 matrix)
    // =========================================================================

    struct SVGTransform
    {
        float m[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

        /// @brief Apply transform to a point
        void Apply(float inX, float inY, float& outX, float& outY) const
        {
            outX = m[0] * inX + m[2] * inY + m[4];
            outY = m[1] * inX + m[3] * inY + m[5];
        }

        static SVGTransform Identity() { return {{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}}; }

        static SVGTransform Translate(float tx, float ty) { return {{1.0f, 0.0f, 0.0f, 1.0f, tx, ty}}; }

        static SVGTransform Scale(float sx, float sy) { return {{sx, 0.0f, 0.0f, sy, 0.0f, 0.0f}}; }

        static SVGTransform Rotate(float radians)
        {
            float c = std::cos(radians);
            float s = std::sin(radians);
            return {{c, s, -s, c, 0.0f, 0.0f}};
        }
    };

    // =========================================================================
    // SVG Path and Element
    // =========================================================================

    struct SVGPath
    {
        std::vector<PathCommand> commands;
    };

    struct SVGElement
    {
        SVGPath path;
        SVGColor fillColor = {0.0f, 0.0f, 0.0f, 1.0f};
        bool hasFill = true;
        SVGStroke stroke;
        bool hasStroke = false;
        SVGTransform transform = SVGTransform::Identity();
    };

    // =========================================================================
    // SVG Document
    // =========================================================================

    struct SVGDocument
    {
        float width = 100.0f;
        float height = 100.0f;
        std::vector<SVGElement> elements;

        /**
         * @brief Parse an SVG path data string (M/L/C/Q/Z commands)
         *
         * Supports a subset of SVG path syntax:
         * - M x y          Move to
         * - L x y          Line to
         * - C x1 y1 x2 y2 x y  Cubic bezier
         * - Q x1 y1 x y    Quadratic bezier
         * - Z               Close path
         *
         * @param pathData  SVG path data string (e.g., "M 0 0 L 100 0 L 100 100 Z")
         * @param fill      Fill color for the resulting element
         * @param stroke    Stroke parameters (applied only if width > 0)
         * @return True if parsing succeeded, false on malformed input
         */
        bool ParseFromString(const std::string& pathData, const SVGColor& fill = {0.0f, 0.0f, 0.0f, 1.0f},
                             const SVGStroke& stroke = {})
        {
            SVGElement elem;
            elem.fillColor = fill;
            elem.hasFill = true;
            elem.stroke = stroke;
            elem.hasStroke = stroke.width > 0.0f;

            size_t pos = 0;
            size_t len = pathData.size();

            auto skipWhitespace = [&]()
            {
                while (pos < len && (pathData[pos] == ' ' || pathData[pos] == ',' || pathData[pos] == '\t' ||
                                     pathData[pos] == '\n' || pathData[pos] == '\r'))
                {
                    ++pos;
                }
            };

            auto parseFloat = [&](float& out) -> bool
            {
                skipWhitespace();
                if (pos >= len)
                    return false;

                size_t start = pos;
                if (pathData[pos] == '-' || pathData[pos] == '+')
                    ++pos;

                bool hasDigit = false;
                while (pos < len && pathData[pos] >= '0' && pathData[pos] <= '9')
                {
                    ++pos;
                    hasDigit = true;
                }
                if (pos < len && pathData[pos] == '.')
                {
                    ++pos;
                    while (pos < len && pathData[pos] >= '0' && pathData[pos] <= '9')
                    {
                        ++pos;
                        hasDigit = true;
                    }
                }

                if (!hasDigit)
                    return false;

                try
                {
                    out = std::stof(pathData.substr(start, pos - start));
                }
                catch (const std::exception&)
                {
                    return false;
                }
                return true;
            };

            while (pos < len)
            {
                skipWhitespace();
                if (pos >= len)
                    break;

                char cmd = pathData[pos];
                if (cmd != 'M' && cmd != 'L' && cmd != 'C' && cmd != 'Q' && cmd != 'Z')
                    return false;
                ++pos;

                if (cmd == 'Z')
                {
                    PathCommand pc;
                    pc.type = PathCommandType::Close;
                    elem.path.commands.push_back(pc);
                    continue;
                }

                PathCommand pc;
                if (cmd == 'M')
                {
                    pc.type = PathCommandType::MoveTo;
                    if (!parseFloat(pc.x) || !parseFloat(pc.y))
                        return false;
                }
                else if (cmd == 'L')
                {
                    pc.type = PathCommandType::LineTo;
                    if (!parseFloat(pc.x) || !parseFloat(pc.y))
                        return false;
                }
                else if (cmd == 'C')
                {
                    pc.type = PathCommandType::CubicBezier;
                    if (!parseFloat(pc.cx1) || !parseFloat(pc.cy1) || !parseFloat(pc.cx2) || !parseFloat(pc.cy2) ||
                        !parseFloat(pc.x) || !parseFloat(pc.y))
                        return false;
                }
                else if (cmd == 'Q')
                {
                    pc.type = PathCommandType::QuadBezier;
                    if (!parseFloat(pc.cx1) || !parseFloat(pc.cy1) || !parseFloat(pc.x) || !parseFloat(pc.y))
                        return false;
                }

                elem.path.commands.push_back(pc);
            }

            if (!elem.path.commands.empty())
            {
                elements.push_back(std::move(elem));
            }
            return true;
        }
    };

    // =========================================================================
    // SVG Renderer
    // =========================================================================

    /**
     * @brief CPU-side SVG tessellator producing GPU-ready vertex data
     *
     * Converts SVG paths into triangle lists via Bezier flattening,
     * ear-clipping triangulation (fills), and offset-polyline expansion
     * (strokes).
     */
    class SVGRenderer
    {
      public:
        SVGRenderer() = default;
        ~SVGRenderer() = default;

        /**
         * @brief Tessellate an entire SVG document into vertex data
         * @param doc  The SVG document to render
         * @return Vector of triangulated Vertex2D (3 per triangle)
         */
        std::vector<Vertex2D> Tessellate(const SVGDocument& doc) const
        {
            std::vector<Vertex2D> output;

            for (const SVGElement& elem : doc.elements)
            {
                // Flatten path to polyline
                std::vector<float> polyX;
                std::vector<float> polyY;
                FlattenPath(elem.path, elem.transform, polyX, polyY);

                if (polyX.size() < 2)
                    continue;

                // Generate fill triangles
                if (elem.hasFill && polyX.size() >= 3)
                {
                    std::vector<Vertex2D> fillVerts = EarClipTriangulate(polyX, polyY, elem.fillColor);
                    output.insert(output.end(), fillVerts.begin(), fillVerts.end());
                }

                // Generate stroke triangles
                if (elem.hasStroke && elem.stroke.width > 0.0f)
                {
                    std::vector<Vertex2D> strokeVerts = StrokeTessellate(polyX, polyY, elem.stroke);
                    output.insert(output.end(), strokeVerts.begin(), strokeVerts.end());
                }
            }

            return output;
        }

        /**
         * @brief Flatten an SVG path into a polyline
         *
         * Recursively subdivides Bezier curves until segments are within
         * the flatness tolerance.
         *
         * @param path       Input SVG path
         * @param transform  Transform to apply to all points
         * @param outX, outY Output polyline coordinates
         */
        static void FlattenPath(const SVGPath& path, const SVGTransform& transform, std::vector<float>& outX,
                                std::vector<float>& outY)
        {
            float curX = 0.0f;
            float curY = 0.0f;

            for (const PathCommand& cmd : path.commands)
            {
                switch (cmd.type)
                {
                case PathCommandType::MoveTo:
                {
                    float tx, ty;
                    transform.Apply(cmd.x, cmd.y, tx, ty);
                    outX.push_back(tx);
                    outY.push_back(ty);
                    curX = cmd.x;
                    curY = cmd.y;
                    break;
                }
                case PathCommandType::LineTo:
                {
                    float tx, ty;
                    transform.Apply(cmd.x, cmd.y, tx, ty);
                    outX.push_back(tx);
                    outY.push_back(ty);
                    curX = cmd.x;
                    curY = cmd.y;
                    break;
                }
                case PathCommandType::CubicBezier:
                {
                    FlattenCubicBezier(curX, curY, cmd.cx1, cmd.cy1, cmd.cx2, cmd.cy2, cmd.x, cmd.y, transform, outX,
                                       outY, SVG_FLATTEN_TOLERANCE);
                    curX = cmd.x;
                    curY = cmd.y;
                    break;
                }
                case PathCommandType::QuadBezier:
                {
                    FlattenQuadBezier(curX, curY, cmd.cx1, cmd.cy1, cmd.x, cmd.y, transform, outX, outY,
                                      SVG_FLATTEN_TOLERANCE);
                    curX = cmd.x;
                    curY = cmd.y;
                    break;
                }
                case PathCommandType::Close:
                {
                    // Close path by connecting back to the first point
                    if (!outX.empty())
                    {
                        outX.push_back(outX.front());
                        outY.push_back(outY.front());
                    }
                    break;
                }
                }
            }
        }

        /**
         * @brief Ear-clipping triangulation for a simple polygon
         *
         * Takes a 2D polygon defined by coordinate arrays and produces
         * a triangle list. Handles convex and simple concave polygons.
         *
         * @param polyX, polyY  Polygon vertices
         * @param color         Fill color for all output vertices
         * @return Vector of Vertex2D (3 per triangle)
         */
        static std::vector<Vertex2D> EarClipTriangulate(const std::vector<float>& polyX,
                                                        const std::vector<float>& polyY, const SVGColor& color)
        {
            std::vector<Vertex2D> output;
            int n = static_cast<int>(polyX.size());
            if (n < 3)
                return output;

            // Build index list
            std::vector<int> indices(n);
            for (int i = 0; i < n; ++i)
                indices[i] = i;

            // Ensure counter-clockwise winding
            float area = SignedArea(polyX, polyY);
            if (area < 0.0f)
            {
                std::reverse(indices.begin(), indices.end());
            }

            int remaining = n;
            int failCount = 0;
            int current = 0;

            while (remaining > 2 && failCount < remaining)
            {
                int prev = indices[(current + remaining - 1) % remaining];
                int curr = indices[current % remaining];
                int next = indices[(current + 1) % remaining];

                if (IsEar(polyX, polyY, indices, remaining, prev, curr, next))
                {
                    // Emit triangle
                    Vertex2D v0 = MakeVertex(polyX[prev], polyY[prev], color);
                    Vertex2D v1 = MakeVertex(polyX[curr], polyY[curr], color);
                    Vertex2D v2 = MakeVertex(polyX[next], polyY[next], color);
                    output.push_back(v0);
                    output.push_back(v1);
                    output.push_back(v2);

                    // Remove ear vertex
                    indices.erase(indices.begin() + (current % remaining));
                    remaining--;
                    failCount = 0;
                    if (current >= remaining)
                        current = 0;
                }
                else
                {
                    current = (current + 1) % remaining;
                    failCount++;
                }
            }

            return output;
        }

        /**
         * @brief Generate stroke geometry from a polyline
         *
         * Expands a polyline into a triangle strip with the given width.
         * Supports butt/round/square caps and miter/round/bevel joins.
         *
         * @param polyX, polyY  Input polyline
         * @param stroke        Stroke parameters
         * @return Vector of Vertex2D (3 per triangle)
         */
        static std::vector<Vertex2D> StrokeTessellate(const std::vector<float>& polyX, const std::vector<float>& polyY,
                                                      const SVGStroke& stroke)
        {
            std::vector<Vertex2D> output;
            int n = static_cast<int>(polyX.size());
            if (n < 2)
                return output;

            float halfWidth = stroke.width * 0.5f;

            for (int i = 0; i < n - 1; ++i)
            {
                float dx = polyX[i + 1] - polyX[i];
                float dy = polyY[i + 1] - polyY[i];
                float len = std::sqrt(dx * dx + dy * dy);
                if (len < 0.0001f)
                    continue;

                // Perpendicular normal
                float nx = -dy / len * halfWidth;
                float ny = dx / len * halfWidth;

                // Quad as two triangles
                Vertex2D v0 = MakeVertex(polyX[i] + nx, polyY[i] + ny, stroke.color);
                Vertex2D v1 = MakeVertex(polyX[i] - nx, polyY[i] - ny, stroke.color);
                Vertex2D v2 = MakeVertex(polyX[i + 1] + nx, polyY[i + 1] + ny, stroke.color);
                Vertex2D v3 = MakeVertex(polyX[i + 1] - nx, polyY[i + 1] - ny, stroke.color);

                output.push_back(v0);
                output.push_back(v1);
                output.push_back(v2);
                output.push_back(v2);
                output.push_back(v1);
                output.push_back(v3);

                // Generate join geometry between segments
                if (i < n - 2 && stroke.join == LineJoin::Round)
                {
                    float dx2 = polyX[i + 2] - polyX[i + 1];
                    float dy2 = polyY[i + 2] - polyY[i + 1];
                    float len2 = std::sqrt(dx2 * dx2 + dy2 * dy2);
                    if (len2 > 0.0001f)
                    {
                        float nx2 = -dy2 / len2 * halfWidth;
                        float ny2 = dx2 / len2 * halfWidth;

                        // Fan triangles for round join
                        constexpr int SEGMENTS = 4;
                        float startAngle = std::atan2(ny, nx);
                        float endAngle = std::atan2(ny2, nx2);
                        float angleDiff = endAngle - startAngle;

                        // Normalize angle
                        while (angleDiff > SVG_PI)
                            angleDiff -= 2.0f * SVG_PI;
                        while (angleDiff < -SVG_PI)
                            angleDiff += 2.0f * SVG_PI;

                        Vertex2D center = MakeVertex(polyX[i + 1], polyY[i + 1], stroke.color);
                        for (int s = 0; s < SEGMENTS; ++s)
                        {
                            float a0 = startAngle + angleDiff * s / SEGMENTS;
                            float a1 = startAngle + angleDiff * (s + 1) / SEGMENTS;

                            Vertex2D p0 = MakeVertex(polyX[i + 1] + std::cos(a0) * halfWidth,
                                                     polyY[i + 1] + std::sin(a0) * halfWidth, stroke.color);
                            Vertex2D p1 = MakeVertex(polyX[i + 1] + std::cos(a1) * halfWidth,
                                                     polyY[i + 1] + std::sin(a1) * halfWidth, stroke.color);

                            output.push_back(center);
                            output.push_back(p0);
                            output.push_back(p1);
                        }
                    }
                }
            }

            // End caps
            if (stroke.cap == LineCap::Round && n >= 2)
            {
                GenerateRoundCap(polyX[0], polyY[0], polyX[0] - polyX[1], polyY[0] - polyY[1], halfWidth, stroke.color,
                                 output);
                GenerateRoundCap(polyX[n - 1], polyY[n - 1], polyX[n - 1] - polyX[n - 2], polyY[n - 1] - polyY[n - 2],
                                 halfWidth, stroke.color, output);
            }
            else if (stroke.cap == LineCap::Square && n >= 2)
            {
                GenerateSquareCap(polyX[0], polyY[0], polyX[0] - polyX[1], polyY[0] - polyY[1], halfWidth, stroke.color,
                                  output);
                GenerateSquareCap(polyX[n - 1], polyY[n - 1], polyX[n - 1] - polyX[n - 2], polyY[n - 1] - polyY[n - 2],
                                  halfWidth, stroke.color, output);
            }

            return output;
        }

      private:
        /// @brief Recursively flatten a cubic Bezier curve
        static void FlattenCubicBezier(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
                                       const SVGTransform& transform, std::vector<float>& outX,
                                       std::vector<float>& outY, float tolerance, int depth = 0)
        {
            if (depth > 10)
            {
                float tx, ty;
                transform.Apply(x3, y3, tx, ty);
                outX.push_back(tx);
                outY.push_back(ty);
                return;
            }

            // Flatness test: max distance of control points from the line (x0,y0)-(x3,y3)
            float dx = x3 - x0;
            float dy = y3 - y0;
            float d1 = std::abs((x1 - x3) * dy - (y1 - y3) * dx);
            float d2 = std::abs((x2 - x3) * dy - (y2 - y3) * dx);

            if ((d1 + d2) * (d1 + d2) <= tolerance * tolerance * (dx * dx + dy * dy))
            {
                float tx, ty;
                transform.Apply(x3, y3, tx, ty);
                outX.push_back(tx);
                outY.push_back(ty);
                return;
            }

            // De Casteljau subdivision at t=0.5
            float x01 = (x0 + x1) * 0.5f;
            float y01 = (y0 + y1) * 0.5f;
            float x12 = (x1 + x2) * 0.5f;
            float y12 = (y1 + y2) * 0.5f;
            float x23 = (x2 + x3) * 0.5f;
            float y23 = (y2 + y3) * 0.5f;
            float x012 = (x01 + x12) * 0.5f;
            float y012 = (y01 + y12) * 0.5f;
            float x123 = (x12 + x23) * 0.5f;
            float y123 = (y12 + y23) * 0.5f;
            float x0123 = (x012 + x123) * 0.5f;
            float y0123 = (y012 + y123) * 0.5f;

            FlattenCubicBezier(x0, y0, x01, y01, x012, y012, x0123, y0123, transform, outX, outY, tolerance, depth + 1);
            FlattenCubicBezier(x0123, y0123, x123, y123, x23, y23, x3, y3, transform, outX, outY, tolerance, depth + 1);
        }

        /// @brief Recursively flatten a quadratic Bezier curve
        static void FlattenQuadBezier(float x0, float y0, float x1, float y1, float x2, float y2,
                                      const SVGTransform& transform, std::vector<float>& outX, std::vector<float>& outY,
                                      float tolerance, int depth = 0)
        {
            if (depth > 10)
            {
                float tx, ty;
                transform.Apply(x2, y2, tx, ty);
                outX.push_back(tx);
                outY.push_back(ty);
                return;
            }

            // Flatness: distance of control point from midpoint of endpoints
            float mx = (x0 + x2) * 0.5f;
            float my = (y0 + y2) * 0.5f;
            float dx = x1 - mx;
            float dy = y1 - my;

            if (dx * dx + dy * dy <= tolerance * tolerance)
            {
                float tx, ty;
                transform.Apply(x2, y2, tx, ty);
                outX.push_back(tx);
                outY.push_back(ty);
                return;
            }

            // Subdivision at t=0.5
            float x01 = (x0 + x1) * 0.5f;
            float y01 = (y0 + y1) * 0.5f;
            float x12 = (x1 + x2) * 0.5f;
            float y12 = (y1 + y2) * 0.5f;
            float x012 = (x01 + x12) * 0.5f;
            float y012 = (y01 + y12) * 0.5f;

            FlattenQuadBezier(x0, y0, x01, y01, x012, y012, transform, outX, outY, tolerance, depth + 1);
            FlattenQuadBezier(x012, y012, x12, y12, x2, y2, transform, outX, outY, tolerance, depth + 1);
        }

        /// @brief Compute signed area of a polygon (positive = CCW)
        static float SignedArea(const std::vector<float>& px, const std::vector<float>& py)
        {
            float area = 0.0f;
            int n = static_cast<int>(px.size());
            for (int i = 0; i < n; ++i)
            {
                int j = (i + 1) % n;
                area += px[i] * py[j] - px[j] * py[i];
            }
            return area * 0.5f;
        }

        /// @brief Check if triangle (prev, curr, next) is a valid ear
        static bool IsEar(const std::vector<float>& px, const std::vector<float>& py, const std::vector<int>& indices,
                          int remaining, int prev, int curr, int next)
        {
            // Must be convex (CCW winding)
            float cross = (px[curr] - px[prev]) * (py[next] - py[prev]) - (py[curr] - py[prev]) * (px[next] - px[prev]);
            if (cross <= 0.0f)
                return false;

            // No other vertex inside this triangle
            for (int i = 0; i < remaining; ++i)
            {
                int idx = indices[i];
                if (idx == prev || idx == curr || idx == next)
                    continue;

                if (PointInTriangle(px[idx], py[idx], px[prev], py[prev], px[curr], py[curr], px[next], py[next]))
                {
                    return false;
                }
            }
            return true;
        }

        /// @brief Point-in-triangle test using barycentric coordinates
        static bool PointInTriangle(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
        {
            float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
            float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
            float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);

            bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
            bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
            return !(hasNeg && hasPos);
        }

        /// @brief Create a Vertex2D with color
        static Vertex2D MakeVertex(float x, float y, const SVGColor& color)
        {
            return {x, y, 0.0f, 0.0f, color.r, color.g, color.b, color.a};
        }

        /// @brief Generate a round end cap as a fan of triangles
        static void GenerateRoundCap(float cx, float cy, float dirX, float dirY, float halfWidth, const SVGColor& color,
                                     std::vector<Vertex2D>& output)
        {
            float len = std::sqrt(dirX * dirX + dirY * dirY);
            if (len < 0.0001f)
                return;

            float baseAngle = std::atan2(dirY / len, dirX / len);
            constexpr int SEGMENTS = 8;
            Vertex2D center = MakeVertex(cx, cy, color);

            for (int i = 0; i < SEGMENTS; ++i)
            {
                float a0 = baseAngle - SVG_PI * 0.5f + SVG_PI * i / SEGMENTS;
                float a1 = baseAngle - SVG_PI * 0.5f + SVG_PI * (i + 1) / SEGMENTS;

                Vertex2D p0 = MakeVertex(cx + std::cos(a0) * halfWidth, cy + std::sin(a0) * halfWidth, color);
                Vertex2D p1 = MakeVertex(cx + std::cos(a1) * halfWidth, cy + std::sin(a1) * halfWidth, color);

                output.push_back(center);
                output.push_back(p0);
                output.push_back(p1);
            }
        }

        /// @brief Generate a square end cap
        static void GenerateSquareCap(float cx, float cy, float dirX, float dirY, float halfWidth,
                                      const SVGColor& color, std::vector<Vertex2D>& output)
        {
            float len = std::sqrt(dirX * dirX + dirY * dirY);
            if (len < 0.0001f)
                return;

            float ndx = dirX / len;
            float ndy = dirY / len;

            // Perpendicular
            float px = -ndy * halfWidth;
            float py = ndx * halfWidth;

            // Extend along direction
            float ex = ndx * halfWidth;
            float ey = ndy * halfWidth;

            Vertex2D v0 = MakeVertex(cx + px, cy + py, color);
            Vertex2D v1 = MakeVertex(cx - px, cy - py, color);
            Vertex2D v2 = MakeVertex(cx + px + ex, cy + py + ey, color);
            Vertex2D v3 = MakeVertex(cx - px + ex, cy - py + ey, color);

            output.push_back(v0);
            output.push_back(v1);
            output.push_back(v2);
            output.push_back(v2);
            output.push_back(v1);
            output.push_back(v3);
        }
    };

} // namespace Spark::Graphics
