/**
 * @file DebugOverlay.h
 * @brief Lightweight in-game debug HUD overlay
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a minimal, always-available debug HUD that displays key engine
 * metrics without requiring the full editor or profiler UI. Designed to be
 * rendered as a transparent overlay on top of the game viewport.
 *
 * Features:
 * - FPS counter with frame time graph
 * - Per-system timing breakdown
 * - Memory usage summary
 * - Entity count and draw call statistics
 * - Custom debug text lines from any subsystem
 * - Toggleable sections (compact/expanded modes)
 * - Configurable screen position and opacity
 *
 * Typical usage:
 * @code
 *   auto& overlay = Spark::DebugOverlay::GetInstance();
 *   overlay.SetEnabled(true);
 *   overlay.SetSection(Spark::OverlaySection::FPS, true);
 *   overlay.AddCustomLine("Player", "HP: 100/100");
 *   overlay.Update(deltaTime);
 * @endcode
 *
 * @see Profiler.h, SparkConsole.h
 */

#pragma once

#include "../Core/Platform.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <array>
#include <mutex>
#include <chrono>

namespace Spark
{

    /**
 * @brief Sections that can be individually toggled in the overlay
 */
    enum class OverlaySection
    {
        FPS = 0,   ///< FPS counter and frame time
        Timing,    ///< Per-system timing breakdown
        Memory,    ///< Memory usage
        Rendering, ///< Draw calls, triangles, etc.
        Entities,  ///< Active entity count
        Physics,   ///< Physics stats
        Audio,     ///< Audio stats
        Input,     ///< Input state
        Camera,    ///< Camera position/rotation
        Custom,    ///< Custom debug lines
        Count
    };

    /**
 * @brief Screen corner for overlay positioning
 */
    enum class OverlayPosition
    {
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    };

    /**
 * @brief Real-time stat entry for the overlay
 */
    struct OverlayStat
    {
        std::string label;
        std::string value;
        DirectX::XMFLOAT4 color = {1, 1, 1, 1}; // White by default
    };

    /**
 * @brief Frame time sample for mini-graph
 */
    struct FrameTimeSample
    {
        float frameTimeMs = 0.0f;
        float cpuTimeMs = 0.0f;
        float gpuTimeMs = 0.0f;
    };

    /**
 * @brief Singleton debug overlay for in-game HUD
 */
    class DebugOverlay
    {
      public:
        static constexpr int FRAME_HISTORY_SIZE = 120; // ~2 seconds at 60fps

        [[deprecated("Use EngineContext::Get()->GetSystem<DebugOverlay>() instead")]]
        static DebugOverlay& GetInstance()
        {
            static DebugOverlay instance;
            return instance;
        }

        // ========================================================================
        // Lifecycle
        // ========================================================================

        /**
     * @brief Update the overlay (call once per frame)
     * @param deltaTime Time since last frame in seconds
     */
        void Update(float deltaTime)
        {
            if (!m_enabled)
                return;

            m_frameCount++;
            m_fpsAccumulator += deltaTime;

            // Record frame time
            FrameTimeSample sample;
            sample.frameTimeMs = deltaTime * 1000.0f;
            m_frameHistory[m_frameHistoryIndex] = sample;
            m_frameHistoryIndex = (m_frameHistoryIndex + 1) % FRAME_HISTORY_SIZE;

            // Update FPS every 0.5 seconds
            m_fpsSampleCount++;
            if (m_fpsAccumulator >= 0.5f)
            {
                m_currentFPS = static_cast<float>(m_fpsSampleCount) / m_fpsAccumulator;
                m_fpsAccumulator = 0.0f;
                m_fpsSampleCount = 0;
            }

            // Compute frame time stats
            float sum = 0.0f;
            float minT = 1000.0f;
            float maxT = 0.0f;
            int count = 0;
            for (const auto& s : m_frameHistory)
            {
                if (s.frameTimeMs > 0.0f)
                {
                    sum += s.frameTimeMs;
                    if (s.frameTimeMs < minT)
                        minT = s.frameTimeMs;
                    if (s.frameTimeMs > maxT)
                        maxT = s.frameTimeMs;
                    count++;
                }
            }
            m_avgFrameTime = (count > 0) ? sum / count : 0.0f;
            m_minFrameTime = (count > 0) ? minT : 0.0f;
            m_maxFrameTime = (count > 0) ? maxT : 0.0f;
        }

