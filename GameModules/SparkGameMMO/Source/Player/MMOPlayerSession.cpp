/** @file MMOPlayerSession.cpp
 * @brief Authenticated server actors and authoritative client snapshots.
 */
#include "MMOPlayerSystem.h"
#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif
#include <cmath>
#include <limits>
#include <utility>

namespace MMO
{
    bool MMOPlayerSystem::SpawnSessionPlayer(uint32_t clientId, uint32_t characterId, const std::string& name,
                                             uint32_t areaId, float x, float y, float z, float health)
    {
        if (clientId == 0 || characterId == 0 || name.empty() || areaId == 0 || !std::isfinite(x) ||
            !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(health) || health <= 0.0f)
        {
            return false;
        }
        if (m_players.contains(clientId))
        {
            return false;
        }

        MMOPlayer player{};
        player.clientId = clientId;
        player.characterId = characterId;
        player.name = name;
        player.currentAreaId = areaId;
        player.posX = player.targetPosX = x;
        player.posY = player.targetPosY = y;
        player.posZ = player.targetPosZ = z;
        player.health = player.maxHealth = health;

#ifdef ENABLE_NETWORKING
        auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;
        if (!netMgr || netMgr->GetRole() != Spark::Net::NetworkRole::Server)
        {
            return false;
        }

        Spark::Net::ReplicatedEntity replicated;
        replicated.ownerID = clientId;
        replicated.entityType = "MMOPlayer";
        replicated.position = {x, y, z};
        replicated.areaId = areaId;
        player.networkId = netMgr->RegisterReplicatedEntity(replicated);
        if (player.networkId == 0)
        {
            return false;
        }
#else
        (void)clientId;
#endif

        m_players.emplace(clientId, std::move(player));
        return true;
    }

    bool MMOPlayerSystem::MoveSessionPlayer(uint32_t clientId, float moveX, float moveZ, float deltaTime)
    {
        auto it = m_players.find(clientId);
        if (it == m_players.end() || !std::isfinite(moveX) || !std::isfinite(moveZ) || !std::isfinite(deltaTime) ||
            deltaTime <= 0.0f || it->second.health <= 0.0f)
        {
            return false;
        }

        MMOPlayer movement;
        movement.posX = it->second.posX;
        movement.posY = it->second.posY;
        movement.posZ = it->second.posZ;
        IntegrateMovement(movement, MMOPlayerInput{moveX, moveZ, false}, deltaTime);
        if (!std::isfinite(movement.posX) || !std::isfinite(movement.posZ) || std::abs(movement.posX) > 1.0e6f ||
            std::abs(movement.posZ) > 1.0e6f)
        {
            return false;
        }

#ifdef ENABLE_NETWORKING
        auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;
        if (netMgr && netMgr->GetRole() == Spark::Net::NetworkRole::Server && it->second.networkId != 0)
        {
            Spark::Net::ReplicatedEntityUpdate update;
            update.position = {movement.posX, movement.posY, movement.posZ};
            update.velocity = {movement.velocityX, movement.velocityY, movement.velocityZ};
            update.areaId = it->second.currentAreaId;
            update.needsFullSync = true;
            if (!netMgr->UpdateReplicatedEntity(it->second.networkId, update))
            {
                return false;
            }
        }
#endif
        it->second.posX = it->second.targetPosX = movement.posX;
        it->second.posZ = it->second.targetPosZ = movement.posZ;
        it->second.velocityX = movement.velocityX;
        it->second.velocityZ = movement.velocityZ;
        return true;
    }

    bool MMOPlayerSystem::InteractSessionPlayer(uint32_t clientId, uint32_t targetClientId)
    {
        if (clientId == targetClientId)
        {
            return false;
        }
        auto source = m_players.find(clientId);
        auto target = m_players.find(targetClientId);
        if (source == m_players.end() || target == m_players.end() || source->second.health <= 0.0f ||
            target->second.health <= 0.0f || source->second.currentAreaId != target->second.currentAreaId)
        {
            return false;
        }

        const float dx = source->second.posX - target->second.posX;
        const float dy = source->second.posY - target->second.posY;
        const float dz = source->second.posZ - target->second.posZ;
        if (dx * dx + dy * dy + dz * dz > 9.0f ||
            source->second.interactionCount == std::numeric_limits<uint32_t>::max())
        {
            return false;
        }

        source->second.lastInteractionTarget = target->second.characterId;
        ++source->second.interactionCount;
        return true;
    }

    const MMOPlayer* MMOPlayerSystem::GetPlayer(uint32_t clientId) const
    {
        const auto it = m_players.find(clientId);
        return it == m_players.end() ? nullptr : &it->second;
    }

    bool MMOPlayerSystem::ApplySessionState(uint32_t clientId, const MMOPlayer& state)
    {
        auto it = m_players.find(clientId);
        if (clientId == 0 || state.clientId != clientId || state.characterId == 0 ||
            (it == m_players.end() && m_players.size() >= 64) || !std::isfinite(state.posX) ||
            !std::isfinite(state.posY) || !std::isfinite(state.posZ) || !std::isfinite(state.health) ||
            state.health <= 0.0f)
        {
            return false;
        }

        // Allocate an actor only on first sight; recurring snapshots update scalars in place.
        if (it == m_players.end())
        {
            it = m_players.emplace(clientId, MMOPlayer{}).first;
        }
        MMOPlayer& updated = it->second;
        updated.clientId = clientId;
        updated.characterId = state.characterId;
        updated.currentAreaId = state.currentAreaId;
        updated.posX = state.posX;
        updated.posY = state.posY;
        updated.posZ = state.posZ;
        updated.targetPosX = state.targetPosX;
        updated.targetPosY = state.targetPosY;
        updated.targetPosZ = state.targetPosZ;
        updated.health = state.health;
        updated.maxHealth = state.maxHealth;
        updated.level = state.level;
        updated.lastInteractionTarget = state.lastInteractionTarget;
        updated.interactionCount = state.interactionCount;
        if (updated.name.empty())
        {
            updated.name = "Character_" + std::to_string(state.characterId);
        }
#ifdef ENABLE_NETWORKING
        if (auto* netMgr = m_context ? m_context->GetNetwork() : nullptr; netMgr)
        {
            updated.isLocalPlayer = clientId == netMgr->GetLocalClientID();
            if (updated.isLocalPlayer)
            {
                m_localClientId = clientId;
            }
        }
#endif
        return true;
    }

} // namespace MMO
