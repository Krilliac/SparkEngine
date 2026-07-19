/**
 * @file SeamlessAreaManagerTypes.h
 * @brief Area state, definition, and configuration types for predictive streaming
 * @author Spark Engine Team
 * @date 2026
 *
 * Shared types consumed by SeamlessAreaManager: area identifiers, load
 * states, static area definitions, runtime area state, streaming tuning
 * parameters, and state-change callback signatures.
 *
 * @see SeamlessAreaManager.h
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <functional>
#include <string>

using namespace DirectX;

namespace Spark::Streaming
{

    /// @brief Unique area identifier (matches AreaServer AreaID convention)
    using AreaID = uint32_t;
    constexpr AreaID INVALID_AREA_ID = 0;

    // ========================================================================
    // Area State
    // ========================================================================

    /**
     * @brief Current loading state of a world area
     */
    enum class AreaState : uint8_t
    {
        Unloaded, ///< Not in memory
        Loading,  ///< Async load in progress
        Loaded,   ///< Fully loaded and active
        Unloading ///< Async unload in progress
    };

    // ========================================================================
    // Area Definition
    // ========================================================================

    /**
     * @brief Static definition of a world area (bounds, scene path, priority)
     */
    struct AreaDefinition
    {
        AreaID areaId = INVALID_AREA_ID;
        std::string name;
        std::string scenePath; ///< Path to the area's scene/asset bundle

        XMFLOAT3 boundsMin{0, 0, 0}; ///< AABB minimum corner (world space)
        XMFLOAT3 boundsMax{0, 0, 0}; ///< AABB maximum corner (world space)

        int priority = 0; ///< Higher = more important (loaded first on tie)
    };

    /**
     * @brief Runtime state for a managed area
     */
    struct ManagedArea
    {
        AreaDefinition definition;
        AreaState state = AreaState::Unloaded;
        float distanceToPlayer = 0.0f;     ///< Current distance from player
        float predictedArrivalTime = 0.0f; ///< Estimated time until player enters
    };

    // ========================================================================
    // Streaming Configuration
    // ========================================================================

    /**
     * @brief Tuning parameters for the predictive streaming system
     */
    struct StreamingConfig
    {
        float loadRadius = 500.0f;       ///< Distance at which areas begin loading
        float unloadRadius = 800.0f;     ///< Distance at which loaded areas are unloaded
        float lookaheadTime = 3.0f;      ///< Seconds to predict ahead using velocity
        float updateInterval = 0.25f;    ///< Seconds between prediction recalculations
        uint32_t maxConcurrentLoads = 2; ///< Maximum areas loading simultaneously

        /**
         * @brief Directional preload bias [0..1].
         *
         * Areas lying along the movement / camera direction receive an
         * effective-distance reduction equal to `directionalBias * distance`
         * for sorting and radius tests. 0 disables the bias (isotropic sort),
         * 1 makes areas in front load as if they were zero distance away.
         * Inspired by RAGE / Decima predictive streaming.
         */
        float directionalBias = 0.4f;

        /** @brief Only areas with a forward dot product >= this threshold get the bias. */
        float directionalDotThreshold = 0.25f;
    };

    // ========================================================================
    // Callback Types
    // ========================================================================

    /**
     * @brief Called when an area finishes loading or begins unloading
     * @param areaId The area that changed state
     * @param newState The new state of the area
     */
    using AreaStateChangedCallback = std::function<void(AreaID areaId, AreaState newState)>;

} // namespace Spark::Streaming
