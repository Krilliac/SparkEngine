/**
 * @file ClothSimulation.h
 * @brief Position-based cloth and soft body simulation
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides a position-based dynamics (PBD) cloth simulator for flags, capes,
 * curtains, and other deformable surfaces. Also includes a soft body system
 * for deformable physics objects.
 *
 * ## Features
 * - Distance constraints (structural, shear, bending)
 * - Pin constraints (attach cloth to static/kinematic points)
 * - Wind and gravity forces
 * - Collision with spheres, capsules, and planes
 * - Configurable iteration count for quality/performance trade-off
 * - Self-collision (optional, expensive)
 *
 * ## Usage
 * @code
 *   ClothSimulation cloth;
 *   ClothDescriptor desc;
 *   desc.width = 10; desc.height = 10;
 *   desc.spacing = 0.1f;
 *   desc.mass = 0.5f;
 *   desc.stiffness = 0.9f;
 *   desc.damping = 0.01f;
 *
 *   auto id = cloth.CreateCloth(desc);
 *   cloth.PinParticle(id, 0, {0, 2, 0});   // Pin top-left corner
 *   cloth.PinParticle(id, 9, {0.9f, 2, 0}); // Pin top-right corner
 *   cloth.SetWind(id, {1, 0, 0.5f});
 *
 *   // Per frame:
 *   cloth.Update(deltaTime);
 *   auto& particles = cloth.GetParticles(id);
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <utility>

namespace Spark::Physics
{

    // =============================================================================
    // ClothParticle
    // =============================================================================

    /**
 * @brief A single particle in the cloth simulation.
 */
    struct ClothParticle
    {
        DirectX::XMFLOAT3 position{0, 0, 0};     ///< Current world position
        DirectX::XMFLOAT3 prevPosition{0, 0, 0}; ///< Previous frame position (for Verlet)
        DirectX::XMFLOAT3 velocity{0, 0, 0};     ///< Current velocity
        DirectX::XMFLOAT3 acceleration{0, 0, 0}; ///< Accumulated forces / mass
        float inverseMass = 1.0f;                ///< 0 = pinned/static
        bool pinned = false;                     ///< Whether this particle is pinned
    };

    // =============================================================================
    // ClothConstraint
    // =============================================================================

    /**
 * @brief A distance constraint between two particles.
 */
    struct ClothConstraint
    {
        uint32_t particleA = 0;  ///< First particle index
        uint32_t particleB = 0;  ///< Second particle index
        float restLength = 0.0f; ///< Target distance
        float stiffness = 1.0f;  ///< Constraint stiffness (0-1)
    };

    // =============================================================================
    // ClothCollider
    // =============================================================================

    /**
 * @brief A simple collider for cloth-to-rigid-body interaction.
 */
    struct ClothCollider
    {
        enum class Type
        {
            Sphere,
            Capsule,
            Plane
        };
        Type type = Type::Sphere;
        DirectX::XMFLOAT3 position{0, 0, 0};
        DirectX::XMFLOAT3 normal{0, 1, 0}; ///< Normal for plane, axis for capsule
        float radius = 0.5f;
        float height = 1.0f; ///< Height for capsule
    };

    // =============================================================================
    // ClothDescriptor
    // =============================================================================

    /**
 * @brief Configuration for creating a cloth instance.
 */
    struct ClothDescriptor
    {
        int width = 10;                    ///< Particles in X direction
        int height = 10;                   ///< Particles in Y direction
        float spacing = 0.1f;              ///< Distance between adjacent particles
        float mass = 1.0f;                 ///< Total cloth mass
        float stiffness = 0.9f;            ///< Constraint stiffness (0-1)
        float bendStiffness = 0.5f;        ///< Bending stiffness (0-1)
        float damping = 0.01f;             ///< Velocity damping
        DirectX::XMFLOAT3 origin{0, 0, 0}; ///< World-space origin
        int solverIterations = 4;          ///< Constraint solver iterations per frame
    };

    // =============================================================================
    // ClothInstance
    // =============================================================================

    /**
 * @brief Runtime state of a single cloth simulation.
 */
    struct ClothInstance
    {
        uint32_t id = 0;
        std::vector<ClothParticle> particles;
        std::vector<ClothConstraint> constraints;
        std::vector<ClothCollider> colliders;
        DirectX::XMFLOAT3 gravity{0, -9.81f, 0};
        DirectX::XMFLOAT3 wind{0, 0, 0};
        float damping = 0.01f;
        int solverIterations = 4;
        int width = 0;
        int height = 0;
        bool enabled = true;
    };

    // =============================================================================
    // SoftBodyDescriptor
    // =============================================================================

    /**
 * @brief Configuration for creating a volumetric soft body.
 */
    struct SoftBodyDescriptor
    {
        std::vector<DirectX::XMFLOAT3> vertices; ///< Initial vertex positions
        std::vector<uint32_t> tetrahedra;        ///< Tetrahedral mesh connectivity
        float mass = 5.0f;
        float stiffness = 0.8f;
        float volumeStiffness = 0.9f; ///< Volume preservation strength
        float damping = 0.02f;
        int solverIterations = 8;
    };

    // =============================================================================
    // ClothSimulation
    // =============================================================================

    /**
 * @class ClothSimulation
 * @brief Manages all cloth and soft body instances.
 */
    class ClothSimulation
    {
      public:
        ClothSimulation();
        ~ClothSimulation() = default;

        /**
     * @brief Create a new cloth instance from a descriptor.
     * @param desc Cloth configuration.
     * @return Handle to the cloth instance.
     */
        uint32_t CreateCloth(const ClothDescriptor& desc);

        /**
     * @brief Destroy a cloth instance.
     * @param clothId Cloth handle.
     */
        void DestroyCloth(uint32_t clothId);

        /**
     * @brief Pin a particle to a fixed position.
     * @param clothId   Cloth handle.
     * @param particleIndex Index of the particle to pin.
     * @param position  World-space position to pin to.
     */
        void PinParticle(uint32_t clothId, uint32_t particleIndex, const DirectX::XMFLOAT3& position);

        /**
     * @brief Unpin a particle.
     * @param clothId   Cloth handle.
     * @param particleIndex Particle index.
     */
        void UnpinParticle(uint32_t clothId, uint32_t particleIndex);

        /**
     * @brief Set wind force for a cloth.
     * @param clothId Cloth handle.
     * @param wind    Wind direction and strength.
     */
        void SetWind(uint32_t clothId, const DirectX::XMFLOAT3& wind);

        /**
     * @brief Add a collider for cloth interaction.
     * @param clothId  Cloth handle.
     * @param collider Collider definition.
     */
        void AddCollider(uint32_t clothId, const ClothCollider& collider);

        /**
     * @brief Update all cloth simulations.
     * @param deltaTime Frame time in seconds.
     */
        void Update(float deltaTime);

        /**
     * @brief Get the particles for rendering.
     * @param clothId Cloth handle.
     * @return Reference to particle array.
     */
        const std::vector<ClothParticle>& GetParticles(uint32_t clothId) const;

        /**
         * @brief Get the cloth grid dimensions.
         * @param clothId Cloth handle.
         * @return Pair of {width, height}
         */
        std::pair<int, int> GetClothDimensions(uint32_t clothId) const;

        /** @brief Get the number of active cloth instances. */
        size_t GetInstanceCount() const noexcept { return m_clothInstances.size(); }

        // --- Console integration ---

        /** @brief Get simulation status (console integration). */
        std::string Console_GetStatus() const;

      private:
        void SimulateCloth(ClothInstance& cloth, float deltaTime);
        void ApplyForces(ClothInstance& cloth, float deltaTime);
        void SolveConstraints(ClothInstance& cloth);
        void HandleCollisions(ClothInstance& cloth);
        void IntegratePositions(ClothInstance& cloth, float deltaTime);

        std::unordered_map<uint32_t, ClothInstance> m_clothInstances;
        uint32_t m_nextId = 1;
        static const std::vector<ClothParticle> s_emptyParticles;
    };

} // namespace Spark::Physics
