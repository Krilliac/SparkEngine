/**
 * @file BlendSpace.cpp
 * @brief Implementation of 2D parametric blend space with Delaunay triangulation
 * @author Spark Engine Team
 * @date 2026
 *
 * Uses an incremental Bowyer-Watson algorithm for Delaunay triangulation of
 * the sample points. Evaluation finds the containing triangle and computes
 * barycentric weights for smooth parametric animation blending.
 */

#include "BlendSpace.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Spark::Animation
{

    // =========================================================================
    // BlendSpace2D
    // =========================================================================

    BlendSpace2D::BlendSpace2D(const std::string& name) : m_name(name) {}

    void BlendSpace2D::AddSample(const XMFLOAT2& position, const std::string& animName, float playbackSpeed)
    {
        BlendSpaceSample sample;
        sample.position = position;
        sample.animationName = animName;
        sample.playbackSpeed = playbackSpeed;
        m_samples.push_back(std::move(sample));
        m_dirty = true;
    }

    bool BlendSpace2D::RemoveSample(const std::string& animName)
    {
        auto it = std::remove_if(m_samples.begin(), m_samples.end(),
                                 [&animName](const BlendSpaceSample& s) { return s.animationName == animName; });

        if (it != m_samples.end())
        {
            m_samples.erase(it, m_samples.end());
            m_dirty = true;
            return true;
        }
        return false;
    }

    // =========================================================================
    // Evaluation
    // =========================================================================

    BlendSpaceResult BlendSpace2D::Evaluate(float paramX, float paramY) const
    {
        BlendSpaceResult result;

        if (m_samples.empty())
        {
            return result;
        }

        // Single sample: return it with weight 1.0
        if (m_samples.size() == 1)
        {
            WeightedAnimation wa;
            wa.animationName = m_samples[0].animationName;
            wa.weight = 1.0f;
            wa.playbackSpeed = m_samples[0].playbackSpeed;
            result.animations.push_back(std::move(wa));
            return result;
        }

        // Two samples: interpolate along the line between them
        if (m_samples.size() == 2)
        {
            const auto& a = m_samples[0];
            const auto& b = m_samples[1];

            float dx = b.position.x - a.position.x;
            float dy = b.position.y - a.position.y;
            float lenSq = dx * dx + dy * dy;

            float t = 0.5f;
            if (lenSq > 0.0001f)
            {
                t = ((paramX - a.position.x) * dx + (paramY - a.position.y) * dy) / lenSq;
                t = std::clamp(t, 0.0f, 1.0f);
            }

            if (t > 0.001f)
            {
                WeightedAnimation wb;
                wb.animationName = b.animationName;
                wb.weight = t;
                wb.playbackSpeed = b.playbackSpeed;
                result.animations.push_back(std::move(wb));
            }

            if ((1.0f - t) > 0.001f)
            {
                WeightedAnimation wa;
                wa.animationName = a.animationName;
                wa.weight = 1.0f - t;
                wa.playbackSpeed = a.playbackSpeed;
                result.animations.push_back(std::move(wa));
            }

            return result;
        }

        // Ensure triangulation is current
        if (m_dirty)
        {
            const_cast<BlendSpace2D*>(this)->Retriangulate();
        }

        // Find containing triangle
        float weights[3] = {0.0f, 0.0f, 0.0f};
        int triIdx = FindContainingTriangle(paramX, paramY, weights);

        if (triIdx >= 0)
        {
            const auto& tri = m_triangles[triIdx];
            for (int i = 0; i < 3; ++i)
            {
                if (weights[i] > 0.001f)
                {
                    const auto& sample = m_samples[tri.indices[i]];
                    WeightedAnimation wa;
                    wa.animationName = sample.animationName;
                    wa.weight = weights[i];
                    wa.playbackSpeed = sample.playbackSpeed;
                    result.animations.push_back(std::move(wa));
                }
            }
        }
        else
        {
            // Fallback: nearest sample
            size_t nearest = FindNearestSample(paramX, paramY);
            WeightedAnimation wa;
            wa.animationName = m_samples[nearest].animationName;
            wa.weight = 1.0f;
            wa.playbackSpeed = m_samples[nearest].playbackSpeed;
            result.animations.push_back(std::move(wa));
        }

        return result;
    }

    // =========================================================================
    // Triangulation (Simplified Bowyer-Watson)
    // =========================================================================

    void BlendSpace2D::Retriangulate()
    {
        m_triangles.clear();
        m_dirty = false;

        size_t n = m_samples.size();
        if (n < 3)
        {
            return;
        }

        // Find bounding box of all samples
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();

        for (const auto& s : m_samples)
        {
            minX = std::min(minX, s.position.x);
            maxX = std::max(maxX, s.position.x);
            minY = std::min(minY, s.position.y);
            maxY = std::max(maxY, s.position.y);
        }

        float dx = maxX - minX;
        float dy = maxY - minY;
        float dMax = std::max(dx, dy);
        float midX = (minX + maxX) * 0.5f;
        float midY = (minY + maxY) * 0.5f;

        // Create a super-triangle that encloses all points
        // Super-triangle vertices are appended to a temporary list
        struct Point
        {
            float x, y;
        };
        std::vector<Point> points;
        points.reserve(n + 3);

        for (const auto& s : m_samples)
        {
            points.push_back({s.position.x, s.position.y});
        }

        // Super-triangle vertices at indices n, n+1, n+2
        points.push_back({midX - 2.0f * dMax, midY - dMax});
        points.push_back({midX + 2.0f * dMax, midY - dMax});
        points.push_back({midX, midY + 2.0f * dMax});

        // Initial triangle is the super-triangle
        struct Tri
        {
            uint32_t a, b, c;
        };
        std::vector<Tri> tris;
        tris.push_back({static_cast<uint32_t>(n), static_cast<uint32_t>(n + 1), static_cast<uint32_t>(n + 2)});

        // Circumcircle test lambda
        auto InCircumcircle = [&points](uint32_t ai, uint32_t bi, uint32_t ci, uint32_t pi) -> bool
        {
            float ax = points[ai].x - points[pi].x;
            float ay = points[ai].y - points[pi].y;
            float bx = points[bi].x - points[pi].x;
            float by = points[bi].y - points[pi].y;
            float cx = points[ci].x - points[pi].x;
            float cy = points[ci].y - points[pi].y;

            float det = ax * (by * (cx * cx + cy * cy) - cy * (bx * bx + by * by)) -
                        ay * (bx * (cx * cx + cy * cy) - cx * (bx * bx + by * by)) +
                        (ax * ax + ay * ay) * (bx * cy - by * cx);
            return det > 0.0f;
        };

        // Insert points one at a time
        for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i)
        {
            // Find all triangles whose circumcircle contains the new point
            std::vector<Tri> badTriangles;
            std::vector<Tri> goodTriangles;

            for (const auto& tri : tris)
            {
                if (InCircumcircle(tri.a, tri.b, tri.c, i))
                {
                    badTriangles.push_back(tri);
                }
                else
                {
                    goodTriangles.push_back(tri);
                }
            }

            // Find the boundary polygon (edges of bad triangles not shared by two bad triangles)
            struct Edge
            {
                uint32_t a, b;
            };
            std::vector<Edge> polygon;

            for (const auto& tri : badTriangles)
            {
                Edge edges[3] = {{tri.a, tri.b}, {tri.b, tri.c}, {tri.c, tri.a}};

                for (const auto& edge : edges)
                {
                    bool shared = false;
                    for (const auto& other : badTriangles)
                    {
                        if (&other == &tri)
                            continue;

                        // Check if this edge exists in the other triangle (in either direction)
                        uint32_t ov[3] = {other.a, other.b, other.c};
                        for (int j = 0; j < 3; ++j)
                        {
                            uint32_t ea = ov[j];
                            uint32_t eb = ov[(j + 1) % 3];
                            if ((ea == edge.a && eb == edge.b) || (ea == edge.b && eb == edge.a))
                            {
                                shared = true;
                                break;
                            }
                        }
                        if (shared)
                            break;
                    }

                    if (!shared)
                    {
                        polygon.push_back(edge);
                    }
                }
            }

            // Create new triangles from boundary edges to the new point
            tris = std::move(goodTriangles);
            for (const auto& edge : polygon)
            {
                tris.push_back({edge.a, edge.b, i});
            }
        }

        // Remove triangles referencing super-triangle vertices
        for (const auto& tri : tris)
        {
            if (tri.a >= static_cast<uint32_t>(n) || tri.b >= static_cast<uint32_t>(n) ||
                tri.c >= static_cast<uint32_t>(n))
            {
                continue;
            }

            BlendTriangle bt;
            bt.indices[0] = tri.a;
            bt.indices[1] = tri.b;
            bt.indices[2] = tri.c;
            m_triangles.push_back(bt);
        }
    }

    // =========================================================================
    // Triangle Lookup
    // =========================================================================

    int BlendSpace2D::FindContainingTriangle(float px, float py, float outWeights[3]) const
    {
        for (size_t i = 0; i < m_triangles.size(); ++i)
        {
            const auto& tri = m_triangles[i];
            const auto& a = m_samples[tri.indices[0]].position;
            const auto& b = m_samples[tri.indices[1]].position;
            const auto& c = m_samples[tri.indices[2]].position;

            float u = 0.0f;
            float v = 0.0f;
            float w = 0.0f;

            if (ComputeBarycentric(px, py, a.x, a.y, b.x, b.y, c.x, c.y, u, v, w))
            {
                outWeights[0] = u;
                outWeights[1] = v;
                outWeights[2] = w;
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    size_t BlendSpace2D::FindNearestSample(float px, float py) const
    {
        size_t nearest = 0;
        float bestDist = std::numeric_limits<float>::max();

        for (size_t i = 0; i < m_samples.size(); ++i)
        {
            float dx = m_samples[i].position.x - px;
            float dy = m_samples[i].position.y - py;
            float dist = dx * dx + dy * dy;
            if (dist < bestDist)
            {
                bestDist = dist;
                nearest = i;
            }
        }

        return nearest;
    }

    bool BlendSpace2D::ComputeBarycentric(float px, float py, float ax, float ay, float bx, float by, float cx,
                                          float cy, float& u, float& v, float& w)
    {
        float v0x = bx - ax;
        float v0y = by - ay;
        float v1x = cx - ax;
        float v1y = cy - ay;
        float v2x = px - ax;
        float v2y = py - ay;

        float dot00 = v0x * v0x + v0y * v0y;
        float dot01 = v0x * v1x + v0y * v1y;
        float dot02 = v0x * v2x + v0y * v2y;
        float dot11 = v1x * v1x + v1y * v1y;
        float dot12 = v1x * v2x + v1y * v2y;

        float denom = dot00 * dot11 - dot01 * dot01;
        if (std::abs(denom) < 0.0001f)
        {
            return false; // Degenerate triangle
        }

        float invDenom = 1.0f / denom;
        v = (dot00 * dot12 - dot01 * dot02) * invDenom;
        w = (dot11 * dot02 - dot01 * dot12) * invDenom;
        u = 1.0f - v - w;

        // Allow small epsilon for edge cases
        constexpr float kEpsilon = -0.001f;
        return (u >= kEpsilon && v >= kEpsilon && w >= kEpsilon);
    }

    // =========================================================================
    // BlendSpaceManager
    // =========================================================================

    BlendSpace2D& BlendSpaceManager::CreateBlendSpace(const std::string& name)
    {
        auto [it, inserted] = m_blendSpaces.emplace(name, BlendSpace2D(name));
        return it->second;
    }

    BlendSpace2D* BlendSpaceManager::GetBlendSpace(const std::string& name)
    {
        auto it = m_blendSpaces.find(name);
        if (it != m_blendSpaces.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    bool BlendSpaceManager::RemoveBlendSpace(const std::string& name)
    {
        return m_blendSpaces.erase(name) > 0;
    }

} // namespace Spark::Animation
