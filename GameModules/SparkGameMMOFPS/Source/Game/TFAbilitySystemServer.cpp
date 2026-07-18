/**
 * @file TFAbilitySystemServer.cpp
 * @brief TFAbilitySystem server half: wire entry + validation, the
 *        activate/end/advance phase machine, per-class effects (surge heal
 *        pulse, aegiswall damage absorb, forge deployable placement),
 *        server-truth pawn queries, and the S->C state broadcast machinery.
 *        Split from TFAbilitySystem.cpp.
 */
#include "Game/TFAbilitySystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFPlayerSystem.h"

#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstring>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Server: wire entry + validation
    // ---------------------------------------------------------------------------

    void TFAbilitySystem::ServerHandleAbilityMsgRaw(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_AbilityRequest) || !data)
        {
            ++m_badPackets;
            return;
        }
        TF_AbilityRequest req{};
        std::memcpy(&req, data, sizeof(req));
        ServerHandleRequest(sender, req.on != 0);
    }

    bool TFAbilitySystem::CanUseAbility(PlayerId player) const
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->players)
            return false;
        PawnInfo pi{};
        if (!m_ctx->players->GetPawnByPlayer(player, pi) || !pi.alive)
            return false;
        const ClassAbilityDef* def = AbilityDefOf(pi.cls);
        if (!def)
            return false;
        const TFAbilityKind kind = KindOfKey(def->key);
        const double now = NowSec();

        auto it = m_server.find(player);
        if (it != m_server.end())
        {
            const ServerRec& rec = it->second;
            // Const view of the timer-driven phase (FixedUpdate applies it for real).
            const bool activeExpired =
                rec.phase == TFAbilityPhase::Active && rec.activeUntil > 0.0 && now >= rec.activeUntil;
            if (rec.phase == TFAbilityPhase::Active && !activeExpired)
                return false;
            double cdUntil = rec.cooldownUntil;
            if (activeExpired)
                cdUntil = rec.activeUntil + def->cooldownSec;
            if (now < cdUntil)
                return false;
            if (kind == TFAbilityKind::Jets && rec.fuel01 < kTFJetMinActivateFuel01)
                return false;
        }
        return true;
    }

    bool TFAbilitySystem::UseAbility(PlayerId player, bool on)
    {
        return ServerHandleRequest(player, on);
    }

    bool TFAbilitySystem::ServerHandleRequest(PlayerId sender, bool on)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->players)
            return false;

        ServerRec& rec = m_server[sender];
        if (!ServerRefreshRec(sender, rec))
        {
            ++m_rejected;
            return false;
        }
        const ClassAbilityDef* def = AbilityDefOf(rec.cls);
        if (!def)
        {
            ++m_rejected;
            return false;
        }
        const TFAbilityKind kind = KindOfKey(def->key);
        ServerAdvancePhase(sender, rec); // apply pending timer transitions first

        if (!on)
        {
            // Explicit off is only meaningful for toggles and fuel-driven jets.
            if (rec.phase == TFAbilityPhase::Active && (def->toggle || kind == TFAbilityKind::Jets))
            {
                ServerEndActive(sender, rec, true);
                return true;
            }
            return false;
        }

        if (rec.phase != TFAbilityPhase::Ready)
        {
            ++m_rejected;
            return false;
        }
        return ServerActivate(sender, rec, *def, kind);
    }

    bool TFAbilitySystem::ServerActivate(PlayerId player, ServerRec& rec, const ClassAbilityDef& def,
                                         TFAbilityKind kind)
    {
        const double now = NowSec();

        // Fabricator Field Forge: instant — delegate to the deployable system;
        // only a SUCCESSFUL placement burns the cooldown.
        if (kind == TFAbilityKind::Forge)
        {
            if (!m_ctx->deployables)
                return false;
            const TFDeployResult r = m_ctx->deployables->ServerTryPlaceDeployable(player, DeployableKind::FabTurret);
            if (r != TFDeployResult::Ok)
            {
                ++m_rejected;
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] ability forge (player %u) refused: %s", player,
                               TFDeployableSystem::ResultText(r));
                return false;
            }
            rec.phase = def.cooldownSec > 0.0f ? TFAbilityPhase::Cooldown : TFAbilityPhase::Ready;
            rec.activeUntil = 0.0;
            rec.cooldownUntil = now + def.cooldownSec;
            ++m_activations;
            ServerBroadcastState(player, rec);
            return true;
        }

        if (kind == TFAbilityKind::Jets && rec.fuel01 < kTFJetMinActivateFuel01)
        {
            ++m_rejected;
            return false;
        }

        rec.phase = TFAbilityPhase::Active;
        // Toggles and fuel-driven jets run until turned off / fuel-empty;
        // everything else runs for the data-driven duration. A duration-less,
        // non-toggle unknown key degrades to an instant cooldown poke.
        const bool indefinite = def.toggle || kind == TFAbilityKind::Jets;
        rec.activeUntil = indefinite ? 0.0 : now + def.durationSec;
        if (kind == TFAbilityKind::AegisWall)
            rec.absorbPool = kTFAegisAbsorbHp;
        ++m_activations;

        if (!indefinite && def.durationSec <= 0.0f)
        {
            ServerEndActive(player, rec, true);
            return true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] ability %s ACTIVE (player %u, class %u)", def.key.c_str(),
                       player, static_cast<unsigned>(rec.cls));
        ServerBroadcastState(player, rec);
        return true;
    }

    void TFAbilitySystem::ServerEndActive(PlayerId player, ServerRec& rec, bool startCooldown)
    {
        if (rec.phase != TFAbilityPhase::Active)
            return;
        const ClassAbilityDef* def = AbilityDefOf(rec.cls);
        const float cd = (startCooldown && def) ? def->cooldownSec : 0.0f;
        rec.absorbPool = 0.0f;
        rec.activeUntil = 0.0;
        rec.cooldownUntil = NowSec() + cd;
        rec.phase = cd > 0.0f ? TFAbilityPhase::Cooldown : TFAbilityPhase::Ready;
        ServerBroadcastState(player, rec);
    }

    void TFAbilitySystem::ServerAdvancePhase(PlayerId player, ServerRec& rec)
    {
        const double now = NowSec();
        if (rec.phase == TFAbilityPhase::Active && rec.activeUntil > 0.0 && now >= rec.activeUntil)
            ServerEndActive(player, rec, true);
        if (rec.phase == TFAbilityPhase::Cooldown && now >= rec.cooldownUntil)
        {
            rec.phase = TFAbilityPhase::Ready;
            ServerBroadcastState(player, rec);
        }
    }

    bool TFAbilitySystem::ServerRefreshRec(PlayerId player, ServerRec& rec)
    {
        PawnInfo pi{};
        if (!m_ctx->players->GetPawnByPlayer(player, pi) || !pi.alive)
            return false;
        const EntityId oldPawn = rec.pawn;
        if (rec.cls != pi.cls)
        {
            // Class changed (respawn pick / Colossus terminal swap): fresh state.
            rec = ServerRec{};
            rec.cls = pi.cls;
        }
        if (oldPawn != pi.entity)
            m_pawnOwner.erase(oldPawn);
        rec.pawn = pi.entity;
        m_pawnOwner[pi.entity] = player;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Server: effects
    // ---------------------------------------------------------------------------

    void TFAbilitySystem::ServerTickSurge(const ServerRec& rec, float fdt)
    {
        if (!m_ctx->players || !m_ctx->damage)
            return;
        PawnInfo caster{};
        if (!m_ctx->players->GetPawnByEntity(rec.pawn, caster) || !caster.alive)
            return;

        const float healNow = kTFSurgeHealPerSec * fdt;
        const float r2 = kTFSurgeRadiusM * kTFSurgeRadiusM;
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.faction != caster.faction)
                    return;
                const float dx = p.pos[0] - caster.pos[0];
                const float dy = p.pos[1] - caster.pos[1];
                const float dz = p.pos[2] - caster.pos[2];
                if (dx * dx + dy * dy + dz * dz > r2)
                    return;
                m_ctx->damage->ServerHeal(p.entity, healNow);
                m_healGiven += healNow;
            });
    }

    float TFAbilitySystem::ServerFilterIncomingDamage(EntityId victim, float amount)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || amount <= 0.0f)
            return amount;
        auto ownerIt = m_pawnOwner.find(victim);
        if (ownerIt == m_pawnOwner.end())
            return amount;
        auto it = m_server.find(ownerIt->second);
        if (it == m_server.end() || it->second.phase != TFAbilityPhase::Active || it->second.absorbPool <= 0.0f)
            return amount;
        ServerRec& rec = it->second;
        const ClassAbilityDef* def = AbilityDefOf(rec.cls);
        if (!def || KindOfKey(def->key) != TFAbilityKind::AegisWall)
            return amount;

        const float absorb = std::min(rec.absorbPool, amount);
        rec.absorbPool -= absorb;
        m_absorbed += absorb;
        if (rec.absorbPool <= 0.0f)
            ServerEndActive(ownerIt->second, rec, true); // field broken
        return amount - absorb;
    }

    // ---------------------------------------------------------------------------
    // Server-truth / mirror queries
    // ---------------------------------------------------------------------------

    TFAbilityMoveMods TFAbilitySystem::MoveModsForPawn(EntityId pawnNetEntity) const
    {
        TFAbilityMoveMods mods;
        auto ownerIt = m_pawnOwner.find(pawnNetEntity);
        if (ownerIt == m_pawnOwner.end())
            return mods;
        auto it = m_server.find(ownerIt->second);
        if (it == m_server.end() || it->second.phase != TFAbilityPhase::Active)
            return mods;
        const ClassAbilityDef* def = AbilityDefOf(it->second.cls);
        if (!def)
            return mods;
        switch (KindOfKey(def->key))
        {
        case TFAbilityKind::Jets:
            mods.jetThrust = true;
            break;
        case TFAbilityKind::AegisWall:
            mods.speedMult = kTFAegisSpeedMult;
            break;
        case TFAbilityKind::Lockdown:
            mods.speedMult = 0.0f; // rooted (the wiring snippet also kills jump)
            break;
        default:
            break;
        }
        return mods;
    }

    float TFAbilitySystem::RoFMultiplierForPawn(EntityId pawnNetEntity) const
    {
        auto ownerIt = m_pawnOwner.find(pawnNetEntity);
        if (ownerIt == m_pawnOwner.end())
            return 1.0f;
        auto it = m_server.find(ownerIt->second);
        if (it == m_server.end() || it->second.phase != TFAbilityPhase::Active)
            return 1.0f;
        const ClassAbilityDef* def = AbilityDefOf(it->second.cls);
        return (def && KindOfKey(def->key) == TFAbilityKind::Lockdown) ? kTFLockdownRoFMult : 1.0f;
    }

    bool TFAbilitySystem::IsPawnAbilityActive(EntityId pawnNetEntity) const
    {
        if (m_ctx && m_ctx->IsAuthority())
        {
            auto ownerIt = m_pawnOwner.find(pawnNetEntity);
            if (ownerIt == m_pawnOwner.end())
                return false;
            auto it = m_server.find(ownerIt->second);
            return it != m_server.end() && it->second.phase == TFAbilityPhase::Active;
        }
        return m_activePawns.contains(pawnNetEntity);
    }

    bool TFAbilitySystem::IsPawnVeiled(EntityId pawnNetEntity) const
    {
        ClassId cls = ClassId::COUNT;
        if (m_ctx && m_ctx->IsAuthority())
        {
            auto ownerIt = m_pawnOwner.find(pawnNetEntity);
            if (ownerIt == m_pawnOwner.end())
                return false;
            auto it = m_server.find(ownerIt->second);
            if (it == m_server.end() || it->second.phase != TFAbilityPhase::Active)
                return false;
            cls = it->second.cls;
        }
        else
        {
            auto it = m_activePawns.find(pawnNetEntity);
            if (it == m_activePawns.end())
                return false;
            cls = it->second.cls;
        }
        const ClassAbilityDef* def = AbilityDefOf(cls);
        return def && KindOfKey(def->key) == TFAbilityKind::Veil;
    }

    // ---------------------------------------------------------------------------
    // Broadcast (server -> everyone; local mirror fed directly)
    // ---------------------------------------------------------------------------

    void TFAbilitySystem::ServerBroadcastState(PlayerId player, const ServerRec& rec)
    {
        const double now = NowSec();
        TF_AbilityState st{};
        st.pawnEntity = rec.pawn;
        st.player = player;
        st.abilityClass = static_cast<uint8_t>(rec.cls);
        st.phase = static_cast<uint8_t>(rec.phase);
        st.remainingSec = rec.phase == TFAbilityPhase::Active
                              ? (rec.activeUntil > 0.0 ? static_cast<float>(rec.activeUntil - now) : 0.0f)
                          : rec.phase == TFAbilityPhase::Cooldown ? static_cast<float>(rec.cooldownUntil - now)
                                                                  : 0.0f;
        st.fuel01 = rec.fuel01;

        // Listen host / standalone: the local player is not a network client —
        // feed the mirror directly (TFSquadSystem::SendEchoTo pattern).
        if (m_ctx->HasLocalPlayer() && m_ctx->role != NetRole::Client)
            ClientHandleState(st);

#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server)
        {
            for (const auto& [id, info] : nm.GetClients())
            {
                if (info.state == Spark::Net::ConnectionState::Connected)
                    SendStateWire(id, st);
            }
        }
#endif
    }

#ifdef ENABLE_NETWORKING

    void TFAbilitySystem::SendStateWire(PlayerId target, const TF_AbilityState& st)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized())
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(kTFMsgAbilityState);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(sizeof(st));
        std::memcpy(msg.payload.data(), &st, sizeof(st));
        nm.SendToClient(target, msg);
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
