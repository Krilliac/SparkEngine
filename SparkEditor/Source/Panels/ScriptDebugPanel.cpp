/**
 * @file ScriptDebugPanel.cpp
 * @brief Visual script debugger panel implementation
 */

#include "ScriptDebugPanel.h"

#include <imgui.h>
#include <algorithm>
#include <cstring>

namespace SparkEditor
{

    // Static instance for the trace callback
    static ScriptDebugPanel* s_activeDebugPanel = nullptr;

    static void DebugTraceHandler(uint32_t nodeId, const char* nodeName, const char* output)
    {
        if (s_activeDebugPanel)
            s_activeDebugPanel->AddTraceEntry(nodeId, nodeName ? nodeName : "", "script", output ? output : "");
    }

    ScriptDebugPanel::ScriptDebugPanel() : EditorPanel("Script Debugger", "ScriptDebugger") {}

    bool ScriptDebugPanel::Initialize()
    {
        s_activeDebugPanel = this;
        ASSetDebugTraceCallback(DebugTraceHandler);
        m_isInitialized = true;
        return true;
    }

    void ScriptDebugPanel::Update(float deltaTime)
    {
        if (!m_isPaused)
            m_elapsedTime += deltaTime;
    }

    void ScriptDebugPanel::Render()
    {
        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderToolbar();
        ImGui::Separator();

        // Three-section layout
        if (ImGui::BeginTabBar("DebugTabs"))
        {
            if (ImGui::BeginTabItem("Breakpoints"))
            {
                RenderBreakpoints();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Watch"))
            {
                RenderWatchVariables();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Trace Log"))
            {
                RenderTraceLog();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Call Stack"))
            {
                RenderCallStack();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        EndPanel();
    }

    void ScriptDebugPanel::Shutdown() {}

    void ScriptDebugPanel::RenderToolbar()
    {
        if (m_isPaused)
        {
            if (ImGui::Button("Continue"))
                Continue();
            ImGui::SameLine();
            if (ImGui::Button("Step Over"))
                StepOver();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "PAUSED at Node %u", m_pausedAtNode);
        }
        else
        {
            if (ImGui::Button("Pause"))
                m_isPaused = true;
        }

        ImGui::SameLine();
        ImGui::Checkbox("Debug Compile", &m_debugCompile);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When enabled, compiled scripts emit trace calls at each node");

        ImGui::SameLine();
        if (ImGui::Button("Clear Trace"))
            m_traceLog.clear();

        ImGui::SameLine();
        ImGui::Text("Breakpoints: %zu | Watches: %zu | Trace: %zu", m_breakpoints.size(), m_watches.size(),
                    m_traceLog.size());
    }

    void ScriptDebugPanel::RenderBreakpoints()
    {
        for (size_t i = 0; i < m_breakpoints.size(); i++)
        {
            auto& bp = m_breakpoints[i];
            ImGui::PushID(static_cast<int>(i));

            ImGui::Checkbox("##en", &bp.isEnabled);
            ImGui::SameLine();

            if (bp.isHit)
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Node %u [%s] (hit %u times)", bp.nodeId,
                                   bp.scriptName.c_str(), bp.hitCount);
            else
                ImGui::Text("Node %u [%s]", bp.nodeId, bp.scriptName.c_str());

            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
            {
                m_breakpoints.erase(m_breakpoints.begin() + static_cast<ptrdiff_t>(i));
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }

        if (m_breakpoints.empty())
            ImGui::TextDisabled("No breakpoints set. Click on a node pin in the Visual Script editor.");

        if (ImGui::Button("Clear All"))
            ClearAllBreakpoints();
    }

    void ScriptDebugPanel::RenderWatchVariables()
    {
        // Add watch input
        static char watchName[64] = "";
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputText("##watch", watchName, sizeof(watchName));
        ImGui::SameLine();
        if (ImGui::Button("Add Watch") && watchName[0] != '\0')
        {
            AddWatch(watchName, "auto");
            watchName[0] = '\0';
        }

        ImGui::Separator();

        // Watch list
        ImGui::Columns(3, "WatchCols");
        ImGui::SetColumnWidth(0, 120.0f);
        ImGui::SetColumnWidth(1, 60.0f);
        ImGui::Text("Name");
        ImGui::NextColumn();
        ImGui::Text("Type");
        ImGui::NextColumn();
        ImGui::Text("Value");
        ImGui::NextColumn();
        ImGui::Separator();

        for (size_t i = 0; i < m_watches.size(); i++)
        {
            auto& w = m_watches[i];
            ImGui::PushID(static_cast<int>(i));

            ImGui::Text("%s", w.name.c_str());
            ImGui::NextColumn();
            ImGui::TextDisabled("%s", w.type.c_str());
            ImGui::NextColumn();

            if (w.currentValue.empty())
                ImGui::TextDisabled("(not set)");
            else
                ImGui::Text("%s", w.currentValue.c_str());

            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
            {
                m_watches.erase(m_watches.begin() + static_cast<ptrdiff_t>(i));
                ImGui::PopID();
                ImGui::NextColumn();
                break;
            }
            ImGui::NextColumn();

            ImGui::PopID();
        }
        ImGui::Columns(1);
    }

    void ScriptDebugPanel::RenderTraceLog()
    {
        ImGui::Text("Execution Trace (%zu entries)", m_traceLog.size());
        ImGui::Separator();

        ImGui::BeginChild("TraceScroll", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        // Show most recent first
        for (auto it = m_traceLog.rbegin(); it != m_traceLog.rend(); ++it)
        {
            ImGui::TextDisabled("[%.2fs]", it->timestamp);
            ImGui::SameLine();
            ImGui::Text("Node %u (%s): %s", it->nodeId, it->nodeName.c_str(), it->output.c_str());
        }

        if (m_traceLog.empty())
            ImGui::TextDisabled("No trace data. Enable 'Debug Compile' and run a script.");

        ImGui::EndChild();
    }

    // ========================================================================
    // Public API
    // ========================================================================

    void ScriptDebugPanel::ToggleBreakpoint(uint32_t nodeId, const std::string& scriptName)
    {
        auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                               [nodeId](const ScriptBreakpoint& bp) { return bp.nodeId == nodeId; });
        if (it != m_breakpoints.end())
        {
            m_breakpoints.erase(it);
        }
        else
        {
            ScriptBreakpoint bp;
            bp.nodeId = nodeId;
            bp.scriptName = scriptName;
            m_breakpoints.push_back(bp);
        }
    }

    bool ScriptDebugPanel::HasBreakpoint(uint32_t nodeId) const
    {
        return std::any_of(m_breakpoints.begin(), m_breakpoints.end(),
                           [nodeId](const ScriptBreakpoint& bp) { return bp.nodeId == nodeId && bp.isEnabled; });
    }

    void ScriptDebugPanel::ClearAllBreakpoints()
    {
        m_breakpoints.clear();
    }

    void ScriptDebugPanel::AddTraceEntry(uint32_t nodeId, const std::string& nodeName, const std::string& scriptName,
                                         const std::string& output)
    {
        TraceEntry entry;
        entry.nodeId = nodeId;
        entry.nodeName = nodeName;
        entry.scriptName = scriptName;
        entry.timestamp = m_elapsedTime;
        entry.output = output;
        m_traceLog.push_back(entry);

        // Trim old entries
        if (m_traceLog.size() > m_maxTraceEntries)
            m_traceLog.erase(m_traceLog.begin());

        // Step mode: pause after each node
        if (m_stepMode)
        {
            m_isPaused = true;
            m_stepMode = false;
            m_pausedAtNode = nodeId;
        }

        // Check breakpoints
        for (auto& bp : m_breakpoints)
        {
            if (bp.nodeId == nodeId && bp.isEnabled)
            {
                bp.isHit = true;
                bp.hitCount++;
                m_isPaused = true;
                m_pausedAtNode = nodeId;
            }
        }
    }

    void ScriptDebugPanel::AddWatch(const std::string& name, const std::string& type)
    {
        // Don't add duplicates
        for (const auto& w : m_watches)
        {
            if (w.name == name)
                return;
        }

        WatchVariable w;
        w.name = name;
        w.type = type;
        m_watches.push_back(w);
    }

    void ScriptDebugPanel::UpdateWatch(const std::string& name, const std::string& value)
    {
        for (auto& w : m_watches)
        {
            if (w.name == name)
            {
                w.currentValue = value;
                return;
            }
        }
    }

    void ScriptDebugPanel::RemoveWatch(const std::string& name)
    {
        std::erase_if(m_watches, [&name](const WatchVariable& w) { return w.name == name; });
    }

    void ScriptDebugPanel::StepOver()
    {
        // Allow one trace entry, then pause again
        m_stepMode = true;
        m_isPaused = false;
    }

    void ScriptDebugPanel::Continue()
    {
        m_stepMode = false;
        m_isPaused = false;
        m_pausedAtNode = 0;
    }

    void ScriptDebugPanel::RenderCallStack()
    {
        ImGui::Text("Execution Call Stack");
        ImGui::Separator();

        if (m_traceLog.empty())
        {
            ImGui::TextDisabled("No execution data");
            return;
        }

        // Show recent trace as a call stack (most recent at top)
        size_t start = m_traceLog.size() > 20 ? m_traceLog.size() - 20 : 0;
        for (size_t i = m_traceLog.size(); i > start; i--)
        {
            const auto& entry = m_traceLog[i - 1];
            bool isTop = (i == m_traceLog.size());
            if (isTop && m_isPaused)
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "-> Node %u (%s)", entry.nodeId,
                                   entry.nodeName.c_str());
            else
                ImGui::Text("   Node %u (%s)", entry.nodeId, entry.nodeName.c_str());
        }
    }

} // namespace SparkEditor
