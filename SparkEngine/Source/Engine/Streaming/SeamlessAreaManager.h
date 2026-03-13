/**
 * @file SeamlessAreaManager.h
 * @brief Seamless world area transitions without loading screens
 * @author Spark Engine Team
 * @date 2026
 *
 * Inspired by HeroEngine's Seamless World 2.0 technology, this system manages
 * seamless transitions between world areas (tiles). Adjacent areas share a
 * configurable border overlap region where entities from both areas coexist
 * and interact, enabling continuous gameplay across area boundaries.
 *
 * Key concepts:
 * - **WorldArea**: A self-contained chunk of the game world with its own local
 *   coordinate origin (integrates with WorldOriginSystem).
 * - **BorderRegion**: An overlap zone between two adjacent areas where entities
 *   from both areas are active.
 * - **Area transitions**: When a player crosses an area boundary, the engine
 *   seamlessly loads the new area and unloads distant ones, with no load screen.
 *
 * ## Usage
 * @code
 *   SeamlessAreaManager areaManager;
 *   areaManager.Initialize();
 *
 *   // Define areas
 *   WorldArea town = { .name = "Town", .scenePath = "Scenes/Town.scene", ... };
 *   WorldArea forest = { .name = "Forest", .scenePath = "Scenes/Forest.scene", ... };
 *   areaManager.RegisterArea(town);
 *   areaManager.RegisterArea(forest);
 *   areaManager.DefineBorder("Town", "Forest", 50.0f);
 *
 *   // In game loop:
 *   areaManager.Update(playerPosition, deltaTime);
 * @endcode
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Streaming
{

    // ============================================================================
    // Area State
    // ============================================================================

    enum class AreaState : uint8_t
    {
        Unloaded,  ///< Area is not loaded
        Loading,   ///< Area is being loaded in the background
        Active,    ///< Area is fully loaded and active
        Border,    ///< Area is in border-overlap mode (partially active)
        Unloading, ///< Area is being unloaded
        Failed     ///< Loading failed
    };

    // ============================================================================
    // World Area
    // ============================================================================

    /**
     * @brief A self-contained chunk of the game world
     */
    struct WorldArea
    {
        std::string name;                                     ///< Unique area name
        std::string scenePath;                                ///< Path to area's scene file
        DirectX::XMFLOAT3 worldPosition = {0.0f, 0.0f, 0.0f}; ///< Position in absolute world space
        DirectX::XMFLOAT3 worldSize = {1000, 100, 1000};      ///< Area dimensions
        DirectX::XMFLOAT3 localOrigin = {0.0f, 0.0f, 0.0f};   ///< Local coordinate origin offset

        // Streaming
        float loadDistance = 2000.0f;   ///< Distance at which area starts loading
        float unloadDistance = 3000.0f; ///< Distance at which area unloads
        int priority = 0;               ///< Loading priority
        bool alwaysLoaded = false;      ///< Never unload this area

        // Runtime state
        AreaState state = AreaState::Unloaded;
        float loadProgress = 0.0f;

        /**
         * @brief Check if a point is inside this area's bounds
         */
        bool ContainsPoint(const DirectX::XMFLOAT3& point) const;

        /**
         * @brief Get distance from a point to the area's center
         */
        float GetDistanceToCenter(const DirectX::XMFLOAT3& point) const;

        /**
         * @brief Get distance from a point to the area's nearest edge
         */
        float GetDistanceToEdge(const DirectX::XMFLOAT3& point) const;
    };

    // ============================================================================
    // Border Region
    // ============================================================================

    /**
     * @brief Overlap zone between two adjacent areas
     *
     * Entities within a border region are visible and interactive from both
     * areas. This enables seamless transitions where gameplay (combat, AI,
     * physics) can span area boundaries.
     */
    struct BorderRegion
    {
        std::string areaA;                                    ///< First area name
        std::string areaB;                                    ///< Second area name
        float overlapWidth = 50.0f;                           ///< Width of the overlap zone
        DirectX::XMFLOAT3 center = {0.0f, 0.0f, 0.0f};        ///< Center of the border region
        DirectX::XMFLOAT3 extents = {50.0f, 100.0f, 1000.0f}; ///< Half-size of the border volume

        /**
         * @brief Check if a position is within the border region
         */
        bool ContainsPoint(const DirectX::XMFLOAT3& point) const;
    };

    // ============================================================================
    // Area Transition Event
    // ============================================================================

    /**
     * @brief Describes an area transition event
     */
    struct AreaTransitionEvent
    {
        std::string fromArea;               ///< Area the player is leaving
        std::string toArea;                 ///< Area the player is entering
        DirectX::XMFLOAT3 crossingPosition; ///< Position where the crossing occurred
        bool isSeamless = true;             ///< Whether the transition was seamless
    };

    /// @brief Callback for area transition events
    using AreaTransitionCallback = std::function<void(const AreaTransitionEvent&)>;

    // ============================================================================
    // Seamless Area Manager
    // ============================================================================

    /**
     * @brief Manages seamless world area transitions
     *
     * Coordinates area loading/unloading based on player position, maintains
     * border overlap regions for seamless gameplay, and handles coordinate
     * translation between areas with different local origins.
     */
    class SeamlessAreaManager
    {
      public:
        SeamlessAreaManager();
        ~SeamlessAreaManager();

        // -- Lifecycle --

        bool Initialize();
        void Shutdown();

        // -- Area Management --

        /**
         * @brief Register a world area
         * @param area Area definition
         * @return true if area was registered
         */
        bool RegisterArea(const WorldArea& area);

        /**
         * @brief Remove a registered area
         * @param areaName Name of area to remove
         * @return true if area was found and removed
         */
        bool UnregisterArea(const std::string& areaName);

        /**
         * @brief Get a registered area by name
         * @return Pointer to area, or nullptr if not found
         */
        WorldArea* GetArea(const std::string& areaName);
        const WorldArea* GetArea(const std::string& areaName) const;

        /**
         * @brief Get the area containing a world position
         * @return Pointer to area, or nullptr if position is outside all areas
         */
        WorldArea* GetAreaAtPosition(const DirectX::XMFLOAT3& position);

        /**
         * @brief Get all registered areas
         */
        const std::vector<WorldArea>& GetAllAreas() const { return m_areas; }

        // -- Border Management --

        /**
         * @brief Define a border overlap region between two adjacent areas
         * @param areaA First area name
         * @param areaB Second area name
         * @param overlapWidth Width of the overlap zone in world units
         * @return true if border was created
         */
        bool DefineBorder(const std::string& areaA, const std::string& areaB, float overlapWidth = 50.0f);

        /**
         * @brief Get borders for a specific area
         * @param areaName Area to query
         * @return Vector of border regions involving this area
         */
        std::vector<const BorderRegion*> GetBordersForArea(const std::string& areaName) const;

        // -- Update --

        /**
         * @brief Update area streaming based on player position
         *
         * Loads areas within range, unloads distant ones, and manages
         * border overlap regions. Call once per frame.
         *
         * @param referencePos Player/camera position in local coordinates
         * @param deltaTime    Frame delta time
         */
        void Update(const DirectX::XMFLOAT3& referencePos, float deltaTime);

        // -- Coordinate Translation --

        /**
         * @brief Translate a position from one area's local space to another's
         * @param position  Position in source area's local space
         * @param fromArea  Source area name
         * @param toArea    Target area name
         * @return Position in target area's local space
         */
        DirectX::XMFLOAT3 TranslatePosition(const DirectX::XMFLOAT3& position, const std::string& fromArea,
                                            const std::string& toArea) const;

        // -- Events --

        /**
         * @brief Register a callback for area transition events
         */
        void RegisterTransitionCallback(AreaTransitionCallback callback);

        // -- Queries --

        /**
         * @brief Get the name of the area the player is currently in
         */
        const std::string& GetCurrentArea() const { return m_currentArea; }

        /**
         * @brief Check if the player is in a border region
         */
        bool IsInBorderRegion() const { return m_inBorderRegion; }

        /**
         * @brief Get active (loaded) area count
         */
        int GetActiveAreaCount() const;

        /**
         * @brief Console status string
         */
        std::string Console_GetStatus() const;

      private:
        void UpdateAreaStreaming(const DirectX::XMFLOAT3& referencePos);
        void UpdateBorderRegions(const DirectX::XMFLOAT3& referencePos);
        void LoadArea(const std::string& areaName);
        void UnloadArea(const std::string& areaName);
        void NotifyTransition(const AreaTransitionEvent& event);

        std::vector<WorldArea> m_areas;
        std::vector<BorderRegion> m_borders;
        std::string m_currentArea;
        bool m_inBorderRegion = false;
        bool m_initialized = false;

        std::vector<AreaTransitionCallback> m_transitionCallbacks;

        /// Scene ID tracking for areas loaded via SceneTransitionManager
        std::unordered_map<std::string, uint32_t> m_areaSceneIDs;

        /// Maximum concurrent loaded areas to prevent resource exhaustion
        static constexpr int kMaxConcurrentAreas = 16;
    };

} // namespace Spark::Streaming
