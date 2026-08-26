/**
 * @file PlayControlPanel.cpp
 * @brief Out-of-process play-testing controls implementation.
 *
 * Contains: lifecycle (Initialize/Update/Render/Shutdown) and ImGui rendering
 * (RenderPlayBar, RenderQuickConnect, RenderStatus, RoleLabel). Launch actions
 * and instance bookkeeping live in PlayControlLaunch.cpp.
 */

#include "PlayControlPanel.h"
#include "GameModuleSelectorPanel.h"
#include "Core/ProjectManager.h"
#include "../Utils/EditorLaunchContext.h"
#include "../Utils/EditorProcessLaunch.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <algorithm>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace SparkEditor
{

    PlayControlPanel::PlayControlPanel() : EditorPanel("Play Control", "PlayControl") {}

    PlayControlPanel::~PlayControlPanel() = default;

    bool PlayControlPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayControlPanel initialized");
        m_isInitialized = true;
        const std::filesystem::path initialLog = LaunchContext::ResolveContextFile(
            ProjectManager::GetActiveProjectPath(), GetEditorExecutableDirectory(), "exec_audit.log");
        m_logPath = initialLog;
        return true;
    }

    void PlayControlPanel::Update(float deltaTime)
    {
        PollInstances();

        // Project selection can change after panels are initialized. When no
        // process is alive, keep the displayed audit tail aligned with the next
        // launch context; active launches retain the directory they started in.
        const bool hasLiveInstance = std::any_of(m_instances.begin(), m_instances.end(),
                                                 [](const RunningInstance& instance) { return instance.alive; });
        if (!hasLiveInstance)
        {
            const std::filesystem::path editorDirectory(GetEditorExecutableDirectory());
            const std::filesystem::path modulePath =
                m_gameModuleSelector ? LaunchContext::PathFromUtf8(m_gameModuleSelector->GetLaunchSelectionPath())
                                     : std::filesystem::path{};
#ifdef _WIN32
            const std::filesystem::path engineExecutable = editorDirectory / "SparkEngine.exe";
#else
            const std::filesystem::path engineExecutable = editorDirectory / "SparkEngine";
#endif
            const std::filesystem::path workingDirectory = LaunchContext::ResolveWorkingDirectory(
                LaunchContext::PathFromUtf8(ProjectManager::GetActiveProjectPath()), modulePath, engineExecutable);
            if (!workingDirectory.empty())
                SetLaunchContextDirectory(workingDirectory);
        }

        m_logPollTimer += deltaTime;
        if (m_logPollTimer >= 0.5f)
        {
            m_logPollTimer = 0.0f;
            RefreshLogTail();
        }
    }

    void PlayControlPanel::Render()
    {
        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        ImGui::TextWrapped("Launch and monitor real, out-of-process SparkEngine play-test instances. "
                           "In-editor play is not offered — see Game Module Selector for why.");
        ImGui::Separator();

        RenderPlayBar();
        ImGui::Separator();
        RenderQuickConnect();
        ImGui::Separator();
        RenderStatus();

        EndPanel();
    }

    void PlayControlPanel::Shutdown()
    {
        // Do NOT terminate tracked instances on editor shutdown — they are
        // independent processes, same policy as GameModuleSelectorPanel.
        for (auto& inst : m_instances)
        {
            if (inst.processHandle)
                CloseEditorProcessHandles(inst.processHandle, nullptr);
            inst.processHandle = nullptr;
        }
        m_instances.clear();
    }

    const char* PlayControlPanel::RoleLabel(InstanceRole role)
    {
        switch (role)
        {
        case InstanceRole::Game:
            return "Game";
        case InstanceRole::Dedicated:
            return "Dedicated";
        case InstanceRole::Client:
            return "Client";
        default:
            return "?";
        }
    }

    // ============================================================================
    // Play bar
    // ============================================================================

    void PlayControlPanel::RenderPlayBar()
    {
        ImGui::TextUnformatted("Play");

        const bool hasSelector = m_gameModuleSelector != nullptr;
        const std::string selectedPath = hasSelector ? m_gameModuleSelector->GetLaunchSelectionPath() : std::string();
        const bool hasModule = !selectedPath.empty();

        if (!hasSelector)
            ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.3f, 1.0f),
                               "Game Module Selector not wired — cannot determine which module to launch.");
        else if (!hasModule)
            ImGui::TextDisabled(
                "Select a module in the Game Module Selector panel (radio button) to enable launching.");
        else
            ImGui::Text("Module: %s",
                        LaunchContext::PathToUtf8(LaunchContext::PathFromUtf8(selectedPath).stem()).c_str());

        ImGui::BeginDisabled(!hasModule);

        if (ImGui::Button("Launch Game"))
            LaunchGame();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Windowed client, no scripted -exec.");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::SliderInt("bots##DedicatedBots", &m_botCount, 0, 32);
        ImGui::SameLine();
        if (ImGui::Button("Launch Dedicated + Bots"))
            LaunchDedicatedWithBots();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Headless server. Writes a temp -exec cfg that runs\n"
                                   "'tf_dedicated' then 'tf_chaos <N>' at boot.");
            ImGui::EndTooltip();
        }

        ImGui::EndDisabled();

        if (!m_statusMessage.empty())
            ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "%s", m_statusMessage.c_str());
    }

    void PlayControlPanel::RenderQuickConnect()
    {
        ImGui::TextUnformatted("Quick Connect");

        const bool hasSelector = m_gameModuleSelector != nullptr;
        const bool hasModule = hasSelector && !m_gameModuleSelector->GetLaunchSelectionPath().empty();

        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("host:port", m_quickConnectBuf, sizeof(m_quickConnectBuf));

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasModule || m_quickConnectBuf[0] == '\0');
        if (ImGui::Button("Launch Client (Connecting-To)"))
            LaunchClient(m_quickConnectBuf);
        ImGui::EndDisabled();
    }

    void PlayControlPanel::RenderStatus()
    {
        ImGui::TextUnformatted("Status");

        ImVec4 stopColor(0.65f, 0.15f, 0.15f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, stopColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("STOP ALL", ImVec2(120, 32)))
            StopAll();
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        size_t aliveCount = 0;
        for (const auto& inst : m_instances)
            if (inst.alive)
                ++aliveCount;
        ImGui::Text("%zu running / %zu tracked", aliveCount, m_instances.size());

        if (ImGui::BeginTable("##Instances", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, 0)))
        {
            ImGui::TableSetupColumn("PID");
            ImGui::TableSetupColumn("Role");
            ImGui::TableSetupColumn("Label");
            ImGui::TableSetupColumn("Alive");
            ImGui::TableHeadersRow();

            for (const auto& inst : m_instances)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%lu", inst.pid);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(RoleLabel(inst.role));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(inst.label.c_str());
                ImGui::TableSetColumnIndex(3);
                if (inst.alive)
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "running");
                else
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "exited (%lu)", inst.exitCode);
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("exec_audit.log (tail)");
        ImGui::BeginChild("##ExecAuditLog", ImVec2(0, 220), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& line : m_logLines)
            ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

} // namespace SparkEditor
