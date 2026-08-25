/**
 * @file RPGNPCSystem.cpp
 * @brief NPC behavior, schedule updates, patrol movement, and disposition
 */

#include "RPGNPCSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <sstream>

namespace RPG
{

    bool RPGNPCSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultNPCs();

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG NPC system initialized with %zu NPCs", m_npcs.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] NPC system initialized (" + std::to_string(m_npcs.size()) +
                                                    " NPCs)");
        return true;
    }

    void RPGNPCSystem::Update(float deltaTime)
    {
        deltaTime = std::max(0.0f, deltaTime);

        // Advance world time
        m_worldTime += deltaTime;
        m_worldHour += deltaTime / SECONDS_PER_GAME_HOUR;
        if (m_worldHour >= 24.0f)
            m_worldHour -= 24.0f;

        UpdateSchedules();
        UpdatePatrols(deltaTime);
    }

    void RPGNPCSystem::Shutdown()
    {
        m_npcs.clear();
    }

    void RPGNPCSystem::RegisterDefaultNPCs()
    {
        // === Village NPCs (Area 1) ===

        // Blacksmith — merchant by day, idle at night
        NPCData blacksmith;
        blacksmith.npcId = 1;
        blacksmith.name = "Grimjaw the Blacksmith";
        blacksmith.areaId = 1;
        blacksmith.currentBehavior = NPCBehavior::Merchant;
        blacksmith.disposition = NPCDisposition::Friendly;
        blacksmith.dispositionValue = 60;
        blacksmith.posX = 50.0f;
        blacksmith.posY = 0.0f;
        blacksmith.posZ = 30.0f;
        blacksmith.dialogueTreeId = 1;
        blacksmith.shopItems = {10, 11, 20, 21, 30, 31, 60, 70, 80};
        blacksmith.schedule = {
            {6.0f, 20.0f, NPCBehavior::Merchant, 50.0f, 0.0f, 30.0f},
            {20.0f, 6.0f, NPCBehavior::Idle, 55.0f, 0.0f, 40.0f},
        };
        m_npcs[1] = blacksmith;

        // Village Elder — quest giver
        NPCData elder;
        elder.npcId = 2;
        elder.name = "Elder Mirwen";
        elder.areaId = 1;
        elder.currentBehavior = NPCBehavior::QuestGiver;
        elder.disposition = NPCDisposition::Friendly;
        elder.dispositionValue = 70;
        elder.posX = 0.0f;
        elder.posY = 0.0f;
        elder.posZ = 0.0f;
        elder.dialogueTreeId = 2;
        elder.questId = 1;
        elder.schedule = {
            {7.0f, 19.0f, NPCBehavior::QuestGiver, 0.0f, 0.0f, 0.0f},
            {19.0f, 7.0f, NPCBehavior::Idle, 10.0f, 0.0f, -20.0f},
        };
        m_npcs[2] = elder;

        // Village Guard — patrols by day, guards gate at night
        NPCData guard;
        guard.npcId = 3;
        guard.name = "Captain Aldric";
        guard.areaId = 1;
        guard.currentBehavior = NPCBehavior::Patrol;
        guard.disposition = NPCDisposition::Neutral;
        guard.dispositionValue = 50;
        guard.posX = -50.0f;
        guard.posY = 0.0f;
        guard.posZ = 100.0f;
        guard.patrolPath = {
            {-50.0f, 0.0f, 100.0f, 3.0f},
            {50.0f, 0.0f, 100.0f, 2.0f},
            {50.0f, 0.0f, -100.0f, 3.0f},
            {-50.0f, 0.0f, -100.0f, 2.0f},
        };
        guard.schedule = {
            {6.0f, 22.0f, NPCBehavior::Patrol, -50.0f, 0.0f, 100.0f},
            {22.0f, 6.0f, NPCBehavior::Guard, 0.0f, 0.0f, 180.0f},
        };
        m_npcs[3] = guard;

        // Herbalist — merchant, sells potions
        NPCData herbalist;
        herbalist.npcId = 4;
        herbalist.name = "Liana the Herbalist";
        herbalist.areaId = 1;
        herbalist.currentBehavior = NPCBehavior::Merchant;
        herbalist.disposition = NPCDisposition::Friendly;
        herbalist.dispositionValue = 65;
        herbalist.posX = -30.0f;
        herbalist.posY = 0.0f;
        herbalist.posZ = -50.0f;
        herbalist.shopItems = {1, 2, 3, 4};
        herbalist.schedule = {
            {8.0f, 18.0f, NPCBehavior::Merchant, -30.0f, 0.0f, -50.0f},
            {18.0f, 8.0f, NPCBehavior::Idle, -35.0f, 0.0f, -45.0f},
        };
        m_npcs[4] = herbalist;

        // === Forest NPCs (Area 2) ===

        // Hermit — quest giver, reclusive
        NPCData hermit;
        hermit.npcId = 5;
        hermit.name = "Old Theron";
        hermit.areaId = 2;
        hermit.currentBehavior = NPCBehavior::QuestGiver;
        hermit.disposition = NPCDisposition::Neutral;
        hermit.dispositionValue = 40;
        hermit.posX = 800.0f;
        hermit.posY = 0.0f;
        hermit.posZ = -200.0f;
        hermit.questId = 3;
        m_npcs[5] = hermit;

        // Forest Ranger — patrols the woods
        NPCData ranger;
        ranger.npcId = 6;
        ranger.name = "Ranger Elara";
        ranger.areaId = 2;
        ranger.currentBehavior = NPCBehavior::Patrol;
        ranger.disposition = NPCDisposition::Friendly;
        ranger.dispositionValue = 55;
        ranger.posX = 500.0f;
        ranger.posY = 0.0f;
        ranger.posZ = 0.0f;
        ranger.patrolPath = {
            {500.0f, 0.0f, 0.0f, 4.0f},
            {700.0f, 0.0f, 200.0f, 3.0f},
            {900.0f, 0.0f, -100.0f, 4.0f},
            {700.0f, 0.0f, -300.0f, 3.0f},
        };
        m_npcs[6] = ranger;

        // === Swamp NPCs (Area 5) ===

        // Swamp Merchant — trades rare ingredients
        NPCData swampMerchant;
        swampMerchant.npcId = 7;
        swampMerchant.name = "Bogsworth";
        swampMerchant.areaId = 5;
        swampMerchant.currentBehavior = NPCBehavior::Merchant;
        swampMerchant.disposition = NPCDisposition::Neutral;
        swampMerchant.dispositionValue = 45;
        swampMerchant.posX = -350.0f;
        swampMerchant.posY = 0.0f;
        swampMerchant.posZ = -300.0f;
        swampMerchant.shopItems = {3, 4, 51};
        swampMerchant.questId = 5;
        m_npcs[7] = swampMerchant;
    }

    // === NPC queries ===

    NPCData* RPGNPCSystem::GetNPC(uint32_t npcId)
    {
        auto it = m_npcs.find(npcId);
        return it != m_npcs.end() ? &it->second : nullptr;
    }

    const NPCData* RPGNPCSystem::GetNPC(uint32_t npcId) const
    {
        auto it = m_npcs.find(npcId);
        return it != m_npcs.end() ? &it->second : nullptr;
    }

    std::vector<const NPCData*> RPGNPCSystem::GetNPCsInArea(uint32_t areaId) const
    {
        std::vector<const NPCData*> result;
        for (const auto& [id, npc] : m_npcs)
        {
            if (npc.areaId == areaId)
                result.push_back(&npc);
        }
        return result;
    }

    std::string RPGNPCSystem::GetNPCListString() const
    {
        std::ostringstream ss;
        ss << "=== RPG NPCs ===\n";
        ss << "World Hour: " << static_cast<int>(m_worldHour) << ":"
           << static_cast<int>((m_worldHour - static_cast<int>(m_worldHour)) * 60) << "\n";

        for (const auto& [id, npc] : m_npcs)
        {
            ss << "[" << id << "] " << npc.name << " (Area " << npc.areaId << ") - ";

            switch (npc.currentBehavior)
            {
            case NPCBehavior::Idle:
                ss << "Idle";
                break;
            case NPCBehavior::Patrol:
                ss << "Patrol";
                break;
            case NPCBehavior::Guard:
                ss << "Guard";
                break;
            case NPCBehavior::Merchant:
                ss << "Merchant";
                break;
            case NPCBehavior::QuestGiver:
                ss << "QuestGiver";
                break;
            default:
                ss << "Unknown";
                break;
            }

            ss << " ["
               << (npc.disposition == NPCDisposition::Friendly  ? "Friendly"
                   : npc.disposition == NPCDisposition::Neutral ? "Neutral"
                                                                : "Hostile")
               << "]\n";
        }
        return ss.str();
    }

    // === Disposition ===

    void RPGNPCSystem::AdjustDisposition(uint32_t npcId, int change)
    {
        auto* npc = GetNPC(npcId);
        if (!npc)
            return;

        npc->dispositionValue += change;

        // Clamp to 0-100
        if (npc->dispositionValue < 0)
            npc->dispositionValue = 0;
        if (npc->dispositionValue > 100)
            npc->dispositionValue = 100;

        npc->disposition = GetDispositionTier(npc->dispositionValue);

        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "RPG NPC %s disposition changed to %d", npc->name.c_str(),
                        npc->dispositionValue);
        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] " + npc->name +
                                                    " disposition: " + std::to_string(npc->dispositionValue));
    }

    NPCDisposition RPGNPCSystem::GetDispositionTier(int value) const
    {
        if (value < 25)
            return NPCDisposition::Hostile;
        if (value < 60)
            return NPCDisposition::Neutral;
        return NPCDisposition::Friendly;
    }

    // === Internal updates ===

    void RPGNPCSystem::UpdateSchedules()
    {
        for (auto& [id, npc] : m_npcs)
        {
            if (npc.schedule.empty())
                continue;

            for (const auto& entry : npc.schedule)
            {
                bool inRange = false;
                if (entry.startHour < entry.endHour)
                {
                    // Normal range (e.g., 6-20)
                    inRange = (m_worldHour >= entry.startHour && m_worldHour < entry.endHour);
                }
                else
                {
                    // Wrapping range (e.g., 20-6)
                    inRange = (m_worldHour >= entry.startHour || m_worldHour < entry.endHour);
                }

                if (inRange)
                {
                    npc.currentBehavior = entry.behavior;
                    // Move to schedule position (in a real game this would be pathfinding)
                    npc.posX = entry.posX;
                    npc.posY = entry.posY;
                    npc.posZ = entry.posZ;
                    break;
                }
            }
        }
    }

    void RPGNPCSystem::UpdatePatrols(float deltaTime)
    {
        for (auto& [id, npc] : m_npcs)
        {
            if (npc.currentBehavior != NPCBehavior::Patrol || npc.patrolPath.empty())
                continue;

            auto& wp = npc.patrolPath[npc.currentWaypointIndex];

            // Simple move-toward-waypoint
            float dx = wp.x - npc.posX;
            float dz = wp.z - npc.posZ;
            float dist = std::sqrt(dx * dx + dz * dz);

            if (dist < 1.0f)
            {
                // At waypoint — wait
                npc.waypointWaitTimer -= deltaTime;
                if (npc.waypointWaitTimer <= 0.0f)
                {
                    npc.currentWaypointIndex = (npc.currentWaypointIndex + 1) % static_cast<int>(npc.patrolPath.size());
                    auto& nextWp = npc.patrolPath[npc.currentWaypointIndex];
                    npc.waypointWaitTimer = nextWp.waitTime;
                }
            }
            else
            {
                // Move toward waypoint
                float moveSpeed = 3.0f; // Units per second
                float step = moveSpeed * deltaTime;
                if (step > dist)
                    step = dist;

                npc.posX += (dx / dist) * step;
                npc.posZ += (dz / dist) * step;
            }
        }
    }

    void RPGNPCSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RPG NPC System"))
        {
            ImGui::Text("NPCs: %zu | World Hour: %.1f", m_npcs.size(), m_worldHour);

            for (const auto& [id, npc] : m_npcs)
            {
                ImGui::PushID(static_cast<int>(id));
                if (ImGui::TreeNode(npc.name.c_str()))
                {
                    ImGui::Text("Area: %u | Pos: (%.1f, %.1f, %.1f)", npc.areaId, npc.posX, npc.posY, npc.posZ);

                    const char* behaviorStr = "Unknown";
                    switch (npc.currentBehavior)
                    {
                    case NPCBehavior::Idle:
                        behaviorStr = "Idle";
                        break;
                    case NPCBehavior::Patrol:
                        behaviorStr = "Patrol";
                        break;
                    case NPCBehavior::Guard:
                        behaviorStr = "Guard";
                        break;
                    case NPCBehavior::Merchant:
                        behaviorStr = "Merchant";
                        break;
                    case NPCBehavior::QuestGiver:
                        behaviorStr = "QuestGiver";
                        break;
                    default:
                        break;
                    }
                    ImGui::Text("Behavior: %s | Disposition: %d", behaviorStr, npc.dispositionValue);

                    if (!npc.shopItems.empty())
                        ImGui::Text("Shop items: %zu", npc.shopItems.size());
                    if (npc.dialogueTreeId > 0)
                        ImGui::Text("Dialogue tree: %u", npc.dialogueTreeId);
                    if (npc.questId > 0)
                        ImGui::Text("Quest: %u", npc.questId);

                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RPG
