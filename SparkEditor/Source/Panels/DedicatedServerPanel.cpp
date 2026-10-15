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
#include <cctype>
#include <filesystem>

#include <imgui.h>

#include "../Core/EditorIcons.h"
#include "../Core/ProjectManager.h"
#include "../Utils/EditorProcessLaunch.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include "Utils/LogMacros.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

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

    DedicatedServerPanel::DedicatedServerPanel() : EditorPanel("Dedicated Server", "dedicated_server_panel") {}

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
        Spark::Ui::SubHeader(ICON_FA_SERVER " Server Identity");
        if (Spark::Ui::BeginPropertyGrid("ServerIdentity"))
        {
            Spark::Ui::PropertyLabel("Server Name");
            char nameBuf[256];
            strncpy(nameBuf, m_pieConfig.serverName.c_str(), sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            if (ImGui::InputText("##ServerName", nameBuf, sizeof(nameBuf)))
            {
                m_pieConfig.serverName = nameBuf;
            }
            Spark::Ui::EndPropertyGrid();
        }

        Spark::Ui::SubHeader(ICON_FA_PLUG " Network Settings");
        if (Spark::Ui::BeginPropertyGrid("ServerNetwork"))
        {
            Spark::Ui::PropertyLabel("Port");
            int port = static_cast<int>(m_pieConfig.port);
            if (ImGui::InputInt("##Port", &port))
            {
                m_pieConfig.port = static_cast<uint16_t>(std::clamp(port, 1024, 65535));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("UDP port for game traffic (default: 27015)");

            Spark::Ui::PropertyLabel("Max Players");
            ImGui::SliderInt("##MaxPlayers", &m_pieConfig.maxPlayers, 2, 64);

            Spark::Ui::PropertyLabel("Tick Rate (Hz)");
            float tickRate = m_pieConfig.tickRate;
            if (ImGui::SliderFloat("##TickRate", &tickRate, 20.0f, 128.0f, "%.0f"))
            {
                m_pieConfig.tickRate = tickRate;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Server simulation rate.\n60 Hz = standard\n128 Hz = competitive");

            Spark::Ui::PropertyLabel("Bind Address");
            char bindAddress[64]{};
            std::strncpy(bindAddress, m_pieConfig.bindAddress.c_str(), sizeof(bindAddress) - 1);
            if (ImGui::InputText("##BindAddress", bindAddress, sizeof(bindAddress)))
                m_pieConfig.bindAddress = bindAddress;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Use 'loopback' for this machine, or canonical RFC1918 CIDR such as "
                    "192.168.1.20/24.\nMissing/invalid prefixes and wildcard/public addresses are rejected. "
                    "Transport is not authenticated or encrypted.");
            Spark::Ui::EndPropertyGrid();
        }

        Spark::Ui::SubHeader(ICON_FA_GAMEPAD " Game Settings");
        if (Spark::Ui::BeginPropertyGrid("ServerGame"))
        {
            // SparkServer currently receives the map through its real CLI contract;
            // game-mode selection remains module-owned until that contract exists.
            Spark::Ui::PropertyLabel("Game Mode");
            {
                const Spark::Ui::ScopedDisabled disabled(
                    true, "Unavailable: SparkServer has no game-mode command-line/config contract yet.");
                ImGui::Combo("##GameMode", &m_pieConfig.gameMode, GAME_MODE_NAMES, NUM_GAME_MODES);
            }

            Spark::Ui::PropertyLabel("Starting Map");
            char mapBuf[256];
            strncpy(mapBuf, m_pieConfig.mapName.c_str(), sizeof(mapBuf) - 1);
            mapBuf[sizeof(mapBuf) - 1] = '\0';
            if (ImGui::InputText("##StartingMap", mapBuf, sizeof(mapBuf)))
            {
                m_pieConfig.mapName = mapBuf;
            }
            Spark::Ui::EndPropertyGrid();
        }

        Spark::Ui::SubHeader(ICON_FA_TERMINAL " Administration");
        m_pieConfig.enableRcon = false;
        Spark::Ui::UnavailableFeature("Remote RCON",
                                      "SparkServer intentionally exposes no authenticated remote RCON transport.");
    }

    // ============================================================================
    // Tab: Map Rotation
    // ============================================================================

    void DedicatedServerPanel::RenderMapRotationTab()
    {
        Spark::Ui::SubHeader(ICON_FA_MAP " Map List");
        ImGui::PushTextWrapPos(0.0f);
        Spark::Ui::SecondaryText("SparkServer runs one map per launch. This list is the pick-list for the Map field "
                                 "on the PIE Server tab; the server does not cycle it.");
        ImGui::PopTextWrapPos();

        // Map list
        ImGui::BeginChild("##MapList", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - Spark::Ui::Px(80.0f)), true);
        if (m_mapRotation.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_MAP, "No maps in the list",
                                  "Type a map name below and press Add to build the PIE pick-list.");
        }
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
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Spark::Ui::Px(160.0f));
        ImGui::InputTextWithHint("##NewMap", "Map name", m_newMapNameBuf, sizeof(m_newMapNameBuf));
        ImGui::SameLine();

        if (Spark::Ui::SecondaryButton(ICON_FA_PLUS " Add", Spark::Ui::Px2(70, 0)))
        {
            if (m_newMapNameBuf[0] != '\0')
            {
                m_mapRotation.push_back(m_newMapNameBuf);
                m_newMapNameBuf[0] = '\0';
                SetModified(true);
            }
        }
        ImGui::SameLine();

        const bool hasSelection =
            m_selectedMapIndex >= 0 && m_selectedMapIndex < static_cast<int>(m_mapRotation.size());
        const Spark::Ui::ScopedDisabled disabled(!hasSelection, "Select a map to remove.");
        if (Spark::Ui::SecondaryButton(ICON_FA_MINUS " Remove", Spark::Ui::Px2(80, 0)))
        {
            m_mapRotation.erase(m_mapRotation.begin() + m_selectedMapIndex);
            if (m_selectedMapIndex >= static_cast<int>(m_mapRotation.size()))
                m_selectedMapIndex = static_cast<int>(m_mapRotation.size()) - 1;
            SetModified(true);
        }
    }

    // ============================================================================
    // Tab: Cook / Package
    // ============================================================================

    void DedicatedServerPanel::RenderCookPackageTab()
    {
        Spark::Ui::SubHeader(ICON_FA_HAMMER " Cook Dedicated Server");

        m_cookSettings.platform = NativeServerPlatform();
        if (Spark::Ui::BeginPropertyGrid("ServerCookTarget"))
        {
            Spark::Ui::PropertyLabel("Target Platform");
            ImGui::Text("%s (native)", GetPlatformName(m_cookSettings.platform));

            // Segment order matches the ServerBuildProfile enumerators.
            static constexpr const char* kProfileLabels[] = {ICON_FA_BUG " Debug", ICON_FA_CODE " Dev",
                                                             ICON_FA_ROCKET " Ship"};
            Spark::Ui::PropertyLabel("Build Profile");
            int profileIndex = static_cast<int>(m_cookSettings.profile);
            if (Spark::Ui::SegmentedControl("##ServerProfile", kProfileLabels, 3, &profileIndex))
            {
                m_cookSettings.profile = static_cast<ServerBuildProfile>(profileIndex);
            }
            Spark::Ui::EndPropertyGrid();
        }
        ImGui::PushTextWrapPos(0.0f);
        Spark::Ui::SecondaryText("Server packages include native runtime dependencies. Cross-compilation requires an "
                                 "external matching host/toolchain and is not simulated here.");
        ImGui::PopTextWrapPos();

        Spark::Ui::SubHeader(ICON_FA_COG " Server Cook Options");

        m_cookSettings.stripGraphics = true;
        m_cookSettings.stripAudio = true;
        m_cookSettings.stripEditor = true;
        Spark::Ui::SecondaryText(
            "SparkServer is a dedicated headless host and never bundles editor, graphics, or audio UI.");
        {
            const Spark::Ui::ScopedDisabled disabled(true);
            ImGui::Checkbox("Headless dedicated host", &m_cookSettings.stripGraphics);
            ImGui::Checkbox("Audio omitted by dedicated host", &m_cookSettings.stripAudio);
            ImGui::Checkbox("Editor omitted by dedicated host", &m_cookSettings.stripEditor);
        }
        ImGui::Checkbox("Include Debug Symbols", &m_cookSettings.includeDebugSymbols);
        m_cookSettings.compressAssets = false;
        m_cookSettings.enableLogging = true;
        m_cookSettings.enableRcon = false;
        Spark::Ui::UnavailableFeature("Package archives and remote RCON",
                                      "Package archives are not implemented, SparkServer logging remains enabled, "
                                      "and SparkServer has no authenticated remote RCON transport.");

        Spark::Ui::SubHeader(ICON_FA_FOLDER " Output");
        if (Spark::Ui::BeginPropertyGrid("ServerCookOutput"))
        {
            Spark::Ui::PropertyLabel("Output Directory");
            char outDir[512];
            strncpy(outDir, m_cookSettings.outputDirectory.c_str(), sizeof(outDir) - 1);
            outDir[sizeof(outDir) - 1] = '\0';
            if (ImGui::InputText("##ServerOutDir", outDir, sizeof(outDir)))
            {
                m_cookSettings.outputDirectory = outDir;
            }

            Spark::Ui::PropertyLabel("Server Executable Name");
            char exeName[256];
            strncpy(exeName, m_cookSettings.executableName.c_str(), sizeof(exeName) - 1);
            exeName[sizeof(exeName) - 1] = '\0';
            if (ImGui::InputText("##ServerExeName", exeName, sizeof(exeName)))
            {
                m_cookSettings.executableName = exeName;
            }
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Separator();

        // Cook actions
        const ImVec2 actionSize(-1.0f, Spark::Ui::Px(36.0f));
        if (!m_isCooking)
        {
            if (Spark::Ui::PrimaryButton(ICON_FA_DOWNLOAD " Build, Cook & Package Server", actionSize))
            {
                StartCookServer();
            }
        }
        else
        {
            const Spark::Ui::ScopedDisabled disabled(true);
            Spark::Ui::SecondaryButton(ICON_FA_SPINNER " Building native server package...", actionSize);
        }

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
        Spark::Ui::SubHeader(ICON_FA_PLAY " Play-In-Editor Dedicated Server");
        Spark::Ui::SecondaryText("Launch a local dedicated server for testing multiplayer in the editor.");

        if (!m_pieServerRunning)
        {
            if (Spark::Ui::BeginPropertyGrid("PIEQuickSetup"))
            {
                Spark::Ui::PropertyLabel("Name");
                char nameBuf[128];
                strncpy(nameBuf, m_pieConfig.serverName.c_str(), sizeof(nameBuf) - 1);
                nameBuf[sizeof(nameBuf) - 1] = '\0';
                if (ImGui::InputText("##PIEName", nameBuf, sizeof(nameBuf)))
                {
                    m_pieConfig.serverName = nameBuf;
                }

                Spark::Ui::PropertyLabel("Port");
                int port = static_cast<int>(m_pieConfig.port);
                if (ImGui::InputInt("##PIEPort", &port))
                {
                    m_pieConfig.port = static_cast<uint16_t>(std::clamp(port, 1024, 65535));
                }

                Spark::Ui::PropertyLabel("Max Players");
                ImGui::SliderInt("##PIEMaxPlayers", &m_pieConfig.maxPlayers, 2, 32);

                Spark::Ui::PropertyLabel("Game Mode");
                {
                    const Spark::Ui::ScopedDisabled disabled(
                        true, "Unavailable: SparkServer does not accept a game-mode option yet.");
                    ImGui::Combo("##PIEGameMode", &m_pieConfig.gameMode, GAME_MODE_NAMES, NUM_GAME_MODES);
                }

                // Map selection from rotation
                Spark::Ui::PropertyLabel("Map");
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

                    if (ImGui::Combo("##PIEMap", &mapIdx, mapNames.c_str()))
                    {
                        m_pieConfig.mapName = m_mapRotation[static_cast<size_t>(mapIdx)];
                    }
                }
                else
                {
                    char mapBuf[256];
                    strncpy(mapBuf, m_pieConfig.mapName.c_str(), sizeof(mapBuf) - 1);
                    mapBuf[sizeof(mapBuf) - 1] = '\0';
                    if (ImGui::InputText("##PIEMap", mapBuf, sizeof(mapBuf)))
                    {
                        m_pieConfig.mapName = mapBuf;
                    }
                }
                Spark::Ui::EndPropertyGrid();
            }

            ImGui::Spacing();
            m_pieConfig.openConsoleWindow = false;
            m_pieConfig.autoConnect = false;
            Spark::Ui::UnavailableFeature("Console window and editor auto-connect",
                                          "Server output is streamed here; the editor client connection transport "
                                          "is not integrated yet.");

            ImGui::Spacing();

            // Launch button
            const float fullWidth = ImGui::GetContentRegionAvail().x;
            if (Spark::Ui::PrimaryButton(ICON_FA_PLAY " Launch PIE Dedicated Server",
                                         ImVec2(fullWidth, Spark::Ui::Px(44.0f))))
            {
                LaunchPIEServer();
            }
        }
        else
        {
            // Running state
            Spark::Ui::StatusText(Spark::Ui::Status::Success, "PIE Server Running");
            ImGui::SameLine();
            const int upMins = static_cast<int>(m_pieServerUptime) / 60;
            const int upSecs = static_cast<int>(m_pieServerUptime) % 60;
            Spark::Ui::SecondaryText("(Uptime: %d:%02d)", upMins, upSecs);

            if (Spark::Ui::BeginPropertyGrid("PIERunning"))
            {
                Spark::Ui::PropertyLabel("Server");
                ImGui::TextUnformatted(m_pieConfig.serverName.c_str());
                Spark::Ui::PropertyLabel("Port");
                Spark::Ui::MonoText("%d", m_pieConfig.port);
                Spark::Ui::PropertyLabel("Map");
                ImGui::TextUnformatted(m_pieConfig.mapName.c_str());
                Spark::Ui::PropertyLabel("Mode");
                ImGui::TextUnformatted(GAME_MODE_NAMES[m_pieConfig.gameMode]);
                Spark::Ui::PropertyLabel("Max Players");
                Spark::Ui::MonoText("%d", m_pieConfig.maxPlayers);
                Spark::Ui::EndPropertyGrid();
            }

            ImGui::Separator();

            // Server console
            RenderPIEServerConsole();

            ImGui::Separator();

            // Stop button
            const float fullWidth = ImGui::GetContentRegionAvail().x;
            if (Spark::Ui::DangerButton(ICON_FA_STOP " Stop PIE Server", ImVec2(fullWidth, Spark::Ui::Px(36.0f))))
            {
                StopPIEServer();
            }
        }
    }

    // ============================================================================
    // Tab: Server Browser
    // ============================================================================

    void DedicatedServerPanel::RenderServerBrowserTab()
    {
        Spark::Ui::SubHeader(ICON_FA_GLOBE " LAN Server Browser");

        // Refresh button. There is no LAN discovery transport, so this only
        // re-reads the locally launched PIE server.
        if (Spark::Ui::SecondaryButton(ICON_FA_REFRESH " Refresh"))
        {
            RefreshServerBrowser();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        Spark::Ui::SecondaryText("%d server(s) listed", static_cast<int>(m_discoveredServers.size()));
        Spark::Ui::UnavailableFeature("LAN discovery",
                                      "No LAN discovery transport exists: only a server launched from this editor "
                                      "is listed, and joining needs a client transport that is not wired.");

        ImGui::Separator();

        // Server list
        if (m_discoveredServers.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_GLOBE, "No servers listed",
                                  "Launch a PIE server, then press Refresh to list it here.");
        }
        else if (ImGui::BeginTable("##ServerList", 6,
                                   ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_ScrollY,
                                   ImVec2(0.0f, ImGui::GetContentRegionAvail().y - Spark::Ui::Px(40.0f))))
        {
            ImGui::TableSetupColumn("Server Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(120.0f));
            ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(100.0f));
            ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(70.0f));
            ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(60.0f));
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(60.0f));
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_discoveredServers.size()); ++i)
            {
                const auto& server = m_discoveredServers[static_cast<size_t>(i)];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(server.name.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(server.map.c_str());

                ImGui::TableSetColumnIndex(2);
                if (server.gameMode >= 0 && server.gameMode < NUM_GAME_MODES)
                    ImGui::TextUnformatted(GAME_MODE_NAMES[server.gameMode]);
                else
                    Spark::Ui::SecondaryText("Unknown");

                ImGui::TableSetColumnIndex(3);
                Spark::Ui::TableNumber("%d/%d", server.currentPlayers, server.maxPlayers);

                ImGui::TableSetColumnIndex(4);
                {
                    const Spark::Ui::Role pingRole = server.ping < 50.0f    ? Spark::Ui::Role::SuccessText
                                                     : server.ping < 100.0f ? Spark::Ui::Role::WarningText
                                                                            : Spark::Ui::Role::ErrorText;
                    const Spark::Ui::ScopedRoleColor pingColor(ImGuiCol_Text, pingRole);
                    Spark::Ui::TableNumber("%.0f ms", server.ping);
                }

                ImGui::TableSetColumnIndex(5);
                char joinBtnId[32];
                snprintf(joinBtnId, sizeof(joinBtnId), "Join##%d", i);
                const Spark::Ui::ScopedDisabled disabled(true, "Unavailable: no editor client transport is wired.");
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
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Spark::Ui::Px(80.0f));
        ImGui::InputText("##DirectConnect", directAddr, sizeof(directAddr));
        ImGui::SameLine();
        if (Spark::Ui::SecondaryButton("Connect", Spark::Ui::Px2(72, 0)))
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

        if (!m_connectStatus.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            Spark::Ui::StatusText(Spark::Ui::Status::Warning, "%s", m_connectStatus.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // ============================================================================
    // Tab: Monitor
    // ============================================================================

    void DedicatedServerPanel::RenderMonitorTab()
    {
        if (!m_pieServerRunning)
        {
            Spark::Ui::EmptyState(ICON_FA_CHART_BAR, "No server running",
                                  "Launch a PIE server from the PIE Server tab to monitor it here.");
            return;
        }

        Spark::Ui::SubHeader(ICON_FA_CHART_BAR " Server Monitor");

        // Stats
        RenderServerStats();

        ImGui::Separator();

        // Player list
        if (Spark::Ui::SectionHeader("Connected Players", ICON_FA_USERS))
        {
            RenderPlayerList();
        }

        // RCON Console
        if (Spark::Ui::SectionHeader("RCON Console", ICON_FA_TERMINAL))
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
            ImGui::ProgressBar(m_cookProgress, ImVec2(-1.0f, Spark::Ui::Px(20.0f)));
            Spark::Ui::SecondaryText("%s", m_cookStatus.c_str());
        }
    }

    void DedicatedServerPanel::RenderCookLog()
    {
        if (Spark::Ui::SectionHeader("Cook Log", ICON_FA_TERMINAL))
        {
            ImGui::BeginChild("##CookLog", Spark::Ui::Px2(0, 120), true);
            for (const auto& entry : m_cookLog)
            {
                if (entry.level == "warning")
                    Spark::Ui::StatusText(Spark::Ui::Status::Warning, "%s", entry.message.c_str());
                else if (entry.level == "error")
                    Spark::Ui::StatusText(Spark::Ui::Status::Error, "%s", entry.message.c_str());
                else if (entry.level == "success")
                    Spark::Ui::StatusText(Spark::Ui::Status::Success, "%s", entry.message.c_str());
                else
                    Spark::Ui::MonoText("%s", entry.message.c_str());
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
        Spark::Ui::SubHeader(ICON_FA_TERMINAL " Server Console");
        ImGui::BeginChild("##PIEConsole", Spark::Ui::Px2(0, 150), true);
        for (const auto& line : m_pieServerLog)
        {
            // The source tag at the start of each line picks its colour; the tag itself stays visible.
            Spark::Ui::Role role = Spark::Ui::Role::TextPrimary;
            if (line.starts_with("[Server]"))
                role = Spark::Ui::Role::InfoText;
            else if (line.starts_with("[Client]"))
                role = Spark::Ui::Role::SuccessText;
            else if (line.starts_with("[RCON]"))
                role = Spark::Ui::Role::WarningText;
            const Spark::Ui::ScopedRoleColor color(ImGuiCol_Text, role);
            Spark::Ui::MonoText("%s", line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        Spark::Ui::UnavailableFeature("Remote console",
                                      "Remote console is disabled until SparkServer exposes an authenticated RCON "
                                      "transport.");
    }

    // ============================================================================
    // Server Browser Helpers
    // ============================================================================

    void DedicatedServerPanel::RefreshServerBrowser()
    {
        // There is no discovery socket: the only server this editor can see is one
        // it launched itself. The list is rebuilt immediately, with no scan delay
        // that would suggest a network search is happening.
        m_discoveredServers.clear();

        if (m_pieServerRunning)
        {
            DiscoveredServer local;
            local.name = m_pieConfig.serverName;
            local.map = m_pieConfig.mapName;
            local.address = "127.0.0.1";
            local.port = m_pieConfig.port;
            local.currentPlayers = GetReportedPlayerCount();
            local.maxPlayers = m_pieConfig.maxPlayers;
            local.gameMode = m_pieConfig.gameMode;
            local.ping = 0.0f;
            local.discoveredAt = std::chrono::steady_clock::now();
            m_discoveredServers.push_back(local);
        }
    }

    int DedicatedServerPanel::GetReportedPlayerCount() const
    {
        if (!m_serverProcess)
            return -1;

        const std::string healthJson = m_serverProcess->GetSnapshot().healthJson;
        const std::string key = "\"players\":";
        const size_t keyPos = healthJson.find(key);
        if (keyPos == std::string::npos)
            return -1;

        size_t cursor = keyPos + key.size();
        while (cursor < healthJson.size() && std::isspace(static_cast<unsigned char>(healthJson[cursor])))
            ++cursor;

        int players = 0;
        bool anyDigit = false;
        while (cursor < healthJson.size() && std::isdigit(static_cast<unsigned char>(healthJson[cursor])))
        {
            players = players * 10 + (healthJson[cursor] - '0');
            anyDigit = true;
            ++cursor;
        }
        return anyDigit ? players : -1;
    }

    void DedicatedServerPanel::ConnectToServer(const DiscoveredServer& server)
    {
        // There is no editor client transport, so this can never connect. Say so every time: gating the
        // message on m_pieServerRunning made Direct Connect produce no log line and no UI feedback at
        // all when no PIE server was running, which looks exactly like a successful connect.
        const std::string target = server.address + ":" + std::to_string(server.port);
        m_connectStatus = "Cannot connect to " + target + ": no editor client transport is wired.";
        if (m_pieServerRunning)
        {
            m_pieServerLog.push_back("[Editor] Client connect requested for " + target +
                                     "; no editor client transport is wired");
        }
        SPARK_LOG_WARN(Spark::LogCategory::Editor, "%s", m_connectStatus.c_str());
    }

    // ============================================================================
    // Monitor Helpers
    // ============================================================================

    void DedicatedServerPanel::RenderPlayerList()
    {
        ImGui::BeginChild("##PlayerList", Spark::Ui::Px2(0, 100), true);
        Spark::Ui::SecondaryText("SparkServer health reports aggregate player count only.");
        ImGui::EndChild();
    }

    void DedicatedServerPanel::RenderRconConsole()
    {
        ImGui::BeginChild("##RconConsole", Spark::Ui::Px2(0, 100), true);
        for (const auto& line : m_rconHistory)
        {
            ImGui::TextWrapped("%s", line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        Spark::Ui::UnavailableFeature("RCON commands", "No authenticated RCON transport is configured.");
    }

    void DedicatedServerPanel::RenderServerStats()
    {
        if (!Spark::Ui::BeginPropertyGrid("ServerStats"))
            return;

        const int upMins = static_cast<int>(m_pieServerUptime) / 60;
        const int upSecs = static_cast<int>(m_pieServerUptime) % 60;
        Spark::Ui::PropertyLabel("Uptime");
        Spark::Ui::MonoText("%d:%02d", upMins, upSecs);
        Spark::Ui::PropertyLabel("Tick Rate");
        Spark::Ui::MonoText("%.0f Hz", m_pieConfig.tickRate);
        Spark::Ui::PropertyLabel("Players");
        const int reportedPlayers = GetReportedPlayerCount();
        if (reportedPlayers >= 0)
            Spark::Ui::MonoText("%d/%d", reportedPlayers, m_pieConfig.maxPlayers);
        else
            Spark::Ui::SecondaryText("No health report yet");

        Spark::Ui::PropertyLabel("Map");
        ImGui::TextUnformatted(m_pieConfig.mapName.c_str());
        Spark::Ui::PropertyLabel("Mode");
        ImGui::TextUnformatted(GAME_MODE_NAMES[m_pieConfig.gameMode]);
        Spark::Ui::PropertyLabel("Port");
        Spark::Ui::MonoText("%d", m_pieConfig.port);

        if (m_serverProcess)
        {
            const auto snapshot = m_serverProcess->GetSnapshot();
            if (!snapshot.healthJson.empty())
            {
                Spark::Ui::PropertyLabel("Health");
                ImGui::PushTextWrapPos(0.0f);
                Spark::Ui::MonoText("%s", snapshot.healthJson.c_str());
                ImGui::PopTextWrapPos();
            }
            if (snapshot.exitCode)
            {
                Spark::Ui::PropertyLabel("Last exit code");
                Spark::Ui::MonoText("%d", *snapshot.exitCode);
            }
        }
        Spark::Ui::EndPropertyGrid();
    }

} // namespace SparkEditor
