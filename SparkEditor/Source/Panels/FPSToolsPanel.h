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

namespace SparkEditor
{

    /** @brief Editor panel for FPS-specific level design and gameplay tuning tools. */
    class FPSToolsPanel : public EditorPanel
    {
      public:
        FPSToolsPanel();
        ~FPSToolsPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        void RenderSpawnPointEditor(); ///< Draw the spawn point placement and editing UI.
        void RenderGameModeConfig();   ///< Draw game mode parameter sliders.
        void RenderPlayerConfig();     ///< Draw player movement/health tuning UI.
        void RenderCombatTesting();    ///< Draw damage calculator and combat testing UI.
        void RenderLevelDesignTools(); ///< Draw level design helper tools.

        enum class ActiveTab
        {
            SpawnPoints,
            GameMode,
            Player,
            Combat,
            LevelDesign
        };
        ActiveTab m_activeTab = ActiveTab::GameMode; ///< Currently selected tab.

        // Spawn points
        struct SpawnPointData
        {
            std::string name; ///< Spawn point label.
            float x, y, z;    ///< World-space position.
            float rotY;       ///< Yaw rotation (degrees).
            int teamId;       ///< Team: 0=none, 1=alpha, 2=bravo.
            bool active;      ///< Whether this spawn point is enabled.
        };
        std::vector<SpawnPointData> m_spawnPoints; ///< All configured spawn points.
        int m_selectedSpawn = -1;                  ///< Index of the selected spawn point (-1 = none).

        // Game mode config
        int m_selectedGameMode = 0;        ///< Active game mode index (0=TDM, 1=FFA, etc.).
        int m_scoreLimit = 50;             ///< Score required to win the match.
        float m_timeLimit = 600.0f;        ///< Match time limit (seconds).
        float m_respawnDelay = 3.0f;       ///< Delay before respawning (seconds).
        bool m_friendlyFire = false;       ///< Whether friendly fire is enabled.
        bool m_teamsEnabled = false;       ///< Whether teams are enabled.
        float m_damageMultiplier = 1.0f;   ///< Global damage multiplier.
        float m_healthMultiplier = 1.0f;   ///< Global health multiplier.
        float m_speedMultiplier = 1.0f;    ///< Global speed multiplier.
        bool m_headshots = true;           ///< Whether headshot detection is enabled.
        float m_headshotMultiplier = 2.0f; ///< Damage multiplier for headshots.

        // Player config
        float m_playerHealth = 100.0f;   ///< Default player health.
        float m_playerArmor = 100.0f;    ///< Default player armor.
        float m_playerSpeed = 5.0f;      ///< Walk speed (m/s).
        float m_playerJumpHeight = 3.0f; ///< Jump height (meters).
        float m_playerGravity = 20.0f;   ///< Gravity acceleration (m/s^2).
        float m_playerFriction = 0.9f;   ///< Ground friction coefficient.
        float m_mouseSensitivity = 2.0f; ///< Mouse look sensitivity multiplier.
        float m_fov = 90.0f;             ///< Field of view (degrees).

        // Combat testing
        float m_testDamage = 30.0f;   ///< Test damage value for the calculator.
        float m_testHealth = 100.0f;  ///< Test target health.
        float m_testArmor = 50.0f;    ///< Test target armor.
        float m_testDistance = 10.0f; ///< Test engagement distance (meters).
        bool m_showDamageCalc = true; ///< Whether the damage calculator is visible.

        // Time
        float m_totalTime = 0.0f; ///< Accumulated panel time for animations.
    };

} // namespace SparkEditor
