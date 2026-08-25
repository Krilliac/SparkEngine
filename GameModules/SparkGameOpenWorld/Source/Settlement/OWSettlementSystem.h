/**
 * @file OWSettlementSystem.h
 * @brief NPC settlements, merchants, and player camp building
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages populated areas in the open world: NPC villages and outposts with
 * merchants and services, and a player camp system with progressive upgrades
 * from a basic campfire to a full homestead.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/OpenWorldEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenWorld
{

    /// @brief An NPC inhabitant of a settlement
    struct SettlementNPC
    {
        uint32_t npcId = 0;
        std::string name;
        NPCRole role = NPCRole::Wanderer;
        float disposition = 0.5f; ///< 0 = hostile, 1 = friendly
    };

    /// @brief A populated settlement in the world
    struct Settlement
    {
        uint32_t settlementId = 0;
        std::string name;
        std::string description;
        SettlementType type = SettlementType::Village;
        uint32_t regionId = 0;
        float centerX = 0.0f;
        float centerY = 0.0f;
        float centerZ = 0.0f;
        float radius = 100.0f;
        bool hasMerchant = false;
        bool hasBlacksmith = false;
        bool hasInn = false;
        std::vector<SettlementNPC> npcs;
    };

    /// @brief Player-built camp with progressive upgrades
    struct PlayerCamp
    {
        uint32_t campId = 0;
        std::string name;
        CampTier tier = CampTier::Campfire;
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        uint32_t regionId = 0;
        bool hasCraftingStation = false;
        bool hasStorageChest = false;
        bool hasCookingFire = false;
        uint32_t storageCapacity = 10;
    };

    /// @brief Player-created settlement state stored in an OpenWorld save.
    struct SettlementSaveState
    {
        std::vector<PlayerCamp> camps;
        uint32_t nextCampId = 1;
    };

    /**
     * @brief Settlement and player camp management system
     */
    class OWSettlementSystem
    {
      public:
        OWSettlementSystem() = default;
        ~OWSettlementSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        size_t GetSettlementCount() const { return m_settlements.size(); }
        size_t GetCampCount() const { return m_camps.size(); }
        size_t GetTotalNPCCount() const;

        const Settlement* GetSettlement(uint32_t id) const;
        const Settlement* GetNearestSettlement(float x, float z) const;

        /// @brief Place a new player camp at the given position
        uint32_t PlaceCamp(const std::string& name, float x, float y, float z, uint32_t regionId);

        /// @brief Upgrade an existing camp to the next tier
        bool UpgradeCamp(uint32_t campId);

        SettlementSaveState CaptureSaveState() const;
        bool RestoreSaveState(const SettlementSaveState& state, std::string* error = nullptr);

        std::string GetSettlementListString() const;
        std::string GetCampListString() const;

      private:
        void DefineSettlements();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<Settlement> m_settlements;
        std::unordered_map<uint32_t, PlayerCamp> m_camps;
        uint32_t m_nextCampId{1};
        bool m_initialized{false};
    };

} // namespace OpenWorld
