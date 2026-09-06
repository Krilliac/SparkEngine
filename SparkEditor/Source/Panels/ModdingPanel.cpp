/**
 * @file ModdingPanel.cpp
 * @brief Implementation of the mod management panel
 */

#include "ModdingPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineContext.h"
#include "Utils/LogMacros.h"
#include <algorithm>
#include <imgui.h>

namespace SparkEditor
{

    ModdingPanel::ModdingPanel() : EditorPanel("Modding", "modding_panel") {}

    bool ModdingPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ModdingPanel initialized");
        RefreshFromSystem();
        return true;
    }

    void ModdingPanel::Update(float /*deltaTime*/) {}

    Spark::ModSystem& ModdingPanel::System()
    {
        if (auto* context = ::EngineContext::Get())
        {
            if (auto* modSystem = context->GetModSystem())
            {
                return *modSystem;
            }
        }
        if (!m_ownedModSystem)
        {
            m_ownedModSystem = std::make_unique<Spark::ModSystem>();
        }
        return *m_ownedModSystem;
    }

    void ModdingPanel::RefreshFromSystem()
    {
        m_mods = System().GetAllMods();
        std::sort(m_mods.begin(), m_mods.end(),
                  [](const Spark::ModInfo& a, const Spark::ModInfo& b)
                  {
                      if (a.loadOrder != b.loadOrder)
                          return a.loadOrder < b.loadOrder;
                      return a.id < b.id;
                  });

        if (m_selectedMod >= static_cast<int>(m_mods.size()))
        {
            m_selectedMod = m_mods.empty() ? -1 : static_cast<int>(m_mods.size()) - 1;
        }
    }

    size_t ModdingPanel::ScanForMods(const std::string& directory)
    {
        const size_t found = System().ScanForMods(directory);
        RefreshFromSystem();
        return found;
    }

    bool ModdingPanel::SetModEnabled(const std::string& modId, bool enabled)
    {
        bool applied = true;
        if (enabled)
        {
            applied = System().EnableMod(modId);
        }
        else
        {
            System().DisableMod(modId);
        }
        RefreshFromSystem();
        return applied;
    }

    bool ModdingPanel::ReloadAll()
    {
        System().UnloadAll();
        const bool loaded = System().LoadEnabledMods();
        RefreshFromSystem();
        return loaded;
    }

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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ModdingPanel shutting down");
    }

    void ModdingPanel::RenderModList()
    {
        ImGui::InputText("Mods Directory", m_modsDirectory, sizeof(m_modsDirectory));

        if (ImGui::Button(ICON_FA_SEARCH " Scan for Mods"))
        {
            const size_t found = ScanForMods(m_modsDirectory);
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "ModdingPanel: scan of '%s' found %zu mod(s)", m_modsDirectory,
                           found);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_REFRESH " Reload All"))
        {
            if (!ReloadAll())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor, "ModdingPanel: one or more enabled mods failed to load");
            }
        }

        ImGui::Separator();

        if (m_mods.empty())
        {
            ImGui::TextDisabled("No mods found.");
            ImGui::TextDisabled("Place mods in the Mods/ directory with a mod.json manifest.");
            return;
        }

        // Enable/disable is applied after the loop: SetModEnabled refreshes m_mods,
        // which must not happen while the list is being iterated.
        std::string pendingToggleId;
        bool pendingToggleValue = false;

        for (int i = 0; i < static_cast<int>(m_mods.size()); ++i)
        {
            const Spark::ModInfo& mod = m_mods[static_cast<size_t>(i)];
            const bool dependenciesMet = System().AreDependenciesMet(mod.id);
            ImGui::PushID(i);

            bool enabled = mod.enabled;
            if (ImGui::Checkbox("##enable", &enabled))
            {
                if (enabled && !dependenciesMet)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Editor,
                                   "ModdingPanel: cannot enable mod '%s' - missing dependencies", mod.name.c_str());
                }
                else
                {
                    pendingToggleId = mod.id;
                    pendingToggleValue = enabled;
                }
            }
            ImGui::SameLine();

            const bool selected = (m_selectedMod == i);
            if (ImGui::Selectable(mod.name.c_str(), selected))
                m_selectedMod = i;

            if (!dependenciesMet)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "(missing deps)");
            }

            ImGui::PopID();
        }

        if (!pendingToggleId.empty())
        {
            SetModEnabled(pendingToggleId, pendingToggleValue);
        }
    }

    void ModdingPanel::RenderModDetails()
    {
        if (m_selectedMod < 0 || m_selectedMod >= static_cast<int>(m_mods.size()))
        {
            ImGui::TextDisabled("Select a mod from the Mods tab.");
            return;
        }

        const Spark::ModInfo& mod = m_mods[m_selectedMod];

        ImGui::Text("Name: %s", mod.name.c_str());
        ImGui::Text("ID: %s", mod.id.c_str());
        ImGui::Text("Author: %s", mod.author.c_str());
        ImGui::Text("Version: %s", mod.version.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("Description: %s", mod.description.c_str());
        ImGui::Separator();

        ImGui::Text("Status: %s", mod.loaded ? "Active" : (mod.enabled ? "Enabled" : "Disabled"));
        ImGui::Text("Load Order: %d", mod.loadOrder);
        ImGui::Text("Dependencies Met: %s", System().AreDependenciesMet(mod.id) ? "Yes" : "No");
    }

    void ModdingPanel::RenderLoadOrder()
    {
        ImGui::TextDisabled("Lower numbers load first. Arrows write the order back to the mod system.");
        ImGui::Separator();

        for (int i = 0; i < static_cast<int>(m_mods.size()); ++i)
        {
            if (!m_mods[i].enabled)
                continue;

            ImGui::PushID(i);
            ImGui::Text("%d.", m_mods[i].loadOrder);
            ImGui::SameLine();

            int swapWith = -1;
            if (ImGui::Button(ICON_FA_ARROW_UP "##up") && i > 0)
                swapWith = i - 1;
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ARROW_DOWN "##down") && i < static_cast<int>(m_mods.size()) - 1)
                swapWith = i + 1;
            ImGui::SameLine();
            ImGui::TextUnformatted(m_mods[i].name.c_str());
            ImGui::PopID();

            if (swapWith >= 0)
            {
                std::vector<std::string> orderedIds;
                orderedIds.reserve(m_mods.size());
                for (const auto& mod : m_mods)
                    orderedIds.push_back(mod.id);
                std::swap(orderedIds[static_cast<size_t>(i)], orderedIds[static_cast<size_t>(swapWith)]);
                System().SetLoadOrder(orderedIds);
                RefreshFromSystem();
                break;
            }
        }
    }

} // namespace SparkEditor
