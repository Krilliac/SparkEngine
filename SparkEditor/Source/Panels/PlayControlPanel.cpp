/**
 * @file PlayControlPanel.cpp
 * @brief Out-of-process play-testing controls implementation.
 */

#include "PlayControlPanel.h"
#include "GameModuleSelectorPanel.h"
#include "../Utils/EditorProcessLaunch.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"
#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
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
        m_logPath = (std::filesystem::path(GetExecutableDirectory()) / "exec_audit.log").string();
        return true;
    }

    void PlayControlPanel::Update(float deltaTime)
    {
        PollInstances();

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

        ImGui::TextWrapped("Launch and monitor real, out-of-process SparkEngine.exe play-test instances. "
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
#ifdef _WIN32
            if (inst.processHandle)
                CloseHandle(static_cast<HANDLE>(inst.processHandle));
#endif
            inst.processHandle = nullptr;
        }
        m_instances.clear();
    }

    std::string PlayControlPanel::GetExecutableDirectory()
    {
#ifdef _WIN32
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        return std::filesystem::path(exePath).parent_path().string();
#else
        return std::filesystem::canonical("/proc/self/exe").parent_path().string();
#endif
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
            ImGui::Text("Module: %s", std::filesystem::path(selectedPath).stem().string().c_str());

#ifdef _WIN32
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
#else
        ImGui::TextDisabled("Process launch is available on Windows builds only.");
#endif

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

#ifdef _WIN32
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasModule || m_quickConnectBuf[0] == '\0');
        if (ImGui::Button("Launch Client (Connecting-To)"))
            LaunchClient(m_quickConnectBuf);
        ImGui::EndDisabled();
