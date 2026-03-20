/**
 * @file ModdingPanel.cpp
 * @brief Implementation of the mod management panel
 */

#include "ModdingPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor
{

    ModdingPanel::ModdingPanel() : EditorPanel("Modding", "modding_panel") {}

    bool ModdingPanel::Initialize()
    {
        std::cout << "Initializing Modding panel\n";
        return true;
    }

    void ModdingPanel::Update(float /*deltaTime*/) {}

    void ModdingPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("ModdingTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_BOXES " Mods"))
                {
                    RenderModList();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_SLIDERS " Details"))
                {
                    RenderModDetails();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_LIST " Load Order"))
                {
                    RenderLoadOrder();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void ModdingPanel::Shutdown()
    {
        std::cout << "Shutting down Modding panel\n";
    }

    void ModdingPanel::RenderModList()
    {
        ImGui::InputText("Mods Directory", m_modsDirectory, sizeof(m_modsDirectory));

        if (ImGui::Button(ICON_FA_SEARCH " Scan for Mods"))
        {
            // Would call ModSystem::ScanForMods(m_modsDirectory)
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_REFRESH " Reload All"))
        {
            // Would call ModSystem::UnloadAll() then LoadEnabledMods()
        }

        ImGui::Separator();

        if (m_mods.empty())
        {
            ImGui::TextDisabled("No mods found.");
            ImGui::TextDisabled("Place mods in the Mods/ directory with a mod.json manifest.");
            return;
        }

        for (int i = 0; i < static_cast<int>(m_mods.size()); ++i)
        {
            auto& mod = m_mods[i];
            ImGui::PushID(i);

            bool changed = ImGui::Checkbox("##enable", &mod.enabled);
            ImGui::SameLine();

            bool selected = (m_selectedMod == i);
            if (ImGui::Selectable(mod.name, selected))
                m_selectedMod = i;

            if (!mod.dependenciesMet)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "(missing deps)");
            }

            if (changed && mod.enabled && !mod.dependenciesMet)
            {
                mod.enabled = false;
            }

            ImGui::PopID();
        }
    }

    void ModdingPanel::RenderModDetails()
    {
        if (m_selectedMod < 0 || m_selectedMod >= static_cast<int>(m_mods.size()))
        {
            ImGui::TextDisabled("Select a mod from the Mods tab.");
            return;
        }

        auto& mod = m_mods[m_selectedMod];

        ImGui::Text("Name: %s", mod.name);
        ImGui::Text("ID: %s", mod.id);
        ImGui::Text("Author: %s", mod.author);
        ImGui::Text("Version: %s", mod.version);
        ImGui::Separator();
        ImGui::TextWrapped("Description: %s", mod.description);
        ImGui::Separator();

        ImGui::Text("Status: %s", mod.loaded ? "Active" : (mod.enabled ? "Enabled" : "Disabled"));
        ImGui::Text("Load Order: %d", mod.loadOrder);
        ImGui::Text("Dependencies Met: %s", mod.dependenciesMet ? "Yes" : "No");
    }

    void ModdingPanel::RenderLoadOrder()
    {
        ImGui::TextDisabled("Drag mods to reorder. Lower numbers load first.");
        ImGui::Separator();

        for (int i = 0; i < static_cast<int>(m_mods.size()); ++i)
        {
            if (!m_mods[i].enabled)
                continue;

            ImGui::PushID(i);
            ImGui::Text("%d.", m_mods[i].loadOrder);
            ImGui::SameLine();

            if (ImGui::Button(ICON_FA_ARROW_UP "##up") && i > 0)
            {
                std::swap(m_mods[i].loadOrder, m_mods[i - 1].loadOrder);
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ARROW_DOWN "##down") && i < static_cast<int>(m_mods.size()) - 1)
            {
                std::swap(m_mods[i].loadOrder, m_mods[i + 1].loadOrder);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(m_mods[i].name);
            ImGui::PopID();
        }
    }

} // namespace SparkEditor
