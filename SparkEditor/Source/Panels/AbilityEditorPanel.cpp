/**
 * @file AbilityEditorPanel.cpp
 * @brief Implementation of the ability/aura/proc editor panel
 */

#include "AbilityEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <iostream>
#include <cstring>

namespace SparkEditor
{

    AbilityEditorPanel::AbilityEditorPanel() : EditorPanel("Ability Editor", "ability_editor_panel") {}

    bool AbilityEditorPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "AbilityEditorPanel initialized");
        return true;
    }

    void AbilityEditorPanel::Update(float /*deltaTime*/) {}

    void AbilityEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            // Stats bar
            ImGui::Text("Abilities: %d | Auras: %d | Procs: %d", static_cast<int>(m_abilities.size()),
                        static_cast<int>(m_auras.size()), static_cast<int>(m_procs.size()));

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
            ImGui::SetNextItemWidth(200);
            ImGui::InputText(ICON_FA_FILTER " Filter", m_filterText, sizeof(m_filterText));

            ImGui::Separator();

            if (ImGui::BeginTabBar("AbilityTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_MAGIC " Abilities"))
                {
                    RenderAbilitiesTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_SHIELD " Auras"))
                {
                    RenderAurasTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_BOLT " Procs"))
                {
                    RenderProcsTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void AbilityEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "AbilityEditorPanel shutting down");
    }

    void AbilityEditorPanel::RenderAbilitiesTab()
    {
        if (ImGui::Button(ICON_FA_PLUS " New Ability"))
        {
            AbilityEntry entry;
            entry.id = static_cast<uint32_t>(m_abilities.size()) + 1;
            snprintf(entry.name, sizeof(entry.name), "New Ability %u", entry.id);
            m_abilities.push_back(entry);
            m_selectedAbility = static_cast<int>(m_abilities.size()) - 1;
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "AbilityEditorPanel: created ability '%s' (id=%u)", entry.name,
                           entry.id);
        }

        ImGui::Separator();

        float listWidth = ImGui::GetContentRegionAvail().x * 0.3f;

        // Left: ability list
        ImGui::BeginChild("AbilityList", ImVec2(listWidth, 0), true);
        bool hasFilter = strlen(m_filterText) > 0;

        for (int i = 0; i < static_cast<int>(m_abilities.size()); ++i)
        {
            auto& ab = m_abilities[i];
            if (hasFilter && strstr(ab.name, m_filterText) == nullptr)
                continue;

            char label[160];
            snprintf(label, sizeof(label), "[%u] %s", ab.id, ab.name);
            if (ImGui::Selectable(label, m_selectedAbility == i))
                m_selectedAbility = i;
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right: ability details
        ImGui::BeginChild("AbilityDetails", ImVec2(0, 0), true);
        if (m_selectedAbility >= 0 && m_selectedAbility < static_cast<int>(m_abilities.size()))
        {
            RenderAbilityDetails(m_abilities[m_selectedAbility]);
        }
        else
        {
            ImGui::TextDisabled("Select an ability to edit");
        }
        ImGui::EndChild();
    }

    void AbilityEditorPanel::RenderAbilityDetails(AbilityEntry& ability)
    {
        ImGui::Text("Ability #%u", ability.id);
        ImGui::Separator();

        ImGui::InputText("Name", ability.name, sizeof(ability.name));
        ImGui::InputTextMultiline("Description", ability.description, sizeof(ability.description), ImVec2(-1, 50));

        if (ImGui::CollapsingHeader("Targeting", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* targetTypes[] = {"Self", "Single Target", "Area of Effect", "Cone", "Projectile"};
            ImGui::Combo("Target Type", &ability.targetType, targetTypes, IM_ARRAYSIZE(targetTypes));
            ImGui::DragFloat("Range", &ability.range, 0.5f, 0.0f, 100.0f, "%.1f m");
            ImGui::DragFloat("Radius (AoE)", &ability.radius, 0.5f, 0.0f, 50.0f, "%.1f m");
            ImGui::Checkbox("Requires Target", &ability.requiresTarget);
        }

        if (ImGui::CollapsingHeader("Casting", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::DragFloat("Cast Time", &ability.castTime, 0.1f, 0.0f, 30.0f, "%.1f s");
            ImGui::DragFloat("Cooldown", &ability.cooldown, 0.1f, 0.0f, 3600.0f, "%.1f s");
            ImGui::DragFloat("Resource Cost", &ability.resourceCost, 1.0f, 0.0f, 10000.0f, "%.0f");
            ImGui::Checkbox("Cast While Moving", &ability.canCastWhileMoving);

            ImGui::Checkbox("Channeled", &ability.isChanneled);
            if (ability.isChanneled)
            {
                ImGui::Indent();
                ImGui::DragFloat("Channel Duration", &ability.channelDuration, 0.1f, 0.1f, 60.0f, "%.1f s");
                ImGui::Unindent();
            }
        }

        if (ImGui::CollapsingHeader("Effects"))
        {
            ImGui::Text("Effect slots: %d", ability.effectCount);
            ImGui::TextDisabled("Effects are configured via AbilitySystem::RegisterAbility()");
        }

        ImGui::Separator();
        if (ImGui::Button(ICON_FA_TRASH " Delete"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "AbilityEditorPanel: deleted ability #%u", ability.id);
            m_abilities.erase(m_abilities.begin() + m_selectedAbility);
            m_selectedAbility = -1;
        }
    }

    void AbilityEditorPanel::RenderAurasTab()
    {
        if (ImGui::Button(ICON_FA_PLUS " New Aura"))
        {
            AuraEntry entry;
            entry.id = static_cast<uint32_t>(m_auras.size()) + 1;
            snprintf(entry.name, sizeof(entry.name), "New Aura %u", entry.id);
            m_auras.push_back(entry);
            m_selectedAura = static_cast<int>(m_auras.size()) - 1;
        }

        ImGui::Separator();

        float listWidth = ImGui::GetContentRegionAvail().x * 0.3f;

        // Left: aura list
        ImGui::BeginChild("AuraList", ImVec2(listWidth, 0), true);
        bool hasFilter = strlen(m_filterText) > 0;

        for (int i = 0; i < static_cast<int>(m_auras.size()); ++i)
        {
            auto& aura = m_auras[i];
            if (hasFilter && strstr(aura.name, m_filterText) == nullptr)
                continue;

            char label[160];
            snprintf(label, sizeof(label), "[%u] %s", aura.id, aura.name);
            if (ImGui::Selectable(label, m_selectedAura == i))
                m_selectedAura = i;
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right: aura details
        ImGui::BeginChild("AuraDetails", ImVec2(0, 0), true);
        if (m_selectedAura >= 0 && m_selectedAura < static_cast<int>(m_auras.size()))
        {
            RenderAuraDetails(m_auras[m_selectedAura]);
        }
        else
        {
            ImGui::TextDisabled("Select an aura to edit");
        }
        ImGui::EndChild();
    }

    void AbilityEditorPanel::RenderAuraDetails(AuraEntry& aura)
    {
        ImGui::Text("Aura #%u", aura.id);
        ImGui::Separator();

        ImGui::InputText("Name", aura.name, sizeof(aura.name));

        const char* auraTypes[] = {"Buff",      "Debuff",        "DoT",           "HoT",   "Shield",
                                   "Mod Speed", "Mod Dmg Dealt", "Mod Dmg Taken", "Stun",  "Root",
                                   "Silence",   "Stealth",       "Taunt",         "Custom"};
        ImGui::Combo("Type", &aura.auraType, auraTypes, IM_ARRAYSIZE(auraTypes));

        const char* schools[] = {"Physical", "Fire", "Ice", "Lightning", "Nature", "Shadow", "Holy", "Arcane"};
        ImGui::Combo("School", &aura.school, schools, IM_ARRAYSIZE(schools));

        if (ImGui::CollapsingHeader("Duration & Ticking", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Permanent", &aura.isPermanent);
            if (!aura.isPermanent)
                ImGui::DragFloat("Duration", &aura.duration, 0.5f, 0.0f, 3600.0f, "%.1f s");

            ImGui::DragFloat("Tick Interval", &aura.tickInterval, 0.1f, 0.1f, 60.0f, "%.1f s");
            ImGui::DragFloat("Value/Tick", &aura.valuePerTick, 1.0f, -10000.0f, 10000.0f, "%.0f");
        }

        if (ImGui::CollapsingHeader("Modifiers"))
        {
            ImGui::DragFloat("Flat Modifier", &aura.flatModifier, 1.0f, -10000.0f, 10000.0f, "%.0f");
            ImGui::DragFloat("Percent Modifier", &aura.percentModifier, 0.01f, -10.0f, 10.0f, "%.2f");
        }

        if (ImGui::CollapsingHeader("Stacking"))
        {
            const char* stackTypes[] = {"None (Refresh)", "Stacking", "Separate"};
            ImGui::Combo("Stack Type", &aura.stackType, stackTypes, IM_ARRAYSIZE(stackTypes));
            if (aura.stackType == 1)
                ImGui::DragInt("Max Stacks", &aura.maxStacks, 1.0f, 1, 999);
        }

        if (ImGui::CollapsingHeader("Flags"))
        {
            ImGui::Checkbox("Hidden (no UI)", &aura.isHidden);
            ImGui::Checkbox("Dispellable", &aura.dispellable);
        }

        ImGui::Separator();
        if (ImGui::Button(ICON_FA_TRASH " Delete"))
        {
            m_auras.erase(m_auras.begin() + m_selectedAura);
            m_selectedAura = -1;
        }
    }

    void AbilityEditorPanel::RenderProcsTab()
    {
        if (ImGui::Button(ICON_FA_PLUS " New Proc"))
        {
            ProcEntry entry;
            m_procs.push_back(entry);
            m_selectedProc = static_cast<int>(m_procs.size()) - 1;
        }

        ImGui::Separator();

        if (ImGui::BeginTable("ProcTable", 7,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("##Sel", ImGuiTableColumnFlags_WidthFixed, 20);
            ImGui::TableSetupColumn("Source Aura", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Triggered Ability", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Triggers");
            ImGui::TableSetupColumn("Chance", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Cooldown", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Charges", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_procs.size()); ++i)
            {
                auto& proc = m_procs[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableNextColumn();
                bool selected = (m_selectedProc == i);
                if (ImGui::Selectable("##sel", selected, ImGuiSelectableFlags_SpanAllColumns))
                    m_selectedProc = i;

                ImGui::TableNextColumn();
                ImGui::Text("%u", proc.sourceAuraId);

                ImGui::TableNextColumn();
                ImGui::Text("%u", proc.triggeredAbilityId);

                ImGui::TableNextColumn();
                // Show trigger flags summary
                const char* triggerNames[] = {"Dmg",  "TkDmg", "Heal",  "Kill", "Death", "Cast",
                                              "Crit", "Dodge", "Block", "Hit",  "Miss"};
                for (int t = 0; t < 11; ++t)
                {
                    if (proc.triggerMask & (1u << t))
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", triggerNames[t]);
                    }
                }

                ImGui::TableNextColumn();
                ImGui::Text("%.0f%%", proc.chance);

                ImGui::TableNextColumn();
                ImGui::Text("%.1fs", proc.cooldown);

                ImGui::TableNextColumn();
                ImGui::Text("%d", proc.charges);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // Detail editor for selected proc
        if (m_selectedProc >= 0 && m_selectedProc < static_cast<int>(m_procs.size()))
        {
            auto& proc = m_procs[m_selectedProc];
            ImGui::Separator();
            ImGui::Text("Edit Proc #%d", m_selectedProc);

            int srcAura = static_cast<int>(proc.sourceAuraId);
            if (ImGui::InputInt("Source Aura ID", &srcAura))
                proc.sourceAuraId = static_cast<uint32_t>(std::max(0, srcAura));

            int trigAbility = static_cast<int>(proc.triggeredAbilityId);
            if (ImGui::InputInt("Triggered Ability ID", &trigAbility))
                proc.triggeredAbilityId = static_cast<uint32_t>(std::max(0, trigAbility));

            ImGui::Text("Trigger Conditions:");
            const char* triggerLabels[] = {"On Deal Damage", "On Take Damage",  "On Heal",         "On Kill",
                                           "On Death",       "On Ability Cast", "On Critical Hit", "On Dodge",
                                           "On Block",       "On Hit",          "On Miss"};
            for (int t = 0; t < 11; ++t)
            {
                bool flagSet = (proc.triggerMask & (1u << t)) != 0;
                if (ImGui::Checkbox(triggerLabels[t], &flagSet))
                {
                    if (flagSet)
                        proc.triggerMask |= (1u << t);
                    else
                        proc.triggerMask &= ~(1u << t);
                }
                if (t % 3 != 2 && t < 10)
                    ImGui::SameLine();
            }

            ImGui::DragFloat("Proc Chance", &proc.chance, 1.0f, 0.0f, 100.0f, "%.0f%%");
            ImGui::DragFloat("Internal Cooldown", &proc.cooldown, 0.1f, 0.0f, 300.0f, "%.1f s");
            ImGui::DragInt("Charges (0=unlimited)", &proc.charges, 1.0f, 0, 999);

            if (ImGui::Button(ICON_FA_TRASH " Delete Proc"))
            {
                m_procs.erase(m_procs.begin() + m_selectedProc);
                m_selectedProc = -1;
            }
        }
    }

} // namespace SparkEditor
