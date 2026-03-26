/**
 * @file OcclusionCulling.cpp
 * @brief Implementation of CPU software occlusion culling
 * @author Spark Engine Team
 * @date 2026
 *
 * Software rasterizes simplified occluder meshes into a low-resolution depth buffer,
 * then tests AABB visibility against it. Uses a scanline rasterizer with barycentric
 * interpolation for depth.
 */

#include "OcclusionCulling.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Spark::Graphics
{

    // =========================================================================
    // Initialization
    // =========================================================================

    bool OcclusionCullingSystem::Initialize(const OcclusionSettings& settings)
    {
        if (m_initialized)
        {
            return true;
        }

        m_settings = settings;
        m_settings.bufferWidth = std::max(m_settings.bufferWidth, 16);
        m_settings.bufferHeight = std::max(m_settings.bufferHeight, 16);

        size_t bufferSize = static_cast<size_t>(m_settings.bufferWidth) * static_cast<size_t>(m_settings.bufferHeight);
        m_depthBuffer.resize(bufferSize, 1.0f);

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "OcclusionCulling initialized (%dx%d, maxOccluders=%d)",
                       m_settings.bufferWidth, m_settings.bufferHeight, m_settings.maxOccluders);
        return true;
    }

    void OcclusionCullingSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "OcclusionCulling shutting down");
        m_depthBuffer.clear();
        m_initialized = false;
    }

    // =========================================================================
    // Frame Management
    // =========================================================================

    void OcclusionCullingSystem::BeginFrame(const XMFLOAT4X4& viewMatrix, const XMFLOAT4X4& projMatrix)
    {
        if (!m_initialized || !m_settings.enabled)
        {
            return;
        }

        // Clear depth buffer to far plane
        std::fill(m_depthBuffer.begin(), m_depthBuffer.end(), 1.0f);

        // Compute view-projection matrix (row-major multiplication: view * proj)
        // M[i][j] = sum_k view[i][k] * proj[k][j]
        const auto& v = viewMatrix.m;
        const auto& p = projMatrix.m;

        for (int i = 0; i < 4; ++i)
        {
            for (int j = 0; j < 4; ++j)
            {
                m_viewProj.m[i][j] = v[i][0] * p[0][j] + v[i][1] * p[1][j] + v[i][2] * p[2][j] + v[i][3] * p[3][j];
            }
        }

        m_stats = {};
    }

    OcclusionStats OcclusionCullingSystem::EndFrame()
    {
        return m_stats;
    }

    // =========================================================================
    // Projection
    // =========================================================================

    bool OcclusionCullingSystem::ProjectToScreen(const XMFLOAT3& point, float& outScreenX, float& outScreenY,
                                                 float& outDepth) const
    {
        const auto& m = m_viewProj.m;

        // Transform point by view-projection (row-major: point is a row vector)
        float clipX = point.x * m[0][0] + point.y * m[1][0] + point.z * m[2][0] + m[3][0];
        float clipY = point.x * m[0][1] + point.y * m[1][1] + point.z * m[2][1] + m[3][1];
        float clipZ = point.x * m[0][2] + point.y * m[1][2] + point.z * m[2][2] + m[3][2];
        float clipW = point.x * m[0][3] + point.y * m[1][3] + point.z * m[2][3] + m[3][3];

        // Behind camera check
        if (clipW <= 0.001f)
        {
            return false;
        }

        float invW = 1.0f / clipW;
        float ndcX = clipX * invW;
        float ndcY = clipY * invW;
        float ndcZ = clipZ * invW;

        // NDC [-1,1] to screen space [0, width/height]
        outScreenX = (ndcX * 0.5f + 0.5f) * static_cast<float>(m_settings.bufferWidth);
        outScreenY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(m_settings.bufferHeight);
        outDepth = ndcZ;

        return true;
    }

    // =========================================================================
    // Triangle Rasterization
    // =========================================================================

    void OcclusionCullingSystem::RasterizeTriangle(float x0, float y0, float z0, float x1, float y1, float z1, float x2,
                                                   float y2, float z2)
    {
        // Compute bounding box in screen space
        int minX = std::max(static_cast<int>(std::floor(std::min({x0, x1, x2}))), 0);
        int maxX = std::min(static_cast<int>(std::ceil(std::max({x0, x1, x2}))), m_settings.bufferWidth - 1);
        int minY = std::max(static_cast<int>(std::floor(std::min({y0, y1, y2}))), 0);
        int maxY = std::min(static_cast<int>(std::ceil(std::max({y0, y1, y2}))), m_settings.bufferHeight - 1);

        if (minX > maxX || minY > maxY)
        {
            return;
        }

        // Precompute edge equation denominators for barycentric coordinates
        float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
        if (std::abs(denom) < 0.0001f)
        {
            return; // Degenerate triangle
        }
        float invDenom = 1.0f / denom;

        int width = m_settings.bufferWidth;

        for (int py = minY; py <= maxY; ++py)
        {
            float pfy = static_cast<float>(py) + 0.5f;

            for (int px = minX; px <= maxX; ++px)
            {
                float pfx = static_cast<float>(px) + 0.5f;

                // Barycentric coordinates
                float w0 = ((y1 - y2) * (pfx - x2) + (x2 - x1) * (pfy - y2)) * invDenom;
                float w1 = ((y2 - y0) * (pfx - x2) + (x0 - x2) * (pfy - y2)) * invDenom;
                float w2 = 1.0f - w0 - w1;

                // Point inside triangle check
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                {
                    continue;
                }

                // Interpolate depth
                float depth = w0 * z0 + w1 * z1 + w2 * z2;

                // Depth test (write if closer)
                size_t bufIdx = static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px);
                if (bufIdx < m_depthBuffer.size() && depth < m_depthBuffer[bufIdx])
                {
                    m_depthBuffer[bufIdx] = depth;
                }
            }
        }
    }

    // =========================================================================
    // Occluder Submission
    // =========================================================================

    void OcclusionCullingSystem::SubmitOccluder(const OccluderMesh& mesh, const XMFLOAT4X4& worldMatrix)
    {
        if (!m_initialized || !m_settings.enabled)
        {
            return;
        }

        if (static_cast<int>(m_stats.occludersSubmitted) >= m_settings.maxOccluders)
        {
            return;
        }

        if (mesh.indices.size() < 3 || mesh.vertices.empty())
        {
            return;
        }

        // Transform vertices to world space, then project to screen
        std::vector<XMFLOAT3> screenVerts(mesh.vertices.size());
        std::vector<bool> validVerts(mesh.vertices.size(), false);

        const auto& w = worldMatrix.m;

        for (size_t i = 0; i < mesh.vertices.size(); ++i)
        {
            const auto& v = mesh.vertices[i];

            // World transform (row-major)
            XMFLOAT3 worldPos;
            worldPos.x = v.x * w[0][0] + v.y * w[1][0] + v.z * w[2][0] + w[3][0];
            worldPos.y = v.x * w[0][1] + v.y * w[1][1] + v.z * w[2][1] + w[3][1];
            worldPos.z = v.x * w[0][2] + v.y * w[1][2] + v.z * w[2][2] + w[3][2];

            float sx = 0.0f;
            float sy = 0.0f;
            float sz = 0.0f;
            validVerts[i] = ProjectToScreen(worldPos, sx, sy, sz);
            screenVerts[i] = {sx, sy, sz};
        }

        // Rasterize each triangle
        size_t triCount = mesh.indices.size() / 3;
        for (size_t t = 0; t < triCount; ++t)
        {
            uint32_t i0 = mesh.indices[t * 3 + 0];
            uint32_t i1 = mesh.indices[t * 3 + 1];
            uint32_t i2 = mesh.indices[t * 3 + 2];

            if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
            {
                continue;
            }

            if (!validVerts[i0] || !validVerts[i1] || !validVerts[i2])
            {
                continue;
            }

            RasterizeTriangle(screenVerts[i0].x, screenVerts[i0].y, screenVerts[i0].z, screenVerts[i1].x,
                              screenVerts[i1].y, screenVerts[i1].z, screenVerts[i2].x, screenVerts[i2].y,
                              screenVerts[i2].z);
        }

        m_stats.occludersSubmitted++;
        m_stats.trianglesRasterized += static_cast<uint32_t>(triCount);
    }

    // =========================================================================
    // Visibility Testing
    // =========================================================================

    bool OcclusionCullingSystem::IsRectOccluded(int minX, int minY, int maxX, int maxY, float minDepth) const
    {
        int width = m_settings.bufferWidth;
        int height = m_settings.bufferHeight;

        // Clamp to buffer bounds to prevent out-of-range access
        minX = std::max(minX, 0);
        minY = std::max(minY, 0);
        maxX = std::min(maxX, width - 1);
        maxY = std::min(maxY, height - 1);

        if (minX > maxX || minY > maxY)
        {
            return false;
        }

        for (int py = minY; py <= maxY; ++py)
        {
            for (int px = minX; px <= maxX; ++px)
            {
                size_t bufIdx = static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px);
                if (bufIdx < m_depthBuffer.size() && m_depthBuffer[bufIdx] >= minDepth)
                {
                    // At least one pixel is not occluded
                    return false;
                }
            }
        }

        return true;
    }

    bool OcclusionCullingSystem::TestVisibility(const OcclusionQuery& query) const
    {
        if (!m_initialized || !m_settings.enabled)
        {
            return true; // Conservative: assume visible when disabled
        }

        m_stats.testedCount++;

        // Project all 8 AABB corners to screen space
        const XMFLOAT3& lo = query.boundsMin;
        const XMFLOAT3& hi = query.boundsMax;

        float screenMinX = std::numeric_limits<float>::max();
        float screenMaxX = std::numeric_limits<float>::lowest();
        float screenMinY = std::numeric_limits<float>::max();
        float screenMaxY = std::numeric_limits<float>::lowest();
        float nearestDepth = std::numeric_limits<float>::max();
        int validCorners = 0;

        // Iterate all 8 corners of the AABB
        for (int c = 0; c < 8; ++c)
        {
            XMFLOAT3 corner;
            corner.x = (c & 1) ? hi.x : lo.x;
            corner.y = (c & 2) ? hi.y : lo.y;
            corner.z = (c & 4) ? hi.z : lo.z;

            float sx = 0.0f;
            float sy = 0.0f;
            float sz = 0.0f;

            if (ProjectToScreen(corner, sx, sy, sz))
            {
                screenMinX = std::min(screenMinX, sx);
                screenMaxX = std::max(screenMaxX, sx);
                screenMinY = std::min(screenMinY, sy);
                screenMaxY = std::max(screenMaxY, sy);
                nearestDepth = std::min(nearestDepth, sz);
                validCorners++;
            }
        }

        // If no corners projected (behind camera but might straddle near plane), assume visible
        if (validCorners == 0)
        {
            return true;
        }

        // Clamp to screen bounds
        int minX = std::max(static_cast<int>(std::floor(screenMinX)), 0);
        int maxX = std::min(static_cast<int>(std::ceil(screenMaxX)), m_settings.bufferWidth - 1);
        int minY = std::max(static_cast<int>(std::floor(screenMinY)), 0);
        int maxY = std::min(static_cast<int>(std::ceil(screenMaxY)), m_settings.bufferHeight - 1);

        if (minX > maxX || minY > maxY)
        {
            return true; // Off-screen — conservatively visible
        }

        // Test if the projected rect is fully behind the depth buffer
        bool occluded = IsRectOccluded(minX, minY, maxX, maxY, nearestDepth);

        if (occluded)
        {
            m_stats.occludedCount++;
        }

        return !occluded;
    }

} // namespace Spark::Graphics
