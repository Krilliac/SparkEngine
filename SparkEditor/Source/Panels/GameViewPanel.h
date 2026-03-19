/**
 * @file GameViewPanel.h
 * @brief Game viewport panel with full FPS HUD preview
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>
#include <deque>

namespace SparkEditor
{

    /** @brief In-editor game viewport with full FPS HUD preview and simulated player state. */
    class GameViewPanel : public EditorPanel
    {
      public:
        GameViewPanel();
        ~GameViewPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /** @brief Check if the game view is capturing input (cursor locked) */
        bool IsCursorCaptured() const { return m_isCursorCaptured; }

      private:
        void RenderToolbar();
        void RenderGameContent();
        void RenderFPSHUD();
        void HandleInput(float deltaTime);
        ImVec2 GetConstrainedViewportSize(const ImVec2& available) const;
        void RenderCrosshair(float cx, float cy);
        void RenderHealthArmorBars(float baseX, float baseY, float width);
        void RenderAmmoDisplay(float baseX, float baseY);
        void RenderMinimap(float x, float y, float size);
        void RenderDamageIndicators(float cx, float cy, float radius);
        void RenderKillFeed(float x, float y);
        void RenderHitMarker(float cx, float cy);
        void RenderWeaponInfo(float x, float y);
        void RenderObjectiveBar(float x, float y, float width);
        void RenderScoreboard(float cx, float cy, float width, float height);
        void RenderLowHealthVignette(float x, float y, float w, float h);
        void RenderInteractionPrompt(float cx, float cy);

        // Resolution presets
        enum class Resolution
        {
            Free,
            R1920x1080,
            R1280x720,
            R2560x1440,
            R3840x2160
        };
        Resolution m_resolution = Resolution::Free;
        bool m_lockAspectRatio = true;
        float m_aspectRatio = 16.0f / 9.0f;

        // HUD visibility
        bool m_showHUD = true;
        bool m_showCrosshair = true;
        bool m_showStats = false;
        bool m_showMinimap = true;
        bool m_showKillFeed = true;
        bool m_showDamageIndicators = true;
        bool m_showScoreboard = false;
        bool m_maximized = false;

        // Simulated player state
        float m_health = 85.0f;                            ///< Current player health for HUD preview.
        float m_maxHealth = 100.0f;                        ///< Max health.
        float m_armor = 60.0f;                             ///< Current armor.
        float m_maxArmor = 100.0f;                         ///< Max armor.
        float m_stamina = 100.0f;                          ///< Current stamina.
        int m_ammo = 24;                                   ///< Rounds remaining in magazine.
        int m_maxAmmo = 30;                                ///< Magazine capacity.
        int m_reserveAmmo = 90;                            ///< Reserve ammo pool.
        int m_currentWeaponSlot = 2;                       ///< Active weapon slot index.
        std::string m_currentWeaponName = "Assault Rifle"; ///< Display name.
        bool m_isReloading = false;                        ///< Whether reload animation is playing.
        float m_reloadProgress = 0.0f;                     ///< Reload progress [0, 1].

        // Kill feed simulation
        struct SimKillFeedEntry
        {
            std::string killer; ///< Killer player name.
            std::string victim; ///< Victim player name.
            std::string weapon; ///< Weapon used for the kill.
            bool headshot;      ///< Whether it was a headshot.
            float timer;        ///< Time remaining before the entry fades out.
        };
        std::deque<SimKillFeedEntry> m_killFeed; ///< Active kill feed entries.
        float m_killFeedSpawnTimer = 0.0f;       ///< Timer for spawning simulated kills.

        // Damage indicators
        struct SimDamageIndicator
        {
            float angle;     ///< Direction the damage came from (radians).
            float intensity; ///< Visual intensity [0, 1].
            float timer;     ///< Time remaining before fade-out.
            float maxTime;   ///< Total display duration.
        };
        std::vector<SimDamageIndicator> m_damageIndicators; ///< Active damage direction indicators.
        float m_damageSpawnTimer = 0.0f;                    ///< Timer for spawning simulated damage.

        // Hit marker
        float m_hitMarkerTimer = 0.0f;
        bool m_hitMarkerHeadshot = false;

        // Low health vignette
        float m_vignetteIntensity = 0.0f;
        float m_vignettePulse = 0.0f;

        // Weapon switch display
        float m_weaponSwitchTimer = 0.0f;
        std::string m_weaponSwitchName;
        int m_weaponSwitchSlot = 0;

        // Objective
        float m_objectiveProgress = 0.65f;
        std::string m_objectiveText = "Capture Point A";

        // Interaction prompt
        bool m_showInteraction = false;
        float m_interactionPulse = 0.0f;

        // Scoreboard entries
        struct SimScoreEntry
        {
            std::string name;               ///< Player name.
            int kills, deaths, score, ping; ///< Stats columns.
            bool isLocal;                   ///< True for the local player (highlighted).
        };
        std::vector<SimScoreEntry> m_scoreboardEntries;

        // Minimap
        float m_playerAngle = 0.0f; // Simulated rotation

        // Timers
        float m_totalTime = 0.0f;

        // Input state
        bool m_isCursorCaptured = false;
        float m_cameraYaw = 0.0f;
        float m_cameraPitch = 0.0f;
        float m_cameraPosX = 128.5f;
        float m_cameraPosY = 1.0f;
        float m_cameraPosZ = 64.2f;
        float m_moveSpeed = 5.0f;
        float m_mouseSensitivity = 0.003f;
        float m_lastDeltaTime = 0.016f;
    };

} // namespace SparkEditor
