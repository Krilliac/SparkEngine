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
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace SparkEditor
{
    namespace
    {
        /// Categorical colour role of a spawn team; teams also carry their own icon, so the colour is
        /// never the only cue.
        Spark::Ui::Role TeamRole(int teamId)
        {
            if (teamId == 1)
            {
                return Spark::Ui::Role::Category5; // Team Alpha: blue
            }
            if (teamId == 2)
            {
                return Spark::Ui::Role::Category6; // Team Bravo: vermillion
            }
            return Spark::Ui::Role::TextPrimary; // Free for all
        }
    } // namespace

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
        {
            return;
        }

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
        if (Spark::Ui::DangerButton(ICON_FA_TRASH " Remove") && m_selectedSpawn >= 0)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "FPSToolsPanel: removed spawn point '%s'",
                           m_spawnPoints[m_selectedSpawn].name.c_str());
            m_spawnPoints.erase(m_spawnPoints.begin() + m_selectedSpawn);
            m_selectedSpawn = m_spawnPoints.empty() ? -1 : std::min(m_selectedSpawn, (int)m_spawnPoints.size() - 1);
        }
        ImGui::Separator();

        // Spawn point list
        ImGui::BeginChild("SpawnList", Spark::Ui::Px2(200, 0), true);
        for (int i = 0; i < (int)m_spawnPoints.size(); i++)
        {
            auto& sp = m_spawnPoints[i];
            const char* teamIcon = sp.teamId == 1 ? ICON_FA_SHIELD " " : sp.teamId == 2 ? ICON_FA_FLAG " " : "";
            ImGui::PushStyleColor(ImGuiCol_Text, Spark::Ui::ColV(TeamRole(sp.teamId), sp.active ? 1.0f : 0.4f));
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

            // Minimap preview of spawn positions
            Spark::Ui::SubHeader(ICON_FA_MAP " Spawn Overview");
            const ImVec2 mapSize = Spark::Ui::Px2(200.0f, 200.0f);
            ImVec2 mapPos = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            const float mapRounding = Spark::Ui::Px(4.0f);
            dl->AddRectFilled(mapPos, ImVec2(mapPos.x + mapSize.x, mapPos.y + mapSize.y),
                              Spark::Ui::Col(Spark::Ui::Role::SurfaceSunken), mapRounding);
            dl->AddRect(mapPos, ImVec2(mapPos.x + mapSize.x, mapPos.y + mapSize.y),
                        Spark::Ui::Col(Spark::Ui::Role::BorderSubtle), mapRounding);

            // Grid
            for (int g = 1; g < 8; g++)
            {
                float gx = mapPos.x + g * mapSize.x / 8;
                float gy = mapPos.y + g * mapSize.y / 8;
                const ImU32 gridColor = Spark::Ui::Col(Spark::Ui::Role::BorderSubtle, 0.5f);
                dl->AddLine(ImVec2(gx, mapPos.y), ImVec2(gx, mapPos.y + mapSize.y), gridColor);
                dl->AddLine(ImVec2(mapPos.x, gy), ImVec2(mapPos.x + mapSize.x, gy), gridColor);
            }

            // Draw spawn points
            float mapCX = mapPos.x + mapSize.x * 0.5f;
            float mapCY = mapPos.y + mapSize.y * 0.5f;
            const float mapScale = Spark::Ui::Px(4.0f); // map pixels per world unit

            for (int i = 0; i < (int)m_spawnPoints.size(); i++)
            {
                const auto& s = m_spawnPoints[i];
                float sx = mapCX + s.x * mapScale;
                float sy = mapCY + s.z * mapScale;

                if (sx < mapPos.x || sx > mapPos.x + mapSize.x || sy < mapPos.y || sy > mapPos.y + mapSize.y)
                {
                    continue;
                }

                const ImU32 col = Spark::Ui::Col(TeamRole(s.teamId), s.active ? 0.86f : 0.4f);

                if (i == m_selectedSpawn)
                {
                    dl->AddCircle(ImVec2(sx, sy), Spark::Ui::Px(8.0f),
                                  Spark::Ui::Col(Spark::Ui::Role::TextPrimary, .78f), 12, Spark::Ui::Px(2.0f));
                }

                // Direction arrow
                float rad = MathUtils::DegreesToRadians(s.rotY);
                const float arrowLen = Spark::Ui::Px(8.0f);
                dl->AddCircleFilled(ImVec2(sx, sy), Spark::Ui::Px(4.0f), col, 8);
                dl->AddLine(ImVec2(sx, sy), ImVec2(sx + sinf(rad) * arrowLen, sy - cosf(rad) * arrowLen), col,
                            Spark::Ui::Px(2.0f));
            }

            ImGui::Dummy(mapSize);
        }
        else
        {
            Spark::Ui::EmptyState(ICON_FA_FLAG, "No spawn point selected", "Select a spawn point to edit");
        }
        ImGui::EndChild();
    }

    void FPSToolsPanel::RenderGameModeConfig()
    {
        Spark::Ui::SubHeader(ICON_FA_GAMEPAD " Game Mode Configuration");

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
        Spark::Ui::SubHeader(ICON_FA_COG " Rules");

        if (Spark::Ui::BeginPropertyGrid("GameModeRules"))
        {
            Spark::Ui::PropertyLabel("Score Limit");
            ImGui::SliderInt("##ScoreLimit", &m_scoreLimit, 0, 500);

            Spark::Ui::PropertyLabel("Time Limit (sec)");
            ImGui::SliderFloat("##TimeLimit", &m_timeLimit, 0, 3600, "%.0f");

            Spark::Ui::PropertyLabel("Respawn Delay");
            ImGui::SliderFloat("##RespawnDelay", &m_respawnDelay, 0, 30, "%.1f s");

            Spark::Ui::PropertyLabel("Teams Enabled");
            ImGui::Checkbox("##Teams", &m_teamsEnabled);

            Spark::Ui::PropertyLabel("Friendly Fire");
            ImGui::Checkbox("##FriendlyFire", &m_friendlyFire);
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Spacing();
        Spark::Ui::SubHeader(ICON_FA_BOLT " Modifiers");

        if (Spark::Ui::BeginPropertyGrid("Modifiers"))
        {
            Spark::Ui::PropertyLabel("Damage Multiplier");
            ImGui::SliderFloat("##DamageMult", &m_damageMultiplier, 0.1f, 5.0f, "%.1fx");

            Spark::Ui::PropertyLabel("Health Multiplier");
            ImGui::SliderFloat("##HealthMult", &m_healthMultiplier, 0.1f, 5.0f, "%.1fx");

            Spark::Ui::PropertyLabel("Speed Multiplier");
            ImGui::SliderFloat("##SpeedMult", &m_speedMultiplier, 0.1f, 5.0f, "%.1fx");

            Spark::Ui::PropertyLabel("Headshots");
            ImGui::Checkbox("##Headshots", &m_headshots);

            if (m_headshots)
            {
                Spark::Ui::PropertyLabel("Headshot Multiplier");
                ImGui::SliderFloat("##HSMult", &m_headshotMultiplier, 1.0f, 10.0f, "%.1fx");
            }
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_PLAY " Apply & Start Match", ImVec2(-1.0f, Spark::Ui::Px(30.0f))))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "FPSToolsPanel: applying game mode settings and starting match");
            // Apply game mode settings
        }
    }

    void FPSToolsPanel::RenderPlayerConfig()
    {
        Spark::Ui::SubHeader(ICON_FA_USER " Player Configuration");

        Spark::Ui::SubHeader(ICON_FA_HEART " Health & Armor");
        if (Spark::Ui::BeginPropertyGrid("PlayerHealth"))
        {
            Spark::Ui::PropertyLabel("Max Health");
            ImGui::SliderFloat("##MaxHealth", &m_playerHealth, 1.0f, 1000.0f, "%.0f");

            Spark::Ui::PropertyLabel("Max Armor");
            ImGui::SliderFloat("##MaxArmor", &m_playerArmor, 0.0f, 500.0f, "%.0f");
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Spacing();
        Spark::Ui::SubHeader(ICON_FA_ARROWS_ALT " Movement");
        if (Spark::Ui::BeginPropertyGrid("PlayerMovement"))
        {
            Spark::Ui::PropertyLabel("Move Speed");
            ImGui::SliderFloat("##MoveSpeed", &m_playerSpeed, 1.0f, 30.0f, "%.1f");

            Spark::Ui::PropertyLabel("Jump Height");
            ImGui::SliderFloat("##JumpHeight", &m_playerJumpHeight, 0.5f, 20.0f, "%.1f");

            Spark::Ui::PropertyLabel("Gravity");
            ImGui::SliderFloat("##Gravity", &m_playerGravity, 1.0f, 50.0f, "%.1f");

            Spark::Ui::PropertyLabel("Friction");
            ImGui::SliderFloat("##Friction", &m_playerFriction, 0.0f, 1.0f, "%.2f");
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Spacing();
        Spark::Ui::SubHeader(ICON_FA_CAMERA " Camera");
        if (Spark::Ui::BeginPropertyGrid("PlayerCamera"))
        {
            Spark::Ui::PropertyLabel("Mouse Sensitivity");
            ImGui::SliderFloat("##Sensitivity", &m_mouseSensitivity, 0.1f, 10.0f, "%.1f");

            Spark::Ui::PropertyLabel("Field of View");
            ImGui::SliderFloat("##FOV", &m_fov, 60.0f, 120.0f,
                               "%.0f"
                               "\xc2\xb0");
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_CHECK " Apply Player Settings", ImVec2(-1.0f, Spark::Ui::Px(30.0f))))
        {
            // Apply settings to player
        }
    }

    void FPSToolsPanel::RenderCombatTesting()
    {
        Spark::Ui::SubHeader(ICON_FA_CROSSHAIRS " Combat Testing Calculator");

        Spark::Ui::SecondaryText("Scenario Setup");
        if (Spark::Ui::BeginPropertyGrid("CombatSetup"))
        {
            Spark::Ui::PropertyLabel("Incoming Damage");
            ImGui::SliderFloat("##TestDmg", &m_testDamage, 1.0f, 500.0f, "%.1f");

            Spark::Ui::PropertyLabel("Target Health");
            ImGui::SliderFloat("##TestHP", &m_testHealth, 1.0f, 1000.0f, "%.0f");

            Spark::Ui::PropertyLabel("Target Armor");
            ImGui::SliderFloat("##TestArm", &m_testArmor, 0.0f, 500.0f, "%.0f");

            Spark::Ui::PropertyLabel("Distance (units)");
            ImGui::SliderFloat("##TestDist", &m_testDistance, 1.0f, 200.0f, "%.1f");
            Spark::Ui::EndPropertyGrid();
        }

        ImGui::Spacing();
        Spark::Ui::SubHeader(ICON_FA_CHART_BAR " Damage Calculation Results");

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

        if (Spark::Ui::BeginPropertyGrid("DamageResults"))
        {
            Spark::Ui::PropertyLabel("Health Damage");
            Spark::Ui::MonoText("%.1f", healthDamage);

            Spark::Ui::PropertyLabel("Armor Absorbed");
            Spark::Ui::MonoText("%.1f", armorDamage);

            Spark::Ui::PropertyLabel("Remaining Health");
            float remainHP = std::max(0.0f, m_testHealth - healthDamage);
            Spark::Ui::StatusText(remainHP > 0 ? Spark::Ui::Status::Success : Spark::Ui::Status::Error, "%.1f",
                                  remainHP);

            Spark::Ui::PropertyLabel("Remaining Armor");
            Spark::Ui::MonoText("%.1f", std::max(0.0f, m_testArmor - armorDamage));

            Spark::Ui::PropertyLabel("Shots to Kill");
            Spark::Ui::MonoText("%d", shotsToKill);

            Spark::Ui::PropertyLabel("Headshot Damage");
            Spark::Ui::MonoText("%.1f (%.1fx)", headshotDmg, m_headshotMultiplier);
            Spark::Ui::EndPropertyGrid();
        }

        // Visual damage breakdown
        ImGui::Spacing();
        Spark::Ui::SubHeader("Damage Breakdown");

        float totalBar = m_testHealth + m_testArmor;
        if (totalBar > 0)
        {
            ImVec2 barPos = ImGui::GetCursorScreenPos();
            float barW = ImGui::GetContentRegionAvail().x;
            const float barH = Spark::Ui::Px(24.0f);
            const float barRounding = Spark::Ui::Px(3.0f);
            const float labelInset = Spark::Ui::Px(4.0f);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Background
            dl->AddRectFilled(barPos, ImVec2(barPos.x + barW, barPos.y + barH),
                              Spark::Ui::Col(Spark::Ui::Role::SurfaceSunken), barRounding);

            // Armor portion
            float armorW = (m_testArmor / totalBar) * barW;
            dl->AddRectFilled(ImVec2(barPos.x, barPos.y), ImVec2(barPos.x + armorW, barPos.y + barH),
                              Spark::Ui::Col(Spark::Ui::Role::Secondary, 0.7f), barRounding);

            // Health portion
            dl->AddRectFilled(ImVec2(barPos.x + armorW, barPos.y), ImVec2(barPos.x + barW, barPos.y + barH),
                              Spark::Ui::Col(Spark::Ui::Role::SuccessText, 0.7f), barRounding);

            // Damage marker
            float dmgX = barPos.x + armorW + (healthDamage / m_testHealth) * (barW - armorW);
            dl->AddLine(ImVec2(dmgX, barPos.y), ImVec2(dmgX, barPos.y + barH),
                        Spark::Ui::Col(Spark::Ui::Role::ErrorText), Spark::Ui::Px(2.0f));

            // Labels
            const ImU32 labelColor = Spark::Ui::Col(Spark::Ui::Role::TextOnAccent);
            dl->AddText(ImVec2(barPos.x + labelInset, barPos.y + labelInset), labelColor, ICON_FA_SHIELD " Armor");
            dl->AddText(ImVec2(barPos.x + armorW + labelInset, barPos.y + labelInset), labelColor,
                        ICON_FA_HEART " Health");

            ImGui::Dummy(ImVec2(barW, barH + labelInset));
        }
    }

    void FPSToolsPanel::RenderLevelDesignTools()
    {
        Spark::Ui::SubHeader(ICON_FA_MAP " Level Design Tools");

        if (Spark::Ui::SectionHeader("Explosive Volumes", ICON_FA_BOMB))
        {
            Spark::Ui::SecondaryText("Place explosive barrels, gas tanks, and environmental hazards.");
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

        if (Spark::Ui::SectionHeader("Cover Points", ICON_FA_SHIELD, false))
        {
            Spark::Ui::SecondaryText("Mark positions where AI and players can take cover.");
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

        if (Spark::Ui::SectionHeader("Objective Markers", ICON_FA_BULLSEYE, false))
        {
            Spark::Ui::SecondaryText("Place capture points, bomb sites, and flag positions.");
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

        if (Spark::Ui::SectionHeader("Pickup Spawns", ICON_FA_CUBE, false))
        {
            Spark::Ui::SecondaryText("Configure weapon, ammo, health, and armor pickup locations.");

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

        if (Spark::Ui::SectionHeader("Navigation", ICON_FA_LOCATION_ARROW, false))
        {
            Spark::Ui::SecondaryText("AI navigation mesh and pathfinding tools.");
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