        // ========================================================================
        // Configuration
        // ========================================================================

        bool IsEnabled() const { return m_enabled; }
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        void Toggle() { m_enabled = !m_enabled; }

        void SetPosition(OverlayPosition pos) { m_position = pos; }
        OverlayPosition GetPosition() const { return m_position; }

        void SetOpacity(float opacity) { m_opacity = opacity; }
        float GetOpacity() const { return m_opacity; }

        void SetSection(OverlaySection section, bool visible) { m_sectionVisible[static_cast<int>(section)] = visible; }

        bool IsSectionVisible(OverlaySection section) const { return m_sectionVisible[static_cast<int>(section)]; }

        void ToggleSection(OverlaySection section)
        {
            int idx = static_cast<int>(section);
            m_sectionVisible[idx] = !m_sectionVisible[idx];
        }

        /** @brief Toggle between compact (FPS only) and expanded modes */
        void SetCompactMode(bool compact) { m_compactMode = compact; }
        bool IsCompactMode() const { return m_compactMode; }

        // ========================================================================
        // Stats input (call from subsystems each frame)
        // ========================================================================

        void SetDrawCalls(int drawCalls) { m_drawCalls = drawCalls; }
        void SetTriangleCount(int triangles) { m_triangles = triangles; }
        void SetEntityCount(int entities) { m_entities = entities; }
        void SetVisibleEntities(int visible) { m_visibleEntities = visible; }
        void SetMemoryUsageMB(float mb) { m_memoryUsageMB = mb; }
        void SetVRAMUsageMB(float mb) { m_vramUsageMB = mb; }
        void SetPhysicsBodies(int bodies) { m_physicsBodies = bodies; }
        void SetAudioChannels(int channels) { m_audioChannels = channels; }

        void SetCameraPosition(const DirectX::XMFLOAT3& pos) { m_cameraPos = pos; }
        void SetCameraRotation(const DirectX::XMFLOAT3& rot) { m_cameraRot = rot; }

        /**
     * @brief Set a system timing value for the timing breakdown
     * @param systemName e.g., "Render", "Physics", "Audio"
     * @param timeMs Time in milliseconds
     */
        void SetSystemTime(const std::string& systemName, float timeMs)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_systemTimings[systemName] = timeMs;
        }

