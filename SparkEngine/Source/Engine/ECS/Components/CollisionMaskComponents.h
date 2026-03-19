#pragma once

/**
 * @file CollisionMaskComponents.h
 * @brief Lightweight game-logic collision masks separate from Bullet physics
 *
 * Provides directional FROM/INTO collision filtering.  An entity's FROM mask
 * describes what categories it checks collisions against; its INTO mask
 * describes what categories are allowed to check collisions against it.
 * This is intentionally separate from the physics engine so game logic
 * (triggers, damage, interactions) can use independent filtering.
 */

#include <cstdint>

namespace Spark
{

    /// @brief Collision layer bit constants for game-logic filtering
    namespace CollisionLayer
    {
        constexpr uint32_t Player = 1 << 0;       ///< Player-controlled entities
        constexpr uint32_t Enemy = 1 << 1;        ///< Enemy / AI-controlled entities
        constexpr uint32_t Projectile = 1 << 2;   ///< Bullets, rockets, etc.
        constexpr uint32_t Trigger = 1 << 3;      ///< Trigger volumes
        constexpr uint32_t Interactable = 1 << 4; ///< Interactable objects (doors, items)
        constexpr uint32_t Environment = 1 << 5;  ///< Static world geometry
        constexpr uint32_t All = 0xFFFFFFFF;
        constexpr uint32_t None = 0;
    } // namespace CollisionLayer

    /// @brief Directional collision mask component
    ///
    /// FROM mask: what categories this entity checks collisions against.
    /// INTO mask: what categories are allowed to check collisions against this entity.
    /// A->B collision requires (A.fromMask & B.intoMask) != 0.
    /// This is intentionally directional — A->B does NOT imply B->A.
    struct CollisionMaskComponent
    {
        uint32_t fromMask = CollisionLayer::All; ///< What I collide WITH
        uint32_t intoMask = CollisionLayer::All; ///< What can collide with ME

        /// @brief Check if this entity's FROM mask overlaps target's INTO mask
        bool CanCollideWith(const CollisionMaskComponent& target) const { return (fromMask & target.intoMask) != 0; }

        /// @brief Directional collision test: does @p from collide with @p into?
        static bool CheckCollision(const CollisionMaskComponent& from, const CollisionMaskComponent& into)
        {
            return (from.fromMask & into.intoMask) != 0;
        }

        /// @brief Set the FROM mask to exactly the given layers
        void SetFromLayers(uint32_t layers) { fromMask = layers; }

        /// @brief Set the INTO mask to exactly the given layers
        void SetIntoLayers(uint32_t layers) { intoMask = layers; }

        /// @brief Add layers to the FROM mask
        void AddFromLayer(uint32_t layer) { fromMask |= layer; }

        /// @brief Add layers to the INTO mask
        void AddIntoLayer(uint32_t layer) { intoMask |= layer; }

        /// @brief Remove layers from the FROM mask
        void RemoveFromLayer(uint32_t layer) { fromMask &= ~layer; }

        /// @brief Remove layers from the INTO mask
        void RemoveIntoLayer(uint32_t layer) { intoMask &= ~layer; }
    };

} // namespace Spark
