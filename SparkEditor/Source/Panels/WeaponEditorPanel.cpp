/**
 * @file WeaponEditorPanel.cpp
 * @brief Weapon configuration and balancing editor
 * @author Spark Engine Team
 * @date 2025
 */

#include "WeaponEditorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace SparkEditor {

WeaponEditorPanel::WeaponEditorPanel()
    : EditorPanel("Weapon Editor", "weapon_editor_panel") {
}

bool WeaponEditorPanel::Initialize() {
    std::cout << "Initializing Weapon Editor panel\n";

    // Populate with default weapon data
    m_weapons = {
        {"Pistol",           0, 25.0f, 450.0f, 15, 1.5f, 350.0f, 0.85f, 30.0f, false},
        {"Rifle",            1, 35.0f, 600.0f, 30, 2.2f, 800.0f, 0.75f, 60.0f, false},
        {"Shotgun",          2, 80.0f, 120.0f, 8,  2.8f, 300.0f, 0.45f, 12.0f, false},
        {"Rocket Launcher",  3, 200.0f, 60.0f, 4,  3.5f, 250.0f, 0.95f, 80.0f, false},
        {"Grenade Launcher", 4, 150.0f, 90.0f, 6,  3.0f, 200.0f, 0.70f, 40.0f, false},
        {"Sniper Rifle",     5, 120.0f, 60.0f, 5,  3.0f, 1200.0f, 0.98f, 120.0f, false},
        {"SMG",              6, 18.0f,  900.0f, 40, 2.0f, 400.0f, 0.60f, 25.0f, false},
        {"Assault Rifle",    7, 30.0f,  700.0f, 30, 2.5f, 700.0f, 0.70f, 50.0f, false},
        {"Machine Gun",      8, 40.0f,  800.0f, 100, 5.0f, 600.0f, 0.65f, 45.0f, false},
        {"Flamethrower",     9, 15.0f,  1200.0f, 200, 4.0f, 50.0f, 0.80f, 8.0f, false},
        {"Plasma Rifle",    10, 45.0f,  300.0f, 20, 2.5f, 500.0f, 0.88f, 55.0f, false},
        {"Laser Cannon",    11, 60.0f,  180.0f, 12, 3.0f, 999.0f, 0.95f, 100.0f, false},
        {"Railgun",         12, 150.0f, 40.0f, 3,  4.0f, 2000.0f, 0.99f, 150.0f, false},
        {"Minigun",         13, 12.0f,  3000.0f, 200, 6.0f, 500.0f, 0.55f, 35.0f, false},
        {"Crossbow",        14, 90.0f,  80.0f, 1,  2.0f, 200.0f, 0.92f, 45.0f, false},
    };

    return true;
}

void WeaponEditorPanel::Update(float deltaTime) {
    m_previewTime += deltaTime;
}

