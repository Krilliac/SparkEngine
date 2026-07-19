/**
 * @file TFAbilitySystemClient.cpp
 * @brief TFAbilitySystem client half: local-player mirror queries and tick,
 *        F-key entry + request send, the TF_AbilityState mirror store, veil
 *        ghosting of replicated pawn meshes, and the self-registering net
 *        handler lifecycle. Split from TFAbilitySystem.cpp.
 */
#include "Game/TFAbilitySystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFUiSounds.h" // W10 audio-wave-2: ability activation/end cues
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h" // TFMsg (SendMsg id cast)
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

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstring>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Local mirror queries (client half of the server-truth query pair)
    // ---------------------------------------------------------------------------

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

#ifdef ENABLE_NETWORKING

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

} // namespace Terrafront
