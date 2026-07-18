/**
 * @file PlayControlLaunch.cpp
 * @brief Launch actions and instance bookkeeping for the play control panel.
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains: LaunchGame, LaunchDedicatedWithBots, LaunchClient, StopAll,
 * PollInstances, RefreshLogTail, WriteDedicatedBotsCfg, WriteConnectCfg.
 */

#include "PlayControlPanel.h"
#include "GameModuleSelectorPanel.h"
#include "../Utils/EditorProcessLaunch.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace SparkEditor
{

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
        const fs::path dll = fs::path(m_gameModuleSelector->GetLaunchSelectionPath());

        fs::path engineExe;
        std::string findError;
        if (!FindEngineExecutable(engineExe, findError))
        {
            m_statusMessage = findError;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }
        const fs::path exeDir = engineExe.parent_path();

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
        const fs::path dll = fs::path(m_gameModuleSelector->GetLaunchSelectionPath());

        fs::path engineExe;
        std::string findError;
        if (!FindEngineExecutable(engineExe, findError))
        {
            m_statusMessage = findError;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }
        const fs::path exeDir = engineExe.parent_path();

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
        const fs::path dll = fs::path(m_gameModuleSelector->GetLaunchSelectionPath());

        fs::path engineExe;
        std::string findError;
        if (!FindEngineExecutable(engineExe, findError))
        {
            m_statusMessage = findError;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "PlayControlPanel: %s", m_statusMessage.c_str());
            return;
        }
        const fs::path exeDir = engineExe.parent_path();

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
