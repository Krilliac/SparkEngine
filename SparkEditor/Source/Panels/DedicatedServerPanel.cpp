/**
 * @file DedicatedServerPanel.cpp
 * @brief Editor panel for dedicated server management, cooking, and PIE launching
 * @author Spark Engine Team
 * @date 2025
 */

#include <algorithm>
#include <iostream>

#include <imgui.h>

#include "DedicatedServerPanel.h"
#include "../Core/EditorIcons.h"

namespace SparkEditor
{

    // ============================================================================
    // Construction / Lifecycle
    // ============================================================================

    DedicatedServerPanel::DedicatedServerPanel() : EditorPanel("Dedicated Server", "dedicated_server_panel")
    {
        m_mapRotation.push_back("dm_warehouse");
        m_mapRotation.push_back("dm_rooftops");
        m_mapRotation.push_back("dm_compound");
    }

    bool DedicatedServerPanel::Initialize()
    {
        std::cout << "Initializing Dedicated Server panel\n";
        return true;
    }

    void DedicatedServerPanel::Update(float deltaTime)
    {
        // Update cook progress simulation
        if (m_isCooking && m_cookProgress < 1.0f)
        {
            m_cookProgress += deltaTime * 0.04f;
            if (m_cookProgress >= 1.0f)
            {
                m_cookProgress = 1.0f;
                m_isCooking = false;
                m_cookStatus = "Cook Complete";
                m_cookLog.push_back({"Server cook completed successfully!", "success"});
                m_cookLog.push_back(
                    {"Output: " + m_cookSettings.outputDirectory + "/" + m_cookSettings.executableName, "info"});
            }
        }

        // Update PIE server uptime
        if (m_pieServerRunning)
        {
            m_pieServerUptime += deltaTime;
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
            StopPIEServer();
        }
        std::cout << "Shutting down Dedicated Server panel\n";
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

        // LAN only
        ImGui::Checkbox("LAN Only", &m_pieConfig.lanOnly);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restrict connections to local network only");

        ImGui::Spacing();
        ImGui::Text(ICON_FA_GAMEPAD " Game Settings");
        ImGui::Separator();

        // Game mode
        if (ImGui::Combo("Game Mode", &m_pieConfig.gameMode, GAME_MODE_NAMES, NUM_GAME_MODES))
        {
            SetModified(true);
        }

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

        ImGui::Checkbox("Enable RCON", &m_pieConfig.enableRcon);
        if (m_pieConfig.enableRcon)
        {
            char rconBuf[128];
            strncpy(rconBuf, m_pieConfig.rconPassword.c_str(), sizeof(rconBuf) - 1);
            rconBuf[sizeof(rconBuf) - 1] = '\0';
            if (ImGui::InputText("RCON Password", rconBuf, sizeof(rconBuf), ImGuiInputTextFlags_Password))
            {
                m_pieConfig.rconPassword = rconBuf;
            }
        }
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
        const char* platforms[] = {"Windows x64", "Linux x64", "Linux ARM64"};
        int platformIdx = static_cast<int>(m_cookSettings.platform);
        if (ImGui::Combo("##ServerPlatform", &platformIdx, platforms, 3))
        {
            m_cookSettings.platform = static_cast<ServerPlatform>(platformIdx);
        }

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

        ImGui::Checkbox("Headless (no graphics/GPU)", &m_cookSettings.stripGraphics);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Strip all rendering code. Server runs without a GPU.\nRequired for cloud deployment.");

        ImGui::Checkbox("Strip Audio", &m_cookSettings.stripAudio);
        ImGui::Checkbox("Strip Editor Code", &m_cookSettings.stripEditor);
        ImGui::Checkbox("Include Debug Symbols", &m_cookSettings.includeDebugSymbols);
        ImGui::Checkbox("Compress Assets", &m_cookSettings.compressAssets);
        if (m_cookSettings.compressAssets)
        {
            ImGui::SliderInt("Compression Level", &m_cookSettings.compressionLevel, 1, 9);
        }
        ImGui::Checkbox("Enable Logging", &m_cookSettings.enableLogging);
        ImGui::Checkbox("Enable RCON", &m_cookSettings.enableRcon);

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
        if (ImGui::InputText("Executable Name", exeName, sizeof(exeName)))
        {
            m_cookSettings.executableName = exeName;
        }

        ImGui::Separator();

        // Cook actions
        float halfWidth = (ImGui::GetContentRegionAvail().x - 4.0f) / 2.0f;

        ImVec4 cookColor(0.2f, 0.55f, 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, cookColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.65f, 0.3f, 1.0f));
        if (!m_isCooking)
        {
            if (ImGui::Button(ICON_FA_HAMMER " Cook Server", ImVec2(halfWidth, 36.0f)))
            {
                StartCookServer();
            }
        }
        else
        {
            ImGui::BeginDisabled();
            ImGui::Button(ICON_FA_SPINNER " Cooking...", ImVec2(halfWidth, 36.0f));
            ImGui::EndDisabled();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_DOWNLOAD " Cook & Package", ImVec2(halfWidth, 36.0f)))
        {
            if (!m_isCooking)
            {
                m_cookSettings.compressAssets = true;
                StartCookServer();
            }
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
            ImGui::Combo("Game Mode##PIE", &m_pieConfig.gameMode, GAME_MODE_NAMES, NUM_GAME_MODES);

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
            ImGui::Checkbox("Open Console Window", &m_pieConfig.openConsoleWindow);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Opens a separate console window showing server output");
            ImGui::Checkbox("Auto-Connect Editor Client", &m_pieConfig.autoConnect);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Automatically connect the editor viewport as a client");

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
                direct.port = static_cast<uint16_t>(std::stoi(addrStr.substr(colonPos + 1)));
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
        m_isCooking = true;
        m_cookProgress = 0.0f;
        m_cookStatus = "Cooking dedicated server...";
        m_cookLog.clear();

        m_cookLog.push_back({"Starting " + std::string(GetProfileName(m_cookSettings.profile)) + " server cook for " +
                                 GetPlatformName(m_cookSettings.platform),
                             "info"});
        m_cookLog.push_back({"Headless mode: " + std::string(m_cookSettings.stripGraphics ? "YES" : "NO"), "info"});
        m_cookLog.push_back(
            {"Output: " + m_cookSettings.outputDirectory + "/" + m_cookSettings.executableName, "info"});

        if (m_cookSettings.stripGraphics)
            m_cookLog.push_back({"Stripping: Graphics/GPU subsystem", "warning"});
        if (m_cookSettings.stripAudio)
            m_cookLog.push_back({"Stripping: Audio subsystem", "warning"});
        if (m_cookSettings.stripEditor)
            m_cookLog.push_back({"Stripping: Editor code", "warning"});

        m_cookLog.push_back({"Configuring CMake with -DENABLE_NETWORKING=ON -DSPARK_HEADLESS_SUPPORT=ON...", "info"});
        m_cookLog.push_back({"Compiling server target...", "info"});

        if (m_cookSettings.compressAssets)
            m_cookLog.push_back(
                {"Compressing assets (level " + std::to_string(m_cookSettings.compressionLevel) + ")...", "info"});

        if (!m_mapRotation.empty())
        {
            m_cookLog.push_back(
                {"Packaging " + std::to_string(m_mapRotation.size()) + " maps from rotation...", "info"});
        }
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
        m_pieServerRunning = true;
        m_pieServerUptime = 0.0f;
        m_pieServerLog.clear();

        m_pieServerLog.push_back("[Server] Initializing dedicated server...");
        m_pieServerLog.push_back("[Server] Name: " + m_pieConfig.serverName);
        m_pieServerLog.push_back("[Server] Port: " + std::to_string(m_pieConfig.port));
        m_pieServerLog.push_back("[Server] Map: " + m_pieConfig.mapName);
        m_pieServerLog.push_back("[Server] Mode: " + std::string(GAME_MODE_NAMES[m_pieConfig.gameMode]));
        m_pieServerLog.push_back("[Server] Max players: " + std::to_string(m_pieConfig.maxPlayers));
        m_pieServerLog.push_back("[Server] Tick rate: " + std::to_string(static_cast<int>(m_pieConfig.tickRate)) +
                                 " Hz");
        m_pieServerLog.push_back("[Server] LAN broadcast: " + std::string(m_pieConfig.lanOnly ? "ON" : "OFF"));
        m_pieServerLog.push_back("[Server] RCON: " + std::string(m_pieConfig.enableRcon ? "ENABLED" : "DISABLED"));
        m_pieServerLog.push_back("[Server] --- Server started successfully ---");

        if (m_pieConfig.autoConnect)
        {
            m_pieServerLog.push_back(
                "[Client] Auto-connecting editor viewport to 127.0.0.1:" + std::to_string(m_pieConfig.port) + "...");
            m_pieServerLog.push_back("[Client] Connected as Client 1");
        }
    }

    void DedicatedServerPanel::StopPIEServer()
    {
        m_pieServerLog.push_back("[Server] Shutting down...");
        m_pieServerLog.push_back("[Server] Notifying connected clients...");
        m_pieServerLog.push_back("[Server] Server stopped.");
        m_pieServerRunning = false;
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

        // Input
        bool entered = false;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70);
        if (ImGui::InputText("##PIERcon", m_pieRconInputBuf, sizeof(m_pieRconInputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
        {
            entered = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Send##PIE", ImVec2(62, 0)) || entered)
        {
            if (m_pieRconInputBuf[0] != '\0')
            {
                m_pieServerLog.push_back(std::string("[RCON] > ") + m_pieRconInputBuf);
                // Simulate RCON response
                std::string cmd(m_pieRconInputBuf);
                if (cmd == "status")
                {
                    m_pieServerLog.push_back("[RCON] Server: " + m_pieConfig.serverName);
                    m_pieServerLog.push_back("[RCON] Map: " + m_pieConfig.mapName);
                    int upMins = static_cast<int>(m_pieServerUptime) / 60;
                    int upSecs = static_cast<int>(m_pieServerUptime) % 60;
                    m_pieServerLog.push_back("[RCON] Uptime: " + std::to_string(upMins) + ":" +
                                             (upSecs < 10 ? "0" : "") + std::to_string(upSecs));
                }
                else if (cmd == "help")
                {
                    m_pieServerLog.push_back("[RCON] Commands: status, help, kick, ban, map, say, players, endmatch, "
                                             "nextmap, quit");
                }
                else
                {
                    m_pieServerLog.push_back("[RCON] Executed: " + cmd);
                }
                m_pieRconInputBuf[0] = '\0';
            }
        }
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
            local.ping = 1.0f;
            local.discoveredAt = std::chrono::steady_clock::now();
            m_discoveredServers.push_back(local);
        }
    }

    void DedicatedServerPanel::ConnectToServer(const DiscoveredServer& server)
    {
        // Log connection attempt
        if (m_pieServerRunning)
        {
            m_pieServerLog.push_back("[Client] Connecting to " + server.address + ":" + std::to_string(server.port) +
                                     "...");
            m_pieServerLog.push_back("[Client] Connected to '" + server.name + "'");
        }
    }

    // ============================================================================
    // Monitor Helpers
    // ============================================================================

    void DedicatedServerPanel::RenderPlayerList()
    {
        ImGui::BeginChild("##PlayerList", ImVec2(0, 100), true);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No players connected (PIE simulation)");
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

        bool entered = false;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70);
        if (ImGui::InputText("##RconInput", m_rconInputBuf, sizeof(m_rconInputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
        {
            entered = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Send##Rcon", ImVec2(62, 0)) || entered)
        {
            if (m_rconInputBuf[0] != '\0')
            {
                m_rconHistory.push_back(std::string("> ") + m_rconInputBuf);
                m_rconHistory.push_back("OK");
                m_rconInputBuf[0] = '\0';
            }
        }
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
    }

} // namespace SparkEditor
