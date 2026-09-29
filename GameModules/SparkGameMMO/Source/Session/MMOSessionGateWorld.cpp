/** @file MMOSessionGateWorld.cpp
 * @brief Ownership checks and server-time movement/interaction application.
 */
#include "MMOSessionGate.h"

#ifdef ENABLE_NETWORKING
#include "Account/MMOAccountSystem.h"
#include "Character/MMOCharacterSystem.h"
#include "Player/MMOPlayerSystem.h"

#include <cmath>

namespace MMO
{
    using SessionGateWire::Operation;
    using SessionGateWire::Packet;
    using SessionGateWire::Status;

    Status MMOSessionGate::Apply(Session& session, const Packet& request)
    {
        if (request.operation == Operation::Register || request.operation == Operation::Login)
        {
            return Authenticate(session, request);
        }
        if (session.accountId == 0 || !m_accounts->ValidateSession(session.token))
        {
            return Status::Unauthenticated;
        }
        if (request.operation == Operation::CreateCharacter)
        {
            if (m_players->GetPlayer(session.clientId))
            {
                return Status::Rejected;
            }
            CharacterCreateRequest create;
            create.name = request.name.data();
            create.race = static_cast<RaceId>(request.race);
            create.classId = static_cast<ClassId>(request.classId);
            const auto result = m_characters->CreateCharacter(session.accountId, create);
            if (!result.success)
            {
                return Status::Rejected;
            }
            // A character may be created only before entering the world. This ID is
            // a selection, not world authority: EnterWorld still checks ownership.
            session.characterId = result.characterId;
            return Status::Ok;
        }
        if (!m_characters->OwnsCharacter(session.accountId, request.characterId))
        {
            return Status::NotOwner;
        }
        if (request.operation == Operation::EnterWorld)
        {
            if (m_players->GetPlayer(session.clientId))
            {
                return Status::Rejected;
            }
            const auto* character = m_characters->GetCharacter(request.characterId);
            const auto* race = character ? m_characters->GetRace(character->race) : nullptr;
            if (!character || !race)
            {
                return Status::Rejected;
            }
            const auto stats = m_characters->ComputeStats(character->race, character->classId, character->level);
            if (!m_players->SpawnSessionPlayer(session.clientId, request.characterId, character->name,
                                               character->areaId, race->spawnX, race->spawnY, race->spawnZ,
                                               stats.health))
            {
                return Status::Rejected;
            }
            session.characterId = request.characterId;
            session.moveCredit = 0.0f;
            (void)m_accounts->SetActiveCharacter(session.token, request.characterId);
            for (const auto& existing : m_sessions)
            {
                if (existing.characterId != 0 && m_players->GetPlayer(existing.clientId))
                {
                    SendTo(session.clientId, Snapshot(existing));
                }
            }
            Publish(session);
            return Status::Ok;
        }
        if (session.characterId != request.characterId || !m_players->GetPlayer(session.clientId))
        {
            return Status::Rejected;
        }
        if (request.operation == Operation::Move)
        {
            constexpr float Step = 1.0f / 60.0f;
            if (!std::isfinite(request.x) || !std::isfinite(request.z) ||
                request.x * request.x + request.z * request.z > 1.01f)
            {
                return Status::Invalid;
            }
            if (session.moveCredit < Step)
            {
                return Status::RateLimited;
            }
            session.moveCredit -= Step;
            if (!m_players->MoveSessionPlayer(session.clientId, request.x, request.z, Step))
            {
                return Status::Rejected;
            }
            Publish(session);
            return Status::Ok;
        }
        if (request.operation == Operation::Interact)
        {
            if (session.interactCooldown > 0.0f)
            {
                return Status::RateLimited;
            }
            for (const auto& target : m_sessions)
            {
                if (target.characterId == request.targetId &&
                    m_players->InteractSessionPlayer(session.clientId, target.clientId))
                {
                    session.interactCooldown = 0.5f;
                    Publish(session);
                    return Status::Ok;
                }
            }
            return Status::Rejected;
        }
        return Status::Invalid;
    }

    Packet MMOSessionGate::Snapshot(const Session& session)
    {
        Packet packet;
        packet.stateSequence = ++m_stateSequence;
        packet.operation = Operation::State;
        packet.response = true;
        // State events use requestId for the server-attributed connection ID.
        packet.requestId = session.clientId;
        packet.accountId = session.accountId;
        packet.characterId = session.characterId;
        if (const auto* player = m_players->GetPlayer(session.clientId))
        {
            packet.areaId = player->currentAreaId;
            packet.x = player->posX;
            packet.y = player->posY;
            packet.z = player->posZ;
            packet.health = player->health;
            packet.targetId = player->lastInteractionTarget;
            packet.interactionCount = player->interactionCount;
        }
        return packet;
    }

    void MMOSessionGate::Publish(const Session& session)
    {
        const auto state = Snapshot(session);
        for (const auto& observer : m_sessions)
        {
            if (observer.characterId != 0 && m_players->GetPlayer(observer.clientId) &&
                m_accounts->ValidateSession(observer.token))
            {
                SendTo(observer.clientId, state);
            }
        }
    }
} // namespace MMO
#endif