void WeaponEditorPanel::Render() {
    if (!IsVisible()) return;

    if (BeginPanel()) {
        // Toolbar
        if (ImGui::Button(ICON_FA_SAVE " Save All")) {
            // Save weapon configs
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save all weapon configurations");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_UNDO " Reset")) {
            if (m_selectedWeapon >= 0 && m_selectedWeapon < (int)m_weapons.size()) {
                m_weapons[m_selectedWeapon].isModified = false;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset selected weapon to defaults");
        ImGui::SameLine();
        ImGui::Checkbox("Compare", &m_showComparison);
        ImGui::SameLine();
        ImGui::Checkbox("DPS Chart", &m_showDPSChart);
        ImGui::Separator();

        // Layout: left list, right properties
        float listWidth = 180.0f;
        ImGui::BeginChild("WeaponList", ImVec2(listWidth, 0), true);
        RenderWeaponList();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("WeaponDetails", ImVec2(0, 0), true);
        if (m_selectedWeapon >= 0 && m_selectedWeapon < (int)m_weapons.size()) {
            RenderWeaponProperties();

            ImGui::Separator();

            if (m_showDPSChart) {
                RenderDPSChart();
            }

            if (m_showComparison) {
                ImGui::Separator();
                RenderComparisonTable();
            }
        }
        ImGui::EndChild();
    }
    EndPanel();
}

void WeaponEditorPanel::Shutdown() {
    std::cout << "Shutting down Weapon Editor panel\n";
}

void WeaponEditorPanel::RenderWeaponList() {
    ImGui::Text(ICON_FA_CROSSHAIRS " Weapons");
    ImGui::Separator();

    for (int i = 0; i < (int)m_weapons.size(); i++) {
        const auto& w = m_weapons[i];
        char label[64];
        snprintf(label, sizeof(label), "%s%s##w%d",
                 w.isModified ? "* " : "", w.name.c_str(), i);

        bool selected = (m_selectedWeapon == i);
        if (ImGui::Selectable(label, selected)) {
            m_selectedWeapon = i;
        }
    }
}

void WeaponEditorPanel::RenderWeaponProperties() {
    auto& w = m_weapons[m_selectedWeapon];

    ImGui::Text(ICON_FA_COG " %s Properties", w.name.c_str());
    if (w.isModified) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.96f, 0.65f, 0.14f, 1.0f), "(Modified)");
    }
    ImGui::Separator();

    ImGui::Columns(2, "WeaponProps", false);
    ImGui::SetColumnWidth(0, 130);

    // Damage
    ImGui::Text("Damage");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##Damage", &w.damage, 1.0f, 500.0f, "%.1f")) w.isModified = true;
    ImGui::NextColumn();

    // Fire Rate
    ImGui::Text("Fire Rate (RPM)");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##FireRate", &w.fireRate, 10.0f, 5000.0f, "%.0f")) w.isModified = true;
    ImGui::NextColumn();

    // Magazine Size
    ImGui::Text("Magazine Size");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##MagSize", &w.magazineSize, 1, 500)) w.isModified = true;
    ImGui::NextColumn();

    // Reload Time
    ImGui::Text("Reload Time (s)");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##ReloadTime", &w.reloadTime, 0.5f, 10.0f, "%.2f")) w.isModified = true;
    ImGui::NextColumn();

    // Muzzle Velocity
    ImGui::Text("Muzzle Velocity");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##MuzzleVel", &w.muzzleVelocity, 10.0f, 3000.0f, "%.0f")) w.isModified = true;
    ImGui::NextColumn();

    // Accuracy
    ImGui::Text("Accuracy");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##Accuracy", &w.accuracy, 0.0f, 1.0f, "%.2f")) w.isModified = true;
    ImGui::NextColumn();

    // Range
    ImGui::Text("Eff. Range");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##Range", &w.range, 1.0f, 200.0f, "%.1f")) w.isModified = true;
    ImGui::NextColumn();

    ImGui::Columns(1);

    // Computed stats
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text(ICON_FA_CHART_BAR " Computed Stats");

    float dps = w.damage * (w.fireRate / 60.0f);
    float shotInterval = (w.fireRate > 0) ? (60.0f / w.fireRate) : 1.0f;
    float magDuration = shotInterval * w.magazineSize;
    float magDamage = w.damage * w.magazineSize;
    float sustainedDPS = magDamage / (magDuration + w.reloadTime);

    ImGui::Columns(2, "ComputedStats", false);
    ImGui::SetColumnWidth(0, 130);

    ImGui::Text("Burst DPS"); ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "%.1f", dps);
    ImGui::NextColumn();

    ImGui::Text("Sustained DPS"); ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.0f), "%.1f", sustainedDPS);
    ImGui::NextColumn();

    ImGui::Text("Shot Interval"); ImGui::NextColumn();
    ImGui::Text("%.3f s", shotInterval);
    ImGui::NextColumn();

    ImGui::Text("Mag Duration"); ImGui::NextColumn();
    ImGui::Text("%.1f s", magDuration);
    ImGui::NextColumn();

    ImGui::Text("Mag Damage"); ImGui::NextColumn();
    ImGui::Text("%.0f", magDamage);
    ImGui::NextColumn();

    ImGui::Text("TTK (100hp)"); ImGui::NextColumn();
    int shotsToKill = (int)ceilf(100.0f / w.damage);
    float ttk = shotInterval * (shotsToKill - 1);
    ImGui::TextColored(ttk < 0.5f ? ImVec4(0.9f, 0.3f, 0.3f, 1.0f) :
                        ttk < 1.5f ? ImVec4(0.9f, 0.9f, 0.3f, 1.0f) :
                        ImVec4(0.3f, 0.8f, 0.3f, 1.0f),
                        "%.2f s (%d shots)", ttk, shotsToKill);
    ImGui::NextColumn();

    ImGui::Columns(1);
}

