/**
 * @file TFAbilitySystem.cpp
 * @brief Class abilities (W9) — server-authoritative state machine + effects,
 *        wire handlers, client mirror, F-key entry, veil ghosting, debug UI.
 *
 * See TFAbilitySystem.h for the full lane design. Wire convention: C->S
 * TF_AbilityRequest rides TFServerSim::RouteClientMessage (wiring snippet in
 * the wave report); S->C TF_AbilityState is broadcast on every phase
 * transition (self reads full detail, everyone else the active flag).
 */
#include "Game/TFAbilitySystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFUiSounds.h" // W10 audio-wave-2: ability activation/end cues
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "UI/TFChatWindow.h"
#include "UI/TFHUD.h"
#include "UI/TFLoginFlow.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSocialPanel.h"
#include "UI/TFSpawnScreen.h"

#include "Engine/ECS/Components.h"
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Terrafront
{

    namespace
    {
        constexpr float kSweepPeriodSec = 5.0f; ///< idle server-rec cleanup cadence

        const char* PhaseText(TFAbilityPhase p)
        {
            switch (p)
            {
            case TFAbilityPhase::Ready:
                return "ready";
            case TFAbilityPhase::Active:
                return "ACTIVE";
            case TFAbilityPhase::Cooldown:
                return "cooldown";
            }
            return "?";
        }
    } // namespace

    TFAbilitySystem::TFAbilitySystem() = default;
    TFAbilitySystem::~TFAbilitySystem() = default;

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    bool TFAbilitySystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPlayerSpawned(ev); });
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });

        // Bulwark Field absorb — the one-function TFDamageSystem seam. The
        // filter checks authority itself, so installing it on every role is
        // harmless (ServerApplyDamage only runs on the authority anyway).
        if (m_ctx->damage)
            m_ctx->damage->SetIncomingDamageFilter([this](EntityId victim, float amount)
                                                   { return ServerFilterIncomingDamage(victim, amount); });

        RegisterConsoleCommands();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFAbilitySystem initialized");
        return true;
    }

    void TFAbilitySystem::Shutdown()
    {
        if (!m_initialized)
            return;

        // No dangling `this`: the damage filter and the net handler both
        // capture this instance (TFSquadSystem::ReleaseClientHandlers pattern).
        if (m_ctx && m_ctx->damage)
            m_ctx->damage->SetIncomingDamageFilter(nullptr);
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
#endif

        // Never leave a replicated pawn mesh invisible across a shutdown/reload.
        for (const auto& [net, local] : m_hiddenMeshes)
        {
            (void)local;
            SetPawnMeshVisible(net, true);
        }
        m_hiddenMeshes.clear();
        m_activePawns.clear();
        m_server.clear();
        m_pawnOwner.clear();
        m_initialized = false;
    }

    void TFAbilitySystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle (TFSquadSystem pattern: registered
        // after link-up so the real handler wins the per-type slot).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            m_mirror = SelfMirror{};
            for (const auto& [net, local] : m_hiddenMeshes)
            {
                (void)local;
                SetPawnMeshVisible(net, true);
            }
            m_hiddenMeshes.clear();
            m_activePawns.clear();
        }

        // Late-joiner burst: newly connected clients get every non-Ready state
        // (TFDeployableSystem's GetClients() diff-poll pattern).
        if (m_ctx->IsAuthority())
        {
            m_sweepAccum += deltaTime; // FixedUpdate's idle-record sweep cadence
            auto& nm = Spark::Net::NetworkManager::GetInstance();
            if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server)
            {
                for (const auto& [id, info] : nm.GetClients())
                {
                    if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                        continue;
                    m_knownClients.insert(id);
                    for (const auto& [player, rec] : m_server)
                    {
                        if (rec.phase == TFAbilityPhase::Ready && player != id)
                            continue; // only non-Ready states matter to strangers
                        TF_AbilityState st{};
                        st.pawnEntity = rec.pawn;
                        st.player = player;
                        st.abilityClass = static_cast<uint8_t>(rec.cls);
                        st.phase = static_cast<uint8_t>(rec.phase);
                        const double now = NowSec();
                        if (rec.phase == TFAbilityPhase::Active)
                            st.remainingSec = rec.activeUntil > 0.0 ? static_cast<float>(rec.activeUntil - now) : 0.0f;
                        else if (rec.phase == TFAbilityPhase::Cooldown)
                            st.remainingSec = static_cast<float>(rec.cooldownUntil - now);
                        st.fuel01 = rec.fuel01;
                        SendStateWire(id, st);
                    }
                }
                std::erase_if(m_knownClients, [&nm](PlayerId id) { return !nm.GetClients().contains(id); });
            }
        }