#endif
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

    // ============================================================================
    // Launch actions
    // ============================================================================

    void PlayControlPanel::LaunchGame()
    {
#ifdef _WIN32
        if (!m_gameModuleSelector || m_gameModuleSelector->GetLaunchSelectionPath().empty())
        {
            m_statusMessage = "No module selected in Game Module Selector";
            return;
        }

        namespace fs = std::filesystem;
        const fs::path exeDir = fs::path(GetExecutableDirectory());
        const fs::path engineExe = exeDir / "SparkEngine.exe";
        const fs::path dll = fs::path(m_gameModuleSelector->GetLaunchSelectionPath());

        std::error_code ec;
        if (!fs::exists(engineExe, ec) || ec)
        {
            m_statusMessage = "SparkEngine.exe not found next to the editor (" + exeDir.string() + ")";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }

        std::string buildError;
        const std::wstring cmd = BuildGameLaunchCommandLine(engineExe, dll, false, {}, L"", buildError);
        if (cmd.empty() && !buildError.empty())
        {
            m_statusMessage = buildError;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }

        const ProcessLaunchResult launch = LaunchEditorProcess(engineExe, cmd, exeDir);
        if (!launch.success)
        {
            m_statusMessage = launch.error;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            Spark::SimpleConsole::GetInstance().LogError("[Editor] " + m_statusMessage);
            return;
        }

        RunningInstance inst;
        inst.processHandle = launch.processHandle;
        inst.pid = launch.pid;
        inst.role = InstanceRole::Game;
        inst.label = dll.stem().string();
        m_instances.push_back(inst);

        m_statusMessage = "Game running — " + inst.label + ", PID " + std::to_string(inst.pid);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
        Spark::SimpleConsole::GetInstance().LogSuccess("[Editor] " + m_statusMessage);
#endif
    }

    void PlayControlPanel::LaunchDedicatedWithBots()
    {
#ifdef _WIN32
        if (!m_gameModuleSelector || m_gameModuleSelector->GetLaunchSelectionPath().empty())
        {
            m_statusMessage = "No module selected in Game Module Selector";
            return;
        }

        namespace fs = std::filesystem;
        const fs::path exeDir = fs::path(GetExecutableDirectory());
        const fs::path engineExe = exeDir / "SparkEngine.exe";
        const fs::path dll = fs::path(m_gameModuleSelector->GetLaunchSelectionPath());

        std::error_code ec;
        if (!fs::exists(engineExe, ec) || ec)
        {
            m_statusMessage = "SparkEngine.exe not found next to the editor (" + exeDir.string() + ")";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }

        const std::string cfgPath = WriteDedicatedBotsCfg(m_botCount);
        if (cfgPath.empty())
        {
            m_statusMessage = "Failed to write temp -exec cfg for dedicated+bots launch";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }

        std::string buildError;
        const std::wstring cmd = BuildGameLaunchCommandLine(engineExe, dll, true, fs::path(cfgPath), L"", buildError);
        if (cmd.empty() && !buildError.empty())
        {
            m_statusMessage = buildError;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }

        const ProcessLaunchResult launch = LaunchEditorProcess(engineExe, cmd, exeDir);
        if (!launch.success)
        {
            m_statusMessage = launch.error;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            Spark::SimpleConsole::GetInstance().LogError("[Editor] " + m_statusMessage);
            return;
        }

        RunningInstance inst;
        inst.processHandle = launch.processHandle;
        inst.pid = launch.pid;
        inst.role = InstanceRole::Dedicated;
        inst.label = dll.stem().string() + " (+" + std::to_string(m_botCount) + " bots)";
        m_instances.push_back(inst);

        m_statusMessage = "Dedicated running — " + inst.label + ", PID " + std::to_string(inst.pid);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
        Spark::SimpleConsole::GetInstance().LogSuccess("[Editor] " + m_statusMessage);
#endif
    }

    void PlayControlPanel::LaunchClient(const std::string& hostPort)
    {
#ifdef _WIN32
        if (!m_gameModuleSelector || m_gameModuleSelector->GetLaunchSelectionPath().empty())
        {
            m_statusMessage = "No module selected in Game Module Selector";
            return;
        }
        if (hostPort.empty())
        {
            m_statusMessage = "Quick Connect: host:port is empty";
            return;
        }

        namespace fs = std::filesystem;
        const fs::path exeDir = fs::path(GetExecutableDirectory());
        const fs::path engineExe = exeDir / "SparkEngine.exe";
        const fs::path dll = fs::path(m_gameModuleSelector->GetLaunchSelectionPath());

        std::error_code ec;
        if (!fs::exists(engineExe, ec) || ec)
        {
            m_statusMessage = "SparkEngine.exe not found next to the editor (" + exeDir.string() + ")";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }

        const std::string cfgPath = WriteConnectCfg(hostPort);
        if (cfgPath.empty())
        {
            m_statusMessage = "Failed to write temp -exec cfg for quick-connect launch";
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }

        std::string buildError;
        const std::wstring cmd = BuildGameLaunchCommandLine(engineExe, dll, false, fs::path(cfgPath), L"", buildError);
        if (cmd.empty() && !buildError.empty())
        {
            m_statusMessage = buildError;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }

        const ProcessLaunchResult launch = LaunchEditorProcess(engineExe, cmd, exeDir);
        if (!launch.success)
        {
            m_statusMessage = launch.error;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            Spark::SimpleConsole::GetInstance().LogError("[Editor] " + m_statusMessage);
            return;
        }

        RunningInstance inst;
        inst.processHandle = launch.processHandle;
        inst.pid = launch.pid;
        inst.role = InstanceRole::Client;
        inst.label = dll.stem().string() + " -> " + hostPort;
        m_instances.push_back(inst);

        m_statusMessage = "Client connecting to " + hostPort + " — PID " + std::to_string(inst.pid);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
        Spark::SimpleConsole::GetInstance().LogSuccess("[Editor] " + m_statusMessage);
#endif
    }

    void PlayControlPanel::StopAll()
    {
        size_t stopped = 0;
        for (auto& inst : m_instances)
        {
            if (inst.alive)
            {
                TerminateEditorProcess(inst.processHandle, 1);
                ++stopped;
            }
        }
        m_statusMessage = "Stop All: terminated " + std::to_string(stopped) + " instance(s)";
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
        Spark::SimpleConsole::GetInstance().LogWarning("[Editor] " + m_statusMessage);
    }

    void PlayControlPanel::PollInstances()
    {
#ifdef _WIN32
        for (auto& inst : m_instances)
        {
            if (!inst.alive)
                continue;
            unsigned long exitCode = 0;
            if (PollProcessExited(inst.processHandle, exitCode))
            {
                inst.alive = false;
                inst.exitCode = exitCode;
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "PlayControlPanel: PID %lu (%s) exited with code %lu",
                               inst.pid, inst.label.c_str(), exitCode);
            }
        }
