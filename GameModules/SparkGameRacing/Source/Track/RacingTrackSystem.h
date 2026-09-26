/**
 * @file RacingTrackSystem.h
 * @brief Track layout, checkpoints, and surface zones for the racing showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines tracks as a sequence of waypoints forming a spline, with
 * checkpoint gates for lap validation, surface zone transitions,
 * and hazard placement. Checkpoint gates and trackside barriers are
 * static Jolt bodies in the engine's shared PhysicsSystem.
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
     *
     * The gate is a Jolt sensor box across the road at (x, z), square to the
     * centerline and spanning the track width plus the barrier clearance on
     * both sides (RacingTrackSystem::kGateDepth deep along the road).
     */
    struct Checkpoint
    {
        uint32_t index = 0; ///< Sequential checkpoint number
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool isFinishLine = false; ///< True for start/finish line
    };

    /**
     * @brief One vehicle entering one checkpoint gate, reported by the gate's Jolt sensor
     */
    struct CheckpointCrossing
    {
        uint32_t vehicleId = 0;       ///< Entity ID of the chassis body (RacingVehicleSystem sets it to the vehicle ID)
        uint32_t checkpointIndex = 0; ///< Index into TrackData::checkpoints
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
     * gets static bodies in the shared PhysicsSystem, generated from the
     * authored centerline, width, and elevation:
     * - a road mesh per surface type (friction = RacingVehicleSystem::GetSurfaceGrip)
     *   over a run-off ground slab;
     * - barrier walls along the outside of every bend, kBarrierClearance off the
     *   road edge, where a car that runs wide leaves the road. The inside of a
     *   bend stays open run-off, and a barrier piece that would stand on any
     *   part of the road (a figure-8 crossing) is left out;
     * - one sensor gate per checkpoint. Gate entries arrive through the
     *   PhysicsSystem trigger callback during the physics step and queue up
     *   until the race flow drains them with TakeCheckpointCrossings(), so lap
     *   validation reads the same fixed-step timeline the cars move on.
     *
     * The track system installs the PhysicsSystem trigger callback while its
     * gates exist and clears it when they are removed; the Racing module is
     * the only trigger consumer in its process. Game thread only (the trigger
     * callback runs inside PhysicsSystem::StepFixed on the stepping thread,
     * which is the game thread). The bodies are rebuilt on every
     * LoadDemoTrack() and removed in Shutdown(); allocation happens only
     * there, plus the crossing queue's amortized growth.
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

        /// Solid static Jolt bodies (road surfaces, run-off ground, barriers) built for the current track.
        size_t GetColliderCount() const { return m_colliders.size(); }

        /// Barrier wall pieces among GetColliderCount().
        size_t GetBarrierCount() const { return m_barrierCount; }

        /// Checkpoint sensor gates built for the current track (one per checkpoint when a Jolt world exists).
        size_t GetCheckpointGateCount() const { return m_checkpointGates.size(); }

        /// Hand over the gate entries reported since the last call, in physics-step order, and clear the queue.
        std::vector<CheckpointCrossing> TakeCheckpointCrossings();

        /// Gap between the road edge and a barrier's inner face, in meters.
        static constexpr float kBarrierClearance = 2.0f;
        /// Gate thickness along the driving direction: a car at top speed moves ~1.2 m per 60 Hz tick.
        static constexpr float kGateDepth = 4.0f;

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

        /// Build the current track's road, run-off, barrier, and gate bodies in the shared Jolt world
        /// (no-op without one)
        void BuildTrackColliders();
        void BuildBarriers(PhysicsSystem& physics);
        void BuildCheckpointGates(PhysicsSystem& physics);
        void RemoveTrackColliders();

        /// PhysicsSystem trigger callback: queue a crossing when a chassis enters one of this track's gates.
        void OnTriggerContact(const PhysicsBody* first, const PhysicsBody* second, bool entered);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<TrackData> m_tracks;
        TrackData m_currentTrack;
        std::vector<uint32_t> m_kitEntities; ///< Trackside kit props (MeshRenderer entities) owned by this system
        std::vector<std::shared_ptr<PhysicsBody>> m_colliders; ///< Solid static track bodies owned by this system
        /// A checkpoint's sensor body and the checkpoint it reports.
        struct CheckpointGate
        {
            std::shared_ptr<PhysicsBody> body;
            uint32_t checkpointIndex = 0;
        };

        std::vector<CheckpointGate> m_checkpointGates;      ///< Sensor gates of the current track
        std::vector<CheckpointCrossing> m_pendingCrossings; ///< Gate entries not yet taken by the race
        size_t m_barrierCount{0};
        PhysicsSystem* m_colliderPhysics{nullptr}; ///< World the bodies were built in; non-owning
        bool m_initialized{false};
    };

} // namespace Racing
