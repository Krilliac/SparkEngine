/**
 * @file DedicatedServerPanel.cpp
 * @brief Editor panel for dedicated server management, cooking, and PIE launching
 * @author Spark Engine Team
 * @date 2025
 */

#include "DedicatedServerPanel.h"
#include "BuildPipeline.h"
#include "DedicatedServerProcessController.h"
#include <algorithm>
#include <filesystem>
#include <iostream>

#include <imgui.h>

#include "../Core/EditorIcons.h"
#include "../Core/ProjectManager.h"
#include "../Utils/EditorProcessLaunch.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include "Utils/LogMacros.h"

namespace SparkEditor
{
    namespace
    {
        std::filesystem::path FindSparkServerExecutable()
        {
            const auto directory = std::filesystem::path(GetEditorExecutableDirectory());
#ifdef _WIN32
            constexpr std::string_view filename = "SparkServer.exe";
#else
            constexpr std::string_view filename = "SparkServer";
#endif
            const std::filesystem::path candidates[] = {directory / filename, directory.parent_path() / filename};
            std::error_code error;
            for (const auto& candidate : candidates)
            {
                if (std::filesystem::is_regular_file(candidate, error))
                    return candidate;
                error.clear();
            }
            return {};
        }

        BuildSettings MakeCookSettings(const DedicatedServerPanel::ServerCookSettings& server)
        {
            BuildSettings settings;
            settings.profile = server.profile == DedicatedServerPanel::ServerBuildProfile::Debug
                                   ? BuildCookPanel::BuildProfile::Debug
                               : server.profile == DedicatedServerPanel::ServerBuildProfile::Shipping
                                   ? BuildCookPanel::BuildProfile::Shipping
                                   : BuildCookPanel::BuildProfile::Development;
            switch (server.platform)
            {
            case DedicatedServerPanel::ServerPlatform::WindowsX64:
                settings.platform = BuildCookPanel::TargetPlatform::WindowsX64;
                break;
            case DedicatedServerPanel::ServerPlatform::LinuxX64:
                settings.platform = BuildCookPanel::TargetPlatform::LinuxX64;
                break;
            case DedicatedServerPanel::ServerPlatform::LinuxARM64:
                settings.platform = BuildCookPanel::TargetPlatform::LinuxARM64;
                break;
            case DedicatedServerPanel::ServerPlatform::MacOSX64:
                settings.platform = BuildCookPanel::TargetPlatform::MacOSX64;
                break;
            case DedicatedServerPanel::ServerPlatform::MacOSARM64:
                settings.platform = BuildCookPanel::TargetPlatform::MacOSARM64;
                break;
            }
            settings.includeDebugSymbols = server.includeDebugSymbols;
            settings.cookAssets = true;
            settings.outputDirectory = server.outputDirectory;
            settings.executableName = "SparkGame";
            settings.packageDedicatedServer = true;
            settings.dedicatedServerExecutableName = server.executableName;
            return settings;
        }

        DedicatedServerPanel::ServerPlatform NativeServerPlatform()
        {
            switch (BuildPipeline::NativeTargetPlatform())
            {
            case BuildCookPanel::TargetPlatform::WindowsX64:
                return DedicatedServerPanel::ServerPlatform::WindowsX64;
            case BuildCookPanel::TargetPlatform::LinuxARM64:
                return DedicatedServerPanel::ServerPlatform::LinuxARM64;
            case BuildCookPanel::TargetPlatform::LinuxX64:
                return DedicatedServerPanel::ServerPlatform::LinuxX64;
            case BuildCookPanel::TargetPlatform::MacOSARM64:
                return DedicatedServerPanel::ServerPlatform::MacOSARM64;
            case BuildCookPanel::TargetPlatform::MacOSX64:
                return DedicatedServerPanel::ServerPlatform::MacOSX64;
            default:
                return DedicatedServerPanel::ServerPlatform::WindowsX64;
            }
        }
    } // namespace

    // ============================================================================
    // Construction / Lifecycle
    // ============================================================================

    DedicatedServerPanel::DedicatedServerPanel() : EditorPanel("Dedicated Server", "dedicated_server_panel")
    {
        m_mapRotation.push_back("dm_warehouse");
        m_mapRotation.push_back("dm_rooftops");
        m_mapRotation.push_back("dm_compound");
    }

    DedicatedServerPanel::~DedicatedServerPanel() = default;

