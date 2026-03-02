/**
 * @file GameViewPanel.cpp
 * @brief Game viewport panel — shows player camera perspective
 * @author Spark Engine Team
 * @date 2025
 */

#include "GameViewPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor {

GameViewPanel::GameViewPanel()
    : EditorPanel("Game View", "game_view_panel") {
}

bool GameViewPanel::Initialize() {
    std::cout << "Initializing Game View panel\n";
    return true;
}

void GameViewPanel::Update(float deltaTime) {
    // Simulate health regeneration
    if (m_health < 100.0f) {
        m_health = std::min(100.0f, m_health + 2.0f * deltaTime);
    }
}

void GameViewPanel::Render() {
    if (!IsVisible()) return;

    if (BeginPanel()) {
        RenderToolbar();
        RenderGameContent();
    }
    EndPanel();
}

void GameViewPanel::Shutdown() {
    std::cout << "Shutting down Game View panel\n";
}

bool GameViewPanel::HandleEvent(const std::string& eventType, void* eventData) {
    return false;
}

void GameViewPanel::RenderToolbar() {
    // Resolution selector
    const char* resLabels[] = { "Free", "1920x1080", "1280x720", "2560x1440", "3840x2160" };
    ImGui::SetNextItemWidth(100);
    int resIdx = (int)m_resolution;
    if (ImGui::Combo("##Resolution", &resIdx, resLabels, IM_ARRAYSIZE(resLabels))) {
        m_resolution = (Resolution)resIdx;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output Resolution");

    ImGui::SameLine();
    ImGui::Checkbox("Lock AR", &m_lockAspectRatio);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lock Aspect Ratio (16:9)");

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    ImGui::Checkbox(ICON_FA_CROSSHAIRS " HUD", &m_showHUD);
    ImGui::SameLine();
    ImGui::Checkbox(ICON_FA_CHART_BAR " Stats", &m_showStats);

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    if (ImGui::Button(m_maximized ? ICON_FA_COMPRESS : ICON_FA_EXPAND)) {
        m_maximized = !m_maximized;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(m_maximized ? "Restore" : "Maximize");

    ImGui::Separator();
}

void GameViewPanel::RenderGameContent() {
    ImVec2 viewportSize = ImGui::GetContentRegionAvail();
    if (viewportSize.x <= 0 || viewportSize.y <= 0) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    // Dark game viewport background
    drawList->AddRectFilled(pos, ImVec2(pos.x + viewportSize.x, pos.y + viewportSize.y),
                           IM_COL32(20, 22, 28, 255));

    // Simulated sky gradient
    for (int y = 0; y < (int)(viewportSize.y * 0.6f); y++) {
        float t = (float)y / (viewportSize.y * 0.6f);
        int r = (int)(20 + t * 30);
        int g = (int)(30 + t * 40);
        int b = (int)(60 + t * 50);
        drawList->AddLine(ImVec2(pos.x, pos.y + y),
                         ImVec2(pos.x + viewportSize.x, pos.y + y),
                         IM_COL32(r, g, b, 255));
    }

    // Ground line
    float groundY = pos.y + viewportSize.y * 0.6f;
    drawList->AddRectFilled(ImVec2(pos.x, groundY),
                           ImVec2(pos.x + viewportSize.x, pos.y + viewportSize.y),
                           IM_COL32(35, 40, 30, 255));

    // Grid on ground
    for (int i = 0; i < 30; i++) {
        float x = pos.x + (i * viewportSize.x / 30.0f);
        drawList->AddLine(ImVec2(x, groundY), ImVec2(x, pos.y + viewportSize.y),
                         IM_COL32(50, 55, 40, 200));
    }
    for (int i = 0; i < 10; i++) {
        float y = groundY + (i * (viewportSize.y * 0.4f) / 10.0f);
        drawList->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + viewportSize.x, y),
                         IM_COL32(50, 55, 40, 200));
    }

    // "Game View" center text
    const char* label = "Game View - Player Camera";
    ImVec2 textSize = ImGui::CalcTextSize(label);
    ImVec2 textPos(pos.x + (viewportSize.x - textSize.x) * 0.5f,
                   pos.y + (viewportSize.y - textSize.y) * 0.5f);
    drawList->AddText(textPos, IM_COL32(100, 120, 140, 180), label);

    // FPS HUD overlay
    if (m_showHUD) {
        RenderFPSHUD();
    }

    // Advance cursor past the viewport area
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + viewportSize.y));
    ImGui::Dummy(ImVec2(viewportSize.x, 0));
}

void GameViewPanel::RenderFPSHUD() {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    float padding = 12.0f;
    float toolbarH = 38.0f; // approximate toolbar height

    // === Crosshair ===
    if (m_showCrosshair) {
        float cx = winPos.x + winSize.x * 0.5f;
        float cy = winPos.y + toolbarH + (winSize.y - toolbarH) * 0.5f;
        ImU32 crossColor = IM_COL32(255, 255, 255, 200);
        float size = 12.0f;
        float gap = 3.0f;
        float thickness = 2.0f;
        drawList->AddLine(ImVec2(cx - size, cy), ImVec2(cx - gap, cy), crossColor, thickness);
        drawList->AddLine(ImVec2(cx + gap, cy), ImVec2(cx + size, cy), crossColor, thickness);
        drawList->AddLine(ImVec2(cx, cy - size), ImVec2(cx, cy - gap), crossColor, thickness);
        drawList->AddLine(ImVec2(cx, cy + gap), ImVec2(cx, cy + size), crossColor, thickness);
        drawList->AddCircle(ImVec2(cx, cy), 2.0f, crossColor, 12, 1.5f);
    }

    // === Health bar (bottom-left) ===
    float barW = 200.0f, barH = 16.0f;
    float bx = winPos.x + padding;
    float by = winPos.y + winSize.y - padding - barH;
    float healthPct = m_health / 100.0f;

    // Background
    drawList->AddRectFilled(ImVec2(bx, by), ImVec2(bx + barW, by + barH),
                           IM_COL32(0, 0, 0, 150), 3.0f);
    // Health fill
    ImU32 healthColor = healthPct > 0.5f ? IM_COL32(76, 175, 80, 220) :
                        healthPct > 0.25f ? IM_COL32(245, 166, 35, 220) :
                        IM_COL32(229, 57, 53, 220);
    drawList->AddRectFilled(ImVec2(bx + 2, by + 2),
                           ImVec2(bx + 2 + (barW - 4) * healthPct, by + barH - 2),
                           healthColor, 2.0f);
    // Health text
    char healthText[32];
    snprintf(healthText, sizeof(healthText), ICON_FA_HEART " %.0f", m_health);
    drawList->AddText(ImVec2(bx + 4, by), IM_COL32(255, 255, 255, 220), healthText);

    // === Ammo (bottom-right) ===
    char ammoText[32];
    snprintf(ammoText, sizeof(ammoText), "%d / %d", m_ammo, m_maxAmmo);
    ImVec2 ammoSize = ImGui::CalcTextSize(ammoText);
    float ax = winPos.x + winSize.x - padding - ammoSize.x - 30;
    float ay = by;
    drawList->AddRectFilled(ImVec2(ax - 4, ay), ImVec2(ax + ammoSize.x + 30, ay + barH),
                           IM_COL32(0, 0, 0, 150), 3.0f);
    drawList->AddText(ImVec2(ax, ay), IM_COL32(255, 255, 255, 220), ICON_FA_CROSSHAIRS);
    drawList->AddText(ImVec2(ax + 20, ay), IM_COL32(255, 255, 255, 220), ammoText);
}

} // namespace SparkEditor
