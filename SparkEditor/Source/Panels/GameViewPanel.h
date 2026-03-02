/**
 * @file GameViewPanel.h
 * @brief Game viewport panel showing player-perspective view
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <algorithm>

namespace SparkEditor {

class GameViewPanel : public EditorPanel {
public:
    GameViewPanel();
    ~GameViewPanel() override = default;

    bool Initialize() override;
    void Update(float deltaTime) override;
    void Render() override;
    void Shutdown() override;
    bool HandleEvent(const std::string& eventType, void* eventData) override;

private:
    void RenderToolbar();
    void RenderGameContent();
    void RenderFPSHUD();

    // Resolution presets
    enum class Resolution { Free, R1920x1080, R1280x720, R2560x1440, R3840x2160 };
    Resolution m_resolution = Resolution::Free;
    bool m_lockAspectRatio = true;
    float m_aspectRatio = 16.0f / 9.0f;

    // HUD
    bool m_showHUD = true;
    bool m_showCrosshair = true;
    bool m_showStats = false;
    bool m_maximized = false;

    // Simulation
    float m_health = 100.0f;
    int m_ammo = 30;
    int m_maxAmmo = 30;
};

} // namespace SparkEditor
