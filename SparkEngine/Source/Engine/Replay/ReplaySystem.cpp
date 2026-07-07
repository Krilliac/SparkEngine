/**
 * @file ReplaySystem.cpp
 * @brief Implementation of the replay recording and playback system
 */

#include "ReplaySystem.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Spark
{

    // ============================================================================
    // Construction
    // ============================================================================

    ReplaySystem& ReplaySystem::GetInstance()
    {
        static ReplaySystem instance;
        return instance;
    }

    ReplaySystem::ReplaySystem() = default;

    // ============================================================================
    // Recording
    // ============================================================================

    void ReplaySystem::SetRecordInterval(float seconds)
    {
        std::lock_guard lock(m_mutex);
        m_recordInterval = seconds;
    }

    void ReplaySystem::StartRecording()
    {
        std::lock_guard lock(m_mutex);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "ReplaySystem::StartRecording");
        m_data.frames.clear();
        m_data.events.clear();
        m_data.duration = 0.0f;
        m_frameCounter = 0;
        m_lastRecordTime = -1.0f;
        m_recording = true;
    }

    void ReplaySystem::StopRecording()
    {
        std::lock_guard lock(m_mutex);
        m_recording = false;
        if (!m_data.frames.empty())
            m_data.duration = m_data.frames.back().timestamp;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "ReplaySystem::StopRecording — %zu frames, %.1fs duration",
                       m_data.frames.size(), m_data.duration);
    }

    bool ReplaySystem::IsRecording() const
    {
        std::lock_guard lock(m_mutex);
        return m_recording;
    }

    void ReplaySystem::RecordFrame(const std::vector<ReplayEntityState>& entities, float timestamp)
    {
        std::lock_guard lock(m_mutex);
        if (!m_recording)
            return;

        // Throttle recording to the configured interval
        if (m_lastRecordTime >= 0.0f && (timestamp - m_lastRecordTime) < m_recordInterval)
            return;

        ReplayFrame frame;
        frame.timestamp = timestamp;
        frame.frameNumber = m_frameCounter++;
        frame.entities = entities;
        m_data.frames.push_back(std::move(frame));
        m_lastRecordTime = timestamp;
        m_data.duration = timestamp;
    }

    void ReplaySystem::RecordEvent(const ReplayEvent& event)
    {
        std::lock_guard lock(m_mutex);
        if (!m_recording)
            return;
        m_data.events.push_back(event);
    }

    void ReplaySystem::SetMetadata(const std::string& mapName, const std::string& gameMode)
    {
        std::lock_guard lock(m_mutex);
        m_data.mapName = mapName;
        m_data.gameMode = gameMode;
    }

    // ============================================================================
    // Playback
    // ============================================================================

    void ReplaySystem::StartPlayback()
    {
        std::lock_guard lock(m_mutex);
        m_playbackTime = 0.0f;
        m_currentFrameIndex = 0;
        m_playbackState = PlaybackState::Playing;
    }

    void ReplaySystem::PausePlayback()
    {
        std::lock_guard lock(m_mutex);
        if (m_playbackState == PlaybackState::Playing)
            m_playbackState = PlaybackState::Paused;
    }

    void ReplaySystem::ResumePlayback()
    {
        std::lock_guard lock(m_mutex);
        if (m_playbackState == PlaybackState::Paused)
            m_playbackState = PlaybackState::Playing;
    }

    void ReplaySystem::StopPlayback()
    {
        std::lock_guard lock(m_mutex);
        m_playbackState = PlaybackState::Stopped;
        m_playbackTime = 0.0f;
        m_currentFrameIndex = 0;
    }

    void ReplaySystem::UpdatePlayback(float deltaTime)
    {
        std::lock_guard lock(m_mutex);
        if (m_playbackState != PlaybackState::Playing)
            return;
        if (deltaTime <= 0.0f)
            return;

        m_playbackTime += deltaTime * m_playbackSpeed;

        // Clamp and stop at end
        if (m_playbackTime >= m_data.duration)
        {
            m_playbackTime = m_data.duration;
            m_playbackState = PlaybackState::Stopped;
        }
        else if (m_playbackTime < 0.0f)
        {
            m_playbackTime = 0.0f;
            m_playbackState = PlaybackState::Stopped;
        }

        // Update current frame index via binary search
        m_currentFrameIndex = FindFrameIndex(m_playbackTime);
    }

    void ReplaySystem::SeekTo(float timestamp)
    {
        std::lock_guard lock(m_mutex);
        m_playbackTime = std::clamp(timestamp, 0.0f, m_data.duration);
        m_currentFrameIndex = FindFrameIndex(m_playbackTime);
    }

    void ReplaySystem::SetPlaybackSpeed(float speed)
    {
        std::lock_guard lock(m_mutex);
        m_playbackSpeed = speed;
    }

    float ReplaySystem::GetPlaybackSpeed() const
    {
        std::lock_guard lock(m_mutex);
        return m_playbackSpeed;
    }

    PlaybackState ReplaySystem::GetPlaybackState() const
    {
        std::lock_guard lock(m_mutex);
        return m_playbackState;
    }

    float ReplaySystem::GetPlaybackTime() const
    {
        std::lock_guard lock(m_mutex);
        return m_playbackTime;
    }

    float ReplaySystem::GetDuration() const
    {
        std::lock_guard lock(m_mutex);
        return m_data.duration;
    }

    const ReplayFrame* ReplaySystem::GetCurrentFrame() const
    {
        std::lock_guard lock(m_mutex);
        if (m_data.frames.empty())
            return nullptr;
        if (m_currentFrameIndex >= m_data.frames.size())
            return &m_data.frames.back();
        return &m_data.frames[m_currentFrameIndex];
    }

    std::vector<ReplayEvent> ReplaySystem::GetEventsNearTime(float windowSeconds) const
    {
        std::lock_guard lock(m_mutex);
        std::vector<ReplayEvent> result;
        float lo = m_playbackTime - windowSeconds;
        float hi = m_playbackTime + windowSeconds;
        for (const auto& ev : m_data.events)
        {
            if (ev.timestamp >= lo && ev.timestamp <= hi)
                result.push_back(ev);
        }
        return result;
    }

    // ============================================================================
    // Kill Cam
    // ============================================================================

    void ReplaySystem::StartKillCam(float rewindSeconds, uint32_t focusEntity)
    {
        // Take the lock once and touch every shared member under it. SeekTo() and
        // StopPlayback() also lock m_mutex (which is non-recursive), so their bodies are
        // inlined here rather than called, to avoid self-deadlock.
        std::lock_guard lock(m_mutex);

        // Seek to the rewind point.
        float seekTime = m_data.duration - rewindSeconds;
        if (seekTime < 0.0f)
            seekTime = 0.0f;
        m_playbackTime = std::clamp(seekTime, 0.0f, m_data.duration);
        m_currentFrameIndex = FindFrameIndex(m_playbackTime);

        m_killCamActive = true;
        m_playbackSpeed = 0.5f;
        m_camera = PlaybackCamera::KillCam;
        m_followEntity = focusEntity;
        m_playbackState = PlaybackState::Playing;
    }

    void ReplaySystem::StopKillCam()
    {
        // Inline StopPlayback()'s body under a single lock (StopPlayback() would relock
        // the non-recursive m_mutex and deadlock).
        std::lock_guard lock(m_mutex);
        m_killCamActive = false;
        m_playbackSpeed = 1.0f;
        m_playbackState = PlaybackState::Stopped;
        m_playbackTime = 0.0f;
        m_currentFrameIndex = 0;
    }

    bool ReplaySystem::IsKillCamActive() const
    {
        std::lock_guard lock(m_mutex);
        return m_killCamActive;
    }

    // ============================================================================
    // Camera
    // ============================================================================

    void ReplaySystem::SetCamera(PlaybackCamera mode)
    {
        std::lock_guard lock(m_mutex);
        m_camera = mode;
    }

    PlaybackCamera ReplaySystem::GetCamera() const
    {
        std::lock_guard lock(m_mutex);
        return m_camera;
    }

    void ReplaySystem::SetFollowEntity(uint32_t entityId)
    {
        std::lock_guard lock(m_mutex);
        m_followEntity = entityId;
    }

    // ============================================================================
    // File I/O
    // ============================================================================

    namespace
    {

        void WriteString(std::ofstream& file, const std::string& str)
        {
            uint32_t len = static_cast<uint32_t>(str.size());
            file.write(reinterpret_cast<const char*>(&len), sizeof(len));
            if (len > 0)
                file.write(str.data(), len);
        }

        bool ReadString(std::ifstream& file, std::string& str, uint32_t maxLen = kMaxStringLength)
        {
            uint32_t len = 0;
            file.read(reinterpret_cast<char*>(&len), sizeof(len));
            if (!file || len > maxLen)
                return false;
            str.resize(len);
            if (len > 0)
                file.read(str.data(), len);
            return file.good() || file.eof();
        }

    } // anonymous namespace

    bool ReplaySystem::SaveToFile(const std::string& filePath) const
    {
        std::lock_guard lock(m_mutex);

        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "ReplaySystem::SaveToFile: cannot open '%s' for writing (errno=%d)", filePath.c_str(),
                            errno);
            return false;
        }

        // Header
        uint32_t magic = kReplayMagic;
        file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        file.write(reinterpret_cast<const char*>(&m_data.version), sizeof(m_data.version));

        // Metadata
        WriteString(file, m_data.mapName);
        WriteString(file, m_data.gameMode);
        file.write(reinterpret_cast<const char*>(&m_data.duration), sizeof(m_data.duration));

        // Frames
        uint32_t frameCount = static_cast<uint32_t>(m_data.frames.size());
        file.write(reinterpret_cast<const char*>(&frameCount), sizeof(frameCount));
        for (const auto& frame : m_data.frames)
        {
            file.write(reinterpret_cast<const char*>(&frame.timestamp), sizeof(frame.timestamp));
            file.write(reinterpret_cast<const char*>(&frame.frameNumber), sizeof(frame.frameNumber));
            uint32_t entityCount = static_cast<uint32_t>(frame.entities.size());
            file.write(reinterpret_cast<const char*>(&entityCount), sizeof(entityCount));
            for (const auto& entity : frame.entities)
            {
                file.write(reinterpret_cast<const char*>(&entity.entityId), sizeof(entity.entityId));
                file.write(reinterpret_cast<const char*>(&entity.position), sizeof(entity.position));
                file.write(reinterpret_cast<const char*>(&entity.rotation), sizeof(entity.rotation));
                file.write(reinterpret_cast<const char*>(&entity.velocity), sizeof(entity.velocity));
                file.write(reinterpret_cast<const char*>(&entity.health), sizeof(entity.health));
                file.write(reinterpret_cast<const char*>(&entity.animationState), sizeof(entity.animationState));
                file.write(reinterpret_cast<const char*>(&entity.flags), sizeof(entity.flags));
            }
        }

        // Events
        uint32_t eventCount = static_cast<uint32_t>(m_data.events.size());
        file.write(reinterpret_cast<const char*>(&eventCount), sizeof(eventCount));
        for (const auto& event : m_data.events)
        {
            file.write(reinterpret_cast<const char*>(&event.timestamp), sizeof(event.timestamp));
            WriteString(file, event.type);
            file.write(reinterpret_cast<const char*>(&event.sourceEntity), sizeof(event.sourceEntity));
            file.write(reinterpret_cast<const char*>(&event.targetEntity), sizeof(event.targetEntity));
            file.write(reinterpret_cast<const char*>(&event.position), sizeof(event.position));
            WriteString(file, event.data);
        }

        return file.good();
    }

    bool ReplaySystem::LoadFromFile(const std::string& filePath)
    {
        std::lock_guard lock(m_mutex);

        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "ReplaySystem::LoadFromFile: cannot open '%s' (errno=%d)",
                            filePath.c_str(), errno);
            return false;
        }

        // Header
        uint32_t magic = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (!file || magic != kReplayMagic)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "ReplaySystem::LoadFromFile: '%s' has invalid magic (got 0x%08X, expected 0x%08X)",
                           filePath.c_str(), magic, kReplayMagic);
            return false;
        }

        file.read(reinterpret_cast<char*>(&m_data.version), sizeof(m_data.version));
        if (!file)
            return false;

        // Metadata
        if (!ReadString(file, m_data.mapName))
            return false;
        if (!ReadString(file, m_data.gameMode))
            return false;
        file.read(reinterpret_cast<char*>(&m_data.duration), sizeof(m_data.duration));
        if (!file)
            return false;

        // Frames
        uint32_t frameCount = 0;
        file.read(reinterpret_cast<char*>(&frameCount), sizeof(frameCount));
        if (!file || frameCount > kMaxFrameCount)
            return false;

        m_data.frames.clear();
        m_data.frames.resize(frameCount);
        for (uint32_t f = 0; f < frameCount; ++f)
        {
            auto& frame = m_data.frames[f];
            file.read(reinterpret_cast<char*>(&frame.timestamp), sizeof(frame.timestamp));
            file.read(reinterpret_cast<char*>(&frame.frameNumber), sizeof(frame.frameNumber));

            uint32_t entityCount = 0;
            file.read(reinterpret_cast<char*>(&entityCount), sizeof(entityCount));
            if (!file || entityCount > kMaxEntityCount)
                return false;

            frame.entities.resize(entityCount);
            for (uint32_t e = 0; e < entityCount; ++e)
            {
                auto& entity = frame.entities[e];
                file.read(reinterpret_cast<char*>(&entity.entityId), sizeof(entity.entityId));
                file.read(reinterpret_cast<char*>(&entity.position), sizeof(entity.position));
                file.read(reinterpret_cast<char*>(&entity.rotation), sizeof(entity.rotation));
                file.read(reinterpret_cast<char*>(&entity.velocity), sizeof(entity.velocity));
                file.read(reinterpret_cast<char*>(&entity.health), sizeof(entity.health));
                file.read(reinterpret_cast<char*>(&entity.animationState), sizeof(entity.animationState));
                file.read(reinterpret_cast<char*>(&entity.flags), sizeof(entity.flags));
                if (!file)
                    return false;
            }
        }

        // Events
        uint32_t eventCount = 0;
        file.read(reinterpret_cast<char*>(&eventCount), sizeof(eventCount));
        if (!file || eventCount > kMaxEventCount)
            return false;

        m_data.events.clear();
        m_data.events.resize(eventCount);
        for (uint32_t i = 0; i < eventCount; ++i)
        {
            auto& event = m_data.events[i];
            file.read(reinterpret_cast<char*>(&event.timestamp), sizeof(event.timestamp));
            if (!ReadString(file, event.type))
                return false;
            file.read(reinterpret_cast<char*>(&event.sourceEntity), sizeof(event.sourceEntity));
            file.read(reinterpret_cast<char*>(&event.targetEntity), sizeof(event.targetEntity));
            file.read(reinterpret_cast<char*>(&event.position), sizeof(event.position));
            if (!ReadString(file, event.data))
                return false;
        }

        // Reset playback state
        m_playbackState = PlaybackState::Stopped;
        m_playbackTime = 0.0f;
        m_currentFrameIndex = 0;
        m_recording = false;

        return true;
    }

    // ============================================================================
    // Console
    // ============================================================================

    std::string ReplaySystem::Console_GetStatus() const
    {
        std::lock_guard lock(m_mutex);

        std::ostringstream ss;
        ss << "=== Replay System ===\n";
        ss << "Recording: " << (m_recording ? "YES" : "NO") << "\n";

        const char* stateStr = "Stopped";
        switch (m_playbackState)
        {
        case PlaybackState::Playing:
            stateStr = "Playing";
            break;
        case PlaybackState::Paused:
            stateStr = "Paused";
            break;
        case PlaybackState::Rewinding:
            stateStr = "Rewinding";
            break;
        case PlaybackState::FastForward:
            stateStr = "FastForward";
            break;
        default:
            break;
        }
        ss << "Playback: " << stateStr << "\n";
        ss << "Playback time: " << m_playbackTime << "s / " << m_data.duration << "s\n";
        ss << "Speed: " << m_playbackSpeed << "x\n";
        ss << "Frames recorded: " << m_data.frames.size() << "\n";
        ss << "Events recorded: " << m_data.events.size() << "\n";
        ss << "Map: " << (m_data.mapName.empty() ? "(none)" : m_data.mapName) << "\n";
        ss << "Mode: " << (m_data.gameMode.empty() ? "(none)" : m_data.gameMode) << "\n";
        ss << "Kill cam: " << (m_killCamActive ? "ON" : "OFF") << "\n";
        return ss.str();
    }

    // ============================================================================
    // Internal Helpers
    // ============================================================================

    size_t ReplaySystem::FindFrameIndex(float timestamp) const
    {
        if (m_data.frames.empty())
            return 0;

        auto it = std::lower_bound(m_data.frames.begin(), m_data.frames.end(), timestamp,
                                   [](const ReplayFrame& f, float t) { return f.timestamp < t; });

        if (it == m_data.frames.end())
            return m_data.frames.size() - 1;

        return static_cast<size_t>(std::distance(m_data.frames.begin(), it));
    }

} // namespace Spark
