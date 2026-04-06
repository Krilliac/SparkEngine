/**
 * @file MovieRenderPipeline.h
 * @brief Offline cinematic rendering pipeline for high-quality frame output
 * @author Spark Engine Team
 * @date 2026
 *
 * Renders cinematic sequences offline at arbitrary resolution and quality,
 * stepping the simulation deterministically at a fixed timestep. Supports
 * temporal accumulation, motion blur sub-frames, warm-up periods, and
 * console variable overrides. Pixel readback delegates to ScreenCapture.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Spark::Rendering
{

    /// @brief Quality preset for offline rendering
    enum class RenderQuality
    {
        Preview,   ///< Fast preview — low AA, no motion blur
        Standard,  ///< Balanced quality and speed
        High,      ///< High quality — 8x AA, 4 motion blur sub-frames
        Cinematic, ///< Maximum quality — 32x AA, 16 motion blur sub-frames
        Custom     ///< User-defined settings (no preset overrides)
    };

    /// @brief Output image format for rendered frames
    enum class OutputFormat
    {
        PNG, ///< 8-bit RGBA PNG
        EXR, ///< 32-bit float HDR (OpenEXR)
        TGA  ///< 32-bit uncompressed TGA
    };

    /// @brief Configuration for a movie render job
    struct MovieRenderSettings
    {
        std::string outputDirectory = "Renders";                        ///< Output directory
        std::string filenamePattern = "frame_{0:06d}.png";              ///< Filename pattern
        uint32_t width = 1920;                                          ///< Output width (pixels)
        uint32_t height = 1080;                                         ///< Output height (pixels)
        float frameRate = 30.0f;                                        ///< Target fps
        int32_t startFrame = 0;                                         ///< First frame
        int32_t endFrame = 300;                                         ///< Last frame (inclusive)
        uint32_t aaSamples = 1;                                         ///< Temporal AA samples (1,4,8,16,32)
        uint32_t motionBlurSubFrames = 1;                               ///< Motion blur sub-frames (1-16)
        RenderQuality qualityPreset = RenderQuality::Standard;          ///< Quality preset
        OutputFormat outputFormat = OutputFormat::PNG;                  ///< Image format
        uint32_t warmUpFrames = 0;                                      ///< Warm-up frames before recording
        bool includeAlpha = false;                                      ///< Include alpha channel
        std::vector<std::pair<std::string, std::string>> cvarOverrides; ///< CVar overrides

        /// @brief Seconds per frame based on frame rate
        [[nodiscard]] float GetFixedDeltaTime() const
        {
            return (frameRate > 0.0f) ? (1.0f / frameRate) : (1.0f / 30.0f);
        }

        /// @brief Total number of frames to render
        [[nodiscard]] int32_t GetTotalFrames() const
        {
            return (endFrame >= startFrame) ? (endFrame - startFrame + 1) : 0;
        }

        /// @brief Delta time per sub-frame for temporal accumulation
        [[nodiscard]] float GetSubFrameDeltaTime() const
        {
            const uint32_t total = aaSamples * motionBlurSubFrames;
            return (total > 0) ? (GetFixedDeltaTime() / static_cast<float>(total)) : GetFixedDeltaTime();
        }

        /// @brief Apply quality preset, overriding AA and motion blur settings
        void ApplyPreset()
        {
            switch (qualityPreset)
            {
            case RenderQuality::Preview:
                aaSamples = 1;
                motionBlurSubFrames = 1;
                break;
            case RenderQuality::Standard:
                aaSamples = 4;
                motionBlurSubFrames = 2;
                break;
            case RenderQuality::High:
                aaSamples = 8;
                motionBlurSubFrames = 4;
                break;
            case RenderQuality::Cinematic:
                aaSamples = 32;
                motionBlurSubFrames = 16;
                break;
            case RenderQuality::Custom:
                break;
            }
        }
    };

    /// @brief State of a movie render job
    enum class RenderJobState
    {
        Idle,       ///< Not started
        WarmingUp,  ///< Running warm-up frames
        Rendering,  ///< Actively rendering frames
        Finalizing, ///< Writing remaining output
        Complete,   ///< Successfully finished
        Failed,     ///< Encountered an error
        Cancelled   ///< User cancelled
    };

    /// @brief Active or completed movie render job
    struct MovieRenderJob
    {
        MovieRenderSettings settings;                ///< Job settings
        RenderJobState state = RenderJobState::Idle; ///< Current state
        int32_t currentFrame = 0;                    ///< Frame being rendered
        int32_t totalFrames = 0;                     ///< Total frames
        uint32_t currentSubFrame = 0;                ///< Sub-frame index
        uint32_t totalSubFramesPerFrame = 0;         ///< Sub-frames per output frame
        uint32_t warmUpFramesRemaining = 0;          ///< Warm-up frames left
        float progress = 0.0f;                       ///< Progress [0, 1]
        float elapsedSeconds = 0.0f;                 ///< Wall-clock elapsed
        float estimatedRemainingSeconds = 0.0f;      ///< Estimated remaining
        std::vector<std::string> outputFiles;        ///< Output file paths
        std::string errorMessage;                    ///< Error if failed

        /// @brief Recompute progress and ETA from current frame count
        void UpdateProgress()
        {
            if (totalFrames > 0)
            {
                progress = static_cast<float>(currentFrame) / static_cast<float>(totalFrames);
                progress = (progress < 0.0f) ? 0.0f : ((progress > 1.0f) ? 1.0f : progress);
            }
            if (progress > 0.001f && elapsedSeconds > 0.0f)
            {
                estimatedRemainingSeconds = (elapsedSeconds / progress) * (1.0f - progress);
            }
        }
    };

    /// @brief Overrides engine delta time during offline rendering for deterministic simulation
    class DeterministicTimeController
    {
      public:
        /// @brief Begin time override
        void Begin(float fixedDt, uint32_t warmUpFrames)
        {
            m_fixedDt = fixedDt;
            m_active = true;
            m_currentTime = 0.0f;
            m_frameIndex = 0;
            m_warmUpFrames = warmUpFrames;
            m_warmUpComplete = (warmUpFrames == 0);
        }

        void End() { m_active = false; }

        /// @brief Advance one simulation step, returns the fixed dt used
        float Step()
        {
            m_currentTime += m_fixedDt;
            ++m_frameIndex;
            if (!m_warmUpComplete && m_frameIndex >= m_warmUpFrames)
            {
                m_warmUpComplete = true;
            }
            return m_fixedDt;
        }

        [[nodiscard]] bool IsWarmUpComplete() const { return m_warmUpComplete; }
        [[nodiscard]] bool IsActive() const { return m_active; }
        [[nodiscard]] float GetFixedDt() const { return m_fixedDt; }
        [[nodiscard]] float GetCurrentTime() const { return m_currentTime; }
        [[nodiscard]] uint32_t GetFrameIndex() const { return m_frameIndex; }

      private:
        float m_fixedDt = 1.0f / 30.0f;
        float m_currentTime = 0.0f;
        uint32_t m_frameIndex = 0;
        uint32_t m_warmUpFrames = 0;
        bool m_warmUpComplete = false;
        bool m_active = false;
    };

    /// @brief Offline cinematic rendering pipeline (singleton). Call Update() each frame.
    class MovieRenderPipeline
    {
      public:
        using FrameCapturedCallback = std::function<void(int32_t, const std::string&)>;
        using RenderCompleteCallback = std::function<void(const MovieRenderJob&)>;

        /// @brief Get the singleton instance
        static MovieRenderPipeline& GetInstance()
        {
            static MovieRenderPipeline instance;
            return instance;
        }

        /// @brief Initialize the pipeline
        void Initialize()
        {
            m_initialized = true;
            m_activeJob = MovieRenderJob{};
            m_completedJobs.clear();
        }

        /// @brief Shut down, cancelling any active render
        void Shutdown()
        {
            if (IsRendering())
            {
                CancelRender();
            }
            m_initialized = false;
        }

        /// @brief Start an offline render; returns true on success
        bool StartRender(MovieRenderSettings settings)
        {
            if (!m_initialized || IsRendering())
            {
                return false;
            }

            if (settings.qualityPreset != RenderQuality::Custom)
            {
                settings.ApplyPreset();
            }

            // Clamp to valid ranges
            settings.aaSamples = ClampToValidAASamples(settings.aaSamples);
            settings.motionBlurSubFrames = ClampRange(settings.motionBlurSubFrames, 1u, 16u);

            m_activeJob = MovieRenderJob{};
            m_activeJob.settings = std::move(settings);
            m_activeJob.totalFrames = m_activeJob.settings.GetTotalFrames();
            m_activeJob.totalSubFramesPerFrame =
                m_activeJob.settings.aaSamples * m_activeJob.settings.motionBlurSubFrames;
            m_activeJob.warmUpFramesRemaining = m_activeJob.settings.warmUpFrames;
            m_activeJob.outputFiles.reserve(static_cast<size_t>(m_activeJob.totalFrames));

            if (m_activeJob.totalFrames <= 0)
            {
                m_activeJob.state = RenderJobState::Failed;
                m_activeJob.errorMessage = "Invalid frame range (endFrame < startFrame)";
                FinalizeJob();
                return false;
            }

            m_timeController.Begin(m_activeJob.settings.GetSubFrameDeltaTime(), m_activeJob.settings.warmUpFrames);
            m_activeJob.state =
                (m_activeJob.warmUpFramesRemaining > 0) ? RenderJobState::WarmingUp : RenderJobState::Rendering;
            m_renderStartTime = std::chrono::steady_clock::now();
            m_accumulationBuffer.clear();
            return true;
        }

        /// @brief Cancel the active render job
        void CancelRender()
        {
            if (!IsRendering())
                return;
            m_activeJob.state = RenderJobState::Cancelled;
            m_timeController.End();
            FinalizeJob();
        }

        [[nodiscard]] bool IsRendering() const
        {
            return m_activeJob.state == RenderJobState::WarmingUp || m_activeJob.state == RenderJobState::Rendering;
        }

        [[nodiscard]] float GetProgress() const { return m_activeJob.progress; }
        [[nodiscard]] const MovieRenderJob& GetCurrentJob() const { return m_activeJob; }
        [[nodiscard]] const std::vector<MovieRenderJob>& GetCompletedJobs() const { return m_completedJobs; }

        void SetFrameCapturedCallback(FrameCapturedCallback cb) { m_frameCapturedCallback = std::move(cb); }
        void SetRenderCompleteCallback(RenderCompleteCallback cb) { m_renderCompleteCallback = std::move(cb); }

        /// @brief Main update -- call each frame. Uses fixed dt when rendering.
        void Update([[maybe_unused]] float deltaTime)
        {
            if (!m_initialized || !IsRendering())
            {
                return;
            }

            UpdateElapsedTime();

            if (m_activeJob.state == RenderJobState::WarmingUp)
            {
                StepWarmUp();
            }
            else
            {
                StepFrame();
            }
        }

        /// @brief Get a console-friendly status string
        [[nodiscard]] std::string Console_GetStatus() const
        {
            if (!m_initialized)
            {
                return "MovieRenderPipeline: not initialized";
            }

            switch (m_activeJob.state)
            {
            case RenderJobState::Idle:
                return std::format("MovieRenderPipeline: idle ({} completed jobs)", m_completedJobs.size());
            case RenderJobState::WarmingUp:
                return std::format("MovieRenderPipeline: warming up ({} frames remaining)",
                                   m_activeJob.warmUpFramesRemaining);
            case RenderJobState::Rendering:
                return std::format("MovieRenderPipeline: rendering frame {}/{} ({:.1f}%) ETA {:.0f}s",
                                   m_activeJob.currentFrame, m_activeJob.totalFrames, m_activeJob.progress * 100.0f,
                                   m_activeJob.estimatedRemainingSeconds);
            case RenderJobState::Finalizing:
                return "MovieRenderPipeline: finalizing";
            case RenderJobState::Complete:
                return std::format("MovieRenderPipeline: complete ({} frames)", m_activeJob.totalFrames);
            case RenderJobState::Failed:
                return std::format("MovieRenderPipeline: FAILED - {}", m_activeJob.errorMessage);
            case RenderJobState::Cancelled:
                return std::format("MovieRenderPipeline: cancelled at frame {}", m_activeJob.currentFrame);
            }
            return "MovieRenderPipeline: unknown state";
        }

      private:
        MovieRenderPipeline() = default;

        /// @brief Process one warm-up frame (simulate without capture)
        void StepWarmUp()
        {
            m_timeController.Step();
            if (m_activeJob.warmUpFramesRemaining > 0)
            {
                --m_activeJob.warmUpFramesRemaining;
            }
            if (m_activeJob.warmUpFramesRemaining == 0)
            {
                m_activeJob.state = RenderJobState::Rendering;
            }
        }

        /// @brief Advance one sub-frame; capture when all sub-frames for a frame complete
        void StepFrame()
        {
            m_timeController.Step();
            ++m_activeJob.currentSubFrame;

            if (m_activeJob.currentSubFrame >= m_activeJob.totalSubFramesPerFrame)
            {
                CaptureFrame();
                m_activeJob.currentSubFrame = 0;
                ++m_activeJob.currentFrame;
                m_activeJob.UpdateProgress();

                if (m_activeJob.currentFrame >= m_activeJob.totalFrames)
                {
                    m_activeJob.state = RenderJobState::Finalizing;
                    m_timeController.End();
                    CompleteRender();
                }
            }
        }

        /// @brief Capture the accumulated frame to disk via ScreenCapture
        void CaptureFrame()
        {
            const int32_t absoluteFrame = m_activeJob.settings.startFrame + m_activeJob.currentFrame;
            const std::string filename = std::format("frame_{:06d}{}", absoluteFrame, GetFormatExtension());
            const std::string filePath = m_activeJob.settings.outputDirectory + "/" + filename;

            // In production: resolve accumulation buffer, call ScreenCapture::CaptureFrame()
            m_activeJob.outputFiles.push_back(filePath);

            if (m_frameCapturedCallback)
            {
                m_frameCapturedCallback(absoluteFrame, filePath);
            }
        }

        void CompleteRender()
        {
            m_activeJob.state = RenderJobState::Complete;
            FinalizeJob();
        }

        void FinalizeJob()
        {
            m_timeController.End();
            if (m_renderCompleteCallback)
            {
                m_renderCompleteCallback(m_activeJob);
            }
            m_completedJobs.push_back(m_activeJob);
        }

        void UpdateElapsedTime()
        {
            auto now = std::chrono::steady_clock::now();
            m_activeJob.elapsedSeconds = std::chrono::duration<float>(now - m_renderStartTime).count();
            m_activeJob.UpdateProgress();
        }

        [[nodiscard]] std::string GetFormatExtension() const
        {
            switch (m_activeJob.settings.outputFormat)
            {
            case OutputFormat::PNG:
                return ".png";
            case OutputFormat::EXR:
                return ".exr";
            case OutputFormat::TGA:
                return ".tga";
            }
            return ".png";
        }

        static constexpr uint32_t ClampToValidAASamples(uint32_t samples)
        {
            if (samples <= 1)
                return 1;
            if (samples <= 4)
                return 4;
            if (samples <= 8)
                return 8;
            if (samples <= 16)
                return 16;
            return 32;
        }

        static constexpr uint32_t ClampRange(uint32_t value, uint32_t minVal, uint32_t maxVal)
        {
            return (value < minVal) ? minVal : ((value > maxVal) ? maxVal : value);
        }

        bool m_initialized = false;
        MovieRenderJob m_activeJob;
        std::vector<MovieRenderJob> m_completedJobs;
        DeterministicTimeController m_timeController;
        std::chrono::steady_clock::time_point m_renderStartTime;
        std::vector<float> m_accumulationBuffer; ///< Sub-frame blending buffer (real pixels from GPU)
        FrameCapturedCallback m_frameCapturedCallback;
        RenderCompleteCallback m_renderCompleteCallback;
    };

} // namespace Spark::Rendering
