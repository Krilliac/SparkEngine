/**
 * @file WeaponEditorPanel.cpp
 * @brief Weapon configuration and balancing editor
 * @author Spark Engine Team
 * @date 2025
 */

#include "WeaponEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace SparkEditor
{
    namespace
    {
        /// Lethality tier of a burst DPS value, shared by the DPS chart and the comparison table.
        Spark::Ui::Status DpsTier(float dps)
        {
            if (dps > 500.0f)
            {
                return Spark::Ui::Status::Error;
            }
            if (dps > 200.0f)
            {
                return Spark::Ui::Status::Warning;
            }
            if (dps > 100.0f)
            {
                return Spark::Ui::Status::Success;
            }
            return Spark::Ui::Status::Info;
        }

        /// Lethality tier of a time-to-kill value: very fast kills are flagged as errors.
        Spark::Ui::Status TtkTier(float ttk)
        {
            if (ttk < 0.5f)
            {
                return Spark::Ui::Status::Error;
            }
            if (ttk < 1.5f)
            {
                return Spark::Ui::Status::Warning;
            }
            return Spark::Ui::Status::Success;
        }
    } // namespace

    WeaponEditorPanel::WeaponEditorPanel() : EditorPanel("Weapon Editor", "weapon_editor_panel") {}

    bool WeaponEditorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Weapon Editor panel");

        // Populate with default weapon data
        m_weapons = {
            {"Pistol", 0, 25.0f, 450.0f, 15, 1.5f, 350.0f, 0.85f, 30.0f, false},
            {"Rifle", 1, 35.0f, 600.0f, 30, 2.2f, 800.0f, 0.75f, 60.0f, false},
            {"Shotgun", 2, 80.0f, 120.0f, 8, 2.8f, 300.0f, 0.45f, 12.0f, false},
            {"Rocket Launcher", 3, 200.0f, 60.0f, 4, 3.5f, 250.0f, 0.95f, 80.0f, false},
            {"Grenade Launcher", 4, 150.0f, 90.0f, 6, 3.0f, 200.0f, 0.70f, 40.0f, false},
            {"Sniper Rifle", 5, 120.0f, 60.0f, 5, 3.0f, 1200.0f, 0.98f, 120.0f, false},
            {"SMG", 6, 18.0f, 900.0f, 40, 2.0f, 400.0f, 0.60f, 25.0f, false},
            {"Assault Rifle", 7, 30.0f, 700.0f, 30, 2.5f, 700.0f, 0.70f, 50.0f, false},
            {"Machine Gun", 8, 40.0f, 800.0f, 100, 5.0f, 600.0f, 0.65f, 45.0f, false},
            {"Flamethrower", 9, 15.0f, 1200.0f, 200, 4.0f, 50.0f, 0.80f, 8.0f, false},
            {"Plasma Rifle", 10, 45.0f, 300.0f, 20, 2.5f, 500.0f, 0.88f, 55.0f, false},
            {"Laser Cannon", 11, 60.0f, 180.0f, 12, 3.0f, 999.0f, 0.95f, 100.0f, false},
            {"Railgun", 12, 150.0f, 40.0f, 3, 4.0f, 2000.0f, 0.99f, 150.0f, false},
            {"Minigun", 13, 12.0f, 3000.0f, 200, 6.0f, 500.0f, 0.55f, 35.0f, false},
            {"Crossbow", 14, 90.0f, 80.0f, 1, 2.0f, 200.0f, 0.92f, 45.0f, false},
        };
        m_baseline = m_weapons;

        return true;
    }

    void WeaponEditorPanel::Update(float /*deltaTime*/) {}

    void WeaponEditorPanel::Render()
    {
        if (!IsVisible())
        {
            return;
        }

        if (BeginPanel())
        {
            // Toolbar
            if (ImGui::Button(ICON_FA_UNDO " Reset"))
            {
                ResetSelectedWeapon();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Restore the selected weapon's starting values");
            }
            ImGui::SameLine();
            ImGui::Checkbox("Compare", &m_showComparison);
            ImGui::SameLine();
            ImGui::Checkbox("DPS Chart", &m_showDPSChart);
            Spark::Ui::SecondaryText("Balance calculator: these rows are not read or written by any game");
            Spark::Ui::SecondaryText("module, and edits are not persisted.");
            ImGui::Separator();

            // Layout: left list, right properties
            const float listWidth = Spark::Ui::Px(180.0f);
            ImGui::BeginChild("WeaponList", ImVec2(listWidth, 0), true);
            RenderWeaponList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("WeaponDetails", ImVec2(0, 0), true);
            if (m_selectedWeapon >= 0 && m_selectedWeapon < (int)m_weapons.size())
            {
                RenderWeaponProperties();

                ImGui::Separator();

                if (m_showDPSChart)
                {
                    RenderDPSChart();
                }

                if (m_showComparison)
                {
                    ImGui::Separator();
                    RenderComparisonTable();
                }
            }
            ImGui::EndChild();
        }
        EndPanel();
    }

    void WeaponEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Weapon Editor panel");
    }

    void WeaponEditorPanel::RenderWeaponList()
    {
        ImGui::Text(ICON_FA_CROSSHAIRS " Weapons");
        ImGui::Separator();

        for (int i = 0; i < (int)m_weapons.size(); i++)
        {
            const auto& w = m_weapons[i];
            char label[64];
            snprintf(label, sizeof(label), "%s%s##w%d", w.isModified ? "* " : "", w.name.c_str(), i);

            bool selected = (m_selectedWeapon == i);
            if (ImGui::Selectable(label, selected))
            {
                m_selectedWeapon = i;
            }
        }
    }

    void WeaponEditorPanel::RenderWeaponProperties()
    {
        auto& w = m_weapons[m_selectedWeapon];

        ImGui::Text(ICON_FA_COG " %s Properties", w.name.c_str());
        if (w.isModified)
        {
            ImGui::SameLine();
            Spark::Ui::Badge("Modified", Spark::Ui::Status::Warning);
        }
        ImGui::Separator();

        if (!Spark::Ui::BeginPropertyGrid("WeaponProps"))
        {
            return;
        }

        // Damage
        Spark::Ui::PropertyLabel("Damage");
        if (ImGui::SliderFloat("##Damage", &w.damage, 1.0f, 500.0f, "%.1f"))
        {
            w.isModified = true;
        }

        // Fire Rate
        Spark::Ui::PropertyLabel("Fire Rate (RPM)");
        if (ImGui::SliderFloat("##FireRate", &w.fireRate, 10.0f, 5000.0f, "%.0f"))
        {
            w.isModified = true;
        }

        // Magazine Size
        Spark::Ui::PropertyLabel("Magazine Size");
        if (ImGui::SliderInt("##MagSize", &w.magazineSize, 1, 500))
        {
            w.isModified = true;
        }

        // Reload Time
        Spark::Ui::PropertyLabel("Reload Time (s)");
        if (ImGui::SliderFloat("##ReloadTime", &w.reloadTime, 0.5f, 10.0f, "%.2f"))
        {
            w.isModified = true;
        }

        // Muzzle Velocity
        Spark::Ui::PropertyLabel("Muzzle Velocity");
        if (ImGui::SliderFloat("##MuzzleVel", &w.muzzleVelocity, 10.0f, 3000.0f, "%.0f"))
        {
            w.isModified = true;
        }

        // Accuracy
        Spark::Ui::PropertyLabel("Accuracy");
        if (ImGui::SliderFloat("##Accuracy", &w.accuracy, 0.0f, 1.0f, "%.2f"))
        {
            w.isModified = true;
        }

        // Range
        Spark::Ui::PropertyLabel("Eff. Range");
        if (ImGui::SliderFloat("##Range", &w.range, 1.0f, 200.0f, "%.1f"))
        {
            w.isModified = true;
        }

        Spark::Ui::EndPropertyGrid();

        // Computed stats
        ImGui::Spacing();
        Spark::Ui::SubHeader(ICON_FA_CHART_BAR " Computed Stats");

        float dps = w.damage * (w.fireRate / 60.0f);
        float shotInterval = (w.fireRate > 0) ? (60.0f / w.fireRate) : 1.0f;
        float magDuration = shotInterval * w.magazineSize;
        float magDamage = w.damage * w.magazineSize;
        float sustainedDPS = magDamage / (magDuration + w.reloadTime);

        if (!Spark::Ui::BeginPropertyGrid("ComputedStats"))
        {
            return;
        }

        Spark::Ui::PropertyLabel("Burst DPS");
        Spark::Ui::MonoText("%.1f", dps);

        Spark::Ui::PropertyLabel("Sustained DPS");
        Spark::Ui::MonoText("%.1f", sustainedDPS);

        Spark::Ui::PropertyLabel("Shot Interval");
        Spark::Ui::MonoText("%.3f s", shotInterval);

        Spark::Ui::PropertyLabel("Mag Duration");
        Spark::Ui::MonoText("%.1f s", magDuration);

        Spark::Ui::PropertyLabel("Mag Damage");
        Spark::Ui::MonoText("%.0f", magDamage);

        Spark::Ui::PropertyLabel("TTK (100hp)");
        int shotsToKill = (int)ceilf(100.0f / w.damage);
        float ttk = shotInterval * (shotsToKill - 1);
        Spark::Ui::StatusText(TtkTier(ttk), "%.2f s (%d shots)", ttk, shotsToKill);

        Spark::Ui::EndPropertyGrid();
    }

    void WeaponEditorPanel::RenderDPSChart()
    {
        Spark::Ui::SubHeader(ICON_FA_CHART_BAR " DPS Comparison");

        float maxDPS = 0;
        for (const auto& w : m_weapons)
        {
            float dps = w.damage * (w.fireRate / 60.0f);
            if (dps > maxDPS)
            {
                maxDPS = dps;
            }
        }

        ImVec2 avail = ImGui::GetContentRegionAvail();
        float chartH = std::min(Spark::Ui::Px(200.0f), avail.y * 0.4f);

        ImGui::BeginChild("DPSChart", ImVec2(0, chartH), false);

        for (int i = 0; i < (int)m_weapons.size(); i++)
        {
            const auto& w = m_weapons[i];
            float dps = w.damage * (w.fireRate / 60.0f);
            float pct = maxDPS > 0 ? dps / maxDPS : 0;

            // Color based on DPS tier; unselected weapons are dimmed.
            const ImVec4 barColor =
                Spark::Ui::ColV(Spark::Ui::StatusTextRole(DpsTier(dps)), i == m_selectedWeapon ? 1.0f : 0.6f);

            char label[64];
            snprintf(label, sizeof(label), "%.0f", dps);

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
            ImGui::ProgressBar(pct, ImVec2(-1.0f, Spark::Ui::Px(16.0f)), label);
            ImGui::PopStyleColor();

            // Show weapon name on same line
            ImVec2 textPos = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(textPos.x + Spark::Ui::Px(4.0f), textPos.y + Spark::Ui::Px(1.0f)),
                Spark::Ui::Col(Spark::Ui::Role::TextPrimary, .78f), w.name.c_str());
        }

        ImGui::EndChild();
    }

    void WeaponEditorPanel::RenderComparisonTable()
    {
        Spark::Ui::SubHeader(ICON_FA_TH " Full Comparison");

        if (ImGui::BeginTable("WeaponCompare", 8,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, Spark::Ui::Px(200.0f))))
        {

            ImGui::TableSetupColumn("Weapon", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(120));
            ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(60));
            ImGui::TableSetupColumn("RPM", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(50));
            ImGui::TableSetupColumn("Mag", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(40));
            ImGui::TableSetupColumn("Reload", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(50));
            ImGui::TableSetupColumn("Accuracy", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(60));
            ImGui::TableSetupColumn("DPS", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(60));
            ImGui::TableSetupColumn("TTK", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(50));
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)m_weapons.size(); i++)
            {
                const auto& w = m_weapons[i];
                float dps = w.damage * (w.fireRate / 60.0f);
                float shotInterval = (w.fireRate > 0) ? (60.0f / w.fireRate) : 1.0f;
                int shotsToKill = (int)ceilf(100.0f / w.damage);
                float ttk = shotInterval * (shotsToKill - 1);

                ImGui::TableNextRow();
                if (i == m_selectedWeapon)
                {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, Spark::Ui::Col(Spark::Ui::Role::SurfaceSelected));
                }

                ImGui::TableNextColumn();
                ImGui::Text("%s", w.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", w.damage);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", w.fireRate);
                ImGui::TableNextColumn();
                ImGui::Text("%d", w.magazineSize);
                ImGui::TableNextColumn();
                ImGui::Text("%.1fs", w.reloadTime);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f%%", w.accuracy * 100);
                ImGui::TableNextColumn();
                Spark::Ui::StatusText(DpsTier(dps), "%.0f", dps);
                ImGui::TableNextColumn();
                Spark::Ui::StatusText(TtkTier(ttk), "%.2fs", ttk);
            }

            ImGui::EndTable();
        }
    }

    bool WeaponEditorPanel::ResetSelectedWeapon()
    {
        if (m_selectedWeapon < 0 || m_selectedWeapon >= static_cast<int>(m_weapons.size()) ||
            m_weapons.size() != m_baseline.size())
        {
            return false;
        }

        const size_t index = static_cast<size_t>(m_selectedWeapon);
        m_weapons[index] = m_baseline[index];
        m_weapons[index].isModified = false;
        return true;
    }

} // namespace SparkEditor
