/**
 * @file DestructionSystem.h
 * @brief Destructible environment system for FPS gameplay
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides a system for destructible objects — walls, cover, crates, etc.
 * When a destructible object takes enough damage, it fractures into debris
 * pieces that become physics-simulated. Supports health-based destruction,
 * fracture patterns, debris spawning, and damage propagation.
 *
 * ## Usage
 * @code
 *   DestructionSystem destruction;
 *   destruction.Initialize();
 *
 *   // Register a fracture pattern
 *   FracturePattern crate;
 *   crate.AddPiece(FracturePiece{"top", meshTop, {0,0.5f,0}, 2.0f});
 *   crate.AddPiece(FracturePiece{"side1", meshSide, {0.5f,0,0}, 1.5f});
 *   destruction.RegisterPattern("wooden_crate", crate);
 *
 *   // Attach to entity via ECS
 *   auto& destComp = world.AddComponent<DestructibleComponent>(entity);
 *   destComp.health = 50.0f;
 *   destComp.patternName = "wooden_crate";
 *
 *   // Apply damage:
 *   destruction.ApplyDamage(entity, 30.0f, hitPoint, hitDirection);
 * @endcode
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace Spark
{

    // =============================================================================
    // FracturePiece
    // =============================================================================

    /**
 * @brief A single debris piece created when an object is destroyed.
 */
    struct FracturePiece
    {
        std::string name;                       ///< Piece identifier
        std::string meshName;                   ///< Mesh asset name for this debris
        DirectX::XMFLOAT3 localOffset{0, 0, 0}; ///< Offset from parent center
        float mass = 1.0f;                      ///< Physics mass
        float lifetime = 10.0f;                 ///< Seconds before debris is cleaned up
        float scatterForce = 5.0f;              ///< Force applied at fracture
    };

    // =============================================================================
    // FracturePattern
    // =============================================================================

    /**
 * @brief Defines how an object breaks apart — a collection of debris pieces.
 */
    class FracturePattern
    {
      public:
        /** @brief Add a piece to the pattern. */
        void AddPiece(const FracturePiece& piece) { m_pieces.push_back(piece); }

        /** @brief Get all pieces. */
        const std::vector<FracturePiece>& GetPieces() const { return m_pieces; }

        /** @brief Set the destruction sound effect. */
        void SetDestructionSound(const std::string& sound) { m_destructionSound = sound; }
        const std::string& GetDestructionSound() const { return m_destructionSound; }

        /** @brief Set the particle effect name for destruction. */
        void SetParticleEffect(const std::string& effect) { m_particleEffect = effect; }
        const std::string& GetParticleEffect() const { return m_particleEffect; }

      private:
        std::vector<FracturePiece> m_pieces;
        std::string m_destructionSound;
        std::string m_particleEffect;
    };

    // =============================================================================
    // DestructibleComponent
    // =============================================================================

    /**
 * @brief ECS component for destructible entities.
 */
    struct DestructibleComponent
    {
        float health = 100.0f;             ///< Current health
        float maxHealth = 100.0f;          ///< Maximum health
        std::string patternName;           ///< Fracture pattern to use
        bool isDestroyed = false;          ///< Whether the object has been destroyed
        bool destructionProcessed = false; ///< Whether debris has been spawned

        // Damage resistance
        float damageThreshold = 0.0f;  ///< Minimum damage to register (ignore scratches)
        float damageMultiplier = 1.0f; ///< Multiplier for incoming damage

        // Partial damage states
        int damageStage = 0;     ///< Current visual damage stage (0 = pristine)
        int maxDamageStages = 3; ///< Number of damage stages before destruction

        /**
     * @brief Apply damage to the destructible.
     * @param amount Raw damage amount.
     * @return true if the object was destroyed by this damage.
     */
        bool ApplyDamage(float amount)
        {
            if (isDestroyed)
            {
                return false;
            }
            float effective = amount * damageMultiplier;
            if (effective < damageThreshold)
            {
                return false;
            }
            health -= effective;

            // Update damage stage
            float healthPercent = health / maxHealth;
            damageStage = static_cast<int>((1.0f - healthPercent) * maxDamageStages);

            if (health <= 0.0f)
            {
                health = 0.0f;
                isDestroyed = true;
                return true;
            }
            return false;
        }
    };

    // =============================================================================
    // DestructionEvent
    // =============================================================================

    /**
 * @brief Event data for a destruction occurrence.
 */
    struct DestructionEvent
    {
        uint32_t entityId = 0;                ///< Entity that was destroyed
        DirectX::XMFLOAT3 position{0, 0, 0};  ///< World position of destruction
        DirectX::XMFLOAT3 impactDir{0, 0, 0}; ///< Direction of the destroying hit
        float impactForce = 0.0f;             ///< Force of the destroying hit
        std::string patternName;              ///< Fracture pattern used
    };

    // =============================================================================
    // DestructionSystem
    // =============================================================================

    /**
 * @class DestructionSystem
 * @brief Manages destructible objects, fracturing, and debris.
 */
    class DestructionSystem
    {
      public:
        DestructionSystem();
        ~DestructionSystem() = default;

        /** @brief Initialize the destruction system. */
        void Initialize();

        /**
     * @brief Update the system — clean up expired debris.
     * @param deltaTime Frame time.
     */
        void Update(float deltaTime);

        // --- Pattern management ---

        /**
     * @brief Register a fracture pattern.
     * @param name    Pattern identifier.
     * @param pattern The fracture pattern.
     */
        void RegisterPattern(const std::string& name, const FracturePattern& pattern);

        /**
     * @brief Get a registered pattern by name.
     * @param name Pattern name.
     * @return Pointer to pattern, or nullptr if not found.
     */
        const FracturePattern* GetPattern(const std::string& name) const;

        // --- Damage application ---

        /**
     * @brief Apply damage to a destructible entity.
     * @param entityId   Entity with DestructibleComponent.
     * @param damage     Damage amount.
     * @param hitPoint   World-space hit position.
     * @param hitDir     Hit direction (for debris scattering).
     */
        void ApplyDamage(uint32_t entityId, float damage, const DirectX::XMFLOAT3& hitPoint,
                         const DirectX::XMFLOAT3& hitDir);

        /**
     * @brief Force-destroy an entity regardless of health.
     * @param entityId Entity with DestructibleComponent.
     * @param force    Explosion force for debris scattering.
     */
        void ForceDestroy(uint32_t entityId, float force = 10.0f);

        // --- Debris management ---

        /** @brief Get the current active debris count. */
        size_t GetActiveDebrisCount() const { return m_activeDebrisCount; }

        /** @brief Set maximum debris pieces allowed at once. */
        void SetMaxDebris(size_t max) { m_maxDebris = max; }

        /** @brief Set debris lifetime multiplier. */
        void SetDebrisLifetimeMultiplier(float mult) { m_debrisLifetimeMultiplier = mult; }

        // --- Callbacks ---

        /**
     * @brief Register callback for destruction events.
     * @param callback Called when an object is destroyed.
     */
        void OnDestruction(std::function<void(const DestructionEvent&)> callback);

        // --- Console integration ---

        /** @brief Get destruction system status (console integration). */
        std::string Console_GetStatus() const;

      private:
        struct DebrisInstance
        {
            uint32_t entityId = 0;
            float remainingLifetime = 10.0f;
        };

        std::unordered_map<std::string, FracturePattern> m_patterns;
        std::vector<DebrisInstance> m_debris;
        std::vector<std::function<void(const DestructionEvent&)>> m_destructionCallbacks;
        size_t m_activeDebrisCount = 0;
        size_t m_maxDebris = 500;
        float m_debrisLifetimeMultiplier = 1.0f;
        size_t m_totalDestructions = 0;
    };

} // namespace Spark
