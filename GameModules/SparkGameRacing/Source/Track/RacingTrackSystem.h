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
#include <string>
#include <vector>

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
     * @brief Manages track definitions, checkpoint collision, and surface queries
     *
     * Provides the track spline for AI path-following, validates checkpoint
     * passage for lap counting, and reports the surface type under any
     * world-space position.
     */
    class RacingTrackSystem
    {
      public:
        RacingTrackSystem() = default;
        ~RacingTrackSystem() = default;

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

        const TrackData& GetCurrentTrack() const { return m_currentTrack; }
        size_t GetTrackCount() const { return m_tracks.size(); }
        size_t GetCheckpointCount() const { return m_currentTrack.checkpoints.size(); }

        std::string GetTrackListString() const;

      private:
        void BuildDemoTracks();
        TrackData CreateCircuitTrack() const;
        TrackData CreatePointToPointTrack() const;
        TrackData CreateFigure8Track() const;

        Spark::IEngineContext* m_context{nullptr};
        std::vector<TrackData> m_tracks;
        TrackData m_currentTrack;
        bool m_initialized{false};
    };

} // namespace Racing