    bool DedicatedServerPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Dedicated Server panel");
        m_cookPipeline = std::make_unique<BuildPipeline>();
        m_serverProcess = std::make_unique<DedicatedServerProcessController>();
        m_cookSettings.platform = NativeServerPlatform();
        return true;
    }

    void DedicatedServerPanel::Update(float deltaTime)
    {
        if (m_cookPipeline)
        {
            for (auto& line : m_cookPipeline->DrainLogLines())
                m_cookLog.push_back({std::move(line.text), line.level == BuildLogLine::Level::Error     ? "error"
                                                           : line.level == BuildLogLine::Level::Warning ? "warning"
                                                                                                        : "info"});
            m_cookProgress = m_cookPipeline->GetProgress();
            m_cookStatus = m_cookPipeline->GetStatusText();
            if (m_isCooking && !m_cookPipeline->IsRunning())
            {
                m_isCooking = false;
                if (m_cookPipeline->GetResult() == BuildResult::Success)
                {
                    m_cookProgress = 1.0f;
                    m_cookStatus = "Native server package complete";
                    m_cookLog.push_back(
                        {"Built the game module, cooked content, and packaged SparkServer with native launchers",
                         "success"});
                }
                else
                {
                    m_cookStatus =
                        m_cookPipeline->GetResult() == BuildResult::Cancelled ? "Cook cancelled" : "Cook failed";
                }
            }
        }

        if (m_serverProcess)
        {
            m_serverProcess->Update();
            for (auto& line : m_serverProcess->DrainLogLines())
                m_pieServerLog.push_back(std::move(line));
            const auto snapshot = m_serverProcess->GetSnapshot();
            m_pieServerRunning = snapshot.state == DedicatedServerProcessState::Running ||
                                 snapshot.state == DedicatedServerProcessState::Stopping;
            if (m_pieServerRunning)
                m_pieServerUptime += deltaTime;
            if (!snapshot.error.empty() &&
                (m_pieServerLog.empty() || m_pieServerLog.back() != "[Editor] " + snapshot.error))
                m_pieServerLog.push_back("[Editor] " + snapshot.error);
        }

        // Update server browser scan
        if (m_isScanning)
        {
            m_scanTimer -= deltaTime;
            if (m_scanTimer <= 0.0f)
            {
                m_isScanning = false;
            }
        }
    }

    void DedicatedServerPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            // Tab bar
            if (ImGui::BeginTabBar("##DediServerTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_COG " Config"))
                {
                    m_activeTab = TAB_CONFIG;
                    RenderServerConfigTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_MAP " Maps"))
                {
                    m_activeTab = TAB_MAPS;
                    RenderMapRotationTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_HAMMER " Cook/Pack"))
                {
                    m_activeTab = TAB_COOK;
                    RenderCookPackageTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_PLAY " PIE Server"))
                {
                    m_activeTab = TAB_PIE;
                    RenderPIEServerTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_GLOBE " Browser"))
                {
                    m_activeTab = TAB_BROWSER;
                    RenderServerBrowserTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_CHART_BAR " Monitor"))
                {
                    m_activeTab = TAB_MONITOR;
                    RenderMonitorTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void DedicatedServerPanel::Shutdown()
    {
        if (m_pieServerRunning)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Stopping PIE server during shutdown");
            StopPIEServer();
        }
        if (m_cookPipeline && m_cookPipeline->IsRunning())
            m_cookPipeline->Cancel();
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Dedicated Server panel");
    }

    bool DedicatedServerPanel::HandleEvent(const std::string& /*eventType*/, void* /*eventData*/)
    {
        return false;
    }

    // ============================================================================
    // Tab: Server Configuration
    // ============================================================================

    void DedicatedServerPanel::RenderServerConfigTab()
    {
        ImGui::Text(ICON_FA_SERVER " Server Identity");
        ImGui::Separator();

        // Server name
        char nameBuf[256];
        strncpy(nameBuf, m_pieConfig.serverName.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Server Name", nameBuf, sizeof(nameBuf)))
        {
            m_pieConfig.serverName = nameBuf;
        }

        ImGui::Spacing();
        ImGui::Text(ICON_FA_PLUG " Network Settings");
        ImGui::Separator();

        // Port
        int port = static_cast<int>(m_pieConfig.port);
        if (ImGui::InputInt("Port", &port))
        {
            m_pieConfig.port = static_cast<uint16_t>(std::clamp(port, 1024, 65535));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("UDP port for game traffic (default: 27015)");

        // Max players
        ImGui::SliderInt("Max Players", &m_pieConfig.maxPlayers, 2, 64);

        // Tick rate
        float tickRate = m_pieConfig.tickRate;
        if (ImGui::SliderFloat("Tick Rate (Hz)", &tickRate, 20.0f, 128.0f, "%.0f"))
        {
            m_pieConfig.tickRate = tickRate;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Server simulation rate.\n60 Hz = standard\n128 Hz = competitive");

        char bindAddress[64]{};
        std::strncpy(bindAddress, m_pieConfig.bindAddress.c_str(), sizeof(bindAddress) - 1);
        if (ImGui::InputText("Bind Address", bindAddress, sizeof(bindAddress)))
            m_pieConfig.bindAddress = bindAddress;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use 'loopback' for this machine, or canonical RFC1918 CIDR such as "
                              "192.168.1.20/24.\nMissing/invalid prefixes and wildcard/public addresses are rejected. "
                              "Transport is not authenticated or encrypted.");

        ImGui::Spacing();
        ImGui::Text(ICON_FA_GAMEPAD " Game Settings");
        ImGui::Separator();

        // SparkServer currently receives the map through its real CLI contract;
        // game-mode selection remains module-owned until that contract exists.
        ImGui::BeginDisabled();
        ImGui::Combo("Game Mode", &m_pieConfig.gameMode, GAME_MODE_NAMES, NUM_GAME_MODES);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Unavailable: SparkServer has no game-mode command-line/config contract yet.");

        // Map
        char mapBuf[256];
        strncpy(mapBuf, m_pieConfig.mapName.c_str(), sizeof(mapBuf) - 1);
        mapBuf[sizeof(mapBuf) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Starting Map", mapBuf, sizeof(mapBuf)))
        {
            m_pieConfig.mapName = mapBuf;
        }

        ImGui::Spacing();
        ImGui::Text(ICON_FA_TERMINAL " Administration");
        ImGui::Separator();

        m_pieConfig.enableRcon = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox("Enable RCON", &m_pieConfig.enableRcon);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Unavailable: SparkServer intentionally exposes no authenticated remote RCON transport.");
    }

    // ============================================================================
    // Tab: Map Rotation
    // ============================================================================

    void DedicatedServerPanel::RenderMapRotationTab()
    {
        ImGui::Text(ICON_FA_MAP " Map Rotation");
        ImGui::Separator();

        // Map list
        ImGui::BeginChild("##MapList", ImVec2(0, ImGui::GetContentRegionAvail().y - 80), true);
        for (int i = 0; i < static_cast<int>(m_mapRotation.size()); ++i)
        {
            bool selected = (m_selectedMapIndex == i);
            char label[256];
            snprintf(label, sizeof(label), "%d. %s", i + 1, m_mapRotation[static_cast<size_t>(i)].c_str());

            if (ImGui::Selectable(label, selected))
            {
                m_selectedMapIndex = i;
            }

            // Drag-drop reorder
            if (ImGui::IsItemActive() && !ImGui::IsItemHovered())
            {
                int nextIdx = i + (ImGui::GetMouseDragDelta(0).y < 0.0f ? -1 : 1);
                if (nextIdx >= 0 && nextIdx < static_cast<int>(m_mapRotation.size()))
                {
                    std::swap(m_mapRotation[static_cast<size_t>(i)], m_mapRotation[static_cast<size_t>(nextIdx)]);
                    m_selectedMapIndex = nextIdx;
                    ImGui::ResetMouseDragDelta();
                    SetModified(true);
                }
            }
        }
        ImGui::EndChild();

        // Controls
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 160);
        ImGui::InputText("##NewMap", m_newMapNameBuf, sizeof(m_newMapNameBuf));
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_PLUS " Add", ImVec2(70, 0)))
        {
            if (m_newMapNameBuf[0] != '\0')
            {
                m_mapRotation.push_back(m_newMapNameBuf);
                m_newMapNameBuf[0] = '\0';
                SetModified(true);
            }
        }
        ImGui::SameLine();

        bool hasSelection = m_selectedMapIndex >= 0 && m_selectedMapIndex < static_cast<int>(m_mapRotation.size());
        if (!hasSelection)
            ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_MINUS " Remove", ImVec2(80, 0)))
        {
            m_mapRotation.erase(m_mapRotation.begin() + m_selectedMapIndex);
            if (m_selectedMapIndex >= static_cast<int>(m_mapRotation.size()))
                m_selectedMapIndex = static_cast<int>(m_mapRotation.size()) - 1;
            SetModified(true);
        }
        if (!hasSelection)
            ImGui::EndDisabled();
    }

    // ============================================================================
    // Tab: Cook / Package
    // ============================================================================

    void DedicatedServerPanel::RenderCookPackageTab()
    {
        ImGui::Text(ICON_FA_HAMMER " Cook Dedicated Server");
        ImGui::Separator();

        // Platform
        ImGui::Text("Target Platform:");
        m_cookSettings.platform = NativeServerPlatform();
        ImGui::Text("%s (native)", GetPlatformName(m_cookSettings.platform));
        ImGui::TextDisabled("Server packages include native runtime dependencies. Cross-compilation requires an "
                            "external matching host/toolchain and is not simulated here.");

        // Build profile
        ImGui::Text("Build Profile:");
        float btnWidth = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
        ImVec2 btnSize(btnWidth, 28.0f);

        auto ProfileBtn = [&](const char* label, ServerBuildProfile profile, const ImVec4& color)
        {
            bool active = (m_cookSettings.profile == profile);
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, color);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(color.x + 0.1f, color.y + 0.1f, color.z + 0.1f, 1.0f));
            }
            if (ImGui::Button(label, btnSize))
            {
                m_cookSettings.profile = profile;
            }
            if (active)
                ImGui::PopStyleColor(2);
            ImGui::SameLine();
        };

        ProfileBtn(ICON_FA_BUG " Debug", ServerBuildProfile::Debug, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
        ProfileBtn(ICON_FA_CODE " Dev", ServerBuildProfile::Development, ImVec4(0.3f, 0.5f, 0.7f, 1.0f));
        ProfileBtn(ICON_FA_ROCKET " Ship", ServerBuildProfile::Shipping, ImVec4(0.8f, 0.55f, 0.1f, 1.0f));
        ImGui::NewLine();

        ImGui::Separator();
        ImGui::Text(ICON_FA_COG " Server Cook Options:");
        ImGui::Indent(8.0f);

        m_cookSettings.stripGraphics = true;
        m_cookSettings.stripAudio = true;
        m_cookSettings.stripEditor = true;
        ImGui::TextDisabled(
            "SparkServer is a dedicated headless host and never bundles editor, graphics, or audio UI.");
        ImGui::BeginDisabled();
        ImGui::Checkbox("Headless dedicated host", &m_cookSettings.stripGraphics);
        ImGui::Checkbox("Audio omitted by dedicated host", &m_cookSettings.stripAudio);
        ImGui::Checkbox("Editor omitted by dedicated host", &m_cookSettings.stripEditor);
        ImGui::EndDisabled();
        ImGui::Checkbox("Include Debug Symbols", &m_cookSettings.includeDebugSymbols);
        m_cookSettings.compressAssets = false;
        m_cookSettings.enableLogging = true;
        m_cookSettings.enableRcon = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox("Compress Package", &m_cookSettings.compressAssets);
        ImGui::Checkbox("Engine Logging", &m_cookSettings.enableLogging);
        ImGui::Checkbox("Remote RCON", &m_cookSettings.enableRcon);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Unavailable: SparkServer has no authenticated remote RCON transport.");
        ImGui::TextDisabled("Package archives are not implemented; SparkServer logging remains enabled.");

        ImGui::Unindent(8.0f);

        ImGui::Separator();
        ImGui::Text(ICON_FA_FOLDER " Output:");

        char outDir[512];
        strncpy(outDir, m_cookSettings.outputDirectory.c_str(), sizeof(outDir) - 1);
        outDir[sizeof(outDir) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##ServerOutDir", outDir, sizeof(outDir)))
        {
            m_cookSettings.outputDirectory = outDir;
        }

        char exeName[256];
        strncpy(exeName, m_cookSettings.executableName.c_str(), sizeof(exeName) - 1);
        exeName[sizeof(exeName) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Server Executable Name", exeName, sizeof(exeName)))
        {
            m_cookSettings.executableName = exeName;
        }

        ImGui::Separator();

        // Cook actions
        ImVec4 cookColor(0.2f, 0.55f, 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, cookColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.65f, 0.3f, 1.0f));
        if (!m_isCooking)
        {
            if (ImGui::Button(ICON_FA_DOWNLOAD " Build, Cook & Package Server", ImVec2(-1.0f, 36.0f)))
            {
                StartCookServer();
            }
        }
        else
        {
            ImGui::BeginDisabled();
            ImGui::Button(ICON_FA_SPINNER " Building native server package...", ImVec2(-1.0f, 36.0f));
            ImGui::EndDisabled();
        }
        ImGui::PopStyleColor(2);

        // Progress
        RenderCookProgress();

        // Log
        if (!m_cookLog.empty())
        {
            RenderCookLog();
        }
    }

    // ============================================================================
    // Tab: PIE Server (Play-In-Editor Dedicated Server)
    // ============================================================================

    void DedicatedServerPanel::RenderPIEServerTab()
    {
        ImGui::Text(ICON_FA_PLAY " Play-In-Editor Dedicated Server");
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                           "Launch a local dedicated server for testing multiplayer in the editor.");
        ImGui::Separator();

        if (!m_pieServerRunning)
        {
            // Configuration
            ImGui::Text("Quick Setup:");
            ImGui::Indent(8.0f);

            // Server name
            char nameBuf[128];
            strncpy(nameBuf, m_pieConfig.serverName.c_str(), sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            if (ImGui::InputText("Name##PIE", nameBuf, sizeof(nameBuf)))
            {
                m_pieConfig.serverName = nameBuf;
            }

            // Port
            int port = static_cast<int>(m_pieConfig.port);
            if (ImGui::InputInt("Port##PIE", &port))
            {
                m_pieConfig.port = static_cast<uint16_t>(std::clamp(port, 1024, 65535));
            }

            ImGui::SliderInt("Max Players##PIE", &m_pieConfig.maxPlayers, 2, 32);
            ImGui::BeginDisabled();
            ImGui::Combo("Game Mode##PIE", &m_pieConfig.gameMode, GAME_MODE_NAMES, NUM_GAME_MODES);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Unavailable: SparkServer does not accept a game-mode option yet.");

            // Map selection from rotation
            if (!m_mapRotation.empty())
            {
                // Build a combined string of map names for combo
                std::string mapNames;
                for (const auto& map : m_mapRotation)
                {
                    mapNames += map + '\0';
                }
                mapNames += '\0';

                // Find current map in rotation
                int mapIdx = 0;
                for (int i = 0; i < static_cast<int>(m_mapRotation.size()); ++i)
                {
                    if (m_mapRotation[static_cast<size_t>(i)] == m_pieConfig.mapName)
                    {
                        mapIdx = i;
                        break;
                    }
                }

                if (ImGui::Combo("Map##PIE", &mapIdx, mapNames.c_str()))
                {
                    m_pieConfig.mapName = m_mapRotation[static_cast<size_t>(mapIdx)];
                }
            }
            else
            {
                char mapBuf[256];
                strncpy(mapBuf, m_pieConfig.mapName.c_str(), sizeof(mapBuf) - 1);
                mapBuf[sizeof(mapBuf) - 1] = '\0';
                if (ImGui::InputText("Map##PIE", mapBuf, sizeof(mapBuf)))
                {
                    m_pieConfig.mapName = mapBuf;
                }
            }

            ImGui::Spacing();
            m_pieConfig.openConsoleWindow = false;
            m_pieConfig.autoConnect = false;
            ImGui::BeginDisabled();
            ImGui::Checkbox("Open Console Window", &m_pieConfig.openConsoleWindow);
            ImGui::Checkbox("Auto-Connect Editor Client", &m_pieConfig.autoConnect);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Unavailable: editor client connection transport is not integrated yet.");
            ImGui::TextDisabled("Server output is streamed here; automatic client connection is pending.");

            ImGui::Unindent(8.0f);

            ImGui::Spacing();

            // Launch button
            ImVec4 launchColor(0.15f, 0.5f, 0.15f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, launchColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
            float fullWidth = ImGui::GetContentRegionAvail().x;
            if (ImGui::Button(ICON_FA_PLAY " Launch PIE Dedicated Server", ImVec2(fullWidth, 44.0f)))
            {
                LaunchPIEServer();
            }
            ImGui::PopStyleColor(2);
        }
        else
        {
            // Running state
            ImVec4 runningColor(0.2f, 0.7f, 0.2f, 1.0f);
            ImGui::TextColored(runningColor, ICON_FA_CHECK " PIE Server Running");
            ImGui::SameLine();
            int upMins = static_cast<int>(m_pieServerUptime) / 60;
            int upSecs = static_cast<int>(m_pieServerUptime) % 60;
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(Uptime: %d:%02d)", upMins, upSecs);

            ImGui::Text("Server: %s | Port: %d | Map: %s", m_pieConfig.serverName.c_str(), m_pieConfig.port,
                        m_pieConfig.mapName.c_str());
            ImGui::Text("Mode: %s | Max Players: %d", GAME_MODE_NAMES[m_pieConfig.gameMode], m_pieConfig.maxPlayers);

            ImGui::Separator();

            // RCON console
            RenderPIEServerConsole();

            ImGui::Separator();

            // Stop button
            ImVec4 stopColor(0.65f, 0.15f, 0.15f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, stopColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.25f, 0.25f, 1.0f));
            float fullWidth = ImGui::GetContentRegionAvail().x;
            if (ImGui::Button(ICON_FA_STOP " Stop PIE Server", ImVec2(fullWidth, 36.0f)))
            {
                StopPIEServer();
            }
            ImGui::PopStyleColor(2);
        }
    }

    // ============================================================================
    // Tab: Server Browser
    // ============================================================================

    void DedicatedServerPanel::RenderServerBrowserTab()
    {
        ImGui::Text(ICON_FA_GLOBE " LAN Server Browser");
        ImGui::Separator();

        // Refresh button
        if (!m_isScanning)
        {
            if (ImGui::Button(ICON_FA_REFRESH " Scan LAN", ImVec2(120, 30)))
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Scanning LAN for servers");
                RefreshServerBrowser();
            }
        }
        else
        {
            ImGui::BeginDisabled();
            ImGui::Button(ICON_FA_SPINNER " Scanning...", ImVec2(120, 30));
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Found %d server(s)",
                           static_cast<int>(m_discoveredServers.size()));

        ImGui::Separator();

        // Server list
        if (ImGui::BeginTable("##ServerList", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2(0, ImGui::GetContentRegionAvail().y - 40)))
        {
            ImGui::TableSetupColumn("Server Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_discoveredServers.size()); ++i)
            {
                const auto& server = m_discoveredServers[static_cast<size_t>(i)];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", server.name.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", server.map.c_str());

                ImGui::TableSetColumnIndex(2);
                if (server.gameMode >= 0 && server.gameMode < NUM_GAME_MODES)
                    ImGui::Text("%s", GAME_MODE_NAMES[server.gameMode]);
                else
                    ImGui::Text("Unknown");

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d/%d", server.currentPlayers, server.maxPlayers);

                ImGui::TableSetColumnIndex(4);
                ImVec4 pingColor = (server.ping < 50.0f)    ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                                   : (server.ping < 100.0f) ? ImVec4(0.8f, 0.8f, 0.2f, 1.0f)
                                                            : ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
                ImGui::TextColored(pingColor, "%.0f", server.ping);

                ImGui::TableSetColumnIndex(5);
                char joinBtnId[32];
                snprintf(joinBtnId, sizeof(joinBtnId), "Join##%d", i);
                if (ImGui::SmallButton(joinBtnId))
                {
                    ConnectToServer(server);
                }
            }

            ImGui::EndTable();
        }

        // Direct connect
        ImGui::Separator();
        static char directAddr[128] = "127.0.0.1:27015";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80);
        ImGui::InputText("##DirectConnect", directAddr, sizeof(directAddr));
        ImGui::SameLine();
        if (ImGui::Button("Connect", ImVec2(72, 0)))
        {
            DiscoveredServer direct;
            direct.name = "Direct";
            direct.address = directAddr;
            direct.port = 27015;
            // Parse address:port
            std::string addrStr(directAddr);
            auto colonPos = addrStr.find(':');
            if (colonPos != std::string::npos)
            {
                direct.address = addrStr.substr(0, colonPos);
                try
                {
                    direct.port = static_cast<uint16_t>(std::stoi(addrStr.substr(colonPos + 1)));
                }
                catch (const std::exception&)
                {
                    direct.port = 27015;
                }
            }
            ConnectToServer(direct);
        }
    }

    // ============================================================================
    // Tab: Monitor
    // ============================================================================

    void DedicatedServerPanel::RenderMonitorTab()
    {
        if (!m_pieServerRunning)
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.3f, 1.0f),
                               ICON_FA_EXCLAMATION " No server running. Launch a PIE server first.");
            return;
        }

        ImGui::Text(ICON_FA_CHART_BAR " Server Monitor");
        ImGui::Separator();

        // Stats
        RenderServerStats();

        ImGui::Separator();

        // Player list
        if (ImGui::CollapsingHeader(ICON_FA_USERS " Connected Players", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RenderPlayerList();
        }

        ImGui::Separator();

        // RCON Console
        if (ImGui::CollapsingHeader(ICON_FA_TERMINAL " RCON Console", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RenderRconConsole();
        }
    }

    // ============================================================================
    // Cook Helpers
    // ============================================================================

    void DedicatedServerPanel::StartCookServer()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Starting dedicated server cook (platform=%s, profile=%s)",
                       GetPlatformName(m_cookSettings.platform), GetProfileName(m_cookSettings.profile));
        m_cookLog.clear();
        if (!m_cookPipeline)
        {
            m_cookStatus = "Cook pipeline is unavailable";
            m_cookLog.push_back({m_cookStatus, "error"});
            return;
        }
        const std::string projectRoot = ProjectManager::GetActiveProjectPath();
        if (projectRoot.empty())
        {
            m_cookStatus = "Open a project before cooking a server";
            m_cookLog.push_back({m_cookStatus, "error"});
            return;
        }
        if (!m_cookPipeline->StartBuild(MakeCookSettings(m_cookSettings), projectRoot))
        {
            for (auto& line : m_cookPipeline->DrainLogLines())
                m_cookLog.push_back(
                    {std::move(line.text), line.level == BuildLogLine::Level::Error ? "error" : "info"});
            m_cookStatus = m_cookPipeline->GetStatusText();
            if (m_cookLog.empty())
                m_cookLog.push_back({"Could not start the native module/server build", "error"});
            return;
        }
        m_isCooking = true;
        m_cookProgress = 0.0f;
        m_cookStatus = "Building game module and native server hosts...";
        m_cookLog.push_back(
            {"Native build, content cook, and complete package flow started for " + projectRoot, "info"});
    }

    void DedicatedServerPanel::RenderCookProgress()
    {
        if (m_isCooking || m_cookProgress > 0.0f)
        {
            ImGui::Spacing();
            ImGui::ProgressBar(m_cookProgress, ImVec2(-1, 20));
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", m_cookStatus.c_str());
        }
    }

    void DedicatedServerPanel::RenderCookLog()
    {
        if (ImGui::CollapsingHeader(ICON_FA_TERMINAL " Cook Log", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginChild("##CookLog", ImVec2(0, 120), true);
            for (const auto& entry : m_cookLog)
            {
                ImVec4 color(0.8f, 0.8f, 0.8f, 1.0f);
                const char* prefix = ICON_FA_INFO_CIRCLE;
                if (entry.level == "warning")
                {
                    color = ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
                    prefix = ICON_FA_EXCLAMATION;
                }
                else if (entry.level == "error")
                {
                    color = ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
                    prefix = ICON_FA_TIMES;
                }
                else if (entry.level == "success")
                {
                    color = ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
                    prefix = ICON_FA_CHECK;
                }
                ImGui::TextColored(color, "%s %s", prefix, entry.message.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
    }

    const char* DedicatedServerPanel::GetPlatformName(ServerPlatform platform)
    {
        switch (platform)
        {
        case ServerPlatform::WindowsX64:
            return "Windows x64";
        case ServerPlatform::LinuxX64:
            return "Linux x64";
        case ServerPlatform::LinuxARM64:
            return "Linux ARM64";
        case ServerPlatform::MacOSX64:
            return "macOS x64";
        case ServerPlatform::MacOSARM64:
            return "macOS ARM64";
        default:
            return "Unknown";
        }
    }

    const char* DedicatedServerPanel::GetProfileName(ServerBuildProfile profile)
    {
        switch (profile)
        {
        case ServerBuildProfile::Debug:
            return "Debug";
        case ServerBuildProfile::Development:
            return "Development";
        case ServerBuildProfile::Shipping:
            return "Shipping";
        default:
            return "Unknown";
        }
    }

    // ============================================================================
    // PIE Server Helpers
    // ============================================================================

    void DedicatedServerPanel::LaunchPIEServer()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Launching PIE server: %s port=%d map=%s mode=%s maxPlayers=%d",
                       m_pieConfig.serverName.c_str(), m_pieConfig.port, m_pieConfig.mapName.c_str(),
                       GAME_MODE_NAMES[m_pieConfig.gameMode], m_pieConfig.maxPlayers);
        m_pieServerUptime = 0.0f;
        m_pieServerLog.clear();
        if (!m_serverProcess)
        {
            m_pieServerLog.push_back("[Editor] SparkServer process controller is unavailable");
            return;
        }
        const auto projectRoot = std::filesystem::path(ProjectManager::GetActiveProjectPath());
        const auto executable = FindSparkServerExecutable();
        const auto manifest = projectRoot / "spark.modules.json";
        std::error_code error;
        if (projectRoot.empty() || !std::filesystem::is_regular_file(manifest, error))
        {
            m_pieServerLog.push_back("[Editor] Open a built project with spark.modules.json before launching");
            return;
        }

        DedicatedServerLaunchRequest request;
        request.executable = executable;
        request.workingDirectory = projectRoot;
        request.manifest = manifest;
        request.healthFile = projectRoot / "Temp" / "spark-pie-server-health.json";
        request.stopFile = projectRoot / "Temp" / "spark-pie-server.stop";
        request.serverName = m_pieConfig.serverName;
        request.map = m_pieConfig.mapName;
        request.port = m_pieConfig.port;
        request.maxClients = static_cast<uint32_t>(m_pieConfig.maxPlayers);
        request.tickRate = m_pieConfig.tickRate;
        request.bindAddress = m_pieConfig.bindAddress;
        request.lanBroadcast = false;
        m_pieServerRunning = m_serverProcess->Launch(request);
        for (auto& line : m_serverProcess->DrainLogLines())
            m_pieServerLog.push_back(std::move(line));
        if (m_pieConfig.autoConnect && m_pieServerRunning)
            m_pieServerLog.push_back("[Editor] Server launched; editor client auto-connect transport is unavailable");
    }

    void DedicatedServerPanel::StopPIEServer()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Stopping PIE server (uptime: %.0fs)", m_pieServerUptime);
        if (m_serverProcess)
            m_serverProcess->RequestStop();
        m_pieServerLog.push_back("[Editor] Waiting for SparkServer graceful shutdown...");
    }

    void DedicatedServerPanel::RenderPIEServerConsole()
    {
        ImGui::Text(ICON_FA_TERMINAL " Server Console");
        ImGui::BeginChild("##PIEConsole", ImVec2(0, 150), true);
        for (const auto& line : m_pieServerLog)
        {
            ImVec4 color(0.8f, 0.8f, 0.8f, 1.0f);
            if (line.find("[Server]") == 0)
                color = ImVec4(0.4f, 0.7f, 1.0f, 1.0f);
            else if (line.find("[Client]") == 0)
                color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
            else if (line.find("[RCON]") == 0)
                color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
            ImGui::TextColored(color, "%s", line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        ImGui::TextDisabled("Remote console is disabled until SparkServer exposes an authenticated RCON transport.");
    }

    // ============================================================================
    // Server Browser Helpers
    // ============================================================================

    void DedicatedServerPanel::RefreshServerBrowser()
    {
        m_isScanning = true;
        m_scanTimer = 2.0f; // 2 second scan
        m_discoveredServers.clear();

        // If we have a PIE server running, add it to the list
        if (m_pieServerRunning)
        {
            DiscoveredServer local;
            local.name = m_pieConfig.serverName;
            local.map = m_pieConfig.mapName;
            local.address = "127.0.0.1";
            local.port = m_pieConfig.port;
            local.currentPlayers = 0;
            local.maxPlayers = m_pieConfig.maxPlayers;
            local.gameMode = m_pieConfig.gameMode;
            local.ping = 0.0f;
            local.discoveredAt = std::chrono::steady_clock::now();
            m_discoveredServers.push_back(local);
        }
    }

    void DedicatedServerPanel::ConnectToServer(const DiscoveredServer& server)
    {
        if (m_pieServerRunning)
            m_pieServerLog.push_back("[Editor] Client connect requested for " + server.address + ":" +
                                     std::to_string(server.port) + "; no editor client transport is wired");
    }

    // ============================================================================
    // Monitor Helpers
    // ============================================================================

    void DedicatedServerPanel::RenderPlayerList()
    {
        ImGui::BeginChild("##PlayerList", ImVec2(0, 100), true);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "SparkServer health reports aggregate player count only.");
        ImGui::EndChild();
    }

    void DedicatedServerPanel::RenderRconConsole()
    {
        ImGui::BeginChild("##RconConsole", ImVec2(0, 100), true);
        for (const auto& line : m_rconHistory)
        {
            ImGui::TextWrapped("%s", line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        ImGui::TextDisabled("No authenticated RCON transport is configured.");
    }

    void DedicatedServerPanel::RenderServerStats()
    {
        ImGui::Columns(2, "##ServerStatsCols", false);

        int upMins = static_cast<int>(m_pieServerUptime) / 60;
        int upSecs = static_cast<int>(m_pieServerUptime) % 60;
        ImGui::Text("Uptime: %d:%02d", upMins, upSecs);
        ImGui::Text("Tick Rate: %.0f Hz", m_pieConfig.tickRate);
        ImGui::Text("Players: 0/%d", m_pieConfig.maxPlayers);

        ImGui::NextColumn();

        ImGui::Text("Map: %s", m_pieConfig.mapName.c_str());
        ImGui::Text("Mode: %s", GAME_MODE_NAMES[m_pieConfig.gameMode]);
        ImGui::Text("Port: %d", m_pieConfig.port);

        ImGui::Columns(1);
        if (m_serverProcess)
        {
            const auto snapshot = m_serverProcess->GetSnapshot();
            if (!snapshot.healthJson.empty())
                ImGui::TextWrapped("Health: %s", snapshot.healthJson.c_str());
            if (snapshot.exitCode)
                ImGui::Text("Last exit code: %d", *snapshot.exitCode);
        }
    }

} // namespace SparkEditor