void WeaponEditorPanel::RenderDPSChart() {
    ImGui::Text(ICON_FA_CHART_BAR " DPS Comparison");

    float maxDPS = 0;
    for (const auto& w : m_weapons) {
        float dps = w.damage * (w.fireRate / 60.0f);
        if (dps > maxDPS) maxDPS = dps;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float chartH = std::min(200.0f, avail.y * 0.4f);

    ImGui::BeginChild("DPSChart", ImVec2(0, chartH), false);

    for (int i = 0; i < (int)m_weapons.size(); i++) {
        const auto& w = m_weapons[i];
        float dps = w.damage * (w.fireRate / 60.0f);
        float pct = maxDPS > 0 ? dps / maxDPS : 0;

        // Color based on DPS tier
        ImVec4 barColor = dps > 500 ? ImVec4(0.9f, 0.2f, 0.2f, 1.0f) :
                          dps > 200 ? ImVec4(0.9f, 0.6f, 0.1f, 1.0f) :
                          dps > 100 ? ImVec4(0.2f, 0.7f, 0.3f, 1.0f) :
                          ImVec4(0.2f, 0.5f, 0.8f, 1.0f);

        if (i == m_selectedWeapon) {
            barColor.w = 1.0f;
        } else {
            barColor.w = 0.6f;
        }

        char label[64];
        snprintf(label, sizeof(label), "%.0f", dps);

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
        char barLabel[64];
        snprintf(barLabel, sizeof(barLabel), "%s##dps%d", w.name.c_str(), i);
        ImGui::ProgressBar(pct, ImVec2(-1, 16), label);
        ImGui::PopStyleColor();

        // Show weapon name on same line
        ImVec2 textPos = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(textPos.x + 4, textPos.y + 1),
            IM_COL32(255, 255, 255, 200), w.name.c_str());
    }

    ImGui::EndChild();
}

void WeaponEditorPanel::RenderComparisonTable() {
    ImGui::Text(ICON_FA_TH " Full Comparison");

    if (ImGui::BeginTable("WeaponCompare", 8,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
        ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {

        ImGui::TableSetupColumn("Weapon", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("RPM", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Mag", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Reload", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Accuracy", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("DPS", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("TTK", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)m_weapons.size(); i++) {
            const auto& w = m_weapons[i];
            float dps = w.damage * (w.fireRate / 60.0f);
            float shotInterval = (w.fireRate > 0) ? (60.0f / w.fireRate) : 1.0f;
            int shotsToKill = (int)ceilf(100.0f / w.damage);
            float ttk = shotInterval * (shotsToKill - 1);

            ImGui::TableNextRow();
            if (i == m_selectedWeapon) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(45, 140, 240, 40));
            }

            ImGui::TableNextColumn(); ImGui::Text("%s", w.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f", w.damage);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", w.fireRate);
            ImGui::TableNextColumn(); ImGui::Text("%d", w.magazineSize);
            ImGui::TableNextColumn(); ImGui::Text("%.1fs", w.reloadTime);
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", w.accuracy * 100);
            ImGui::TableNextColumn();
            ImGui::TextColored(dps > 500 ? ImVec4(0.9f, 0.2f, 0.2f, 1) :
                               dps > 200 ? ImVec4(0.9f, 0.6f, 0.1f, 1) :
                               ImVec4(0.3f, 0.8f, 0.3f, 1), "%.0f", dps);
            ImGui::TableNextColumn();
            ImGui::TextColored(ttk < 0.5f ? ImVec4(0.9f, 0.3f, 0.3f, 1) :
                               ImVec4(0.8f, 0.8f, 0.8f, 1), "%.2fs", ttk);
        }

        ImGui::EndTable();
    }
}

void WeaponEditorPanel::RenderWeaponPreview() {
    // Placeholder for 3D weapon preview
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float previewH = std::min(150.0f, avail.y);

    ImGui::BeginChild("WeaponPreview", ImVec2(0, previewH), true);
    ImGui::Text(ICON_FA_CUBE " 3D Preview");

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(avail.x - 20, previewH - 30);

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                     IM_COL32(25, 28, 35, 255), 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
               IM_COL32(60, 70, 80, 200), 4.0f);

    // Rotating weapon silhouette (placeholder)
    float cx = pos.x + size.x * 0.5f;
    float cy = pos.y + size.y * 0.5f;
    float scale = sinf(m_previewTime * 0.5f) * 0.1f + 1.0f;

    dl->AddRectFilled(ImVec2(cx - 40 * scale, cy - 8 * scale),
                     ImVec2(cx + 40 * scale, cy + 8 * scale),
                     IM_COL32(80, 90, 100, 200), 2.0f);
    dl->AddRectFilled(ImVec2(cx - 5 * scale, cy + 2 * scale),
                     ImVec2(cx + 5 * scale, cy + 20 * scale),
                     IM_COL32(60, 65, 70, 200), 2.0f);

    ImGui::EndChild();
}

} // namespace SparkEditor
