/**
 * @file ScriptDebugPanel.h
 * @brief Visual script debugger panel with breakpoints, watch variables, and trace
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Engine/Scripting/VisualScriptCompiler.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SparkEditor
{

    /// @brief A breakpoint set on a visual script node
    struct ScriptBreakpoint
    {
        uint32_t nodeId = 0;
        std::string scriptName;
        bool isEnabled = true;
        bool isHit = false;
        uint32_t hitCount = 0;
    };

    /// @brief A variable being watched in the debugger
    struct WatchVariable
    {
        std::string name;
        std::string type;
        std::string currentValue;
        bool isExpanded = false; ///< For Vector3 and compound types
    };

    /// @brief An entry in the execution trace log
    struct TraceEntry
    {
        uint32_t nodeId = 0;
        std::string nodeName;
        std::string scriptName;
        float timestamp = 0.0f;
        std::string output; ///< Generated output or side effect
    };

    /**
     * @brief Debugger panel for visual scripts
     *
     * Provides breakpoint management, variable watch, execution trace,
     * and compile-time debug instrumentation for visual script graphs.
     */
    class ScriptDebugPanel : public EditorPanel
    {
      public:
        ScriptDebugPanel();
        ~ScriptDebugPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "ScriptDebugPanel"; }

        // Breakpoint management (called by VisualScriptPanel)
        void ToggleBreakpoint(uint32_t nodeId, const std::string& scriptName);
        bool HasBreakpoint(uint32_t nodeId) const;
        void ClearAllBreakpoints();

        // Trace log
        void AddTraceEntry(uint32_t nodeId, const std::string& nodeName, const std::string& scriptName,
                           const std::string& output);

        // Watch management
        void AddWatch(const std::string& name, const std::string& type);
        void UpdateWatch(const std::string& name, const std::string& value);
        void RemoveWatch(const std::string& name);

        // Debug compilation flag
        bool IsDebugCompileEnabled() const { return m_debugCompile; }

        // Step-through control
        bool IsPaused() const { return m_isPaused; }
        void StepOver(); ///< Execute one node, then pause again
        void Continue(); ///< Resume until next breakpoint

      private:
        void RenderBreakpoints();
        void RenderWatchVariables();
        void RenderTraceLog();
        void RenderToolbar();
        void RenderCallStack();

        std::vector<ScriptBreakpoint> m_breakpoints;
        std::vector<WatchVariable> m_watches;
        std::vector<TraceEntry> m_traceLog;
        bool m_debugCompile = false; ///< When true, compiler inserts trace calls
        bool m_isPaused = false;
        bool m_stepMode = false;     ///< Step one node then pause
        uint32_t m_pausedAtNode = 0; ///< Node ID where execution is paused
        float m_elapsedTime = 0.0f;
        size_t m_maxTraceEntries = 500;
    };

} // namespace SparkEditor
