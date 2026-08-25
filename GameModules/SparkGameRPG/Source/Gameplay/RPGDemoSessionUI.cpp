/**
 * @file RPGDemoSessionUI.cpp
 * @brief Editor controls for the playable SparkGameRPG showcase
 */

#include "RPGDemoSession.h"

#include "NPC/RPGNPCSystem.h"
#include "World/RPGWorldSetup.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace RPG
{
    void RPGDemoSession::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Oakhollow Adventure", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        ImGui::TextUnformatted(GetStatusString().c_str());
        ImGui::Separator();
        if (m_activeEncounterId != 0)
        {
            if (ImGui::Button("Attack"))
            {
                Attack();
            }
            ImGui::SameLine();
            if (ImGui::Button("Flee"))
            {
                Flee();
            }
            ImGui::SameLine();
            if (ImGui::Button("Use Health Potion"))
            {
                UseItem(1);
            }
        }
        else
        {
            const auto* area = m_world ? m_world->GetArea(m_currentAreaId) : nullptr;
            if (area)
            {
                for (const uint32_t connectedAreaId : area->connectedAreas)
                {
                    const auto* destination = m_world->GetArea(connectedAreaId);
                    if (destination && ImGui::Button(("Travel to " + destination->name).c_str()))
                    {
                        Travel(connectedAreaId);
                    }
                }
                if (area->isSafeZone && ImGui::Button("Rest at the Inn"))
                {
                    Rest();
                }
                if (m_npcs && ImGui::TreeNode("Nearby NPCs"))
                {
                    for (const auto* npc : m_npcs->GetNPCsInArea(m_currentAreaId))
                    {
                        if (npc && ImGui::Button(("Talk to " + npc->name).c_str()))
                        {
                            Talk(npc->npcId);
                        }
                    }
                    ImGui::TreePop();
                }
            }
        }
#endif
    }

} // namespace RPG
