/**
 * @file RacingTrackSystem.h
 * @brief Track layout, checkpoints, and surface zones for the racing showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines tracks as a sequence of waypoints forming a spline, with
 * checkpoint gates for lap validation, surface zone transitions,
 * and hazard placement.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RacingEnums.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class PhysicsBody;
class PhysicsSystem;

namespace Racing
{

    /**
     * @brief Track layout categories
     */
    enum class TrackLayout : uint8_t
    {
        Circuit = 0,      ///< Closed loop
        PointToPoint = 1, ///< Start and finish at different locations
        Figure8 = 2,      ///< Crossing track with overpass/underpass
        Count = 3
    };

    /**
     * @brief A single waypoint on the track spline
     */
    struct TrackWaypoint
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float width = 12.0f; ///< Track width at this point in meters
        SurfaceType surface = SurfaceType::Asphalt;
    };

    /**
     * @brief A checkpoint gate that vehicles must pass through
     */
    struct Checkpoint
    {
        uint32_t index = 0; ///< Sequential checkpoint number
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float radius = 15.0f;      ///< Trigger radius in meters
        bool isFinishLine = false; ///< True for start/finish line
    };

    /**
     * @brief A track hazard (oil slick, barrier, jump ramp, etc.)
     */
    struct TrackHazard
    {
        enum class Type : uint8_t
        {
            OilSlick = 0,  ///< Reduces grip dramatically
            Barrier = 1,   ///< Collision obstacle — damages vehicle
            JumpRamp = 2,  ///< Launches vehicle airborne
            SpeedBoost = 3 ///< Temporary speed increase
        };

        Type type = Type::OilSlick;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float radius = 3.0f;
    };

    /**
     * @brief Complete track definition
     */
    struct TrackData
    {
        uint32_t id = 0;
        std::string name;
        TrackLayout layout = TrackLayout::Circuit;
        uint32_t totalLaps = 3;
        std::vector<TrackWaypoint> waypoints;
        std::vector<Checkpoint> checkpoints;
        std::vector<TrackHazard> hazards;
    };

    /**
     * @brief Where a world position lies relative to the track centerline
     */
    struct TrackProjection
    {
        uint32_t segment = 0;         ///< Centerline segment from waypoint `segment` to the next waypoint
        float t = 0.0f;               ///< 0-1 position along that segment
        float lateralDistance = 0.0f; ///< Distance from the centerline in meters
    };

    /**
     * @brief Manages track definitions, checkpoint collision, and surface queries
     *
     * Provides the track spline for AI path-following, validates checkpoint
     * passage for lap counting, and reports the surface type under any
     * world-space position.
     *
     * When the engine context has a live Jolt world, every loaded track also
     * gets static colliders in the shared PhysicsSystem: a road mesh per surface
     * type (friction = RacingVehicleSystem::GetSurfaceGrip) over a run-off
     * ground slab, so the Jolt vehicles drive on the authored centerline,
     * width, and elevation. Game thread only; the colliders are rebuilt on
     * every LoadDemoTrack() and removed in Shutdown().
     */
    class RacingTrackSystem
    {
      public:
        RacingTrackSystem() = default;
        ~RacingTrackSystem(); ///< Removes any track colliders still in the physics world (which must outlive this)

        RacingTrackSystem(const RacingTrackSystem&) = delete;
        RacingTrackSystem& operator=(const RacingTrackSystem&) = delete;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();

        void RenderDebugUI();

        /// Load a built-in demo track by index
        void LoadDemoTrack(uint32_t index);

        /// Query which surface type is under a world position
        SurfaceType GetSurfaceAt(float x, float z) const;

        /// Check if a position triggers a checkpoint, returns checkpoint index or -1
        int CheckCheckpoint(float x, float z) const;

        /// Check if a position triggers a hazard, returns hazard index or -1
        int CheckHazard(float x, float z) const;

        /// Get the nearest waypoint index for AI path-following
        uint32_t GetNearestWaypoint(float x, float z) const;

        /// Get a waypoint by index (wraps for circuits)
        const TrackWaypoint& GetWaypoint(uint32_t index) const;

        /// Project a position onto the centerline; a heading breaks ties where the track crosses itself.
        TrackProjection ProjectOntoTrack(float x, float z, std::optional<float> heading = std::nullopt) const;

        /// Centerline height (road surface level) at a projection.
        float GetCenterlineHeight(const TrackProjection& projection) const;

        /// Driving heading (yaw, 0 = +Z) of the centerline segment under a projection.
        float GetCenterlineHeading(const TrackProjection& projection) const;

        /// Point `distance` meters further along the centerline (clamped at a point-to-point finish).
        void GetPointAhead(const TrackProjection& from, float distance, float& outX, float& outZ) const;

        const TrackData& GetCurrentTrack() const { return m_currentTrack; }
        size_t GetTrackCount() const { return m_tracks.size(); }
        size_t GetCheckpointCount() const { return m_currentTrack.checkpoints.size(); }

        /// Static Jolt bodies (road surfaces + run-off ground) built for the current track.
        size_t GetColliderCount() const { return m_colliders.size(); }

        std::string GetTrackListString() const;

      private:
        uint32_t GetSegmentCount() const;
        void BuildDemoTracks();
        TrackData CreateCircuitTrack() const;
        TrackData CreatePointToPointTrack() const;
        TrackData CreateFigure8Track() const;

        /// Dress the current track with the Blender circuit kit (Assets/Models/Racing/Kit) when a world exists
        void PlaceTrackKit();
        void RemoveTrackKit();

        /// Build the current track's road and run-off colliders in the shared Jolt world (no-op without one)
        void BuildTrackColliders();
        void RemoveTrackColliders();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<TrackData> m_tracks;
        TrackData m_currentTrack;
        std::vector<uint32_t> m_kitEntities; ///< Trackside kit props (MeshRenderer entities) owned by this system
        std::vector<std::shared_ptr<PhysicsBody>> m_colliders; ///< Static track bodies owned by this system
        PhysicsSystem* m_colliderPhysics{nullptr};             ///< World the colliders were built in; non-owning
        bool m_initialized{false};
    };

} // namespace Racing
