/**
 * @file FPSToolsPanel.h
 * @brief FPS-specific development tools panel
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor {

class FPSToolsPanel : public EditorPanel {
public:
    FPSToolsPanel();
    ~FPSToolsPanel() override = default;

    bool Initialize() override;
    void Update(float deltaTime) override;
    void Render() override;
    void Shutdown() override;

private:
    void RenderSpawnPointEditor();
    void RenderGameModeConfig();
    void RenderPlayerConfig();
    void RenderCombatTesting();
    void RenderLevelDesignTools();

    enum class ActiveTab {
        SpawnPoints,
        GameMode,
        Player,
        Combat,
        LevelDesign
    };
    ActiveTab m_activeTab = ActiveTab::GameMode;

    // Spawn points
    struct SpawnPointData {
        std::string name;
        float x, y, z;
        float rotY;
        int teamId;     // 0=none, 1=alpha, 2=bravo
        bool active;
    };
    std::vector<SpawnPointData> m_spawnPoints;
    int m_selectedSpawn = -1;

    // Game mode config
    int m_selectedGameMode = 0;
    int m_scoreLimit = 50;
    float m_timeLimit = 600.0f;
    float m_respawnDelay = 3.0f;
    bool m_friendlyFire = false;
    bool m_teamsEnabled = false;
    float m_damageMultiplier = 1.0f;
    float m_healthMultiplier = 1.0f;
    float m_speedMultiplier = 1.0f;
    bool m_headshots = true;
    float m_headshotMultiplier = 2.0f;

    // Player config
    float m_playerHealth = 100.0f;
    float m_playerArmor = 100.0f;
    float m_playerSpeed = 5.0f;
    float m_playerJumpHeight = 3.0f;
    float m_playerGravity = 20.0f;
    float m_playerFriction = 0.9f;
    float m_mouseSensitivity = 2.0f;
    float m_fov = 90.0f;

    // Combat testing
    float m_testDamage = 30.0f;
    float m_testHealth = 100.0f;
    float m_testArmor = 50.0f;
    float m_testDistance = 10.0f;
    bool m_showDamageCalc = true;

    // Time
    float m_totalTime = 0.0f;
};

} // namespace SparkEditor
