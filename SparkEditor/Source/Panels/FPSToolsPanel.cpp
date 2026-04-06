/**
 * @file FPSToolsPanel.cpp
 * @brief FPS-specific development tools
 * @author Spark Engine Team
 * @date 2025
 */

#include "FPSToolsPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include "Utils/MathUtils.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>
#include <cmath>

namespace SparkEditor
{

    FPSToolsPanel::FPSToolsPanel() : EditorPanel("FPS Tools", "fps_tools_panel") {}

    bool FPSToolsPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "FPSToolsPanel initialized");

        // Default spawn points
        m_spawnPoints = {
            {"Alpha Spawn 1", 10.0f, 0.0f, 10.0f, 0.0f, 1, true},
            {"Alpha Spawn 2", 12.0f, 0.0f, 8.0f, 45.0f, 1, true},
            {"Alpha Spawn 3", 8.0f, 0.0f, 12.0f, -45.0f, 1, true},
            {"Bravo Spawn 1", -10.0f, 0.0f, -10.0f, 180.0f, 2, true},
            {"Bravo Spawn 2", -12.0f, 0.0f, -8.0f, 135.0f, 2, true},
            {"FFA Spawn 1", 0.0f, 0.0f, 20.0f, 0.0f, 0, true},
            {"FFA Spawn 2", 20.0f, 0.0f, 0.0f, 270.0f, 0, true},
            {"FFA Spawn 3", -20.0f, 0.0f, 0.0f, 90.0f, 0, true},
        };

        return true;
    }

    void FPSToolsPanel::Update(float deltaTime)
    {
        m_totalTime += deltaTime;
    }

    void FPSToolsPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            // Tab bar
            if (ImGui::BeginTabBar("FPSToolsTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_FLAG " Spawns"))
                {
                    m_activeTab = ActiveTab::SpawnPoints;
                    RenderSpawnPointEditor();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_GAMEPAD " Game Mode"))
                {
                    m_activeTab = ActiveTab::GameMode;
                    RenderGameModeConfig();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_USER " Player"))
                {
                    m_activeTab = ActiveTab::Player;
                    RenderPlayerConfig();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_CROSSHAIRS " Combat"))
                {
                    m_activeTab = ActiveTab::Combat;
                    RenderCombatTesting();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_MAP " Level"))
                {
                    m_activeTab = ActiveTab::LevelDesign;
                    RenderLevelDesignTools();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void FPSToolsPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "FPSToolsPanel shutting down");
    }

    void FPSToolsPanel::RenderSpawnPointEditor()
    {
        // Toolbar
        if (ImGui::Button(ICON_FA_PLUS " Add Spawn"))
        {
            SpawnPointData sp;
            sp.name = "New Spawn " + std::to_string(m_spawnPoints.size() + 1);
            sp.x = sp.y = sp.z = 0.0f;
            sp.rotY = 0.0f;
            sp.teamId = 0;
            sp.active = true;
            m_spawnPoints.push_back(sp);
            m_selectedSpawn = (int)m_spawnPoints.size() - 1;
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "FPSToolsPanel: added spawn point '%s'", sp.name.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH " Remove") && m_selectedSpawn >= 0)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "FPSToolsPanel: removed spawn point '%s'",
                           m_spawnPoints[m_selectedSpawn].name.c_str());
            m_spawnPoints.erase(m_spawnPoints.begin() + m_selectedSpawn);
            m_selectedSpawn = m_spawnPoints.empty() ? -1 : std::min(m_selectedSpawn, (int)m_spawnPoints.size() - 1);
        }
        ImGui::Separator();

        // Spawn point list
        ImGui::BeginChild("SpawnList", ImVec2(200, 0), true);
        for (int i = 0; i < (int)m_spawnPoints.size(); i++)
        {
            auto& sp = m_spawnPoints[i];
            const char* teamIcon = sp.teamId == 1 ? ICON_FA_SHIELD " " : sp.teamId == 2 ? ICON_FA_FLAG " " : "";
            ImVec4 teamColor = sp.teamId == 1   ? ImVec4(0.2f, 0.5f, 0.9f, 1.0f)
                               : sp.teamId == 2 ? ImVec4(0.9f, 0.3f, 0.2f, 1.0f)
                                                : ImVec4(0.8f, 0.8f, 0.8f, 1.0f);

            if (!sp.active)
                teamColor.w = 0.4f;

            ImGui::PushStyleColor(ImGuiCol_Text, teamColor);
            char label[64];
            snprintf(label, sizeof(label), "%s%s##sp%d", teamIcon, sp.name.c_str(), i);
            if (ImGui::Selectable(label, m_selectedSpawn == i))
            {
                m_selectedSpawn = i;
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Spawn point properties
        ImGui::BeginChild("SpawnProps", ImVec2(0, 0), true);
        if (m_selectedSpawn >= 0 && m_selectedSpawn < (int)m_spawnPoints.size())
        {
            auto& sp = m_spawnPoints[m_selectedSpawn];

            char nameBuf[64];
            snprintf(nameBuf, sizeof(nameBuf), "%s", sp.name.c_str());
            if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
            {
                sp.name = nameBuf;
            }

            ImGui::DragFloat3("Position", &sp.x, 0.1f);
            ImGui::DragFloat("Rotation Y", &sp.rotY, 1.0f, -180.0f, 180.0f,
                             "%.1f"
                             "\xc2\xb0");

            const char* teamLabels[] = {"None (FFA)", "Team Alpha", "Team Bravo"};
            ImGui::Combo("Team", &sp.teamId, teamLabels, 3);
            ImGui::Checkbox("Active", &sp.active);

            ImGui::Separator();

            // Minimap preview of spawn positions
            ImGui::Text(ICON_FA_MAP " Spawn Overview");
            ImVec2 mapSize(200, 200);
            ImVec2 mapPos = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            dl->AddRectFilled(mapPos, ImVec2(mapPos.x + mapSize.x, mapPos.y + mapSize.y), IM_COL32(20, 25, 30, 255),
                              4.0f);
            dl->AddRect(mapPos, ImVec2(mapPos.x + mapSize.x, mapPos.y + mapSize.y), IM_COL32(60, 70, 80, 200), 4.0f);

            // Grid
            for (int g = 1; g < 8; g++)
            {
                float gx = mapPos.x + g * mapSize.x / 8;
                float gy = mapPos.y + g * mapSize.y / 8;
                dl->AddLine(ImVec2(gx, mapPos.y), ImVec2(gx, mapPos.y + mapSize.y), IM_COL32(40, 45, 50, 100));
                dl->AddLine(ImVec2(mapPos.x, gy), ImVec2(mapPos.x + mapSize.x, gy), IM_COL32(40, 45, 50, 100));
            }

            // Draw spawn points
            float mapCX = mapPos.x + mapSize.x * 0.5f;
            float mapCY = mapPos.y + mapSize.y * 0.5f;
            float mapScale = 4.0f;

            for (int i = 0; i < (int)m_spawnPoints.size(); i++)
            {
                const auto& s = m_spawnPoints[i];
                float sx = mapCX + s.x * mapScale;
                float sy = mapCY + s.z * mapScale;

                if (sx < mapPos.x || sx > mapPos.x + mapSize.x || sy < mapPos.y || sy > mapPos.y + mapSize.y)
                    continue;

                ImU32 col = s.teamId == 1   ? IM_COL32(50, 130, 230, s.active ? 220 : 100)
                            : s.teamId == 2 ? IM_COL32(230, 70, 50, s.active ? 220 : 100)
                                            : IM_COL32(200, 200, 200, s.active ? 200 : 80);

                if (i == m_selectedSpawn)
                {
                    dl->AddCircle(ImVec2(sx, sy), 8.0f, IM_COL32(255, 255, 255, 200), 12, 2.0f);
                }

                // Direction arrow
                float rad = MathUtils::DegreesToRadians(s.rotY);
                float arrowLen = 8.0f;
                dl->AddCircleFilled(ImVec2(sx, sy), 4.0f, col, 8);
                dl->AddLine(ImVec2(sx, sy), ImVec2(sx + sinf(rad) * arrowLen, sy - cosf(rad) * arrowLen), col, 2.0f);
            }

            ImGui::Dummy(mapSize);
        }
        else
        {
            ImGui::Text("Select a spawn point to edit");
        }
        ImGui::EndChild();
    }

    void FPSToolsPanel::RenderGameModeConfig()
    {
        ImGui::Text(ICON_FA_GAMEPAD " Game Mode Configuration");
        ImGui::Separator();

        const char* gameModes[] = {"Free Play",   "Deathmatch", "Team Deathmatch",    "Capture the Flag", "Domination",
                                   "Elimination", "Gun Game",   "Search and Destroy", "King of the Hill", "Survival"};
        int prevMode = m_selectedGameMode;
        ImGui::Combo("Game Mode", &m_selectedGameMode, gameModes, IM_ARRAYSIZE(gameModes));
        if (m_selectedGameMode != prevMode)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "FPSToolsPanel: game mode changed to '%s'",
                           gameModes[m_selectedGameMode]);
        }

        ImGui::Spacing();
        ImGui::Text(ICON_FA_COG " Rules");
        ImGui::Separator();

        ImGui::Columns(2, "GameModeRules", false);
        ImGui::SetColumnWidth(0, 160);

        ImGui::Text("Score Limit");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##ScoreLimit", &m_scoreLimit, 0, 500);
        ImGui::NextColumn();

        ImGui::Text("Time Limit (sec)");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##TimeLimit", &m_timeLimit, 0, 3600, "%.0f");
        ImGui::NextColumn();

        ImGui::Text("Respawn Delay");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##RespawnDelay", &m_respawnDelay, 0, 30, "%.1f s");
        ImGui::NextColumn();

        ImGui::Text("Teams Enabled");
        ImGui::NextColumn();
        ImGui::Checkbox("##Teams", &m_teamsEnabled);
        ImGui::NextColumn();

        ImGui::Text("Friendly Fire");
        ImGui::NextColumn();
        ImGui::Checkbox("##FriendlyFire", &m_friendlyFire);
        ImGui::NextColumn();

        ImGui::Columns(1);

        ImGui::Spacing();
        ImGui::Text(ICON_FA_BOLT " Modifiers");
        ImGui::Separator();

        ImGui::Columns(2, "Modifiers", false);
        ImGui::SetColumnWidth(0, 160);

        ImGui::Text("Damage Multiplier");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##DamageMult", &m_damageMultiplier, 0.1f, 5.0f, "%.1fx");
        ImGui::NextColumn();

        ImGui::Text("Health Multiplier");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##HealthMult", &m_healthMultiplier, 0.1f, 5.0f, "%.1fx");
        ImGui::NextColumn();

        ImGui::Text("Speed Multiplier");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##SpeedMult", &m_speedMultiplier, 0.1f, 5.0f, "%.1fx");
        ImGui::NextColumn();

        ImGui::Text("Headshots");
        ImGui::NextColumn();
        ImGui::Checkbox("##Headshots", &m_headshots);
        ImGui::NextColumn();

        if (m_headshots)
        {
            ImGui::Text("Headshot Multiplier");
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##HSMult", &m_headshotMultiplier, 1.0f, 10.0f, "%.1fx");
            ImGui::NextColumn();
        }

        ImGui::Columns(1);

        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_PLAY " Apply & Start Match", ImVec2(-1, 30)))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "FPSToolsPanel: applying game mode settings and starting match");
            // Apply game mode settings
        }
    }

    void FPSToolsPanel::RenderPlayerConfig()
    {
        ImGui::Text(ICON_FA_USER " Player Configuration");
        ImGui::Separator();

        ImGui::Text(ICON_FA_HEART " Health & Armor");
        ImGui::Columns(2, "PlayerHealth", false);
        ImGui::SetColumnWidth(0, 160);

        ImGui::Text("Max Health");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##MaxHealth", &m_playerHealth, 1.0f, 1000.0f, "%.0f");
        ImGui::NextColumn();

        ImGui::Text("Max Armor");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##MaxArmor", &m_playerArmor, 0.0f, 500.0f, "%.0f");
        ImGui::NextColumn();

        ImGui::Columns(1);

        ImGui::Spacing();
        ImGui::Text(ICON_FA_ARROWS_ALT " Movement");
        ImGui::Columns(2, "PlayerMovement", false);
        ImGui::SetColumnWidth(0, 160);

        ImGui::Text("Move Speed");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##MoveSpeed", &m_playerSpeed, 1.0f, 30.0f, "%.1f");
        ImGui::NextColumn();

        ImGui::Text("Jump Height");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##JumpHeight", &m_playerJumpHeight, 0.5f, 20.0f, "%.1f");
        ImGui::NextColumn();

        ImGui::Text("Gravity");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##Gravity", &m_playerGravity, 1.0f, 50.0f, "%.1f");
        ImGui::NextColumn();

        ImGui::Text("Friction");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##Friction", &m_playerFriction, 0.0f, 1.0f, "%.2f");
        ImGui::NextColumn();

        ImGui::Columns(1);

        ImGui::Spacing();
        ImGui::Text(ICON_FA_CAMERA " Camera");
        ImGui::Columns(2, "PlayerCamera", false);
        ImGui::SetColumnWidth(0, 160);

        ImGui::Text("Mouse Sensitivity");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##Sensitivity", &m_mouseSensitivity, 0.1f, 10.0f, "%.1f");
        ImGui::NextColumn();

        ImGui::Text("Field of View");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##FOV", &m_fov, 60.0f, 120.0f,
                           "%.0f"
                           "\xc2\xb0");
        ImGui::NextColumn();

        ImGui::Columns(1);

        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_CHECK " Apply Player Settings", ImVec2(-1, 30)))
        {
            // Apply settings to player
        }
    }

    void FPSToolsPanel::RenderCombatTesting()
    {
        ImGui::Text(ICON_FA_CROSSHAIRS " Combat Testing Calculator");
        ImGui::Separator();

        ImGui::Text("Scenario Setup:");
        ImGui::Columns(2, "CombatSetup", false);
        ImGui::SetColumnWidth(0, 160);

        ImGui::Text("Incoming Damage");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##TestDmg", &m_testDamage, 1.0f, 500.0f, "%.1f");
        ImGui::NextColumn();

        ImGui::Text("Target Health");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##TestHP", &m_testHealth, 1.0f, 1000.0f, "%.0f");
        ImGui::NextColumn();

        ImGui::Text("Target Armor");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##TestArm", &m_testArmor, 0.0f, 500.0f, "%.0f");
        ImGui::NextColumn();

        ImGui::Text("Distance (units)");
        ImGui::NextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##TestDist", &m_testDistance, 1.0f, 200.0f, "%.1f");
        ImGui::NextColumn();

        ImGui::Columns(1);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text(ICON_FA_CHART_BAR " Damage Calculation Results");

        // Calculate damage with armor absorption
        float armorAbsorption = 0.5f; // 50% damage reduction from armor
        float effectiveDamage = m_testDamage;
        float armorDamage = 0.0f;
        float healthDamage = 0.0f;

        if (m_testArmor > 0.0f)
        {
            armorDamage = effectiveDamage * armorAbsorption;
            armorDamage = std::min(armorDamage, m_testArmor);
            healthDamage = effectiveDamage - armorDamage;
        }
        else
        {
            healthDamage = effectiveDamage;
        }

        int shotsToKill = 0;
        float tempHP = m_testHealth;
        float tempArmor = m_testArmor;
        while (tempHP > 0 && shotsToKill < 999)
        {
            float aDmg = m_testDamage * armorAbsorption;
            aDmg = std::min(aDmg, tempArmor);
            float hDmg = m_testDamage - aDmg;
            tempArmor -= aDmg;
            tempHP -= hDmg;
            shotsToKill++;
        }

        float headshotDmg = m_testDamage * m_headshotMultiplier;

        ImGui::Columns(2, "DamageResults", false);
        ImGui::SetColumnWidth(0, 200);

        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Health Damage:");
        ImGui::NextColumn();
        ImGui::Text("%.1f", healthDamage);
        ImGui::NextColumn();

        ImGui::TextColored(ImVec4(0.2f, 0.5f, 0.9f, 1.0f), "Armor Absorbed:");
        ImGui::NextColumn();
        ImGui::Text("%.1f", armorDamage);
        ImGui::NextColumn();

        ImGui::Text("Remaining Health:");
        ImGui::NextColumn();
        float remainHP = std::max(0.0f, m_testHealth - healthDamage);
        ImGui::TextColored(remainHP > 0 ? ImVec4(0.3f, 0.8f, 0.3f, 1) : ImVec4(0.9f, 0.2f, 0.2f, 1), "%.1f", remainHP);
        ImGui::NextColumn();

        ImGui::Text("Remaining Armor:");
        ImGui::NextColumn();
        ImGui::Text("%.1f", std::max(0.0f, m_testArmor - armorDamage));
        ImGui::NextColumn();

        ImGui::Text("Shots to Kill:");
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.1f, 1.0f), "%d", shotsToKill);
        ImGui::NextColumn();

        ImGui::Text("Headshot Damage:");
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "%.1f (%.1fx)", headshotDmg, m_headshotMultiplier);
        ImGui::NextColumn();

        ImGui::Columns(1);

        // Visual damage breakdown
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Damage Breakdown:");

        float totalBar = m_testHealth + m_testArmor;
        if (totalBar > 0)
        {
            ImVec2 barPos = ImGui::GetCursorScreenPos();
            float barW = ImGui::GetContentRegionAvail().x;
            float barH = 24.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Background
            dl->AddRectFilled(barPos, ImVec2(barPos.x + barW, barPos.y + barH), IM_COL32(30, 35, 40, 255), 3.0f);

            // Armor portion
            float armorW = (m_testArmor / totalBar) * barW;
            dl->AddRectFilled(ImVec2(barPos.x, barPos.y), ImVec2(barPos.x + armorW, barPos.y + barH),
                              IM_COL32(45, 140, 240, 180), 3.0f);

            // Health portion
            dl->AddRectFilled(ImVec2(barPos.x + armorW, barPos.y), ImVec2(barPos.x + barW, barPos.y + barH),
                              IM_COL32(76, 175, 80, 180), 3.0f);

            // Damage marker
            float dmgX = barPos.x + armorW + (healthDamage / m_testHealth) * (barW - armorW);
            dl->AddLine(ImVec2(dmgX, barPos.y), ImVec2(dmgX, barPos.y + barH), IM_COL32(229, 57, 53, 255), 2.0f);

            // Labels
            dl->AddText(ImVec2(barPos.x + 4, barPos.y + 4), IM_COL32(255, 255, 255, 220), ICON_FA_SHIELD " Armor");
            dl->AddText(ImVec2(barPos.x + armorW + 4, barPos.y + 4), IM_COL32(255, 255, 255, 220),
                        ICON_FA_HEART " Health");

            ImGui::Dummy(ImVec2(barW, barH + 4));
        }
    }

    void FPSToolsPanel::RenderLevelDesignTools()
    {
        ImGui::Text(ICON_FA_MAP " Level Design Tools");
        ImGui::Separator();

        if (ImGui::CollapsingHeader(ICON_FA_BOMB " Explosive Volumes", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Place explosive barrels, gas tanks, and environmental hazards.");
            if (ImGui::Button("Add Explosive Barrel"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Placing explosive barrel at cursor position");
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Gas Tank"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Placing gas tank at cursor position");
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Mine"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Placing mine at cursor position");
            }
        }

        if (ImGui::CollapsingHeader(ICON_FA_SHIELD " Cover Points"))
        {
            ImGui::Text("Mark positions where AI and players can take cover.");
            if (ImGui::Button("Add Full Cover"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Placing full cover point at cursor position");
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Half Cover"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Placing half cover point at cursor position");
            }
        }

        if (ImGui::CollapsingHeader(ICON_FA_BULLSEYE " Objective Markers"))
        {
            ImGui::Text("Place capture points, bomb sites, and flag positions.");
            if (ImGui::Button("Add Capture Point"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Placing capture point at cursor position");
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Bomb Site"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Placing bomb site at cursor position");
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Flag Post"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Placing flag post at cursor position");
            }
        }

        if (ImGui::CollapsingHeader(ICON_FA_CUBE " Pickup Spawns"))
        {
            ImGui::Text("Configure weapon, ammo, health, and armor pickup locations.");

            static int pickupType = 0;
            const char* pickupTypes[] = {"Health Pack", "Armor Pack", "Ammo Crate", "Weapon Spawn"};
            ImGui::Combo("Pickup Type", &pickupType, pickupTypes, IM_ARRAYSIZE(pickupTypes));

            static float respawnTime = 30.0f;
            ImGui::SliderFloat("Respawn Time", &respawnTime, 5.0f, 120.0f, "%.0f s");

            if (ImGui::Button("Place Pickup at Cursor"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Placing %s pickup (respawn: %.0fs) at cursor",
                               pickupTypes[pickupType], respawnTime);
            }
        }

        if (ImGui::CollapsingHeader(ICON_FA_LOCATION_ARROW " Navigation"))
        {
            ImGui::Text("AI navigation mesh and pathfinding tools.");
            if (ImGui::Button("Generate NavMesh"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Generating navigation mesh for current scene");
            }
            ImGui::SameLine();
            if (ImGui::Button("Show Nav Debug"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Toggling navigation debug visualization");
            }
            ImGui::SameLine();
            if (ImGui::Button("Test Path"))
            {
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Testing pathfinding from cursor position");
            }
        }
    }

} // namespace SparkEditor