        /**
     * @brief Add or update a custom debug line
     * @param key   Unique key for this line (used for updates)
     * @param value Display value text
     * @param color Optional text color
     */
        void AddCustomLine(const std::string& key, const std::string& value,
                           const DirectX::XMFLOAT4& color = {1, 1, 1, 1})
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_customLines[key] = {key, value, color};
        }

        /**
     * @brief Remove a custom debug line
     */
        void RemoveCustomLine(const std::string& key)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_customLines.erase(key);
        }

        /** @brief Clear all custom debug lines */
        void ClearCustomLines()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_customLines.clear();
        }

        // ========================================================================
        // Data access (for rendering)
        // ========================================================================

        float GetCurrentFPS() const { return m_currentFPS; }
        float GetAvgFrameTime() const { return m_avgFrameTime; }
        float GetMinFrameTime() const { return m_minFrameTime; }
        float GetMaxFrameTime() const { return m_maxFrameTime; }
        int GetDrawCalls() const { return m_drawCalls; }
        int GetTriangleCount() const { return m_triangles; }
        int GetEntityCount() const { return m_entities; }
        uint64_t GetFrameCount() const { return m_frameCount; }

        const std::array<FrameTimeSample, FRAME_HISTORY_SIZE>& GetFrameHistory() const { return m_frameHistory; }

        /**
     * @brief Build a vector of all stats lines for rendering
     */
        std::vector<OverlayStat> BuildStatsLines() const
        {
            std::vector<OverlayStat> lines;

            if (m_sectionVisible[static_cast<int>(OverlaySection::FPS)])
            {
                char buf[128];
                snprintf(buf, sizeof(buf), "FPS: %.0f (%.2f ms avg, %.2f-%.2f ms)", m_currentFPS, m_avgFrameTime,
                         m_minFrameTime, m_maxFrameTime);

                DirectX::XMFLOAT4 fpsColor = {0, 1, 0, 1}; // Green
                if (m_currentFPS < 30.0f)
                    fpsColor = {1, 0, 0, 1}; // Red
                else if (m_currentFPS < 60.0f)
                    fpsColor = {1, 1, 0, 1}; // Yellow

                lines.push_back({"FPS", buf, fpsColor});
            }

            if (!m_compactMode)
            {
                if (m_sectionVisible[static_cast<int>(OverlaySection::Rendering)])
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Draw Calls: %d | Triangles: %d", m_drawCalls, m_triangles);
                    lines.push_back({"Rendering", buf, {1, 1, 1, 1}});
                }

                if (m_sectionVisible[static_cast<int>(OverlaySection::Memory)])
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "RAM: %.1f MB | VRAM: %.1f MB", m_memoryUsageMB, m_vramUsageMB);
                    lines.push_back({"Memory", buf, {1, 1, 1, 1}});
                }

                if (m_sectionVisible[static_cast<int>(OverlaySection::Entities)])
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Entities: %d (%d visible)", m_entities, m_visibleEntities);
                    lines.push_back({"Entities", buf, {1, 1, 1, 1}});
                }

                if (m_sectionVisible[static_cast<int>(OverlaySection::Timing)])
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (const auto& [name, timeMs] : m_systemTimings)
                    {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "%s: %.2f ms", name.c_str(), timeMs);

                        DirectX::XMFLOAT4 color = {1, 1, 1, 1};
                        if (timeMs > 8.0f)
                            color = {1, 0, 0, 1};
                        else if (timeMs > 4.0f)
                            color = {1, 1, 0, 1};

                        lines.push_back({name, buf, color});
                    }
                }

                if (m_sectionVisible[static_cast<int>(OverlaySection::Camera)])
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Pos: (%.1f, %.1f, %.1f) Rot: (%.1f, %.1f, %.1f)", m_cameraPos.x,
                             m_cameraPos.y, m_cameraPos.z, m_cameraRot.x, m_cameraRot.y, m_cameraRot.z);
                    lines.push_back({"Camera", buf, {0.7f, 0.7f, 1.0f, 1.0f}});
                }

                if (m_sectionVisible[static_cast<int>(OverlaySection::Physics)])
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Bodies: %d", m_physicsBodies);
                    lines.push_back({"Physics", buf, {1, 1, 1, 1}});
                }

                if (m_sectionVisible[static_cast<int>(OverlaySection::Audio)])
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Channels: %d", m_audioChannels);
                    lines.push_back({"Audio", buf, {1, 1, 1, 1}});
                }

                if (m_sectionVisible[static_cast<int>(OverlaySection::Custom)])
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (const auto& [key, stat] : m_customLines)
                    {
                        lines.push_back(stat);
                    }
                }
            }

            return lines;
        }

      private:
        DebugOverlay()
        {
            m_sectionVisible.fill(true);
            m_frameHistory.fill({});
        }
        DebugOverlay(const DebugOverlay&) = delete;
        DebugOverlay& operator=(const DebugOverlay&) = delete;

        bool m_enabled = false;
        bool m_compactMode = false;
        OverlayPosition m_position = OverlayPosition::TopLeft;
        float m_opacity = 0.8f;

        // Section visibility
        std::array<bool, static_cast<int>(OverlaySection::Count)> m_sectionVisible;

        // FPS tracking
        float m_currentFPS = 0.0f;
        float m_fpsAccumulator = 0.0f;
        int m_fpsSampleCount = 0;
        uint64_t m_frameCount = 0;

        // Frame time stats
        float m_avgFrameTime = 0.0f;
        float m_minFrameTime = 0.0f;
        float m_maxFrameTime = 0.0f;

        // Frame history for graph
        std::array<FrameTimeSample, FRAME_HISTORY_SIZE> m_frameHistory;
        int m_frameHistoryIndex = 0;

        // Subsystem stats
        int m_drawCalls = 0;
        int m_triangles = 0;
        int m_entities = 0;
        int m_visibleEntities = 0;
        float m_memoryUsageMB = 0.0f;
        float m_vramUsageMB = 0.0f;
        int m_physicsBodies = 0;
        int m_audioChannels = 0;

        DirectX::XMFLOAT3 m_cameraPos = {0, 0, 0};
        DirectX::XMFLOAT3 m_cameraRot = {0, 0, 0};

        // System timings
        std::unordered_map<std::string, float> m_systemTimings;

        // Custom lines
        std::unordered_map<std::string, OverlayStat> m_customLines;

        mutable std::mutex m_mutex;
    };

} // namespace Spark
