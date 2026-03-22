#include "../Core/Platform.h"
/**
 * @file PhysicsDebugRenderer.cpp
 * @brief Debug visualization data collector for Jolt Physics
 * @author Spark Engine Team
 * @date 2025
 */

#include "PhysicsDebugRenderer.h"

void PhysicsDebugRenderer::Clear()
{
    m_lines.clear();
    m_triangles.clear();
}

void PhysicsDebugRenderer::AddLine(const XMFLOAT3& from, const XMFLOAT3& to, const XMFLOAT4& color)
{
    m_lines.push_back({from, to, color});
}

void PhysicsDebugRenderer::AddTriangle(const XMFLOAT3& v1, const XMFLOAT3& v2, const XMFLOAT3& v3,
                                       const XMFLOAT4& color)
{
    m_triangles.push_back({v1, v2, v3, color});
}
