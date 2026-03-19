#pragma once

/**
 * @file VisibilityComponents.h
 * @brief Per-entity visibility bitmask and camera draw mask components
 *
 * Provides a bitmask-based visibility system for filtering which entities
 * each camera can see. Entities have a VisibilityMaskComponent and cameras
 * have a CameraDrawMaskComponent; an entity is visible to a camera only
 * when their masks overlap.
 */

#include <cstdint>

namespace Spark
{

    /// @brief Default visibility masks for common rendering layers
    namespace VisibilityLayer
    {
        constexpr uint32_t Default = 1 << 0;     ///< Standard geometry
        constexpr uint32_t Shadow = 1 << 1;      ///< Shadow casters
        constexpr uint32_t Reflection = 1 << 2;  ///< Reflection geometry
        constexpr uint32_t UI = 1 << 3;          ///< UI elements
        constexpr uint32_t Debug = 1 << 4;       ///< Debug visualization
        constexpr uint32_t Transparent = 1 << 5; ///< Transparent objects
        constexpr uint32_t Decal = 1 << 6;       ///< Decal receivers
        constexpr uint32_t All = 0xFFFFFFFF;
    } // namespace VisibilityLayer

    /// @brief Controls which cameras can see this entity
    struct VisibilityMaskComponent
    {
        uint32_t mask = VisibilityLayer::All;

        /// @brief Check whether this entity is visible to a camera with the given mask
        bool IsVisibleTo(uint32_t cameraMask) const { return (mask & cameraMask) != 0; }

        /// @brief Add a layer to this entity's visibility
        void SetLayer(uint32_t layer) { mask |= layer; }

        /// @brief Remove a layer from this entity's visibility
        void ClearLayer(uint32_t layer) { mask &= ~layer; }

        /// @brief Set the mask to exactly one layer (clears all others)
        void SetExclusive(uint32_t layer) { mask = layer; }
    };

    /// @brief Camera draw mask — filters which entities this camera renders
    struct CameraDrawMaskComponent
    {
        uint32_t drawMask = VisibilityLayer::All;

        /// @brief Check whether this camera can see an entity with the given mask
        bool CanSee(uint32_t entityMask) const { return (drawMask & entityMask) != 0; }

        /// @brief Enable rendering of entities on the given layer
        void EnableLayer(uint32_t layer) { drawMask |= layer; }

        /// @brief Disable rendering of entities on the given layer
        void DisableLayer(uint32_t layer) { drawMask &= ~layer; }
    };

} // namespace Spark
