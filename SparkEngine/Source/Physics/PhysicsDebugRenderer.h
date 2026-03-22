/**
 * @file PhysicsDebugRenderer.h
 * @brief Debug visualization for Jolt Physics
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements JPH::DebugRenderer to bridge Jolt's debug drawing into the
 * engine's line renderer. When enabled, draws wireframe collision shapes,
 * contact points, constraint visualizations, and body AABBs.
 *
 * Usage:
 *   physics.SetDebugDrawEnabled(true);
 *   physics.RenderDebug();  // call each frame after Update()
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <vector>
#include <cstdint>

using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;

/**
 * @brief A single debug line segment for physics visualization.
 */
struct PhysicsDebugLine
{
    XMFLOAT3 from;
    XMFLOAT3 to;
    XMFLOAT4 color; // RGBA [0-1]
};

/**
 * @brief A single debug triangle for physics visualization.
 */
struct PhysicsDebugTriangle
{
    XMFLOAT3 v1, v2, v3;
    XMFLOAT4 color;
};

/**
 * @brief Collects debug draw data from Jolt for rendering by the graphics engine.
 *
 * This is a data collector, not a renderer. Call CollectDebugData() after physics
 * Update(), then retrieve lines/triangles and draw them with the engine's
 * immediate-mode line/triangle renderer.
 */
class PhysicsDebugRenderer
{
  public:
    PhysicsDebugRenderer() = default;
    ~PhysicsDebugRenderer() = default;

    /** @brief Clear all collected debug data. */
    void Clear();

    /** @brief Get collected debug lines. */
    const std::vector<PhysicsDebugLine>& GetLines() const { return m_lines; }

    /** @brief Get collected debug triangles. */
    const std::vector<PhysicsDebugTriangle>& GetTriangles() const { return m_triangles; }

    /** @brief Add a line directly (used by PhysicsSystem::RenderDebug). */
    void AddLine(const XMFLOAT3& from, const XMFLOAT3& to, const XMFLOAT4& color);

    /** @brief Add a triangle directly. */
    void AddTriangle(const XMFLOAT3& v1, const XMFLOAT3& v2, const XMFLOAT3& v3, const XMFLOAT4& color);

  private:
    std::vector<PhysicsDebugLine> m_lines;
    std::vector<PhysicsDebugTriangle> m_triangles;
};
