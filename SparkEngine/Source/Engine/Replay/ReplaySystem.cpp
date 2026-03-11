/**
 * @file ReplaySystem.cpp
 * @brief Implementation of the replay recording and playback system
 */

#include "ReplaySystem.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace Spark
{

    ReplaySystem::ReplaySystem() = default;

    void ReplaySystem::StartRecording()
    {
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
        m_replayData.mapName = mapName;
        m_replayData.gameMode = gameMode;
    }

    void ReplaySystem::StartPlayback()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_playbackState = PlaybackState::Playing;
        m_playbackTime = 0.0f;
        m_currentFrameIndex = 0;
    }

    void ReplaySystem::PausePlayback()
    {
        m_playbackState = PlaybackState::Paused;
    }

    void ReplaySystem::ResumePlayback()
    {
        if (m_playbackState == PlaybackState::Paused)
        {
            m_playbackState = PlaybackState::Playing;
        }
    }

    void ReplaySystem::StopPlayback()
    {
        m_playbackState = PlaybackState::Stopped;
        m_playbackTime = 0.0f;
        m_currentFrameIndex = 0;
    }

    void ReplaySystem::UpdatePlayback(float deltaTime)
    {
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
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        // Write header
        uint32_t magic = 0x52504C59; // "RPLY"
        file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        file.write(reinterpret_cast<const char*>(&m_replayData.version), sizeof(uint32_t));

        // Write metadata
        uint32_t mapLen = static_cast<uint32_t>(m_replayData.mapName.size());
        file.write(reinterpret_cast<const char*>(&mapLen), sizeof(mapLen));
        file.write(m_replayData.mapName.data(), mapLen);

        uint32_t modeLen = static_cast<uint32_t>(m_replayData.gameMode.size());
        file.write(reinterpret_cast<const char*>(&modeLen), sizeof(modeLen));
        file.write(m_replayData.gameMode.data(), modeLen);

        file.write(reinterpret_cast<const char*>(&m_replayData.duration), sizeof(float));

        // Write frame count and frames
        uint32_t frameCount = static_cast<uint32_t>(m_replayData.frames.size());
        file.write(reinterpret_cast<const char*>(&frameCount), sizeof(frameCount));

        for (const auto& frame : m_replayData.frames)
        {
            file.write(reinterpret_cast<const char*>(&frame.timestamp), sizeof(float));
            file.write(reinterpret_cast<const char*>(&frame.frameNumber), sizeof(uint32_t));
            uint32_t entityCount = static_cast<uint32_t>(frame.entities.size());
            file.write(reinterpret_cast<const char*>(&entityCount), sizeof(entityCount));
            for (const auto& entity : frame.entities)
            {
                file.write(reinterpret_cast<const char*>(&entity), sizeof(ReplayEntityState));
            }
        }

        // Write events
        uint32_t eventCount = static_cast<uint32_t>(m_replayData.events.size());
        file.write(reinterpret_cast<const char*>(&eventCount), sizeof(eventCount));
        for (const auto& event : m_replayData.events)
        {
            file.write(reinterpret_cast<const char*>(&event.timestamp), sizeof(float));
            uint32_t typeLen = static_cast<uint32_t>(event.type.size());
            file.write(reinterpret_cast<const char*>(&typeLen), sizeof(typeLen));
            file.write(event.type.data(), typeLen);
            file.write(reinterpret_cast<const char*>(&event.sourceEntity), sizeof(uint32_t));
            file.write(reinterpret_cast<const char*>(&event.targetEntity), sizeof(uint32_t));
            file.write(reinterpret_cast<const char*>(&event.position), sizeof(DirectX::XMFLOAT3));
        }

        return true;
    }

    bool ReplaySystem::LoadFromFile(const std::string& filePath)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        uint32_t magic = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (magic != 0x52504C59)
        {
            return false;
        }

        file.read(reinterpret_cast<char*>(&m_replayData.version), sizeof(uint32_t));

        // Read metadata
        uint32_t mapLen = 0;
        file.read(reinterpret_cast<char*>(&mapLen), sizeof(mapLen));
        m_replayData.mapName.resize(mapLen);
        file.read(m_replayData.mapName.data(), mapLen);

        uint32_t modeLen = 0;
        file.read(reinterpret_cast<char*>(&modeLen), sizeof(modeLen));
        m_replayData.gameMode.resize(modeLen);
        file.read(m_replayData.gameMode.data(), modeLen);

        file.read(reinterpret_cast<char*>(&m_replayData.duration), sizeof(float));

        // Read frames
        uint32_t frameCount = 0;
        file.read(reinterpret_cast<char*>(&frameCount), sizeof(frameCount));
        m_replayData.frames.resize(frameCount);

        for (auto& frame : m_replayData.frames)
        {
            file.read(reinterpret_cast<char*>(&frame.timestamp), sizeof(float));
            file.read(reinterpret_cast<char*>(&frame.frameNumber), sizeof(uint32_t));
            uint32_t entityCount = 0;
            file.read(reinterpret_cast<char*>(&entityCount), sizeof(entityCount));
            frame.entities.resize(entityCount);
            for (auto& entity : frame.entities)
            {
                file.read(reinterpret_cast<char*>(&entity), sizeof(ReplayEntityState));
            }
        }

        // Read events
        uint32_t eventCount = 0;
        file.read(reinterpret_cast<char*>(&eventCount), sizeof(eventCount));
        m_replayData.events.resize(eventCount);

        for (auto& event : m_replayData.events)
        {
            file.read(reinterpret_cast<char*>(&event.timestamp), sizeof(float));
            uint32_t typeLen = 0;
            file.read(reinterpret_cast<char*>(&typeLen), sizeof(typeLen));
            event.type.resize(typeLen);
            file.read(event.type.data(), typeLen);
            file.read(reinterpret_cast<char*>(&event.sourceEntity), sizeof(uint32_t));
            file.read(reinterpret_cast<char*>(&event.targetEntity), sizeof(uint32_t));
            file.read(reinterpret_cast<char*>(&event.position), sizeof(DirectX::XMFLOAT3));
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
