/**
 * @file GravitySystem.h
 * @brief Enhanced gravity system with zones and per-object gravity
 * @author Spark Engine Team
 * @date 2025
 *
 * Extends the existing physics gravity with spatial gravity zones that can
 * create varied environmental gameplay: low-gravity areas, zero-G corridors,
 * reverse gravity rooms, and radial gravity around celestial objects.
 */

#pragma once
#include "Core/Platform.h"

#include "Enums/GameSystemEnums.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>
#include <string>
#include <functional>

using SparkEditor::GravityZoneType;

class Player;

namespace Spark
{

    /**
 * @brief Gravity zone definition
 *
 * Defines a spatial volume with custom gravity behavior.
 * When objects or players enter a zone, their gravity is overridden.
 */
    struct GravityZone
    {
        std::string name;
        GravityZoneType type = GravityZoneType::NORMAL;

        // Shape (axis-aligned box for simplicity)
        XMFLOAT3 center = {0, 0, 0};
        XMFLOAT3 halfExtents = {5, 5, 5}; ///< Half-size in each axis

        // Gravity parameters
        float gravityStrength = -20.0f;         ///< Strength (negative = down)
        XMFLOAT3 gravityDirection = {0, -1, 0}; ///< Direction for DIRECTIONAL type
        XMFLOAT3 radialCenter = {0, 0, 0};      ///< Center point for RADIAL type
        float transitionSpeed = 5.0f;           ///< How fast gravity transitions when entering

        // Visual/gameplay
        bool isActive = true;
        bool showBoundary = false; ///< Debug: render zone boundary
        int priority = 0;          ///< Higher priority overrides lower
        float damping = 0.0f;      ///< Extra air resistance inside zone

        /**
     * @brief Test if a point is inside this zone
     */
        bool Contains(const XMFLOAT3& point) const;

        /**
     * @brief Get the gravity vector for a point inside this zone
     */
        XMFLOAT3 GetGravityAt(const XMFLOAT3& point) const;
    };

    /**
 * @brief Per-object gravity state
 *
 * Tracks the current gravity affecting a specific object,
 * including smooth transitions between zones.
 */
    struct GravityState
    {
        XMFLOAT3 currentGravity = {0, -20.0f, 0}; ///< Current effective gravity
        XMFLOAT3 targetGravity = {0, -20.0f, 0};  ///< Target gravity (for transitions)
        int activeZoneIndex = -1;                 ///< Currently active zone (-1 = world default)
        float transitionProgress = 1.0f;          ///< 0 = start transition, 1 = complete
        bool inZone = false;
    };

    /**
 * @brief Gravity system manager
 *
 * Manages gravity zones and computes per-object gravity vectors.
 * The system supports smooth transitions between zones and
 * multiple overlapping zones with priority resolution.
 */
    class GravitySystem
    {
      public:
        GravitySystem();
        ~GravitySystem() = default;

        /**
     * @brief Initialize with world default gravity
     */
        bool Initialize(const XMFLOAT3& worldGravity = {0, -20.0f, 0});

        /**
     * @brief Update gravity transitions
     */
        void Update(float deltaTime);

        // === Zone Management ===

        /**
     * @brief Add a gravity zone to the world
     * @return Index of the added zone
     */
        int AddZone(const GravityZone& zone);

        /**
     * @brief Remove a gravity zone by index
     */
        void RemoveZone(int index);

        /**
     * @brief Remove a gravity zone by name
     */
        void RemoveZoneByName(const std::string& name);

        /**
     * @brief Get a zone by index
     */
        GravityZone* GetZone(int index);

        /**
     * @brief Get a zone by name
     */
        GravityZone* GetZoneByName(const std::string& name);

        /**
     * @brief Get all zones
     */
        const std::vector<GravityZone>& GetZones() const { return m_zones; }

        /**
     * @brief Clear all zones
     */
        void ClearZones();

        // === Gravity Queries ===

        /**
     * @brief Get the effective gravity at a world position
     *
     * Checks all active zones and returns the gravity vector for the
     * highest-priority zone containing the point, or world default.
     */
        XMFLOAT3 GetGravityAt(const XMFLOAT3& position) const;

        /**
     * @brief Get gravity state for an object (with transitions)
     *
     * Updates and returns the smooth-transitioning gravity for a tracked object.
     * Call each frame for each object that needs gravity zone support.
     */
        GravityState UpdateObjectGravity(const XMFLOAT3& position, GravityState& state, float dt) const;

        /**
     * @brief Check if a position is inside any gravity zone
     */
        bool IsInGravityZone(const XMFLOAT3& position) const;

        /**
     * @brief Get the zone index containing a position (highest priority)
     */
        int GetZoneAt(const XMFLOAT3& position) const;

        // === World Gravity ===

        /**
     * @brief Set the world default gravity
     */
        void SetWorldGravity(const XMFLOAT3& gravity) { m_worldGravity = gravity; }

        /**
     * @brief Get the world default gravity
     */
        XMFLOAT3 GetWorldGravity() const { return m_worldGravity; }

        // === Presets ===

        /**
     * @brief Create a low-gravity zone
     */
        int CreateLowGravityZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents,
                                 float strength = -5.0f);

        /**
     * @brief Create a zero-gravity zone
     */
        int CreateZeroGravityZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents);

        /**
     * @brief Create a reverse gravity zone
     */
        int CreateReverseGravityZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents,
                                     float strength = 15.0f);

        /**
     * @brief Create a high-gravity zone
     */
        int CreateHighGravityZone(const std::string& name, const XMFLOAT3& center, const XMFLOAT3& halfExtents,
                                  float strength = -40.0f);

        /**
     * @brief Create a radial gravity zone (pulls toward center)
     */
        int CreateRadialGravityZone(const std::string& name, const XMFLOAT3& center, float radius,
                                    float strength = -15.0f);

        // === Console Integration ===

        std::string Console_ListZones() const;
        bool Console_AddZone(const std::string& name, const std::string& type, float cx, float cy, float cz, float hx,
                             float hy, float hz, float strength);
        bool Console_RemoveZone(const std::string& name);
        void Console_SetWorldGravity(float x, float y, float z);

      private:
        std::vector<GravityZone> m_zones;
        XMFLOAT3 m_worldGravity = {0, -20.0f, 0};

        static constexpr int MAX_ZONES = 64;
    };

} // namespace Spark