#else
        m_sweepAccum += deltaTime;
#endif

        if (m_ctx->HasLocalPlayer())
        {
            ClientPollAbilityKey();
            ClientTickMirror(deltaTime);
            UpdateVeilVisuals();
        }
    }

    void TFAbilitySystem::FixedUpdate(float fixedDeltaTime)
    {
        m_clock += fixedDeltaTime;
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        const double now = NowSec();
        for (auto& [player, rec] : m_server)
        {
            const ClassAbilityDef* def = AbilityDefOf(rec.cls);
            if (!def)
                continue;
            const TFAbilityKind kind = KindOfKey(def->key);

            // Jets fuel model: drain while thrusting, regen otherwise.
            if (kind == TFAbilityKind::Jets)
            {
                if (rec.phase == TFAbilityPhase::Active)
                {
                    rec.fuel01 -= fixedDeltaTime / std::max(def->durationSec, 0.1f);
                    if (rec.fuel01 <= 0.0f)
                    {
                        rec.fuel01 = 0.0f;
                        ServerEndActive(player, rec, true);
                    }
                }
                else
                {
                    rec.fuel01 = std::min(1.0f, rec.fuel01 + def->regenPerSec * fixedDeltaTime);
                }
            }

            // Medtech heal pulse while active.
            if (kind == TFAbilityKind::Surge && rec.phase == TFAbilityPhase::Active)
                ServerTickSurge(rec, fixedDeltaTime);

            ServerAdvancePhase(player, rec);
        }

        // Cheap idle-record cleanup (bots included — they re-seed lazily).
        if (m_sweepAccum >= kSweepPeriodSec)
        {
            m_sweepAccum = 0.0f;
            std::erase_if(m_server,
                          [this, now](const auto& kv)
                          {
                              const ServerRec& rec = kv.second;
                              if (rec.phase != TFAbilityPhase::Ready || now < rec.cooldownUntil)
                                  return false;
                              PawnInfo pi{};
                              const bool hasPawn =
                                  m_ctx->players && m_ctx->players->GetPawnByPlayer(kv.first, pi) && pi.alive;
                              if (!hasPawn)
                                  m_pawnOwner.erase(rec.pawn);
                              return !hasPawn;
                          });
        }
    }

    // ---------------------------------------------------------------------------
    // Data helpers
    // ---------------------------------------------------------------------------

    TFAbilityKind TFAbilitySystem::KindOfKey(const std::string& key)
    {
        if (key.empty())
            return TFAbilityKind::None;
        if (key == "veil")
            return TFAbilityKind::Veil;
        if (key == "jets")
            return TFAbilityKind::Jets;
        if (key == "surge")
            return TFAbilityKind::Surge;
        if (key == "forge")
            return TFAbilityKind::Forge;
        if (key == "aegiswall")
            return TFAbilityKind::AegisWall;
        if (key == "lockdown")
            return TFAbilityKind::Lockdown;
        return TFAbilityKind::GenericTimed;
    }

    const ClassAbilityDef* TFAbilitySystem::AbilityDefOf(ClassId cls) const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return nullptr;
        const ClassDef* cd = m_ctx->data->GetClass(cls);
        if (!cd || cd->ability.key.empty())
            return nullptr;
        return &cd->ability;
    }

    double TFAbilitySystem::NowSec() const
    {
        if (m_ctx && m_ctx->IsAuthority() && m_ctx->serverSim)
            return m_ctx->serverSim->ServerTime();
        return m_clock;
    }

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

    float TFAbilitySystem::RoFMultiplierLocal() const
    {
        if (!m_mirror.valid || m_mirror.phase != TFAbilityPhase::Active)
            return 1.0f;
        const ClassAbilityDef* def = AbilityDefOf(m_mirror.cls);
        return (def && KindOfKey(def->key) == TFAbilityKind::Lockdown) ? kTFLockdownRoFMult : 1.0f;
    }

    TFAbilityMoveMods TFAbilitySystem::MoveModsLocal() const
    {
        TFAbilityMoveMods mods;
        if (!m_mirror.valid || m_mirror.phase != TFAbilityPhase::Active)
            return mods;
        const ClassAbilityDef* def = AbilityDefOf(m_mirror.cls);
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
            mods.speedMult = 0.0f;
            break;
        default:
            break;
        }
        return mods;
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

    bool TFAbilitySystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFAbilitySystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgAbilityState),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_AbilityState))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_AbilityState st{};
                               std::memcpy(&st, m.payload.data(), sizeof(st));
                               ClientHandleState(st);
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] ability mirror handlers registered");
    }

    void TFAbilitySystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgAbilityState),
                           [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Client: mirror + F-key + veil ghosting
    // ---------------------------------------------------------------------------

    void TFAbilitySystem::ClientHandleState(const TF_AbilityState& st)
    {
        const TFAbilityPhase phase = static_cast<TFAbilityPhase>(st.phase);
        const ClassId cls = st.abilityClass < static_cast<uint8_t>(ClassId::COUNT)
                                ? static_cast<ClassId>(st.abilityClass)
                                : ClassId::COUNT;

        if (m_ctx && st.player == m_ctx->localPlayer)
        {
            // W10 audio-wave-2: local activation/end cue (bleep_04 reuse — no bespoke ability clips shipped).
            TFUiSounds_ActiveEdge(m_ctx, m_mirror.valid && m_mirror.phase == TFAbilityPhase::Active,
                                  phase == TFAbilityPhase::Active);
            m_mirror.valid = true;
            m_mirror.cls = cls;
            m_mirror.phase = phase;
            m_mirror.remainingSec = std::max(0.0f, st.remainingSec);
            m_mirror.fuel01 = std::clamp(st.fuel01, 0.0f, 1.0f);
        }

        if (st.pawnEntity == 0)
            return;
        if (phase == TFAbilityPhase::Active)
        {
            m_activePawns[st.pawnEntity] = ActivePawn{cls, st.player};
        }
        else
        {
            m_activePawns.erase(st.pawnEntity);
            RestorePawnVisibility(st.pawnEntity);
        }
    }

    void TFAbilitySystem::ClientTickMirror(float dt)
    {
        if (!m_mirror.valid)
            return;
        if (m_mirror.remainingSec > 0.0f)
            m_mirror.remainingSec = std::max(0.0f, m_mirror.remainingSec - dt);

        // Jets fuel re-simulation between authoritative transitions — same
        // rates the server integrates, so the HUD gauge tracks closely.
        const ClassAbilityDef* def = AbilityDefOf(m_mirror.cls);
        if (def && KindOfKey(def->key) == TFAbilityKind::Jets)
        {
            if (m_mirror.phase == TFAbilityPhase::Active)
                m_mirror.fuel01 = std::max(0.0f, m_mirror.fuel01 - dt / std::max(def->durationSec, 0.1f));
            else
                m_mirror.fuel01 = std::min(1.0f, m_mirror.fuel01 + def->regenPerSec * dt);
        }
    }

    void TFAbilitySystem::ClientPollAbilityKey()
    {
        if (!m_ctx || !m_ctx->InWorld() || m_ctx->localPlayer == kInvalidPlayer)
            return;
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (!input)
            return;
        // Same input-suppression gate set as TFClientNet::SampleAndSendInput.
        const bool uiOpen =
            (m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()) ||
            (m_ctx->loginFlow && m_ctx->loginFlow->IsOpen()) || (m_ctx->hud && m_ctx->hud->IsChatOpen()) ||
            (m_ctx->chatWindow && m_ctx->chatWindow->IsOpen()) || (m_ctx->socialPanel && m_ctx->socialPanel->IsOpen());
        if (uiOpen)
            return;
        PawnInfo pi{};
        if (!m_ctx->players || !m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pi) || !pi.alive)
            return;
        if (!input->WasKeyPressed('F'))
            return;

        const bool on = !(m_mirror.valid && m_mirror.phase == TFAbilityPhase::Active);
        SendRequest(on);
    }

    void TFAbilitySystem::SendRequest(bool on)
    {
        if (!m_ctx || m_ctx->localPlayer == kInvalidPlayer)
            return;
        if (m_ctx->IsAuthority())
        {
            // Mirror the RouteClientMessage enter-world gate for the direct
            // authority path (TFSquadSystem::SendOp defense-in-depth pattern).
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m_ctx->localPlayer))
                return;
