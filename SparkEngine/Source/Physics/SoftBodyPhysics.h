/**
 * @file SoftBodyPhysics.h
 * @brief Jolt soft body wrapper for cloth, ropes, and deformable objects
 * @author Spark Engine Team
 * @date 2025
 *
 * Wraps Jolt's XPBD-based soft body simulation for cloth, flags, capes,
 * and deformable physics objects with edge, bend, and volume constraints.
 *
 * @see PhysicsSystem.h, PhysicsTypes.h
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <cstdint>
#include <memory>
#include <vector>

using DirectX::XMFLOAT3;

class PhysicsSystem;

/**
 * @brief Soft body vertex (particle) description.
 */
struct SoftBodyVertexDesc
{
    XMFLOAT3 position = {0, 0, 0}; ///< Initial position
    float invMass = 1.0f;          ///< Inverse mass (0 = pinned/fixed)
};

/**
 * @brief Soft body edge constraint (distance constraint between two vertices).
 */
struct SoftBodyEdge
{
    uint32_t vertex0 = 0;    ///< First vertex index
    uint32_t vertex1 = 0;    ///< Second vertex index
    float compliance = 0.0f; ///< Constraint compliance (0 = rigid, higher = softer)
};

/**
 * @brief Configuration for creating a soft body.
 */
struct SoftBodyDesc
{
    std::vector<SoftBodyVertexDesc> vertices; ///< Particle positions and masses
    std::vector<SoftBodyEdge> edges;          ///< Distance constraints between vertices
    std::vector<uint32_t> triangles;          ///< Triangle indices (groups of 3) for collision and rendering

    float pressure = 0.0f;      ///< Internal pressure for inflatable objects (0 = no pressure)
    float friction = 0.2f;      ///< Friction against rigid bodies
    float restitution = 0.0f;   ///< Bounce against rigid bodies
    float linearDamping = 0.1f; ///< Velocity damping
    float gravityFactor = 1.0f; ///< Gravity multiplier

    uint32_t numIterations = 4;  ///< XPBD solver iterations (more = stiffer, slower)
    bool enableCollision = true; ///< Enable collision with rigid bodies
};

/**
 * @class SoftBody
 * @brief XPBD-based soft body for cloth, flags, capes, and deformable objects.
 *
 * ### Usage
 * @code
 *   SoftBodyDesc desc;
 *   desc.vertices = clothVertices;
 *   desc.edges = clothEdges;
 *   desc.triangles = clothTriangles;
 *   auto softBody = physics.CreateSoftBody(desc);
 *
 *   // Each frame, read vertex positions for rendering:
 *   for (uint32_t i = 0; i < softBody->GetVertexCount(); i++)
 *   {
 *       meshVertices[i].position = softBody->GetVertexPosition(i);
 *   }
 * @endcode
 */
class SoftBody
{
  public:
    SoftBody(PhysicsSystem* physicsSystem, const SoftBodyDesc& desc);
    ~SoftBody();

    // Non-copyable
    SoftBody(const SoftBody&) = delete;
    SoftBody& operator=(const SoftBody&) = delete;

    // =========================================================================
    // Vertex access (for rendering)
    // =========================================================================

    /** @brief Get number of vertices. */
    uint32_t GetVertexCount() const;

    /** @brief Get the world-space position of a vertex. */
    XMFLOAT3 GetVertexPosition(uint32_t index) const;

    /** @brief Get the world-space normal of a vertex. */
    XMFLOAT3 GetVertexNormal(uint32_t index) const;

    /** @brief Get all vertex positions at once (for batch mesh updates). */
    void GetAllVertexPositions(std::vector<XMFLOAT3>& outPositions) const;

    // =========================================================================
    // Pin control
    // =========================================================================

    /**
     * @brief Pin a vertex to a fixed world position (make it kinematic).
     * @param index Vertex index to pin.
     */
    void PinVertex(uint32_t index);

    /**
     * @brief Unpin a vertex (make it dynamic again).
     * @param index Vertex index to unpin.
     */
    void UnpinVertex(uint32_t index);

    /**
     * @brief Move a pinned vertex to a new position.
     * @param index    Vertex index (must be pinned).
     * @param position New world-space position.
     */
    void SetPinnedVertexPosition(uint32_t index, const XMFLOAT3& position);

    // =========================================================================
    // Forces
    // =========================================================================

    /** @brief Apply a force to all vertices (e.g., wind). */
    void ApplyWindForce(const XMFLOAT3& windDirection, float windStrength);

    /** @brief Get the Jolt body ID for the soft body. */
    uint32_t GetJoltBodyID() const { return m_joltBodyID; }

  private:
    PhysicsSystem* m_physicsSystem;
    SoftBodyDesc m_desc;
    uint32_t m_joltBodyID = 0;
};
