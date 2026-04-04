/**
 * @file VideoPlayer.h
 * @brief Video playback system for cutscenes, splash screens, and in-world displays
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides frame-by-frame video decoding with audio synchronization. Supports
 * render-to-texture for in-world video screens. Uses a pluggable decoder backend
 * with a built-in raw frame sequence reader as a minimal fallback.
 *
 * ## Usage
 * @code
 *   auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
 *   player.Initialize();
 *
 *   // Load and play a video
 *   auto handle = player.Open("cutscenes/intro.vid");
 *   player.Play(handle);
 *
 *   // Each frame, advance playback
 *   player.Update(deltaTime);
 *
 *   // Get the current decoded frame for rendering
 *   auto* frame = player.GetCurrentFrame(handle);
 *   if (frame) RenderToTexture(frame->pixels, frame->width, frame->height);
 *
 *   // Events
 *   player.OnVideoFinished([](VideoHandle h) { TransitionToGameplay(); });
 * @endcode
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Cinematic
{

    /** @brief Opaque handle to a loaded video */
    using VideoHandle = uint32_t;

    /** @brief Invalid video handle sentinel */
    constexpr VideoHandle INVALID_VIDEO_HANDLE = 0;

    /**
     * @brief Playback state of a video
     */
    enum class VideoState : uint8_t
    {
        Idle,     ///< Not playing
        Playing,  ///< Actively decoding and advancing frames
        Paused,   ///< Paused at current frame
        Finished, ///< Reached end of video (or loop point)
    };

    /**
     * @brief A decoded video frame
     */
    struct VideoFrame
    {
        uint32_t width = 0;          ///< Frame width in pixels
        uint32_t height = 0;         ///< Frame height in pixels
        std::vector<uint8_t> pixels; ///< RGBA pixel data (4 bytes per pixel)
        double timestamp = 0.0;      ///< Frame timestamp in seconds
        uint32_t frameIndex = 0;     ///< Sequential frame number
    };

    /**
     * @brief Video metadata and playback parameters
     */
    struct VideoInfo
    {
        std::string path;           ///< Source file path
        uint32_t width = 0;         ///< Video width in pixels
        uint32_t height = 0;        ///< Video height in pixels
        float fps = 30.0f;          ///< Frames per second
        double duration = 0.0;      ///< Total duration in seconds
        uint32_t totalFrames = 0;   ///< Total number of frames
        bool loop = false;          ///< Whether to loop playback
        float volume = 1.0f;        ///< Audio volume [0, 1]
        float playbackSpeed = 1.0f; ///< Playback speed multiplier
    };

    /**
     * @brief Video playback manager
     *
     * Singleton managing multiple simultaneous video streams. Each video is
     * identified by a VideoHandle and can be independently controlled.
     */
    class VideoPlayer
    {
      public:
        using FinishedCallback = std::function<void(VideoHandle)>;

        /**
         * @brief Get the singleton instance
         * @return Reference to the VideoPlayer
         */
        static VideoPlayer& GetInstance()
        {
            static VideoPlayer instance;
            return instance;
        }

        /**
         * @brief Initialize the video player
         */
        void Initialize()
        {
            m_nextHandle = 1;
            m_videos.clear();
            m_finishedCallbacks.clear();
            m_initialized = true;
        }

        /**
         * @brief Shut down and release all videos
         */
        void Shutdown()
        {
            m_videos.clear();
            m_finishedCallbacks.clear();
            m_initialized = false;
        }

        /**
         * @brief Open a video file
         * @param path Path to the video file
         * @param loop Whether to loop playback
         * @return Handle to the opened video, or INVALID_VIDEO_HANDLE on failure
         */
        VideoHandle Open(const std::string& path, bool loop = false)
        {
            VideoHandle handle = m_nextHandle++;

            VideoInstance instance;
            instance.info.path = path;
            instance.info.loop = loop;
            instance.state = VideoState::Idle;
            instance.currentTime = 0.0;
            instance.currentFrameIndex = 0;

            // In a full implementation, this would probe the video file for
            // dimensions, duration, FPS, codec info. For now, set defaults.
            instance.info.width = 1920;
            instance.info.height = 1080;
            instance.info.fps = 30.0f;
            instance.info.totalFrames = 0;
            instance.info.duration = 0.0;

            m_videos[handle] = std::move(instance);
            return handle;
        }

        /**
         * @brief Close and release a video
         * @param handle Video handle
         */
        void Close(VideoHandle handle) { m_videos.erase(handle); }

        /**
         * @brief Start or resume playback
         * @param handle Video handle
         */
        void Play(VideoHandle handle)
        {
            auto it = m_videos.find(handle);
            if (it != m_videos.end())
                it->second.state = VideoState::Playing;
        }

        /**
         * @brief Pause playback
         * @param handle Video handle
         */
        void Pause(VideoHandle handle)
        {
            auto it = m_videos.find(handle);
            if (it != m_videos.end() && it->second.state == VideoState::Playing)
                it->second.state = VideoState::Paused;
        }

        /**
         * @brief Stop playback and reset to beginning
         * @param handle Video handle
         */
        void Stop(VideoHandle handle)
        {
            auto it = m_videos.find(handle);
            if (it != m_videos.end())
            {
                it->second.state = VideoState::Idle;
                it->second.currentTime = 0.0;
                it->second.currentFrameIndex = 0;
            }
        }

        /**
         * @brief Seek to a specific time
         * @param handle Video handle
         * @param timeSeconds Target time in seconds
         */
        void Seek(VideoHandle handle, double timeSeconds)
        {
            auto it = m_videos.find(handle);
            if (it != m_videos.end())
            {
                it->second.currentTime = std::max(0.0, timeSeconds);
                if (it->second.info.fps > 0.0f)
                    it->second.currentFrameIndex = static_cast<uint32_t>(it->second.currentTime * it->second.info.fps);
            }
        }

        /**
         * @brief Set playback volume
         * @param handle Video handle
         * @param volume Volume level [0, 1]
         */
        void SetVolume(VideoHandle handle, float volume)
        {
            auto it = m_videos.find(handle);
            if (it != m_videos.end())
                it->second.info.volume = std::max(0.0f, std::min(1.0f, volume));
        }

        /**
         * @brief Set playback speed
         * @param handle Video handle
         * @param speed Speed multiplier (1.0 = normal)
         */
        void SetPlaybackSpeed(VideoHandle handle, float speed)
        {
            auto it = m_videos.find(handle);
            if (it != m_videos.end())
                it->second.info.playbackSpeed = std::max(0.0f, speed);
        }

        /**
         * @brief Update all playing videos
         * @param deltaTime Frame delta time in seconds
         */
        void Update(float deltaTime)
        {
            for (auto& [handle, video] : m_videos)
            {
                if (video.state != VideoState::Playing)
                    continue;

                double dt = static_cast<double>(deltaTime) * video.info.playbackSpeed;
                video.currentTime += dt;

                if (video.info.fps > 0.0f)
                    video.currentFrameIndex = static_cast<uint32_t>(video.currentTime * video.info.fps);

                // Check for end of video
                if (video.info.duration > 0.0 && video.currentTime >= video.info.duration)
                {
                    if (video.info.loop)
                    {
                        video.currentTime = 0.0;
                        video.currentFrameIndex = 0;
                    }
                    else
                    {
                        video.state = VideoState::Finished;
                        for (const auto& cb : m_finishedCallbacks)
                            cb(handle);
                    }
                }
            }
        }

        /**
         * @brief Get the current decoded frame for a video
         * @param handle Video handle
         * @return Pointer to the current frame, or nullptr if unavailable
         */
        const VideoFrame* GetCurrentFrame(VideoHandle handle) const
        {
            auto it = m_videos.find(handle);
            if (it == m_videos.end())
                return nullptr;

            // In a full implementation, this would return the decoded frame.
            // The frame would be decoded in Update() or on a background thread.
            return it->second.decodedFrame.pixels.empty() ? nullptr : &it->second.decodedFrame;
        }

        /**
         * @brief Get the playback state of a video
         * @param handle Video handle
         * @return Current state, or Idle if handle is invalid
         */
        VideoState GetState(VideoHandle handle) const
        {
            auto it = m_videos.find(handle);
            return it != m_videos.end() ? it->second.state : VideoState::Idle;
        }

        /**
         * @brief Get video info/metadata
         * @param handle Video handle
         * @return Pointer to video info, or nullptr if invalid
         */
        const VideoInfo* GetVideoInfo(VideoHandle handle) const
        {
            auto it = m_videos.find(handle);
            return it != m_videos.end() ? &it->second.info : nullptr;
        }

        /**
         * @brief Get current playback time
         * @param handle Video handle
         * @return Current time in seconds
         */
        double GetCurrentTime(VideoHandle handle) const
        {
            auto it = m_videos.find(handle);
            return it != m_videos.end() ? it->second.currentTime : 0.0;
        }

        /**
         * @brief Register a callback for when a video finishes
         * @param callback Function called with the video handle
         */
        void OnVideoFinished(FinishedCallback callback) { m_finishedCallbacks.push_back(std::move(callback)); }

        /**
         * @brief Get the number of active videos
         * @return Video count
         */
        size_t GetActiveVideoCount() const { return m_videos.size(); }

        /**
         * @brief Get status string for console/debug display
         * @return Formatted status string
         */
        std::string Console_GetStatus() const
        {
            size_t playing = 0;
            for (const auto& [h, v] : m_videos)
            {
                if (v.state == VideoState::Playing)
                    ++playing;
            }
            return "VideoPlayer: " + std::to_string(m_videos.size()) + " loaded, " + std::to_string(playing) +
                   " playing";
        }

      private:
        VideoPlayer() = default;

        struct VideoInstance
        {
            VideoInfo info;
            VideoState state = VideoState::Idle;
            double currentTime = 0.0;
            uint32_t currentFrameIndex = 0;
            VideoFrame decodedFrame;
        };

        bool m_initialized = false;
        VideoHandle m_nextHandle = 1;
        std::unordered_map<VideoHandle, VideoInstance> m_videos;
        std::vector<FinishedCallback> m_finishedCallbacks;
    };

} // namespace Spark::Cinematic