#endif
            ServerHandleRequest(m_ctx->localPlayer, on);
        }
        else if (m_ctx->clientNet)
        {
            TF_AbilityRequest req{};
            req.on = on ? 1 : 0;
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFMsgAbilityRequest), &req, sizeof(req));
        }
    }

    void TFAbilitySystem::UpdateVeilVisuals()
    {
        if (!m_ctx || !m_ctx->players)
            return;

        // Hide pass: enemy-faction veiled Ghosts vanish from the world render.
        // Same-faction viewers (which includes every squadmate) keep the mesh.
        for (const auto& [net, ap] : m_activePawns)
        {
            const ClassAbilityDef* def = AbilityDefOf(ap.cls);
            if (!def || KindOfKey(def->key) != TFAbilityKind::Veil)
                continue;
            if (ap.player == m_ctx->localPlayer)
                continue; // never hide your own pawn
            PawnInfo pi{};
            if (!m_ctx->players->GetPawnByEntity(net, pi))
                continue;
            const bool hide = pi.faction != m_ctx->localFaction;
            if (hide && !m_hiddenMeshes.contains(net))
                SetPawnMeshVisible(net, false);
            else if (!hide && m_hiddenMeshes.contains(net))
            {
                SetPawnMeshVisible(net, true);
                m_hiddenMeshes.erase(net);
            }
        }

        // Restore pass: anything we hid whose veil ended / pawn despawned.
        std::erase_if(m_hiddenMeshes,
                      [this](const auto& kv)
                      {
                          if (m_activePawns.contains(kv.first))
                              return false;
                          SetPawnMeshVisible(kv.first, true);
                          return true;
                      });
    }

    void TFAbilitySystem::RestorePawnVisibility(EntityId pawnNetEntity)
    {
        auto it = m_hiddenMeshes.find(pawnNetEntity);
        if (it == m_hiddenMeshes.end())
            return;
        SetPawnMeshVisible(pawnNetEntity, true);
        m_hiddenMeshes.erase(it);
    }

    void TFAbilitySystem::SetPawnMeshVisible(EntityId pawnNetEntity, bool visible)
    {
        if (!m_ctx || !m_ctx->players || !m_ctx->engine)
            return;
        uint32_t local = 0;
        if (!m_ctx->players->ResolveEntity(pawnNetEntity, local))
            return;
        World* world = m_ctx->engine->GetWorld();
        const auto e = static_cast<EntityID>(local);
        if (!world || !world->GetRegistry().valid(e))
            return;
        if (MeshRenderer* mr = world->GetComponent<MeshRenderer>(e))
        {
            mr->visible = visible;
            if (!visible)
                m_hiddenMeshes[pawnNetEntity] = local;
        }
    }

    // ---------------------------------------------------------------------------
    // Events
    // ---------------------------------------------------------------------------

    void TFAbilitySystem::OnPlayerSpawned(const EvPlayerSpawned& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        ServerRec& rec = m_server[ev.player];
        const EntityId oldPawn = rec.pawn;
        if (rec.cls != ev.cls)
        {
            rec = ServerRec{}; // new class == fresh ability state (incl. cooldown)
            rec.cls = ev.cls;
        }
        if (oldPawn != ev.pawn)
            m_pawnOwner.erase(oldPawn);
        rec.pawn = ev.pawn;
        m_pawnOwner[ev.pawn] = ev.player;
    }

    void TFAbilitySystem::OnPlayerKilled(const EvPlayerKilled& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        auto it = m_server.find(ev.victim);
        if (it == m_server.end())
            return;
        if (it->second.phase == TFAbilityPhase::Active)
            ServerEndActive(ev.victim, it->second, true); // death ends it, cd applies
        m_pawnOwner.erase(it->second.pawn);
        it->second.pawn = 0;
    }

    // ---------------------------------------------------------------------------
    // HUD view + console
    // ---------------------------------------------------------------------------

    TFAbilitySystem::HudView TFAbilitySystem::GetLocalHudView() const
    {
        HudView v;
        if (!m_ctx || !m_mirror.valid)
            return v;
        const ClassAbilityDef* def = AbilityDefOf(m_mirror.cls);
        if (!def)
            return v;
        const TFAbilityKind kind = KindOfKey(def->key);
        v.valid = true;
        v.name = def->name.empty() ? def->key : def->name;
        v.phase = m_mirror.phase;
        v.remainingSec = m_mirror.remainingSec;
        if (kind == TFAbilityKind::Jets)
        {
            v.fuelBased = true;
            v.fraction01 = m_mirror.fuel01;
        }
        else if (m_mirror.phase == TFAbilityPhase::Cooldown && def->cooldownSec > 0.0f)
        {
            v.fraction01 = 1.0f - std::clamp(m_mirror.remainingSec / def->cooldownSec, 0.0f, 1.0f);
        }
        else if (m_mirror.phase == TFAbilityPhase::Active && def->durationSec > 0.0f)
        {
            v.fraction01 = std::clamp(m_mirror.remainingSec / def->durationSec, 0.0f, 1.0f);
        }
        return v;
    }

    void TFAbilitySystem::RegisterConsoleCommands()
    {
        if (m_cmdsRegistered)
            return;
        auto& console = Spark::SimpleConsole::GetInstance();

        console.RegisterCommand(
            "tf_ability",
            [this](const std::vector<std::string>& args) -> std::string
            {
                bool on = !(m_mirror.valid && m_mirror.phase == TFAbilityPhase::Active);
                if (!args.empty())
                {
                    if (args[0] == "on")
                        on = true;
                    else if (args[0] == "off")
                        on = false;
                    else
                        return "usage: tf_ability [on|off]";
                }
                SendRequest(on);
                return std::string("[TF] ability request sent (") + (on ? "on" : "off") + ")";
            },
            "Activate/toggle your class ability (same as the F key)", "TERRAFRONT", "tf_ability [on|off]");

        console.RegisterCommand(
            "tf_ability_status",
            [this](const std::vector<std::string>&) -> std::string
            {
                const HudView v = GetLocalHudView();
                if (!v.valid)
                    return "[TF] no ability state yet (spawn first)";
                char buf[160];
                std::snprintf(buf, sizeof(buf), "[TF] %s: %s  remaining %.1fs  %s %.0f%%", v.name.c_str(),
                              PhaseText(v.phase), v.remainingSec, v.fuelBased ? "fuel" : "fill", v.fraction01 * 100.0f);
                return buf;
            },
            "Show your class ability phase/cooldown/fuel", "TERRAFRONT", "tf_ability_status");

        m_cmdsRegistered = true;
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFAbilitySystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Abilities"))
            return;
        ImGui::Text("activations %u  rejected %u  bad packets %u", m_activations, m_rejected, m_badPackets);
        ImGui::Text("heal given %.0f hp  absorbed %.0f hp", m_healGiven, m_absorbed);

        if (m_ctx && m_ctx->HasLocalPlayer())
        {
            const HudView v = GetLocalHudView();
            if (v.valid)
                ImGui::Text("local: %s  %s  %.1fs  %s %.0f%%", v.name.c_str(), PhaseText(v.phase), v.remainingSec,
                            v.fuelBased ? "fuel" : "fill", v.fraction01 * 100.0f);
            else
                ImGui::TextUnformatted("local: (no state)");
        }

        if (m_ctx && m_ctx->IsAuthority())
        {
            const double now = NowSec();
            ImGui::Text("server records: %zu  active-pawn mirror: %zu", m_server.size(), m_activePawns.size());
            for (const auto& [player, rec] : m_server)
            {
                const ClassAbilityDef* def = AbilityDefOf(rec.cls);
                ImGui::Text("  p%u %-10s %s  cd %.1f  fuel %.2f  absorb %.0f", player, def ? def->key.c_str() : "?",
                            PhaseText(rec.phase), std::max(0.0, rec.cooldownUntil - now), rec.fuel01, rec.absorbPool);
            }
        }
#endif
    }

} // namespace Terrafront
