/**
 * @file RacingTrackSystem.cpp
 * @brief Track layout, checkpoints, and surface zone management
 */

#include "RacingTrackSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace Racing
{

    bool RacingTrackSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;

        BuildDemoTracks();

        // Load the first track by default
        if (!m_tracks.empty())
            LoadDemoTrack(0);

        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing track system initialized with %zu tracks", m_tracks.size());
        console.LogInfo("[Racing Track] Track system initialized (" + std::to_string(m_tracks.size()) + " tracks)");
        return true;
    }

    void RacingTrackSystem::Update(float deltaTime)
    {
        (void)deltaTime;
        // Track is static; nothing to update per frame
    }

    void RacingTrackSystem::Shutdown()
    {
        m_tracks.clear();
        m_currentTrack = {};
        m_initialized = false;
    }

    void RacingTrackSystem::LoadDemoTrack(uint32_t index)
    {
        if (index < m_tracks.size())
        {
            m_currentTrack = m_tracks[index];
            auto& console = Spark::SimpleConsole::GetInstance();
            SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing track loaded: %s", m_currentTrack.name.c_str());
            console.LogInfo("[Racing Track] Loaded track: " + m_currentTrack.name + " (" +
                            std::to_string(m_currentTrack.waypoints.size()) + " waypoints, " +
                            std::to_string(m_currentTrack.checkpoints.size()) + " checkpoints)");
        }
    }

    SurfaceType RacingTrackSystem::GetSurfaceAt(float x, float z) const
    {
        if (m_currentTrack.waypoints.empty())
            return SurfaceType::Asphalt;

        // Measure against the centerline segment, not the nearest waypoint vertex: waypoints are tens of
        // meters apart, so a car on the centerline between two of them is still on the track.
        const TrackProjection projection = ProjectOntoTrack(x, z);
        const TrackWaypoint& from = m_currentTrack.waypoints[projection.segment];
        const TrackWaypoint& to = GetWaypoint(projection.segment + 1);
        const float width = from.width + (to.width - from.width) * projection.t;
        if (projection.lateralDistance > width)
            return SurfaceType::Grass; // Off-track

        return projection.t < 0.5f ? from.surface : to.surface;
    }

    int RacingTrackSystem::CheckCheckpoint(float x, float z) const
    {
        for (size_t i = 0; i < m_currentTrack.checkpoints.size(); ++i)
        {
            const auto& cp = m_currentTrack.checkpoints[i];
            float dx = x - cp.x;
            float dz = z - cp.z;
            float distSq = dx * dx + dz * dz;
            if (distSq < cp.radius * cp.radius)
                return static_cast<int>(i);
        }
        return -1;
    }

    int RacingTrackSystem::CheckHazard(float x, float z) const
    {
        for (size_t i = 0; i < m_currentTrack.hazards.size(); ++i)
        {
            const auto& h = m_currentTrack.hazards[i];
            float dx = x - h.x;
            float dz = z - h.z;
            float distSq = dx * dx + dz * dz;
            if (distSq < h.radius * h.radius)
                return static_cast<int>(i);
        }
        return -1;
    }

    uint32_t RacingTrackSystem::GetNearestWaypoint(float x, float z) const
    {
        uint32_t nearest = 0;
        float bestDistSq = std::numeric_limits<float>::max();

        for (uint32_t i = 0; i < static_cast<uint32_t>(m_currentTrack.waypoints.size()); ++i)
        {
            const auto& wp = m_currentTrack.waypoints[i];
            float dx = x - wp.x;
            float dz = z - wp.z;
            float distSq = dx * dx + dz * dz;
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                nearest = i;
            }
        }
        return nearest;
    }

    const TrackWaypoint& RacingTrackSystem::GetWaypoint(uint32_t index) const
    {
        static const TrackWaypoint empty{};
        if (m_currentTrack.waypoints.empty())
            return empty;
        return m_currentTrack.waypoints[index % m_currentTrack.waypoints.size()];
    }

    uint32_t RacingTrackSystem::GetSegmentCount() const
    {
        const auto waypointCount = static_cast<uint32_t>(m_currentTrack.waypoints.size());
        if (waypointCount < 2)
            return 0;
        return m_currentTrack.layout == TrackLayout::PointToPoint ? waypointCount - 1 : waypointCount;
    }

    TrackProjection RacingTrackSystem::ProjectOntoTrack(float x, float z, std::optional<float> heading) const
    {
        TrackProjection best{};
        const uint32_t segmentCount = GetSegmentCount();
        if (segmentCount == 0)
        {
            if (!m_currentTrack.waypoints.empty())
                best.lateralDistance = std::hypot(x - m_currentTrack.waypoints[0].x, z - m_currentTrack.waypoints[0].z);
            return best;
        }

        // Where segments overlap (a figure-8 crossing), prefer the one running the way the car is facing.
        constexpr float kHeadingTieBreakMeters = 10.0f;
        const float headingX = heading ? std::sin(*heading) : 0.0f;
        const float headingZ = heading ? std::cos(*heading) : 0.0f;
        float bestScore = std::numeric_limits<float>::max();

        for (uint32_t segment = 0; segment < segmentCount; ++segment)
        {
            const TrackWaypoint& from = m_currentTrack.waypoints[segment];
            const TrackWaypoint& to = GetWaypoint(segment + 1);
            const float segX = to.x - from.x;
            const float segZ = to.z - from.z;
            const float lengthSq = segX * segX + segZ * segZ;
            if (lengthSq <= 0.0f)
                continue;

            const float t = std::clamp(((x - from.x) * segX + (z - from.z) * segZ) / lengthSq, 0.0f, 1.0f);
            const float lateral = std::hypot(x - (from.x + segX * t), z - (from.z + segZ * t));
            const float alignment = heading ? (segX * headingX + segZ * headingZ) / std::sqrt(lengthSq) : 1.0f;
            const float score = lateral + (1.0f - alignment) * kHeadingTieBreakMeters;
            if (score < bestScore)
            {
                bestScore = score;
                best.segment = segment;
                best.t = t;
                best.lateralDistance = lateral;
            }
        }
        return best;
    }

    void RacingTrackSystem::GetPointAhead(const TrackProjection& from, float distance, float& outX, float& outZ) const
    {
        const uint32_t segmentCount = GetSegmentCount();
        if (segmentCount == 0)
        {
            const TrackWaypoint& only = GetWaypoint(0);
            outX = only.x;
            outZ = only.z;
            return;
        }

        uint32_t segment = std::min(from.segment, segmentCount - 1);
        float t = std::clamp(from.t, 0.0f, 1.0f);
        float remaining = std::max(distance, 0.0f);

        // Walk at most one full lap so degenerate zero-length tracks cannot loop forever.
        for (uint32_t walked = 0; walked <= segmentCount; ++walked)
        {
            const TrackWaypoint& a = m_currentTrack.waypoints[segment];
            const TrackWaypoint& b = GetWaypoint(segment + 1);
            const float length = std::hypot(b.x - a.x, b.z - a.z);
            const float left = length * (1.0f - t);
            if (remaining <= left && length > 0.0f)
            {
                const float endT = t + remaining / length;
                outX = a.x + (b.x - a.x) * endT;
                outZ = a.z + (b.z - a.z) * endT;
                return;
            }

            remaining -= left;
            t = 0.0f;
            if (segment + 1 >= segmentCount && m_currentTrack.layout == TrackLayout::PointToPoint)
                break; // Hold the target on the finish rather than wrapping back to the start.
            segment = (segment + 1) % segmentCount;
        }

        const TrackWaypoint& end = GetWaypoint(segment + 1);
        outX = end.x;
        outZ = end.z;
    }

    std::string RacingTrackSystem::GetTrackListString() const
    {
        std::string result = "Tracks (" + std::to_string(m_tracks.size()) + "):\n";
        for (const auto& t : m_tracks)
        {
            result += "  [" + std::to_string(t.id) + "] " + t.name;
            result += " | Waypoints: " + std::to_string(t.waypoints.size());
            result += " | Checkpoints: " + std::to_string(t.checkpoints.size());
            result += " | Hazards: " + std::to_string(t.hazards.size());
            result += "\n";
        }
        if (!m_currentTrack.name.empty())
            result += "Active: " + m_currentTrack.name + "\n";
        return result;
    }

    void RacingTrackSystem::BuildDemoTracks()
    {
        m_tracks.push_back(CreateCircuitTrack());
        m_tracks.push_back(CreatePointToPointTrack());
        m_tracks.push_back(CreateFigure8Track());
    }

    TrackData RacingTrackSystem::CreateCircuitTrack() const
    {
        TrackData track{};
        track.id = 1;
        track.name = "Sunset Circuit";
        track.layout = TrackLayout::Circuit;
        track.totalLaps = 3;

        // Oval-ish circuit defined by waypoints around an ellipse
        constexpr int numPoints = 24;
        constexpr float radiusX = 200.0f;
        constexpr float radiusZ = 120.0f;
        constexpr float pi = 3.14159265f;

        for (int i = 0; i < numPoints; ++i)
        {
            float angle = (static_cast<float>(i) / numPoints) * 2.0f * pi;
            TrackWaypoint wp{};
            wp.x = std::cos(angle) * radiusX;
            wp.z = std::sin(angle) * radiusZ;
            wp.y = 0.0f;
            wp.width = 14.0f;
            wp.surface = SurfaceType::Asphalt;
            track.waypoints.push_back(wp);
        }

        // Place checkpoints at quarter intervals
        for (int i = 0; i < 4; ++i)
        {
            int wpIdx = (i * numPoints) / 4;
            Checkpoint cp{};
            cp.index = static_cast<uint32_t>(i);
            cp.x = track.waypoints[wpIdx].x;
            cp.z = track.waypoints[wpIdx].z;
            cp.radius = 20.0f;
            cp.isFinishLine = (i == 0);
            track.checkpoints.push_back(cp);
        }

        // Add a couple of hazards
        TrackHazard oil{};
        oil.type = TrackHazard::Type::OilSlick;
        oil.x = track.waypoints[6].x + 3.0f;
        oil.z = track.waypoints[6].z;
        oil.radius = 4.0f;
        track.hazards.push_back(oil);

        TrackHazard boost{};
        boost.type = TrackHazard::Type::SpeedBoost;
        boost.x = track.waypoints[18].x;
        boost.z = track.waypoints[18].z;
        boost.radius = 3.0f;
        track.hazards.push_back(boost);

        return track;
    }

    TrackData RacingTrackSystem::CreatePointToPointTrack() const
    {
        TrackData track{};
        track.id = 2;
        track.name = "Mountain Pass";
        track.layout = TrackLayout::PointToPoint;
        track.totalLaps = 1;

        // Winding path with elevation
        const float segments[][3] = {
            {0.0f, 0.0f, 0.0f},      {50.0f, 2.0f, 30.0f},    {120.0f, 5.0f, 80.0f},   {180.0f, 10.0f, 60.0f},
            {220.0f, 15.0f, 100.0f}, {280.0f, 20.0f, 150.0f}, {320.0f, 18.0f, 200.0f}, {350.0f, 12.0f, 260.0f},
            {300.0f, 8.0f, 310.0f},  {250.0f, 5.0f, 350.0f},  {200.0f, 2.0f, 380.0f},  {150.0f, 0.0f, 400.0f},
        };

        for (const auto& seg : segments)
        {
            TrackWaypoint wp{};
            wp.x = seg[0];
            wp.y = seg[1];
            wp.z = seg[2];
            wp.width = 12.0f;
            wp.surface = SurfaceType::Asphalt;
            track.waypoints.push_back(wp);
        }

        // Dirt section in the middle
        track.waypoints[4].surface = SurfaceType::Dirt;
        track.waypoints[5].surface = SurfaceType::Dirt;
        track.waypoints[6].surface = SurfaceType::Gravel;

        // Checkpoints at the start, the midpoint, and the finish at the final waypoint
        const auto lastWaypoint = static_cast<uint32_t>(track.waypoints.size() - 1);
        for (uint32_t i = 0; i < 3; ++i)
        {
            const uint32_t wpIdx = (i * lastWaypoint) / 2;
            Checkpoint cp{};
            cp.index = i;
            cp.x = track.waypoints[wpIdx].x;
            cp.z = track.waypoints[wpIdx].z;
            cp.radius = 18.0f;
            cp.isFinishLine = (i == 2);
            track.checkpoints.push_back(cp);
        }

        return track;
    }

    TrackData RacingTrackSystem::CreateFigure8Track() const
    {
        TrackData track{};
        track.id = 3;
        track.name = "Crossover Arena";
        track.layout = TrackLayout::Figure8;
        track.totalLaps = 5;

        // Lemniscate of Gerono: one continuous line that crosses itself at the origin, so the lap runs
        // start/finish -> left lobe -> back through the crossing -> right lobe -> start/finish.
        constexpr int numPoints = 32;
        constexpr float halfLength = 160.0f;
        constexpr float pi = 3.14159265f;

        for (int i = 0; i < numPoints; ++i)
        {
            const float t = (static_cast<float>(i) / numPoints) * 2.0f * pi;
            TrackWaypoint wp{};
            wp.x = -halfLength * std::sin(t);
            wp.z = halfLength * std::sin(t) * std::cos(t);
            wp.width = 12.0f;
            wp.surface = SurfaceType::Asphalt;
            track.waypoints.push_back(wp);
        }

        // Checkpoints at the crossing (start/finish) and the far end of each lobe, in driving order
        Checkpoint cpCenter{};
        cpCenter.index = 0;
        cpCenter.x = 0.0f;
        cpCenter.z = 0.0f;
        cpCenter.radius = 15.0f;
        cpCenter.isFinishLine = true;
        track.checkpoints.push_back(cpCenter);

        Checkpoint cpLeft{};
        cpLeft.index = 1;
        cpLeft.x = -halfLength;
        cpLeft.z = 0.0f;
        cpLeft.radius = 15.0f;
        track.checkpoints.push_back(cpLeft);

        Checkpoint cpRight{};
        cpRight.index = 2;
        cpRight.x = halfLength;
        cpRight.z = 0.0f;
        cpRight.radius = 15.0f;
        track.checkpoints.push_back(cpRight);

        // Barrier at the crossing point
        TrackHazard barrier{};
        barrier.type = TrackHazard::Type::Barrier;
        barrier.x = 5.0f;
        barrier.z = 5.0f;
        barrier.radius = 2.0f;
        track.hazards.push_back(barrier);

        return track;
    }

    void RacingTrackSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Racing Track"))
            return;

        ImGui::Text("Available Tracks: %zu", m_tracks.size());
        ImGui::Text("Current: %s", m_currentTrack.name.c_str());
        ImGui::Text("Layout: %s", m_currentTrack.layout == TrackLayout::Circuit        ? "Circuit"
                                  : m_currentTrack.layout == TrackLayout::PointToPoint ? "Point-to-Point"
                                                                                       : "Figure-8");
        ImGui::Text("Waypoints: %zu", m_currentTrack.waypoints.size());
        ImGui::Text("Checkpoints: %zu", m_currentTrack.checkpoints.size());
        ImGui::Text("Hazards: %zu", m_currentTrack.hazards.size());
        ImGui::Text("Laps: %u", m_currentTrack.totalLaps);

        ImGui::Separator();

        // Track selector
        for (size_t i = 0; i < m_tracks.size(); ++i)
        {
            if (ImGui::Button(m_tracks[i].name.c_str()))
                LoadDemoTrack(static_cast<uint32_t>(i));
            if (i < m_tracks.size() - 1)
                ImGui::SameLine();
        }
#endif
    }

} // namespace Racing
