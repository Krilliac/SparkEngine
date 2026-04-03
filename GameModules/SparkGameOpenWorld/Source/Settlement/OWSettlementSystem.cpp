/**
 * @file OWSettlementSystem.cpp
 * @brief NPC settlements and player camp management
 */

#include "OWSettlementSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <cmath>
#include <sstream>

namespace OpenWorld
{

    bool OWSettlementSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        DefineSettlements();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Settlement system initialized: %zu settlements",
                       m_settlements.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Settlements: " + std::to_string(m_settlements.size()) +
                                                    " | NPCs: " + std::to_string(GetTotalNPCCount()));
        return true;
    }

    void OWSettlementSystem::DefineSettlements()
    {
        m_settlements.clear();
        uint32_t npcId = 1;

        // Meadowbrook — starting village in Emerald Meadows
        {
            Settlement s{};
            s.settlementId = 1;
            s.name = "Meadowbrook";
            s.description = "A friendly farming village at the heart of the meadows";
            s.type = SettlementType::Village;
            s.regionId = 1;
            s.centerX = 100.0f;
            s.centerY = 5.0f;
            s.centerZ = -100.0f;
            s.radius = 150.0f;
            s.hasMerchant = true;
            s.hasBlacksmith = true;
            s.hasInn = true;
            s.npcs = {
                {npcId++, "Marta the Merchant", NPCRole::Merchant, 0.8f},
                {npcId++, "Gorrik the Smith", NPCRole::Blacksmith, 0.7f},
                {npcId++, "Old Hilda", NPCRole::Innkeeper, 0.9f},
                {npcId++, "Captain Brynn", NPCRole::Guard, 0.6f},
                {npcId++, "Farmer Jorik", NPCRole::Farmer, 0.8f},
                {npcId++, "Elder Aldric", NPCRole::Elder, 0.9f},
            };
            m_settlements.push_back(s);
        }

        // Timberhold — forest outpost in Ironwood Forest
        {
            Settlement s{};
            s.settlementId = 2;
            s.name = "Timberhold";
            s.description = "A logging outpost surrounded by towering ironwood trees";
            s.type = SettlementType::Outpost;
            s.regionId = 2;
            s.centerX = 2800.0f;
            s.centerY = 15.0f;
            s.centerZ = 0.0f;
            s.radius = 80.0f;
            s.hasMerchant = true;
            s.hasBlacksmith = false;
            s.hasInn = false;
            s.npcs = {
                {npcId++, "Theren the Woodsman", NPCRole::Hunter, 0.6f},
                {npcId++, "Lina the Trader", NPCRole::Merchant, 0.7f},
                {npcId++, "Sentry Dael", NPCRole::Guard, 0.5f},
            };
            m_settlements.push_back(s);
        }

        // Highpass — mountain town in Stormcrest Mountains
        {
            Settlement s{};
            s.settlementId = 3;
            s.name = "Highpass";
            s.description = "A fortified mountain town guarding the only pass through the peaks";
            s.type = SettlementType::Town;
            s.regionId = 3;
            s.centerX = 200.0f;
            s.centerY = 300.0f;
            s.centerZ = 3000.0f;
            s.radius = 120.0f;
            s.hasMerchant = true;
            s.hasBlacksmith = true;
            s.hasInn = true;
            s.npcs = {
                {npcId++, "Ingrid the Armorer", NPCRole::Blacksmith, 0.6f},
                {npcId++, "Ulfar the Supplier", NPCRole::Merchant, 0.5f},
                {npcId++, "Bruni the Innkeeper", NPCRole::Innkeeper, 0.7f},
                {npcId++, "Watch Commander Sigrun", NPCRole::Guard, 0.4f},
                {npcId++, "Elder Thorvald", NPCRole::Elder, 0.7f},
            };
            m_settlements.push_back(s);
        }

        // Dusthaven — desert oasis settlement
        {
            Settlement s{};
            s.settlementId = 4;
            s.name = "Dusthaven";
            s.description = "A shaded oasis settlement where desert traders converge";
            s.type = SettlementType::Town;
            s.regionId = 4;
            s.centerX = 6000.0f;
            s.centerY = 10.0f;
            s.centerZ = -1000.0f;
            s.radius = 100.0f;
            s.hasMerchant = true;
            s.hasBlacksmith = true;
            s.hasInn = true;
            s.npcs = {
                {npcId++, "Zara the Jeweler", NPCRole::Merchant, 0.6f},
                {npcId++, "Tamir the Bladesmith", NPCRole::Blacksmith, 0.5f},
                {npcId++, "Keeper Amara", NPCRole::Innkeeper, 0.8f},
                {npcId++, "Desert Ranger Khalid", NPCRole::Guard, 0.5f},
            };
            m_settlements.push_back(s);
        }

        // Frosthome — tundra survival camp
        {
            Settlement s{};
            s.settlementId = 5;
            s.name = "Frosthome";
            s.description = "A hardy encampment of fur-clad hunters braving the frozen wastes";
            s.type = SettlementType::Outpost;
            s.regionId = 5;
            s.centerX = -3000.0f;
            s.centerY = 15.0f;
            s.centerZ = 4000.0f;
            s.radius = 60.0f;
            s.hasMerchant = true;
            s.hasBlacksmith = false;
            s.hasInn = false;
            s.npcs = {
                {npcId++, "Ymir the Trapper", NPCRole::Hunter, 0.5f},
                {npcId++, "Helga the Fur Trader", NPCRole::Merchant, 0.6f},
            };
            m_settlements.push_back(s);
        }

        // Bogwatch — swamp farmstead
        {
            Settlement s{};
            s.settlementId = 6;
            s.name = "Bogwatch";
            s.description = "A cluster of stilted huts perched above the murky swamp waters";
            s.type = SettlementType::Farmstead;
            s.regionId = 6;
            s.centerX = -3200.0f;
            s.centerY = 5.0f;
            s.centerZ = -500.0f;
            s.radius = 50.0f;
            s.hasMerchant = false;
            s.hasBlacksmith = false;
            s.hasInn = false;
            s.npcs = {
                {npcId++, "Marsh Warden Fen", NPCRole::Guard, 0.4f},
                {npcId++, "Herbalist Moss", NPCRole::Farmer, 0.7f},
            };
            m_settlements.push_back(s);
        }

        // Port Sunbreak — coastal trade hub
        {
            Settlement s{};
            s.settlementId = 7;
            s.name = "Port Sunbreak";
            s.description = "A prosperous port city with a bustling marketplace and harbor";
            s.type = SettlementType::Town;
            s.regionId = 7;
            s.centerX = 1000.0f;
            s.centerY = 5.0f;
            s.centerZ = -5000.0f;
            s.radius = 200.0f;
            s.hasMerchant = true;
            s.hasBlacksmith = true;
            s.hasInn = true;
            s.npcs = {
                {npcId++, "Captain Reva", NPCRole::Merchant, 0.7f},
                {npcId++, "Dock Master Finn", NPCRole::Merchant, 0.6f},
                {npcId++, "Coral the Smith", NPCRole::Blacksmith, 0.6f},
                {npcId++, "Harbor Guard Orin", NPCRole::Guard, 0.5f},
                {npcId++, "Wanderer Sable", NPCRole::Wanderer, 0.8f},
                {npcId++, "Tavern Keep Rossi", NPCRole::Innkeeper, 0.7f},
            };
            m_settlements.push_back(s);
        }

        // Bandit camps scattered in dangerous regions
        {
            Settlement s{};
            s.settlementId = 8;
            s.name = "Blackthorn Camp";
            s.description = "A fortified bandit hideout in the forest depths";
            s.type = SettlementType::BanditCamp;
            s.regionId = 2;
            s.centerX = 4500.0f;
            s.centerY = 10.0f;
            s.centerZ = -2000.0f;
            s.radius = 40.0f;
            s.npcs = {
                {npcId++, "Bandit Leader Grix", NPCRole::Guard, 0.0f},
                {npcId++, "Bandit Lookout", NPCRole::Guard, 0.0f},
            };
            m_settlements.push_back(s);
        }
    }

    void OWSettlementSystem::Update([[maybe_unused]] float deltaTime)
    {
        if (!m_initialized)
            return;

        // NPC AI updates, disposition changes, shop restocking would happen here
    }

    void OWSettlementSystem::Shutdown()
    {
        if (!m_initialized)
            return;

        m_settlements.clear();
        m_camps.clear();
        m_initialized = false;
    }

    size_t OWSettlementSystem::GetTotalNPCCount() const
    {
        size_t total = 0;
        for (const auto& s : m_settlements)
            total += s.npcs.size();
        return total;
    }

    const Settlement* OWSettlementSystem::GetSettlement(uint32_t id) const
    {
        for (const auto& s : m_settlements)
        {
            if (s.settlementId == id)
                return &s;
        }
        return nullptr;
    }

    const Settlement* OWSettlementSystem::GetNearestSettlement(float x, float z) const
    {
        const Settlement* nearest = nullptr;
        float bestDist = std::numeric_limits<float>::max();

        for (const auto& s : m_settlements)
        {
            float dx = x - s.centerX;
            float dz = z - s.centerZ;
            float dist = std::sqrt(dx * dx + dz * dz);
            if (dist < bestDist)
            {
                bestDist = dist;
                nearest = &s;
            }
        }
        return nearest;
    }

    uint32_t OWSettlementSystem::PlaceCamp(const std::string& name, float x, float y, float z, uint32_t regionId)
    {
        PlayerCamp camp{};
        camp.campId = m_nextCampId++;
        camp.name = name;
        camp.tier = CampTier::Campfire;
        camp.posX = x;
        camp.posY = y;
        camp.posZ = z;
        camp.regionId = regionId;
        camp.storageCapacity = 10;

        m_camps[camp.campId] = camp;

        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Camp '" + name +
                                                    "' placed (id=" + std::to_string(camp.campId) + ")");
        return camp.campId;
    }

    bool OWSettlementSystem::UpgradeCamp(uint32_t campId)
    {
        auto it = m_camps.find(campId);
        if (it == m_camps.end())
            return false;

        auto& camp = it->second;
        if (camp.tier >= CampTier::Homestead)
            return false;

        camp.tier = static_cast<CampTier>(static_cast<uint8_t>(camp.tier) + 1);
        camp.storageCapacity += 10;

        // Unlock features per tier
        if (camp.tier >= CampTier::Lean_To)
            camp.hasCookingFire = true;
        if (camp.tier >= CampTier::Tent)
            camp.hasStorageChest = true;
        if (camp.tier >= CampTier::Cabin)
            camp.hasCraftingStation = true;

        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Camp '" + camp.name + "' upgraded to tier " +
                                                    std::to_string(static_cast<int>(camp.tier)));
        return true;
    }

    std::string OWSettlementSystem::GetSettlementListString() const
    {
        std::ostringstream ss;
        ss << "=== Settlements ===\n";
        for (const auto& s : m_settlements)
        {
            ss << "[" << s.settlementId << "] " << s.name << " (Region " << s.regionId << ")\n";
            ss << "    " << s.description << "\n";
            ss << "    NPCs: " << s.npcs.size();
            if (s.hasMerchant)
                ss << " | Merchant";
            if (s.hasBlacksmith)
                ss << " | Blacksmith";
            if (s.hasInn)
                ss << " | Inn";
            ss << "\n";
        }
        return ss.str();
    }

    std::string OWSettlementSystem::GetCampListString() const
    {
        std::ostringstream ss;
        ss << "=== Player Camps ===\n";
        if (m_camps.empty())
        {
            ss << "No camps placed\n";
            return ss.str();
        }
        for (const auto& [id, camp] : m_camps)
        {
            ss << "[" << id << "] " << camp.name << " (Tier " << static_cast<int>(camp.tier) << ")\n";
            ss << "    Position: (" << camp.posX << ", " << camp.posY << ", " << camp.posZ << ")\n";
            ss << "    Storage: " << camp.storageCapacity << " slots";
            if (camp.hasCookingFire)
                ss << " | Cooking";
            if (camp.hasCraftingStation)
                ss << " | Crafting";
            if (camp.hasStorageChest)
                ss << " | Chest";
            ss << "\n";
        }
        return ss.str();
    }

    void OWSettlementSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Settlements"))
            return;

        ImGui::Text("Settlements: %zu | NPCs: %zu | Player Camps: %zu", m_settlements.size(), GetTotalNPCCount(),
                    m_camps.size());
        ImGui::Separator();

        for (const auto& s : m_settlements)
        {
            ImGui::PushID(static_cast<int>(s.settlementId));
            if (ImGui::TreeNode(s.name.c_str()))
            {
                ImGui::Text("Region: %u | Type: %d | NPCs: %zu", s.regionId, static_cast<int>(s.type), s.npcs.size());
                ImGui::TextWrapped("%s", s.description.c_str());
                for (const auto& npc : s.npcs)
                {
                    ImGui::Text("  %s (Disposition: %.1f)", npc.name.c_str(), npc.disposition);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        if (!m_camps.empty() && ImGui::TreeNode("Player Camps"))
        {
            for (const auto& [id, camp] : m_camps)
            {
                ImGui::Text("[%u] %s (Tier %d, Storage %u)", id, camp.name.c_str(), static_cast<int>(camp.tier),
                            camp.storageCapacity);
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace OpenWorld
