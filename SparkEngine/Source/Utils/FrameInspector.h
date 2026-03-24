/**
 * @file FrameInspector.h
 * @brief Frame stepping and state inspection tool
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides the ability to pause the game loop, step through frames one at a time,
 * and inspect engine state at each frame. Essential for debugging physics glitches,
 * animation issues, and timing-dependent bugs.
 *
 * Features:
 * - Pause/resume game execution
 * - Single-frame stepping (advance one frame at a time)
 * - Multi-frame stepping (advance N frames then pause)
 * - Frame state snapshots for comparison
 * - Slow-motion mode (adjustable time scale)
 * - Breakpoint frames (pause at specific frame numbers)
 * - Conditional frame breaks (pause when a condition is true)
 * - State snapshot comparison between frames
 *
 * Typical usage:
 * @code
 *   auto& inspector = Spark::FrameInspector::GetInstance();
 *
 *   // In the game loop:
 *   if (inspector.ShouldAdvance()) {
 *       UpdateGame(inspector.GetEffectiveDeltaTime(rawDeltaTime));
 *       inspector.OnFrameEnd();
 *   }
 *
 *   // From console or keybinding:
 *   inspector.Pause();
 *   inspector.StepFrame();     // advance exactly one frame
 *   inspector.Resume();
 * @endcode
 *
 * @see Profiler.h, SparkConsole.h
 */

#pragma once

#include "../Core/Platform.h"
#include "ContainerUtils.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <cstdint>

namespace Spark
{

    /**
 * @brief Frame execution state
 */
    enum class FrameState
    {
        Running,   ///< Normal execution
        Paused,    ///< Execution paused
        Stepping,  ///< Advancing a fixed number of frames then pausing
        SlowMotion ///< Running at reduced time scale
    };

    /**
 * @brief Snapshot of key engine state at a specific frame
 */
    struct FrameSnapshot
    {
        uint64_t frameNumber = 0;
        float deltaTime = 0.0f;
        float totalTime = 0.0f;

        // Customizable state entries
        std::unordered_map<std::string, std::string> stateEntries;

        /**
     * @brief Compare this snapshot with another and return differences
     */
        std::vector<std::string> Diff(const FrameSnapshot& other) const
        {
            std::vector<std::string> diffs;

            if (frameNumber != other.frameNumber)
                diffs.push_back("frameNumber: " + std::to_string(frameNumber) + " -> " +
                                std::to_string(other.frameNumber));

            for (const auto& [key, val] : stateEntries)
            {
                auto it = other.stateEntries.find(key);
                if (it == other.stateEntries.end())
                    diffs.push_back(key + ": " + val + " -> (removed)");
                else if (it->second != val)
                    diffs.push_back(key + ": " + val + " -> " + it->second);
            }

            for (const auto& [key, val] : other.stateEntries)
            {
                if (!Spark::ContainerUtils::Contains(stateEntries, key))
                    diffs.push_back(key + ": (new) -> " + val);
            }

            return diffs;
        }
    };

    /**
 * @brief Conditional break predicate
 */
    struct FrameBreakCondition
    {
        std::string name;                ///< Display name for this condition
        std::function<bool()> predicate; ///< Returns true when the break should trigger
        bool enabled = true;             ///< Whether this condition is active
        bool oneShot = false;            ///< If true, auto-disables after triggering
    };

    /**
 * @brief Singleton frame inspector for debugging frame-by-frame
 */
    class FrameInspector
    {
      public:
        static constexpr int MAX_SNAPSHOTS = 60;

        static FrameInspector& GetInstance()
        {
            static FrameInspector instance;
            return instance;
        }

        // ========================================================================
        // Frame control
        // ========================================================================

        /** @brief Pause game execution */
        void Pause()
        {
            m_state = FrameState::Paused;
            m_stepCount = 0;
        }

        /** @brief Resume normal execution */
        void Resume()
        {
            m_state = FrameState::Running;
            m_timeScale = 1.0f;
            m_stepCount = 0;
        }

        /** @brief Advance exactly one frame, then pause */
        void StepFrame()
        {
            m_state = FrameState::Stepping;
            m_stepCount = 1;
        }

        /** @brief Advance N frames, then pause */
        void StepFrames(int count)
        {
            m_state = FrameState::Stepping;
            m_stepCount = count;
        }

        /** @brief Toggle between paused and running */
        void TogglePause()
        {
            if (m_state == FrameState::Paused)
                Resume();
            else
                Pause();
        }

        /**
     * @brief Set slow-motion time scale
     * @param scale Time multiplier (0.1 = 10x slower, 0.5 = half speed)
     */
        void SetTimeScale(float scale)
        {
            m_timeScale = scale;
            if (scale < 1.0f && m_state == FrameState::Running)
                m_state = FrameState::SlowMotion;
            else if (scale >= 1.0f && m_state == FrameState::SlowMotion)
                m_state = FrameState::Running;
        }

        float GetTimeScale() const { return m_timeScale; }

        // ========================================================================
        // Query state (call from game loop)
        // ========================================================================

        /**
     * @brief Check if the game should advance this frame
     * @return true if the frame should be processed
     */
        bool ShouldAdvance()
        {
            switch (m_state)
            {
            case FrameState::Running:
            case FrameState::SlowMotion:
                return true;

            case FrameState::Paused:
                return false;

            case FrameState::Stepping:
                if (m_stepCount > 0)
                    return true;
                m_state = FrameState::Paused;
                return false;
            }
            return true;
        }

        /**
     * @brief Get effective delta time (applies time scale)
     * @param rawDeltaTime Unmodified frame delta time
     */
        float GetEffectiveDeltaTime(float rawDeltaTime) const { return rawDeltaTime * m_timeScale; }

