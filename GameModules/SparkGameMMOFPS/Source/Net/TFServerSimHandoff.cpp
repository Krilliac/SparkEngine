/** @file TFServerSimHandoff.cpp @brief Game-thread pawn capture, suspension and restore for area control. */
#include "Net/TFServerSim.h"

#ifdef ENABLE_NETWORKING
#include "Account/TFAccountSystem.h"
#include "Game/TFDirectiveSystem.h"
#include "Game/TFOutfitSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Net/TFHandoffContinuity.h"
#include "World/TFTravelSystem.h"
#include "World/TFWorldSetup.h"

#include <algorithm>

namespace Terrafront
{
    bool TFServerSim::ResolveContinent(Spark::Net::AreaID area, std::string& key) const
    {
        return m_ctx && m_ctx->travel && m_ctx->travel->LookupHandoffContinent(area, key);
    }

    bool TFServerSim::Capture(uint64_t character, TFHandoffState& state)
    {
        if (!m_ctx || !m_ctx->players || !m_ctx->progression || !m_ctx->db || !m_ctx->IsAuthority())
        {
            return false;
        }
        for (const auto& [player, activeCharacter] : m_activeCharacter)
        {
            if (activeCharacter != character)
            {
                continue;
            }
            const auto move = m_move.find(player);
            PawnInfo pawn{};
            if (move == m_move.end() || !m_enteredWorld.contains(player) ||
                !m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive ||
                !TFTravel_IsInSanctuary(pawn.pos[0], pawn.pos[2]) ||
                (m_ctx->vehicles && m_ctx->vehicles->IsSeated(player)) || !m_ctx->progression->SaveNow())
            {
                return false;
            }
            state.player = player;
            state.cls = move->second.cls;
            std::copy_n(move->second.pos, 3, state.position);
            std::copy_n(move->second.vel, 3, state.velocity);
            state.yaw = move->second.yaw;
            state.pitch = move->second.pitch;
            state.health = pawn.health;
            state.shield = pawn.shield;
            state.lastSequence = move->second.lastSeq;
            state.grounded = move->second.grounded;
            // Only from the pad every continent shares: elsewhere the destination's ground is not this one.
            return TFHandoff_CanCarry(state);
        }
        return false;
    }

    bool TFServerSim::CanInstall(uint64_t character, const TFHandoffState& state) const
    {
        // Never place a pawn on ground this process did not load (defaults are another continent's terrain).
        if (!m_ctx || !m_ctx->players || !m_ctx->world || !m_ctx->world->TerrainLoaded() || !TFHandoff_CanCarry(state))
        {
            return false;
        }
        // A transport ClientID is process-local. Until a gateway-owned identity/transport binding exists,
        // only a destination connection already authenticated as this character's account may receive it.
        // In particular, never let a newly allocated unrelated ClientID inherit a migrated pawn.
        TFCharacterRecord row;
        if (!m_ctx->db || !m_ctx->account || !m_ctx->db->FindCharacter(character, row) ||
            m_ctx->account->AccountForClient(state.player) != row.accountId)
        {
            return false;
        }
        const auto active = m_activeCharacter.find(state.player);
        if (active != m_activeCharacter.end())
        {
            return active->second == character;
        }
        if (m_move.contains(state.player))
        {
            return false;
        }
        return m_suspendedCharacters.contains(character) ||
               m_activeCharacter.size() + m_suspendedCharacters.size() < kMaxPlayers;
    }

