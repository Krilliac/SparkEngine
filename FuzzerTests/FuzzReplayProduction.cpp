/** @brief libc++-compiled production adapter for ReplaySystem fuzzing. */
#include "FuzzReplayProduction.h"

#include "Engine/Replay/ReplaySystem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    constexpr std::size_t kMaxInputBytes = 1024u * 1024u + 1u;
    constexpr std::size_t kVersionOffset = sizeof(std::uint32_t);
    std::atomic<std::uint64_t> s_fileCounter{0};

    std::uint64_t ProcessId()
    {
#if defined(_WIN32)
        return static_cast<std::uint64_t>(_getpid());
#else
        return static_cast<std::uint64_t>(getpid());
#endif
    }

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzReplay: %s\n", what);
        std::abort();
    }

    bool Contains(const std::string& haystack, const char* needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    bool IsFinite(const XMFLOAT3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsFinite(const XMFLOAT4& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
    }

    /// Seeds the replay the loader must keep when it rejects an input: two frames, 1 second, named metadata.
    void RecordSentinel(Spark::ReplaySystem& replay)
    {
        replay.SetMetadata("sentinel-map", "sentinel-mode");
        replay.StartRecording();
        replay.RecordFrame({Spark::ReplayEntityState{}}, 0.0f);
        replay.RecordFrame({}, 1.0f);
        replay.StopRecording();
    }

    bool HoldsSentinel(const Spark::ReplaySystem& replay)
    {
        const std::string status = replay.Console_GetStatus();
        return replay.GetFrameCount() == 2u && replay.GetDuration() == 1.0f &&
               Contains(status, "Map: sentinel-map\n") && Contains(status, "Mode: sentinel-mode\n");
    }

    void CheckPlaybackTimeInRange(const Spark::ReplaySystem& replay, float duration)
    {
        const float time = replay.GetPlaybackTime();
        if (!std::isfinite(time) || time < 0.0f || time > duration)
        {
            InvariantFailure("playback time left [0, duration]");
        }
    }

    /// Frames reached by playback must come in timestamp order and carry only finite entity values.
    void CheckCurrentFrame(const Spark::ReplaySystem& replay, float& lastTimestamp)
    {
        const Spark::ReplayFrame* frame = replay.GetCurrentFrame();
        if (frame == nullptr)
        {
            return;
        }
        if (!std::isfinite(frame->timestamp) || frame->timestamp < lastTimestamp)
        {
            InvariantFailure("playback reached a frame earlier than the previous one");
        }
        lastTimestamp = frame->timestamp;
        for (const Spark::ReplayEntityState& entity : frame->entities)
        {
            if (!IsFinite(entity.position) || !IsFinite(entity.rotation) || !IsFinite(entity.velocity) ||
                !std::isfinite(entity.health))
            {
                InvariantFailure("playback reached a frame with a non-finite entity value");
            }
        }
    }

    /// Drives every consumer of the loaded timeline: seeking, bounded playback to the end, the kill cam
    /// and the event window. Playback must stop within ceil(duration / step) + 2 steps.
    void ExercisePlayback(Spark::ReplaySystem& replay)
    {
        const float duration = replay.GetDuration();
        if (!std::isfinite(duration) || duration < 0.0f)
        {
            InvariantFailure("accepted replay has a non-finite or negative duration");
        }
        for (const float target : {0.0f, duration * 0.5f, duration})
        {
            replay.SeekTo(target);
            CheckPlaybackTimeInRange(replay, duration);
        }

        constexpr float kStepsToEnd = 64.0f;
        const float step = std::max(duration / kStepsToEnd, 1.0f / 60.0f);
        const auto maxSteps = static_cast<std::size_t>(std::ceil(duration / step)) + 2u;
        float lastTimestamp = -std::numeric_limits<float>::infinity();
        replay.StartPlayback();
        for (std::size_t i = 0; i < maxSteps && replay.GetPlaybackState() == Spark::PlaybackState::Playing; ++i)
        {
            replay.UpdatePlayback(step);
            CheckPlaybackTimeInRange(replay, duration);
            CheckCurrentFrame(replay, lastTimestamp);
            (void)replay.GetEventsNearTime(step);
        }
        if (replay.GetPlaybackState() != Spark::PlaybackState::Stopped)
        {
            InvariantFailure("playback did not reach the end of the replay within its step bound");
        }

        replay.StartKillCam(duration * 0.25f, 1u);
        CheckPlaybackTimeInRange(replay, duration);
        for (int i = 0; i < 4; ++i)
        {
            replay.UpdatePlayback(step);
            CheckPlaybackTimeInRange(replay, duration);
        }
        (void)replay.GetEventsNearTime(0.5f);
        (void)replay.Console_GetStatus();
        replay.StopKillCam();
    }
} // namespace

extern "C" int SparkFuzzLoadReplay(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const auto suffix = s_fileCounter.fetch_add(1, std::memory_order_relaxed);
    std::string fileName = "spark-fuzz-replay-";
    fileName += std::to_string(ProcessId());
    fileName += "-";
    fileName += std::to_string(suffix);
    fileName += ".replay";
    const std::filesystem::path path = std::filesystem::temp_directory_path() / fileName;
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return 0;
        }
        if (size != 0)
        {
            file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
    }

    Spark::ReplaySystem replay;
    RecordSentinel(replay);
    if (!HoldsSentinel(replay))
    {
        InvariantFailure("the sentinel recording did not take");
    }
    const bool accepted = replay.LoadFromFile(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    if (!accepted)
    {
        if (!HoldsSentinel(replay))
        {
            InvariantFailure("rejected replay input modified the previously loaded replay");
        }
        return 0;
    }

    // Independent header model: SaveToFile writes version 1, the only layout the reader knows.
    std::uint32_t version = 0;
    if (size < kVersionOffset + sizeof(version))
    {
        InvariantFailure("accepted a replay shorter than its header");
    }
    std::memcpy(&version, data + kVersionOffset, sizeof(version));
    if (version != Spark::kReplayVersion)
    {
        InvariantFailure("accepted a replay with an unknown version");
    }
    ExercisePlayback(replay);
    return 0;
}