        /**
     * @brief Call at the end of each processed frame
     */
        void OnFrameEnd()
        {
            m_frameNumber++;

            if (m_state == FrameState::Stepping)
            {
                m_stepCount--;
                if (m_stepCount <= 0)
                    m_state = FrameState::Paused;
            }

            // Check breakpoint frames
            for (uint64_t bp : m_breakpointFrames)
            {
                if (m_frameNumber == bp)
                {
                    m_state = FrameState::Paused;
                    break;
                }
            }

            // Check conditional breaks
            for (auto& cond : m_breakConditions)
            {
                if (cond.enabled && cond.predicate && cond.predicate())
                {
                    m_state = FrameState::Paused;
                    if (cond.oneShot)
                        cond.enabled = false;
                    break;
                }
            }
        }

        // ========================================================================
        // State inspection
        // ========================================================================

        FrameState GetState() const { return m_state; }
        uint64_t GetFrameNumber() const { return m_frameNumber; }
        bool IsPaused() const { return m_state == FrameState::Paused; }

        const char* GetStateString() const
        {
            switch (m_state)
            {
            case FrameState::Running:
                return "Running";
            case FrameState::Paused:
                return "Paused";
            case FrameState::Stepping:
                return "Stepping";
            case FrameState::SlowMotion:
                return "SlowMotion";
            default:
                return "Unknown";
            }
        }

        // ========================================================================
        // Snapshots
        // ========================================================================

        /**
     * @brief Capture a snapshot of current state
     * @param deltaTime Current frame's delta time
     * @param totalTime Total elapsed time
     */
        void CaptureSnapshot(float deltaTime, float totalTime)
        {
            FrameSnapshot snap;
            snap.frameNumber = m_frameNumber;
            snap.deltaTime = deltaTime;
            snap.totalTime = totalTime;

            // Collect registered state providers
            for (const auto& [key, getter] : m_stateProviders)
            {
                snap.stateEntries[key] = getter();
            }

            m_snapshots.push_back(std::move(snap));
            if (static_cast<int>(m_snapshots.size()) > MAX_SNAPSHOTS)
                m_snapshots.erase(m_snapshots.begin());
        }

        /**
     * @brief Register a state provider for snapshots
     * @param key   State key name
     * @param getter Function returning the current state as string
     */
        void RegisterStateProvider(const std::string& key, std::function<std::string()> getter)
        {
            m_stateProviders[key] = std::move(getter);
        }

        /** @brief Remove a state provider */
        void RemoveStateProvider(const std::string& key) { m_stateProviders.erase(key); }

        /** @brief Get the last N snapshots */
        const std::vector<FrameSnapshot>& GetSnapshots() const { return m_snapshots; }

        /** @brief Clear all snapshots */
        void ClearSnapshots() { m_snapshots.clear(); }

        /**
     * @brief Compare two snapshots and return differences
     * @param index1 Index into snapshot history
     * @param index2 Index into snapshot history
     */
        std::vector<std::string> CompareSnapshots(int index1, int index2) const
        {
            if (index1 < 0 || index1 >= static_cast<int>(m_snapshots.size()) || index2 < 0 ||
                index2 >= static_cast<int>(m_snapshots.size()))
            {
                return {"Invalid snapshot indices"};
            }
            return m_snapshots[index1].Diff(m_snapshots[index2]);
        }

        // ========================================================================
        // Breakpoints
        // ========================================================================

        /** @brief Set a breakpoint at a specific frame number */
        void AddBreakpointFrame(uint64_t frame) { m_breakpointFrames.push_back(frame); }

        /** @brief Remove a frame breakpoint */
        void RemoveBreakpointFrame(uint64_t frame)
        {
            auto it = std::find(m_breakpointFrames.begin(), m_breakpointFrames.end(), frame);
            if (it != m_breakpointFrames.end())
                m_breakpointFrames.erase(it);
        }

        /** @brief Clear all frame breakpoints */
        void ClearBreakpointFrames() { m_breakpointFrames.clear(); }

        /**
     * @brief Add a conditional break that triggers when predicate returns true
     * @param name     Display name
     * @param predicate Condition to check each frame
     * @param oneShot   If true, disable after first trigger
     */
        void AddBreakCondition(const std::string& name, std::function<bool()> predicate, bool oneShot = false)
        {
            m_breakConditions.push_back({name, std::move(predicate), true, oneShot});
        }

        /** @brief Remove a break condition by name */
        void RemoveBreakCondition(const std::string& name)
        {
            auto it = std::find_if(m_breakConditions.begin(), m_breakConditions.end(),
                                   [&](const FrameBreakCondition& c) { return c.name == name; });
            if (it != m_breakConditions.end())
                m_breakConditions.erase(it);
        }

        /** @brief Clear all break conditions */
        void ClearBreakConditions() { m_breakConditions.clear(); }

        /** @brief Get all break conditions (for UI display) */
        const std::vector<FrameBreakCondition>& GetBreakConditions() const { return m_breakConditions; }

      private:
        FrameInspector() = default;
        FrameInspector(const FrameInspector&) = delete;
        FrameInspector& operator=(const FrameInspector&) = delete;

        FrameState m_state = FrameState::Running;
        uint64_t m_frameNumber = 0;
        float m_timeScale = 1.0f;
        int m_stepCount = 0;

        // Snapshots
        std::vector<FrameSnapshot> m_snapshots;
        std::unordered_map<std::string, std::function<std::string()>> m_stateProviders;

        // Breakpoints
        std::vector<uint64_t> m_breakpointFrames;
        std::vector<FrameBreakCondition> m_breakConditions;
    };

} // namespace Spark