    bool TFServerSim::Suspend(uint64_t character)
    {
        if (m_suspendedCharacters.contains(character))
        {
            return true;
        }
        if (!m_ctx)
        {
            return false;
        }
        TFCharacterRecord row;
        TFHandoffState state;
        if (!m_ctx->db || !m_ctx->db->FindCharacter(character, row) ||
            !TFHandoffState::Decode(row.migrationPayload, state))
        {
            return false;
        }
        const auto active = m_activeCharacter.find(state.player);
        if (active == m_activeCharacter.end() || active->second != character)
        {
            // A source restarted after destination commit has no old pawn to remove. It is already inert;
            // accepting the retry must not touch an unrelated connection that now uses this PlayerId.
            return true;
        }
        m_suspendedCharacters.emplace(character, state);
        // Capture committed progress/meta before reserving. Remove the pawn without a kill event, XP,
        // death timer or residency release; the durable reservation remains the one source owner.
        if (m_ctx->players)
        {
            m_ctx->players->ServerHandlePlayerDisconnect(state.player);
        }
        if (m_ctx->progression)
        {
            m_ctx->progression->ClearPlayer(state.player, true);
        }
        m_move.erase(state.player);
        m_inputs.erase(state.player);
        m_enteredWorld.erase(state.player);
        m_activeCharacter.erase(state.player);
        return true;
    }

    bool TFServerSim::Install(const TFCharacterRecord& character, const TFHandoffState& state)
    {
        // An abort may resolve after its original connection has disconnected. Keep the character durable,
        // but finish logout rather than assigning a pawn to a newly recycled transport identity.
        if (m_suspendedCharacters.contains(character.id) && m_ctx && m_ctx->account &&
            m_ctx->account->AccountForClient(state.player) == 0)
        {
            if (!m_ctx->db->ReleaseCharacter(character.id))
            {
                return false;
            }
            Retire(character.id);
            return true;
        }
        if (!CanInstall(character.id, state))
        {
            return false;
        }
        if (ActiveCharacterOf(state.player) == character.id && m_move.contains(state.player))
        {
            return true;
        }
        // Resolve against THIS authority's loaded scene bodies and terrain, never the source's collision world.
        const TFHandoffArrival arrival =
            TFHandoff_Arrive(state, [this](const float prev[3], float pos[3], float vel[3], bool* grounded)
                             { m_ctx->world->ResolveMoveCollision(prev, pos, vel, grounded); });
        m_activeCharacter[state.player] = character.id;
        SetPlayerFaction(state.player, character.faction);
        if (m_ctx->progression)
        {
            m_ctx->progression->ServerLoadCharacter(state.player, character.xp, character.rank, character.flux);
        }
        const EntityId pawn =
            m_ctx->players->ServerSpawnPawn(state.player, character.faction, state.cls, arrival.position, state.yaw);
        if (pawn == 0)
        {
            return false;
        }
        m_enteredWorld.insert(state.player);
        if (m_ctx->travel)
        {
            m_ctx->travel->ServerAdoptHandoff(state.player);
        }
        MoveState& move = m_move[state.player];
        move.pawn = pawn;
        move.cls = state.cls;
        std::copy_n(arrival.position, 3, move.pos);
        std::copy_n(arrival.velocity, 3, move.vel);
        move.yaw = state.yaw;
        move.pitch = state.pitch;
        move.grounded = arrival.grounded;
        move.lastSeq = state.lastSequence;
        WritePawnTransform(move);
        m_ctx->players->ServerSetPawnHealth(pawn, state.health, state.shield);
        m_suspendedCharacters.erase(character.id);
        if (m_ctx->outfits)
        {
            m_ctx->outfits->ServerOnCharacterEntered(state.player, character.id, character.name);
        }
        return true;
    }

    void TFServerSim::Retire(uint64_t character)
    {
        const auto suspended = m_suspendedCharacters.find(character);
        if (suspended == m_suspendedCharacters.end())
        {
            return;
        }
        const PlayerId player = suspended->second.player;
        if (m_ctx->directives)
        {
            m_ctx->directives->ClearPlayer(player);
        }
        if (m_ctx->outfits)
        {
            m_ctx->outfits->ServerOnPlayerLeft(player);
        }
        if (m_ctx->account)
        {
            m_ctx->account->ClearSession(player);
        }
        m_factions.erase(player);
        m_deathTime.erase(player);
        m_chatNextAt.erase(player);
        m_redeployNextAt.erase(player);
        m_suspendedCharacters.erase(suspended);
    }
} // namespace Terrafront
#endif