#endif
    }

    // ============================================================================
    // exec_audit.log tail
    // ============================================================================

    void PlayControlPanel::RefreshLogTail()
    {
        std::error_code ec;
        const auto fileSize = std::filesystem::file_size(m_logPath, ec);
        if (ec)
            return; // file doesn't exist yet — nothing launched with -exec so far

        if (static_cast<long long>(fileSize) < m_logReadOffset)
        {
            // File was recreated/truncated since our last read — restart the tail.
            m_logReadOffset = 0;
            m_logLines.clear();
        }
        if (static_cast<long long>(fileSize) == m_logReadOffset)
            return; // no new bytes

        std::ifstream file(m_logPath, std::ios::binary);
        if (!file)
            return;
        file.seekg(m_logReadOffset, std::ios::beg);

        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            m_logLines.push_back(line);
        }
        m_logReadOffset = static_cast<long long>(fileSize);

        if (m_logLines.size() > kMaxLogLines)
            m_logLines.erase(m_logLines.begin(),
                             m_logLines.begin() + static_cast<long>(m_logLines.size() - kMaxLogLines));
    }

    std::string PlayControlPanel::WriteDedicatedBotsCfg(int botCount)
    {
        namespace fs = std::filesystem;
        const int clampedBots = std::clamp(botCount, 0, 32);

#ifdef _WIN32
        wchar_t tempDirBuf[MAX_PATH];
        const DWORD len = GetTempPathW(MAX_PATH, tempDirBuf);
        if (len == 0 || len >= MAX_PATH)
            return {};
        const fs::path tempDir(std::wstring(tempDirBuf, len));
#else
        const fs::path tempDir = fs::temp_directory_path();
#endif
        unsigned long ownPid = 0;
#ifdef _WIN32
        ownPid = GetCurrentProcessId();
#endif
        const fs::path cfgPath = tempDir / ("spark_playcontrol_dedicated_" + std::to_string(ownPid) + "_" +
                                            std::to_string(m_launchCounter) + ".cfg");
        ++m_launchCounter;

        std::ofstream out(cfgPath);
        if (!out)
            return {};
        out << "# Editor-generated (PlayControlPanel: Launch Dedicated + " << clampedBots << " bots)\n";
        out << "t1 tf_dedicated\n";
        out << "t2 tf_chaos " << clampedBots << " 3600\n";
        out.close();

        return cfgPath.string();
    }

    std::string PlayControlPanel::WriteConnectCfg(const std::string& hostPort)
    {
        namespace fs = std::filesystem;

#ifdef _WIN32
        wchar_t tempDirBuf[MAX_PATH];
        const DWORD len = GetTempPathW(MAX_PATH, tempDirBuf);
        if (len == 0 || len >= MAX_PATH)
            return {};
        const fs::path tempDir(std::wstring(tempDirBuf, len));
#else
        const fs::path tempDir = fs::temp_directory_path();
#endif
        unsigned long ownPid = 0;
#ifdef _WIN32
        ownPid = GetCurrentProcessId();
#endif
        const fs::path cfgPath = tempDir / ("spark_playcontrol_connect_" + std::to_string(ownPid) + "_" +
                                            std::to_string(m_launchCounter) + ".cfg");
        ++m_launchCounter;

        std::ofstream out(cfgPath);
        if (!out)
            return {};
        out << "# Editor-generated (PlayControlPanel: Quick Connect)\n";
        out << "t1 tf_connect " << hostPort << "\n";
        out.close();

        return cfgPath.string();
    }

} // namespace SparkEditor
