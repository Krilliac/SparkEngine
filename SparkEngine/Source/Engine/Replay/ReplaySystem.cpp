/**
 * @file ReplaySystem.cpp
 * @brief Implementation of the replay recording and playback system
 */

#include "ReplaySystem.h"
#include "../../Utils/Serializer.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace Spark
{

    ReplaySystem::ReplaySystem() = default;

    void ReplaySystem::StartRecording()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Starting replay recording");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isRecording = true;
        m_recordTimer = 0.0f;
        m_frameCounter = 0;
        m_replayData.frames.clear();
        m_replayData.events.clear();
        m_replayData.duration = 0.0f;
    }

    void ReplaySystem::StopRecording()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Stopping replay recording");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isRecording = false;
        if (!m_replayData.frames.empty())
        {
            m_replayData.duration = m_replayData.frames.back().timestamp;
        }
    }

    void ReplaySystem::RecordFrame(const std::vector<ReplayEntityState>& entities, float timestamp)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isRecording)
        {
            return;
        }

        ReplayFrame frame;
        frame.timestamp = timestamp;
        frame.frameNumber = m_frameCounter++;
        frame.entities = entities;
        m_replayData.frames.push_back(std::move(frame));
        m_replayData.duration = timestamp;
    }

    void ReplaySystem::RecordEvent(const ReplayEvent& event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isRecording)
        {
            return;
        }
        m_replayData.events.push_back(event);
    }

    void ReplaySystem::SetMetadata(const std::string& mapName, const std::string& gameMode)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_replayData.mapName = mapName;
        m_replayData.gameMode = gameMode;
    }

    void ReplaySystem::StartPlayback()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Starting replay playback");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_playbackState = PlaybackState::Playing;
        m_playbackTime = 0.0f;
        m_currentFrameIndex = 0;
    }

    void ReplaySystem::PausePlayback()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_playbackState = PlaybackState::Paused;
    }

    void ReplaySystem::ResumePlayback()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_playbackState == PlaybackState::Paused)
        {
            m_playbackState = PlaybackState::Playing;
        }
    }

    void ReplaySystem::StopPlayback()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Stopping replay playback");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_playbackState = PlaybackState::Stopped;
        m_playbackTime = 0.0f;
        m_currentFrameIndex = 0;
    }

    void ReplaySystem::UpdatePlayback(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_playbackState != PlaybackState::Playing)
        {
            return;
        }

        m_playbackTime += deltaTime * m_playbackSpeed;

        // Clamp to replay duration
        if (m_playbackTime >= m_replayData.duration)
        {
            m_playbackTime = m_replayData.duration;
            m_playbackState = PlaybackState::Stopped;
            return;
        }
        if (m_playbackTime < 0.0f)
        {
            m_playbackTime = 0.0f;
        }

        // Find the frame closest to the current playback time
        m_currentFrameIndex = FindFrameIndex(m_playbackTime);
    }

    void ReplaySystem::SeekTo(float timestamp)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_playbackTime = std::clamp(timestamp, 0.0f, m_replayData.duration);
        m_currentFrameIndex = FindFrameIndex(m_playbackTime);
    }

    const ReplayFrame* ReplaySystem::GetCurrentFrame() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_currentFrameIndex < m_replayData.frames.size())
        {
            return &m_replayData.frames[m_currentFrameIndex];
        }
        return nullptr;
    }

    std::vector<ReplayEvent> ReplaySystem::GetEventsNearTime(float windowSeconds) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<ReplayEvent> result;
        float minTime = m_playbackTime - windowSeconds;
        float maxTime = m_playbackTime + windowSeconds;

        for (const auto& event : m_replayData.events)
        {
            if (event.timestamp >= minTime && event.timestamp <= maxTime)
            {
                result.push_back(event);
            }
        }
        return result;
    }

    void ReplaySystem::StartKillCam(float rewindSeconds, uint32_t focusEntity)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_WARN_IF(Spark::LogCategory::Game, rewindSeconds <= 0.0f,
                      "StartKillCam called with non-positive rewindSeconds");
        m_killCamActive = true;
        m_killCamFocusEntity = focusEntity;
        m_killCamStartTime = m_replayData.duration - rewindSeconds;
        m_cameraMode = PlaybackCamera::KillCam;
        m_followEntity = focusEntity;

        // Seek to the kill cam start point
        SeekTo(m_killCamStartTime);
        m_playbackSpeed = 0.5f; // Slow-motion for kill cam
        m_playbackState = PlaybackState::Playing;
    }

    void ReplaySystem::StopKillCam()
    {
        m_killCamActive = false;
        m_playbackSpeed = 1.0f;
        m_playbackState = PlaybackState::Stopped;
    }

    bool ReplaySystem::SaveToFile(const std::string& filePath) const
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Saving replay to '%s'", filePath.c_str());
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        Spark::BinaryWriter writer;

        // Write header
        writer.Write<uint32_t>(0x52504C59); // "RPLY"
        writer.Write<uint32_t>(m_replayData.version);

        // Write metadata
        writer.WriteString(m_replayData.mapName);
        writer.WriteString(m_replayData.gameMode);
        writer.Write<float>(m_replayData.duration);

        // Write frames
        writer.Write<uint32_t>(static_cast<uint32_t>(m_replayData.frames.size()));
        for (const auto& frame : m_replayData.frames)
        {
            writer.Write<float>(frame.timestamp);
            writer.Write<uint32_t>(frame.frameNumber);
            writer.Write<uint32_t>(static_cast<uint32_t>(frame.entities.size()));
            for (const auto& entity : frame.entities)
            {
                writer.WriteBytes(&entity, sizeof(ReplayEntityState));
            }
        }

        // Write events
        writer.Write<uint32_t>(static_cast<uint32_t>(m_replayData.events.size()));
        for (const auto& event : m_replayData.events)
        {
            writer.Write<float>(event.timestamp);
            writer.WriteString(event.type);
            writer.Write<uint32_t>(event.sourceEntity);
            writer.Write<uint32_t>(event.targetEntity);
            writer.WriteBytes(&event.position, sizeof(DirectX::XMFLOAT3));
        }

        const auto& buffer = writer.GetBuffer();
        file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        return true;
    }

    bool ReplaySystem::LoadFromFile(const std::string& filePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Loading replay from '%s'", filePath.c_str());
        // Safety limits for deserialization to prevent OOM from malicious/corrupt files
        constexpr uint32_t kMaxFrameCount = 1'000'000;
        constexpr uint32_t kMaxEntityCount = 100'000;
        constexpr uint32_t kMaxEventCount = 1'000'000;

        std::lock_guard<std::mutex> lock(m_mutex);
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            return false;
        }

        auto fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));
        file.read(reinterpret_cast<char*>(fileData.data()), fileSize);
        if (!file)
        {
            return false;
        }

        Spark::BinaryReader reader(fileData);

        // Read and verify header
        uint32_t magic = reader.Read<uint32_t>();
        if (reader.HasError() || magic != 0x52504C59)
        {
            return false;
        }

        m_replayData.version = reader.Read<uint32_t>();

        // Read metadata
        m_replayData.mapName = reader.ReadString();
        m_replayData.gameMode = reader.ReadString();
        m_replayData.duration = reader.Read<float>();
        if (reader.HasError())
        {
            return false;
        }

        // Read frames with bounds checking
        uint32_t frameCount = reader.Read<uint32_t>();
        if (reader.HasError() || frameCount > kMaxFrameCount)
        {
            return false;
        }
        m_replayData.frames.resize(frameCount);

        for (auto& frame : m_replayData.frames)
        {
            frame.timestamp = reader.Read<float>();
            frame.frameNumber = reader.Read<uint32_t>();
            uint32_t entityCount = reader.Read<uint32_t>();
            if (reader.HasError() || entityCount > kMaxEntityCount)
            {
                return false;
            }
            frame.entities.resize(entityCount);
            for (auto& entity : frame.entities)
            {
                reader.ReadBytes(&entity, sizeof(ReplayEntityState));
            }
            if (reader.HasError())
            {
                return false;
            }
        }

        // Read events with bounds checking
        uint32_t eventCount = reader.Read<uint32_t>();
        if (reader.HasError() || eventCount > kMaxEventCount)
        {
            return false;
        }
        m_replayData.events.resize(eventCount);

        for (auto& event : m_replayData.events)
        {
            event.timestamp = reader.Read<float>();
            event.type = reader.ReadString();
            event.sourceEntity = reader.Read<uint32_t>();
            event.targetEntity = reader.Read<uint32_t>();
            reader.ReadBytes(&event.position, sizeof(DirectX::XMFLOAT3));
            if (reader.HasError())
            {
                return false;
            }
        }

        return true;
    }

    size_t ReplaySystem::FindFrameIndex(float timestamp) const
    {
        if (m_replayData.frames.empty())
        {
            return 0;
        }

        // Binary search for the nearest frame
        auto it = std::lower_bound(m_replayData.frames.begin(), m_replayData.frames.end(), timestamp,
                                   [](const ReplayFrame& frame, float time) { return frame.timestamp < time; });

        if (it == m_replayData.frames.end())
        {
            return m_replayData.frames.size() - 1;
        }
        return static_cast<size_t>(std::distance(m_replayData.frames.begin(), it));
    }

    std::string ReplaySystem::Console_GetStatus() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream oss;
        oss << "=== Replay System ===\n";
        oss << "Recording: " << (m_isRecording ? "YES" : "NO") << "\n";
        const char* stateNames[] = {"Stopped", "Playing", "Paused", "Rewinding", "FastForward"};
        oss << "Playback: " << stateNames[static_cast<int>(m_playbackState)] << "\n";
        oss << "Playback time: " << m_playbackTime << "s / " << m_replayData.duration << "s\n";
        oss << "Speed: " << m_playbackSpeed << "x\n";
        oss << "Frames recorded: " << m_replayData.frames.size() << "\n";
        oss << "Events recorded: " << m_replayData.events.size() << "\n";
        oss << "Map: " << m_replayData.mapName << "\n";
        oss << "Mode: " << m_replayData.gameMode << "\n";
        oss << "Kill cam: " << (m_killCamActive ? "ACTIVE" : "OFF") << "\n";
        return oss.str();
    }

} // namespace Spark
